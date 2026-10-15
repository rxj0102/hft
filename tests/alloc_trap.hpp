#pragma once
// Test-only allocation trap. Include in exactly ONE translation unit per test
// binary, because it replaces the global operator new/delete.
//
// Arming is per thread, so a background thread (the logger flusher) allocating
// off the hot path does not trip a check about the hot thread.

#include <cstdint>
#include <cstdlib>
#include <new>

// The replacement allocator pairs malloc/free by design; GCC cannot see that
// across inlining and reports a false mismatch.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

namespace alloc_trap {

inline thread_local bool g_armed = false;
inline thread_local std::uint64_t g_count = 0;

inline void arm() noexcept {
  g_count = 0;
  g_armed = true;
}
inline std::uint64_t disarm() noexcept {
  g_armed = false;
  return g_count;
}

}  // namespace alloc_trap

void* operator new(std::size_t n) {
  if (alloc_trap::g_armed) ++alloc_trap::g_count;
  if (void* p = std::malloc(n ? n : 1)) return p;
  std::abort();  // hot-path code builds without exceptions; so does the trap
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
