#pragma once
// A venue that exists only for our orders, sitting on top of a replayed tape.
//
// The tape is what the exchange published. Our orders are not in it, so they
// are simulated beside it with a queue position:
//
//   ahead  = quantity that was resting at our price when our order arrived
//            and has not yet traded or been cancelled.
//
// Every tape event updates it, using the arrival sequence numbers kept by
// TapeBook to tell orders ahead of ours from orders behind it:
//   - an order ahead executes:           ahead shrinks (a trade advances the queue)
//   - an order ahead is cancelled/cut:   ahead shrinks (Exact model only)
//   - an order BEHIND ours executes:     price-time priority says we would have
//                                        traded first, so we fill min(leaves, qty)
//   - a trade prints at a worse price:   the aggressor must have passed through us,
//                                        so we fill min(leaves, size of that print)
// Fills are reported at our resting price as the maker.
//
// Assumptions, all deliberate, none optimistic (the fourth one is a knob):
//   - No market impact. The tape is what it is whether or not we were there. A
//     real sweep that ends exactly on our order, with nothing behind us to show
//     it on the tape, is invisible here: fills can be missed, never invented.
//   - Passive orders only. Anything that would take liquidity (a crossing limit,
//     market, IOC, FOK) is rejected, because filling it honestly needs an
//     impact model. The tests measure how pessimistic this is against the real
//     matching engine (test_bt: queue-vs-engine).
//   - queue_penalty_pct: pretend an extra p% of the level's quantity was ahead of
//     us when we joined, and let only trades (not cancels) work it off. 0 is the
//     exact L3 model; larger values answer "what if the venue's queue were
//     deeper than the tape says", a standard robustness check on fill models.
//
// Self-check: the fill decision uses arrival order only, while `ahead` is tracked
// separately as a quantity. The two must agree (nothing is ahead of us when
// someone behind us trades, and ahead never shrinks by more than it holds).
// Disagreements are counted in queue_inconsistent; the tests demand zero.

#include <cstdint>

#include "hft/bt/tape_book.hpp"
#include "hft/sim/types.hpp"

namespace hft::bt {

struct VenueStats {
  std::uint64_t accepted = 0, rejected = 0, wouldcross = 0, fills = 0, filled_qty = 0, canceled = 0,
                cancel_rejected = 0, replaced = 0, replace_rejected = 0, full_fills = 0, trade_throughs = 0, queue_inconsistent = 0,
                quotes = 0, quotes_filled = 0, quoted_qty = 0;
  // quotes: orders accepted plus replaces that went to the back of a queue (each is a fresh offer);
  // quotes_filled: how many of those traded at least once; quoted_qty: shares offered, counted per offer
};

class VirtualVenue {
 public:
  static constexpr std::uint32_t kMaxLive = 256;

  void init(const TapeBook* book, std::uint32_t queue_penalty_pct = 0, sim::Owner owner = 1) noexcept {
    book_ = book;
    penalty_pct_ = queue_penalty_pct;
    owner_ = owner;
    n_ = 0;
    report_seq_ = 0;
    stats_ = VenueStats{};
  }

  // Call for each tape event BEFORE it is applied to the book (we need the
  // state in which the event happened). `out.report(const sim::Report&)`.
  template <class Out>
  void on_tape(const sim::PublicEvent& e, Out& out) noexcept {
    if (n_ == 0) return;
    switch (e.type) {
      case sim::PublicType::Exec: on_exec(e, out); break;
      case sim::PublicType::Reduce:
      case sim::PublicType::Delete: on_removal(e); break;
      case sim::PublicType::Add: break;  // newcomers are behind everyone we know about
    }
  }

  template <class Out>
  void on_new(const sim::NewOrder& o, std::uint64_t ts, Out& out) noexcept {
    sim::Report r = base(o.side, o.id, ts);
    r.price = o.price;
    if (o.id == 0) return reject(r, sim::Reason::BadId, out);
    if (o.qty <= 0) return reject(r, sim::Reason::BadQty, out);
    if (o.type != sim::OrderType::Limit && o.type != sim::OrderType::PostOnly)
      return reject(r, sim::Reason::BadPrice, out);  // taking liquidity needs an impact model
    if (!book_->price_ok(o.price)) return reject(r, sim::Reason::BadPrice, out);
    if (find(o.id) >= 0) return reject(r, sim::Reason::Duplicate, out);
    if (crosses(o.side, o.price)) {
      ++stats_.wouldcross;
      return reject(r, sim::Reason::WouldCross, out);
    }
    if (n_ == kMaxLive) return reject(r, sim::Reason::BookFull, out);
    Live& v = live_[n_++];
    v = Live{o.id, o.side, o.price, o.qty, 0, 0, book_->seq() + 1, false};
    queue_up(v);
    ++stats_.accepted;
    ++stats_.quotes;
    stats_.quoted_qty += static_cast<std::uint64_t>(o.qty);
    r.type = sim::ReportType::Accepted;
    r.qty = o.qty;
    r.leaves = o.qty;
    out.report(r);
  }

  template <class Out>
  void on_cancel(core::OrderId id, std::uint64_t ts, Out& out) noexcept {
    const int k = find(id);
    sim::Report r = base(core::Side::Buy, id, ts);
    if (k < 0) {
      ++stats_.cancel_rejected;
      r.type = sim::ReportType::CancelRejected;
      r.reason = sim::Reason::UnknownOrder;
      out.report(r);
      return;
    }
    r.side = live_[k].side;
    r.price = live_[k].price;
    r.type = sim::ReportType::Canceled;
    r.reason = sim::Reason::UserRequested;
    r.qty = live_[k].leaves;
    r.leaves = 0;
    remove_at(k);
    ++stats_.canceled;
    out.report(r);
  }

  // new_qty is the new open quantity, as in the order entry protocol.
  template <class Out>
  void on_replace(core::OrderId id, Price new_price, Qty new_qty, std::uint64_t ts, Out& out) noexcept {
    const int k = find(id);
    sim::Report r = base(core::Side::Buy, id, ts);
    r.type = sim::ReportType::ReplaceRejected;
    if (k < 0) return rej_replace(r, sim::Reason::UnknownOrder, out);
    Live& v = live_[k];
    r.side = v.side;
    r.price = v.price;
    if (new_qty <= 0) return rej_replace(r, sim::Reason::BadQty, out);
    if (!book_->price_ok(new_price)) return rej_replace(r, sim::Reason::BadPrice, out);
    if (crosses(v.side, new_price)) {
      ++stats_.wouldcross;
      return rej_replace(r, sim::Reason::WouldCross, out);
    }
    if (new_price == v.price && new_qty <= v.leaves) {
      v.leaves = new_qty;  // shrink in place keeps the queue position
    } else {
      v.price = new_price;  // anything else goes to the back of the new level
      v.leaves = new_qty;
      v.join = book_->seq() + 1;
      queue_up(v);
      v.hit = false;
      ++stats_.quotes;
    }
    ++stats_.replaced;
    stats_.quoted_qty += static_cast<std::uint64_t>(new_qty);
    r.type = sim::ReportType::Replaced;
    r.reason = sim::Reason::None;
    r.price = v.price;
    r.qty = v.leaves;
    r.leaves = v.leaves;
    out.report(r);
  }

  std::uint32_t live_orders() const noexcept { return n_; }
  const VenueStats& stats() const noexcept { return stats_; }

  // Queue state of one order, for tests and diagnostics. False if not live.
  bool queue_state(core::OrderId id, Qty& ahead, Qty& leaves) const noexcept {
    const int k = find(id);
    if (k < 0) return false;
    ahead = live_[k].ahead;
    leaves = live_[k].leaves;
    return true;
  }

 private:
  struct Live {
    core::OrderId id;
    core::Side side;
    Price price;
    Qty leaves;
    Qty ahead;           // quantity ahead of us (exact, from the tape)
    Qty extra;           // phantom quantity from queue_penalty_pct, worked off by trades only
    std::uint64_t join;  // tape orders with seq < join arrived before us
    bool hit;            // has traded at least once
  };

  void queue_up(Live& v) noexcept {
    v.ahead = book_->level_qty(v.side, v.price);
    v.extra = (v.ahead * static_cast<Qty>(penalty_pct_) + 99) / 100;
  }

  sim::Report base(core::Side side, core::OrderId id, std::uint64_t ts) noexcept {
    sim::Report r{};
    r.type = sim::ReportType::Accepted;
    r.reason = sim::Reason::None;
    r.liq = sim::Liquidity::None;
    r.side = side;
    r.owner = owner_;
    r.id = id;
    r.seq = ++report_seq_;
    r.ts = ts;
    return r;
  }

  template <class Out>
  void reject(sim::Report& r, sim::Reason why, Out& out) noexcept {
    ++stats_.rejected;
    r.type = sim::ReportType::Rejected;
    r.reason = why;
    out.report(r);
  }
  template <class Out>
  void rej_replace(sim::Report& r, sim::Reason why, Out& out) noexcept {
    ++stats_.replace_rejected;
    r.reason = why;
    out.report(r);
  }

  bool crosses(core::Side s, Price p) const noexcept {
    if (s == core::Side::Buy) {
      const Price a = book_->best_ask();
      return a != Price::invalid() && p.raw() >= a.raw();
    }
    const Price b = book_->best_bid();
    return b != Price::invalid() && p.raw() <= b.raw();
  }

  int find(core::OrderId id) const noexcept {
    for (std::uint32_t i = 0; i < n_; ++i)
      if (live_[i].id == id) return static_cast<int>(i);
    return -1;
  }

  void remove_at(int k) noexcept {
    for (std::uint32_t i = static_cast<std::uint32_t>(k); i + 1 < n_; ++i) live_[i] = live_[i + 1];  // keep arrival order
    --n_;
  }

  template <class Out>
  void fill(Live& v, Qty q, std::uint64_t ts, Out& out) noexcept {
    sim::Report r = base(v.side, v.id, ts);
    r.type = sim::ReportType::Fill;
    r.liq = sim::Liquidity::Maker;
    r.price = v.price;
    r.qty = q;
    v.leaves -= q;
    r.leaves = v.leaves;
    if (!v.hit) {
      v.hit = true;
      ++stats_.quotes_filled;
    }
    ++stats_.fills;
    stats_.filled_qty += static_cast<std::uint64_t>(q);
    if (v.leaves == 0) ++stats_.full_fills;
    out.report(r);
  }

  // Quantity of a print that actually reaches us once the phantom queue has taken its share.
  Qty reach(Live& v, Qty avail) noexcept {
    const Qty eat = v.extra < avail ? v.extra : avail;
    v.extra -= eat;
    return avail - eat;
  }

  template <class Out>
  void on_exec(const sim::PublicEvent& e, Out& out) noexcept {
    TapeBook::Info inf;
    const bool known = book_->lookup(e.id, inf);
    Qty rem = e.qty;
    for (std::uint32_t i = 0; i < n_;) {
      Live& v = live_[i];
      if (v.side != e.side) {
        ++i;
        continue;
      }
      const bool same_px = v.price == e.price;
      const bool through = e.side == core::Side::Buy ? e.price.raw() < v.price.raw() : e.price.raw() > v.price.raw();
      if (through) {
        // Price priority: a print at a worse price than ours means whoever was selling
        // would have reached us first. How much of its size got past us is unknown, but
        // at least the size of this print did, so that is the provable part of our fill.
        if (rem > 0) {
          ++stats_.trade_throughs;
          if (penalty_pct_ == 0 && v.ahead != 0) ++stats_.queue_inconsistent;
          v.ahead = 0;
          const Qty reach_q = reach(v, rem);
          const Qty q = v.leaves < reach_q ? v.leaves : reach_q;
          rem -= q + (rem - reach_q);  // what the phantom queue ate and what we took
          if (q > 0) fill(v, q, e.ts, out);
        }
      } else if (same_px) {
        if (known && inf.seq < v.join) {
          if (v.ahead < e.qty) ++stats_.queue_inconsistent;
          v.ahead -= v.ahead < e.qty ? v.ahead : e.qty;  // a trade ahead of us advances the queue
        } else if (rem > 0) {
          // behind us, so we are first in line for what was traded
          if (v.ahead != 0) ++stats_.queue_inconsistent;
          v.ahead = 0;
          const Qty reach_q = reach(v, rem);
          const Qty q = v.leaves < reach_q ? v.leaves : reach_q;
          rem -= q + (rem - reach_q);
          if (q > 0) fill(v, q, e.ts, out);
        }
      }
      if (v.leaves == 0) {
        remove_at(static_cast<int>(i));
      } else {
        ++i;
      }
    }
  }

  void on_removal(const sim::PublicEvent& e) noexcept {
    TapeBook::Info inf;
    if (!book_->lookup(e.id, inf)) return;
    for (std::uint32_t i = 0; i < n_; ++i) {
      Live& v = live_[i];
      if (v.side == inf.side && v.price == inf.price && inf.seq < v.join) {
        if (v.ahead < e.qty) ++stats_.queue_inconsistent;
        v.ahead -= v.ahead < e.qty ? v.ahead : e.qty;
      }
    }
  }

  const TapeBook* book_ = nullptr;
  std::uint32_t penalty_pct_ = 0;
  sim::Owner owner_ = 1;
  Live live_[kMaxLive];
  std::uint32_t n_ = 0;
  std::uint64_t report_seq_ = 0;
  VenueStats stats_;
};

}  // namespace hft::bt
