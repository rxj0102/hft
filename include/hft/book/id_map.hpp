#pragma once
// Open-addressing hash map from order id to pool slot. Linear probing with
// backward-shift deletion: no tombstones, so lookups never degrade as orders
// churn, which matters because a book deletes about as often as it inserts.
// Key 0 marks an empty slot, so order id 0 is invalid by contract.
// Capacity is fixed and kept at <= 50% load by the caller, so probes stay short.

#include <bit>
#include <cstdint>
#include <cstring>

#include "hft/core/memory.hpp"

namespace hft::book {

class IdMap {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  // capacity_pow2 must be a power of two >= 2. Startup only.
  bool init(core::Arena& a, std::uint32_t capacity_pow2) noexcept {
    if (capacity_pow2 < 2 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) return false;
    mask_ = capacity_pow2 - 1;
    cap_ = capacity_pow2;
    shift_ = 64u - static_cast<unsigned>(std::countr_zero(capacity_pow2));
    slots_ = a.allocate_array<Slot>(capacity_pow2);
    if (!slots_) return false;
    std::memset(slots_, 0, sizeof(Slot) * capacity_pow2);
    return true;
  }

  void clear() noexcept { std::memset(slots_, 0, sizeof(Slot) * cap_); }

  std::uint32_t find(std::uint64_t key) const noexcept {
    if (key == 0) return kNil;
    std::uint32_t i = home(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.key == key) return s.val;
      if (s.key == 0) return kNil;
      i = (i + 1) & mask_;
    }
  }

  // Caller guarantees the key is absent and the table is not full.
  void insert(std::uint64_t key, std::uint32_t val) noexcept {
    std::uint32_t i = home(key);
    while (slots_[i].key != 0) i = (i + 1) & mask_;
    slots_[i].key = key;
    slots_[i].val = val;
  }

  bool erase(std::uint64_t key) noexcept {
    if (key == 0) return false;
    std::uint32_t i = home(key);
    for (;;) {
      if (slots_[i].key == key) break;
      if (slots_[i].key == 0) return false;
      i = (i + 1) & mask_;
    }
    // Backward shift: pull later entries of the probe run into the hole when
    // doing so keeps them reachable from their home slot. The test is on
    // cyclic distance: an entry at j with home k may move to i only if i lies
    // between k and j, i.e. dist(i->j) <= dist(k->j).
    std::uint32_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].key == 0) break;
      const std::uint32_t k = home(slots_[j].key);
      if (((j - k) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].key = 0;
    return true;
  }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t val;
    std::uint32_t pad;
  };

  // Fibonacci multiply-shift. Sequential ids (the common case) spread evenly
  // instead of clustering in adjacent slots, which linear probing would punish.
  std::uint32_t home(std::uint64_t key) const noexcept {
    return static_cast<std::uint32_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
  }

  Slot* slots_{nullptr};
  std::uint32_t cap_{0};
  std::uint32_t mask_{0};
  unsigned shift_{0};
};

}  // namespace hft::book
