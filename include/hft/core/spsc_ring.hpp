#pragma once
// Single-producer single-consumer ring buffer, Disruptor style: monotonically
// increasing 64-bit sequence numbers, power-of-two capacity, slot = seq & mask.
// Wraparound of a 64-bit counter is not a practical concern (centuries at
// 1e9 msgs/sec), so we never need a "one slot empty" trick.
//
// Memory-order justification (the only ordering this file relies on):
//
//  producer: tail_.store(release)
//    Publishes the slot contents written before it. A consumer that observes
//    the new tail with acquire is guaranteed to see the payload.
//  consumer: tail_.load(acquire)
//    Pairs with the producer's release above.
//  consumer: head_.store(release)
//    Tells the producer the slot has been fully read and may be overwritten.
//    Without release the consumer's reads could be reordered after the store
//    and the producer would overwrite data still being read.
//  producer: head_.load(acquire)
//    Pairs with the consumer's release above.
//  own-index loads (producer reads tail_, consumer reads head_): relaxed.
//    Each index has exactly one writer, so a thread always sees its own latest
//    value; no cross-thread ordering is needed to read it.
//
// x86 gives release/acquire for free (plain MOV); on ARM these become
// stlr/ldar, and the same reasoning keeps the code correct without changes.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "hft/core/types.hpp"

namespace hft::core {

template <Message T, std::size_t N>
class SpscRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
  static constexpr std::uint64_t kMask = N - 1;

 public:
  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // ---- producer side -------------------------------------------------------

  // Zero-copy: returns the next free slot or nullptr if full. The caller fills
  // it in place, then calls publish(). Avoids a copy for large messages.
  T* claim() noexcept {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= N) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t - cached_head_ >= N) return nullptr;
    }
    return &slots_[t & kMask];
  }

  void publish() noexcept {
    // Only the producer writes tail_, so relaxed read + release store is the
    // cheapest correct increment.
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  bool try_push(const T& v) noexcept {
    T* slot = claim();
    if (!slot) return false;
    *slot = v;
    publish();
    return true;
  }

  // ---- consumer side -------------------------------------------------------

  // Returns the oldest unread slot or nullptr if empty. Valid until pop().
  const T* peek() noexcept {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return nullptr;
    }
    return &slots_[h & kMask];
  }

  void pop() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  bool try_pop(T& out) noexcept {
    const T* s = peek();
    if (!s) return false;
    out = *s;
    pop();
    return true;
  }

  // Approximate occupancy; safe from either thread, exact from neither.
  std::size_t size_approx() const noexcept {
    return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) -
                                    head_.load(std::memory_order_acquire));
  }
  static constexpr std::size_t capacity() noexcept { return N; }

 private:
  // Producer-owned line: tail_ plus the producer's cache of head_. Padded to a
  // 128-byte pair so the consumer's line below never shares a prefetch pair.
  alignas(kPrefetchPair) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_{0};

  // Consumer-owned line.
  alignas(kPrefetchPair) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_{0};

  alignas(kPrefetchPair) T slots_[N];
};

}  // namespace hft::core
