// Compile-time enforcement of the hot-path rules for the matching engine and
// simulator: built with -fno-exceptions -fno-rtti (hft_hotpath).
#include <cstddef>
#include <cstdint>

#include "hft/sim/exchange_sim.hpp"
#include "hft/sim/feed_publisher.hpp"
#include "hft/sim/matching_engine.hpp"

namespace hft::sim {

template class MatchingEngine<NullListener>;

struct Emit {
  std::uint64_t n = 0;
  void operator()(const std::uint8_t*, std::size_t len) noexcept { n += len; }
};
struct SinkT {
  std::uint64_t n = 0;
  void on_report(Owner, const Report& r, std::uint64_t) noexcept { n += static_cast<std::uint64_t>(r.qty); }
  void on_public(const PublicEvent& e, std::uint64_t) noexcept { n += static_cast<std::uint64_t>(e.qty); }
};

std::uint64_t sim_hotpath_touch(ExchangeSim& s, ItchPublisher<Emit>& pub, const NewOrder& o) noexcept {
  SinkT sink;
  s.submit_new(o.owner, o, 1);
  s.submit_cancel(o.owner, o.id, 2);
  s.submit_replace(o.owner, o.id, o.price, o.qty, 3);
  s.advance_to(10, sink);
  pub.on_public(PublicEvent{});
  pub.flush();
  return sink.n + s.engine().digest() + (s.engine().check_invariants() ? 1u : 0u) + s.dropped_out();
}

}  // namespace hft::sim
