#pragma once
// Fixed-point price. The hot path never touches floating point: float rounding
// makes two runs of the same replay disagree, which breaks determinism, and
// equality on prices (is this order at the best bid?) must be exact.

#include <cstdint>
#include <limits>

#include "hft/core/types.hpp"

namespace hft::core {

// Price stored as an integer count of ticks of 10^-kScaleDigits currency units.
// 4 decimal digits covers equities (cent ticks, sub-penny to 1e-4) and most
// crypto quotes; instruments needing more can use a coarser unit upstream.
class Price {
 public:
  static constexpr int kScaleDigits = 4;
  static constexpr std::int64_t kScale = 10'000;

  constexpr Price() noexcept = default;
  static constexpr Price from_raw(std::int64_t raw) noexcept { return Price{raw}; }
  // Whole currency units plus a fractional part already in 1e-4 units.
  static constexpr Price from_units(std::int64_t whole, std::int64_t frac_1e4 = 0) noexcept {
    return Price{whole * kScale + frac_1e4};
  }

  constexpr std::int64_t raw() const noexcept { return raw_; }
  constexpr bool valid() const noexcept { return raw_ != std::numeric_limits<std::int64_t>::min(); }
  static constexpr Price invalid() noexcept {
    return Price{std::numeric_limits<std::int64_t>::min()};
  }

  friend constexpr bool operator==(Price a, Price b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr auto operator<=>(Price a, Price b) noexcept { return a.raw_ <=> b.raw_; }
  friend constexpr Price operator+(Price a, Price b) noexcept { return Price{a.raw_ + b.raw_}; }
  friend constexpr Price operator-(Price a, Price b) noexcept { return Price{a.raw_ - b.raw_}; }

  // Display and offline analysis only. Never call on the hot path.
  double to_double() const noexcept { return static_cast<double>(raw_) / static_cast<double>(kScale); }

 private:
  explicit constexpr Price(std::int64_t raw) noexcept : raw_(raw) {}
  std::int64_t raw_{0};
};

// Notional = price * qty in raw price units, computed in 128 bits so a large
// order at a large price cannot silently wrap. Pre-trade risk relies on this:
// a wrapped notional would pass a limit check it should fail.
constexpr i128 notional_raw(Price p, Qty q) noexcept {
  return static_cast<i128>(p.raw()) * static_cast<i128>(q);
}

// Saturating narrowing, for storing a notional that is known to be bounded by
// risk limits into a 64-bit field. Saturation fails closed (limit check trips).
constexpr std::int64_t saturate_i64(i128 v) noexcept {
  constexpr auto kMax = static_cast<i128>(std::numeric_limits<std::int64_t>::max());
  constexpr auto kMin = static_cast<i128>(std::numeric_limits<std::int64_t>::min());
  return static_cast<std::int64_t>(v > kMax ? kMax : (v < kMin ? kMin : v));
}

// Price band check without division: |p - ref| * 10000 <= ref * bps.
// Used by the fat-finger and price-band risk checks in Phase 5.
constexpr bool within_bps(Price p, Price ref, std::int64_t bps) noexcept {
  const i128 diff = static_cast<i128>(p.raw()) - static_cast<i128>(ref.raw());
  const i128 abs_diff = diff < 0 ? -diff : diff;
  return abs_diff * 10'000 <= static_cast<i128>(ref.raw()) * static_cast<i128>(bps);
}

}  // namespace hft::core
