// Compile-time enforcement of the hot-path rules for the OMS, risk engine and
// order-entry gateway: built with -fno-exceptions -fno-rtti (hft_hotpath).
#include <cstddef>
#include <cstdint>

#include "hft/gw/exchange_edge.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/oms/oms.hpp"
#include "hft/risk/risk.hpp"

namespace hft::oms {

std::uint64_t oms_hotpath_touch(Oms& o, risk::RiskEngine& r, const sim::Report& rep) noexcept {
  const auto res = r.check_new(Side::Buy, OrderType::Limit, Price::from_raw(100), 1, o.position(), 1);
  r.check_replace(Side::Buy, Price::from_raw(100), 2, 1, o.position(), 2);
  o.on_send_new(1, Side::Buy, OrderType::Limit, Price::from_raw(100), 1, 1);
  o.on_request_replace(1, Price::from_raw(100), 2, 2);
  o.on_request_cancel(1, 3);
  o.on_report(rep);
  std::uint64_t n = 0;
  o.for_each_open([&](const OrderRec& x) { n += static_cast<std::uint64_t>(x.leaves); });
  return n + static_cast<std::uint64_t>(res) + (o.check_invariants() ? 1u : 0u) + o.stats().anomalies;
}

}  // namespace hft::oms

namespace hft::gw {

struct NullSend {
  std::uint64_t n = 0;
  void operator()(const std::uint8_t*, std::size_t len) noexcept { n += len; }
};
struct NullOut {
  std::uint64_t n = 0;
  void operator()(sim::Owner, const std::uint8_t*, std::size_t len, std::uint64_t) noexcept { n += len; }
};

template class Gateway<NullSend>;
template class ExchangeEdge<NullOut>;

std::uint64_t gw_hotpath_touch(Gateway<NullSend>& g, const std::uint8_t* d, std::size_t n) noexcept {
  const auto r = g.new_order(Side::Buy, OrderType::Limit, Price::from_raw(100), 1, 1);
  g.cancel(r.id, 2);
  g.replace(r.id, Price::from_raw(100), 2, 3);
  g.on_bytes(d, n, 4);
  g.kill(KillReason::Manual, 5);
  return g.stats().reports + g.stats().sent_new;
}

}  // namespace hft::gw
