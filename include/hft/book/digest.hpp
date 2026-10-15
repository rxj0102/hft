#pragma once
// Order-sensitive digest of book state. The book and the slow reference both
// feed their full state through this in the same canonical order (bids from
// best price down, asks from best price up, each level's queue head to tail),
// so equal digests mean equal prices, quantities AND time priority.

#include <cstdint>

namespace hft::book {

inline constexpr std::uint64_t kDigestSeed = 0xCBF29CE484222325ULL;

constexpr std::uint64_t digest_combine(std::uint64_t h, std::uint64_t v) noexcept {
  // boost::hash_combine-style mix widened to 64 bits.
  return h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

}  // namespace hft::book
