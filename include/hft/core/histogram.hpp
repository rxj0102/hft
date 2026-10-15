#pragma once
// Log-linear latency histogram (HdrHistogram style). Values are bucketed by
// power of two, and each power-of-two range is split into 2^kSubBits linear
// sub-buckets. Relative error is bounded by 1 / 2^kSubBits (about 1.6%).
// Storage is fixed, so record() never allocates.

#include <array>
#include <bit>
#include <cstdint>

namespace hft::core {

class Histogram {
 public:
  static constexpr unsigned kSubBits = 6;
  static constexpr unsigned kSubCount = 1u << kSubBits;
  static constexpr unsigned kMaxExp = 40;  // covers values up to ~2^40 ticks
  static constexpr unsigned kBuckets = (kMaxExp + 2) * kSubCount;

  void record(std::uint64_t v) noexcept {
    ++counts_[index_of(v)];
    ++total_;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
  }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t max() const noexcept { return total_ ? max_ : 0; }
  std::uint64_t min() const noexcept { return total_ ? min_ : 0; }

  // Smallest bucket upper edge such that at least q of samples are <= it.
  // q in [0,1]. Returns exact max for q == 1 so tails are not understated.
  std::uint64_t quantile(double q) const noexcept {
    if (total_ == 0) return 0;
    if (q >= 1.0) return max_;
    auto target = static_cast<std::uint64_t>(q * static_cast<double>(total_));
    if (target == 0) target = 1;
    std::uint64_t seen = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen >= target) {
        const std::uint64_t hi = upper_edge(i);
        return hi > max_ ? max_ : hi;
      }
    }
    return max_;
  }

  void reset() noexcept {
    counts_.fill(0);
    total_ = 0;
    max_ = 0;
    min_ = ~std::uint64_t{0};
  }

 private:
  static unsigned index_of(std::uint64_t v) noexcept {
    if (v < kSubCount) return static_cast<unsigned>(v);
    // Level 0 holds v < kSubCount exactly. Level L >= 1 holds
    // [kSubCount << (L-1), kSubCount << L) in steps of 2^(L-1).
    const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
    unsigned shift = msb - kSubBits;
    if (shift > kMaxExp) shift = kMaxExp;
    const auto sub = static_cast<unsigned>((v >> shift) & (kSubCount - 1));
    return (shift + 1) * kSubCount + sub;
  }

  static std::uint64_t upper_edge(unsigned idx) noexcept {
    const unsigned level = idx / kSubCount;
    const unsigned sub = idx % kSubCount;
    if (level == 0) return sub;
    return ((static_cast<std::uint64_t>(kSubCount) + sub + 1) << (level - 1)) - 1;
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_{0};
  std::uint64_t max_{0};
  std::uint64_t min_{~std::uint64_t{0}};
};

}  // namespace hft::core
