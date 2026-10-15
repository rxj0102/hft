#pragma once
// Two-level bitmap over price levels. Level 1 has one bit per price level;
// level 2 has one bit per level-1 word ("this word is non-empty"). Finding the
// next occupied level after the best one empties is then a couple of word
// scans, not a walk across thousands of empty levels. That keeps best bid/ask
// maintenance O(1) in practice even when the book is sparse.

#include <bit>
#include <cstdint>
#include <cstring>

#include "hft/core/memory.hpp"

namespace hft::book {

class LevelBitmap {
 public:
  static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

  // Startup only. Zeroes explicitly because an Arena may be reused after reset().
  bool init(core::Arena& a, std::uint32_t nbits) noexcept {
    nbits_ = nbits;
    nwords_ = (nbits + 63) / 64;
    nsum_ = (nwords_ + 63) / 64;
    words_ = a.allocate_array<std::uint64_t>(nwords_);
    sum_ = a.allocate_array<std::uint64_t>(nsum_);
    if (!words_ || !sum_) return false;
    std::memset(words_, 0, sizeof(std::uint64_t) * nwords_);
    std::memset(sum_, 0, sizeof(std::uint64_t) * nsum_);
    return true;
  }

  void set(std::uint32_t i) noexcept {
    const std::uint32_t w = i >> 6;
    words_[w] |= 1ULL << (i & 63);
    sum_[w >> 6] |= 1ULL << (w & 63);
  }

  void clear(std::uint32_t i) noexcept {
    const std::uint32_t w = i >> 6;
    words_[w] &= ~(1ULL << (i & 63));
    if (words_[w] == 0) sum_[w >> 6] &= ~(1ULL << (w & 63));
  }

  bool test(std::uint32_t i) const noexcept { return (words_[i >> 6] >> (i & 63)) & 1ULL; }

  // Smallest set index >= i, or kNone.
  std::uint32_t next_ge(std::uint32_t i) const noexcept {
    const std::uint32_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~0ULL << (i & 63));
    if (m) return (w << 6) + static_cast<std::uint32_t>(std::countr_zero(m));
    const std::uint32_t nw = w + 1;
    if (nw >= nwords_) return kNone;
    std::uint32_t s = nw >> 6;
    std::uint64_t sm = sum_[s] & (~0ULL << (nw & 63));
    for (;;) {
      if (sm) {
        const std::uint32_t wi = (s << 6) + static_cast<std::uint32_t>(std::countr_zero(sm));
        return (wi << 6) + static_cast<std::uint32_t>(std::countr_zero(words_[wi]));
      }
      if (++s >= nsum_) return kNone;
      sm = sum_[s];
    }
  }

  // Largest set index <= i, or kNone.
  std::uint32_t prev_le(std::uint32_t i) const noexcept {
    const std::uint32_t w = i >> 6;
    const std::uint32_t b = i & 63;
    const std::uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);
    const std::uint64_t m = words_[w] & mask;
    if (m) return (w << 6) + 63u - static_cast<std::uint32_t>(std::countl_zero(m));
    if (w == 0) return kNone;
    const std::uint32_t pw = w - 1;
    std::uint32_t s = pw >> 6;
    const std::uint32_t sb = pw & 63;
    const std::uint64_t smask = (sb == 63) ? ~0ULL : ((1ULL << (sb + 1)) - 1);
    std::uint64_t sm = sum_[s] & smask;
    for (;;) {
      if (sm) {
        const std::uint32_t wi = (s << 6) + 63u - static_cast<std::uint32_t>(std::countl_zero(sm));
        return (wi << 6) + 63u - static_cast<std::uint32_t>(std::countl_zero(words_[wi]));
      }
      if (s == 0) return kNone;
      sm = sum_[--s];
    }
  }

  std::uint32_t size_bits() const noexcept { return nbits_; }

 private:
  std::uint64_t* words_{nullptr};
  std::uint64_t* sum_{nullptr};
  std::uint32_t nbits_{0}, nwords_{0}, nsum_{0};
};

}  // namespace hft::book
