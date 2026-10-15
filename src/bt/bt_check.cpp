// Compile-time enforcement of the hot-path rules for the backtest replay loop,
// the queue simulation and the strategy: built with -fno-exceptions -fno-rtti.
// (The synthetic market generator is setup code and is not included here.)
#include <cstddef>
#include <cstdint>

#include "hft/bt/backtest.hpp"
#include "hft/strat/market_maker.hpp"

namespace hft::bt {

std::uint64_t bt_hotpath_touch(core::Arena& arena, const sim::PublicEvent* tape, std::size_t n,
                               const book::BookConfig& bc, const BtConfig& cfg) noexcept {
  strat::MarketMaker mm;
  Backtester<strat::MarketMaker> b;
  if (!b.init(arena, tape, n, bc, cfg, &mm)) return 0;
  b.run();
  const BtReport r = b.report();
  return r.digest + r.fills;
}

}  // namespace hft::bt
