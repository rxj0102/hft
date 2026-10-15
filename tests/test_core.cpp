// Minimal self-checking tests. GoogleTest arrives in Phase 1 via vcpkg; Phase 0
// stays dependency-free so the gate is reproducible anywhere.
#include <cstdio>
#include <cstdlib>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

static bool within(std::uint64_t got, std::uint64_t want, double rel) {
  const double lo = static_cast<double>(want) * (1.0 - rel);
  const double hi = static_cast<double>(want) * (1.0 + rel);
  return static_cast<double>(got) >= lo && static_cast<double>(got) <= hi;
}

static void test_clock() {
  const auto a = hft::core::rdtsc_ordered();
  const auto b = hft::core::rdtsc_ordered();
  CHECK(b >= a);
  hft::core::TscCalibration cal(20'000'000ULL);
  CHECK(cal.ticks_per_ns() > 0.1 && cal.ticks_per_ns() < 10.0);
}

static void test_histogram_small_exact() {
  hft::core::Histogram h;
  for (std::uint64_t v = 0; v < 64; ++v) h.record(v);
  CHECK(h.count() == 64);
  CHECK(h.min() == 0);
  CHECK(h.max() == 63);
  CHECK(h.quantile(0.5) == 31);
}

static void test_histogram_uniform() {
  hft::core::Histogram h;
  for (std::uint64_t v = 1; v <= 100000; ++v) h.record(v);
  CHECK(h.count() == 100000);
  CHECK(h.max() == 100000);
  CHECK(within(h.quantile(0.50), 50000, 0.02));
  CHECK(within(h.quantile(0.99), 99000, 0.02));
  CHECK(within(h.quantile(0.999), 99900, 0.02));
  CHECK(h.quantile(1.0) == 100000);
}

static void test_histogram_tail() {
  hft::core::Histogram h;
  // 0.2% of samples in the tail, so p99.9 must land in the tail bucket.
  for (int i = 0; i < 9980; ++i) h.record(100);
  for (int i = 0; i < 20; ++i) h.record(1'000'000);
  CHECK(within(h.quantile(0.50), 100, 0.02));
  CHECK(within(h.quantile(0.999), 1'000'000, 0.02));
  CHECK(h.max() == 1'000'000);
}

static void test_histogram_reset() {
  hft::core::Histogram h;
  h.record(5);
  h.reset();
  CHECK(h.count() == 0);
  CHECK(h.quantile(0.5) == 0);
}

int main() {
  test_clock();
  test_histogram_small_exact();
  test_histogram_uniform();
  test_histogram_tail();
  test_histogram_reset();
  if (g_failures == 0) std::puts("test_core: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
