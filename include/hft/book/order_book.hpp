#pragma once
// Limit order book rebuilt from a feed (it does NOT match; crossing prices are
// stored as given, because the exchange already did the matching). Four events
// as in ITCH-style feeds: add, cancel, execute (partial or full fill of a
// resting order), and replace.
//
// Layout, chosen for O(1) hot-path work and no allocation after init():
//  - Each side has a flat array of levels indexed by (price - base) / tick.
//    No tree, no pointer chasing to find a level.
//  - Each level holds an intrusive FIFO of orders (32-bit pool indices), so
//    time priority is the queue order and unlink is O(1).
//  - A two-level bitmap per side finds the next occupied level when the best
//    one empties.
//  - An open-addressing map turns an order id into its pool slot.
//
// Limitation by design: prices outside [base, base + num_levels * tick) are
// rejected (BadPrice). A production system re-centres the window as the market
// moves; Phase 3 will add that when the feed handler needs it.

#include <cstdint>

#include "hft/book/digest.hpp"
#include "hft/book/id_map.hpp"
#include "hft/book/level_bitmap.hpp"
#include "hft/core/fixed_point.hpp"
#include "hft/core/memory.hpp"
#include "hft/core/types.hpp"

namespace hft::book {

using core::OrderId;
using core::Price;
using core::Qty;
using core::Side;

enum class BookResult : std::uint8_t { Ok = 0, BadId, BadQty, BadPrice, Duplicate, NotFound, Full };

struct BookConfig {
  Price base;                  // lowest representable price
  std::int64_t tick_raw = 100; // price increment in raw Price units (100 = 0.01)
  std::uint32_t num_levels = 4096;
  std::uint32_t max_orders = 1u << 16;
};

class OrderBook {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  // Startup only: all memory comes from `arena`. Returns false if it is too small.
  bool init(core::Arena& arena, const BookConfig& cfg) noexcept {
    base_raw_ = cfg.base.raw();
    tick_ = cfg.tick_raw;
    num_levels_ = cfg.num_levels;
    max_orders_ = cfg.max_orders;
    if (tick_ <= 0 || num_levels_ == 0 || max_orders_ == 0) return false;
    for (int s = 0; s < 2; ++s) {
      levels_[s] = arena.allocate_array<Level>(num_levels_);
      if (!levels_[s]) return false;
      for (std::uint32_t i = 0; i < num_levels_; ++i) levels_[s][i] = Level{kNil, kNil, 0};
      if (!bitmap_[s].init(arena, num_levels_)) return false;
      best_[s] = LevelBitmap::kNone;
    }
    if (!pool_.init(arena, max_orders_)) return false;
    std::uint32_t cap = 2;
    while (cap < max_orders_ * 2u) cap <<= 1;  // <= 50% load
    if (!map_.init(arena, cap)) return false;
    count_ = 0;
    return true;
  }

  BookResult add(OrderId id, Side side, Price price, Qty qty) noexcept {
    if (id == 0) return BookResult::BadId;
    if (qty <= 0) return BookResult::BadQty;
    std::uint32_t lvl;
    if (!to_level(price, lvl)) return BookResult::BadPrice;
    if (map_.find(id) != kNil) return BookResult::Duplicate;
    Order* o = pool_.emplace(id, qty, lvl, side);
    if (!o) return BookResult::Full;
    const std::uint32_t idx = pool_.index_of(o);
    map_.insert(id, idx);
    link_tail(idx, *o);
    ++count_;
    return BookResult::Ok;
  }

  BookResult cancel(OrderId id) noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return BookResult::NotFound;
    remove(idx, id);
    return BookResult::Ok;
  }

  // A resting order was filled for `qty` (partial or full) by the exchange.
  BookResult execute(OrderId id, Qty qty) noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return BookResult::NotFound;
    Order& o = *pool_.at(idx);
    if (qty <= 0 || qty > o.qty) return BookResult::BadQty;
    if (qty == o.qty) {
      remove(idx, id);
    } else {
      o.qty -= qty;
      levels_[side_idx(o.side)][o.level].qty -= qty;
    }
    return BookResult::Ok;
  }

  // Same id, new price and/or quantity. Reducing size at the same price keeps
  // queue position (the exchange does too); anything else loses priority.
  BookResult replace(OrderId id, Price new_price, Qty new_qty) noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return BookResult::NotFound;
    if (new_qty <= 0) return BookResult::BadQty;
    std::uint32_t new_lvl;
    if (!to_level(new_price, new_lvl)) return BookResult::BadPrice;
    Order& o = *pool_.at(idx);
    if (new_lvl == o.level && new_qty <= o.qty) {
      levels_[side_idx(o.side)][o.level].qty -= (o.qty - new_qty);
      o.qty = new_qty;
      return BookResult::Ok;
    }
    unlink(idx, o);
    o.qty = new_qty;
    o.level = new_lvl;
    link_tail(idx, o);
    return BookResult::Ok;
  }

  // ---- queries -------------------------------------------------------------

  Price best_bid() const noexcept { return level_price(best_[0]); }
  Price best_ask() const noexcept { return level_price(best_[1]); }
  Qty best_bid_qty() const noexcept { return best_[0] == kNone ? 0 : levels_[0][best_[0]].qty; }
  Qty best_ask_qty() const noexcept { return best_[1] == kNone ? 0 : levels_[1][best_[1]].qty; }
  std::uint32_t order_count() const noexcept { return count_; }

  Qty level_qty(Side side, Price p) const noexcept {
    std::uint32_t lvl;
    if (!to_level(p, lvl)) return 0;
    return levels_[side_idx(side)][lvl].qty;
  }

  bool contains(OrderId id) const noexcept { return map_.find(id) != kNil; }

  // Full-state digest (see digest.hpp). O(book size); for tests and checkpoints.
  std::uint64_t digest() const noexcept {
    std::uint64_t h = kDigestSeed;
    for (int s = 0; s < 2; ++s) {
      h = digest_combine(h, static_cast<std::uint64_t>(s));
      std::uint32_t l = best_[s];
      while (l != kNone) {
        const Level& L = levels_[s][l];
        h = digest_combine(h, static_cast<std::uint64_t>(base_raw_ + static_cast<std::int64_t>(l) * tick_));
        h = digest_combine(h, static_cast<std::uint64_t>(L.qty));
        for (std::uint32_t i = L.head; i != kNil; i = pool_.at_const(i)->next) {
          h = digest_combine(h, pool_.at_const(i)->id);
          h = digest_combine(h, static_cast<std::uint64_t>(pool_.at_const(i)->qty));
        }
        // Bids walk down from the best price, asks walk up.
        if (s == 0) {
          l = (l == 0) ? kNone : bitmap_[0].prev_le(l - 1);
        } else {
          l = (l + 1 >= num_levels_) ? kNone : bitmap_[1].next_ge(l + 1);
        }
      }
    }
    return h;
  }

 private:
  static constexpr std::uint32_t kNone = LevelBitmap::kNone;

  struct Order {
    Order(OrderId id_, Qty qty_, std::uint32_t level_, Side side_) noexcept
        : id(id_), qty(qty_), prev(kNil), next(kNil), level(level_), side(side_) {}
    OrderId id;
    Qty qty;
    std::uint32_t prev, next;  // neighbours in the level queue
    std::uint32_t level;
    Side side;
  };
  static_assert(sizeof(Order) == 32, "one order should fit half a cache line");

  struct Level {
    std::uint32_t head, tail;
    Qty qty;  // total resting quantity at this price
  };

  static constexpr std::size_t side_idx(Side s) noexcept { return s == Side::Buy ? 0 : 1; }

  bool to_level(Price p, std::uint32_t& out) const noexcept {
    const std::int64_t off = p.raw() - base_raw_;
    if (off < 0) return false;
    const std::int64_t q = off / tick_;
    if (q * tick_ != off || q >= static_cast<std::int64_t>(num_levels_)) return false;
    out = static_cast<std::uint32_t>(q);
    return true;
  }

  Price level_price(std::uint32_t l) const noexcept {
    if (l == kNone) return Price::invalid();
    return Price::from_raw(base_raw_ + static_cast<std::int64_t>(l) * tick_);
  }

  void link_tail(std::uint32_t idx, Order& o) noexcept {
    const std::size_t s = side_idx(o.side);
    Level& L = levels_[s][o.level];
    o.prev = L.tail;
    o.next = kNil;
    if (L.tail != kNil) {
      pool_.at(L.tail)->next = idx;
    } else {
      // Level just became occupied: mark it and maybe improve the best price.
      L.head = idx;
      bitmap_[s].set(o.level);
      if (best_[s] == kNone || (s == 0 ? o.level > best_[s] : o.level < best_[s])) best_[s] = o.level;
    }
    L.tail = idx;
    L.qty += o.qty;
  }

  void unlink(std::uint32_t idx, Order& o) noexcept {
    (void)idx;
    const std::size_t s = side_idx(o.side);
    Level& L = levels_[s][o.level];
    if (o.prev != kNil) pool_.at(o.prev)->next = o.next; else L.head = o.next;
    if (o.next != kNil) pool_.at(o.next)->prev = o.prev; else L.tail = o.prev;
    L.qty -= o.qty;
    if (L.head == kNil) {
      bitmap_[s].clear(o.level);
      if (best_[s] == o.level) {
        // The best level emptied; everything better was already empty, so the
        // next best lies strictly behind it.
        if (s == 0) best_[s] = (o.level == 0) ? kNone : bitmap_[0].prev_le(o.level - 1);
        else best_[s] = (o.level + 1 >= num_levels_) ? kNone : bitmap_[1].next_ge(o.level + 1);
      }
    }
  }

  void remove(std::uint32_t idx, OrderId id) noexcept {
    Order* o = pool_.at(idx);
    unlink(idx, *o);
    map_.erase(id);
    pool_.release(o);
    --count_;
  }

  std::int64_t base_raw_{0}, tick_{1};
  std::uint32_t num_levels_{0}, max_orders_{0}, count_{0};
  Level* levels_[2]{nullptr, nullptr};
  LevelBitmap bitmap_[2];
  std::uint32_t best_[2]{kNone, kNone};
  core::Pool<Order> pool_;
  IdMap map_;
};

}  // namespace hft::book
