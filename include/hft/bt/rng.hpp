#pragma once
// Small deterministic generator (xorshift64*). Integer-only so a given seed
// gives the same stream on every compiler and platform.
#include <cstdint>

namespace hft::bt {

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) noexcept : s_(seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL) {
    if (s_ == 0) s_ = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 4; ++i) next();
  }
  std::uint64_t next() noexcept {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1DULL;
  }
  // Uniform in [0, n). n == 0 returns 0.
  std::uint32_t below(std::uint32_t n) noexcept {
    return n == 0 ? 0u : static_cast<std::uint32_t>(((next() >> 32) * n) >> 32);
  }

 private:
  std::uint64_t s_;
};

}  // namespace hft::bt
