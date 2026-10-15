#pragma once
// Startup-time memory: one mmap'd Region, carved up by an Arena or a Pool.
// All page faults, locking and NUMA placement happen here, once, so the hot
// path never takes a page fault or a syscall.

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "hft/core/types.hpp"

namespace hft::core {

struct RegionOptions {
  bool try_huge_pages = true;  // 2 MiB pages cut TLB misses on large books
  bool lock = true;            // mlock: keep pages resident, never swapped
  bool prefault = true;        // touch every page now, not on first hot-path use
  int numa_node = -1;          // -1: no binding
};

// What startup actually achieved. Callers (and the runbook) log this, because
// a silently missing hugepage or mlock changes latency and should be visible.
struct RegionReport {
  bool huge_explicit = false;  // MAP_HUGETLB succeeded
  bool huge_thp_hint = false;  // fell back to madvise(MADV_HUGEPAGE)
  bool locked = false;
  bool numa_bound = false;
};

class Region {
 public:
  Region() noexcept = default;
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  Region(Region&& o) noexcept { swap(o); }
  Region& operator=(Region&& o) noexcept {
    if (this != &o) {
      release();
      swap(o);
    }
    return *this;
  }
  ~Region() { release(); }

  // Returns an empty Region (data()==nullptr) on failure; no exceptions.
  static Region create(std::size_t bytes, const RegionOptions& opt = {}) noexcept {
    Region r;
    constexpr std::size_t kHuge = 2u << 20;
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    std::size_t len = round_up(bytes, opt.try_huge_pages ? kHuge : page);
    void* p = MAP_FAILED;

    if (opt.try_huge_pages) {
      p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
      if (p != MAP_FAILED) r.report_.huge_explicit = true;
    }
    if (p == MAP_FAILED) {
      len = round_up(bytes, page);
      p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p == MAP_FAILED) return r;
      if (opt.try_huge_pages && madvise(p, len, MADV_HUGEPAGE) == 0) r.report_.huge_thp_hint = true;
    }
    r.base_ = static_cast<std::byte*>(p);
    r.size_ = len;

    // Bind before prefault: pages are placed on the node that first touches
    // them unless a policy is set, so the policy must exist before the touch.
    if (opt.numa_node >= 0) r.report_.numa_bound = bind_node(p, len, opt.numa_node);
    if (opt.lock) r.report_.locked = (mlock(p, len) == 0);
    if (opt.prefault) {
      const std::size_t step = r.report_.huge_explicit ? kHuge : page;
      for (std::size_t off = 0; off < len; off += step) r.base_[off] = std::byte{0};
    }
    return r;
  }

  std::byte* data() const noexcept { return base_; }
  std::size_t size() const noexcept { return size_; }
  const RegionReport& report() const noexcept { return report_; }
  explicit operator bool() const noexcept { return base_ != nullptr; }

 private:
  static constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1) / a * a;
  }

  // Raw mbind(2) so we do not depend on libnuma. MPOL_BIND = 2. Best effort:
  // returns false on single-node machines or without privilege. Not exercised
  // by tests in the single-node sandbox; verify on a multi-socket host.
  static bool bind_node(void* p, std::size_t len, int node) noexcept {
    if (node < 0 || node >= 64) return false;
    unsigned long mask = 1ul << node;
    return syscall(SYS_mbind, p, len, 2 /*MPOL_BIND*/, &mask, 64ul, 0u) == 0;
  }

  void swap(Region& o) noexcept {
    std::swap(base_, o.base_);
    std::swap(size_, o.size_);
    std::swap(report_, o.report_);
  }
  void release() noexcept {
    if (base_) munmap(base_, size_);
    base_ = nullptr;
    size_ = 0;
  }

  std::byte* base_{nullptr};
  std::size_t size_{0};
  RegionReport report_{};
};

// Bump allocator over a caller-owned byte range. Freed only by reset(), which
// is what makes it O(1) and branch-light. Returns nullptr when exhausted so
// the caller decides (the hot path treats exhaustion as a fatal config error).
class Arena {
 public:
  Arena() noexcept = default;
  Arena(std::byte* base, std::size_t size) noexcept : base_(base), size_(size) {}
  explicit Arena(const Region& r) noexcept : Arena(r.data(), r.size()) {}

  void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept {
    const auto cur = reinterpret_cast<std::uintptr_t>(base_) + used_;
    const std::uintptr_t aligned = (cur + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
    const std::size_t new_used = static_cast<std::size_t>(aligned - reinterpret_cast<std::uintptr_t>(base_)) + bytes;
    if (new_used > size_) return nullptr;
    used_ = new_used;
    return reinterpret_cast<void*>(aligned);
  }

  template <class T>
  T* allocate_array(std::size_t n) noexcept {
    return static_cast<T*>(allocate(sizeof(T) * n, alignof(T)));
  }

  void reset() noexcept { used_ = 0; }
  std::size_t used() const noexcept { return used_; }
  std::size_t capacity() const noexcept { return size_; }

 private:
  std::byte* base_{nullptr};
  std::size_t size_{0};
  std::size_t used_{0};
};

// Fixed-capacity object pool with an index-based LIFO free list. Index links
// (not pointers) keep slots compact and let a 32-bit handle stand in for a
// pointer in the order book later. LIFO reuse hands back the most recently
// freed slot, which is the one most likely still in cache. Single-writer: not
// thread-safe by design.
template <class T>
class Pool {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  Pool() noexcept = default;

  // Carves slots and the free-list out of an Arena. Returns false if too small.
  bool init(Arena& arena, std::uint32_t capacity) noexcept {
    slots_ = static_cast<Slot*>(arena.allocate(sizeof(Slot) * capacity, alignof(Slot)));
    if (!slots_) return false;
    capacity_ = capacity;
    for (std::uint32_t i = 0; i < capacity; ++i)
      slots_[i].next = (i + 1 < capacity) ? i + 1 : kNil;
    free_head_ = capacity ? 0 : kNil;
    in_use_ = 0;
    return true;
  }

  template <class... Args>
  T* emplace(Args&&... args) noexcept {
    if (free_head_ == kNil) return nullptr;
    Slot& s = slots_[free_head_];
    free_head_ = s.next;
    ++in_use_;
    return ::new (static_cast<void*>(s.storage)) T(std::forward<Args>(args)...);
  }

  void release(T* p) noexcept {
    p->~T();
    const std::uint32_t idx = index_of(p);
    slots_[idx].next = free_head_;
    free_head_ = idx;
    --in_use_;
  }

  std::uint32_t index_of(const T* p) const noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<const Slot*>(p) - slots_);
  }
  T* at(std::uint32_t idx) noexcept { return reinterpret_cast<T*>(slots_[idx].storage); }

  std::uint32_t capacity() const noexcept { return capacity_; }
  std::uint32_t in_use() const noexcept { return in_use_; }

 private:
  // The link lives in a union with the object storage so an empty slot costs
  // no extra bytes beyond sizeof(T) rounded to alignment.
  union Slot {
    std::uint32_t next;
    alignas(T) std::byte storage[sizeof(T)];
  };

  Slot* slots_{nullptr};
  std::uint32_t capacity_{0};
  std::uint32_t free_head_{kNil};
  std::uint32_t in_use_{0};
};

}  // namespace hft::core
