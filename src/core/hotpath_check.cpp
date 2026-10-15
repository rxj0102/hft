// Same purpose as src/book/book_check.cpp, for the Phase 1 hot-path headers.
// The logger is excluded on purpose: its flusher thread is off the hot path.
#include <cstdint>

#include "hft/core/clock.hpp"
#include "hft/core/fixed_point.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/core/spsc_ring.hpp"

namespace hft::core {

template class SpscRing<std::uint64_t, 8>;

std::uint64_t core_hotpath_touch(SpscRing<std::uint64_t, 8>& ring, Pool<std::uint64_t>& pool,
                                 Arena& arena, Histogram& h) noexcept {
  ring.try_push(1);
  std::uint64_t out = 0;
  ring.try_pop(out);
  if (std::uint64_t* p = pool.emplace(out)) {
    h.record(*p);
    pool.release(p);
  }
  (void)arena.allocate(8, 8);
  const Price a = Price::from_units(1);
  return static_cast<std::uint64_t>(saturate_i64(notional_raw(a, 3))) + rdtsc_fast() +
         (within_bps(a, a, 10) ? 1u : 0u);
}

}  // namespace hft::core
