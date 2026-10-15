#pragma once
// Backtest accounting. Everything in the PnL identity is exact integer
// arithmetic in half raw price units (so a half-tick mid stays an integer):
//
//   V2 = 2*cash + pos*mid2            (mark-to-market value, doubled)
//
//   at a fill:       dV2 = side*qty*(mid2 - 2*price) - 2*fee*qty
//   at a mid change: dV2 = pos*(mid2_new - mid2_old)
//
// so  V2(end) = edge2 + inventory2 + fees2  exactly, with
//   edge2       spread capture: how far from the mid each fill was priced
//   inventory2  what the position earned or lost as the mid moved
//   fees2       fees paid (rebates are negative fees)
// The tests assert that equality to the last unit.
//
// Adverse selection is measured per fill against the mid H nanoseconds later:
//   realized spread = side*(mid_{t+H} - price), the edge actually kept
//   adverse move    = side*(mid_{t+H} - mid_t), negative when the market moved
//                     against us after the fill
// all in ticks per share, quantity weighted.

#include <cmath>
#include <cstdint>

#include "hft/core/fixed_point.hpp"
#include "hft/core/memory.hpp"

namespace hft::bt {

constexpr int kHorizons = 3;

struct MetricsConfig {
  std::int64_t tick_raw = 100;
  std::int64_t fee_raw_per_unit = 0;                // paid per share filled; negative = rebate
  std::uint64_t bin_ns = 1'000'000;                 // PnL sampling interval for Sharpe
  std::uint64_t horizons_ns[kHorizons] = {100'000, 1'000'000, 10'000'000};
  double bins_per_year = 252.0 * 6.5 * 3600.0 * 1e9 / 1e6;  // for bin_ns = 1 ms, a 6.5h day, 252 days
};

struct Fill {
  std::uint64_t ts;
  std::int64_t price;
  std::int64_t qty;
  std::int64_t mid2;
  std::int8_t side;  // +1 buy, -1 sell
};

class Metrics {
 public:
  using i128 = core::i128;

  bool init(core::Arena& arena, const MetricsConfig& c) noexcept {
    ring_ = arena.allocate_array<Fill>(kCap);
    if (!ring_) return false;
    c_ = c;
    started_ = mid_valid_ = seen_mid_ = false;
    last_t_ = next_bin_ = final_t_ = 0;
    mid2_ = first_mid2_ = pos_ = max_pos_ = qty_ = 0;
    cash_ = edge2_ = inventory2_ = fees2_ = notional_ = 0;
    fills_ = dropped_ = 0;
    digest_ = 1469598103934665603ULL;
    pending_n_ = 0;
    for (int h = 0; h < kHorizons; ++h) {
      cursor_[h] = 0;
      real2_[h] = adv2_[h] = 0;
      qh_[h] = 0;
    }
    bin_v2_ = peak2_ = max_dd2_ = 0;
    nb_ = 0;
    mean_ = m2_ = 0;
    return true;
  }

  // Call before the market state changes at time `t` (it settles everything
  // that was due by then using the state still in force).
  void before_event(std::uint64_t t) noexcept {
    if (!started_) {
      started_ = true;
      next_bin_ = t + c_.bin_ns;
      last_t_ = t;
    }
    settle(t);
    last_t_ = t;
  }

  // The mid after a tape event (valid = both sides present).
  void on_mid(bool valid, std::int64_t mid2) noexcept {
    if (!valid) return;
    if (mid_valid_) inventory2_ += static_cast<i128>(pos_) * (mid2 - mid2_);
    mid2_ = mid2;
    mid_valid_ = true;
    if (!seen_mid_) {
      seen_mid_ = true;
      first_mid2_ = mid2;
    }
  }

  void on_fill(std::uint64_t t, std::int8_t side, std::int64_t price, std::int64_t qty) noexcept {
    if (!mid_valid_) return;  // cannot value a fill with no mid (does not happen: fills need a book)
    const i128 s = side;
    edge2_ += s * qty * (mid2_ - 2 * price);
    fees2_ -= static_cast<i128>(2) * c_.fee_raw_per_unit * qty;
    cash_ -= s * price * qty + static_cast<i128>(c_.fee_raw_per_unit) * qty;
    pos_ += side * qty;
    if (pos_ > max_pos_) max_pos_ = pos_;
    if (-pos_ > max_pos_) max_pos_ = -pos_;
    ++fills_;
    qty_ += qty;
    notional_ += static_cast<i128>(price) * qty;
    // order-sensitive digest of the fill sequence
    digest_ = mix(digest_, t);
    digest_ = mix(digest_, static_cast<std::uint64_t>(price));
    digest_ = mix(digest_, static_cast<std::uint64_t>(qty * side));
    if (pending_n_ - tail_min() >= kCap) ++dropped_;  // oldest sample overwritten before use
    ring_[pending_n_ % kCap] = Fill{t, price, qty, mid2_, side};
    ++pending_n_;
  }

  // End of the replay: settle open horizons and bins with the final mid.
  void finish(std::uint64_t t_end) noexcept {
    settle(t_end);
    for (int h = 0; h < kHorizons; ++h)
      while (cursor_[h] < pending_n_) resolve(h);
    final_t_ = t_end;
  }

  // ---- results ----
  i128 value2() const noexcept { return static_cast<i128>(2) * cash_ + static_cast<i128>(pos_) * mid2_; }
  i128 edge2() const noexcept { return edge2_; }
  i128 inventory2() const noexcept { return inventory2_; }
  i128 fees2() const noexcept { return fees2_; }
  i128 cash() const noexcept { return cash_; }
  std::int64_t pos() const noexcept { return pos_; }
  std::int64_t max_abs_pos() const noexcept { return max_pos_; }
  std::int64_t mid2() const noexcept { return mid2_; }
  std::uint64_t fills() const noexcept { return fills_; }
  std::int64_t qty() const noexcept { return qty_; }
  std::uint64_t dropped_samples() const noexcept { return dropped_; }
  std::uint64_t digest() const noexcept { return digest_; }
  double notional_units() const noexcept { return static_cast<double>(notional_) / static_cast<double>(core::Price::kScale); }

  std::uint64_t horizon_ns(int h) const noexcept { return c_.horizons_ns[h]; }

  double to_units(i128 x2) const noexcept {  // doubled raw -> currency units
    return static_cast<double>(x2) / 2.0 / static_cast<double>(core::Price::kScale);
  }
  double max_drawdown_units() const noexcept { return to_units(max_dd2_); }

  // Per-bin statistics of the PnL increments (currency units).
  std::uint64_t bins() const noexcept { return nb_; }
  double bin_mean() const noexcept { return mean_; }
  double bin_std() const noexcept { return nb_ > 1 ? std::sqrt(m2_ / static_cast<double>(nb_ - 1)) : 0.0; }
  double sharpe_per_bin() const noexcept {
    const double s = bin_std();
    return s > 0 ? mean_ / s : 0.0;
  }
  double sharpe_annualized() const noexcept { return sharpe_per_bin() * std::sqrt(c_.bins_per_year); }

  // Ticks per share. realized(h): edge kept after h; adverse(h): post-fill move (negative is bad).
  double effective_edge_ticks() const noexcept { return per_tick(edge2_, qty_); }
  double realized_ticks(int h) const noexcept { return per_tick(real2_[h], qh_[h]); }
  double adverse_ticks(int h) const noexcept { return per_tick(adv2_[h], qh_[h]); }

 private:
  static constexpr std::uint64_t kCap = 1u << 14;

  static std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h * 0x100000001B3ULL;
  }
  double per_tick(i128 x2, std::int64_t q) const noexcept {
    return q > 0 ? static_cast<double>(x2) / 2.0 / static_cast<double>(c_.tick_raw) / static_cast<double>(q) : 0.0;
  }
  std::uint64_t tail_min() const noexcept {
    std::uint64_t m = cursor_[0];
    for (int h = 1; h < kHorizons; ++h)
      if (cursor_[h] < m) m = cursor_[h];
    return m;
  }

  void resolve(int h) noexcept {
    const Fill& f = ring_[cursor_[h] % kCap];
    real2_[h] += static_cast<i128>(f.side) * f.qty * (mid2_ - 2 * f.price);
    adv2_[h] += static_cast<i128>(f.side) * f.qty * (mid2_ - f.mid2);
    qh_[h] += f.qty;
    ++cursor_[h];
  }

  void settle(std::uint64_t t) noexcept {
    for (int h = 0; h < kHorizons; ++h) {
      if (cursor_[h] + kCap < pending_n_) cursor_[h] = pending_n_ - kCap;  // lost to overflow, counted in dropped_
      while (cursor_[h] < pending_n_ && ring_[cursor_[h] % kCap].ts + c_.horizons_ns[h] <= t) resolve(h);
    }
    while (started_ && next_bin_ <= t) {
      const i128 v = value2();
      const double d = to_units(v - bin_v2_);
      bin_v2_ = v;
      ++nb_;
      const double delta = d - mean_;
      mean_ += delta / static_cast<double>(nb_);
      m2_ += delta * (d - mean_);
      if (v > peak2_) peak2_ = v;
      if (peak2_ - v > max_dd2_) max_dd2_ = peak2_ - v;
      next_bin_ += c_.bin_ns;
    }
  }

  MetricsConfig c_{};
  bool started_ = false, mid_valid_ = false, seen_mid_ = false;
  std::uint64_t last_t_ = 0, next_bin_ = 0, final_t_ = 0;
  std::int64_t mid2_ = 0, first_mid2_ = 0, pos_ = 0, max_pos_ = 0, qty_ = 0;
  i128 cash_ = 0, edge2_ = 0, inventory2_ = 0, fees2_ = 0, notional_ = 0;
  std::uint64_t fills_ = 0, dropped_ = 0, digest_ = 1469598103934665603ULL;
  Fill* ring_ = nullptr;
  std::uint64_t pending_n_ = 0, cursor_[kHorizons] = {};
  i128 real2_[kHorizons] = {}, adv2_[kHorizons] = {};
  std::int64_t qh_[kHorizons] = {};
  i128 bin_v2_ = 0, peak2_ = 0, max_dd2_ = 0;
  std::uint64_t nb_ = 0;
  double mean_ = 0, m2_ = 0;
};

}  // namespace hft::bt
