#pragma once
// Strategy lab (ADR 33): run market maker parameter variants over the same
// generated markets and compare them with paired statistics. Offline only;
// uses the heap, strings and stdio freely and is never linked into the hot path.
//
//   variant spec   "name:key=value,key=value"   (keys are MmConfig fields)
//   paired diff    variant minus base on the same market, one number per market
//   sign test      exact two-sided binomial test on the signs of those numbers
//
// What the numbers can and cannot say is in docs/STRATEGY_LAB.md. In short:
// the fills are simulated (queue model, no market impact), the markets come
// from one generator, and a comparison of K variants makes K tests that are
// not adjusted for each other.

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "hft/bt/backtest.hpp"
#include "hft/bt/market_gen.hpp"
#include "hft/strat/market_maker.hpp"

namespace hft::bt::lab {

struct Variant {
  std::string name;
  strat::MmConfig cfg;
};

inline bool parse_int(std::string_view s, long long& out) {
  if (s.empty()) return false;
  if (!((s[0] >= '0' && s[0] <= '9') || s[0] == '-')) return false;  // strtoll would skip spaces and accept '+'
  const std::string t(s);
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(t.c_str(), &end, 10);
  if (errno != 0 || end != t.c_str() + t.size()) return false;
  out = v;
  return true;
}

// Sets one MmConfig field by name. Rejects unknown keys, non-integers and values the strategy or the risk
// limits of the lab cannot honour (the message says which).
inline bool apply_param(strat::MmConfig& c, std::string_view key, std::string_view val, std::string& err) {
  long long v = 0;
  if (!parse_int(val, v)) {
    err = "value of " + std::string(key) + " is not an integer: '" + std::string(val) + "'";
    return false;
  }
  auto range = [&](long long lo, long long hi) {
    if (v < lo || v > hi) {
      err = std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi);
      return false;
    }
    return true;
  };
  if (key == "half_spread16") return range(0, 1 << 20) && ((c.half_spread16 = static_cast<std::int32_t>(v)), true);
  if (key == "alpha16") return range(-(1 << 20), 1 << 20) && ((c.alpha16 = static_cast<std::int32_t>(v)), true);
  if (key == "skew16") return range(0, 1 << 20) && ((c.skew16 = static_cast<std::int32_t>(v)), true);
  if (key == "depth_levels") return range(1, 64) && ((c.depth_levels = static_cast<std::int32_t>(v)), true);
  if (key == "qty") return range(1, 100) && ((c.qty = v), true);
  if (key == "max_pos") return range(1, 10000) && ((c.max_pos = v), true);
  if (key == "requote_ticks") return range(0, 1000) && ((c.requote_ticks = static_cast<std::int32_t>(v)), true);
  if (key == "min_leaves") return range(0, 100) && ((c.min_leaves = v), true);
  if (key == "min_interval_ns") return range(0, 1'000'000'000) && ((c.min_interval_ns = static_cast<std::uint64_t>(v)), true);
  err = "unknown parameter '" + std::string(key) +
        "' (half_spread16 alpha16 skew16 depth_levels qty max_pos requote_ticks min_leaves min_interval_ns)";
  return false;
}

inline bool valid_name(std::string_view n) {
  if (n.empty() || n.size() > 48) return false;
  for (const char ch : n) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '=' ||
                    ch == '.' || ch == '-';
    if (!ok) return false;
  }
  return true;
}

// "name:key=value,key=value" over `base`. A name alone is a copy of `base`.
inline bool parse_variant(std::string_view spec, const strat::MmConfig& base, Variant& out, std::string& err) {
  const std::size_t colon = spec.find(':');
  const std::string_view name = spec.substr(0, colon);
  if (!valid_name(name)) {
    err = "bad variant name '" + std::string(name) + "' (letters, digits and _ = . - only, up to 48)";
    return false;
  }
  out.name = std::string(name);
  out.cfg = base;
  if (colon == std::string_view::npos) return true;
  std::string_view rest = spec.substr(colon + 1);
  if (rest.empty()) {
    err = "variant '" + out.name + "' has a colon but no parameters";
    return false;
  }
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view kv = rest.substr(0, comma);
    const std::size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
      err = "expected key=value, got '" + std::string(kv) + "'";
      return false;
    }
    if (!apply_param(out.cfg, kv.substr(0, eq), kv.substr(eq + 1), err)) return false;
    if (comma == std::string_view::npos) break;
    rest = rest.substr(comma + 1);
    if (rest.empty()) {
      err = "trailing comma in variant '" + out.name + "'";
      return false;
    }
  }
  return true;
}

// ---- statistics ------------------------------------------------------------------------------------------------

struct Summary {
  std::size_t n = 0;
  double mean = 0, sd = 0, se = 0;
};

inline Summary summarize(const std::vector<double>& x) {
  Summary s;
  s.n = x.size();
  if (s.n == 0) return s;
  double sum = 0;
  for (const double v : x) sum += v;
  s.mean = sum / static_cast<double>(s.n);
  if (s.n > 1) {
    double ss = 0;
    for (const double v : x) ss += (v - s.mean) * (v - s.mean);
    s.sd = std::sqrt(ss / static_cast<double>(s.n - 1));
    s.se = s.sd / std::sqrt(static_cast<double>(s.n));
  }
  return s;
}

// Exact two-sided sign test: with `wins` and `losses` among the non-tied pairs, the chance of a split at least this
// uneven if each sign were a fair coin.
inline double sign_test_p(std::size_t wins, std::size_t losses) {
  const std::size_t n = wins + losses;
  if (n == 0) return 1.0;
  const std::size_t k = wins < losses ? wins : losses;
  double tail = 0;
  for (std::size_t i = 0; i <= k; ++i) {
    const double lg = std::lgamma(static_cast<double>(n) + 1) - std::lgamma(static_cast<double>(i) + 1) -
                      std::lgamma(static_cast<double>(n - i) + 1) - static_cast<double>(n) * std::log(2.0);
    tail += std::exp(lg);
  }
  const double p = 2 * tail;
  return p > 1.0 ? 1.0 : p;
}

// p printed to four places, with values that would round to zero shown as a bound.
inline std::string format_p(double p) {
  if (p < 0.0001) return "<0.0001";
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", p);
  return b;
}

struct Paired {
  std::size_t n = 0, wins = 0, losses = 0, ties = 0;
  double mean = 0, se = 0, t = 0, p_sign = 1;
  bool t_defined = false;
};

// variant[i] - base[i] over markets i. Both vectors must have the same length.
inline Paired paired(const std::vector<double>& variant, const std::vector<double>& base) {
  Paired r;
  std::vector<double> d;
  for (std::size_t i = 0; i < variant.size() && i < base.size(); ++i) {
    const double x = variant[i] - base[i];
    d.push_back(x);
    if (x > 0) ++r.wins;
    else if (x < 0) ++r.losses;
    else ++r.ties;
  }
  const Summary s = summarize(d);
  r.n = s.n;
  r.mean = s.mean;
  r.se = s.se;
  if (s.n >= 2) {
    r.t_defined = true;
    if (s.se > 0) r.t = s.mean / s.se;
    else r.t = s.mean == 0 ? 0.0 : (s.mean > 0 ? HUGE_VAL : -HUGE_VAL);
  }
  r.p_sign = sign_test_p(r.wins, r.losses);
  return r;
}

// ---- event log -------------------------------------------------------------------------------------------------

// Writes the venue's view of a run in the event format of hft_live, so scripts/pnl_report.py reads it unchanged:
//   fill  ts_ms side px qty mid_x2 pos applied   (px raw 1e-4 units, qty in shares, so run the report with --qty-scale 1)
//   mid   ts_ms mid_x2                           (the last mid of each millisecond of market time, plus the final one)
// The mid is the venue's, not the strategy's delayed view. Time resolution is a millisecond.
class EventObs {
 public:
  void open(std::FILE* f) noexcept {
    f_ = f;
    have_mid_ = false;
  }
  void md(std::uint64_t, std::size_t) noexcept {}
  void report(std::uint64_t, const std::uint8_t*, std::size_t) noexcept {}
  void sent(std::uint64_t, const std::uint8_t*, std::size_t) noexcept {}

  void fill(std::uint64_t ts, std::int8_t side, std::int64_t px, std::int64_t qty, std::int64_t mid2, std::int64_t pos) noexcept {
    if (!f_) return;
    std::fprintf(f_, "{\"ts_ms\":%llu,\"event\":\"fill\",\"seg\":1,\"side\":\"%s\",\"px\":%lld,\"qty\":%lld,\"mid_x2\":%lld,\"pos\":%lld,\"applied\":1}\n",
                 static_cast<unsigned long long>(ts / 1'000'000), side > 0 ? "buy" : "sell", static_cast<long long>(px),
                 static_cast<long long>(qty), static_cast<long long>(mid2), static_cast<long long>(pos));
  }

  void mid(std::uint64_t ts, bool valid, std::int64_t mid2) noexcept {
    if (!f_ || !valid) return;
    const std::uint64_t ms = ts / 1'000'000;
    if (have_mid_ && ms != mid_ms_) write_mid();
    mid_ms_ = ms;
    mid2_ = mid2;
    have_mid_ = true;
  }

  void finish() noexcept {
    if (f_ && have_mid_) write_mid();
    have_mid_ = false;
  }

 private:
  void write_mid() noexcept {
    std::fprintf(f_, "{\"ts_ms\":%llu,\"event\":\"mid\",\"mid_x2\":%lld}\n", static_cast<unsigned long long>(mid_ms_),
                 static_cast<long long>(mid2_));
  }
  std::FILE* f_ = nullptr;
  bool have_mid_ = false;
  std::uint64_t mid_ms_ = 0;
  std::int64_t mid2_ = 0;
};

// ---- one run ---------------------------------------------------------------------------------------------------

struct RunResult {
  bool ok = false;        // set up and started
  bool finished = false;  // the replay ended by itself (false: the step budget ran out, see run_one)
  BtReport rep;
};

inline risk::RiskConfig lab_risk(std::int64_t max_pos) {
  risk::RiskConfig r;
  r.max_order_qty = 100;  // apply_param caps the quote size at 100
  r.max_order_notional_raw = 100'000'000'000LL;
  r.price_band_bps = 2000;
  r.max_position = max_pos < 40 ? 80 : 2 * max_pos;  // the strategy's cap binds, not the risk limit
  r.max_open_orders = 10;
  r.rate_per_sec = 1'000'000;
  r.burst = 50;
  return r;
}

// Runs one strategy over one market. `events` may be null.
inline RunResult run_one(const Market& m, BtConfig c, const strat::MmConfig& sc, const risk::RiskConfig& rc, std::FILE* events) {
  RunResult out;
  c.risk = rc;
  c.tick_raw = m.cfg.tick_raw;
  book::BookConfig bc;
  bc.base = m.cfg.base;
  bc.tick_raw = m.cfg.tick_raw;
  bc.num_levels = m.cfg.num_levels;
  core::Region region = core::Region::create(128u << 20, core::RegionOptions{false, false, true, -1});
  core::Arena arena(region);
  strat::MarketMaker mm(sc);
  auto* b = new Backtester<strat::MarketMaker, EventObs>();
  if (b->init(arena, m.tape.data(), m.tape.size(), bc, c, &mm)) {
    b->observer().open(events);
    // Normal runs take about two steps per tape event plus a few messages per event. A budget of 20 per event plus a
    // million is far above that and stops a requote loop (replace answered by replace, with no pacing) from hanging.
    out.finished = b->run(20 * static_cast<std::uint64_t>(m.tape.size()) + 1'000'000);
    b->observer().finish();
    out.rep = b->report();
    out.ok = true;
  }
  delete b;
  return out;
}

// Anything that makes a run untrustworthy; empty when the run is clean.
inline std::string health_problem(const BtReport& r) {
  std::string s;
  auto add = [&](const char* what, std::uint64_t n) {
    if (n) s += std::string(s.empty() ? "" : ", ") + what + " " + std::to_string(n);
  };
  add("oms anomalies", r.oms_anomalies);
  add("oms mismatches", r.oms_mismatches);
  add("queue inconsistencies", r.queue_inconsistent);
  add("queue overflows", r.queue_overflows);
  add("tape errors", r.tape_errors);
  add("dropped samples", r.dropped_samples);
  add("kill switch reason", r.kill_reason);
  return s;
}

inline std::string run_problem(const RunResult& r) {
  std::string s = health_problem(r.rep);
  if (!r.finished) s = std::string("did not finish within the step budget (a requote loop?)") + (s.empty() ? "" : ", ") + s;
  return s;
}

}  // namespace hft::bt::lab
