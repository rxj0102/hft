#pragma once
// Order management: the client-side state machine for every order we send, and
// the position and exposure books that pre-trade risk reads.
//
// States and the events that move them are in two small enums, and the
// Action table below says, for every (state, event) pair, what happens. The
// table is the specification: Oms dispatches through it, and the tests drive
// every cell and compare against an independently written copy.
//
//   Apply    the event is legal here and changes (or confirms) the order
//   Ignore   a benign race that is expected under latency, counted and dropped
//            (a cancel/replace reject arriving after the order already ended)
//   Anomaly  a protocol violation (a fill after a cancel, a second ack, ...);
//            counted, the order is left untouched
//   Refuse   a local request that is not allowed in this state (only for the
//            RequestCancel and RequestReplace events); the caller is told
//
// Races the table is built to absorb (reports can cross our requests on the
// wire): a fill while PendingCancel/PendingReplace keeps the pending state;
// a full fill there ends the order, and the cancel/replace reject that follows
// is Ignored. One request is outstanding per order: a second cancel or
// replace while one is pending is Refused rather than queued.
//
// Memory: records live in a fixed pool. Finished orders are kept (so late
// events can still be classified) until the pool needs the slot; then the
// oldest finished order is evicted. If every slot holds a live order, new
// orders are refused (Submit::Full).

#include <cstdint>

#include "hft/book/id_map.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/types.hpp"

namespace hft::oms {

using core::i128;
using sim::OrderId;
using sim::OrderType;
using sim::Price;
using sim::Qty;
using sim::Report;
using sim::ReportType;
using sim::Side;

enum class State : std::uint8_t {
  PendingNew,
  New,
  PartiallyFilled,
  PendingCancel,
  PendingReplace,
  Filled,
  Canceled,
  Rejected,
};
inline constexpr int kNumStates = 8;

enum class Event : std::uint8_t {
  Ack,            // exchange accepted the order
  Reject,         // exchange rejected the new order
  Fill,
  Cancel,         // order (or part of it) cancelled by the exchange
  Replaced,
  CancelReject,
  ReplaceReject,
  RequestCancel,  // local
  RequestReplace, // local
};
inline constexpr int kNumEvents = 9;

enum class Action : std::uint8_t { Apply, Ignore, Anomaly, Refuse };

constexpr bool is_terminal(State s) noexcept {
  return s == State::Filled || s == State::Canceled || s == State::Rejected;
}
constexpr bool is_workable(State s) noexcept { return s == State::New || s == State::PartiallyFilled; }

// Rows: State. Columns: Event, in the order declared above.
inline constexpr Action kAction[kNumStates][kNumEvents] = {
    //                 Ack              Reject           Fill             Cancel           Replaced         CancelReject     ReplaceReject    ReqCancel        ReqReplace
    /* PendingNew  */ {Action::Apply,   Action::Apply,   Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Refuse,  Action::Refuse},
    /* New         */ {Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply,   Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply},
    /* PartFilled  */ {Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply,   Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply},
    /* PendCancel  */ {Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply,   Action::Anomaly, Action::Apply,   Action::Anomaly, Action::Refuse,  Action::Refuse},
    /* PendReplace */ {Action::Anomaly, Action::Anomaly, Action::Apply,   Action::Apply,   Action::Apply,   Action::Anomaly, Action::Apply,   Action::Refuse,  Action::Refuse},
    /* Filled      */ {Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Ignore,  Action::Ignore,  Action::Refuse,  Action::Refuse},
    /* Canceled    */ {Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Ignore,  Action::Ignore,  Action::Refuse,  Action::Refuse},
    /* Rejected    */ {Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Anomaly, Action::Ignore,  Action::Ignore,  Action::Refuse,  Action::Refuse},
};

struct OmsConfig {
  std::uint32_t max_orders = 1u << 14;
};

struct OrderRec {
  OrderId id;
  Price price;      // current limit price (invalid for market)
  Qty qty;          // total quantity: cum + leaves
  Qty cum;          // filled so far
  Qty leaves;       // open quantity
  Qty reserved;     // exposure counted against risk for this order
  Price pend_price; // PendingReplace: requested values
  Qty pend_qty;
  std::uint64_t last_ts;
  std::uint32_t a_prev, a_next;  // active (non-terminal) list
  State state;
  Side side;
  OrderType type;
};

// Position and exposure, the inputs to pre-trade risk.
struct Position {
  std::int64_t pos = 0;          // signed net filled quantity
  std::int64_t open_buy = 0;     // reserved quantity on working buy orders
  std::int64_t open_sell = 0;
  std::uint32_t open_orders = 0; // non-terminal orders
  i128 cash = 0;                 // sum of -side * price * qty over fills, raw price units
  std::uint64_t buy_qty = 0, sell_qty = 0, fills = 0;

  // Cash plus the position valued at `ref`: PnL in raw price units.
  i128 mark_pnl(Price ref) const noexcept { return cash + static_cast<i128>(pos) * ref.raw(); }
};

struct OmsStats {
  std::uint64_t anomalies = 0;       // Anomaly cells hit
  std::uint64_t ignored = 0;         // Ignore cells hit
  std::uint64_t unknown_reports = 0; // report for an id we do not hold
  std::uint64_t orphan_fills = 0;    // ... and it was a fill: the dangerous kind
  std::uint64_t overfills = 0;       // fill larger than leaves (or non-positive)
  std::uint64_t mismatches = 0;      // exchange leaves/qty disagree with ours
  std::uint64_t evicted = 0;
  std::uint64_t refused = 0;         // local requests refused
};

enum class Submit : std::uint8_t { Ok, BadParams, Duplicate, Full };
enum class Request : std::uint8_t { Ok, Unknown, NotAllowed, BadParams };

class Oms {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  bool init(core::Arena& arena, const OmsConfig& cfg) noexcept {
    max_ = cfg.max_orders;
    if (max_ == 0) return false;
    if (!pool_.init(arena, max_)) return false;
    std::uint32_t cap = 2;
    while (cap < max_ * 2u) cap <<= 1;
    if (!map_.init(arena, cap)) return false;
    ring_ = arena.allocate_array<std::uint32_t>(max_);
    if (!ring_) return false;
    return true;
  }

  // ---- local actions ---------------------------------------------------------

  // Registers a new order as PendingNew (call after risk approved it).
  Submit on_send_new(OrderId id, Side side, OrderType type, Price price, Qty qty, std::uint64_t ts) noexcept {
    if (id == 0 || qty <= 0) return Submit::BadParams;
    if (map_.find(id) != kNil) return Submit::Duplicate;
    OrderRec* r = pool_.emplace();
    if (!r) {
      if (!evict_oldest_terminal()) return Submit::Full;
      r = pool_.emplace();
      if (!r) return Submit::Full;
    }
    const std::uint32_t idx = pool_.index_of(r);
    *r = OrderRec{id, type == OrderType::Market ? Price::invalid() : price, qty, 0, qty, 0, Price::invalid(), 0, ts,
                  kNil, kNil, State::PendingNew, side, type};
    map_.insert(id, idx);
    link_active(idx, *r);
    set_reserved(*r, qty);
    return Submit::Ok;
  }

  Request on_request_cancel(OrderId id, std::uint64_t ts) noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return Request::Unknown;
    OrderRec& r = *pool_.at(idx);
    if (kAction[static_cast<int>(r.state)][static_cast<int>(Event::RequestCancel)] != Action::Apply) {
      ++stats_.refused;
      return Request::NotAllowed;
    }
    r.state = State::PendingCancel;
    r.last_ts = ts;
    return Request::Ok;
  }

  Request on_request_replace(OrderId id, Price new_price, Qty new_qty, std::uint64_t ts) noexcept {
    const std::uint32_t idx = map_.find(id);
    if (idx == kNil) return Request::Unknown;
    OrderRec& r = *pool_.at(idx);
    if (kAction[static_cast<int>(r.state)][static_cast<int>(Event::RequestReplace)] != Action::Apply) {
      ++stats_.refused;
      return Request::NotAllowed;
    }
    if (new_qty <= 0 || r.type == OrderType::Market) return Request::BadParams;
    r.state = State::PendingReplace;
    r.pend_price = new_price;
    r.pend_qty = new_qty;
    r.last_ts = ts;
    set_reserved(r, r.leaves + new_qty);  // worst case: fills race the replace, then the new size rests
    return Request::Ok;
  }

  // ---- exchange reports --------------------------------------------------------

  void on_report(const Report& rep) noexcept {
    Event ev;
    switch (rep.type) {
      case ReportType::Accepted: ev = Event::Ack; break;
      case ReportType::Rejected: ev = Event::Reject; break;
      case ReportType::Fill: ev = Event::Fill; break;
      case ReportType::Canceled: ev = Event::Cancel; break;
      case ReportType::Replaced: ev = Event::Replaced; break;
      case ReportType::CancelRejected: ev = Event::CancelReject; break;
      case ReportType::ReplaceRejected: ev = Event::ReplaceReject; break;
      default: ++stats_.anomalies; return;
    }
    const std::uint32_t idx = map_.find(rep.id);
    if (idx == kNil) {
      ++stats_.unknown_reports;
      if (ev == Event::Fill) ++stats_.orphan_fills;
      return;
    }
    OrderRec& r = *pool_.at(idx);
    switch (kAction[static_cast<int>(r.state)][static_cast<int>(ev)]) {
      case Action::Ignore: ++stats_.ignored; return;
      case Action::Anomaly:
      case Action::Refuse: ++stats_.anomalies; return;
      case Action::Apply: break;
    }
    r.last_ts = rep.ts;
    switch (ev) {
      case Event::Ack:
        if (rep.qty != r.qty) ++stats_.mismatches;
        r.state = State::New;
        break;
      case Event::Reject:
        finish(idx, r, State::Rejected);
        break;
      case Event::Fill:
        on_fill(idx, r, rep);
        break;
      case Event::Cancel:
        on_cancel(idx, r, rep);
        break;
      case Event::Replaced:
        if (rep.leaves <= 0 || rep.price.raw() <= 0) {  // nonsense from the exchange: keep the old order
          ++stats_.anomalies;
          r.state = working_state(r);
          r.pend_qty = 0;
          r.pend_price = Price::invalid();
          set_reserved(r, r.leaves);
          break;
        }
        r.price = rep.price;
        r.leaves = rep.leaves;
        r.qty = r.cum + r.leaves;
        r.state = working_state(r);
        r.pend_qty = 0;
        r.pend_price = Price::invalid();
        set_reserved(r, r.leaves);
        break;
      case Event::CancelReject:
      case Event::ReplaceReject:
        r.state = working_state(r);
        r.pend_qty = 0;
        r.pend_price = Price::invalid();
        set_reserved(r, r.leaves);
        break;
      default: break;
    }
  }

  // ---- queries -------------------------------------------------------------------

  const OrderRec* find(OrderId id) const noexcept {
    const std::uint32_t idx = map_.find(id);
    return idx == kNil ? nullptr : pool_.at_const(idx);
  }
  const Position& position() const noexcept { return pos_; }
  const OmsStats& stats() const noexcept { return stats_; }
  std::uint32_t open_orders() const noexcept { return pos_.open_orders; }

  // Calls f(const OrderRec&) for every non-terminal order. Not a hot path.
  template <class F>
  void for_each_open(F&& f) const noexcept {
    for (std::uint32_t i = head_; i != kNil; i = pool_.at_const(i)->a_next) f(*pool_.at_const(i));
  }

  // Verifies the books against the records: exposure totals equal the sum of
  // per-order reservations, the active list holds exactly the non-terminal
  // orders, leaves/cum add up, terminal orders reserve nothing. O(orders).
  bool check_invariants() const noexcept {
    std::int64_t ob = 0, os = 0;
    std::uint32_t active = 0;
    std::uint32_t prev = kNil;
    for (std::uint32_t i = head_; i != kNil; i = pool_.at_const(i)->a_next) {
      const OrderRec& r = *pool_.at_const(i);
      if (r.a_prev != prev || is_terminal(r.state) || map_.find(r.id) != i) return false;
      if (r.cum < 0 || r.leaves < 0 || r.cum + r.leaves != r.qty || r.leaves == 0) return false;
      Qty want = r.leaves;
      if (r.state == State::PendingNew) want = r.qty;
      if (r.state == State::PendingReplace) want = r.leaves + r.pend_qty;
      if (r.reserved != want) return false;
      (r.side == Side::Buy ? ob : os) += r.reserved;
      prev = i;
      if (++active > max_) return false;
    }
    if (prev != tail_ || active != pos_.open_orders || ob != pos_.open_buy || os != pos_.open_sell) return false;
    for (std::uint32_t k = 0; k < ring_n_; ++k) {
      const OrderRec& r = *pool_.at_const(ring_[(ring_head_ + k) % max_]);
      if (!is_terminal(r.state) || r.reserved != 0 || r.leaves < 0) return false;
    }
    return active + ring_n_ == pool_.in_use();
  }

 private:
  State working_state(const OrderRec& r) const noexcept {
    return r.cum > 0 ? State::PartiallyFilled : State::New;
  }

  void link_active(std::uint32_t idx, OrderRec& r) noexcept {
    r.a_prev = tail_;
    r.a_next = kNil;
    if (tail_ != kNil) pool_.at(tail_)->a_next = idx; else head_ = idx;
    tail_ = idx;
    ++pos_.open_orders;
  }
  void unlink_active(const OrderRec& r) noexcept {
    if (r.a_prev != kNil) pool_.at(r.a_prev)->a_next = r.a_next; else head_ = r.a_next;
    if (r.a_next != kNil) pool_.at(r.a_next)->a_prev = r.a_prev; else tail_ = r.a_prev;
    --pos_.open_orders;
  }

  void set_reserved(OrderRec& r, Qty v) noexcept {
    (r.side == Side::Buy ? pos_.open_buy : pos_.open_sell) += v - r.reserved;
    r.reserved = v;
  }

  void finish(std::uint32_t idx, OrderRec& r, State terminal) noexcept {
    unlink_active(r);
    set_reserved(r, 0);
    r.state = terminal;
    r.leaves = 0;
    ring_[(ring_head_ + ring_n_) % max_] = idx;
    ++ring_n_;
    r.a_prev = r.a_next = kNil;
  }

  bool evict_oldest_terminal() noexcept {
    if (ring_n_ == 0) return false;
    const std::uint32_t idx = ring_[ring_head_];
    ring_head_ = (ring_head_ + 1) % max_;
    --ring_n_;
    OrderRec* r = pool_.at(idx);
    map_.erase(r->id);
    pool_.release(r);
    ++stats_.evicted;
    return true;
  }

  void on_fill(std::uint32_t idx, OrderRec& r, const Report& rep) noexcept {
    const Qty q = rep.qty;
    if (q <= 0 || q > r.leaves) {
      ++stats_.overfills;
      return;
    }
    r.cum += q;
    r.leaves -= q;
    if (rep.leaves != r.leaves) ++stats_.mismatches;
    const std::int64_t sg = r.side == Side::Buy ? 1 : -1;
    pos_.pos += sg * q;
    pos_.cash -= static_cast<i128>(sg) * rep.price.raw() * q;
    (r.side == Side::Buy ? pos_.buy_qty : pos_.sell_qty) += static_cast<std::uint64_t>(q);
    ++pos_.fills;
    if (r.leaves == 0) {
      finish(idx, r, State::Filled);
      return;
    }
    if (r.state == State::New) r.state = State::PartiallyFilled;
    // Exposure follows the open quantity, but a pending replace keeps its worst case.
    set_reserved(r, r.state == State::PendingReplace ? (r.leaves + r.pend_qty) : r.leaves);
  }

  void on_cancel(std::uint32_t idx, OrderRec& r, const Report& rep) noexcept {
    // A cancel that leaves quantity open (self-trade decrement) shrinks the
    // order; one that leaves none ends it.
    if (rep.qty != r.leaves - rep.leaves) ++stats_.mismatches;
    if (rep.leaves < 0 || rep.leaves > r.leaves) {
      ++stats_.anomalies;
      return;
    }
    if (rep.leaves == 0) {
      r.leaves = 0;
      finish(idx, r, State::Canceled);
      return;
    }
    r.leaves = rep.leaves;
    r.qty = r.cum + r.leaves;
    set_reserved(r, r.state == State::PendingReplace ? (r.leaves + r.pend_qty) : r.leaves);
  }

  std::uint32_t max_{0};
  core::Pool<OrderRec> pool_;
  book::IdMap map_;
  std::uint32_t* ring_{nullptr};  // finished orders, oldest first
  std::uint32_t ring_head_{0}, ring_n_{0};
  std::uint32_t head_{kNil}, tail_{kNil};
  Position pos_;
  OmsStats stats_;
};

}  // namespace hft::oms
