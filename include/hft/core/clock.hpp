#pragma once
// TSC clock. Reading the TSC is ~20 cycles and needs no syscall, which is why
// it is the only clock allowed on the hot path. Wall time is derived offline.

#include <cpuid.h>

#include <cstdint>
#include <ctime>
#include <x86intrin.h>

namespace hft::core {

// lfence before rdtsc keeps the CPU from hoisting the read above earlier
// instructions, so we time the code under test and not the pipeline's
// out-of-order window.
[[gnu::always_inline]] inline std::uint64_t rdtsc_ordered() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

[[gnu::always_inline]] inline std::uint64_t rdtsc_fast() noexcept { return __rdtsc(); }

inline std::uint64_t monotonic_raw_ns() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// True if the CPU advertises an invariant TSC (constant rate across P and C
// states). Without it, cycle counts cannot be converted to time reliably.
inline bool has_invariant_tsc() noexcept {
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) || eax < 0x80000007u) return false;
  __cpuid(0x80000007u, eax, ebx, ecx, edx);
  return (edx & (1u << 8)) != 0;
}

// Ticks-per-nanosecond, measured against CLOCK_MONOTONIC_RAW. A longer window
// reduces error from the two clock reads; 50 ms gives well under 0.1% in
// practice and runs once at startup, never on the hot path.
class TscCalibration {
 public:
  explicit TscCalibration(std::uint64_t window_ns = 50'000'000ULL) noexcept {
    const std::uint64_t n0 = monotonic_raw_ns();
    const std::uint64_t t0 = rdtsc_ordered();
    std::uint64_t n1 = n0;
    while (n1 - n0 < window_ns) n1 = monotonic_raw_ns();
    const std::uint64_t t1 = rdtsc_ordered();
    ticks_per_ns_ = static_cast<double>(t1 - t0) / static_cast<double>(n1 - n0);
  }

  double ticks_per_ns() const noexcept { return ticks_per_ns_; }
  double to_ns(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) / ticks_per_ns_;
  }

 private:
  double ticks_per_ns_{1.0};
};

}  // namespace hft::core
