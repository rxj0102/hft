#pragma once
// Measurement hook: a thin forwarder around the Gateway that stamps the TSC the
// first time the strategy hands it an order action. That splits a tick into
// "strategy decided" and "gateway built and sent the frame" without touching
// either of them. Used only by the latency harness; production code talks to
// the Gateway directly. It forwards everything unchanged, which the tests check
// by comparing the bytes on the wire with and without it.

#include <cstdint>

#include "hft/core/clock.hpp"

namespace hft::t2t {

struct StageStamp {
  std::uint64_t t_enter = 0;  // first order action reached the gateway (0 = none yet)
};

template <class G>
class ProbedGateway {
 public:
  ProbedGateway(G& g, StageStamp& s) noexcept : g_(g), s_(s) {}

  auto& risk() noexcept { return g_.risk(); }
  auto& oms() noexcept { return g_.oms(); }

  template <class... A>
  auto new_order(A&&... a) noexcept {
    mark();
    return g_.new_order(static_cast<A&&>(a)...);
  }
  template <class... A>
  auto replace(A&&... a) noexcept {
    mark();
    return g_.replace(static_cast<A&&>(a)...);
  }
  template <class... A>
  auto cancel(A&&... a) noexcept {
    mark();
    return g_.cancel(static_cast<A&&>(a)...);
  }

 private:
  [[gnu::always_inline]] void mark() noexcept {
    if (s_.t_enter == 0) s_.t_enter = core::rdtsc_ordered();
  }
  G& g_;
  StageStamp& s_;
};

}  // namespace hft::t2t
