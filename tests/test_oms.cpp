// Phase 5 tests, part 1: the OMS state machine (every state x event cell), the
// position and exposure books, record eviction, and the pre-trade risk engine
// against a naive reference.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "check.hpp"
#include "event_gen.hpp"
#include "hft/core/memory.hpp"
#include "hft/oms/oms.hpp"
#include "hft/risk/risk.hpp"

using namespace hft;
using namespace hft::oms;
using hft::risk::KillReason;
using hft::risk::RiskConfig;
using hft::risk::RiskEngine;
using hft::risk::RiskResult;
using hft::sim::Liquidity;
using hft::sim::Reason;

namespace {

core::Region make_region(std::size_t mb) {
  return core::Region::create(mb << 20, core::RegionOptions{false, false, true, -1});
}

Price PX(std::int64_t raw) { return Price::from_raw(raw); }

Report mkrep(ReportType t, OrderId id, Qty qty, Qty leaves, std::int64_t px = 100, Reason r = Reason::None,
             std::uint64_t ts = 1) {
  Report x{};
  x.type = t;
  x.reason = r;
  x.liq = Liquidity::None;
  x.side = Side::Buy;
  x.owner = 1;
  x.id = id;
  x.counter_id = 0;
  x.price = PX(px);
  x.qty = qty;
  x.leaves = leaves;
  x.seq = 0;
  x.ts = ts;
  return x;
}
Report ack(OrderId id, Qty q) { return mkrep(ReportType::Accepted, id, q, q); }

struct Rig {
  core::Region region = make_region(4);
  core::Arena arena{region};
  Oms oms;
  explicit Rig(std::uint32_t max_orders = 64) {
    OmsConfig c;
    c.max_orders = max_orders;
    CHECK(oms.init(arena, c));
  }
};

// ---- the transition matrix -----------------------------------------------------

enum Col { Ack_, Reject_, FillPart, FillFull, CancelAll, CancelPart, Replaced_, CancelRej, ReplaceRej, ReqCancel, ReqReplace, NCols };
const char* kColNames[NCols] = {"Ack",      "Reject",    "FillPart",   "FillFull",   "CancelAll", "CancelPart",
                                "Replaced", "CancelRej", "ReplaceRej", "ReqCancel", "ReqReplace"};

enum Row { PN, NEW, PF, PC, PC_PF, PR, PR_PF, FILLED, CANCELED, REJECTED, NRows };
const char* kRowNames[NRows] = {"PendingNew", "New", "PartFilled", "PendCancel", "PendCancel(pf)",
                                "PendReplace", "PendReplace(pf)", "Filled", "Canceled", "Rejected"};

// Expected outcome per cell, written out by hand from the rules in oms.hpp:
//   "X" anomaly, "I" ignore, "R" refuse, otherwise the state the order ends in.
const char* kSpec[NRows][NCols] = {
    //                Ack       Reject      FillPart   FillFull   CancelAll  CancelPart  Replaced   CancelRej  ReplaceRej ReqCancel  ReqReplace
    /* PN        */ {"New",     "Rejected", "X",       "X",       "X",       "X",        "X",       "X",       "X",       "R",       "R"},
    /* NEW       */ {"X",       "X",        "PartF",   "Filled",  "Canceled","New",      "X",       "X",       "X",       "PendC",   "PendR"},
    /* PF        */ {"X",       "X",        "PartF",   "Filled",  "Canceled","PartF",    "X",       "X",       "X",       "PendC",   "PendR"},
    /* PC        */ {"X",       "X",        "PendC",   "Filled",  "Canceled","PendC",    "X",       "New",     "X",       "R",       "R"},
    /* PC(pf)    */ {"X",       "X",        "PendC",   "Filled",  "Canceled","PendC",    "X",       "PartF",   "X",       "R",       "R"},
    /* PR        */ {"X",       "X",        "PendR",   "Filled",  "Canceled","PendR",    "New",     "X",       "New",     "R",       "R"},
    /* PR(pf)    */ {"X",       "X",        "PendR",   "Filled",  "Canceled","PendR",    "PartF",   "X",       "PartF",   "R",       "R"},
    /* Filled    */ {"X",       "X",        "X",       "X",       "X",       "X",        "X",       "I",       "I",       "R",       "R"},
    /* Canceled  */ {"X",       "X",        "X",       "X",       "X",       "X",        "X",       "I",       "I",       "R",       "R"},
    /* Rejected  */ {"X",       "X",        "X",       "X",       "X",       "X",        "X",       "I",       "I",       "R",       "R"},
};

State state_of(const char* s) {
  if (!std::strcmp(s, "New")) return State::New;
  if (!std::strcmp(s, "PartF")) return State::PartiallyFilled;
  if (!std::strcmp(s, "PendC")) return State::PendingCancel;
  if (!std::strcmp(s, "PendR")) return State::PendingReplace;
  if (!std::strcmp(s, "Filled")) return State::Filled;
  if (!std::strcmp(s, "Canceled")) return State::Canceled;
  return State::Rejected;
}

constexpr OrderId kId = 7;

// Puts a fresh order (buy 10 @100) into the state for `row`.
void setup(Oms& o, Row row) {
  CHECK(o.on_send_new(kId, Side::Buy, OrderType::Limit, PX(100), 10, 1) == Submit::Ok);
  const bool pf = row == PF || row == PC_PF || row == PR_PF;
  if (row == REJECTED) {
    o.on_report(mkrep(ReportType::Rejected, kId, 10, 0));
    return;
  }
  if (row == PN) return;
  o.on_report(ack(kId, 10));
  if (pf) o.on_report(mkrep(ReportType::Fill, kId, 3, 7));
  if (row == PC || row == PC_PF) CHECK(o.on_request_cancel(kId, 2) == Request::Ok);
  if (row == PR || row == PR_PF) CHECK(o.on_request_replace(kId, PX(101), 12, 2) == Request::Ok);
  if (row == FILLED) o.on_report(mkrep(ReportType::Fill, kId, 10, 0));
  if (row == CANCELED) o.on_report(mkrep(ReportType::Canceled, kId, 10, 0, 100, Reason::UserRequested));
}

void test_transition_matrix() {
  int cells = 0, anomalies = 0, ignores = 0, refusals = 0, applies = 0;
  for (int r = 0; r < NRows; ++r) {
    for (int c = 0; c < NCols; ++c) {
      Rig rig;
      Oms& o = rig.oms;
      setup(o, static_cast<Row>(r));
      CHECK(o.check_invariants());
      const OrderRec before = *o.find(kId);
      const Position pos_before = o.position();
      const OmsStats st = o.stats();
      const Qty L = before.leaves;

      Request req = Request::Ok;
      bool is_request = false;
      switch (c) {
        case Ack_: o.on_report(ack(kId, 10)); break;
        case Reject_: o.on_report(mkrep(ReportType::Rejected, kId, 10, 0)); break;
        case FillPart: o.on_report(mkrep(ReportType::Fill, kId, 1, L - 1)); break;
        case FillFull: o.on_report(mkrep(ReportType::Fill, kId, L, 0)); break;
        case CancelAll: o.on_report(mkrep(ReportType::Canceled, kId, L, 0, 100, Reason::UserRequested)); break;
        case CancelPart: o.on_report(mkrep(ReportType::Canceled, kId, 1, L - 1, 100, Reason::StpDecrement)); break;
        case Replaced_: o.on_report(mkrep(ReportType::Replaced, kId, 12, 12, 101)); break;
        case CancelRej: o.on_report(mkrep(ReportType::CancelRejected, kId, 0, 0)); break;
        case ReplaceRej: o.on_report(mkrep(ReportType::ReplaceRejected, kId, 0, 0)); break;
        case ReqCancel: req = o.on_request_cancel(kId, 5); is_request = true; break;
        case ReqReplace: req = o.on_request_replace(kId, PX(99), 5, 5); is_request = true; break;
      }
      const OrderRec* after = o.find(kId);
      CHECK(after != nullptr);  // finished orders stay findable
      CHECK(o.check_invariants());
      const char* want = kSpec[r][c];
      const OmsStats& st2 = o.stats();
      ++cells;
      const bool bad = !after;
      if (bad) continue;
      const bool ok_name = true;
      (void)ok_name;
      if (!std::strcmp(want, "X")) {
        ++anomalies;
        CHECK(st2.anomalies == st.anomalies + 1);
        CHECK(after->state == before.state && after->leaves == before.leaves && after->cum == before.cum &&
              after->reserved == before.reserved);
        CHECK(o.position().pos == pos_before.pos && o.position().cash == pos_before.cash);
      } else if (!std::strcmp(want, "I")) {
        ++ignores;
        CHECK(st2.ignored == st.ignored + 1 && st2.anomalies == st.anomalies);
        CHECK(after->state == before.state && after->leaves == before.leaves);
      } else if (!std::strcmp(want, "R")) {
        ++refusals;
        CHECK(is_request && req == Request::NotAllowed && st2.refused == st.refused + 1);
        CHECK(after->state == before.state && after->leaves == before.leaves && after->reserved == before.reserved);
      } else {
        ++applies;
        CHECK(st2.anomalies == st.anomalies && st2.ignored == st.ignored && st2.refused == st.refused);
        CHECK(!is_request || req == Request::Ok);
        const State ws = state_of(want);
        if (after->state != ws) {
          std::fprintf(stderr, "cell (%s, %s): expected state %s, got %d\n", kRowNames[r], kColNames[c], want,
                       static_cast<int>(after->state));
        }
        CHECK(after->state == ws);
      }
      // Kind-of-action check against the library's own table as well.
      const State s0 = before.state;
      const Event e = static_cast<Event>(c == Ack_ ? 0 : c == Reject_ ? 1 : (c == FillPart || c == FillFull) ? 2
                                         : (c == CancelAll || c == CancelPart) ? 3 : c == Replaced_ ? 4
                                         : c == CancelRej ? 5 : c == ReplaceRej ? 6 : c == ReqCancel ? 7 : 8);
      const Action a = kAction[static_cast<int>(s0)][static_cast<int>(e)];
      const bool want_apply = std::strcmp(want, "X") && std::strcmp(want, "I") && std::strcmp(want, "R");
      CHECK((a == Action::Apply) == want_apply);
      CHECK((a == Action::Anomaly) == !std::strcmp(want, "X"));
      CHECK((a == Action::Ignore) == !std::strcmp(want, "I"));
      CHECK((a == Action::Refuse) == !std::strcmp(want, "R"));
    }
  }
  CHECK(cells == static_cast<int>(NRows) * static_cast<int>(NCols));
  std::printf("  transition matrix: %d cells (%d apply, %d anomaly, %d ignore, %d refuse)\n", cells, applies,
              anomalies, ignores, refusals);
}

// Results of the applied cells beyond the state name.
// Found by fuzz_oms: a Replaced report carrying leaves <= 0 used to be applied and broke the books.
void test_replaced_nonsense() {
  for (const std::int64_t bad_leaves : {0, -7}) {
    Rig r;
    setup(r.oms, NEW);
    CHECK(r.oms.on_request_replace(kId, PX(100), 4, 3) == Request::Ok);
    r.oms.on_report(mkrep(ReportType::Replaced, kId, 4, bad_leaves, 100));
    const OrderRec* o = r.oms.find(kId);
    CHECK(o->state == State::New && o->leaves == 10 && o->reserved == 10 && r.oms.stats().anomalies == 1);
    CHECK(r.oms.check_invariants());
  }
}

void test_ack_mismatch() {
  Rig r;
  CHECK(r.oms.on_send_new(kId, Side::Buy, OrderType::Limit, PX(100), 10, 1) == Submit::Ok);
  r.oms.on_report(ack(kId, 9));  // the exchange accepted a different size than we sent
  CHECK(r.oms.stats().mismatches == 1 && r.oms.find(kId)->state == State::New);
  Rig q;
  CHECK(q.oms.on_send_new(kId, Side::Buy, OrderType::Limit, PX(100), 10, 1) == Submit::Ok);
  q.oms.on_report(ack(kId, 10));
  CHECK(q.oms.stats().mismatches == 0);
}

void test_transition_effects() {
  {  // PendingReplace + Replaced: new price and open quantity, reservation follows
    Rig r;
    setup(r.oms, PR_PF);  // leaves 7, cum 3, pending qty 12 -> worst-case reservation 7 + 12
    CHECK(r.oms.find(kId)->reserved == 19 && r.oms.position().open_buy == 19);
    r.oms.on_report(mkrep(ReportType::Replaced, kId, 12, 12, 101));
    const OrderRec* o = r.oms.find(kId);
    CHECK(o->price == PX(101) && o->leaves == 12 && o->cum == 3 && o->qty == 15 && o->reserved == 12);
    CHECK(r.oms.position().open_buy == 12 && r.oms.check_invariants());
  }
  {  // a replace reserves the old size plus the new one until the exchange answers
    Rig r;
    setup(r.oms, NEW);
    CHECK(r.oms.on_request_replace(kId, PX(100), 4, 3) == Request::Ok);
    CHECK(r.oms.find(kId)->reserved == 14 && r.oms.position().open_buy == 14);
    r.oms.on_report(mkrep(ReportType::Replaced, kId, 4, 4, 100));
    CHECK(r.oms.find(kId)->reserved == 4 && r.oms.position().open_buy == 4);
    CHECK(r.oms.check_invariants());
  }
  {  // replace reject restores the reservation
    Rig r;
    setup(r.oms, NEW);
    CHECK(r.oms.on_request_replace(kId, PX(100), 30, 3) == Request::Ok);
    CHECK(r.oms.position().open_buy == 40);
    r.oms.on_report(mkrep(ReportType::ReplaceRejected, kId, 0, 0));
    CHECK(r.oms.position().open_buy == 10 && r.oms.find(kId)->state == State::New);
  }
  {  // partial cancel (self-trade decrement) shrinks the order and its reservation
    Rig r;
    setup(r.oms, NEW);
    r.oms.on_report(mkrep(ReportType::Canceled, kId, 4, 6, 100, Reason::StpDecrement));
    const OrderRec* o = r.oms.find(kId);
    CHECK(o->state == State::New && o->leaves == 6 && o->qty == 6 && o->reserved == 6);
    CHECK(r.oms.stats().mismatches == 0 && r.oms.check_invariants());
  }
  {  // fills during a pending replace shrink the old-size part, so fills never add exposure
    Rig r;
    setup(r.oms, NEW);
    CHECK(r.oms.on_request_replace(kId, PX(100), 20, 3) == Request::Ok);
    r.oms.on_report(mkrep(ReportType::Fill, kId, 2, 8));
    CHECK(r.oms.find(kId)->reserved == 28 && r.oms.position().open_buy == 28);
    r.oms.on_report(mkrep(ReportType::Fill, kId, 8, 0));  // fully filled before the exchange saw the replace
    CHECK(r.oms.find(kId)->state == State::Filled && r.oms.position().open_buy == 0);
    r.oms.on_report(mkrep(ReportType::ReplaceRejected, kId, 0, 0));  // the late reject
    CHECK(r.oms.stats().ignored == 1 && r.oms.stats().anomalies == 0 && r.oms.check_invariants());
  }
  {  // market orders cannot be replaced
    Rig r;
    CHECK(r.oms.on_send_new(1, Side::Sell, OrderType::Market, PX(0), 5, 1) == Submit::Ok);
    r.oms.on_report(ack(1, 5));
    CHECK(r.oms.on_request_replace(1, PX(100), 5, 2) == Request::BadParams);
    CHECK(r.oms.on_request_replace(1, PX(100), 0, 2) == Request::BadParams);
    CHECK(r.oms.on_request_replace(99, PX(100), 5, 2) == Request::Unknown);
    CHECK(r.oms.on_request_cancel(99, 2) == Request::Unknown);
  }
}

void test_position_and_exposure() {
  Rig r;
  Oms& o = r.oms;
  CHECK(o.on_send_new(1, Side::Buy, OrderType::Limit, PX(100), 10, 1) == Submit::Ok);
  CHECK(o.on_send_new(2, Side::Sell, OrderType::Limit, PX(110), 6, 1) == Submit::Ok);
  CHECK(o.position().open_buy == 10 && o.position().open_sell == 6 && o.position().open_orders == 2);
  o.on_report(ack(1, 10));
  o.on_report(ack(2, 6));
  o.on_report(mkrep(ReportType::Fill, 1, 4, 6, 100));
  o.on_report(mkrep(ReportType::Fill, 1, 6, 0, 101));
  CHECK(o.position().pos == 10 && o.position().cash == -(400 + 606) && o.position().buy_qty == 10);
  CHECK(o.position().open_buy == 0 && o.position().open_orders == 1);
  o.on_report(mkrep(ReportType::Fill, 2, 6, 0, 110));
  CHECK(o.position().pos == 4 && o.position().cash == -1006 + 660 && o.position().sell_qty == 6);
  CHECK(o.position().fills == 3 && o.position().open_sell == 0 && o.position().open_orders == 0);
  CHECK(o.position().mark_pnl(PX(105)) == -346 + 4 * 105);
  CHECK(o.check_invariants());
  // Rejected and cancelled orders release their exposure.
  CHECK(o.on_send_new(3, Side::Buy, OrderType::Limit, PX(100), 9, 2) == Submit::Ok);
  o.on_report(mkrep(ReportType::Rejected, 3, 9, 0));
  CHECK(o.position().open_buy == 0);
  CHECK(o.on_send_new(4, Side::Buy, OrderType::Limit, PX(100), 9, 2) == Submit::Ok);
  o.on_report(ack(4, 9));
  CHECK(o.on_request_cancel(4, 3) == Request::Ok && o.position().open_buy == 9);  // still counted until confirmed
  o.on_report(mkrep(ReportType::Canceled, 4, 9, 0, 100, Reason::UserRequested));
  CHECK(o.position().open_buy == 0 && o.check_invariants());
}

void test_bad_reports_and_validation() {
  Rig r;
  Oms& o = r.oms;
  CHECK(o.on_send_new(0, Side::Buy, OrderType::Limit, PX(100), 1, 1) == Submit::BadParams);
  CHECK(o.on_send_new(1, Side::Buy, OrderType::Limit, PX(100), 0, 1) == Submit::BadParams);
  CHECK(o.on_send_new(1, Side::Buy, OrderType::Limit, PX(100), 5, 1) == Submit::Ok);
  CHECK(o.on_send_new(1, Side::Buy, OrderType::Limit, PX(100), 5, 1) == Submit::Duplicate);
  o.on_report(ack(1, 5));
  o.on_report(mkrep(ReportType::Fill, 1, 6, 0));  // more than is open
  CHECK(o.stats().overfills == 1 && o.find(1)->leaves == 5 && o.position().pos == 0);
  o.on_report(mkrep(ReportType::Fill, 1, 0, 5));  // zero quantity
  CHECK(o.stats().overfills == 2);
  o.on_report(mkrep(ReportType::Fill, 1, 2, 4));  // exchange claims leaves 4, we compute 3
  CHECK(o.stats().mismatches == 1 && o.find(1)->leaves == 3);
  o.on_report(mkrep(ReportType::Canceled, 1, 1, 1, 100, Reason::StpDecrement));  // removes 2, claims 1
  CHECK(o.stats().mismatches == 2);
  o.on_report(mkrep(ReportType::Fill, 77, 1, 0));  // we never heard of 77
  CHECK(o.stats().unknown_reports == 1 && o.stats().orphan_fills == 1);
  o.on_report(mkrep(ReportType::CancelRejected, 78, 0, 0));
  CHECK(o.stats().unknown_reports == 2 && o.stats().orphan_fills == 1);
  Report bogus = mkrep(ReportType::Accepted, 1, 1, 1);
  bogus.type = static_cast<ReportType>(99);
  o.on_report(bogus);
  CHECK(o.stats().anomalies == 1);
  CHECK(o.check_invariants());
}

void test_eviction() {
  Rig r(4);
  Oms& o = r.oms;
  for (OrderId i = 1; i <= 4; ++i) {
    CHECK(o.on_send_new(i, Side::Buy, OrderType::Limit, PX(100), 1, i) == Submit::Ok);
  }
  CHECK(o.on_send_new(5, Side::Buy, OrderType::Limit, PX(100), 1, 9) == Submit::Full);  // all four are live
  o.on_report(ack(1, 1));
  o.on_report(mkrep(ReportType::Fill, 1, 1, 0));  // order 1 finished
  o.on_report(mkrep(ReportType::Rejected, 2, 1, 0));
  CHECK(o.find(1) && o.find(2));  // finished orders are still there...
  CHECK(o.on_send_new(5, Side::Buy, OrderType::Limit, PX(100), 1, 9) == Submit::Ok);  // ...until the space is needed
  CHECK(o.find(1) == nullptr && o.find(2) != nullptr && o.stats().evicted == 1);  // oldest finished goes first
  CHECK(o.on_send_new(6, Side::Buy, OrderType::Limit, PX(100), 1, 9) == Submit::Ok);
  CHECK(o.find(2) == nullptr && o.stats().evicted == 2);
  CHECK(o.on_send_new(7, Side::Buy, OrderType::Limit, PX(100), 1, 9) == Submit::Full);
  o.on_report(mkrep(ReportType::CancelRejected, 1, 0, 0));  // late event for an evicted order
  CHECK(o.stats().unknown_reports == 1 && o.check_invariants());
  // Many cycles through a tiny pool keep every invariant.
  testsupport::Rng rng(5);
  OrderId next = 100;
  for (int i = 0; i < 20000; ++i) {
    const OrderId id = next++;
    if (o.on_send_new(id, Side::Sell, OrderType::Limit, PX(100), 3, 1) != Submit::Ok) {
      // free a slot by finishing a live order
      OrderId victim = 0;
      o.for_each_open([&](const OrderRec& x) { if (!victim) victim = x.id; });
      CHECK(victim != 0);
      const OrderRec* v = o.find(victim);
      if (v->state == State::PendingNew) o.on_report(ack(victim, v->qty));
      o.on_report(mkrep(ReportType::Canceled, victim, o.find(victim)->leaves, 0, 100, Reason::UserRequested));
      continue;
    }
    if (rng.below(2)) o.on_report(ack(id, 3));
    if (i % 997 == 0) CHECK(o.check_invariants());
  }
  CHECK(o.check_invariants());
}

// ---- risk -----------------------------------------------------------------------

RiskConfig base_cfg() {
  RiskConfig c;
  c.max_order_qty = 100;
  c.max_order_notional_raw = 6'000'000;
  c.price_band_bps = 500;
  c.max_position = 200;
  c.max_open_orders = 3;
  c.rate_per_sec = 1000;
  c.burst = 5;
  c.max_loss_raw = 50'000;
  return c;
}

constexpr std::int64_t kRef = 100'000;

RiskEngine mk_risk(const RiskConfig& c, bool with_ref = true) {
  RiskEngine r;
  r.init(c);
  if (with_ref) r.set_reference(PX(kRef));
  return r;
}

RiskConfig no_rate_limit(RiskConfig c) {
  c.rate_per_sec = 1'000'000'000;
  c.burst = 1000;
  return c;
}

void test_risk_limits() {
  const Position flat{};
  {  // happy path
    RiskEngine r = mk_risk(base_cfg());
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 10, flat, 0) == RiskResult::Ok);
  }
  {  // malformed
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 0, flat, 0) == RiskResult::BadParams);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), -5, flat, 0) == RiskResult::BadParams);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(0), 5, flat, 0) == RiskResult::BadParams);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(-1), 5, flat, 0) == RiskResult::BadParams);
  }
  {  // order quantity
    RiskConfig c = no_rate_limit(base_cfg());
    c.max_order_notional_raw = 1'000'000'000'000LL;  // keep the notional limit out of the way
    RiskEngine r = mk_risk(c);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 100, flat, 0) == RiskResult::Ok);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 101, flat, 0) == RiskResult::MaxOrderQty);
  }
  {  // reference price
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()), false);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 0) == RiskResult::NoReference);
    CHECK(r.check_new(Side::Buy, OrderType::Market, PX(0), 1, flat, 0) == RiskResult::NoReference);
    RiskConfig c = no_rate_limit(base_cfg());
    c.price_band_bps = 0;  // band off: limit orders no longer need a reference, market orders still do
    RiskEngine r2 = mk_risk(c, false);
    CHECK(r2.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 0) == RiskResult::Ok);
    CHECK(r2.check_new(Side::Buy, OrderType::Market, PX(0), 1, flat, 0) == RiskResult::NoReference);
  }
  {  // notional: limit orders at their own price, market orders at the reference
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 60, flat, 0) == RiskResult::Ok);        // 6.0M
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 61, flat, 0) == RiskResult::MaxNotional);
    CHECK(r.check_new(Side::Buy, OrderType::Market, PX(0), 60, flat, 0) == RiskResult::Ok);
    CHECK(r.check_new(Side::Sell, OrderType::Market, PX(0), 61, flat, 0) == RiskResult::MaxNotional);
    // 128-bit math: a huge price must fail the limit, not wrap around it
    RiskConfig c = no_rate_limit(base_cfg());
    c.price_band_bps = 0;
    c.max_order_qty = 1LL << 40;
    c.max_order_notional_raw = 1'000'000;
    RiskEngine big = mk_risk(c);
    CHECK(big.check_new(Side::Buy, OrderType::Limit, PX(1LL << 62), 1LL << 40, flat, 0) == RiskResult::MaxNotional);
  }
  {  // price band: exactly at the edge passes
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(105'000), 1, flat, 0) == RiskResult::Ok);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(105'001), 1, flat, 0) == RiskResult::PriceBand);
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(95'000), 1, flat, 0) == RiskResult::Ok);
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(94'999), 1, flat, 0) == RiskResult::PriceBand);
  }
  {  // position, counting working orders on the same side only
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
    Position p;
    p.pos = 150;
    p.open_buy = 40;
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 10, p, 0) == RiskResult::Ok);  // 150+40+10 = 200
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 11, p, 0) == RiskResult::MaxPosition);
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(kRef), 11, p, 0) == RiskResult::Ok);  // reduces
    p.pos = -150;
    p.cash = static_cast<core::i128>(150) * kRef;  // flat PnL at the reference, so the loss limit stays out of it
    p.open_buy = 0;
    p.open_sell = 40;
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(kRef), 10, p, 0) == RiskResult::Ok);
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(kRef), 11, p, 0) == RiskResult::MaxPosition);
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 11, p, 0) == RiskResult::Ok);
    p.pos = 200;  // already at the limit: only reducing is allowed
    p.cash = -static_cast<core::i128>(200) * kRef;
    p.open_sell = 0;
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::MaxPosition);
    CHECK(r.check_new(Side::Sell, OrderType::Limit, PX(kRef), 50, p, 0) == RiskResult::Ok);
  }
  {  // open orders
    RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
    Position p;
    p.open_orders = 2;
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::Ok);
    p.open_orders = 3;
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::MaxOpenOrders);
  }
}

void test_risk_rate_limit() {
  RiskConfig c = base_cfg();  // 1000/s = one token per millisecond, burst 5
  const Position flat{};
  RiskEngine r = mk_risk(c);
  for (int i = 0; i < 5; ++i) CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 1000) == RiskResult::Ok);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 1000) == RiskResult::RateLimit);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 1000 + 999'999) == RiskResult::RateLimit);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 1000 + 1'000'000) == RiskResult::Ok);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 1000 + 1'000'000) == RiskResult::RateLimit);
  // a clock that steps backwards neither refunds nor refills
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 10) == RiskResult::RateLimit);
  // a long pause refills to the burst size and no further
  for (int i = 0; i < 5; ++i)
    CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 10'000'000'000ULL) == RiskResult::Ok);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 10'000'000'000ULL) == RiskResult::RateLimit);
  // a huge gap must not overflow the refill arithmetic
  RiskEngine big = mk_risk(c);
  CHECK(big.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 5) == RiskResult::Ok);
  CHECK(big.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, ~0ULL - 5) == RiskResult::Ok);
  // refused orders do not spend tokens
  RiskEngine rr = mk_risk(c);
  for (int i = 0; i < 1000; ++i) CHECK(rr.check_new(Side::Buy, OrderType::Limit, PX(kRef), 0, flat, 7) == RiskResult::BadParams);
  for (int i = 0; i < 5; ++i) CHECK(rr.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 7) == RiskResult::Ok);
}

void test_risk_kill_and_loss() {
  const Position flat{};
  RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
  CHECK(!r.killed());
  r.kill(KillReason::Manual);
  r.kill(KillReason::Disconnect);  // the first cause is kept
  CHECK(r.killed() && r.kill_reason() == KillReason::Manual);
  CHECK(r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 0) == RiskResult::KillSwitch);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 1, 1, flat, 0) == RiskResult::KillSwitch);
  r.reset_kill();
  CHECK(!r.killed() && r.check_new(Side::Buy, OrderType::Limit, PX(kRef), 1, flat, 0) == RiskResult::Ok);

  // Loss limit: marked at the reference. Long 100 bought at 100'500 loses 50'000 at 100'000.
  RiskEngine l = mk_risk(no_rate_limit(base_cfg()));
  Position p;
  p.pos = 100;
  p.cash = -static_cast<core::i128>(100) * 100'500;
  CHECK(p.mark_pnl(PX(kRef)) == -50'000);
  CHECK(l.check_new(Side::Sell, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::Ok);  // exactly at the limit
  CHECK(!l.killed());
  p.cash -= 1;
  CHECK(l.check_new(Side::Sell, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::MaxLoss);
  CHECK(l.killed() && l.kill_reason() == KillReason::MaxLoss);
  CHECK(l.check_new(Side::Sell, OrderType::Limit, PX(kRef), 1, flat, 0) == RiskResult::KillSwitch);  // latched
  RiskConfig off = no_rate_limit(base_cfg());
  off.max_loss_raw = 0;
  RiskEngine d = mk_risk(off);
  CHECK(d.check_new(Side::Sell, OrderType::Limit, PX(kRef), 1, p, 0) == RiskResult::Ok);  // disabled
}

void test_risk_replace() {
  RiskEngine r = mk_risk(no_rate_limit(base_cfg()));
  Position p;
  p.pos = 140;
  p.open_buy = 10;  // includes the order being replaced (leaves 10); fills can race, so the new size needs full headroom
  CHECK(r.check_replace(Side::Buy, PX(kRef), 10, 10, p, 0) == RiskResult::Ok);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 50, 10, p, 0) == RiskResult::Ok);        // 140 + 10 + 50 = 200
  CHECK(r.check_replace(Side::Buy, PX(kRef), 51, 10, p, 0) == RiskResult::MaxPosition);  // +1 over the limit
  p.pos = 190;
  CHECK(r.check_replace(Side::Buy, PX(kRef), 4, 10, p, 0) == RiskResult::MaxPosition);  // even a shrink needs headroom now
  p.pos = 150;
  CHECK(r.check_replace(Side::Buy, PX(kRef), 40, 10, p, 0) == RiskResult::Ok);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 41, 10, p, 0) == RiskResult::MaxPosition);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 0, 10, p, 0) == RiskResult::BadParams);
  CHECK(r.check_replace(Side::Buy, PX(0), 5, 10, p, 0) == RiskResult::BadParams);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 101, 10, p, 0) == RiskResult::MaxOrderQty);
  CHECK(r.check_replace(Side::Buy, PX(106'000), 5, 10, p, 0) == RiskResult::PriceBand);
  CHECK(r.check_replace(Side::Buy, PX(kRef), 61, 10, Position{}, 0) == RiskResult::MaxNotional);
  RiskEngine nr = mk_risk(no_rate_limit(base_cfg()), false);
  CHECK(nr.check_replace(Side::Buy, PX(kRef), 5, 10, Position{}, 0) == RiskResult::NoReference);
}

// Naive restatement of the documented rules, with an independent rate limiter
// (a GCRA / leaky bucket in whole nanoseconds, exact when 1e9 % rate == 0).
struct RefRisk {
  RiskConfig c;
  Price ref = Price::invalid();
  bool killed = false;
  bool started = false;
  std::uint64_t last = 0;
  std::int64_t tat = 0;  // theoretical arrival time

  bool rate_ok(std::uint64_t now) {
    const std::int64_t T = static_cast<std::int64_t>(1'000'000'000ULL / c.rate_per_sec);
    const std::int64_t tau = (static_cast<std::int64_t>(c.burst) - 1) * T;
    if (!started) {
      started = true;
      tat = static_cast<std::int64_t>(now);
      last = now;
    }
    if (now > last) last = now;  // time never runs backwards
    const std::int64_t t = static_cast<std::int64_t>(last);
    const std::int64_t base = tat > t ? tat : t;
    if (base - t > tau) return false;
    tat = base + T;
    return true;
  }

  bool loss_ok(const Position& p) {
    if (c.max_loss_raw <= 0 || ref == Price::invalid()) return true;
    const core::i128 pnl = p.cash + static_cast<core::i128>(p.pos) * ref.raw();
    if (pnl < -static_cast<core::i128>(c.max_loss_raw)) {
      killed = true;
      return false;
    }
    return true;
  }
  static bool band(Price px, Price ref, std::int64_t bps) {
    const core::i128 d = static_cast<core::i128>(px.raw()) - ref.raw();
    const core::i128 ad = d < 0 ? -d : d;
    return ad * 10000 <= static_cast<core::i128>(ref.raw()) * bps;
  }
  bool pos_ok(Side s, const Position& p, Qty add) const {
    if (s == Side::Buy) return p.pos + p.open_buy + add <= c.max_position;
    return p.pos - p.open_sell - add >= -c.max_position;
  }

  RiskResult new_order(Side s, OrderType t, Price px, Qty q, const Position& p, std::uint64_t now) {
    if (killed) return RiskResult::KillSwitch;
    if (!loss_ok(p)) return RiskResult::MaxLoss;
    const bool priced = t != OrderType::Market;
    if (q <= 0 || (priced && px.raw() <= 0)) return RiskResult::BadParams;
    if (q > c.max_order_qty) return RiskResult::MaxOrderQty;
    const bool need_ref = !priced || c.price_band_bps > 0;
    if (need_ref && ref == Price::invalid()) return RiskResult::NoReference;
    const Price vp = priced ? px : ref;
    if (static_cast<core::i128>(vp.raw()) * q > c.max_order_notional_raw) return RiskResult::MaxNotional;
    if (priced && c.price_band_bps > 0 && !band(px, ref, c.price_band_bps)) return RiskResult::PriceBand;
    if (!pos_ok(s, p, q)) return RiskResult::MaxPosition;
    if (p.open_orders >= c.max_open_orders) return RiskResult::MaxOpenOrders;
    return rate_ok(now) ? RiskResult::Ok : RiskResult::RateLimit;
  }

  RiskResult replace(Side s, Price px, Qty q, Qty leaves, const Position& p, std::uint64_t now) {
    if (killed) return RiskResult::KillSwitch;
    if (!loss_ok(p)) return RiskResult::MaxLoss;
    if (q <= 0 || px.raw() <= 0) return RiskResult::BadParams;
    if (q > c.max_order_qty) return RiskResult::MaxOrderQty;
    if (c.price_band_bps > 0 && ref == Price::invalid()) return RiskResult::NoReference;
    if (static_cast<core::i128>(px.raw()) * q > c.max_order_notional_raw) return RiskResult::MaxNotional;
    if (c.price_band_bps > 0 && !band(px, ref, c.price_band_bps)) return RiskResult::PriceBand;
    (void)leaves;
    if (!pos_ok(s, p, q)) return RiskResult::MaxPosition;
    return rate_ok(now) ? RiskResult::Ok : RiskResult::RateLimit;
  }
};

void test_risk_differential(std::uint64_t n) {
  testsupport::Rng rng(31337);
  std::uint64_t seen[11] = {};
  const std::uint64_t rates[] = {1000, 100'000, 1'000'000, 10'000'000};
  for (std::uint64_t round = 0; round < 200; ++round) {
    RiskConfig c;
    c.max_order_qty = 1 + static_cast<Qty>(rng.below(200));
    c.max_order_notional_raw = 1 + static_cast<std::int64_t>(rng.below(50)) * 1'000'000;
    c.price_band_bps = rng.below(4) == 0 ? 0 : 1 + rng.below(1000);
    c.max_position = 1 + static_cast<Qty>(rng.below(500));
    c.max_open_orders = 1 + rng.below(10);
    c.rate_per_sec = rates[rng.below(4)];
    c.burst = 1 + rng.below(10);
    c.max_loss_raw = rng.below(3) == 0 ? 0 : 1 + static_cast<std::int64_t>(rng.below(100'000));
    RiskEngine r;
    r.init(c);
    RefRisk ref;
    ref.c = c;
    if (rng.below(8) != 0) {
      r.set_reference(PX(kRef));
      ref.ref = PX(kRef);
    }
    std::uint64_t now = rng.below(1000);
    for (std::uint64_t i = 0; i < n / 200; ++i) {
      now = rng.below(20) == 0 && now > 500 ? now - rng.below(500) : now + rng.below(3) * rng.below(2'000'000);
      Position p;
      p.pos = static_cast<std::int64_t>(rng.below(1001)) - 500;
      p.open_buy = rng.below(100);
      p.open_sell = rng.below(100);
      p.open_orders = rng.below(12);
      p.cash = static_cast<core::i128>(rng.below(2'000'000)) * 1000 - 1'000'000'000LL;
      if (rng.below(3) == 0) p.cash = -static_cast<core::i128>(p.pos) * kRef - static_cast<core::i128>(rng.below(120'000));
      const Side s = rng.below(2) ? Side::Buy : Side::Sell;
      const Price px = PX(static_cast<std::int64_t>(rng.below(4)) == 0
                              ? static_cast<std::int64_t>(rng.below(130'000)) - 1000
                              : 95'000 + static_cast<std::int64_t>(rng.below(10'001)));
      const Qty q = static_cast<Qty>(rng.below(260)) - 10;
      RiskResult a, b;
      if (rng.below(3) == 0) {
        const Qty leaves = 1 + static_cast<Qty>(rng.below(100));
        a = r.check_replace(s, px, q, leaves, p, now);
        b = ref.replace(s, px, q, leaves, p, now);
      } else {
        const OrderType t = static_cast<OrderType>(rng.below(5));
        a = r.check_new(s, t, px, q, p, now);
        b = ref.new_order(s, t, px, q, p, now);
      }
      ++seen[static_cast<int>(a)];
      if (a != b) {
        std::fprintf(stderr, "risk differential: round %llu step %llu engine=%d reference=%d\n",
                     static_cast<unsigned long long>(round), static_cast<unsigned long long>(i), static_cast<int>(a),
                     static_cast<int>(b));
        CHECK(false);
        return;
      }
      if (rng.below(500) == 0) {
        r.kill(KillReason::Manual);
        ref.killed = true;
      }
      if (rng.below(300) == 0) {
        r.reset_kill();
        ref.killed = false;
      }
    }
  }
  for (int k = 0; k <= static_cast<int>(RiskResult::RateLimit); ++k) CHECK(seen[k] > 0);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 400000ULL;
  test_transition_matrix();
  test_transition_effects();
  test_replaced_nonsense();
  test_ack_mismatch();
  test_position_and_exposure();
  test_bad_reports_and_validation();
  test_eviction();
  test_risk_limits();
  test_risk_rate_limit();
  test_risk_kill_and_loss();
  test_risk_replace();
  test_risk_differential(n);
  if (g_failures == 0) std::puts("test_oms: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
