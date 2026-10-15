#pragma once
// Slow, obviously-correct matching engine used as the oracle for the real one.
// It keeps every resting order in one flat vector and finds the next order to
// match by scanning for the best price and earliest arrival. It shares no
// storage or matching code with hft::sim::MatchingEngine, only the vocabulary
// types and the digest mixer, and it is written from the rules in the engine
// header rather than from the engine's code.
//
// FOK is decided by a dry run of the real matching rules on a copy of the
// book (the engine instead walks the book read-only), so the two feasibility
// checks agree only if both are right.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "hft/book/digest.hpp"
#include "hft/sim/matching_engine.hpp"

namespace testsupport {

using namespace hft::sim;
using hft::book::digest_combine;
using hft::book::kDigestSeed;

// Collects what an engine emits, for comparison with the reference.
struct Recorder {
  std::vector<Report> reports;
  std::vector<PublicEvent> pubs;
  void on_report(const Report& r) { reports.push_back(r); }
  void on_public(const PublicEvent& e) { pubs.push_back(e); }
  void clear() {
    reports.clear();
    pubs.clear();
  }
};

inline bool same(const Report& a, const Report& b) {
  return a.type == b.type && a.reason == b.reason && a.liq == b.liq && a.side == b.side && a.owner == b.owner &&
         a.id == b.id && a.counter_id == b.counter_id && a.price == b.price && a.qty == b.qty &&
         a.leaves == b.leaves && a.seq == b.seq && a.ts == b.ts;
}
inline bool same(const PublicEvent& a, const PublicEvent& b) {
  return a.type == b.type && a.side == b.side && a.id == b.id && a.price == b.price && a.qty == b.qty &&
         a.seq == b.seq && a.ts == b.ts;
}

class RefMatcher {
 public:
  explicit RefMatcher(const EngineConfig& c) : c_(c) {}

  Recorder out;

  void new_order(const NewOrder& o, std::uint64_t ts) {
    ts_ = ts;
    if (o.id == 0) return reject(o, Reason::BadId);
    if (o.qty <= 0) return reject(o, Reason::BadQty);
    const bool priced = o.type != OrderType::Market;
    if (priced && !valid_price(o.price.raw())) return reject(o, Reason::BadPrice);
    if (find(o.id) >= 0) return reject(o, Reason::Duplicate);
    if (o.type == OrderType::PostOnly && would_cross(o.side, o.price.raw())) return reject(o, Reason::WouldCross);

    const Price tprice = priced ? o.price : Price::invalid();
    rep(ReportType::Accepted, Reason::None, Liquidity::None, o.side, o.owner, o.id, 0, tprice, o.qty, o.qty);

    if (o.type == OrderType::FOK) {
      RefMatcher dry(*this);
      dry.dry_ = true;
      Qty rem = o.qty;
      bool done = false;
      Qty traded = 0;
      dry.match(o.id, o.owner, o.side, true, o.price.raw(), tprice, rem, done, traded);
      if (traded != o.qty) {
        rep(ReportType::Canceled, Reason::FokUnfillable, Liquidity::None, o.side, o.owner, o.id, 0, o.price, o.qty, 0);
        return;
      }
    }

    Qty rem = o.qty;
    bool done = false;
    Qty traded = 0;
    match(o.id, o.owner, o.side, priced, o.price.raw(), tprice, rem, done, traded);
    if (rem == 0 || done) return;
    switch (o.type) {
      case OrderType::Limit:
      case OrderType::PostOnly:
        rest_or_cancel(o.id, o.owner, o.side, o.price.raw(), rem);
        break;
      case OrderType::IOC:
        rep(ReportType::Canceled, Reason::IocRemainder, Liquidity::None, o.side, o.owner, o.id, 0, tprice, rem, 0);
        break;
      case OrderType::Market:
        rep(ReportType::Canceled, Reason::MarketRemainder, Liquidity::None, o.side, o.owner, o.id, 0, tprice, rem,
            0);
        break;
      case OrderType::FOK:
        rep(ReportType::Canceled, Reason::FokUnfillable, Liquidity::None, o.side, o.owner, o.id, 0, tprice, rem, 0);
        break;
    }
  }

  void cancel(OrderId id, Owner owner, std::uint64_t ts) {
    ts_ = ts;
    const int i = find(id);
    if (i < 0 || book_[static_cast<std::size_t>(i)].owner != owner) {
      rep(ReportType::CancelRejected, Reason::UnknownOrder, Liquidity::None, Side::Buy, owner, id, 0,
          Price::invalid(), 0, 0);
      return;
    }
    const R o = book_[static_cast<std::size_t>(i)];
    pub(PublicType::Delete, o.side, o.id, Price::from_raw(o.price), o.qty);
    book_.erase(book_.begin() + i);
    rep(ReportType::Canceled, Reason::UserRequested, Liquidity::None, o.side, o.owner, o.id, 0,
        Price::from_raw(o.price), o.qty, 0);
  }

  void replace(OrderId id, Owner owner, Price np, Qty nq, std::uint64_t ts) {
    ts_ = ts;
    const int i = find(id);
    auto rej = [&](Reason r) {
      rep(ReportType::ReplaceRejected, r, Liquidity::None, Side::Buy, owner, id, 0, np, 0, 0);
    };
    if (i < 0 || book_[static_cast<std::size_t>(i)].owner != owner) return rej(Reason::UnknownOrder);
    if (nq <= 0) return rej(Reason::BadQty);
    if (!valid_price(np.raw())) return rej(Reason::BadPrice);
    R& o = book_[static_cast<std::size_t>(i)];
    const Side side = o.side;
    if (o.price == np.raw() && nq <= o.qty) {
      const Qty d = o.qty - nq;
      if (d > 0) {
        o.qty = nq;
        pub(PublicType::Reduce, side, id, np, d);
      }
      rep(ReportType::Replaced, Reason::None, Liquidity::None, side, owner, id, 0, np, nq, nq);
      return;
    }
    pub(PublicType::Delete, side, id, Price::from_raw(o.price), o.qty);
    book_.erase(book_.begin() + i);
    rep(ReportType::Replaced, Reason::None, Liquidity::None, side, owner, id, 0, np, nq, nq);
    Qty rem = nq;
    bool done = false;
    Qty traded = 0;
    match(id, owner, side, true, np.raw(), np, rem, done, traded);
    if (rem > 0 && !done) rest_or_cancel(id, owner, side, np.raw(), rem);
  }

  // Canonical digest, identical walk to the engine's and OrderBook's.
  std::uint64_t digest() const {
    std::uint64_t h = kDigestSeed;
    for (int s = 0; s < 2; ++s) {
      h = digest_combine(h, static_cast<std::uint64_t>(s));
      const Side side = s == 0 ? Side::Buy : Side::Sell;
      std::vector<std::int64_t> prices;
      for (const R& o : book_)
        if (o.side == side) prices.push_back(o.price);
      std::sort(prices.begin(), prices.end());
      prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
      if (s == 0) std::reverse(prices.begin(), prices.end());
      for (std::int64_t p : prices) {
        std::vector<const R*> q;
        Qty total = 0;
        for (const R& o : book_)
          if (o.side == side && o.price == p) {
            q.push_back(&o);
            total += o.qty;
          }
        std::sort(q.begin(), q.end(), [](const R* a, const R* b) { return a->arr < b->arr; });
        h = digest_combine(h, static_cast<std::uint64_t>(p));
        h = digest_combine(h, static_cast<std::uint64_t>(total));
        for (const R* o : q) {
          h = digest_combine(h, o->id);
          h = digest_combine(h, static_cast<std::uint64_t>(o->qty));
        }
      }
    }
    return h;
  }

  std::size_t order_count() const { return book_.size(); }

 private:
  struct R {
    OrderId id;
    Owner owner;
    Side side;
    std::int64_t price;
    Qty qty;
    std::uint64_t arr;
  };

  bool valid_price(std::int64_t raw) const {
    const std::int64_t off = raw - c_.base.raw();
    return off >= 0 && off % c_.tick_raw == 0 && off / c_.tick_raw < static_cast<std::int64_t>(c_.num_levels);
  }

  int find(OrderId id) const {
    for (std::size_t i = 0; i < book_.size(); ++i)
      if (book_[i].id == id) return static_cast<int>(i);
    return -1;
  }

  // Index of the best resting order on `side` (price priority, then arrival), or -1.
  int best(Side side) const {
    int b = -1;
    for (std::size_t i = 0; i < book_.size(); ++i) {
      const R& o = book_[i];
      if (o.side != side) continue;
      if (b < 0) {
        b = static_cast<int>(i);
        continue;
      }
      const R& c = book_[static_cast<std::size_t>(b)];
      const bool better = side == Side::Buy ? o.price > c.price : o.price < c.price;
      if (better || (o.price == c.price && o.arr < c.arr)) b = static_cast<int>(i);
    }
    return b;
  }

  bool would_cross(Side side, std::int64_t price) const {
    const int b = best(opposite(side));
    if (b < 0) return false;
    const std::int64_t bp = book_[static_cast<std::size_t>(b)].price;
    return side == Side::Buy ? bp <= price : bp >= price;
  }

  void rep(ReportType t, Reason r, Liquidity liq, Side side, Owner owner, OrderId id, OrderId counter, Price px,
           Qty qty, Qty leaves) {
    if (dry_) return;
    out.reports.push_back(Report{t, r, liq, side, owner, id, counter, px, qty, leaves, ++rseq_, ts_});
  }
  void pub(PublicType t, Side side, OrderId id, Price px, Qty qty) {
    if (dry_) return;
    out.pubs.push_back(PublicEvent{t, side, id, px, qty, ++pseq_, ts_});
  }
  void reject(const NewOrder& o, Reason r) {
    rep(ReportType::Rejected, r, Liquidity::None, o.side, o.owner, o.id, 0, o.price, o.qty, 0);
  }

  void rest_or_cancel(OrderId id, Owner owner, Side side, std::int64_t price, Qty rem) {
    if (book_.size() >= c_.max_orders) {
      rep(ReportType::Canceled, Reason::BookFull, Liquidity::None, side, owner, id, 0, Price::from_raw(price), rem,
          0);
      return;
    }
    book_.push_back(R{id, owner, side, price, rem, ++arr_});
    pub(PublicType::Add, side, id, Price::from_raw(price), rem);
  }

  void match(OrderId id, Owner owner, Side side, bool priced, std::int64_t price, Price tprice, Qty& rem,
             bool& done, Qty& traded) {
    while (rem > 0) {
      const int bi = best(opposite(side));
      if (bi < 0) return;
      const R m = book_[static_cast<std::size_t>(bi)];
      if (priced && !(side == Side::Buy ? m.price <= price : m.price >= price)) return;
      const Price mpx = Price::from_raw(m.price);
      if (c_.stp != StpMode::None && m.owner == owner) {
        if (c_.stp == StpMode::CancelNewest) {
          rep(ReportType::Canceled, Reason::StpCancel, Liquidity::None, side, owner, id, 0, tprice, rem, 0);
          rem = 0;
          done = true;
          return;
        }
        if (c_.stp == StpMode::CancelOldest || c_.stp == StpMode::CancelBoth) {
          pub(PublicType::Delete, m.side, m.id, mpx, m.qty);
          book_.erase(book_.begin() + bi);
          rep(ReportType::Canceled, Reason::StpCancel, Liquidity::None, m.side, m.owner, m.id, 0, mpx, m.qty, 0);
          if (c_.stp == StpMode::CancelBoth) {
            rep(ReportType::Canceled, Reason::StpCancel, Liquidity::None, side, owner, id, 0, tprice, rem, 0);
            rem = 0;
            done = true;
            return;
          }
          continue;
        }
        // DecrementAndCancel
        const Qty q = std::min(rem, m.qty);
        rem -= q;
        rep(ReportType::Canceled, Reason::StpDecrement, Liquidity::None, side, owner, id, m.id, tprice, q, rem);
        if (q == m.qty) {
          pub(PublicType::Delete, m.side, m.id, mpx, q);
          book_.erase(book_.begin() + bi);
        } else {
          book_[static_cast<std::size_t>(bi)].qty -= q;
          pub(PublicType::Reduce, m.side, m.id, mpx, q);
        }
        rep(ReportType::Canceled, Reason::StpDecrement, Liquidity::None, m.side, m.owner, m.id, id, mpx, q, m.qty - q);
        if (rem == 0) done = true;
        continue;
      }
      const Qty q = std::min(rem, m.qty);
      rem -= q;
      traded += q;
      rep(ReportType::Fill, Reason::None, Liquidity::Taker, side, owner, id, m.id, mpx, q, rem);
      rep(ReportType::Fill, Reason::None, Liquidity::Maker, m.side, m.owner, m.id, id, mpx, q, m.qty - q);
      pub(PublicType::Exec, m.side, m.id, mpx, q);
      if (q == m.qty) book_.erase(book_.begin() + bi);
      else book_[static_cast<std::size_t>(bi)].qty -= q;
    }
  }

  EngineConfig c_;
  std::vector<R> book_;
  std::uint64_t arr_{0}, rseq_{0}, pseq_{0}, ts_{0};
  bool dry_{false};
};

}  // namespace testsupport
