// Phase 6 tests: tape rebuild, queue-position simulation (hand-made scenarios
// plus a comparison against the real matching engine), the PnL identity,
// determinism, signal sanity, kill switch in a backtest, no allocation.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "alloc_trap.hpp"  // replaces global operator new; keep in this one TU
#include "check.hpp"
#include "hft/bt/backtest.hpp"
#include "hft/bt/market_gen.hpp"
#include "hft/bt/tape_book.hpp"
#include "hft/bt/virtual_venue.hpp"
#include "hft/sim/matching_engine.hpp"
#include "hft/strat/market_maker.hpp"

using namespace hft;
using hft::core::OrderId;
using hft::core::Price;
using hft::core::Qty;
using hft::core::Side;
using hft::sim::PublicEvent;
using hft::sim::PublicType;
using hft::sim::Reason;
using hft::sim::Report;
using hft::sim::ReportType;
using namespace hft::bt;

namespace {

constexpr std::int64_t kBase = 1'000'000;
constexpr std::int64_t kTick = 100;
constexpr std::uint32_t kLevels = 4096;

Price P(std::int64_t level) { return Price::from_raw(kBase + level * kTick); }
core::Region make_region(std::size_t mb) {
  return core::Region::create(mb << 20, core::RegionOptions{false, false, true, -1});
}
book::BookConfig book_cfg(std::uint32_t max_orders = 1u << 12) {
  book::BookConfig b;
  b.base = Price::from_raw(kBase);
  b.tick_raw = kTick;
  b.num_levels = kLevels;
  b.max_orders = max_orders;
  return b;
}

// ---- hand-built scenarios for the queue model ------------------------------------------

struct Out {
  std::vector<Report> r;
  void report(const Report& x) { r.push_back(x); }
  std::size_t count(ReportType t) const {
    std::size_t n = 0;
    for (const Report& x : r) n += x.type == t;
    return n;
  }
  const Report* last(ReportType t) const {
    for (std::size_t i = r.size(); i-- > 0;)
      if (r[i].type == t) return &r[i];
    return nullptr;
  }
};

struct H {
  core::Region region = make_region(8);
  core::Arena arena{region};
  TapeBook book;
  VirtualVenue venue;
  Out out;
  std::uint64_t ts = 0;

  explicit H(std::uint32_t penalty_pct = 0) {
    CHECK(book.init(arena, book_cfg()));
    venue.init(&book, penalty_pct);
  }
  void tape(PublicType t, Side s, OrderId id, std::int64_t level, Qty q) {
    const PublicEvent e{t, s, id, P(level), q, ts, ts};
    ++ts;
    venue.on_tape(e, out);
    CHECK(book.apply(e));
  }
  void add(Side s, OrderId id, std::int64_t level, Qty q) { tape(PublicType::Add, s, id, level, q); }
  // Events on an order already in the book: look side and level up from the book.
  void on(PublicType t, OrderId id, Qty q) {
    TapeBook::Info i;
    CHECK(book.lookup(id, i));
    tape(t, i.side, id, book.tick_index(i.price), q);
  }
  void exec(OrderId id, Qty q) { on(PublicType::Exec, id, q); }
  void reduce(OrderId id, Qty q) { on(PublicType::Reduce, id, q); }
  void del(OrderId id) {
    TapeBook::Info i;
    CHECK(book.lookup(id, i));
    on(PublicType::Delete, id, i.qty);
  }
  void send(OrderId id, Side s, std::int64_t level, Qty q, sim::OrderType t = sim::OrderType::Limit) {
    venue.on_new(sim::NewOrder{id, 1, s, t, P(level), q}, ts++, out);
  }
  Qty ahead(OrderId id) const {
    Qty a = -1, l = -1;
    CHECK(venue.queue_state(id, a, l));
    return a;
  }
  Qty leaves(OrderId id) const {
    Qty a = -1, l = -1;
    CHECK(venue.queue_state(id, a, l));
    return l;
  }
};

void test_queue_basics() {
  {  // join behind the level; ahead is the whole level
    H h;
    h.add(Side::Buy, 1, 100, 10);
    h.add(Side::Buy, 2, 100, 5);
    h.add(Side::Sell, 3, 103, 7);
    h.send(900, Side::Buy, 100, 4);
    CHECK(h.out.count(ReportType::Accepted) == 1 && h.ahead(900) == 15 && h.leaves(900) == 4);
    h.exec(1, 6);  // a trade ahead of us advances the queue
    CHECK(h.ahead(900) == 9 && h.out.count(ReportType::Fill) == 0);
    h.del(2);      // cancel ahead of us: Exact model advances
    CHECK(h.ahead(900) == 4);
    h.reduce(1, 2);
    CHECK(h.ahead(900) == 2);
    h.add(Side::Buy, 4, 100, 9);  // arrives behind us
    CHECK(h.ahead(900) == 2);
    h.exec(1, 2);  // the rest of the order ahead
    CHECK(h.ahead(900) == 0 && h.out.count(ReportType::Fill) == 0);
    h.exec(4, 3);  // a trade behind us means we traded first
    const Report* f = h.out.last(ReportType::Fill);
    CHECK(f && f->qty == 3 && f->leaves == 1 && f->price == P(100) && f->liq == sim::Liquidity::Maker && f->id == 900);
    h.exec(4, 5);  // only 1 left of ours
    CHECK(h.out.count(ReportType::Fill) == 2 && h.out.last(ReportType::Fill)->qty == 1 &&
          h.out.last(ReportType::Fill)->leaves == 0);
    CHECK(h.venue.live_orders() == 0 && h.venue.stats().full_fills == 1 && h.venue.stats().filled_qty == 4);
  }
  {  // queue penalty: pretend 50% more of the level was ahead; only trades work it off
    H h(50);
    h.add(Side::Buy, 1, 100, 10);
    h.add(Side::Buy, 2, 100, 6);
    h.send(900, Side::Buy, 100, 4);  // level 16 -> 8 phantom shares ahead of us
    CHECK(h.ahead(900) == 16);
    h.del(2);
    h.exec(1, 10);
    CHECK(h.ahead(900) == 0);
    h.add(Side::Buy, 3, 100, 20);
    h.exec(3, 5);  // 5 behind us traded: the phantom queue takes all 5
    CHECK(h.out.count(ReportType::Fill) == 0);
    h.exec(3, 6);  // 3 more clear the phantom queue, 3 reach us
    CHECK(h.out.count(ReportType::Fill) == 1 && h.out.last(ReportType::Fill)->qty == 3 &&
          h.out.last(ReportType::Fill)->leaves == 1);
    h.exec(3, 9);  // no phantom left: the rest of us
    CHECK(h.out.count(ReportType::Fill) == 2 && h.out.last(ReportType::Fill)->qty == 1 && h.venue.live_orders() == 0);
    CHECK(h.venue.stats().queue_inconsistent == 0);
  }
  {  // ahead never goes negative; an exec at another price level does not touch us
    H h;
    h.add(Side::Buy, 1, 100, 3);
    h.add(Side::Buy, 2, 99, 8);
    h.send(900, Side::Buy, 100, 4);
    h.exec(1, 3);
    h.exec(2, 2);  // a print at a worse price than ours: whoever sold passed through us, at least 2 of it
    CHECK(h.out.count(ReportType::Fill) == 1 && h.out.last(ReportType::Fill)->qty == 2 &&
          h.out.last(ReportType::Fill)->leaves == 2 && h.venue.stats().trade_throughs == 1);
    h.exec(2, 5);  // more prints at the worse price: the rest of us
    CHECK(h.out.count(ReportType::Fill) == 2 && h.out.last(ReportType::Fill)->qty == 2 && h.venue.live_orders() == 0);
  }
  {  // sell side mirrors: a print at a HIGHER price than our ask is a trade-through
    H h;
    h.add(Side::Sell, 1, 105, 5);
    h.add(Side::Sell, 2, 106, 5);
    h.add(Side::Buy, 3, 100, 5);
    h.send(901, Side::Sell, 105, 3);
    CHECK(h.ahead(901) == 5);
    h.exec(2, 4);
    CHECK(h.out.count(ReportType::Fill) == 1 && h.out.last(ReportType::Fill)->qty == 3 &&
          h.out.last(ReportType::Fill)->side == Side::Sell);
  }
  {  // an empty level: we are first in line
    H h;
    h.add(Side::Buy, 1, 100, 5);
    h.add(Side::Sell, 2, 103, 5);
    h.send(900, Side::Buy, 101, 2);
    CHECK(h.ahead(900) == 0);
    h.add(Side::Buy, 3, 101, 4);  // joins behind us
    h.exec(3, 1);
    CHECK(h.out.count(ReportType::Fill) == 1 && h.out.last(ReportType::Fill)->qty == 1);
  }
  {  // two orders of ours on one level fill in arrival order
    H h;
    h.add(Side::Buy, 1, 100, 2);
    h.add(Side::Sell, 9, 104, 2);
    h.send(900, Side::Buy, 100, 3);
    h.send(901, Side::Buy, 100, 3);
    CHECK(h.ahead(900) == 2 && h.ahead(901) == 2);  // snapshot of the tape level at each arrival
    h.exec(1, 2);
    h.add(Side::Buy, 2, 100, 10);
    h.exec(2, 4);  // 3 to the first, 1 to the second
    CHECK(h.out.count(ReportType::Fill) == 2);
    CHECK(h.out.r[h.out.r.size() - 2].id == 900 && h.out.r[h.out.r.size() - 2].qty == 3);
    CHECK(h.out.last(ReportType::Fill)->id == 901 && h.out.last(ReportType::Fill)->qty == 1 &&
          h.leaves(901) == 2);
  }
}

void test_orders_and_rejects() {
  H h;
  h.add(Side::Buy, 1, 100, 5);
  h.add(Side::Sell, 2, 103, 5);
  h.send(900, Side::Buy, 103, 1);  // would take
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::WouldCross && h.venue.stats().wouldcross == 1);
  h.send(900, Side::Sell, 100, 1);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::WouldCross);
  h.send(900, Side::Buy, 101, 0);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::BadQty);
  h.send(0, Side::Buy, 101, 1);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::BadId);
  h.send(900, Side::Buy, 101, 1, sim::OrderType::Market);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::BadPrice);
  h.send(900, Side::Buy, 101, 1, sim::OrderType::IOC);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::BadPrice);
  h.venue.on_new(sim::NewOrder{900, 1, Side::Buy, sim::OrderType::Limit, Price::from_raw(kBase + 50), 1}, h.ts++, h.out);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::BadPrice);  // off the tick grid
  CHECK(h.venue.live_orders() == 0);
  h.send(900, Side::Buy, 101, 3, sim::OrderType::PostOnly);
  CHECK(h.venue.live_orders() == 1);
  h.send(900, Side::Buy, 101, 3);
  CHECK(h.out.last(ReportType::Rejected)->reason == Reason::Duplicate);
  // cancel
  h.venue.on_cancel(900, h.ts++, h.out);
  CHECK(h.out.last(ReportType::Canceled)->qty == 3 && h.out.last(ReportType::Canceled)->leaves == 0 &&
        h.venue.live_orders() == 0);
  h.venue.on_cancel(900, h.ts++, h.out);  // again: it is gone
  CHECK(h.out.last(ReportType::CancelRejected)->reason == Reason::UnknownOrder);
  // a cancel that loses the race with a fill
  h.send(901, Side::Buy, 100, 1);
  h.add(Side::Buy, 3, 100, 4);
  h.exec(1, 5);  // order 1 ahead of us
  h.exec(3, 1);  // behind: fills us
  h.venue.on_cancel(901, h.ts++, h.out);
  CHECK(h.out.last(ReportType::CancelRejected) != nullptr && h.venue.stats().cancel_rejected == 2);
}

void test_replace_semantics() {
  H h;
  h.add(Side::Buy, 1, 100, 10);
  h.add(Side::Sell, 2, 105, 5);
  h.send(900, Side::Buy, 100, 8);
  h.add(Side::Buy, 3, 100, 6);
  CHECK(h.ahead(900) == 10);
  h.venue.on_replace(900, P(100), 5, h.ts++, h.out);  // shrink in place: keeps its place
  CHECK(h.out.last(ReportType::Replaced)->leaves == 5 && h.ahead(900) == 10 && h.leaves(900) == 5);
  h.venue.on_replace(900, P(100), 5, h.ts++, h.out);  // same size, same price: nothing to lose
  CHECK(h.ahead(900) == 10 && h.out.count(ReportType::Replaced) == 2);
  h.venue.on_replace(900, P(100), 7, h.ts++, h.out);  // grow: back of the queue behind order 3 too
  CHECK(h.ahead(900) == 16 && h.leaves(900) == 7);
  h.venue.on_replace(900, P(101), 7, h.ts++, h.out);  // new price, empty level
  CHECK(h.ahead(900) == 0 && h.out.last(ReportType::Replaced)->price == P(101));
  h.venue.on_replace(900, P(105), 7, h.ts++, h.out);  // would take
  CHECK(h.out.last(ReportType::ReplaceRejected)->reason == Reason::WouldCross && h.out.count(ReportType::Replaced) == 4);
  h.venue.on_replace(900, P(101), 0, h.ts++, h.out);
  CHECK(h.out.last(ReportType::ReplaceRejected)->reason == Reason::BadQty);
  h.venue.on_replace(777, P(101), 3, h.ts++, h.out);
  CHECK(h.out.last(ReportType::ReplaceRejected)->reason == Reason::UnknownOrder);
  CHECK(h.venue.live_orders() == 1 && h.leaves(900) == 7);
  CHECK(h.venue.stats().quotes == 3);  // accepted, then grow and move lost priority (the shrink kept it)
}

// ---- tape rebuild ----------------------------------------------------------------------

struct EngRec {
  std::vector<PublicEvent>* tape = nullptr;
  std::uint64_t first_fill_ts = 0;
  Qty first_fill_qty = 0;
  bool filled = false;
  OrderId watch = 0;
  void on_report(const Report& r) noexcept {
    if (!filled && r.type == ReportType::Fill && r.id == watch) {
      filled = true;
      first_fill_ts = r.ts;
      first_fill_qty = r.qty;
    }
  }
  void on_public(const PublicEvent& e) noexcept {
    if (tape) tape->push_back(e);
  }
};

bool same_events(const std::vector<PublicEvent>& a, const std::vector<PublicEvent>& b) {  // field-wise: padding is not defined
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const PublicEvent &x = a[i], &y = b[i];
    if (x.type != y.type || x.side != y.side || x.id != y.id || x.price != y.price || x.qty != y.qty ||
        x.seq != y.seq || x.ts != y.ts)
      return false;
  }
  return true;
}

sim::EngineConfig engine_cfg() {
  sim::EngineConfig ec;
  ec.base = Price::from_raw(kBase);
  ec.tick_raw = kTick;
  ec.num_levels = kLevels;
  ec.max_orders = 1u << 16;
  return ec;
}

void apply_op(sim::MatchingEngine<EngRec>& e, const ScriptOp& op) {
  if (op.is_cancel) e.cancel(op.id, op.owner, op.ts);
  else e.new_order(op.o, op.ts);
}

MarketConfig mkt(std::uint64_t requests, std::uint64_t seed) {
  MarketConfig c;
  c.base = Price::from_raw(kBase);
  c.tick_raw = kTick;
  c.num_levels = kLevels;
  c.requests = requests;
  c.seed = seed;
  return c;
}

void test_tape_and_generator(std::uint64_t n) {
  const Market a = MarketGen::generate(mkt(n, 5));
  const Market b = MarketGen::generate(mkt(n, 5));
  const Market c = MarketGen::generate(mkt(n, 6));
  CHECK(a.ops.size() == b.ops.size() && same_events(a.tape, b.tape));  // same seed, same tape
  CHECK(!same_events(a.tape, c.tape));
  // tape times never go backwards
  for (std::size_t i = 1; i < a.tape.size(); ++i) CHECK(a.tape[i].ts >= a.tape[i - 1].ts);

  // replaying the script through a fresh engine gives the same tape; TapeBook ends in the same book
  core::Region region = make_region(64);
  core::Arena arena(region);
  std::vector<PublicEvent> again;
  EngRec rec;
  rec.tape = &again;
  sim::MatchingEngine<EngRec> eng;
  CHECK(eng.init(arena, engine_cfg(), rec));
  for (const ScriptOp& op : a.ops) apply_op(eng, op);
  CHECK(same_events(again, a.tape));
  TapeBook tb;
  CHECK(tb.init(arena, book_cfg(1u << 16)));
  for (const PublicEvent& e : a.tape) CHECK(tb.apply(e));
  CHECK(tb.errors() == 0 && tb.check_invariants() && tb.order_count() == eng.order_count());
  CHECK(tb.digest() == eng.digest());
  // a stream that does not fit the book is counted, not applied
  CHECK(!tb.apply(PublicEvent{PublicType::Exec, Side::Buy, 987654321, P(100), 1, 1, 1}) && tb.errors() == 1);
  CHECK(!tb.apply(PublicEvent{PublicType::Delete, Side::Buy, 987654321, P(100), 1, 1, 1}) && tb.errors() == 2);
  std::printf("  tape: %zu requests -> %zu public events; %llu latent jumps, mean spread %.2f ticks\n", a.ops.size(),
              a.tape.size(), (unsigned long long)a.stats.jumps, a.stats.mean_spread_ticks);
}

// ---- the queue simulation against the real matching engine -------------------------------
//
// Inject one passive order at a random moment into (a) the real engine, replaying the same
// script of other participants, and (b) the tape-based simulation. Until our order first
// trades, the two worlds are identical, so the first fill is directly comparable.
// The simulation may miss a fill (a sweep that ends on us with nothing behind to show it on
// the tape); it must never fill earlier than the engine, and never more than the engine did.

void test_queue_vs_engine(std::uint64_t trials) {
  const Market m = MarketGen::generate(mkt(40'000, 77));
  const std::size_t nops = m.ops.size();
  std::vector<std::size_t> tape_pos(nops);  // first tape index with ts >= ops[k].ts
  for (std::size_t k = 0; k < nops; ++k) {
    tape_pos[k] = static_cast<std::size_t>(
        std::lower_bound(m.tape.begin(), m.tape.end(), m.ops[k].ts,
                         [](const PublicEvent& e, std::uint64_t t) { return e.ts < t; }) -
        m.tape.begin());
  }
  Rng rng(4242);
  core::Region region = make_region(64);
  std::uint64_t eng_fills = 0, exact_fills = 0, same_time = 0, same_qty = 0, cons_fills = 0, runs = 0;
  for (std::uint64_t t = 0; t < trials; ++t) {
    const std::size_t k = 300 + rng.below(static_cast<std::uint32_t>(nops - 300 - 7000));
    const std::uint64_t T = m.ops[k].ts;
    const std::size_t horizon_op = k + 6000;
    const std::uint64_t TL = m.ops[horizon_op].ts;
    core::Arena arena(region);

    // simulation world, state at time T
    TapeBook tb;
    CHECK(tb.init(arena, book_cfg(1u << 16)));
    for (std::size_t i = 0; i < tape_pos[k]; ++i) CHECK(tb.apply(m.tape[i]));
    if (!tb.two_sided()) continue;
    const std::int64_t bb = tb.tick_index(tb.best_bid()), ba = tb.tick_index(tb.best_ask());
    const bool buy = rng.below(2) == 0;
    std::int64_t lvl;
    const std::uint32_t pick = rng.below(4);
    if (buy) lvl = (pick == 3 && ba - bb >= 2) ? bb + 1 : bb - static_cast<std::int64_t>(pick % 3);
    else lvl = (pick == 3 && ba - bb >= 2) ? ba - 1 : ba + static_cast<std::int64_t>(pick % 3);
    const Qty q = 1 + static_cast<Qty>(rng.below(8));
    const OrderId ours = 1ull << 40;
    const sim::NewOrder no{ours, 1, buy ? Side::Buy : Side::Sell, sim::OrderType::Limit, P(lvl), q};

    VirtualVenue ve, vc;
    ve.init(&tb, 0);
    vc.init(&tb, 50);
    Out oe, oc;
    ve.on_new(no, T, oe);
    vc.on_new(no, T, oc);
    CHECK(oe.count(ReportType::Accepted) == 1);
    for (std::size_t i = tape_pos[k]; i < m.tape.size() && m.tape[i].ts <= TL; ++i) {
      ve.on_tape(m.tape[i], oe);
      vc.on_tape(m.tape[i], oc);
      CHECK(tb.apply(m.tape[i]));
      if (oe.count(ReportType::Fill) && oc.count(ReportType::Fill)) break;
      if (oe.count(ReportType::Fill) && ve.live_orders() == 0 && vc.live_orders() == 0) break;
    }
    CHECK(ve.stats().queue_inconsistent == 0);  // arrival order and queue quantity told the same story
    const Report* se = nullptr;
    for (const Report& r : oe.r)
      if (r.type == ReportType::Fill) {
        se = &r;
        break;
      }
    const Report* sc = nullptr;
    for (const Report& r : oc.r)
      if (r.type == ReportType::Fill) {
        sc = &r;
        break;
      }

    // real engine world
    core::Arena arena2(region);  // restart the arena: the sim world is no longer needed
    EngRec rec;
    rec.watch = ours;
    sim::MatchingEngine<EngRec> eng;
    CHECK(eng.init(arena2, engine_cfg(), rec));
    for (std::size_t i = 0; i < k; ++i) apply_op(eng, m.ops[i]);
    eng.new_order(no, T);
    for (std::size_t i = k; i <= horizon_op && !rec.filled; ++i) apply_op(eng, m.ops[i]);

    ++runs;
    if (rec.filled) ++eng_fills;
    if (se) {
      ++exact_fills;
      CHECK(rec.filled);                          // never a fill the engine did not give
      CHECK(rec.first_fill_ts <= se->ts);         // never earlier than the engine
      if (rec.filled && rec.first_fill_ts == se->ts) {
        ++same_time;
        CHECK(se->qty <= rec.first_fill_qty);     // never more than the engine gave
        if (se->qty == rec.first_fill_qty) ++same_qty;
      }
    }
    if (sc) {
      ++cons_fills;
      CHECK(se != nullptr && se->ts <= sc->ts);   // a deeper queue is never faster
    }
  }
  std::printf("  queue vs engine: %llu injections; engine filled %llu, exact sim %llu (same moment %llu, same size %llu), penalty-50 sim %llu\n",
              (unsigned long long)runs, (unsigned long long)eng_fills, (unsigned long long)exact_fills,
              (unsigned long long)same_time, (unsigned long long)same_qty, (unsigned long long)cons_fills);
  CHECK(runs > trials / 2 && eng_fills > runs / 4);
  CHECK(exact_fills * 10 >= eng_fills * 7);   // misses at most 30% of the engine's fills
  CHECK(same_time * 10 >= exact_fills * 9);   // and when it fills it is on time
  CHECK(cons_fills <= exact_fills);
}

// ---- accounting by hand ----------------------------------------------------------------

void test_metrics_by_hand() {
  core::Region region = make_region(4);
  core::Arena arena(region);
  Metrics m;
  MetricsConfig mc;
  mc.tick_raw = 100;
  mc.fee_raw_per_unit = 0;
  mc.bin_ns = 1000;
  mc.horizons_ns[0] = 2000;
  mc.horizons_ns[1] = 10000;
  mc.horizons_ns[2] = 50000;
  CHECK(m.init(arena, mc));
  const std::int64_t mid = 100000;  // raw; mid2 = 200000
  m.before_event(0);
  m.on_mid(true, 2 * mid);
  m.before_event(1000);
  m.on_fill(1000, +1, mid - 100, 10);  // bought 10, one tick under the mid
  CHECK(m.edge2() == 2000 && m.pos() == 10 && m.inventory2() == 0);
  CHECK(std::fabs(m.effective_edge_ticks() - 1.0) < 1e-9);
  m.before_event(5000);  // 2000 ns after the fill: horizon 0 is read at the mid still in force
  m.on_mid(true, 2 * mid + 400);  // mid +2 ticks
  CHECK(m.inventory2() == 4000);
  CHECK(std::fabs(m.realized_ticks(0) - 1.0) < 1e-9 && std::fabs(m.adverse_ticks(0)) < 1e-9);
  m.before_event(20000);  // horizon 1 (10000 ns) reads the new mid
  CHECK(std::fabs(m.realized_ticks(1) - 3.0) < 1e-9 && std::fabs(m.adverse_ticks(1) - 2.0) < 1e-9);
  m.finish(100000);
  CHECK(std::fabs(m.realized_ticks(2) - 3.0) < 1e-9 && std::fabs(m.adverse_ticks(2) - 2.0) < 1e-9);
  CHECK(m.value2() == m.edge2() + m.inventory2() + m.fees2() && m.value2() == 6000);

  // a sell that the market then moves against (mid rises): adverse selection is negative
  Metrics s;
  core::Arena arena2(region);
  CHECK(s.init(arena2, mc));
  s.before_event(0);
  s.on_mid(true, 2 * mid);
  s.before_event(1000);
  s.on_fill(1000, -1, mid + 100, 4);  // sold 4, one tick over the mid
  s.before_event(3000);
  s.on_mid(true, 2 * mid + 200);  // mid +1 tick against the short
  s.finish(60000);
  CHECK(std::fabs(s.effective_edge_ticks() - 1.0) < 1e-9);
  CHECK(std::fabs(s.adverse_ticks(2) + 1.0) < 1e-9 && std::fabs(s.realized_ticks(2) - 0.0) < 1e-9);
  CHECK(s.inventory2() == -4 * 200 && s.value2() == s.edge2() + s.inventory2());
  // fees and rebates enter the identity
  Metrics f;
  core::Arena arena3(region);
  mc.fee_raw_per_unit = -2;  // rebate of 2 raw per share
  CHECK(f.init(arena3, mc));
  f.before_event(0);
  f.on_mid(true, 2 * mid);
  f.on_fill(10, +1, mid - 100, 5);
  CHECK(f.fees2() == 2 * 2 * 5 && f.value2() == f.edge2() + f.inventory2() + f.fees2());
  // PnL samples: constant PnL gives zero variance and no Sharpe
  CHECK(s.sharpe_per_bin() == s.sharpe_per_bin());  // not NaN
}

// ---- whole backtests ---------------------------------------------------------------------

risk::RiskConfig bt_risk() {
  risk::RiskConfig r;
  r.max_order_qty = 100;
  r.max_order_notional_raw = 100'000'000'000LL;
  r.price_band_bps = 2000;
  r.max_position = 80;
  r.max_open_orders = 10;
  r.rate_per_sec = 1'000'000;
  r.burst = 50;
  r.max_loss_raw = 0;
  return r;
}

BtConfig bt_cfg() {
  BtConfig c;
  c.risk = bt_risk();
  c.tick_raw = kTick;
  return c;
}

struct RunResult {
  BtReport rep;
  std::int64_t oms_pos = 0;
  core::i128 oms_cash = 0;
  std::uint64_t oms_fills = 0;
  std::uint32_t oms_open = 0, venue_live = 0;
  std::uint64_t kill_cancels = 0;
  bool killed = false;
  std::uint64_t venue_filled_qty = 0, venue_fills = 0;
  core::i128 value2 = 0, edge2 = 0, inv2 = 0, fees2 = 0;
  std::int64_t mid2 = 0;
  bool oms_ok = false;
  std::uint64_t exposure_ok = 0;
};

RunResult run_bt(const Market& m, const BtConfig& c, const strat::MmConfig& sc, bool trap = false) {
  core::Region region = make_region(96);
  core::Arena arena(region);
  strat::MarketMaker mm(sc);
  Backtester<strat::MarketMaker>* bt = new Backtester<strat::MarketMaker>();
  CHECK(bt->init(arena, m.tape.data(), m.tape.size(), book_cfg(), c, &mm));
  if (trap) alloc_trap::arm();
  bt->run();
  if (trap) CHECK(alloc_trap::disarm() == 0);
  RunResult r;
  r.rep = bt->report();
  const oms::Oms& o = bt->gateway().oms();
  r.oms_pos = o.position().pos;
  r.oms_cash = o.position().cash;
  r.oms_fills = o.position().fills;
  r.oms_open = o.open_orders();
  r.venue_live = bt->venue().live_orders();
  r.kill_cancels = bt->gateway().stats().kill_cancels;
  r.killed = bt->gateway().risk().killed();
  r.venue_filled_qty = bt->venue().stats().filled_qty;
  r.venue_fills = bt->venue().stats().fills;
  r.value2 = bt->metrics().value2();
  r.edge2 = bt->metrics().edge2();
  r.inv2 = bt->metrics().inventory2();
  r.fees2 = bt->metrics().fees2();
  r.mid2 = bt->metrics().mid2();
  r.oms_ok = o.check_invariants() && o.stats().anomalies == 0 && o.stats().mismatches == 0 &&
             o.stats().orphan_fills == 0 && o.stats().overfills == 0 && o.stats().unknown_reports == 0 &&
             bt->gateway().stats().seq_gaps == 0 && bt->gateway().stats().bad_messages == 0;
  delete bt;
  return r;
}

void check_health(const RunResult& r, const BtConfig& c) {
  CHECK(r.value2 == r.edge2 + r.inv2 + r.fees2);                       // the attribution is exact
  CHECK(r.oms_pos == r.rep.final_pos);                                 // the OMS agrees with the venue once drained
  CHECK(r.oms_fills == r.rep.fills && r.venue_fills == r.rep.fills);
  CHECK(static_cast<std::int64_t>(r.venue_filled_qty) == r.rep.filled_qty);
  // OMS cash has no fees; metrics cash does: 2*cash_oms + pos*mid2 + fees2 == value2
  CHECK(2 * r.oms_cash + static_cast<core::i128>(r.oms_pos) * r.mid2 + r.fees2 == r.value2);
  CHECK(r.oms_ok);
  CHECK(r.rep.queue_inconsistent == 0 || c.queue_penalty_pct != 0);
  CHECK(r.rep.oms_anomalies == 0 && r.rep.oms_mismatches == 0);
  CHECK(r.rep.queue_overflows == 0 && r.rep.tape_errors == 0 && r.rep.dropped_samples == 0);
  CHECK(r.rep.max_abs_pos <= c.risk.max_position);                     // the risk limit held, in flight and all
  CHECK(r.oms_open == r.venue_live);                                   // what we think is working is what works
  CHECK(r.rep.fills == 0 || r.rep.filled_qty >= static_cast<std::int64_t>(r.rep.fills));
  CHECK(r.rep.fill_rate_quotes >= 0 && r.rep.fill_rate_quotes <= 1 && r.rep.fill_rate_qty >= 0 &&
        r.rep.fill_rate_qty <= 1.0000001);
}

// A message that reaches the venue at the same instant as a tape event is handled after it.
struct TieStub {
  bool sent = false;
  template <class GW>
  void on_update(std::uint64_t now, const TapeBook& book, GW& gw) {
    if (sent || !book.two_sided()) return;
    sent = true;
    gw.risk().set_reference(P(101));
    gw.new_order(Side::Buy, sim::OrderType::PostOnly, P(102), 1, now);
  }
};

void test_tie_order() {
  const std::vector<PublicEvent> tape = {
      {PublicType::Add, Side::Buy, 1, P(100), 5, 1, 5},
      {PublicType::Add, Side::Sell, 2, P(104), 5, 2, 10},   // the stub sees a two-sided book here, at t = 10
      {PublicType::Add, Side::Sell, 3, P(102), 5, 3, 110},  // lands exactly when its order does
  };
  core::Region region = make_region(32);
  core::Arena arena(region);
  BtConfig c = bt_cfg();
  c.ingress_ns = 100;
  c.risk.price_band_bps = 2000;
  TieStub stub;
  Backtester<TieStub>* b = new Backtester<TieStub>();
  CHECK(b->init(arena, tape.data(), tape.size(), book_cfg(), c, &stub));
  b->run();
  CHECK(stub.sent && b->venue().stats().wouldcross == 1 && b->venue().stats().accepted == 0);  // tape first
  CHECK(b->gateway().oms().stats().anomalies == 0 && b->gateway().oms().open_orders() == 0);
  delete b;
}

constexpr std::uint64_t kGolden = 0x955e301bb0a9a585ULL;

// A fixed run pinned to its digest: any change in simulation semantics changes it.
void test_golden_digest() {
  const Market m = MarketGen::generate(mkt(20000, 31));
  BtConfig c = bt_cfg();
  c.md_ns = 12'000;
  c.ingress_ns = 20'000;
  c.egress_ns = 15'000;
  c.jitter_ns = 8'000;
  c.seed = 5;
  const RunResult r = run_bt(m, c, strat::MmConfig{});
  CHECK(r.rep.fills > 50);
  if (r.rep.digest != kGolden) std::printf("golden digest is now %016llx\n", (unsigned long long)r.rep.digest);
  CHECK(r.rep.digest == kGolden);
}

void test_attribution_and_health(std::uint64_t n) {
  Rng rng(99);
  std::uint64_t total_fills = 0, runs = 0;
  for (int i = 0; i < 12; ++i) {
    const Market m = MarketGen::generate(mkt(n, 100 + static_cast<std::uint64_t>(i)));
    BtConfig c = bt_cfg();
    c.md_ns = rng.below(3) == 0 ? 0 : rng.below(60'000);
    c.ingress_ns = rng.below(3) == 0 ? 0 : rng.below(60'000);
    c.egress_ns = rng.below(3) == 0 ? 0 : rng.below(60'000);
    c.jitter_ns = rng.below(2) ? rng.below(20'000) : 0;
    c.seed = 7 + static_cast<std::uint64_t>(i);
    c.queue_penalty_pct = rng.below(2) ? 0 : 10 + rng.below(90);
    c.fee_raw_per_unit = static_cast<std::int64_t>(rng.below(7)) - 3;  // fees and rebates
    strat::MmConfig s;
    s.half_spread16 = 8 + static_cast<std::int32_t>(rng.below(33));
    s.alpha16 = static_cast<std::int32_t>(rng.below(33));
    s.skew16 = static_cast<std::int32_t>(rng.below(7));
    s.qty = 1 + static_cast<std::int64_t>(rng.below(10));
    s.max_pos = 10 + static_cast<std::int64_t>(rng.below(60));
    s.requote_ticks = 1 + static_cast<std::int32_t>(rng.below(3));
    s.min_interval_ns = rng.below(3) == 0 ? 0 : rng.below(150'000);
    const RunResult r = run_bt(m, c, s);
    check_health(r, c);
    CHECK(r.rep.max_abs_pos <= s.max_pos);
    total_fills += r.rep.fills;
    ++runs;
  }
  std::printf("  attribution identity and OMS/venue agreement held on %llu random configs (%llu fills)\n",
              (unsigned long long)runs, (unsigned long long)total_fills);
  CHECK(total_fills > 1000);
}

void test_determinism(std::uint64_t n) {
  const Market m = MarketGen::generate(mkt(n, 31));
  BtConfig c = bt_cfg();
  c.md_ns = 12'000;
  c.ingress_ns = 20'000;
  c.egress_ns = 15'000;
  c.jitter_ns = 8'000;
  c.seed = 5;
  strat::MmConfig s;
  const RunResult a = run_bt(m, c, s), b = run_bt(m, c, s);
  CHECK(a.rep.fills > 0);
  CHECK(std::memcmp(&a.rep, &b.rep, sizeof(BtReport)) == 0);  // every field, every double, identical
  CHECK(a.rep.digest == b.rep.digest && a.value2 == b.value2 && a.rep.queue_inconsistent == 0);
  BtConfig c2 = c;
  c2.seed = 6;  // different jitter draw
  const RunResult d = run_bt(m, c2, s);
  CHECK(d.rep.digest != a.rep.digest);
  const Market m2 = MarketGen::generate(mkt(n, 32));  // different market
  CHECK(run_bt(m2, c, s).rep.digest != a.rep.digest);
  // zero latency: jitter has nothing to act on only if jitter is zero; with it, order still deterministic
  BtConfig z = bt_cfg();
  CHECK(run_bt(m, z, s).rep.digest == run_bt(m, z, s).rep.digest);
  std::printf("  deterministic replay: identical report bytes and digest %016llx over two runs\n",
              (unsigned long long)a.rep.digest);
}

// The synthetic market has a real, measurable signal; the strategy uses it; latency eats it.
void test_signal_and_decay(std::uint64_t n) {
  // 1. the generator: top-of-book imbalance predicts the direction of the next mid move
  {
    const Market m = MarketGen::generate(mkt(n, 1));
    core::Region region = make_region(64);
    core::Arena arena(region);
    TapeBook tb;
    CHECK(tb.init(arena, book_cfg(1u << 16)));
    std::uint64_t hit = 0, tot = 0;
    double prev_imb = 0, prev_mid = 0;
    bool have = false;
    for (const PublicEvent& e : m.tape) {
      CHECK(tb.apply(e));
      if (!tb.two_sided()) continue;
      const double mid = static_cast<double>(tb.mid2());
      if (have && mid != prev_mid) {  // the mid moved: was the imbalance before it pointing that way?
        if (prev_imb > 0.3 || prev_imb < -0.3) {
          ++tot;
          hit += (mid > prev_mid) == (prev_imb > 0);
        }
      }
      if (!have || mid != prev_mid) {
        // remember the imbalance as it stands right after a move; the next move is what it predicts
        const double qb = static_cast<double>(tb.depth(Side::Buy, 1)), qa = static_cast<double>(tb.depth(Side::Sell, 1));
        prev_imb = (qb + qa) > 0 ? (qb - qa) / (qb + qa) : 0;
        prev_mid = mid;
        have = true;
      } else {
        const double qb = static_cast<double>(tb.depth(Side::Buy, 1)), qa = static_cast<double>(tb.depth(Side::Sell, 1));
        prev_imb = (qb + qa) > 0 ? (qb - qa) / (qb + qa) : 0;
      }
    }
    const double rate = tot ? static_cast<double>(hit) / static_cast<double>(tot) : 0;
    std::printf("  generator: imbalance points the right way for the next mid move %.1f%% of %llu signals\n", 100 * rate,
                (unsigned long long)tot);
    CHECK(tot > 300 && rate > 0.6);
  }
  // 2. the strategy: with the lean on it beats the same strategy without it; and latency costs it
  double pnl_on = 0, pnl_off = 0, pnl_slow = 0;
  int wins = 0, decays = 0;
  const int kSeeds = 5;
  for (int i = 0; i < kSeeds; ++i) {
    const Market m = MarketGen::generate(mkt(n, 200 + static_cast<std::uint64_t>(i)));
    BtConfig c = bt_cfg();
    strat::MmConfig on, off;
    off.alpha16 = 0;
    const double a = run_bt(m, c, on).rep.pnl, b = run_bt(m, c, off).rep.pnl;
    BtConfig slow = c;
    slow.md_ns = slow.ingress_ns = slow.egress_ns = 100'000;
    const double s = run_bt(m, slow, on).rep.pnl;
    pnl_on += a;
    pnl_off += b;
    pnl_slow += s;
    wins += a > b;
    decays += s < a;
  }
  std::printf("  signal ablation over %d markets: PnL with imbalance lean %+.2f, without %+.2f; with 100 us each way %+.2f\n",
              kSeeds, pnl_on, pnl_off, pnl_slow);
  CHECK(pnl_on > pnl_off && wins >= 4);
  CHECK(pnl_on > pnl_slow && decays >= 4);
}

// The strategy clamps its quotes so a PostOnly never takes; with no latency almost none cross.
void test_quotes_do_not_cross(std::uint64_t n) {
  std::uint64_t sent = 0, crossed = 0;
  for (int i = 0; i < 3; ++i) {
    const Market m = MarketGen::generate(mkt(n, 300 + static_cast<std::uint64_t>(i)));
    strat::MmConfig s;
    s.half_spread16 = 4;  // tight and heavily leaned: the raw target often sits past the touch
    s.alpha16 = 32;
    s.skew16 = 0;
    s.min_interval_ns = 0;
    const RunResult r = run_bt(m, bt_cfg(), s);
    sent += r.rep.sent_new + r.rep.sent_replace;
    crossed += r.rep.post_only_rejects;
  }
  std::printf("  quote discipline: %llu of %llu requests crossed the touch at the venue\n", (unsigned long long)crossed,
              (unsigned long long)sent);
  CHECK(sent > 1000 && crossed * 50 <= sent);
}

// The strategy's own cap is tighter than the risk limit and holds on its own.
void test_position_cap(std::uint64_t n) {
  const Market m = MarketGen::generate(mkt(n, 77));
  BtConfig c = bt_cfg();
  c.md_ns = c.ingress_ns = c.egress_ns = 30'000;  // latency makes inventory build up
  strat::MmConfig s;
  s.alpha16 = 0;
  s.skew16 = 0;  // no skew, so only the cap limits inventory
  s.qty = 5;
  s.max_pos = 1000;
  const RunResult free_run = run_bt(m, c, s);
  CHECK(free_run.rep.max_abs_pos > 10);
  s.max_pos = 10;
  const RunResult capped = run_bt(m, c, s);
  std::printf("  position cap: max |pos| %lld uncapped, %lld with the strategy's cap at 10 (risk limit %lld)\n",
              (long long)free_run.rep.max_abs_pos, (long long)capped.rep.max_abs_pos, (long long)c.risk.max_position);
  CHECK(capped.rep.max_abs_pos <= 10);
  check_health(capped, c);
}

void test_loss_limit_kills(std::uint64_t n) {
  const Market m = MarketGen::generate(mkt(n, 55));
  BtConfig c = bt_cfg();
  c.md_ns = c.ingress_ns = c.egress_ns = 100'000;
  strat::MmConfig s;
  s.alpha16 = -32;         // leans the wrong way on purpose: this configuration bleeds from adverse selection
  s.max_pos = 60;
  const RunResult free_run = run_bt(m, c, s);
  CHECK(!free_run.killed);
  c.risk.max_loss_raw = 3'000;  // 0.30 currency units
  const RunResult r = run_bt(m, c, s);
  CHECK(r.killed && r.rep.kill_reason == static_cast<std::uint64_t>(risk::KillReason::MaxLoss));
  CHECK(r.oms_open == 0 && r.venue_live == 0 && r.kill_cancels > 0);  // everything swept and acknowledged
  CHECK(r.rep.sent_new < free_run.rep.sent_new);                          // trading stopped early
  check_health(r, c);
  std::printf("  loss limit: tripped after %llu orders (unconstrained run sent %llu); all quotes swept, position %+lld left to unwind\n",
              (unsigned long long)r.rep.sent_new, (unsigned long long)free_run.rep.sent_new, (long long)r.rep.final_pos);
}

void test_no_allocation(std::uint64_t n) {
  const Market m = MarketGen::generate(mkt(n, 9));
  BtConfig c = bt_cfg();
  c.md_ns = 10'000;
  c.ingress_ns = 20'000;
  c.egress_ns = 20'000;
  c.jitter_ns = 5'000;
  const RunResult r = run_bt(m, c, strat::MmConfig{}, /*trap=*/true);
  CHECK(r.rep.fills > 0);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000ULL;
  test_queue_basics();
  test_orders_and_rejects();
  test_replace_semantics();
  test_tape_and_generator(std::min<std::uint64_t>(n, 60000));
  test_queue_vs_engine(std::max<std::uint64_t>(n / 400, 20));
  test_attribution_and_health(std::max<std::uint64_t>(n / 2, 20000));
  test_metrics_by_hand();
  test_tie_order();
  test_golden_digest();
  test_determinism(std::max<std::uint64_t>(n / 2, 20000));
  test_signal_and_decay(std::max<std::uint64_t>(n, 50000));
  test_position_cap(std::max<std::uint64_t>(n / 2, 20000));
  test_quotes_do_not_cross(std::max<std::uint64_t>(n / 2, 20000));
  test_loss_limit_kills(std::max<std::uint64_t>(n, 50000));
  test_no_allocation(std::max<std::uint64_t>(n / 2, 20000));
  if (g_failures == 0) std::puts("test_bt: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
