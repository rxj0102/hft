#pragma once
// Book rebuilt from a public event tape (L3: every order is known), plus what a
// queue simulator needs that the plain OrderBook does not keep: for each
// resting order its arrival sequence number and remaining quantity.
//
// Arrival sequence numbers are what tell us whether an order was ahead of or
// behind a simulated order of ours: ahead means smaller number.

#include <cstdint>

#include "hft/book/id_map.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/fast_div.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/types.hpp"

namespace hft::bt {

using core::OrderId;
using core::Price;
using core::Qty;
using core::Side;

class TapeBook {
 public:
  struct Info {
    Side side = Side::Buy;
    Price price{};
    Qty qty = 0;
    std::uint64_t seq = 0;
  };

  bool init(core::Arena& arena, const book::BookConfig& cfg) noexcept {
    cfg_ = cfg;
    if (cfg.tick_raw > 0) tick_div_.set(static_cast<std::uint64_t>(cfg.tick_raw));
    if (!book_.init(arena, cfg)) return false;
    if (!pool_.init(arena, cfg.max_orders)) return false;
    std::uint32_t cap = 2;
    while (cap < cfg.max_orders * 2u) cap <<= 1;
    if (!map_.init(arena, cap)) return false;
    seq_ = 0;
    errors_ = 0;
    return true;
  }

  // Applies one public event. Returns false (and counts an error) if the event
  // does not fit the book: an unknown id, a duplicate, a full pool.
  bool apply(const sim::PublicEvent& e) noexcept {
    switch (e.type) {
      case sim::PublicType::Add: {
        if (book_.add(e.id, e.side, e.price, e.qty) != book::BookResult::Ok) return fail();
        Rec* r = pool_.emplace(Rec{e.id, ++seq_, e.qty, e.price, e.side});
        if (!r) {
          book_.cancel(e.id);
          return fail();
        }
        map_.insert(e.id, pool_.index_of(r));
        return true;
      }
      case sim::PublicType::Exec: {
        const std::uint32_t idx = map_.find(e.id);
        if (idx == kNil) return fail();
        Rec& r = *pool_.at(idx);
        if (e.qty <= 0 || e.qty > r.qty) return fail();
        book_.execute(e.id, e.qty);
        r.qty -= e.qty;
        if (r.qty == 0) drop(idx, e.id);
        return true;
      }
      case sim::PublicType::Reduce: {
        const std::uint32_t idx = map_.find(e.id);
        if (idx == kNil) return fail();
        Rec& r = *pool_.at(idx);
        if (e.qty <= 0 || e.qty >= r.qty) return fail();
        if (book_.replace(e.id, e.price, r.qty - e.qty) != book::BookResult::Ok) return fail();
        r.qty -= e.qty;
        return true;
      }
      case sim::PublicType::Delete: {
        const std::uint32_t idx = map_.find(e.id);
        if (idx == kNil) return fail();
        book_.cancel(e.id);
        drop(idx, e.id);
        return true;
      }
    }
    return fail();
  }

  // The order as it is now. False if it is not resting.
  bool lookup(OrderId id, Info& out) const noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return false;
    const Rec& r = *pool_.at_const(idx);
    out.side = r.side;
    out.price = r.price;
    out.qty = r.qty;
    out.seq = r.seq;
    return true;
  }

  Price best_bid() const noexcept { return book_.best_bid(); }
  Price best_ask() const noexcept { return book_.best_ask(); }
  Qty level_qty(Side s, Price p) const noexcept { return book_.level_qty(s, p); }
  bool two_sided() const noexcept { return best_bid() != Price::invalid() && best_ask() != Price::invalid(); }

  // Twice the mid price in raw units (so a half-tick mid is still an integer).
  // Only meaningful when two_sided().
  std::int64_t mid2() const noexcept { return best_bid().raw() + best_ask().raw(); }

  // Total quantity on the n price levels nearest the touch on one side, counted
  // by price distance (an empty level inside the window contributes zero).
  Qty depth(Side s, int n) const noexcept { return book_.depth(s, n); }

  // True if p is on the tick grid and inside the window.
  bool price_ok(Price p) const noexcept {
    const std::int64_t d = p.raw() - cfg_.base.raw();
    return d >= 0 && d % cfg_.tick_raw == 0 && d / cfg_.tick_raw < static_cast<std::int64_t>(cfg_.num_levels);
  }
  std::int64_t tick_index(Price p) const noexcept {
    const std::int64_t off = p.raw() - cfg_.base.raw();
    if (off < 0) return off / cfg_.tick_raw;
    return static_cast<std::int64_t>(tick_div_.div(static_cast<std::uint64_t>(off)));
  }
  Price price_at(std::int64_t idx) const noexcept { return Price::from_raw(cfg_.base.raw() + idx * cfg_.tick_raw); }

  std::uint64_t seq() const noexcept { return seq_; }  // arrival number of the newest order
  std::uint64_t errors() const noexcept { return errors_; }
  std::uint32_t order_count() const noexcept { return book_.order_count(); }
  const book::BookConfig& config() const noexcept { return cfg_; }
  bool check_invariants() const noexcept { return book_.check_invariants() && pool_.in_use() == book_.order_count(); }
  std::uint64_t digest() const noexcept { return book_.digest(); }

 private:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;
  struct Rec {
    OrderId id;
    std::uint64_t seq;
    Qty qty;
    Price price;
    Side side;
  };

  bool fail() noexcept {
    ++errors_;
    return false;
  }
  void drop(std::uint32_t idx, OrderId id) noexcept {
    map_.erase(id);
    pool_.release(pool_.at(idx));
  }

  book::BookConfig cfg_{};
  core::InvariantDivisor tick_div_{};
  book::OrderBook book_;
  core::Pool<Rec> pool_;
  book::IdMap map_;
  std::uint64_t seq_ = 0;
  std::uint64_t errors_ = 0;
};

}  // namespace hft::bt
