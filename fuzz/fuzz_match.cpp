// libFuzzer target for the matching engine: arbitrary byte strings are decoded
// into order flow, run through BOTH the engine and the slow reference matcher,
// and any difference in reports, public events, book contents or invariants is
// a crash (__builtin_trap), as is any sanitizer finding.
//
// Input: byte 0 selects the self-trade mode (b % 5); then 5-byte records
//   [0] kind/type  0-4 new order of that OrderType, 5 cancel, 6 replace, 7 new limit
//   [1] owner 1 + (b & 3), side (b >> 2) & 1
//   [2] id  (b % 48 + 1; 255 gives the invalid id 0), a small space so ids collide
//   [3] price level b % 70 (64 is outside the 64-level window); >= 240 is off-tick
//   [4] quantity (int)b - 20, so zero and negative values occur
// The book is tiny (64 levels, 32 orders) so full books, level reuse and
// window edges are reached by short inputs.
#include <cstddef>
#include <cstdint>

#include "hft/core/memory.hpp"
#include "hft/sim/matching_engine.hpp"
#include "ref_matcher.hpp"

namespace {

constexpr std::int64_t kBase = 1'000'000;
constexpr std::int64_t kTick = 100;

hft::core::Region& region() {
  static hft::core::Region r =
      hft::core::Region::create(4u << 20, hft::core::RegionOptions{false, false, true, -1});
  return r;
}

[[noreturn]] void fail() { __builtin_trap(); }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace hft::sim;
  using namespace testsupport;
  if (size < 1) return 0;

  EngineConfig cfg;
  cfg.base = Price::from_raw(kBase);
  cfg.tick_raw = kTick;
  cfg.num_levels = 64;
  cfg.max_orders = 32;
  cfg.stp = static_cast<StpMode>(data[0] % 5);

  hft::core::Arena arena(region());
  Recorder rec;
  MatchingEngine<Recorder> eng;
  if (!eng.init(arena, cfg, rec)) fail();
  RefMatcher ref(cfg);

  std::uint64_t ts = 0;
  for (std::size_t i = 1; i + 5 <= size; i += 5) {
    const std::uint8_t k = data[i] & 7;
    const Owner owner = static_cast<Owner>(1 + (data[i + 1] & 3));
    const Side side = ((data[i + 1] >> 2) & 1) ? Side::Sell : Side::Buy;
    const OrderId id = data[i + 2] == 255 ? 0 : static_cast<OrderId>(data[i + 2] % 48 + 1);
    std::int64_t raw = kBase + static_cast<std::int64_t>(data[i + 3] % 70) * kTick;
    if (data[i + 3] >= 240) raw += 1;
    const Qty qty = static_cast<Qty>(data[i + 4]) - 20;
    rec.clear();
    ref.out.clear();
    ++ts;
    if (k == 5) {
      eng.cancel(id, owner, ts);
      ref.cancel(id, owner, ts);
    } else if (k == 6) {
      eng.replace(id, owner, Price::from_raw(raw), qty, ts);
      ref.replace(id, owner, Price::from_raw(raw), qty, ts);
    } else {
      const OrderType t = k == 7 ? OrderType::Limit : static_cast<OrderType>(k);
      const NewOrder o{id, owner, side, t, Price::from_raw(raw), qty};
      eng.new_order(o, ts);
      ref.new_order(o, ts);
    }
    if (rec.reports.size() != ref.out.reports.size() || rec.pubs.size() != ref.out.pubs.size()) fail();
    for (std::size_t j = 0; j < rec.reports.size(); ++j)
      if (!same(rec.reports[j], ref.out.reports[j])) fail();
    for (std::size_t j = 0; j < rec.pubs.size(); ++j)
      if (!same(rec.pubs[j], ref.out.pubs[j])) fail();
    if (eng.digest() != ref.digest() || eng.order_count() != ref.order_count() || !eng.check_invariants()) fail();
  }
  return 0;
}
