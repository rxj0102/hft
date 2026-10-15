#pragma once
// Two-sided passive market maker with inventory skew and an order book
// imbalance signal. One bid and one ask live at a time, always PostOnly.
//
// Fair value, in 1/16 ticks, integer only:
//   fair = mid + alpha * I - skew * position
//   I    = (bid depth - ask depth) / (bid depth + ask depth) over the top
//          `depth_levels` price levels, in [-1, 1]
// Quotes: bid = floor(fair - half_spread), ask = ceil(fair + half_spread),
// clamped so a quote never crosses the opposite touch and the two never
// cross each other. Position cap: a side is not quoted when filling it would
// take position plus working orders past `max_pos`.
//
// Quotes are moved with replace only when the target is `requote_ticks` or
// more away from the working price (hysteresis; every replace sends the order
// to the back of the queue, so moving is not free), and no more often than
// `min_interval_ns` per side.
//
// The strategy owns no order state. It reads its orders back from the OMS, so
// what it believes is what the gateway believes: a side is quotable only when
// the OMS says New or PartiallyFilled, and left alone while a request is in
// flight.

#include <cstdint>

#include "hft/bt/tape_book.hpp"
#include "hft/oms/oms.hpp"
#include "hft/sim/types.hpp"

namespace hft::strat {

struct MmConfig {
  std::int32_t half_spread16 = 20;     // half the quoted spread, 1/16 tick (20 = 1.25 ticks)
  std::int32_t alpha16 = 16;           // imbalance lean at I = +-1, 1/16 tick (16 = 1 tick)
  std::int32_t skew16 = 2;             // 1/16 tick per share of inventory
  std::int32_t depth_levels = 3;       // price levels in the imbalance
  std::int64_t qty = 5;                // quote size
  std::int64_t max_pos = 40;           // position cap incl. working orders
  std::int32_t requote_ticks = 1;      // move a quote when the target is this far away
  std::int64_t min_leaves = 2;         // refill a partly filled quote below this
  std::uint64_t min_interval_ns = 50'000;  // per side, between requests
};

struct MmStats {
  std::uint64_t updates = 0, new_sent = 0, replace_sent = 0, cancel_sent = 0, risk_blocked = 0;
};

class MarketMaker {
 public:
  explicit MarketMaker(const MmConfig& c = MmConfig{}) noexcept : c_(c) {}

  // Call after every market data event and every report. `GW` is a Gateway.
  template <class GW>
  void on_update(std::uint64_t now, const bt::TapeBook& book, GW& gw) noexcept {
    ++stats_.updates;
    if (!book.two_sided() || gw.risk().killed()) return;
    const std::int64_t bi = book.tick_index(book.best_bid());
    const std::int64_t ai = book.tick_index(book.best_ask());
    gw.risk().set_reference(book.price_at((bi + ai) / 2));

    const core::Qty qb = book.depth(core::Side::Buy, c_.depth_levels);
    const core::Qty qa = book.depth(core::Side::Sell, c_.depth_levels);
    const std::int64_t tot = qb + qa;
    const std::int64_t imb16 = tot > 0 ? (16 * (qb - qa)) / tot : 0;  // [-16, 16]
    const oms::Position& pos = gw.oms().position();
    const std::int64_t fair16 = (bi + ai) * 8 + (static_cast<std::int64_t>(c_.alpha16) * imb16) / 16 -
                                static_cast<std::int64_t>(c_.skew16) * pos.pos;

    std::int64_t bid = floor_div(fair16 - c_.half_spread16, 16);
    std::int64_t ask = ceil_div(fair16 + c_.half_spread16, 16);
    if (bid > ai - 1) bid = ai - 1;  // post-only: never take
    if (ask < bi + 1) ask = bi + 1;
    if (bid >= ask) ask = bid + 1;   // never lock or cross ourselves

    side(core::Side::Buy, bid, now, book, gw);
    side(core::Side::Sell, ask, now, book, gw);
  }

  const MmStats& stats() const noexcept { return stats_; }
  const MmConfig& config() const noexcept { return c_; }

 private:
  struct SideState {
    core::OrderId id = 0;
    std::uint64_t last = 0;
    bool ever = false;
  };

  static std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
  }
  static std::int64_t ceil_div(std::int64_t a, std::int64_t b) noexcept { return -floor_div(-a, b); }

  template <class GW>
  void side(core::Side s, std::int64_t target_tick, std::uint64_t now, const bt::TapeBook& book, GW& gw) noexcept {
    SideState& st = s == core::Side::Buy ? bid_ : ask_;
    const oms::Position& pos = gw.oms().position();
    const oms::OrderRec* o = st.id ? gw.oms().find(st.id) : nullptr;
    if (o && oms::is_terminal(o->state)) {
      st.id = 0;
      o = nullptr;
    }
    const bool due = !st.ever || now - st.last >= c_.min_interval_ns;

    // exposure of everything else working on this side, plus our post-fill position
    const std::int64_t others = (s == core::Side::Buy ? pos.open_buy : pos.open_sell) - (o ? o->reserved : 0);
    const std::int64_t held = s == core::Side::Buy ? pos.pos : -pos.pos;
    const bool want = held + others + c_.qty <= c_.max_pos;

    if (!o) {
      if (!want || !due) return;
      const auto r = gw.new_order(s, sim::OrderType::PostOnly, book.price_at(target_tick), c_.qty, now);
      st.last = now;
      st.ever = true;
      if (r.ok()) {
        st.id = r.id;
        ++stats_.new_sent;
      } else {
        ++stats_.risk_blocked;
      }
      return;
    }
    if (o->state != oms::State::New && o->state != oms::State::PartiallyFilled) return;  // a request is in flight
    if (!due) return;
    if (!want) {
      gw.cancel(o->id, now);
      st.last = now;
      ++stats_.cancel_sent;
      return;
    }
    const std::int64_t cur = book.tick_index(o->price);
    const std::int64_t d = cur > target_tick ? cur - target_tick : target_tick - cur;
    if (d >= c_.requote_ticks || o->leaves < c_.min_leaves) {
      const auto r = gw.replace(o->id, book.price_at(target_tick), c_.qty, now);
      st.last = now;
      if (r.ok()) {
        ++stats_.replace_sent;
      } else {
        ++stats_.risk_blocked;
      }
    }
  }

  MmConfig c_;
  SideState bid_, ask_;
  MmStats stats_;
};

}  // namespace hft::strat
