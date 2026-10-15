// Phase 5 gate: pre-trade risk check latency, p99 < 1 microsecond.
//
// Measures RiskEngine::check_new / check_replace (the gate), then the OMS
// bookkeeping and the whole gateway submit path (risk + OMS + encode + send)
// for context. Timer overhead (the cost of two ordered TSC reads) is
// subtracted. Exits non-zero if the risk check p99 is 1000 ns or more.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/oms/oms.hpp"
#include "hft/sim/types.hpp"
#include "hft/risk/risk.hpp"

using namespace hft;
using core::Price;
using core::Side;
using sim::OrderType;

namespace {

struct NullSend {
  std::uint64_t n = 0;
  void operator()(const std::uint8_t*, std::size_t len) noexcept { n += len; }
};

template <class T>
inline void do_not_optimize(T const& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

struct Rng {
  std::uint64_t s;
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

constexpr std::int64_t kRef = 1'200'000;

void report(const char* name, const core::Histogram& h, const core::TscCalibration& cal, std::uint64_t n) {
  std::printf("  %-34s n=%8llu  p50 %6.0f  p99 %6.0f  p99.9 %7.0f  max %9.0f ns\n", name,
              static_cast<unsigned long long>(n), cal.to_ns(h.quantile(0.5)), cal.to_ns(h.quantile(0.99)),
              cal.to_ns(h.quantile(0.999)), cal.to_ns(h.max()));
}

}  // namespace

int main() {
  const core::TscCalibration cal;
  std::printf("TSC %.4f ticks/ns, invariant=%s\n", cal.ticks_per_ns(), core::has_invariant_tsc() ? "yes" : "NO");

  core::Histogram oh;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t a = core::rdtsc_ordered();
    oh.record(core::rdtsc_ordered() - a);
  }
  const std::uint64_t overhead = oh.quantile(0.5);
  std::printf("timer overhead %.1f ns\n\n", cal.to_ns(overhead));

  risk::RiskConfig rc;
  rc.max_order_qty = 1000;
  rc.max_order_notional_raw = 1'000'000'000'000LL;
  rc.price_band_bps = 500;
  rc.max_position = 100'000;
  rc.max_open_orders = 1u << 30;
  rc.rate_per_sec = 1'000'000'000;
  rc.burst = 1'000'000;
  rc.max_loss_raw = 1'000'000'000'000LL;  // loss check active, never trips

  constexpr std::uint64_t kN = 2'000'000;
  struct In {
    Side side;
    OrderType type;
    Price px;
    core::Qty qty;
  };
  std::vector<In> in;
  in.reserve(kN);
  Rng rng{88172645463325252ULL};
  for (std::uint64_t i = 0; i < kN; ++i) {
    In x;
    x.side = (rng.next() & 1) ? Side::Buy : Side::Sell;
    x.type = (rng.next() % 8 == 0) ? OrderType::Market : OrderType::Limit;
    x.px = Price::from_raw(kRef + static_cast<std::int64_t>(rng.next() % 41) * 100 - 2000);
    x.qty = 1 + static_cast<core::Qty>(rng.next() % 500);
    in.push_back(x);
  }

  bool pass = true;
  std::printf("pre-trade risk (gate: p99 < 1000 ns)\n");
  {
    risk::RiskEngine eng;
    eng.init(rc);
    eng.set_reference(Price::from_raw(kRef));
    oms::Position pos;
    pos.pos = 50;
    pos.open_buy = 100;
    pos.open_sell = 100;
    pos.open_orders = 20;
    core::Histogram h;
    std::uint64_t now = 0, ok = 0;
    for (const In& x : in) {
      now += 1000;
      const std::uint64_t a = core::rdtsc_ordered();
      const risk::RiskResult r = eng.check_new(x.side, x.type, x.px, x.qty, pos, now);
      const std::uint64_t d = core::rdtsc_ordered() - a;
      h.record(d > overhead ? d - overhead : 0);
      ok += r == risk::RiskResult::Ok;
    }
    do_not_optimize(ok);
    report("check_new (all checks, accepting)", h, cal, kN);
    if (cal.to_ns(h.quantile(0.99)) >= 1000.0) pass = false;

    core::Histogram h2;
    for (const In& x : in) {
      now += 1000;
      const std::uint64_t a = core::rdtsc_ordered();
      const risk::RiskResult r = eng.check_replace(x.side, x.px, x.qty, 100, pos, now);
      const std::uint64_t d = core::rdtsc_ordered() - a;
      h2.record(d > overhead ? d - overhead : 0);
      ok += r == risk::RiskResult::Ok;
    }
    do_not_optimize(ok);
    report("check_replace", h2, cal, kN);
    if (cal.to_ns(h2.quantile(0.99)) >= 1000.0) pass = false;

    // Rejections: a tight rate limit so a share of requests die at each stage.
    risk::RiskConfig tight = rc;
    tight.max_order_qty = 300;
    tight.max_position = 400;
    tight.rate_per_sec = 5'000'000;
    tight.burst = 50;
    risk::RiskEngine e2;
    e2.init(tight);
    e2.set_reference(Price::from_raw(kRef));
    core::Histogram h3;
    std::uint64_t rejects = 0;
    now = 0;
    for (const In& x : in) {
      now += 150;
      const std::uint64_t a = core::rdtsc_ordered();
      const risk::RiskResult r = e2.check_new(x.side, x.type, x.px, x.qty, pos, now);
      const std::uint64_t d = core::rdtsc_ordered() - a;
      h3.record(d > overhead ? d - overhead : 0);
      rejects += r != risk::RiskResult::Ok;
    }
    do_not_optimize(rejects);
    char nm[64];
    std::snprintf(nm, sizeof nm, "check_new (%.0f%% rejected)", 100.0 * static_cast<double>(rejects) / kN);
    report(nm, h3, cal, kN);
    if (cal.to_ns(h3.quantile(0.99)) >= 1000.0) pass = false;
  }

  std::printf("\nfor context (not gated)\n");
  {
    core::Region region = core::Region::create(256u << 20, core::RegionOptions{true, true, true, -1});
    if (!region) return 1;
    core::Arena arena(region);
    gw::Gateway<NullSend> g{NullSend{}};
    if (!g.init(arena, oms::OmsConfig{1u << 20}, rc)) return 2;
    g.risk().set_reference(Price::from_raw(kRef));
    core::Histogram hs, hc;
    std::uint64_t now = 0;
    constexpr std::uint64_t kG = 500'000;
    for (std::uint64_t i = 0; i < kG; ++i) {
      now += 1000;
      const In& x = in[i];
      const std::uint64_t a = core::rdtsc_ordered();
      const auto res = g.new_order(x.side, x.type, x.px, x.qty, now);
      const std::uint64_t d = core::rdtsc_ordered() - a;
      hs.record(d > overhead ? d - overhead : 0);
      if (res.risk == risk::RiskResult::Ok && (i & 1)) {
        const std::uint64_t b = core::rdtsc_ordered();
        g.cancel(res.id, now);
        const std::uint64_t e = core::rdtsc_ordered() - b;
        hc.record(e > overhead ? e - overhead : 0);
      }
    }
    report("gateway new_order (risk+oms+encode)", hs, cal, kG);
    report("gateway cancel (oms+encode)", hc, cal, kG / 2);
  }

  std::printf("\ngate: risk check p99 < 1000 ns on every path ... %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
