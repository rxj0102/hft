// libFuzzer target for the OMS: arbitrary sequences of client actions and
// exchange reports (many of them nonsense: reports for unknown ids, fills
// beyond leaves, replies in the wrong state). The oracle is
// Oms::check_invariants() after every step plus the sanitizers: whatever the
// input, bookkeeping must stay self-consistent and nothing may crash.
//
// Input: 6-byte records
//   [0] op: 0-1 send new, 2 request cancel, 3 request replace, 4-10 report (ReportType = op - 4)
//   [1] order id (b % 24 + 1; 255 gives the invalid id 0)
//   [2] side (bit 0), order type (bits 1-3)
//   [3] quantity (int)b - 10
//   [4] leaves / second quantity (int)b - 10
//   [5] price level, 0 gives the invalid price
// A small order table (16 slots) makes eviction and id reuse frequent.
#include <cstddef>
#include <cstdint>

#include "hft/core/memory.hpp"
#include "hft/oms/oms.hpp"

namespace {

hft::core::Region& region() {
  static hft::core::Region r =
      hft::core::Region::create(2u << 20, hft::core::RegionOptions{false, false, true, -1});
  return r;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace hft;
  using namespace hft::sim;
  hft::core::Arena arena(region());
  oms::Oms o;
  if (!o.init(arena, oms::OmsConfig{16})) __builtin_trap();
  std::uint64_t ts = 0;
  for (std::size_t i = 0; i + 6 <= size; i += 6) {
    const std::uint8_t* r = data + i;
    ++ts;
    const OrderId id = r[1] == 255 ? 0 : static_cast<OrderId>(r[1] % 24 + 1);
    const Side side = (r[2] & 1) ? Side::Sell : Side::Buy;
    const Qty q = static_cast<Qty>(r[3]) - 10;
    const Qty q2 = static_cast<Qty>(r[4]) - 10;
    const Price px = r[5] == 0 ? Price::from_raw(0) : Price::from_raw(1'000'000 + static_cast<std::int64_t>(r[5]) * 100);
    if (r[0] < 2) {
      o.on_send_new(id, side, static_cast<OrderType>((r[2] >> 1) % 5), px, q, ts);
    } else if (r[0] == 2) {
      o.on_request_cancel(id, ts);
    } else if (r[0] == 3) {
      o.on_request_replace(id, px, q, ts);
    } else if (r[0] <= 10) {
      Report rep{};
      rep.type = static_cast<ReportType>(r[0] - 4);
      rep.reason = Reason::None;
      rep.liq = Liquidity::None;
      rep.side = side;
      rep.owner = 1;
      rep.id = id;
      rep.price = px;
      rep.qty = q;
      rep.leaves = q2;
      rep.seq = ts;
      rep.ts = ts;
      o.on_report(rep);
    }
    if (!o.check_invariants()) __builtin_trap();
  }
  return 0;
}
