#pragma once
// Pre-trade risk. Every order the strategy wants to send, and every replace,
// passes through check_new()/check_replace() before it reaches the OMS or the
// wire. Cancels never do: you must always be able to get out.
//
// Checks, in the order they are evaluated (the first failure is reported):
//   1. kill switch latched
//   2. loss limit (marks the position at the reference price; trips the kill
//      switch and stays tripped)
//   3. malformed parameters
//   4. max order quantity (fat finger)
//   5. a reference price is needed (market orders, price band, loss limit)
//   6. max order notional (market orders are valued at the reference price)
//   7. price band around the reference price, in basis points
//   8. max position, counting everything working: pos + open buys + this
//      order must stay <= limit, and likewise on the sell side
//   9. max open orders (new orders only)
//  10. order rate limit (token bucket, driven by the caller's clock)
// The rate limiter is last on purpose: an order refused for any other reason
// does not spend a token. Replaces that only shrink an order skip the
// position check, because they cannot increase exposure.
//
// Everything is integer arithmetic on caller-supplied state and time. The
// engine allocates nothing, throws nothing, and has no branches that depend
// on anything but its inputs, so a recorded session replays identically.

#include <cstdint>

#include "hft/core/fixed_point.hpp"
#include "hft/oms/oms.hpp"

namespace hft::risk {

using oms::Position;
using sim::OrderType;
using sim::Price;
using sim::Qty;
using sim::Side;

enum class RiskResult : std::uint8_t {
  Ok = 0,
  KillSwitch,
  MaxLoss,
  BadParams,
  MaxOrderQty,
  NoReference,
  MaxNotional,
  PriceBand,
  MaxPosition,
  MaxOpenOrders,
  RateLimit,
};

enum class KillReason : std::uint8_t { None = 0, Manual, MaxLoss, SessionGap, Disconnect };

struct RiskConfig {
  Qty max_order_qty = 1000;
  std::int64_t max_order_notional_raw = 50'000'000'000LL;  // price*qty in raw price units
  std::int64_t price_band_bps = 500;                       // +-5% around the reference
  Qty max_position = 5000;
  std::uint32_t max_open_orders = 200;
  std::uint64_t rate_per_sec = 100'000;  // sustained orders per second
  std::uint32_t burst = 100;             // bucket size
  std::int64_t max_loss_raw = 0;         // trip when mark-to-market PnL < -max_loss_raw; 0 disables
};

class RiskEngine {
 public:
  void init(const RiskConfig& c) noexcept {
    cfg_ = c;
    tokens_ = static_cast<std::uint64_t>(c.burst) * kUnit;
    last_ns_ = 0;
    started_ = false;
    ref_ = Price::invalid();
    kill_ = KillReason::None;
  }

  void set_reference(Price p) noexcept { ref_ = p; }
  Price reference() const noexcept { return ref_; }

  void kill(KillReason r) noexcept {
    if (kill_ == KillReason::None) kill_ = r;  // keep the first cause
  }
  bool killed() const noexcept { return kill_ != KillReason::None; }
  KillReason kill_reason() const noexcept { return kill_; }
  // Deliberate human action only; nothing in the system calls this by itself.
  void reset_kill() noexcept { kill_ = KillReason::None; }

  RiskResult check_new(Side side, OrderType type, Price price, Qty qty, const Position& pos,
                       std::uint64_t now_ns) noexcept {
    if (kill_ != KillReason::None) return RiskResult::KillSwitch;
    if (!loss_ok(pos)) return RiskResult::MaxLoss;
    const bool priced = type != OrderType::Market;
    if (qty <= 0 || (priced && price.raw() <= 0)) return RiskResult::BadParams;
    if (qty > cfg_.max_order_qty) return RiskResult::MaxOrderQty;
    if ((!priced || cfg_.price_band_bps > 0) && ref_ == Price::invalid()) return RiskResult::NoReference;
    const Price px = priced ? price : ref_;
    if (core::notional_raw(px, qty) > cfg_.max_order_notional_raw) return RiskResult::MaxNotional;
    if (priced && cfg_.price_band_bps > 0 && !core::within_bps(price, ref_, cfg_.price_band_bps))
      return RiskResult::PriceBand;
    if (!position_ok(side, pos, qty)) return RiskResult::MaxPosition;
    if (pos.open_orders >= cfg_.max_open_orders) return RiskResult::MaxOpenOrders;
    return take_token(now_ns) ? RiskResult::Ok : RiskResult::RateLimit;
  }

  // `leaves` is the order's current open quantity.
  RiskResult check_replace(Side side, Price new_price, Qty new_qty, Qty leaves, const Position& pos,
                           std::uint64_t now_ns) noexcept {
    if (kill_ != KillReason::None) return RiskResult::KillSwitch;
    if (!loss_ok(pos)) return RiskResult::MaxLoss;
    if (new_qty <= 0 || new_price.raw() <= 0) return RiskResult::BadParams;
    if (new_qty > cfg_.max_order_qty) return RiskResult::MaxOrderQty;
    if (cfg_.price_band_bps > 0 && ref_ == Price::invalid()) return RiskResult::NoReference;
    if (core::notional_raw(new_price, new_qty) > cfg_.max_order_notional_raw) return RiskResult::MaxNotional;
    if (cfg_.price_band_bps > 0 && !core::within_bps(new_price, ref_, cfg_.price_band_bps))
      return RiskResult::PriceBand;
    (void)leaves;  // fills may land before the replace does, so the full new size needs headroom
    if (!position_ok(side, pos, new_qty)) return RiskResult::MaxPosition;
    return take_token(now_ns) ? RiskResult::Ok : RiskResult::RateLimit;
  }

  const RiskConfig& config() const noexcept { return cfg_; }

 private:
  static constexpr std::uint64_t kUnit = 1'000'000'000ULL;  // one token, in bucket units

  bool loss_ok(const Position& pos) noexcept {
    if (cfg_.max_loss_raw <= 0 || ref_ == Price::invalid()) return true;
    if (pos.mark_pnl(ref_) < -static_cast<core::i128>(cfg_.max_loss_raw)) {
      kill(KillReason::MaxLoss);
      return false;
    }
    return true;
  }

  // Worst case if every working order and this one fill.
  bool position_ok(Side side, const Position& pos, Qty add) const noexcept {
    if (side == Side::Buy) return pos.pos + pos.open_buy + add <= cfg_.max_position;
    return pos.pos - pos.open_sell - add >= -cfg_.max_position;
  }

  // Bucket units: one token = 1e9. Refill adds elapsed_ns * rate_per_sec
  // units, which is exactly rate/1e9 tokens per nanosecond with no division.
  bool take_token(std::uint64_t now_ns) noexcept {
    const std::uint64_t cap = static_cast<std::uint64_t>(cfg_.burst) * kUnit;
    if (!started_) {
      started_ = true;
      last_ns_ = now_ns;
    } else if (now_ns > last_ns_) {
      std::uint64_t dt = now_ns - last_ns_;
      // Clamp so dt*rate cannot overflow: beyond cap/rate ns the bucket is full anyway.
      const std::uint64_t full_after = cfg_.rate_per_sec ? cap / cfg_.rate_per_sec + 1 : 0;
      if (dt > full_after) dt = full_after;
      const std::uint64_t add = dt * cfg_.rate_per_sec;
      tokens_ = (tokens_ + add > cap || tokens_ + add < tokens_) ? cap : tokens_ + add;
      last_ns_ = now_ns;
    }  // a clock that goes backwards neither refills nor refunds
    if (tokens_ < kUnit) return false;
    tokens_ -= kUnit;
    return true;
  }

  RiskConfig cfg_{};
  Price ref_{Price::invalid()};
  KillReason kill_{KillReason::None};
  std::uint64_t tokens_{0}, last_ns_{0};
  bool started_{false};
};

}  // namespace hft::risk
