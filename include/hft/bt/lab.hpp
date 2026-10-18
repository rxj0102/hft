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

#include <algorithm>
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

// The current value of a field by name; the counterpart of apply_param.
inline bool get_param(const strat::MmConfig& c, std::string_view key, long long& out) {
  if (key == "half_spread16") out = c.half_spread16;
  else if (key == "alpha16") out = c.alpha16;
  else if (key == "skew16") out = c.skew16;
  else if (key == "depth_levels") out = c.depth_levels;
  else if (key == "qty") out = c.qty;
  else if (key == "max_pos") out = c.max_pos;
  else if (key == "requote_ticks") out = c.requote_ticks;
  else if (key == "min_leaves") out = c.min_leaves;
  else if (key == "min_interval_ns") out = static_cast<long long>(c.min_interval_ns);
  else return false;
  return true;
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

// ---- many markets, many variants ------------------------------------------------------------------------------

inline std::uint64_t mix64(std::uint64_t h, std::uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return h * 0x100000001B3ULL;
}
constexpr std::uint64_t kDigestSeed = 1469598103934665603ULL;

// One variant's results, one entry per market.
struct Row {
  std::vector<double> pnl, edge, inventory, fees, fill_rate, adverse1ms, sharpe;   // pnl is after fees: pnl = edge + inventory - fees
  std::vector<std::uint64_t> fills, digest, risk_rejects;
  std::vector<std::int64_t> max_pos;
};

// Named market regimes: settings of the existing generator, nothing new in the generator itself. "default" is the generator's
// own configuration, so a run without --regimes is unchanged. The others move one thing at a time.
struct Regime {
  const char* name;
  const char* what;
  std::uint32_t jump_period, target_orders, informed_pct, skew_pct;
  std::uint64_t mean_gap_ns;
};

inline constexpr Regime kRegimes[] = {
    {"default", "the generator's own settings", 400, 200, 40, 35, 10'000},
    {"volatile", "the fair value jumps four times as often", 100, 200, 40, 35, 10'000},
    {"quiet", "the fair value jumps a quarter as often", 1600, 200, 40, 35, 10'000},
    {"informed", "more informed flow and a stronger lean (more adverse selection)", 400, 200, 70, 55, 10'000},
    {"uninformed", "little informed flow and a weak lean (a weak signal)", 400, 200, 10, 10, 10'000},
    {"thin", "a third of the resting orders (a thin book)", 400, 60, 40, 35, 10'000},
    {"slow", "requests four times as far apart (a slow tape)", 400, 200, 40, 35, 40'000},
};

inline const Regime* find_regime(std::string_view name) {
  for (const Regime& r : kRegimes)
    if (name == r.name) return &r;
  return nullptr;
}

inline MarketConfig regime_config(const Regime& r) {
  MarketConfig c;
  c.jump_period = r.jump_period;
  c.target_orders = r.target_orders;
  c.informed_pct = r.informed_pct;
  c.skew_pct = r.skew_pct;
  c.mean_gap_ns = r.mean_gap_ns;
  return c;
}

// "a,b,c" -> regimes, in order. Unknown names, empty names and repeats are errors (err says which).
inline bool parse_regimes(std::string_view spec, std::vector<const Regime*>& out, std::string& err) {
  out.clear();
  std::size_t pos = 0;
  while (pos <= spec.size()) {
    const std::size_t comma = spec.find(',', pos);
    const std::string_view name = spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    const Regime* r = find_regime(name);
    if (!r) {
      err = "unknown regime '" + std::string(name) + "'";
      return false;
    }
    for (const Regime* q : out)
      if (q == r) {
        err = "regime '" + std::string(name) + "' listed twice";
        return false;
      }
    out.push_back(r);
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return true;
}

// How one variant did against the base in every regime: the mean PnL difference per regime, in how many regimes it is above
// zero, and the worst one. `per_regime[r][v]` is variant v's row in regime r; row 0 of each is the base.
struct AcrossRegimes {
  std::vector<double> d;
  std::size_t better = 0;
  double worst = 0;
};

inline AcrossRegimes across_regimes(const std::vector<std::vector<Row>>& per_regime, std::size_t vi) {
  AcrossRegimes a;
  for (const std::vector<Row>& rows : per_regime) a.d.push_back(paired(rows[vi].pnl, rows[0].pnl).mean);
  a.worst = a.d.empty() ? 0.0 : a.d[0];
  for (const double x : a.d) {
    if (x > 0) ++a.better;
    if (x < a.worst) a.worst = x;
  }
  return a;
}

// Latency sweep: the same variants and markets run once per delay. A delay is applied to one hop or to all three
// (market data in, order in, report out); jitter is left as configured.
enum class LatencyAxis { All, Md, Ingress, Egress };

inline const char* axis_name(LatencyAxis a) {
  switch (a) {
    case LatencyAxis::All: return "all";
    case LatencyAxis::Md: return "md";
    case LatencyAxis::Ingress: return "ingress";
    case LatencyAxis::Egress: return "egress";
  }
  return "all";
}

inline bool parse_latency_axis(std::string_view s, LatencyAxis& out) {
  for (const LatencyAxis a : {LatencyAxis::All, LatencyAxis::Md, LatencyAxis::Ingress, LatencyAxis::Egress})
    if (s == axis_name(a)) {
      out = a;
      return true;
    }
  return false;
}

inline BtConfig with_latency(BtConfig bc, LatencyAxis axis, long long us) {
  const std::uint64_t ns = static_cast<std::uint64_t>(us) * 1000;
  if (axis == LatencyAxis::All || axis == LatencyAxis::Md) bc.md_ns = ns;
  if (axis == LatencyAxis::All || axis == LatencyAxis::Ingress) bc.ingress_ns = ns;
  if (axis == LatencyAxis::All || axis == LatencyAxis::Egress) bc.egress_ns = ns;
  return bc;
}

// "0,5,20" -> microseconds, strictly increasing, each 0..100000, at most 12 (err says what is wrong).
inline bool parse_latencies(std::string_view spec, std::vector<long long>& out, std::string& err) {
  out.clear();
  std::size_t pos = 0;
  while (pos <= spec.size()) {
    const std::size_t comma = spec.find(',', pos);
    const std::string tok(spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    long long v = 0;
    if (!parse_int(tok, v) || v < 0 || v > 100000) {
      err = "latency '" + tok + "' is not an integer between 0 and 100000 microseconds";
      return false;
    }
    if (!out.empty() && v <= out.back()) {
      err = "latencies must be strictly increasing ('" + tok + "' follows " + std::to_string(out.back()) + ")";
      return false;
    }
    if (out.size() == 12) {
      err = "at most 12 latencies";
      return false;
    }
    out.push_back(v);
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return true;
}

// Fee in currency units for a run that traded `notional` currency units, at `bps` basis points of notional (negative is a
// rebate). Every fill in a lab run is a maker fill (the strategy only sends post-only orders), so one rate is enough.
inline double fee_for(double notional, double bps) { return notional * bps / 1e4; }

// Runs every variant on `markets` generated markets with seeds seed, seed+1, ... and fills `rows` (one per variant,
// rebuilt on each call). PnL in the rows is after fees at `maker_fee_bps`. Markets are generated from `proto` (its requests and seed are replaced). Unhealthy runs are reported on stderr and counted. Returns 0, or 2 when an event file
// cannot be written or the backtester cannot be set up. `tag` prefixes event file names.
inline int evaluate(const std::vector<Variant>& vars, long long seed, long long markets, long long requests, const BtConfig& bc,
                    const risk::RiskConfig& rc, const MarketConfig& proto, double maker_fee_bps, const std::string& events_dir, const std::string& tag,
                    std::vector<Row>& rows, std::uint64_t& digest, int& unhealthy) {
  rows.assign(vars.size(), Row{});
  for (long long mi = 0; mi < markets; ++mi) {
    MarketConfig mc = proto;
    mc.requests = static_cast<std::uint64_t>(requests);
    mc.seed = static_cast<std::uint64_t>(seed + mi);
    const Market m = MarketGen::generate(mc);
    for (std::size_t vi = 0; vi < vars.size(); ++vi) {
      std::FILE* ev = nullptr;
      if (!events_dir.empty()) {
        const std::string path = events_dir + "/" + tag + vars[vi].name + "_m" + std::to_string(mi) + ".jsonl";
        ev = std::fopen(path.c_str(), "w");
        if (!ev) {
          std::fprintf(stderr, "hft_lab: cannot write %s\n", path.c_str());
          return 2;
        }
      }
      const RunResult r = run_one(m, bc, vars[vi].cfg, rc, ev);
      if (ev) std::fclose(ev);
      if (!r.ok) {
        std::fprintf(stderr, "hft_lab: backtester setup failed\n");
        return 2;
      }
      const std::string bad = run_problem(r);
      if (!bad.empty()) {
        std::fprintf(stderr, "hft_lab: UNHEALTHY run (variant %s, market %lld): %s\n", vars[vi].name.c_str(), mi, bad.c_str());
        ++unhealthy;
      }
      Row& w = rows[vi];
      const double fee = fee_for(r.rep.volume, maker_fee_bps);
      w.fees.push_back(fee);
      w.pnl.push_back(r.rep.pnl - fee);
      w.edge.push_back(r.rep.edge);
      w.inventory.push_back(r.rep.inventory);
      w.fill_rate.push_back(r.rep.fill_rate_quotes);
      w.adverse1ms.push_back(r.rep.adverse[1]);
      w.sharpe.push_back(r.rep.sharpe_per_bin);
      w.fills.push_back(r.rep.fills);
      w.digest.push_back(r.rep.digest);
      w.risk_rejects.push_back(r.rep.risk_rejects);
      w.max_pos.push_back(r.rep.max_abs_pos);
      digest = mix64(digest, r.rep.digest);
    }
  }
  return 0;
}

// ---- grid search -----------------------------------------------------------------------------------------------

inline bool parse_double(std::string_view s, double& out) {
  if (s.empty()) return false;
  const std::string t(s);
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || !std::isfinite(v)) return false;
  out = v;
  return true;
}

constexpr std::size_t kMaxGridAxes = 8, kMaxAxisValues = 50, kMaxGridPoints = 500;

struct GridAxis {
  std::string key;
  std::vector<long long> vals;
};

// "key=v1,v2;key2=w1,w2" -> every combination, each a Variant over `base` named "key=v1_key2=w1". The first axis
// changes slowest. Refuses repeated keys, empty axes, bad values and grids over 500 points.
inline bool parse_grid(std::string_view spec, const strat::MmConfig& base, std::vector<Variant>& out, std::string& err,
                       std::vector<GridAxis>* axes_out = nullptr) {
  struct Axis {
    std::string key;
    std::vector<std::string> vals;
  };
  std::vector<Axis> axes;
  std::string_view rest = spec;
  while (true) {
    const std::size_t semi = rest.find(';');
    const std::string_view one = rest.substr(0, semi);
    const std::size_t eq = one.find('=');
    if (eq == std::string_view::npos) {
      err = "grid axis wants key=v1,v2,...: '" + std::string(one) + "'";
      return false;
    }
    Axis a;
    a.key = std::string(one.substr(0, eq));
    for (const Axis& b : axes)
      if (b.key == a.key) {
        err = "grid axis repeated: " + a.key;
        return false;
      }
    std::string_view vs = one.substr(eq + 1);
    while (true) {
      const std::size_t comma = vs.find(',');
      a.vals.emplace_back(vs.substr(0, comma));
      if (comma == std::string_view::npos) break;
      vs = vs.substr(comma + 1);
    }
    if (a.vals.size() > kMaxAxisValues) {
      err = "grid axis " + a.key + " has more than 50 values";
      return false;
    }
    axes.push_back(a);
    if (axes.size() > kMaxGridAxes) {
      err = "more than 8 grid axes";
      return false;
    }
    if (semi == std::string_view::npos) break;
    rest = rest.substr(semi + 1);
  }
  std::size_t total = 1;
  for (const Axis& a : axes) {
    total *= a.vals.size();
    if (total > kMaxGridPoints) {
      err = "grid has more than 500 points";
      return false;
    }
  }
  out.clear();
  if (axes_out) {
    axes_out->clear();
    for (const Axis& a : axes) {
      GridAxis g;
      g.key = a.key;
      for (const std::string& v : a.vals) {
        long long x = 0;
        parse_int(v, x);  // already accepted by apply_param below, which would fail first otherwise
        g.vals.push_back(x);
      }
      axes_out->push_back(g);
    }
  }
  std::vector<std::size_t> idx(axes.size(), 0);
  for (std::size_t n = 0; n < total; ++n) {
    Variant v;
    v.cfg = base;
    for (std::size_t k = 0; k < axes.size(); ++k) {
      if (!apply_param(v.cfg, axes[k].key, axes[k].vals[idx[k]], err)) return false;
      v.name += std::string(k ? "_" : "") + axes[k].key + "=" + axes[k].vals[idx[k]];
    }
    if (!valid_name(v.name)) {
      err = "grid point name too long or odd: '" + v.name + "' (fewer or shorter axes)";
      return false;
    }
    out.push_back(v);
    for (std::size_t k = axes.size(); k-- > 0;) {
      if (++idx[k] < axes[k].vals.size()) break;
      idx[k] = 0;
    }
  }
  return true;
}

// Objective for choosing parameters: mean PnL per market minus a penalty times the mean largest position. With the
// penalty at zero it is plain mean PnL.
inline double score(const Row& r, double risk_penalty) {
  std::vector<double> pos;
  for (const std::int64_t p : r.max_pos) pos.push_back(static_cast<double>(p));
  return summarize(r.pnl).mean - risk_penalty * summarize(pos).mean;
}

// Indices of rows[first..] best first by score; equal scores keep their order.
inline std::vector<std::size_t> rank(const std::vector<Row>& rows, std::size_t first, double risk_penalty) {
  std::vector<std::size_t> idx;
  for (std::size_t i = first; i < rows.size(); ++i) idx.push_back(i);
  std::vector<double> sc(rows.size(), 0.0);
  for (std::size_t i = first; i < rows.size(); ++i) sc[i] = score(rows[i], risk_penalty);
  std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return sc[a] > sc[b]; });
  return idx;
}

// The axes on which `c` sits at the smallest or largest value of the grid (only axes with at least two different
// values count): the true optimum may lie beyond the edge, so the grid should be widened there.
inline std::vector<std::string> edge_axes(const std::vector<GridAxis>& axes, const strat::MmConfig& c) {
  std::vector<std::string> out;
  for (const GridAxis& a : axes) {
    long long lo = a.vals.empty() ? 0 : a.vals[0], hi = lo, cur = 0;
    for (const long long v : a.vals) {
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    if (lo == hi || !get_param(c, a.key, cur)) continue;
    if (cur == lo) out.push_back(a.key + " (smallest)");
    if (cur == hi) out.push_back(a.key + " (largest)");
  }
  return out;
}

// How much of the gain over the base seen in training was still there on held-out markets. NaN when training showed no
// gain (there is nothing to retain).
inline double retained(double train_gain, double test_gain) {
  return train_gain > 0 ? test_gain / train_gain : std::nan("");
}

}  // namespace hft::bt::lab
