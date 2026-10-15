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
#include <cstring>
#include <limits>

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

  // Needed to turn a feed "replace" (new id, same side) into cancel + add.
  bool side_of(OrderId id, Side& out) const noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return false;
    out = pool_.at_const(idx)->side;
    return true;
  }

  // Drops every order. Used when a snapshot replaces the whole book. O(levels).
  void clear() noexcept {
    for (int s = 0; s < 2; ++s) {
      for (std::uint32_t i = 0; i < num_levels_; ++i) levels_[s][i] = Level{kNil, kNil, 0};
      bitmap_[s].clear_all();
      best_[s] = kNone;
    }
    pool_.reset();
    map_.clear();
    count_ = 0;
  }

  // Shifts the price window so `new_base` becomes the lowest representable
  // price. Order prices do not change, only their level indices. Fails (book
  // untouched) if the shift is not a whole number of ticks or any resting
  // order would fall outside the new window. O(levels + orders): a maintenance
  // operation between messages, not part of the per-update path.
  bool rebase(Price new_base) noexcept {
    const std::int64_t dp = new_base.raw() - base_raw_;
    if (dp % tick_ != 0) return false;
    const std::int64_t delta = dp / tick_;  // new level index = old index - delta
    if (delta == 0) return true;

    std::uint32_t lo[2] = {kNone, kNone}, hi[2] = {0, 0};
    std::int64_t glo = std::numeric_limits<std::int64_t>::max();
    std::int64_t ghi = std::numeric_limits<std::int64_t>::min();
    for (int s = 0; s < 2; ++s) {
      if (best_[s] == kNone) continue;
      lo[s] = bitmap_[s].next_ge(0);
      hi[s] = bitmap_[s].prev_le(num_levels_ - 1);
      if (static_cast<std::int64_t>(lo[s]) < glo) glo = lo[s];
      if (static_cast<std::int64_t>(hi[s]) > ghi) ghi = hi[s];
    }
    if (glo > ghi) {  // empty book: nothing to move
      base_raw_ = new_base.raw();
      return true;
    }
    if (glo - delta < 0 || ghi - delta >= static_cast<std::int64_t>(num_levels_)) return false;

    for (int s = 0; s < 2; ++s) {
      if (lo[s] == kNone) continue;
      Level* L = levels_[s];
      const std::int64_t nlo = static_cast<std::int64_t>(lo[s]) - delta;
      const std::int64_t nhi = static_cast<std::int64_t>(hi[s]) - delta;
      std::memmove(L + nlo, L + lo[s], (hi[s] - lo[s] + 1u) * sizeof(Level));
      // Empty the old positions the move did not overwrite.
      for (std::int64_t i = lo[s]; i <= static_cast<std::int64_t>(hi[s]); ++i)
        if (i < nlo || i > nhi) L[i] = Level{kNil, kNil, 0};
      bitmap_[s].clear_all();
      for (std::int64_t i = nlo; i <= nhi; ++i) {
        if (L[i].head == kNil) continue;
        const auto lvl = static_cast<std::uint32_t>(i);
        bitmap_[s].set(lvl);
        for (std::uint32_t o = L[i].head; o != kNil; o = pool_.at(o)->next) pool_.at(o)->level = lvl;
      }
      best_[s] = (s == 0) ? bitmap_[0].prev_le(num_levels_ - 1) : bitmap_[1].next_ge(0);
    }
    base_raw_ = new_base.raw();
    return true;
  }

  // Verifies every structural invariant: bitmap bits match occupied levels,
  // queue links are consistent both ways, each order sits in the level and side
  // it claims, level totals equal the sum of their orders, the id map points at
  // the right slots, best prices are the true extremes, and counts agree.
  // O(levels + orders); used by tests and as a fuzzing oracle, never per-update.
  bool check_invariants() const noexcept {
    std::uint32_t total = 0;
    for (std::size_t s = 0; s < 2; ++s) {
      std::uint32_t occupied = 0, lo = kNone, hi = kNone;
      for (std::uint32_t l = 0; l < num_levels_; ++l) {
        const Level& L = levels_[s][l];
        const bool occ = L.head != kNil;
        if (occ != bitmap_[s].test(l)) return false;
        if ((L.head == kNil) != (L.tail == kNil)) return false;
        if (!occ) {
          if (L.qty != 0) return false;
          continue;
        }
        ++occupied;
        if (lo == kNone) lo = l;
        hi = l;
        Qty sum = 0;
        std::uint32_t prev = kNil;
        for (std::uint32_t i = L.head; i != kNil; i = pool_.at_const(i)->next) {
          if (i >= max_orders_) return false;
          const Order& o = *pool_.at_const(i);
          if (o.prev != prev || o.level != l || side_idx(o.side) != s || o.qty <= 0) return false;
          if (map_.find(o.id) != i) return false;
          sum += o.qty;
          prev = i;
          if (++total > max_orders_) return false;  // also stops on a cyclic list
        }
        if (prev != L.tail || sum != L.qty) return false;
      }
      if (occupied == 0) {
        if (best_[s] != kNone) return false;
      } else if (best_[s] != (s == 0 ? hi : lo)) {
        return false;
      }
    }
    return total == count_ && total == pool_.in_use();
  }

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
