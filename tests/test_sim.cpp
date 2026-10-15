// Phase 4 tests: conformance suite for the matching rules, differential and
// property tests against a slow reference matcher, consistency of the
// published feed with the Phase 3 market data handler, and the latency model
// of the exchange simulator.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "alloc_trap.hpp"  // replaces global operator new; keep in this one TU
#include "check.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "hft/md/md_handler.hpp"
#include "hft/sim/exchange_sim.hpp"
#include "hft/sim/feed_publisher.hpp"
#include "hft/sim/matching_engine.hpp"
#include "order_gen.hpp"
#include "ref_matcher.hpp"

using namespace hft::sim;
using hft::book::BookConfig;
using hft::book::OrderBook;
using hft::core::Arena;
using hft::core::Region;
using hft::core::RegionOptions;
using hft::md::Line;
using hft::md::MdConfig;
using hft::md::MdHandler;
using testsupport::FlowConfig;
using testsupport::FlowGen;
using testsupport::Op;
using testsupport::RefMatcher;
using testsupport::Recorder;
using testsupport::same;

namespace {

constexpr std::int64_t kBase = 1'000'000;  // raw; Price::from_units(100)
constexpr std::int64_t kTick = 100;
constexpr std::uint32_t kLevels = 4096;

Price P(long ticks) { return Price::from_raw(kBase + ticks * kTick); }

Region make_region(std::size_t mb) { return Region::create(mb << 20, RegionOptions{false, false, true, -1}); }

EngineConfig ecfg(StpMode stp = StpMode::None, std::uint32_t max_orders = 1024, std::uint32_t levels = kLevels) {
  EngineConfig c;
  c.base = Price::from_raw(kBase);
  c.tick_raw = kTick;
  c.num_levels = levels;
  c.max_orders = max_orders;
  c.stp = stp;
  return c;
}

// ---- compact expectation helpers --------------------------------------------

constexpr long kAny = -9999;
constexpr long kInv = -8888;  // Price::invalid()

struct X {
  ReportType t;
  Reason r;
  Liquidity l;
  Owner o;
  OrderId id;
  OrderId c;
  long px;  // in ticks above base; kInv = invalid price; kAny = don't care
  long q;
  long lv;
};

X Acc(Owner o, OrderId id, long px, long q) { return {ReportType::Accepted, Reason::None, Liquidity::None, o, id, 0, px, q, q}; }
X Rej(Owner o, OrderId id, Reason r) { return {ReportType::Rejected, r, Liquidity::None, o, id, 0, kAny, kAny, 0}; }
X FillT(Owner o, OrderId id, OrderId c, long px, long q, long lv) { return {ReportType::Fill, Reason::None, Liquidity::Taker, o, id, c, px, q, lv}; }
X FillM(Owner o, OrderId id, OrderId c, long px, long q, long lv) { return {ReportType::Fill, Reason::None, Liquidity::Maker, o, id, c, px, q, lv}; }
X Can(Owner o, OrderId id, Reason r, long px, long q) { return {ReportType::Canceled, r, Liquidity::None, o, id, 0, px, q, 0}; }
X CanD(Owner o, OrderId id, OrderId c, long px, long q, long lv) { return {ReportType::Canceled, Reason::StpDecrement, Liquidity::None, o, id, c, px, q, lv}; }
X Repl(Owner o, OrderId id, long px, long q) { return {ReportType::Replaced, Reason::None, Liquidity::None, o, id, 0, px, q, q}; }
X CRej(Owner o, OrderId id) { return {ReportType::CancelRejected, Reason::UnknownOrder, Liquidity::None, o, id, 0, kInv, 0, 0}; }
X RRej(Owner o, OrderId id, Reason r) { return {ReportType::ReplaceRejected, r, Liquidity::None, o, id, 0, kAny, 0, 0}; }

struct XP {
  PublicType t;
  OrderId id;
  long px;
  long q;
};
XP PAdd(OrderId id, long px, long q) { return {PublicType::Add, id, px, q}; }
XP PExec(OrderId id, long px, long q) { return {PublicType::Exec, id, px, q}; }
XP PRed(OrderId id, long px, long q) { return {PublicType::Reduce, id, px, q}; }
XP PDel(OrderId id, long px, long q) { return {PublicType::Delete, id, px, q}; }

bool px_ok(Price got, long want) {
  if (want == kAny) return true;
  if (want == kInv) return got == Price::invalid();
  return got == P(want);
}

bool expect_reports(const Recorder& rec, std::initializer_list<X> xs, int line) {
  bool ok = rec.reports.size() == xs.size();
  if (!ok) std::fprintf(stderr, "line %d: expected %zu reports, got %zu\n", line, xs.size(), rec.reports.size());
  std::size_t i = 0;
  for (const X& x : xs) {
    if (i >= rec.reports.size()) break;
    const Report& r = rec.reports[i];
    const bool m = r.type == x.t && r.reason == x.r && r.liq == x.l && r.owner == x.o && r.id == x.id &&
                   r.counter_id == x.c && px_ok(r.price, x.px) && (x.q == kAny || r.qty == x.q) &&
                   (x.lv == kAny || r.leaves == x.lv);
    if (!m) {
      std::fprintf(stderr,
                   "line %d: report %zu mismatch: type=%d reason=%d liq=%d owner=%u id=%llu counter=%llu qty=%lld "
                   "leaves=%lld\n",
                   line, i, static_cast<int>(r.type), static_cast<int>(r.reason), static_cast<int>(r.liq), r.owner,
                   static_cast<unsigned long long>(r.id), static_cast<unsigned long long>(r.counter_id),
                   static_cast<long long>(r.qty), static_cast<long long>(r.leaves));
      ok = false;
    }
    ++i;
  }
  return ok;
}

bool expect_pubs(const Recorder& rec, std::initializer_list<XP> xs, int line) {
  bool ok = rec.pubs.size() == xs.size();
  if (!ok) std::fprintf(stderr, "line %d: expected %zu public events, got %zu\n", line, xs.size(), rec.pubs.size());
  std::size_t i = 0;
  for (const XP& x : xs) {
    if (i >= rec.pubs.size()) break;
    const PublicEvent& e = rec.pubs[i];
    if (!(e.type == x.t && e.id == x.id && e.price == P(x.px) && e.qty == x.q)) {
      std::fprintf(stderr, "line %d: public event %zu mismatch: type=%d id=%llu qty=%lld\n", line, i,
                   static_cast<int>(e.type), static_cast<unsigned long long>(e.id), static_cast<long long>(e.qty));
      ok = false;
    }
    ++i;
  }
  return ok;
}

#define ER(rig, ...) CHECK(expect_reports((rig).rec, {__VA_ARGS__}, __LINE__))
#define EP(rig, ...) CHECK(expect_pubs((rig).rec, {__VA_ARGS__}, __LINE__))
#define NO_PUBS(rig) CHECK((rig).rec.pubs.empty())

NewOrder mk(OrderId id, Owner o, Side s, OrderType t, long px, Qty q) {
  return NewOrder{id, o, s, t, P(px), q};
}
NewOrder Lim(OrderId id, Owner o, Side s, long px, Qty q) { return mk(id, o, s, OrderType::Limit, px, q); }
NewOrder Ioc(OrderId id, Owner o, Side s, long px, Qty q) { return mk(id, o, s, OrderType::IOC, px, q); }
NewOrder Fok(OrderId id, Owner o, Side s, long px, Qty q) { return mk(id, o, s, OrderType::FOK, px, q); }
NewOrder Post(OrderId id, Owner o, Side s, long px, Qty q) { return mk(id, o, s, OrderType::PostOnly, px, q); }
NewOrder Mkt(OrderId id, Owner o, Side s, Qty q) { return NewOrder{id, o, s, OrderType::Market, Price::from_raw(7), q}; }

constexpr Side Buy = Side::Buy;
constexpr Side Sell = Side::Sell;

// An engine wired to a Recorder. Each go()/cancel()/replace() clears the
// recorder first, so assertions see exactly one request's events.
struct Rig {
  Region region = make_region(8);
  Arena arena{region};
  Recorder rec;
  MatchingEngine<Recorder> eng;
  std::uint64_t ts = 0;
  bool ok = false;
  explicit Rig(StpMode stp = StpMode::None, std::uint32_t max_orders = 1024, std::uint32_t levels = kLevels) {
    ok = eng.init(arena, ecfg(stp, max_orders, levels), rec);
  }
  void go(const NewOrder& o) {
    rec.clear();
    eng.new_order(o, ++ts);
    CHECK(eng.check_invariants());
  }
  void cancel(OrderId id, Owner o) {
    rec.clear();
    eng.cancel(id, o, ++ts);
    CHECK(eng.check_invariants());
  }
  void replace(OrderId id, Owner o, long px, Qty q) {
    rec.clear();
    eng.replace(id, o, P(px), q, ++ts);
    CHECK(eng.check_invariants());
  }
};

// ---- conformance: basic matching ----------------------------------------------

void test_rest_on_empty_book() {
  Rig r;
  CHECK(r.ok);
  r.go(Lim(1, 1, Buy, 100, 10));
  ER(r, Acc(1, 1, 100, 10));
  EP(r, PAdd(1, 100, 10));
  CHECK(r.eng.best_bid() == P(100) && r.eng.best_bid_qty() == 10);
  CHECK(r.eng.best_ask() == Price::invalid() && r.eng.order_count() == 1);
}

void test_cross_fills_at_resting_price() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 10));
  r.go(Lim(11, 1, Buy, 105, 10));  // willing to pay 105, pays 101
  ER(r, Acc(1, 11, 105, 10), FillT(1, 11, 10, 101, 10, 0), FillM(2, 10, 11, 101, 10, 0));
  EP(r, PExec(10, 101, 10));
  CHECK(r.eng.order_count() == 0);
  CHECK(r.eng.best_bid() == Price::invalid() && r.eng.best_ask() == Price::invalid());
}

void test_partial_fill_then_rest() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 4));
  r.go(Lim(11, 1, Buy, 102, 10));
  ER(r, Acc(1, 11, 102, 10), FillT(1, 11, 10, 101, 4, 6), FillM(2, 10, 11, 101, 4, 0));
  EP(r, PExec(10, 101, 4), PAdd(11, 102, 6));
  CHECK(r.eng.best_bid() == P(102) && r.eng.best_bid_qty() == 6 && r.eng.order_count() == 1);
}

void test_price_priority_across_levels() {
  Rig r;
  r.go(Lim(12, 2, Sell, 103, 5));  // inserted out of price order on purpose
  r.go(Lim(10, 2, Sell, 101, 5));
  r.go(Lim(11, 2, Sell, 102, 5));
  r.go(Lim(20, 1, Buy, 103, 12));
  ER(r, Acc(1, 20, 103, 12), FillT(1, 20, 10, 101, 5, 7), FillM(2, 10, 20, 101, 5, 0),
     FillT(1, 20, 11, 102, 5, 2), FillM(2, 11, 20, 102, 5, 0), FillT(1, 20, 12, 103, 2, 0),
     FillM(2, 12, 20, 103, 2, 3));
  CHECK(r.eng.best_ask() == P(103) && r.eng.best_ask_qty() == 3);

  // The mirror image: a sell sweeps bids from the highest price down.
  Rig s;
  s.go(Lim(1, 2, Buy, 98, 5));
  s.go(Lim(2, 2, Buy, 100, 5));
  s.go(Lim(3, 2, Buy, 99, 5));
  s.go(Lim(9, 1, Sell, 98, 12));
  ER(s, Acc(1, 9, 98, 12), FillT(1, 9, 2, 100, 5, 7), FillM(2, 2, 9, 100, 5, 0), FillT(1, 9, 3, 99, 5, 2),
     FillM(2, 3, 9, 99, 5, 0), FillT(1, 9, 1, 98, 2, 0), FillM(2, 1, 9, 98, 2, 3));
}

void test_time_priority_within_level() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 5));
  r.go(Lim(11, 3, Sell, 101, 5));
  r.go(Lim(20, 1, Buy, 101, 7));
  ER(r, Acc(1, 20, 101, 7), FillT(1, 20, 10, 101, 5, 2), FillM(2, 10, 20, 101, 5, 0), FillT(1, 20, 11, 101, 2, 0),
     FillM(3, 11, 20, 101, 2, 3));
  CHECK(r.eng.level_qty(Sell, P(101)) == 3);
}

void test_no_cross_no_trade() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 5));
  r.go(Lim(2, 2, Sell, 101, 5));
  ER(r, Acc(2, 2, 101, 5));
  CHECK(r.eng.best_bid() == P(100) && r.eng.best_ask() == P(101) && r.eng.order_count() == 2);
}

// ---- conformance: order types -------------------------------------------------

void test_market_orders() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 5));
  r.go(Lim(11, 2, Sell, 102, 5));
  r.go(Mkt(20, 1, Buy, 8));  // the price field of a market order is ignored
  ER(r, Acc(1, 20, kInv, 8), FillT(1, 20, 10, 101, 5, 3), FillM(2, 10, 20, 101, 5, 0), FillT(1, 20, 11, 102, 3, 0),
     FillM(2, 11, 20, 102, 3, 2));
  r.go(Mkt(21, 1, Buy, 20));
  ER(r, Acc(1, 21, kInv, 20), FillT(1, 21, 11, 102, 2, 18), FillM(2, 11, 21, 102, 2, 0),
     Can(1, 21, Reason::MarketRemainder, kInv, 18));
  CHECK(!r.eng.contains(21) && r.eng.order_count() == 0);
  r.go(Mkt(22, 1, Sell, 5));  // empty book
  ER(r, Acc(1, 22, kInv, 5), Can(1, 22, Reason::MarketRemainder, kInv, 5));
  NO_PUBS(r);
  r.go(Mkt(22, 1, Sell, 0));
  ER(r, Rej(1, 22, Reason::BadQty));
}

void test_ioc() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 5));
  r.go(Ioc(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), FillT(1, 20, 10, 101, 5, 3), FillM(2, 10, 20, 101, 5, 0),
     Can(1, 20, Reason::IocRemainder, 101, 3));
  CHECK(r.eng.order_count() == 0);
  r.go(Lim(11, 2, Sell, 101, 5));
  r.go(Ioc(21, 1, Buy, 100, 4));  // does not reach the ask
  ER(r, Acc(1, 21, 100, 4), Can(1, 21, Reason::IocRemainder, 100, 4));
  NO_PUBS(r);
  CHECK(r.eng.order_count() == 1 && !r.eng.contains(21));
}

void test_fok() {
  auto seed = [](Rig& r) {
    r.go(Lim(10, 2, Sell, 101, 5));
    r.go(Lim(11, 2, Sell, 102, 5));
  };
  {
    Rig r;
    seed(r);
    r.go(Fok(20, 1, Buy, 102, 10));  // exactly enough across two levels
    ER(r, Acc(1, 20, 102, 10), FillT(1, 20, 10, 101, 5, 5), FillM(2, 10, 20, 101, 5, 0), FillT(1, 20, 11, 102, 5, 0),
       FillM(2, 11, 20, 102, 5, 0));
    CHECK(r.eng.order_count() == 0);
  }
  {
    Rig r;
    seed(r);
    const std::uint64_t before = r.eng.digest();
    r.go(Fok(20, 1, Buy, 102, 11));  // one short
    ER(r, Acc(1, 20, 102, 11), Can(1, 20, Reason::FokUnfillable, 102, 11));
    NO_PUBS(r);
    CHECK(r.eng.digest() == before && r.eng.order_count() == 2);
    r.go(Fok(21, 1, Buy, 101, 6));  // price limit stops it short of the second level
    ER(r, Acc(1, 21, 101, 6), Can(1, 21, Reason::FokUnfillable, 101, 6));
    CHECK(r.eng.digest() == before);
    r.go(Fok(22, 1, Buy, 101, 5));
    ER(r, Acc(1, 22, 101, 5), FillT(1, 22, 10, 101, 5, 0), FillM(2, 10, 22, 101, 5, 0));
  }
  {
    Rig r;  // sell side
    r.go(Lim(30, 2, Buy, 100, 5));
    r.go(Fok(31, 1, Sell, 100, 5));
    ER(r, Acc(1, 31, 100, 5), FillT(1, 31, 30, 100, 5, 0), FillM(2, 30, 31, 100, 5, 0));
  }
}

void test_post_only() {
  Rig r;
  r.go(Lim(10, 2, Sell, 101, 5));
  r.go(Post(20, 1, Buy, 100, 5));
  ER(r, Acc(1, 20, 100, 5));
  EP(r, PAdd(20, 100, 5));
  const std::uint64_t before = r.eng.digest();
  r.go(Post(21, 1, Buy, 101, 5));  // equal to the ask counts as crossing
  ER(r, Rej(1, 21, Reason::WouldCross));
  r.go(Post(22, 1, Buy, 105, 5));
  ER(r, Rej(1, 22, Reason::WouldCross));
  NO_PUBS(r);
  CHECK(r.eng.digest() == before);
  r.go(Post(23, 2, Sell, 100, 5));  // sell at the bid
  ER(r, Rej(2, 23, Reason::WouldCross));
  r.go(Post(24, 2, Sell, 102, 5));
  ER(r, Acc(2, 24, 102, 5));
  CHECK(r.eng.best_ask() == P(101) && r.eng.order_count() == 3);
}

// ---- conformance: self-trade prevention ---------------------------------------

// Asks: id10 (owner 1) then id11 (owner 2), both 5 @101. Owner 1 buys 8.
void stp_setup(Rig& r) {
  r.go(Lim(10, 1, Sell, 101, 5));
  r.go(Lim(11, 2, Sell, 101, 5));
}

void test_stp_none_allows_self_trade() {
  Rig r(StpMode::None);
  stp_setup(r);
  r.go(Lim(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), FillT(1, 20, 10, 101, 5, 3), FillM(1, 10, 20, 101, 5, 0), FillT(1, 20, 11, 101, 3, 0),
     FillM(2, 11, 20, 101, 3, 2));
}

void test_stp_cancel_newest() {
  Rig r(StpMode::CancelNewest);
  stp_setup(r);
  r.go(Lim(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), Can(1, 20, Reason::StpCancel, 101, 8));
  NO_PUBS(r);
  CHECK(r.eng.level_qty(Sell, P(101)) == 10 && r.eng.order_count() == 2 && !r.eng.contains(20));
  // A taker that trades with others first and meets its own order later.
  Rig s(StpMode::CancelNewest);
  s.go(Lim(11, 2, Sell, 101, 5));
  s.go(Lim(10, 1, Sell, 102, 5));
  s.go(Lim(20, 1, Buy, 102, 8));
  ER(s, Acc(1, 20, 102, 8), FillT(1, 20, 11, 101, 5, 3), FillM(2, 11, 20, 101, 5, 0),
     Can(1, 20, Reason::StpCancel, 102, 3));
  CHECK(s.eng.contains(10) && !s.eng.contains(20) && s.eng.best_bid() == Price::invalid());
}

void test_stp_cancel_oldest() {
  Rig r(StpMode::CancelOldest);
  stp_setup(r);
  r.go(Lim(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), Can(1, 10, Reason::StpCancel, 101, 5), FillT(1, 20, 11, 101, 5, 3),
     FillM(2, 11, 20, 101, 5, 0));
  EP(r, PDel(10, 101, 5), PExec(11, 101, 5), PAdd(20, 101, 3));
  CHECK(r.eng.best_bid() == P(101) && r.eng.best_bid_qty() == 3 && r.eng.best_ask() == Price::invalid());
}

void test_stp_cancel_both() {
  Rig r(StpMode::CancelBoth);
  stp_setup(r);
  r.go(Lim(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), Can(1, 10, Reason::StpCancel, 101, 5), Can(1, 20, Reason::StpCancel, 101, 8));
  EP(r, PDel(10, 101, 5));
  CHECK(r.eng.contains(11) && !r.eng.contains(10) && !r.eng.contains(20) && r.eng.order_count() == 1);
}

void test_stp_decrement_and_cancel() {
  Rig r(StpMode::DecrementAndCancel);
  stp_setup(r);
  r.go(Lim(20, 1, Buy, 101, 8));
  ER(r, Acc(1, 20, 101, 8), CanD(1, 20, 10, 101, 5, 3), CanD(1, 10, 20, 101, 5, 0), FillT(1, 20, 11, 101, 3, 0),
     FillM(2, 11, 20, 101, 3, 2));
  EP(r, PDel(10, 101, 5), PExec(11, 101, 3));
  CHECK(r.eng.level_qty(Sell, P(101)) == 2 && r.eng.order_count() == 1);

  Rig s(StpMode::DecrementAndCancel);  // resting order is bigger: it shrinks and stays
  s.go(Lim(10, 1, Sell, 101, 10));
  s.go(Lim(20, 1, Buy, 101, 4));
  ER(s, Acc(1, 20, 101, 4), CanD(1, 20, 10, 101, 4, 0), CanD(1, 10, 20, 101, 4, 6));
  EP(s, PRed(10, 101, 4));
  CHECK(s.eng.level_qty(Sell, P(101)) == 6 && s.eng.order_count() == 1 && !s.eng.contains(20));
}

void test_stp_with_fok() {
  auto seed = [](Rig& r) {
    stp_setup(r);  // id10 owner 1, id11 owner 2, 5 each
  };
  {
    Rig r(StpMode::None);
    seed(r);
    r.go(Fok(20, 1, Buy, 101, 5));
    ER(r, Acc(1, 20, 101, 5), FillT(1, 20, 10, 101, 5, 0), FillM(1, 10, 20, 101, 5, 0));
  }
  {
    Rig r(StpMode::CancelOldest);  // the own order is skipped, so 5 is still available
    seed(r);
    r.go(Fok(20, 1, Buy, 101, 5));
    ER(r, Acc(1, 20, 101, 5), Can(1, 10, Reason::StpCancel, 101, 5), FillT(1, 20, 11, 101, 5, 0),
       FillM(2, 11, 20, 101, 5, 0));
  }
  for (StpMode m : {StpMode::CancelNewest, StpMode::CancelBoth, StpMode::DecrementAndCancel}) {
    Rig r(m);  // these modes would stop or shrink the taker, so it cannot fill: nothing may change
    seed(r);
    const std::uint64_t before = r.eng.digest();
    r.go(Fok(20, 1, Buy, 101, 5));
    ER(r, Acc(1, 20, 101, 5), Can(1, 20, Reason::FokUnfillable, 101, 5));
    NO_PUBS(r);
    CHECK(r.eng.digest() == before);
  }
}

void test_stp_on_replace() {
  Rig r(StpMode::CancelNewest);
  r.go(Lim(1, 1, Buy, 100, 5));
  r.go(Lim(2, 1, Sell, 101, 5));
  r.replace(1, 1, 101, 5);  // would now cross the owner's own ask
  ER(r, Repl(1, 1, 101, 5), Can(1, 1, Reason::StpCancel, 101, 5));
  CHECK(!r.eng.contains(1) && r.eng.contains(2));
}

// ---- conformance: cancel and replace --------------------------------------------

void test_cancel() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 10));
  r.cancel(1, 1);
  ER(r, Can(1, 1, Reason::UserRequested, 100, 10));
  EP(r, PDel(1, 100, 10));
  CHECK(r.eng.order_count() == 0 && r.eng.best_bid() == Price::invalid());
  r.cancel(1, 1);  // already gone
  ER(r, CRej(1, 1));
  r.cancel(99, 1);
  ER(r, CRej(1, 99));

  r.go(Lim(2, 1, Buy, 100, 5));
  r.cancel(2, 2);  // someone else's order
  ER(r, CRej(2, 2));
  CHECK(r.eng.contains(2));

  r.go(Lim(3, 1, Buy, 100, 10));  // partial fill, then cancel the rest
  r.go(Ioc(30, 3, Sell, 100, 9));  // eats id2 (5) first, then 4 of id3
  r.cancel(3, 1);
  ER(r, Can(1, 3, Reason::UserRequested, 100, 6));
  r.go(Ioc(31, 3, Sell, 100, 1));
  r.cancel(2, 1);  // filled completely earlier
  ER(r, CRej(1, 2));
}

void test_replace_shrink_keeps_priority() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 10));
  r.go(Lim(2, 2, Buy, 100, 10));
  r.replace(1, 1, 100, 4);
  ER(r, Repl(1, 1, 100, 4));
  EP(r, PRed(1, 100, 6));
  r.go(Ioc(30, 3, Sell, 100, 6));
  ER(r, Acc(3, 30, 100, 6), FillT(3, 30, 1, 100, 4, 2), FillM(1, 1, 30, 100, 4, 0), FillT(3, 30, 2, 100, 2, 0),
     FillM(2, 2, 30, 100, 2, 8));
}

void test_replace_increase_loses_priority() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 10));
  r.go(Lim(2, 2, Buy, 100, 10));
  r.replace(1, 1, 100, 12);
  ER(r, Repl(1, 1, 100, 12));
  EP(r, PDel(1, 100, 10), PAdd(1, 100, 12));
  r.go(Ioc(30, 3, Sell, 100, 5));
  ER(r, Acc(3, 30, 100, 5), FillT(3, 30, 2, 100, 5, 0), FillM(2, 2, 30, 100, 5, 5));
  CHECK(r.eng.level_qty(Buy, P(100)) == 17);
}

void test_replace_price_change() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 10));
  r.go(Lim(2, 1, Buy, 100, 10));
  r.replace(2, 1, 101, 10);
  ER(r, Repl(1, 2, 101, 10));
  CHECK(r.eng.best_bid() == P(101) && r.eng.level_qty(Buy, P(100)) == 10);
  r.replace(2, 1, 99, 10);  // and back down; the old level must be recomputed
  CHECK(r.eng.best_bid() == P(100) && r.eng.level_qty(Buy, P(99)) == 10);
}

void test_replace_that_crosses_trades() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 5));
  r.go(Lim(9, 2, Sell, 101, 3));
  r.replace(1, 1, 101, 5);
  ER(r, Repl(1, 1, 101, 5), FillT(1, 1, 9, 101, 3, 2), FillM(2, 9, 1, 101, 3, 0));
  EP(r, PDel(1, 100, 5), PExec(9, 101, 3), PAdd(1, 101, 2));
  CHECK(r.eng.best_bid() == P(101) && r.eng.best_bid_qty() == 2 && r.eng.best_ask() == Price::invalid());
}

void test_replace_rejections_and_noop() {
  Rig r;
  r.go(Lim(1, 1, Buy, 100, 10));
  const std::uint64_t before = r.eng.digest();
  r.replace(99, 1, 100, 5);
  ER(r, RRej(1, 99, Reason::UnknownOrder));
  r.replace(1, 2, 100, 5);
  ER(r, RRej(2, 1, Reason::UnknownOrder));
  r.replace(1, 1, 100, 0);
  ER(r, RRej(1, 1, Reason::BadQty));
  r.replace(1, 1, 100, -3);
  ER(r, RRej(1, 1, Reason::BadQty));
  r.rec.clear();
  r.eng.replace(1, 1, Price::from_raw(P(100).raw() + 1), 5, 50);  // off tick
  ER(r, RRej(1, 1, Reason::BadPrice));
  r.replace(1, 1, 4096, 5);  // beyond the window
  ER(r, RRej(1, 1, Reason::BadPrice));
  r.replace(1, 1, -1, 5);  // below the base
  ER(r, RRej(1, 1, Reason::BadPrice));
  CHECK(r.eng.digest() == before);
  r.replace(1, 1, 100, 10);  // identical: acknowledged, nothing changes
  ER(r, Repl(1, 1, 100, 10));
  NO_PUBS(r);
  CHECK(r.eng.digest() == before);
}

// ---- conformance: validation and limits -------------------------------------------

void test_validation() {
  Rig r;
  r.go(Lim(0, 1, Buy, 100, 5));
  ER(r, Rej(1, 0, Reason::BadId));
  r.go(Lim(1, 1, Buy, 100, 0));
  ER(r, Rej(1, 1, Reason::BadQty));
  r.go(Lim(1, 1, Buy, 100, -4));
  ER(r, Rej(1, 1, Reason::BadQty));
  r.go(NewOrder{1, 1, Buy, OrderType::Limit, Price::from_raw(P(100).raw() + 50), 5});
  ER(r, Rej(1, 1, Reason::BadPrice));
  r.go(Lim(1, 1, Buy, -1, 5));  // below base
  ER(r, Rej(1, 1, Reason::BadPrice));
  r.go(Lim(1, 1, Buy, 4096, 5));  // first level past the window
  ER(r, Rej(1, 1, Reason::BadPrice));
  r.go(Lim(1, 1, Buy, 4095, 5));  // last valid level
  ER(r, Acc(1, 1, 4095, 5));
  r.go(Lim(1, 1, Sell, 4000, 5));  // id already resting
  ER(r, Rej(1, 1, Reason::Duplicate));
  r.go(Mkt(1, 1, Sell, 5));  // market orders obey the id rule too
  ER(r, Rej(1, 1, Reason::Duplicate));
  CHECK(r.eng.order_count() == 1 && r.eng.order_qty(1) == 5);
  r.go(Lim(0, 1, Buy, 100, 0));  // several faults: the first check in the documented order wins
  ER(r, Rej(1, 0, Reason::BadId));

  Rig s;  // an id is reusable once its order is gone
  s.go(Lim(10, 2, Sell, 101, 5));
  s.go(Lim(11, 1, Buy, 101, 5));
  s.go(Lim(10, 2, Sell, 105, 5));
  ER(s, Acc(2, 10, 105, 5));
}

void test_book_full() {
  Rig r(StpMode::None, 4);
  for (OrderId i = 1; i <= 4; ++i) r.go(Lim(i, 2, Buy, 100, 5));
  r.go(Lim(5, 1, Buy, 99, 3));
  ER(r, Acc(1, 5, 99, 3), Can(1, 5, Reason::BookFull, 99, 3));
  CHECK(r.eng.order_count() == 4);
  // A crossing order frees slots first, so its remainder can rest.
  r.go(Lim(6, 1, Sell, 99, 25));
  ER(r, Acc(1, 6, 99, 25), FillT(1, 6, 1, 100, 5, 20), FillM(2, 1, 6, 100, 5, 0), FillT(1, 6, 2, 100, 5, 15),
     FillM(2, 2, 6, 100, 5, 0), FillT(1, 6, 3, 100, 5, 10), FillM(2, 3, 6, 100, 5, 0), FillT(1, 6, 4, 100, 5, 5),
     FillM(2, 4, 6, 100, 5, 0));
  CHECK(r.eng.order_count() == 1 && r.eng.best_ask() == P(99) && r.eng.best_ask_qty() == 5);
}

void test_window_edges() {
  Rig r;
  r.go(Lim(1, 1, Buy, 0, 5));
  r.go(Lim(2, 1, Sell, 4095, 5));
  CHECK(r.eng.best_bid() == P(0) && r.eng.best_ask() == P(4095));
  r.go(Ioc(3, 2, Sell, 0, 5));
  ER(r, Acc(2, 3, 0, 5), FillT(2, 3, 1, 0, 5, 0), FillM(1, 1, 3, 0, 5, 0));
  CHECK(r.eng.best_bid() == Price::invalid());
  r.go(Mkt(4, 1, Buy, 5));
  ER(r, Acc(1, 4, kInv, 5), FillT(1, 4, 2, 4095, 5, 0), FillM(1, 2, 4, 4095, 5, 0));
  CHECK(r.eng.best_ask() == Price::invalid() && r.eng.order_count() == 0);
}


// ---- differential and property tests -----------------------------------------------

struct Counters {
  std::uint64_t fills = 0, fok_fail = 0, fok_ok = 0, wouldcross = 0, stp = 0, bookfull = 0, rejects = 0,
                cancel_rej = 0, replaced = 0, ioc_rem = 0, mkt_rem = 0, cancels = 0;
};

// Rules that must hold for every request, checked without consulting the reference.
void check_properties(const Op& op, const Recorder& rec, Counters& c) {
  Qty taker_fill = 0, maker_fill = 0, exec_pub = 0;
  bool any_fill = false;
  const bool any_pub = !rec.pubs.empty();
  for (const Report& r : rec.reports) {
    if (r.type == ReportType::Fill) {
      any_fill = true;
      ++c.fills;
      (r.liq == Liquidity::Taker ? taker_fill : maker_fill) += r.qty;
      CHECK(r.qty > 0);
    }
    if (r.type == ReportType::Rejected) ++c.rejects;
    if (r.type == ReportType::CancelRejected) ++c.cancel_rej;
    if (r.type == ReportType::Replaced) ++c.replaced;
    if (r.type == ReportType::Canceled) {
      if (r.reason == Reason::StpCancel || r.reason == Reason::StpDecrement) ++c.stp;
      if (r.reason == Reason::BookFull) ++c.bookfull;
      if (r.reason == Reason::IocRemainder) ++c.ioc_rem;
      if (r.reason == Reason::MarketRemainder) ++c.mkt_rem;
      if (r.reason == Reason::UserRequested) ++c.cancels;
    }
  }
  for (const PublicEvent& e : rec.pubs)
    if (e.type == PublicType::Exec) exec_pub += e.qty;
  CHECK(taker_fill == maker_fill);  // every trade has two equal sides
  CHECK(exec_pub == taker_fill);    // and exactly one public Exec
  if (op.kind != Op::New) return;

  const NewOrder& o = op.o;
  const bool rejected = !rec.reports.empty() && rec.reports[0].type == ReportType::Rejected;
  if (rejected) {
    CHECK(rec.reports.size() == 1 && !any_pub);  // a reject leaves no trace
    if (rec.reports[0].reason == Reason::WouldCross) ++c.wouldcross;
    return;
  }
  if (o.type == OrderType::PostOnly) CHECK(!any_fill);
  if (o.type == OrderType::FOK) {
    const bool killed = rec.reports.size() == 2 && rec.reports[1].reason == Reason::FokUnfillable;
    if (killed) {
      CHECK(!any_fill && !any_pub);  // killed means the book was not touched
      ++c.fok_fail;
    } else {
      CHECK(taker_fill == o.qty);  // otherwise it traded everything
      ++c.fok_ok;
    }
  }
}

bool streams_equal(const Recorder& a, const Recorder& b) {
  if (a.reports.size() != b.reports.size() || a.pubs.size() != b.pubs.size()) return false;
  for (std::size_t i = 0; i < a.reports.size(); ++i)
    if (!same(a.reports[i], b.reports[i])) return false;
  for (std::size_t i = 0; i < a.pubs.size(); ++i)
    if (!same(a.pubs[i], b.pubs[i])) return false;
  return true;
}

template <class Eng>
void apply(Eng& e, const Op& op, std::uint64_t ts) {
  switch (op.kind) {
    case Op::New: e.new_order(op.o, ts); break;
    case Op::Cancel: e.cancel(op.id, op.owner, ts); break;
    case Op::Replace: e.replace(op.id, op.owner, op.price, op.qty, ts); break;
  }
}

Counters diff_run(StpMode stp, std::uint64_t nops, std::uint64_t seed, std::uint32_t max_orders, std::uint32_t levels) {
  Region region = make_region(16);
  Arena arena{region};
  Recorder rec;
  MatchingEngine<Recorder> eng;
  const EngineConfig cfg = ecfg(stp, max_orders, levels);
  CHECK(eng.init(arena, cfg, rec));
  RefMatcher ref(cfg);
  FlowGen gen(FlowConfig{cfg.base, kTick, levels, 4, true}, seed);
  Counters c;
  const std::uint64_t inv_every = levels <= 128 ? 1 : 61;
  for (std::uint64_t i = 0; i < nops; ++i) {
    const Op op = gen.next();
    rec.clear();
    ref.out.clear();
    apply(eng, op, i + 1);
    apply(ref, op, i + 1);
    if (!streams_equal(rec, ref.out) || eng.digest() != ref.digest() || eng.order_count() != ref.order_count()) {
      std::fprintf(stderr, "diff_run stp=%d seed=%llu: engine and reference diverged at op %llu (kind %d)\n",
                   static_cast<int>(stp), static_cast<unsigned long long>(seed), static_cast<unsigned long long>(i),
                   static_cast<int>(op.kind));
      CHECK(false);
      return c;
    }
    check_properties(op, rec, c);
    if (i % inv_every == 0 && !eng.check_invariants()) {
      std::fprintf(stderr, "diff_run: invariants broken at op %llu\n", static_cast<unsigned long long>(i));
      CHECK(false);
      return c;
    }
  }
  CHECK(eng.check_invariants());
  return c;
}

void test_differential(std::uint64_t nops) {
  const StpMode modes[] = {StpMode::None, StpMode::CancelNewest, StpMode::CancelOldest, StpMode::CancelBoth,
                           StpMode::DecrementAndCancel};
  std::uint64_t seed = 1;
  for (StpMode m : modes) {
    // Tiny book: forces BookFull and heavy level reuse. Large book: realistic depth.
    for (int big = 0; big < 2; ++big) {
      const Counters c = diff_run(m, nops, seed++, big ? 4096u : 48u, big ? 1024u : 64u);
      if (nops >= 20000) {  // short runs may legitimately miss a rare path
      CHECK(c.fills > 0 && c.fok_fail > 0 && c.fok_ok > 0 && c.wouldcross > 0 && c.ioc_rem > 0 && c.mkt_rem > 0);
      CHECK(c.cancels > 0 && c.replaced > 0 && c.cancel_rej > 0 && c.rejects > 0);
      if (m == StpMode::None) CHECK(c.stp == 0);
      else CHECK(c.stp > 0);
      if (!big) CHECK(c.bookfull > 0);
      }
      std::printf("  stp=%d %-5s fills=%llu fok(ok/killed)=%llu/%llu stp_events=%llu bookfull=%llu\n",
                  static_cast<int>(m), big ? "large" : "tiny", static_cast<unsigned long long>(c.fills),
                  static_cast<unsigned long long>(c.fok_ok), static_cast<unsigned long long>(c.fok_fail),
                  static_cast<unsigned long long>(c.stp), static_cast<unsigned long long>(c.bookfull));
    }
  }
}

// Many seeds on a tiny book: short runs reach states a single long run may not.
void test_many_seeds(std::uint64_t seeds) {
  for (std::uint64_t s = 0; s < seeds; ++s) {
    const StpMode m = static_cast<StpMode>(s % 5);
    diff_run(m, 3000, 1000 + s, 24, 32);
  }
}

// ---- engine -> feed -> Phase 3 handler ------------------------------------------------

struct FeedEmit {
  MdHandler<>* h;
  std::uint64_t* now;
  void operator()(const std::uint8_t* d, std::size_t n) { h->on_packet(Line::A, d, n, ++*now); }
};
struct FeedTap {
  ItchPublisher<FeedEmit>* pub;
  void on_report(const Report&) {}
  void on_public(const PublicEvent& e) { pub->on_public(e); }
};

void test_feed_rebuilds_book(std::uint64_t nops, std::uint64_t flush_every, StpMode stp, std::uint64_t seed) {
  Region region = make_region(32);
  Arena arena{region};
  BookConfig bc;
  bc.base = Price::from_raw(kBase);
  bc.tick_raw = kTick;
  bc.num_levels = kLevels;
  bc.max_orders = 1u << 14;
  OrderBook book;
  MdHandler<> h;
  MdConfig mc;
  mc.locate = 7;
  CHECK(book.init(arena, bc) && h.init(arena, book, mc));
  h.begin_live(1);
  std::uint64_t now = 0;
  ItchPublisher<FeedEmit> pub("ENGSIM0001", 7, FeedEmit{&h, &now});
  FeedTap tap{&pub};
  MatchingEngine<FeedTap> eng;
  CHECK(eng.init(arena, ecfg(stp, 1u << 14), tap));
  FlowGen gen(FlowConfig{Price::from_raw(kBase), kTick, kLevels, 4, true}, seed);
  for (std::uint64_t i = 0; i < nops; ++i) {
    apply(eng, gen.next(), i + 1);
    if ((i + 1) % flush_every == 0) {
      pub.flush();
      if (book.digest() != eng.digest() || !h.book_valid()) {
        std::fprintf(stderr, "feed diverged at op %llu\n", static_cast<unsigned long long>(i));
        CHECK(false);
        return;
      }
    }
  }
  pub.flush();
  CHECK(book.digest() == eng.digest());
  CHECK(book.check_invariants() && eng.check_invariants());
  const auto& st = h.stats();
  CHECK(h.state() == hft::md::FeedState::Live && h.book_valid());
  CHECK(st.desyncs == 0 && st.book_errors == 0 && st.malformed == 0 && st.gaps_declared == 0);
  CHECK(st.dups[0] == 0 && st.parked == 0 && st.unknown_types == 0);
  CHECK(h.next_seq() == pub.next_seq());
  CHECK(st.messages > 0);
}

// ---- exchange simulator -----------------------------------------------------------------

struct Entry {
  int kind;  // 0 report, 1 public
  Owner p;
  std::uint64_t time;
  Report r;
  PublicEvent e;
};
struct LogSink {
  std::vector<Entry> v;
  void on_report(Owner p, const Report& r, std::uint64_t t) { v.push_back(Entry{0, p, t, r, PublicEvent{}}); }
  void on_public(const PublicEvent& e, std::uint64_t t) { v.push_back(Entry{1, 0, t, Report{}, e}); }
};

struct HashSink {
  std::uint64_t h = 1469598103934665603ULL, reports = 0, pubs = 0, last = 0;
  bool monotonic = true;
  void mix(std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; }
  void note(std::uint64_t t) {
    monotonic = monotonic && t >= last;
    last = t;
  }
  void on_report(Owner p, const Report& r, std::uint64_t t) {
    mix(p), mix(t), mix(r.seq), mix(r.ts), mix(r.id), mix(r.counter_id), mix(static_cast<std::uint64_t>(r.qty));
    mix(static_cast<std::uint64_t>(r.leaves)), mix(static_cast<std::uint64_t>(r.price.raw()));
    mix(static_cast<std::uint64_t>(r.type) | static_cast<std::uint64_t>(r.reason) << 8);
    note(t);
    ++reports;
  }
  void on_public(const PublicEvent& e, std::uint64_t t) {
    mix(t), mix(e.seq), mix(e.ts), mix(e.id), mix(static_cast<std::uint64_t>(e.qty));
    mix(static_cast<std::uint64_t>(e.type));
    note(t);
    ++pubs;
  }
};

SimConfig scfg(std::uint64_t in, std::uint64_t eg, std::uint64_t md, std::uint64_t proc, std::uint64_t jit,
               std::uint64_t seed, std::uint32_t cap = 1u << 14) {
  SimConfig sc;
  sc.engine = ecfg();
  sc.latency.ingress_ns = in;
  sc.latency.egress_ns = eg;
  sc.latency.md_ns = md;
  sc.latency.processing_ns = proc;
  sc.latency.jitter_ns = jit;
  sc.latency.seed = seed;
  sc.queue_capacity = cap;
  sc.max_participants = 8;
  return sc;
}

void test_sim_latency_math() {
  Region region = make_region(8);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(100, 50, 30, 10, 0, 1, 64)));
  LogSink log;
  CHECK(sim.submit_new(1, Lim(1, 1, Buy, 100, 5), 1000));
  CHECK(sim.advance_to(1099, log) == 0 && sim.pending() == 1);  // still on the wire
  CHECK(sim.advance_to(1139, log) == 0);                        // matched at 1100; outputs not yet due
  CHECK(sim.pending() == 2);
  CHECK(sim.advance_to(1140, log) == 1);  // public Add leaves at engine time 1110 + md 30
  CHECK(log.v.size() == 1 && log.v[0].kind == 1 && log.v[0].time == 1140 && log.v[0].e.ts == 1110);
  CHECK(log.v[0].e.type == PublicType::Add && log.v[0].e.id == 1);
  CHECK(sim.advance_to(1159, log) == 0);
  CHECK(sim.advance_to(1160, log) == 1);  // report: 1110 + egress 50
  CHECK(log.v.size() == 2 && log.v[1].kind == 0 && log.v[1].time == 1160 && log.v[1].p == 1);
  CHECK(log.v[1].r.type == ReportType::Accepted && log.v[1].r.ts == 1110);
  CHECK(sim.pending() == 0 && sim.dropped_out() == 0);
}

void test_sim_race_between_participants() {
  for (int first = 1; first <= 2; ++first) {
    Region region = make_region(8);
    Arena arena{region};
    ExchangeSim sim;
    CHECK(sim.init(arena, scfg(100, 0, 0, 0, 0, 1)));
    LogSink log;
    // Participant 1 sells and participant 2 buys the same price. Whoever's
    // order ARRIVES first rests; the other one trades against it.
    const std::uint64_t t1 = first == 1 ? 1000 : 1050, t2 = first == 1 ? 1050 : 1000;
    CHECK(sim.submit_new(2, Lim(2, 2, Buy, 100, 5), t2) == true);
    CHECK(sim.submit_new(1, Lim(1, 1, Sell, 100, 5), t1) == true);
    sim.advance_to(5000, log);
    Liquidity liq1 = Liquidity::None;
    for (const Entry& e : log.v)
      if (e.kind == 0 && e.p == 1 && e.r.type == ReportType::Fill) liq1 = e.r.liq;
    CHECK(liq1 == (first == 1 ? Liquidity::Maker : Liquidity::Taker));
  }
}

void test_sim_fifo_per_link_with_jitter() {
  Region region = make_region(8);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(100, 50, 20, 0, 500, 9)));
  LogSink log;
  for (OrderId i = 1; i <= 300; ++i) CHECK(sim.submit_new(1, Lim(i, 1, Buy, 100 - static_cast<long>(i % 50), 5), 1000 + i));
  sim.advance_to(1'000'000, log);
  OrderId expect = 1;
  std::uint64_t last_t = 0, last_ts = 0;
  std::size_t reports = 0;
  bool jittered = false;
  for (const Entry& e : log.v) {
    CHECK(e.time >= last_t);
    last_t = e.time;
    if (e.kind != 0) continue;
    ++reports;
    CHECK(e.r.id == expect++);  // jitter delays but never reorders one link
    CHECK(e.r.ts >= last_ts);
    last_ts = e.r.ts;
    if (e.r.ts != 1100 + e.r.id) jittered = true;
  }
  CHECK(reports == 300 && jittered);
  CHECK(sim.engine().order_count() == 300 && sim.pending() == 0 && sim.dropped_out() == 0);
}

void test_sim_refusals() {
  Region region = make_region(8);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(10, 0, 0, 0, 0, 1, 4)));
  LogSink log;
  for (OrderId i = 1; i <= 4; ++i) CHECK(sim.submit_new(1, Lim(i, 1, Buy, 90, 1), 100));
  CHECK(!sim.submit_new(1, Lim(5, 1, Buy, 90, 1), 100));  // queue full
  CHECK(!sim.submit_cancel(1, 1, 100));
  CHECK(sim.pending() == 4);  // refusals queue nothing
  sim.advance_to(200, log);
  CHECK(!sim.submit_new(1, Lim(6, 1, Buy, 90, 1), 150));  // earlier than the simulated clock
  CHECK(!sim.submit_new(8, Lim(7, 8, Buy, 90, 1), 300));   // unknown participant
  CHECK(sim.submit_new(7, Lim(8, 7, Buy, 90, 1), 300));
}

struct FlowRun {
  std::uint64_t hash, reports, pubs;
  bool monotonic;
  std::uint64_t dropped;
};

template <class Sink>
void drive(ExchangeSim& sim, Sink& sink, std::uint64_t nops, std::uint64_t seed, std::uint64_t step) {
  FlowGen gen(FlowConfig{Price::from_raw(kBase), kTick, kLevels, 3, true}, seed);
  for (std::uint64_t i = 0; i < nops; ++i) {
    const Op op = gen.next();
    const std::uint64_t t = (i + 1) * step;
    switch (op.kind) {
      case Op::New: CHECK(sim.submit_new(op.o.owner, op.o, t)); break;
      case Op::Cancel: CHECK(sim.submit_cancel(op.owner, op.id, t)); break;
      case Op::Replace: CHECK(sim.submit_replace(op.owner, op.id, op.price, op.qty, t)); break;
    }
    sim.advance_to(t, sink);
  }
  sim.advance_to(~std::uint64_t{0} >> 1, sink);
}

FlowRun run_flow(const SimConfig& sc, std::uint64_t nops, std::uint64_t seed) {
  Region region = make_region(32);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, sc));
  HashSink sink;
  drive(sim, sink, nops, seed, 40);
  CHECK(sim.pending() == 0);
  return FlowRun{sink.h, sink.reports, sink.pubs, sink.monotonic, sim.dropped_out()};
}

void test_sim_determinism(std::uint64_t nops) {
  const SimConfig a = scfg(300, 120, 80, 25, 400, 77);
  const FlowRun r1 = run_flow(a, nops, 5), r2 = run_flow(a, nops, 5);
  CHECK(r1.hash == r2.hash && r1.reports == r2.reports && r1.pubs == r2.pubs);
  CHECK(r1.monotonic && r1.dropped == 0 && r1.reports > nops / 2);
  const FlowRun r3 = run_flow(scfg(300, 120, 80, 25, 400, 78), nops, 5);  // different jitter stream
  CHECK(r3.hash != r1.hash);
  const FlowRun r4 = run_flow(a, nops, 6);  // different order flow
  CHECK(r4.hash != r1.hash);
}

// With every latency at zero the simulator must be indistinguishable from
// calling the engine directly.
void test_sim_zero_latency_equals_engine(std::uint64_t nops) {
  Region region = make_region(32);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(0, 0, 0, 0, 0, 1)));
  LogSink log;
  drive(sim, log, nops, 21, 10);

  Region region2 = make_region(16);
  Arena arena2{region2};
  Recorder rec;
  MatchingEngine<Recorder> eng;
  CHECK(eng.init(arena2, ecfg(), rec));
  FlowGen gen(FlowConfig{Price::from_raw(kBase), kTick, kLevels, 3, true}, 21);
  for (std::uint64_t i = 0; i < nops; ++i) apply(eng, gen.next(), (i + 1) * 10);

  std::vector<Report> reps;
  std::vector<PublicEvent> pubs;
  for (const Entry& e : log.v) (e.kind == 0 ? reps.push_back(e.r) : pubs.push_back(e.e));
  CHECK(reps.size() == rec.reports.size() && pubs.size() == rec.pubs.size());
  bool eq = reps.size() == rec.reports.size() && pubs.size() == rec.pubs.size();
  for (std::size_t i = 0; eq && i < reps.size(); ++i) eq = same(reps[i], rec.reports[i]);
  for (std::size_t i = 0; eq && i < pubs.size(); ++i) eq = same(pubs[i], rec.pubs[i]);
  CHECK(eq);
  CHECK(sim.engine().digest() == eng.digest());
}

// Whole pipeline, jittery network included: engine -> latency model -> ITCH
// -> handler -> book must reproduce the exchange's book.
struct PubSink {
  ItchPublisher<FeedEmit>* pub;
  std::uint64_t n = 0;
  void on_report(Owner, const Report&, std::uint64_t) { ++n; }
  void on_public(const PublicEvent& e, std::uint64_t) { pub->on_public(e); }
};

void test_sim_feed_end_to_end(std::uint64_t nops) {
  Region region = make_region(64);
  Arena arena{region};
  BookConfig bc;
  bc.base = Price::from_raw(kBase);
  bc.tick_raw = kTick;
  bc.num_levels = kLevels;
  bc.max_orders = 1u << 14;
  OrderBook book;
  MdHandler<> h;
  MdConfig mc;
  mc.locate = 3;
  CHECK(book.init(arena, bc) && h.init(arena, book, mc));
  h.begin_live(1);
  std::uint64_t now = 0;
  ItchPublisher<FeedEmit> pub("SIMFEED001", 3, FeedEmit{&h, &now});
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(200, 100, 150, 20, 300, 5)));
  PubSink sink{&pub};
  drive(sim, sink, nops, 33, 40);
  pub.flush();
  CHECK(book.digest() == sim.engine().digest());
  CHECK(book.check_invariants() && h.book_valid());
  CHECK(h.stats().desyncs == 0 && h.stats().book_errors == 0 && h.stats().gaps_declared == 0);
  CHECK(sim.dropped_out() == 0 && sink.n > 0);
}

// The hot path may not allocate: init() takes everything from the arena.
void test_no_allocation(std::uint64_t nops) {
  Region region = make_region(64);
  Arena arena{region};
  ExchangeSim sim;
  CHECK(sim.init(arena, scfg(100, 50, 30, 5, 100, 3)));
  std::uint64_t packets = 0;
  struct CountEmit {
    std::uint64_t* n;
    void operator()(const std::uint8_t*, std::size_t) { ++*n; }
  };
  ItchPublisher<CountEmit> pub("ALLOCTEST1", 1, CountEmit{&packets});
  struct S {
    ItchPublisher<CountEmit>* p;
    std::uint64_t n = 0;
    void on_report(Owner, const Report& r, std::uint64_t) { n += static_cast<std::uint64_t>(r.qty); }
    void on_public(const PublicEvent& e, std::uint64_t) { p->on_public(e); }
  } sink{&pub};

  FlowGen gen(FlowConfig{Price::from_raw(kBase), kTick, kLevels, 3, true}, 99);
  std::vector<Op> ops;  // generated before arming: the generator itself uses std::vector
  ops.reserve(nops);
  for (std::uint64_t i = 0; i < nops; ++i) ops.push_back(gen.next());

  alloc_trap::arm();
  for (std::uint64_t i = 0; i < nops; ++i) {
    const Op& op = ops[i];
    const std::uint64_t t = (i + 1) * 40;
    switch (op.kind) {
      case Op::New: sim.submit_new(op.o.owner, op.o, t); break;
      case Op::Cancel: sim.submit_cancel(op.owner, op.id, t); break;
      case Op::Replace: sim.submit_replace(op.owner, op.id, op.price, op.qty, t); break;
    }
    sim.advance_to(t, sink);
  }
  sim.advance_to(~std::uint64_t{0} >> 1, sink);
  pub.flush();
  const std::uint64_t allocs = alloc_trap::disarm();
  CHECK(allocs == 0);
  CHECK(packets > 0 && sink.n > 0 && sim.dropped_out() == 0 && sim.engine().check_invariants());
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000ULL;
  test_rest_on_empty_book();
  test_cross_fills_at_resting_price();
  test_partial_fill_then_rest();
  test_price_priority_across_levels();
  test_time_priority_within_level();
  test_no_cross_no_trade();
  test_market_orders();
  test_ioc();
  test_fok();
  test_post_only();
  test_stp_none_allows_self_trade();
  test_stp_cancel_newest();
  test_stp_cancel_oldest();
  test_stp_cancel_both();
  test_stp_decrement_and_cancel();
  test_stp_with_fok();
  test_stp_on_replace();
  test_cancel();
  test_replace_shrink_keeps_priority();
  test_replace_increase_loses_priority();
  test_replace_price_change();
  test_replace_that_crosses_trades();
  test_replace_rejections_and_noop();
  test_validation();
  test_book_full();
  test_window_edges();
  std::puts("conformance suite done");
  test_differential(n);
  test_many_seeds(n / 100 > 20 ? n / 100 : 20);
  test_feed_rebuilds_book(n, 1, StpMode::None, 11);
  test_feed_rebuilds_book(n, 37, StpMode::CancelOldest, 12);
  test_feed_rebuilds_book(n, 5, StpMode::DecrementAndCancel, 13);
  test_sim_latency_math();
  test_sim_race_between_participants();
  test_sim_fifo_per_link_with_jitter();
  test_sim_refusals();
  test_sim_determinism(n);
  test_sim_zero_latency_equals_engine(n);
  test_sim_feed_end_to_end(n);
  test_no_allocation(n);
  if (g_failures == 0) std::puts("test_sim: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
