// Phase 6 gate report: one deterministic backtest of the market maker on the
// synthetic informed-flow market, with PnL, Sharpe, fill rate and adverse
// selection, then the studies that make the numbers mean something:
//   - same run twice: identical digest (deterministic replay);
//   - imbalance lean on and off (is the signal doing the work?);
//   - latency sweep (how fast does the edge decay?);
//   - queue model sensitivity (exact L3 queue vs ignoring cancels ahead).
// The market is synthetic by construction: the numbers show the machinery and
// the shape of the effects, not returns anyone could earn.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hft/bt/backtest.hpp"
#include "hft/bt/market_gen.hpp"
#include "hft/strat/market_maker.hpp"

using namespace hft;

namespace {

book::BookConfig book_cfg(const bt::MarketConfig& m) {
  book::BookConfig b;
  b.base = m.base;
  b.tick_raw = m.tick_raw;
  b.num_levels = m.num_levels;
  return b;
}

risk::RiskConfig risk_cfg() {
  risk::RiskConfig r;
  r.max_order_qty = 100;
  r.max_order_notional_raw = 100'000'000'000LL;
  r.price_band_bps = 2000;
  r.max_position = 80;
  r.max_open_orders = 10;
  r.rate_per_sec = 1'000'000;
  r.burst = 50;
  return r;
}

struct Result {
  bt::BtReport rep;
  double seconds = 0;
};

Result run(const bt::Market& m, bt::BtConfig c, const strat::MmConfig& sc) {
  c.risk = risk_cfg();
  c.tick_raw = m.cfg.tick_raw;
  core::Region region = core::Region::create(128u << 20, core::RegionOptions{false, false, true, -1});
  core::Arena arena(region);
  strat::MarketMaker mm(sc);
  auto* b = new bt::Backtester<strat::MarketMaker>();
  if (!b->init(arena, m.tape.data(), m.tape.size(), book_cfg(m.cfg), c, &mm)) {
    std::fprintf(stderr, "init failed\n");
    std::exit(1);
  }
  const auto t0 = std::chrono::steady_clock::now();
  b->run();
  const auto t1 = std::chrono::steady_clock::now();
  Result r;
  r.rep = b->report();
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  delete b;
  return r;
}

bt::MarketConfig market(std::uint64_t requests, std::uint64_t seed) {
  bt::MarketConfig c;
  c.requests = requests;
  c.seed = seed;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t requests = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000ULL;
  const int markets = argc > 2 ? std::atoi(argv[2]) : 4;

  std::vector<bt::Market> mk;
  for (int i = 0; i < markets; ++i) mk.push_back(bt::MarketGen::generate(market(requests, 1000 + static_cast<std::uint64_t>(i))));
  std::printf("%d synthetic markets, %llu requests each (%zu public events in the first), mean spread %.2f ticks\n\n", markets,
              (unsigned long long)requests, mk[0].tape.size(), mk[0].stats.mean_spread_ticks);

  bt::BtConfig base;
  base.md_ns = 5'000;
  base.ingress_ns = 10'000;
  base.egress_ns = 10'000;
  base.jitter_ns = 2'000;
  base.seed = 1;
  const strat::MmConfig mm;

  std::printf("== headline run: market 0, market data 5 us, ingress 10 us, egress 10 us, jitter 2 us ==\n");
  const Result a = run(mk[0], base, mm);
  bt::print_report(a.rep);
  std::printf("  speed           %.2f M tape events/s (replay incl. gateway, OMS, risk, wire codec)\n",
              static_cast<double>(a.rep.tape_events) / a.seconds / 1e6);
  const Result b = run(mk[0], base, mm);
  std::printf("\n  deterministic replay: digest %016llx then %016llx -> %s\n", (unsigned long long)a.rep.digest,
              (unsigned long long)b.rep.digest, a.rep.digest == b.rep.digest ? "IDENTICAL" : "DIFFERENT");

  std::printf("\n== is the imbalance signal doing the work? (sum over %d markets) ==\n", markets);
  std::printf("  %-26s %10s %10s %10s %10s %12s\n", "alpha (ticks at full imb)", "PnL", "edge", "inventory", "fills",
              "post-fill 1ms");
  for (const int alpha : {0, 8, 16, 32}) {
    strat::MmConfig s = mm;
    s.alpha16 = alpha;
    double pnl = 0, edge = 0, inv = 0, adv = 0;
    std::uint64_t fills = 0;
    for (const bt::Market& m : mk) {
      const Result r = run(m, base, s);
      pnl += r.rep.pnl;
      edge += r.rep.edge;
      inv += r.rep.inventory;
      adv += r.rep.adverse[1];
      fills += r.rep.fills;
    }
    std::printf("  %-26.2f %+10.2f %+10.2f %+10.2f %10llu %+12.3f\n", alpha / 16.0, pnl, edge, inv, (unsigned long long)fills,
                adv / markets);
  }

  std::printf("\n== latency vs alpha decay (round trip = md + ingress + egress, split evenly; sum over %d markets) ==\n", markets);
  std::printf("  %-12s %10s %10s %10s %10s %10s %12s\n", "round trip", "PnL", "edge", "inventory", "fill%", "Sharpe/bin",
              "post-fill 1ms");
  for (const std::uint64_t rt_us : {0ull, 15ull, 30ull, 60ull, 120ull, 250ull, 500ull, 1000ull}) {
    bt::BtConfig c = base;
    c.md_ns = c.ingress_ns = c.egress_ns = rt_us * 1000 / 3;
    c.jitter_ns = 0;
    double pnl = 0, edge = 0, inv = 0, adv = 0, sh = 0, fr = 0;
    for (const bt::Market& m : mk) {
      const Result r = run(m, c, mm);
      pnl += r.rep.pnl;
      edge += r.rep.edge;
      inv += r.rep.inventory;
      adv += r.rep.adverse[1];
      sh += r.rep.sharpe_per_bin;
      fr += r.rep.fill_rate_quotes;
    }
    std::printf("  %5llu us     %+10.2f %+10.2f %+10.2f %9.1f%% %10.3f %+12.3f\n", (unsigned long long)rt_us, pnl, edge, inv,
                100 * fr / markets, sh / markets, adv / markets);
  }

  std::printf("\n== queue depth sensitivity: pretend p%% more of the level was ahead of us (sum over %d markets) ==\n", markets);
  for (const std::uint32_t pct : {0u, 25u, 50u, 100u, 200u}) {
    bt::BtConfig c = base;
    c.queue_penalty_pct = pct;
    double pnl = 0, fr = 0;
    std::uint64_t fills = 0;
    for (const bt::Market& m : mk) {
      const Result r = run(m, c, mm);
      pnl += r.rep.pnl;
      fr += r.rep.fill_rate_quotes;
      fills += r.rep.fills;
    }
    std::printf("  +%3u%% queue    PnL %+8.2f  fills %6llu  quotes traded %.1f%%\n", pct, pnl, (unsigned long long)fills,
                100 * fr / markets);
  }
  return a.rep.digest == b.rep.digest ? 0 : 1;
}
