#pragma once
// Fundamental types shared by every module. Everything here is trivially
// copyable so messages can cross rings and the log with a plain memcpy.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hft::core {

// x86-64 cache line. Hardware may prefetch adjacent pairs (128 bytes), so hot
// shared indices are separated by a full 128 to avoid that false sharing too.
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kPrefetchPair = 128;

// 128-bit integer for overflow-safe notional math. __extension__ keeps
// -Wpedantic quiet; both GCC and Clang on x86-64 support it.
__extension__ typedef __int128 i128;

using OrderId = std::uint64_t;
using SeqNo = std::uint64_t;
using InstrumentId = std::uint32_t;
using Qty = std::int64_t;   // signed so position deltas need no casts
using TscTicks = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

// +1 for buy, -1 for sell. Used for branch-free position and PnL arithmetic.
constexpr std::int64_t sign(Side s) noexcept { return s == Side::Buy ? 1 : -1; }

template <class T>
concept Message = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

}  // namespace hft::core
