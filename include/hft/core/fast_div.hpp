#pragma once
// Division by a divisor that is fixed at startup but not known at compile time
// (the price tick). A hardware 64-bit divide costs 35 to 90 cycles on the
// server cores this runs on and sat on every price-to-level conversion of the
// tick-to-trade path. Multiplying by a precomputed reciprocal and fixing the
// result up once costs about 8 cycles and gives exactly the same answer.
//
// Method: M = floor((2^64 - 1) / d). For any x below 2^64, floor(x * M / 2^64)
// is either floor(x / d) or one less, so one compare and one add repair it.
// This is proved in the comment on divmod() and checked against the hardware
// divide in tests/test_core.cpp (edge values and millions of random pairs).

#include <cstdint>

namespace hft::core {

__extension__ typedef unsigned __int128 u128_t;  // GCC and Clang; -Wpedantic needs the marker

class InvariantDivisor {
 public:
  constexpr InvariantDivisor() noexcept = default;
  explicit constexpr InvariantDivisor(std::uint64_t d) noexcept { set(d); }

  // d must be non-zero (a zero divisor is a configuration error caught at init).
  constexpr void set(std::uint64_t d) noexcept {
    d_ = d;
    m_ = d ? ~std::uint64_t{0} / d : 0;
  }
  constexpr std::uint64_t divisor() const noexcept { return d_; }

  // q = x / d and r = x % d, for every 64-bit x.
  //
  // Proof sketch. M*d <= 2^64 - 1 < M*d + d, so x*M/2^64 <= x/d * (M*d / 2^64) < x/d,
  // hence the estimate never exceeds the true quotient q. And M > (2^64 - d)/d, so
  // x*M/2^64 > x/d - x/2^64 > x/d - 1 >= q - 1, hence the estimate is at least q - 1.
  // The remainder computed from the estimate therefore lies in [0, 2d), and
  // subtracting d once when it is >= d lands on the exact answer.
  constexpr void divmod(std::uint64_t x, std::uint64_t& q, std::uint64_t& r) const noexcept {
    std::uint64_t qe = static_cast<std::uint64_t>((static_cast<u128_t>(x) * m_) >> 64);
    std::uint64_t re = x - qe * d_;
    if (re >= d_) {
      ++qe;
      re -= d_;
    }
    q = qe;
    r = re;
  }

  constexpr std::uint64_t div(std::uint64_t x) const noexcept {
    std::uint64_t q, r;
    divmod(x, q, r);
    return q;
  }

 private:
  std::uint64_t d_{1};
  std::uint64_t m_{~std::uint64_t{0}};
};

}  // namespace hft::core
