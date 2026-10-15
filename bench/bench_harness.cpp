// Phase 0 benchmark harness. Measures the cost of the timer itself, then times
// a known workload so later phases can reuse measure() for real components.
#include <cstdint>
#include <cstdio>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"

namespace {

using hft::core::Histogram;
using hft::core::TscCalibration;

// Prevents the compiler from deleting or hoisting the work under test.
template <class T>
inline void do_not_optimize(T const& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

// Runs fn() `iters` times, recording per-call ticks with the timer overhead
// subtracted. Warmup runs first so caches, branch predictors and the TLB are
// primed; otherwise the first samples dominate the max.
template <class Fn>
Histogram measure(Fn&& fn, std::uint64_t iters, std::uint64_t warmup, std::uint64_t overhead) {
  for (std::uint64_t i = 0; i < warmup; ++i) fn();
  Histogram h;
  for (std::uint64_t i = 0; i < iters; ++i) {
    const std::uint64_t t0 = hft::core::rdtsc_ordered();
    fn();
    const std::uint64_t t1 = hft::core::rdtsc_ordered();
    const std::uint64_t d = t1 - t0;
    h.record(d > overhead ? d - overhead : 0);
  }
  return h;
}

void print_row(const char* name, const Histogram& h, const TscCalibration& cal) {
  std::printf("%-22s %9llu  p50 %8.1f  p99 %8.1f  p99.9 %8.1f  max %10.1f  (ns)\n", name,
              static_cast<unsigned long long>(h.count()), cal.to_ns(h.quantile(0.50)),
              cal.to_ns(h.quantile(0.99)), cal.to_ns(h.quantile(0.999)),
              cal.to_ns(h.max()));
}

}  // namespace

int main() {
  const TscCalibration cal;
  std::printf("invariant TSC : %s\n", hft::core::has_invariant_tsc() ? "yes" : "NO (results unreliable)");
  std::printf("TSC rate      : %.4f ticks/ns\n", cal.ticks_per_ns());
#ifdef __AVX2__
  std::printf("AVX2          : compiled in\n");
#else
  std::printf("AVX2          : not enabled in this build\n");
#endif

  // Timer overhead: back-to-back reads with an empty body. Use the low
  // quantile, because we want the floor to subtract, not the noisy tail.
  const Histogram empty = measure([] {}, 1'000'000, 10'000, 0);
  const std::uint64_t overhead = empty.quantile(0.50);
  std::printf("timer overhead: %.1f ns (p50, subtracted below)\n\n", cal.to_ns(overhead));

  print_row("empty (after subtract)", measure([] {}, 1'000'000, 10'000, overhead), cal);

  volatile std::uint64_t sink = 0;
  print_row("100 dependent adds",
            measure(
                [&] {
                  std::uint64_t x = sink;
                  for (int i = 0; i < 100; ++i) x += static_cast<std::uint64_t>(i) ^ x;
                  do_not_optimize(x);
                  sink = x;
                },
                1'000'000, 10'000, overhead),
            cal);

  static std::uint64_t table[1 << 16];
  std::uint64_t idx = 1;
  print_row("L1-resident load",
            measure(
                [&] {
                  idx = (idx * 2862933555777941757ULL + 3037000493ULL) & 0xFF;
                  sink = table[idx];
                },
                1'000'000, 10'000, overhead),
            cal);

  return 0;
}
