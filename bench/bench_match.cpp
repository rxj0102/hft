// Matching engine throughput and per-request latency (Phase 4; the phase gate
// is the conformance suite, this just puts numbers on the engine).
//
// A pre-generated mixed flow (limit / market / IOC / FOK / post-only /
// cancel / replace, 4 owners, no invalid requests, STP off) is replayed 7
// times on a fresh engine; throughput is reported as min / median / max.
// Per-request latency comes from the last trial, split by request kind, with
// the measured cost of the clock reads subtracted.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/matching_engine.hpp"
#include "order_gen.hpp"

using namespace hft;

namespace {

struct CountListener {
  std::uint64_t reports = 0, fills = 0, pubs = 0;
  void on_report(const sim::Report& r) noexcept {
    ++reports;
    if (r.type == sim::ReportType::Fill) fills += static_cast<std::uint64_t>(r.qty);
  }
  void on_public(const sim::PublicEvent&) noexcept { ++pubs; }
};

template <class T>
inline void do_not_optimize(T const& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

void apply(sim::MatchingEngine<CountListener>& e, const testsupport::Op& op, std::uint64_t ts) {
  switch (op.kind) {
    case testsupport::Op::New: e.new_order(op.o, ts); break;
    case testsupport::Op::Cancel: e.cancel(op.id, op.owner, ts); break;
    case testsupport::Op::Replace: e.replace(op.id, op.owner, op.price, op.qty, ts); break;
  }
}

int kind_of(const testsupport::Op& op) {
  if (op.kind == testsupport::Op::Cancel) return 5;
  if (op.kind == testsupport::Op::Replace) return 6;
  return static_cast<int>(op.o.type);  // 0 limit 1 market 2 ioc 3 fok 4 post-only
}
const char* kNames[] = {"limit", "market", "ioc", "fok", "post-only", "cancel", "replace"};

}  // namespace

int main() {
  const core::TscCalibration cal;
  std::printf("TSC %.4f ticks/ns, invariant=%s\n", cal.ticks_per_ns(), core::has_invariant_tsc() ? "yes" : "NO");

  sim::EngineConfig cfg;
  cfg.base = core::Price::from_units(100);
  cfg.tick_raw = 100;
  cfg.num_levels = 4096;
  cfg.max_orders = 1u << 16;
  cfg.stp = sim::StpMode::None;

  constexpr std::uint64_t kOps = 2'000'000;
  std::vector<testsupport::Op> ops;
  ops.reserve(kOps);
  {
    testsupport::FlowGen gen({cfg.base, cfg.tick_raw, cfg.num_levels, 4, false}, 2026);
    for (std::uint64_t i = 0; i < kOps; ++i) ops.push_back(gen.next());
  }

  // Cost of the two clock reads, to subtract from per-request samples.
  core::Histogram oh;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t a = core::rdtsc_ordered();
    oh.record(core::rdtsc_ordered() - a);
  }
  const std::uint64_t overhead = oh.quantile(0.5);
  std::printf("timer overhead %.1f ns\n\n", cal.to_ns(overhead));

  core::Region region = core::Region::create(64u << 20, core::RegionOptions{true, true, true, -1});
  if (!region) return 1;

  constexpr int kTrials = 7;
  std::vector<double> rates;
  core::Histogram lat[7];
  std::uint64_t counts[7] = {};
  std::uint32_t peak_orders = 0;
  CountListener last;
  for (int t = 0; t < kTrials; ++t) {
    core::Arena arena(region);
    CountListener l;
    sim::MatchingEngine<CountListener> eng;
    if (!eng.init(arena, cfg, l)) return 2;
    const bool record = t == kTrials - 1;
    const std::uint64_t c0 = core::rdtsc_ordered();
    if (!record) {
      std::uint64_t ts = 0;
      for (const auto& op : ops) apply(eng, op, ++ts);
    } else {
      std::uint64_t ts = 0;
      for (const auto& op : ops) {
        const std::uint64_t a = core::rdtsc_ordered();
        apply(eng, op, ++ts);
        const std::uint64_t d = core::rdtsc_ordered() - a;
        lat[kind_of(op)].record(d > overhead ? d - overhead : 0);
        ++counts[kind_of(op)];
        if ((ts & 1023) == 0 && eng.order_count() > peak_orders) peak_orders = eng.order_count();
      }
    }
    const std::uint64_t c1 = core::rdtsc_ordered();
    rates.push_back(static_cast<double>(ops.size()) / (cal.to_ns(c1 - c0) * 1e-9));
    do_not_optimize(l.reports);
    if (!eng.check_invariants()) {
      std::fprintf(stderr, "invariants broken after the run\n");
      return 3;
    }
    last = l;
  }
  std::sort(rates.begin(), rates.end());
  std::printf("%llu requests, %llu reports, %llu public events, ~%u resting orders at peak\n",
              static_cast<unsigned long long>(ops.size()), static_cast<unsigned long long>(last.reports),
              static_cast<unsigned long long>(last.pubs), peak_orders);
  std::printf("throughput  min %.2fM  median %.2fM  max %.2fM requests/sec\n\n", rates.front() / 1e6,
              rates[rates.size() / 2] / 1e6, rates.back() / 1e6);
  std::printf("per-request latency (last trial, timer overhead subtracted), ns\n");
  for (int k = 0; k < 7; ++k) {
    if (!counts[k]) continue;
    std::printf("  %-10s n=%8llu  p50 %6.0f  p99 %6.0f  p99.9 %7.0f  max %9.0f\n", kNames[k],
                static_cast<unsigned long long>(counts[k]), cal.to_ns(lat[k].quantile(0.5)),
                cal.to_ns(lat[k].quantile(0.99)), cal.to_ns(lat[k].quantile(0.999)), cal.to_ns(lat[k].max()));
  }
  return 0;
}
