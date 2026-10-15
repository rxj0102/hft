#pragma once
// Price-time priority matching engine for one instrument.
//
// Storage reuses the Phase 2 book design (flat level arrays, intrusive FIFO
// per level, two-level bitmap, id hash map) with an owner field added for
// self-trade prevention. All memory comes from an Arena at init(); no
// allocation, exceptions or virtual calls afterwards.
//
// Rules (the conformance suite pins every one of these):
//  * Best price first; within a price, earlier arrival first. A trade happens
//    at the RESTING order's price, so aggressors get price improvement.
//  * Limit: matches what crosses, rests the remainder.
//  * Market: matches at any price; the remainder is cancelled, never rested.
//  * IOC: like Limit but the remainder is cancelled.
//  * FOK: checked read-only first. If the whole quantity cannot trade now, the
//    order is cancelled and the book is untouched; otherwise it fully trades.
//  * PostOnly: if it would cross (equal price counts) it is rejected, else it rests.
//  * Self-trade prevention is applied the moment an incoming order meets a
//    resting order of the same owner, per StpMode.
//  * Replace keeps the id. Reducing size at the same price keeps queue
//    position; any other change loses it and re-enters as a Limit order, so
//    it can trade if the new price crosses.
//  * Order ids are unique among RESTING orders. Ids of finished orders may be
//    reused (an exchange would normally forbid that; the engine does not
//    remember history because that needs unbounded memory).
//
// Event order within one call is part of the contract (the reference matcher
// in the tests produces the same sequence): Accepted, then for each trade the
// taker's Fill followed by the maker's Fill, then any terminal Canceled for
// the remainder. A trade's public Exec event is emitted right after the two
// Fills. Public Add comes last, when the remainder starts resting.

#include <cstdint>

#include "hft/book/digest.hpp"
#include "hft/book/id_map.hpp"
#include "hft/book/level_bitmap.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/types.hpp"

namespace hft::sim {

struct EngineConfig {
  Price base;                   // lowest representable price
  std::int64_t tick_raw = 100;  // price increment in raw Price units
  std::uint32_t num_levels = 4096;
  std::uint32_t max_orders = 1u << 16;  // resting orders
  StpMode stp = StpMode::None;
};

template <class Listener = NullListener>
class MatchingEngine {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  // Startup only. The listener must outlive the engine.
  bool init(core::Arena& arena, const EngineConfig& cfg, Listener& listener) noexcept {
    lis_ = &listener;
    base_raw_ = cfg.base.raw();
    tick_ = cfg.tick_raw;
    num_levels_ = cfg.num_levels;
    max_orders_ = cfg.max_orders;
    stp_ = cfg.stp;
    if (tick_ <= 0 || num_levels_ == 0 || max_orders_ == 0) return false;
    for (int s = 0; s < 2; ++s) {
      levels_[s] = arena.allocate_array<Level>(num_levels_);
      if (!levels_[s]) return false;
      for (std::uint32_t i = 0; i < num_levels_; ++i) levels_[s][i] = Level{kNil, kNil, 0};
      if (!bitmap_[s].init(arena, num_levels_)) return false;
      best_[s] = kNone;
    }
    if (!pool_.init(arena, max_orders_)) return false;
    std::uint32_t cap = 2;
    while (cap < max_orders_ * 2u) cap <<= 1;
    if (!map_.init(arena, cap)) return false;
    count_ = 0;
    return true;
  }

  // ---- requests ------------------------------------------------------------

  void new_order(const NewOrder& o, std::uint64_t ts) noexcept {
    ts_ = ts;
    if (o.id == 0) return reject(o, Reason::BadId);
    if (o.qty <= 0) return reject(o, Reason::BadQty);
    const bool priced = o.type != OrderType::Market;
    std::uint32_t lvl = 0;
    if (priced && !to_level(o.price, lvl)) return reject(o, Reason::BadPrice);
    if (map_.find(o.id) != kNil) return reject(o, Reason::Duplicate);
    if (o.type == OrderType::PostOnly && crosses(o.side, lvl)) return reject(o, Reason::WouldCross);

    emit_report(ReportType::Accepted, Reason::None, Liquidity::None, o.side, o.owner, o.id, 0,
                priced ? o.price : Price::invalid(), o.qty, o.qty);

    if (o.type == OrderType::FOK && !fok_feasible(o.side, lvl, o.qty, o.owner)) {
      emit_report(ReportType::Canceled, Reason::FokUnfillable, Liquidity::None, o.side, o.owner, o.id, 0,
                  o.price, o.qty, 0);
      return;
    }

    Qty rem = o.qty;
    bool done = false;  // taker already received its terminal report
    const Price tprice = priced ? o.price : Price::invalid();
    match(o.id, o.owner, o.side, priced, lvl, tprice, rem, done);
    if (rem == 0 || done) return;

    switch (o.type) {
      case OrderType::Limit:
      case OrderType::PostOnly:
        rest_or_cancel(o.id, o.owner, o.side, lvl, rem);
        break;
      case OrderType::IOC:
        cancel_taker(o, rem, Reason::IocRemainder);
        break;
      case OrderType::Market:
        cancel_taker(o, rem, Reason::MarketRemainder);
        break;
      case OrderType::FOK:  // unreachable: feasibility guarantees a full fill
        cancel_taker(o, rem, Reason::FokUnfillable);
        break;
    }
  }

  void cancel(OrderId id, Owner owner, std::uint64_t ts) noexcept {
    ts_ = ts;
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil || pool_.at(idx)->owner != owner) {
      emit_report(ReportType::CancelRejected, Reason::UnknownOrder, Liquidity::None, Side::Buy, owner, id, 0,
                  Price::invalid(), 0, 0);
      return;
    }
    const Order o = *pool_.at(idx);
    emit_public(PublicType::Delete, o.side, o.id, level_price(o.level), o.qty);
    remove(idx);
    emit_report(ReportType::Canceled, Reason::UserRequested, Liquidity::None, o.side, o.owner, o.id, 0,
                level_price(o.level), o.qty, 0);
  }

  void replace(OrderId id, Owner owner, Price new_price, Qty new_qty, std::uint64_t ts) noexcept {
    ts_ = ts;
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil || pool_.at(idx)->owner != owner)
      return reject_replace(owner, id, Reason::UnknownOrder, new_price);
    if (new_qty <= 0) return reject_replace(owner, id, Reason::BadQty, new_price);
    std::uint32_t lvl;
    if (!to_level(new_price, lvl)) return reject_replace(owner, id, Reason::BadPrice, new_price);

    Order& o = *pool_.at(idx);
    const Side side = o.side;
    if (lvl == o.level && new_qty <= o.qty) {  // shrink in place: keeps queue position
      const Qty d = o.qty - new_qty;
      if (d > 0) {
        levels_[sidx(side)][o.level].qty -= d;
        o.qty = new_qty;
        emit_public(PublicType::Reduce, side, id, new_price, d);
      }
      emit_report(ReportType::Replaced, Reason::None, Liquidity::None, side, owner, id, 0, new_price, new_qty,
                  new_qty);
      return;
    }
    // Priority lost: take it off the book, announce, and re-enter as a Limit.
    emit_public(PublicType::Delete, side, id, level_price(o.level), o.qty);
    remove(idx);
    emit_report(ReportType::Replaced, Reason::None, Liquidity::None, side, owner, id, 0, new_price, new_qty,
                new_qty);
    Qty rem = new_qty;
    bool done = false;
    match(id, owner, side, true, lvl, new_price, rem, done);
    if (rem > 0 && !done) rest_or_cancel(id, owner, side, lvl, rem);
  }

  // ---- queries -------------------------------------------------------------

  Price best_bid() const noexcept { return level_price(best_[0]); }
  Price best_ask() const noexcept { return level_price(best_[1]); }
  Qty best_bid_qty() const noexcept { return best_[0] == kNone ? 0 : levels_[0][best_[0]].qty; }
  Qty best_ask_qty() const noexcept { return best_[1] == kNone ? 0 : levels_[1][best_[1]].qty; }
  std::uint32_t order_count() const noexcept { return count_; }
  bool contains(OrderId id) const noexcept { return map_.find(id) != kNil; }

  Qty level_qty(Side side, Price p) const noexcept {
    std::uint32_t lvl;
    if (!to_level(p, lvl)) return 0;
    return levels_[sidx(side)][lvl].qty;
  }

  // Remaining quantity of a resting order, 0 if it is not resting.
  Qty order_qty(OrderId id) const noexcept {
    const std::uint32_t idx = map_.find(id);
    return idx == kNil ? 0 : pool_.at_const(idx)->qty;
  }

  // Same canonical walk as book::OrderBook::digest(), so an engine and a book
  // fed from its public events can be compared directly.
  std::uint64_t digest() const noexcept {
    std::uint64_t h = book::kDigestSeed;
    for (int s = 0; s < 2; ++s) {
      h = book::digest_combine(h, static_cast<std::uint64_t>(s));
      std::uint32_t l = best_[s];
      while (l != kNone) {
        const Level& L = levels_[s][l];
        h = book::digest_combine(h, static_cast<std::uint64_t>(level_price(l).raw()));
        h = book::digest_combine(h, static_cast<std::uint64_t>(L.qty));
        for (std::uint32_t i = L.head; i != kNil; i = pool_.at_const(i)->next) {
          h = book::digest_combine(h, pool_.at_const(i)->id);
          h = book::digest_combine(h, static_cast<std::uint64_t>(pool_.at_const(i)->qty));
        }
        if (s == 0) l = (l == 0) ? kNone : bitmap_[0].prev_le(l - 1);
        else l = (l + 1 >= num_levels_) ? kNone : bitmap_[1].next_ge(l + 1);
      }
    }
    return h;
  }

  // Structural invariants (as in OrderBook) plus the matching invariant: the
  // book is never crossed after a request completes. O(levels + orders).
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
          if (o.prev != prev || o.level != l || sidx(o.side) != s || o.qty <= 0) return false;
          if (map_.find(o.id) != i) return false;
          sum += o.qty;
          prev = i;
          if (++total > max_orders_) return false;
        }
        if (prev != L.tail || sum != L.qty) return false;
      }
      if (occupied == 0) {
        if (best_[s] != kNone) return false;
      } else if (best_[s] != (s == 0 ? hi : lo)) {
        return false;
      }
    }
    if (best_[0] != kNone && best_[1] != kNone && best_[0] >= best_[1]) return false;
    return total == count_ && total == pool_.in_use();
  }

 private:
  static constexpr std::uint32_t kNone = book::LevelBitmap::kNone;

  struct Order {
    Order(OrderId id_, Qty qty_, std::uint32_t level_, Side side_, Owner owner_) noexcept
        : id(id_), qty(qty_), prev(kNil), next(kNil), level(level_), owner(owner_), side(side_) {}
    OrderId id;
    Qty qty;
    std::uint32_t prev, next;
    std::uint32_t level;
    Owner owner;
    Side side;
  };
  static_assert(sizeof(Order) == 32);

  struct Level {
    std::uint32_t head, tail;
    Qty qty;
  };

  static constexpr std::size_t sidx(Side s) noexcept { return s == Side::Buy ? 0 : 1; }

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

  // Would a priced order on `side` at level `lvl` trade against the best opposite level?
  bool crosses(Side side, std::uint32_t lvl) const noexcept {
    const std::uint32_t b = best_[sidx(core::opposite(side))];
    if (b == kNone) return false;
    return side == Side::Buy ? b <= lvl : b >= lvl;
  }

  // ---- event emission --------------------------------------------------------

  void emit_report(ReportType t, Reason r, Liquidity liq, Side side, Owner owner, OrderId id, OrderId counter,
                   Price price, Qty qty, Qty leaves) noexcept {
    lis_->on_report(Report{t, r, liq, side, owner, id, counter, price, qty, leaves, ++report_seq_, ts_});
  }
  void emit_public(PublicType t, Side side, OrderId id, Price price, Qty qty) noexcept {
    lis_->on_public(PublicEvent{t, side, id, price, qty, ++public_seq_, ts_});
  }
  void reject(const NewOrder& o, Reason r) noexcept {
    emit_report(ReportType::Rejected, r, Liquidity::None, o.side, o.owner, o.id, 0, o.price, o.qty, 0);
  }
  void reject_replace(Owner owner, OrderId id, Reason r, Price p) noexcept {
    emit_report(ReportType::ReplaceRejected, r, Liquidity::None, Side::Buy, owner, id, 0, p, 0, 0);
  }
  void cancel_taker(const NewOrder& o, Qty rem, Reason r) noexcept {
    emit_report(ReportType::Canceled, r, Liquidity::None, o.side, o.owner, o.id, 0,
                o.type == OrderType::Market ? Price::invalid() : o.price, rem, 0);
  }

  // ---- book mutation ---------------------------------------------------------

  void link_tail(std::uint32_t idx, Order& o) noexcept {
    const std::size_t s = sidx(o.side);
    Level& L = levels_[s][o.level];
    o.prev = L.tail;
    o.next = kNil;
    if (L.tail != kNil) {
      pool_.at(L.tail)->next = idx;
    } else {
      L.head = idx;
      bitmap_[s].set(o.level);
      if (best_[s] == kNone || (s == 0 ? o.level > best_[s] : o.level < best_[s])) best_[s] = o.level;
    }
    L.tail = idx;
    L.qty += o.qty;
  }

  void unlink(Order& o) noexcept {
    const std::size_t s = sidx(o.side);
    Level& L = levels_[s][o.level];
    if (o.prev != kNil) pool_.at(o.prev)->next = o.next; else L.head = o.next;
    if (o.next != kNil) pool_.at(o.next)->prev = o.prev; else L.tail = o.prev;
    L.qty -= o.qty;
    if (L.head == kNil) {
      bitmap_[s].clear(o.level);
      if (best_[s] == o.level) {
        if (s == 0) best_[s] = (o.level == 0) ? kNone : bitmap_[0].prev_le(o.level - 1);
        else best_[s] = (o.level + 1 >= num_levels_) ? kNone : bitmap_[1].next_ge(o.level + 1);
      }
    }
  }

  void remove(std::uint32_t idx) noexcept {
    Order* o = pool_.at(idx);
    const OrderId id = o->id;
    unlink(*o);
    map_.erase(id);
    pool_.release(o);
    --count_;
  }

  void rest_or_cancel(OrderId id, Owner owner, Side side, std::uint32_t lvl, Qty rem) noexcept {
    Order* o = pool_.emplace(id, rem, lvl, side, owner);
    if (!o) {
      emit_report(ReportType::Canceled, Reason::BookFull, Liquidity::None, side, owner, id, 0, level_price(lvl),
                  rem, 0);
      return;
    }
    const std::uint32_t idx = pool_.index_of(o);
    map_.insert(id, idx);
    link_tail(idx, *o);
    ++count_;
    emit_public(PublicType::Add, side, id, level_price(lvl), rem);
  }

  // Cancels the resting order at `idx` on behalf of self-trade prevention.
  void stp_cancel_resting(std::uint32_t idx) noexcept {
    const Order m = *pool_.at(idx);
    emit_public(PublicType::Delete, m.side, m.id, level_price(m.level), m.qty);
    remove(idx);
    emit_report(ReportType::Canceled, Reason::StpCancel, Liquidity::None, m.side, m.owner, m.id, 0,
                level_price(m.level), m.qty, 0);
  }

  // Read-only: could the whole quantity trade right now? Mirrors what match()
  // would do, including how self-trade prevention treats same-owner orders.
  bool fok_feasible(Side side, std::uint32_t lvl, Qty qty, Owner owner) const noexcept {
    Qty need = qty;
    const std::size_t os = sidx(core::opposite(side));
    std::uint32_t l = best_[os];
    while (l != kNone && (side == Side::Buy ? l <= lvl : l >= lvl)) {
      for (std::uint32_t i = levels_[os][l].head; i != kNil; i = pool_.at_const(i)->next) {
        const Order& m = *pool_.at_const(i);
        if (stp_ != StpMode::None && m.owner == owner) {
          if (stp_ == StpMode::CancelOldest) continue;  // removed, matching goes on past it
          return false;                                  // the other modes stop or shrink the taker
        }
        need -= m.qty;
        if (need <= 0) return true;
      }
      if (os == 0) l = (l == 0) ? kNone : bitmap_[0].prev_le(l - 1);
      else l = (l + 1 >= num_levels_) ? kNone : bitmap_[1].next_ge(l + 1);
    }
    return false;
  }

  // Trades the taker against the opposite side. `rem` is updated in place.
  // `done` is set when STP already delivered the taker's terminal report.
  void match(OrderId id, Owner owner, Side side, bool priced, std::uint32_t lvl, Price tprice, Qty& rem,
             bool& done) noexcept {
    const std::size_t os = sidx(core::opposite(side));
    while (rem > 0) {
      const std::uint32_t bl = best_[os];
      if (bl == kNone) return;
      if (priced && !(side == Side::Buy ? bl <= lvl : bl >= lvl)) return;
      Level& L = levels_[os][bl];
      const Price px = level_price(bl);
      while (rem > 0 && L.head != kNil) {
        const std::uint32_t mi = L.head;
        Order& m = *pool_.at(mi);
        if (stp_ != StpMode::None && m.owner == owner) {
          switch (stp_) {
            case StpMode::CancelNewest:
              emit_report(ReportType::Canceled, Reason::StpCancel, Liquidity::None, side, owner, id, 0, tprice, rem, 0);
              rem = 0;
              done = true;
              return;
            case StpMode::CancelOldest:
              stp_cancel_resting(mi);
              continue;
            case StpMode::CancelBoth:
              stp_cancel_resting(mi);
              emit_report(ReportType::Canceled, Reason::StpCancel, Liquidity::None, side, owner, id, 0, tprice, rem, 0);
              rem = 0;
              done = true;
              return;
            case StpMode::DecrementAndCancel: {
              const Qty q = rem < m.qty ? rem : m.qty;
              rem -= q;
              emit_report(ReportType::Canceled, Reason::StpDecrement, Liquidity::None, side, owner, id, m.id, tprice, q,
                          rem);
              const Order snap = m;
              if (q == m.qty) {
                emit_public(PublicType::Delete, snap.side, snap.id, px, q);
                remove(mi);
              } else {
                m.qty -= q;
                L.qty -= q;
                emit_public(PublicType::Reduce, snap.side, snap.id, px, q);
              }
              emit_report(ReportType::Canceled, Reason::StpDecrement, Liquidity::None, snap.side, snap.owner,
                          snap.id, id, px, q, snap.qty - q);
              if (rem == 0) done = true;
              continue;
            }
            case StpMode::None:
              break;
          }
        }
        const Qty q = rem < m.qty ? rem : m.qty;
        rem -= q;
        const OrderId mid = m.id;
        const Owner mowner = m.owner;
        const Side mside = m.side;
        const Qty mleaves = m.qty - q;
        emit_report(ReportType::Fill, Reason::None, Liquidity::Taker, side, owner, id, mid, px, q, rem);
        emit_report(ReportType::Fill, Reason::None, Liquidity::Maker, mside, mowner, mid, id, px, q, mleaves);
        emit_public(PublicType::Exec, mside, mid, px, q);
        if (mleaves == 0) {
          remove(mi);
        } else {
          m.qty = mleaves;
          L.qty -= q;
        }
      }
    }
  }

  Listener* lis_{nullptr};
  std::uint64_t ts_{0}, report_seq_{0}, public_seq_{0};
  std::int64_t base_raw_{0}, tick_{1};
  std::uint32_t num_levels_{0}, max_orders_{0}, count_{0};
  StpMode stp_{StpMode::None};
  Level* levels_[2]{nullptr, nullptr};
  book::LevelBitmap bitmap_[2];
  std::uint32_t best_[2]{kNone, kNone};
  core::Pool<Order> pool_;
  book::IdMap map_;
};

}  // namespace hft::sim
