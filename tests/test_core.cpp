// Minimal self-checking tests. GoogleTest arrives in Phase 1 via vcpkg; Phase 0
// stays dependency-free so the gate is reproducible anywhere.
#include <cstdio>
#include <cstdlib>

#include "hft/core/clock.hpp"
#include "hft/core/fast_div.hpp"
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

// The reciprocal divide must equal the hardware divide for every input.
static void test_invariant_divisor() {
  using hft::core::InvariantDivisor;
  // Edge values against a spread of divisors, including powers of two, 1, and huge ones.
  const std::uint64_t ds[] = {1, 2, 3, 7, 10, 16, 99, 100, 101, 255, 256, 1000, 4096, 65535, 65537, 1u << 20,
                              1000000007ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL, 1ULL << 32, (1ULL << 32) + 1,
                              1ULL << 40, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL};
  const std::uint64_t edge_x[] = {0, 1, 2, 99, 100, 101, 0xFFFFFFFFULL, 0x100000000ULL, 0x7FFFFFFFFFFFFFFFULL,
                                  0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};
  for (const std::uint64_t d : ds) {
    const InvariantDivisor v(d);
    std::uint64_t xs[sizeof edge_x / sizeof edge_x[0] + 8];
    std::size_t n = 0;
    for (const std::uint64_t x : edge_x) xs[n++] = x;
    xs[n++] = d - 1;
    xs[n++] = d;
    xs[n++] = d + 1;
    xs[n++] = d * 2 - 1;
    xs[n++] = d * 2;
    xs[n++] = ~std::uint64_t{0} - (~std::uint64_t{0} % d);       // largest multiple of d
    xs[n++] = ~std::uint64_t{0} - (~std::uint64_t{0} % d) - 1;
    xs[n++] = ~std::uint64_t{0} % d;
    for (std::size_t i = 0; i < n; ++i) {
      std::uint64_t q, r;
      v.divmod(xs[i], q, r);
      CHECK(q == xs[i] / d && r == xs[i] % d);
    }
  }
  // Random pairs, with the divisor drawn at every magnitude.
  std::uint64_t s = 0x9E3779B97F4A7C15ULL;
  auto next = [&] {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  };
  for (int i = 0; i < 3'000'000; ++i) {
    std::uint64_t d = next() >> (next() % 64);
    if (d == 0) d = 1;
    const std::uint64_t x = next() >> (next() % 64);
    std::uint64_t q, r;
    InvariantDivisor(d).divmod(x, q, r);
    if (q != x / d || r != x % d) {
      std::fprintf(stderr, "divmod(%llu, %llu) = %llu rem %llu\n", (unsigned long long)x, (unsigned long long)d,
                   (unsigned long long)q, (unsigned long long)r);
      CHECK(false);
      break;
    }
  }
}

int main() {
  test_invariant_divisor();
  test_clock();
  test_histogram_small_exact();
  test_histogram_uniform();
  test_histogram_tail();
  test_histogram_reset();
  if (g_failures == 0) std::puts("test_core: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
