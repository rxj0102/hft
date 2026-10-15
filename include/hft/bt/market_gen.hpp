#pragma once
// Synthetic market with informed flow, for backtesting without licensed data.
//
// A latent fair value jumps by 1-3 ticks every few hundred requests. Everyone
// else sees only the book. Some participants know the latent value:
//   - when it is above the mid, passive flow leans to the bid (more bids
//     added, asks cancelled), so the book becomes imbalanced BEFORE the move;
//   - informed takers send market orders toward the latent value until the mid
//     catches up.
// The rest is uninformed noise: limit orders around the touch, random
// cancels, small random market orders. So order book imbalance really does
// predict the next mid move, for a short while, and then the signal is used
// up. That is exactly the property a latency-versus-alpha-decay study needs,
// and the tests verify it holds (it is measured, not assumed).
//
// The generator drives the real matching engine and records two things: the
// script of requests (so a test can replay the same flow with extra orders
// injected) and the public event tape (what a market data feed shows).
// Setup code, not hot path: it allocates freely.
//
// Everything is integer arithmetic, and every random draw sits in its own
// statement (argument evaluation order is unspecified in C++ and differs
// between gcc and clang), so a seed reproduces the tape exactly on any compiler.

#include <cstdint>
#include <vector>

#include "hft/bt/rng.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/matching_engine.hpp"

namespace hft::bt {

struct MarketConfig {
  core::Price base = core::Price::from_units(100);
  std::int64_t tick_raw = 100;
  std::uint32_t num_levels = 4096;
  std::uint64_t requests = 200'000;      // after the opening ladder
  std::uint64_t mean_gap_ns = 10'000;    // between requests, uniform in [mean/2, 3*mean/2]
  std::uint32_t jump_period = 400;       // mean requests between latent jumps
  std::uint32_t target_orders = 200;     // resting orders the flow tries to hold
  std::uint32_t informed_pct = 40;       // chance an informed taker acts while the mid lags
  std::uint32_t skew_pct = 35;           // how far passive flow leans toward the latent value
  std::uint32_t improve_pct = 5;         // chance a limit order lands one tick inside the touch
  std::uint64_t seed = 1;
};

struct ScriptOp {
  std::uint64_t ts;
  bool is_cancel;
  sim::NewOrder o;      // new
  core::OrderId id;     // cancel
  sim::Owner owner;     // cancel
};

struct MarketStats {
  std::uint64_t jumps = 0, trades_qty = 0, informed_orders = 0;
  double mean_spread_ticks = 0;
  std::uint64_t spread_samples = 0;
};

struct Market {
  MarketConfig cfg;
  std::vector<ScriptOp> ops;
  std::vector<sim::PublicEvent> tape;
  MarketStats stats;
};

class MarketGen {
 public:
  static Market generate(const MarketConfig& cfg) {
    MarketGen g(cfg);
    g.run();
    return std::move(g.m_);
  }

 private:
  using Side = core::Side;
  using Price = core::Price;
  using OrderId = core::OrderId;

  struct Rec {
    std::vector<sim::PublicEvent>* tape;
    std::uint64_t* trades;
    void on_report(const sim::Report&) noexcept {}
    void on_public(const sim::PublicEvent& e) noexcept {
      tape->push_back(e);
      if (e.type == sim::PublicType::Exec) *trades += static_cast<std::uint64_t>(e.qty);
    }
  };

  explicit MarketGen(const MarketConfig& c)
      : rng_(c.seed),
        region_(core::Region::create(64u << 20, core::RegionOptions{false, false, true, -1})),
        arena_(region_) {
    m_.cfg = c;
    rec_ = Rec{&m_.tape, &m_.stats.trades_qty};
    sim::EngineConfig ec;
    ec.base = c.base;
    ec.tick_raw = c.tick_raw;
    ec.num_levels = c.num_levels;
    ec.max_orders = 1u << 16;
    ec.stp = sim::StpMode::None;
    ok_ = engine_.init(arena_, ec, rec_);
  }

  Price px(std::int64_t idx) const { return Price::from_raw(m_.cfg.base.raw() + idx * m_.cfg.tick_raw); }
  std::int64_t idx_of(Price p) const { return (p.raw() - m_.cfg.base.raw()) / m_.cfg.tick_raw; }

  void send_new(sim::Side side, sim::OrderType t, std::int64_t price_idx, core::Qty qty, sim::Owner owner) {
    sim::NewOrder o{next_id_++, owner, side, t, t == sim::OrderType::Market ? Price::from_raw(0) : px(price_idx), qty};
    m_.ops.push_back(ScriptOp{ts_, false, o, 0, 0});
    engine_.new_order(o, ts_);
    if (t == sim::OrderType::Limit && engine_.contains(o.id)) live_.push_back(Live{o.id, owner, side});
  }
  void send_cancel(OrderId id, sim::Owner owner) {
    m_.ops.push_back(ScriptOp{ts_, true, sim::NewOrder{}, id, owner});
    engine_.cancel(id, owner, ts_);
  }

  sim::Owner owner() { return static_cast<sim::Owner>(2 + rng_.below(3)); }

  void opening_ladder() {
    const std::int64_t c = m_.cfg.num_levels / 2;
    for (int k = 0; k < 40; ++k) {
      ts_ += 100;
      core::Qty q = 1 + static_cast<core::Qty>(rng_.below(10));
      sim::Owner who = owner();
      send_new(Side::Buy, sim::OrderType::Limit, c - 1 - k, q, who);
      ts_ += 100;
      q = 1 + static_cast<core::Qty>(rng_.below(10));
      who = owner();
      send_new(Side::Sell, sim::OrderType::Limit, c + 1 + k, q, who);
    }
    v2_ = 2 * c;
  }

  void run() {
    if (!ok_) return;
    opening_ladder();
    const MarketConfig& c = m_.cfg;
    for (std::uint64_t i = 0; i < c.requests; ++i) {
      ts_ += c.mean_gap_ns / 2 + rng_.below(static_cast<std::uint32_t>(c.mean_gap_ns));
      if (rng_.below(c.jump_period) == 0) {
        const std::int64_t j = 2 * (1 + static_cast<std::int64_t>(rng_.below(3)));
        v2_ += rng_.below(2) ? j : -j;
        const std::int64_t lo = 2 * 300, hi = 2 * (static_cast<std::int64_t>(c.num_levels) - 300);
        if (v2_ < lo) v2_ = lo;
        if (v2_ > hi) v2_ = hi;
        ++m_.stats.jumps;
      }
      const Price bb = engine_.best_bid(), ba = engine_.best_ask();
      const bool two = bb != Price::invalid() && ba != Price::invalid();
      int dir = 0;
      if (two) {
        const std::int64_t mid2 = idx_of(bb) + idx_of(ba);
        const std::int64_t gap2 = v2_ - mid2;
        dir = gap2 >= 2 ? 1 : (gap2 <= -2 ? -1 : 0);
        m_.stats.mean_spread_ticks += static_cast<double>(idx_of(ba) - idx_of(bb));
        ++m_.stats.spread_samples;
      }
      step(two, dir, bb, ba);
    }
    if (m_.stats.spread_samples) m_.stats.mean_spread_ticks /= static_cast<double>(m_.stats.spread_samples);
  }

  void step(bool two, int dir, Price bb, Price ba) {
    const MarketConfig& c = m_.cfg;
    const std::int64_t live = static_cast<std::int64_t>(live_.size());
    const std::int64_t tgt = c.target_orders;
    std::int64_t pull = (tgt - live) * 20 / tgt;
    if (pull > 20) pull = 20;
    if (pull < -15) pull = -15;
    const std::uint32_t p_add = static_cast<std::uint32_t>(30 + pull);
    const std::uint32_t r = rng_.below(100);
    if (!two || r < p_add) return add_limit(two, dir, bb, ba);
    if (r < p_add + 22) return cancel_one(dir);
    if (r < p_add + 32) {
      // draws are sequenced in separate statements: argument evaluation order differs between compilers
      const Side side = rng_.below(2) ? Side::Sell : Side::Buy;
      const core::Qty q = 1 + static_cast<core::Qty>(rng_.below(3));
      const sim::Owner who = owner();
      send_new(side, sim::OrderType::Market, 0, q, who);
      return;
    }
    if (dir != 0 && rng_.below(100) < c.informed_pct) {
      ++m_.stats.informed_orders;
      const core::Qty q = 1 + static_cast<core::Qty>(rng_.below(4));
      const sim::Owner who = owner();
      send_new(dir > 0 ? Side::Buy : Side::Sell, sim::OrderType::Market, 0, q, who);
      return;
    }
    // nobody informed acted: a little more noise, one way or the other
    if (rng_.below(2)) return add_limit(two, dir, bb, ba);
    cancel_one(dir);
  }

  void add_limit(bool two, int dir, Price bb, Price ba) {
    const MarketConfig& c = m_.cfg;
    const std::uint32_t pb = static_cast<std::uint32_t>(50 + dir * static_cast<int>(c.skew_pct));
    const bool buy = rng_.below(100) < pb;
    std::int64_t d = rng_.below(100) < c.improve_pct ? 1 : 2;
    while (d < 9 && rng_.below(100) < 50) ++d;
    std::int64_t i;
    if (!two) {
      // one side empty: seed it next to the latent value
      i = (v2_ / 2) + (buy ? -d : d);
    } else {
      i = buy ? idx_of(ba) - d : idx_of(bb) + d;
      // never cross the opposite touch, never go outside the window
      if (buy && i >= idx_of(ba)) i = idx_of(ba) - 1;
      if (!buy && i <= idx_of(bb)) i = idx_of(bb) + 1;
    }
    if (i < 1 || i >= static_cast<std::int64_t>(c.num_levels) - 1) return;
    const core::Qty q = 1 + static_cast<core::Qty>(rng_.below(10));
    const sim::Owner who = owner();
    send_new(buy ? Side::Buy : Side::Sell, sim::OrderType::Limit, i, q, who);
  }

  void cancel_one(int dir) {
    for (int tries = 0; tries < 6 && !live_.empty(); ++tries) {
      const std::size_t k = rng_.below(static_cast<std::uint32_t>(live_.size()));
      const Live l = live_[k];
      if (!engine_.contains(l.id)) {
        live_[k] = live_.back();
        live_.pop_back();
        continue;
      }
      // while the mid lags the latent value, cancel the side that is about to be run over more often
      const bool vulnerable = (dir > 0 && l.side == Side::Sell) || (dir < 0 && l.side == Side::Buy);
      if (dir != 0 && !vulnerable && rng_.below(100) < 50) continue;
      live_[k] = live_.back();
      live_.pop_back();
      send_cancel(l.id, l.owner);
      return;
    }
  }

  struct Live {
    OrderId id;
    sim::Owner owner;
    Side side;
  };

  Market m_;
  Rng rng_;
  core::Region region_;
  core::Arena arena_;
  Rec rec_{};
  sim::MatchingEngine<Rec> engine_;
  bool ok_ = false;
  std::vector<Live> live_;
  std::uint64_t ts_ = 0;
  OrderId next_id_ = 1;
  std::int64_t v2_ = 0;
};

}  // namespace hft::bt
