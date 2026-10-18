// hft_lab: compare market maker parameter variants over generated markets (ADR 33, 34).
//
//   hft_lab [--requests N] [--markets M] [--seed S] [--variant name:k=v,k=v]... [--sweep key=v1,v2,...]...
//           [--md-us U] [--ingress-us U] [--egress-us U] [--jitter-us U] [--queue-penalty-pct P]
//           [--maker-fee-bps X] [--json FILE] [--events DIR]
//   hft_lab --grid "key=v1,v2;key2=w1,w2" [--top K] [--risk-penalty X] [--test-markets N] [--test-seed T] [same options]
//
// Variant 0 is always "base" (the default strategy configuration). Every variant runs on every market, so the
// comparison is paired. --events DIR writes one hft_live-style event log per run for scripts/pnl_report.py.
// --grid chooses parameters on the training markets (seeds S..S+M-1): it keeps the best K by mean PnL minus X times
// the mean largest position, then runs only those and the base on held-out markets (seeds T..T+N-1, T defaults to
// S+100000) and reports how much of the gain over the base survived.
// --maker-fee-bps X charges X basis points of notional on every fill (negative is a rebate); every fill in a lab run is a
// maker fill because the strategy only sends post-only orders, so there is no taker rate. PnL is then after fees.
// Exit: 0 ok, 1 usage, 2 setup failed, 3 a run failed its health checks (results are not to be trusted).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hft/bt/lab.hpp"

using namespace hft;
using namespace hft::bt;
using lab::Row;

namespace {

int usage(const char* msg) {
  std::fprintf(stderr, "hft_lab: %s\n", msg);
  std::fprintf(stderr,
               "usage: hft_lab [--requests N] [--markets M] [--seed S] [--variant name:k=v,...]... [--sweep key=v1,v2,...]...\n"
               "               [--md-us U] [--ingress-us U] [--egress-us U] [--jitter-us U] [--queue-penalty-pct P]\n"
               "               [--maker-fee-bps X] [--regimes a,b,...] [--latency-sweep us1,us2,... [--latency-axis all|md|ingress|egress]]\n"
               "               [--queue-sweep pct1,pct2,...]\n"
               "               [--json FILE] [--events DIR]\n"
               "       hft_lab --grid \"key=v1,v2;key2=w1,w2\" [--top K] [--risk-penalty X] [--test-markets N] [--test-seed T] [...]\n");
  return 1;
}

bool num_arg(const char* s, long long lo, long long hi, long long& out) {
  if (!lab::parse_int(s, out)) return false;
  return out >= lo && out <= hi;
}

void json_vec(std::FILE* f, const char* key, const std::vector<double>& v) {
  std::fprintf(f, "      \"%s\": [", key);
  for (std::size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s%.6f", i ? ", " : "", v[i]);
  std::fprintf(f, "],\n");
}

void json_variants(std::FILE* f, const std::vector<lab::Variant>& vars, const std::vector<Row>& rows) {
  std::fprintf(f, "[\n");
  for (std::size_t vi = 0; vi < vars.size(); ++vi) {
    const Row& w = rows[vi];
    std::fprintf(f, "    {\n      \"name\": \"%s\",\n", vars[vi].name.c_str());
    json_vec(f, "pnl", w.pnl);
    json_vec(f, "edge", w.edge);
    json_vec(f, "inventory", w.inventory);
    json_vec(f, "fees", w.fees);
    std::fprintf(f, "      \"fills\": [");
    for (std::size_t k = 0; k < w.fills.size(); ++k) std::fprintf(f, "%s%llu", k ? ", " : "", static_cast<unsigned long long>(w.fills[k]));
    std::fprintf(f, "],\n      \"max_pos\": [");
    for (std::size_t k = 0; k < w.max_pos.size(); ++k) std::fprintf(f, "%s%lld", k ? ", " : "", static_cast<long long>(w.max_pos[k]));
    std::fprintf(f, "],\n      \"digest\": [");
    for (std::size_t k = 0; k < w.digest.size(); ++k) std::fprintf(f, "%s\"%016llx\"", k ? ", " : "", static_cast<unsigned long long>(w.digest[k]));
    std::fprintf(f, "]");
    if (vi > 0) {
      const lab::Paired d = lab::paired(w.pnl, rows[0].pnl);
      std::fprintf(f, ",\n      \"vs_base\": {\"mean\": %.6f, \"se\": %.6f, \"wins\": %zu, \"losses\": %zu, \"ties\": %zu, \"sign_p\": %.6f}", d.mean,
                   d.se, d.wins, d.losses, d.ties, d.p_sign);
    }
    std::fprintf(f, "\n    }%s\n", vi + 1 < vars.size() ? "," : "");
  }
  std::fprintf(f, "  ]");
}

void print_table(const std::vector<lab::Variant>& vars, const std::vector<Row>& rows, bool show_fees) {
  std::printf("%-26s %12s %9s %12s %11s %10s %7s %9s%s\n", "variant", "PnL mean", "+- se", "edge mean", "inventory", "fills/mkt", "fill%", "max|pos|", show_fees ? "   fees mean" : "");
  for (std::size_t vi = 0; vi < vars.size(); ++vi) {
    const Row& w = rows[vi];
    const lab::Summary p = lab::summarize(w.pnl), e = lab::summarize(w.edge), iv = lab::summarize(w.inventory), fr = lab::summarize(w.fill_rate);
    double fl = 0, mp = 0;
    for (std::size_t k = 0; k < w.fills.size(); ++k) {
      fl += static_cast<double>(w.fills[k]);
      mp = static_cast<double>(w.max_pos[k]) > mp ? static_cast<double>(w.max_pos[k]) : mp;
    }
    std::printf("%-26s %+12.2f %9.2f %+12.2f %+11.2f %10.0f %6.1f%% %9.0f", vars[vi].name.c_str(), p.mean, p.se, e.mean, iv.mean,
                fl / static_cast<double>(w.fills.size()), 100 * fr.mean, mp);
    if (show_fees) std::printf("  %+11.2f", lab::summarize(w.fees).mean);
    std::printf("\n");
  }
}

void print_paired(const std::vector<lab::Variant>& vars, const std::vector<Row>& rows) {
  std::printf("%-26s %12s %9s %7s %10s %9s %12s %12s\n", "variant", "dPnL mean", "+- se", "t", "W/L/T", "sign p", "dEdge mean", "dInv mean");
  for (std::size_t vi = 1; vi < vars.size(); ++vi) {
    const lab::Paired d = lab::paired(rows[vi].pnl, rows[0].pnl);
    const lab::Paired de = lab::paired(rows[vi].edge, rows[0].edge);
    const lab::Paired di = lab::paired(rows[vi].inventory, rows[0].inventory);
    char tb[32];
    if (!d.t_defined) std::snprintf(tb, sizeof tb, "n/a");
    else if (std::isinf(d.t)) std::snprintf(tb, sizeof tb, "%s", d.t > 0 ? "+inf" : "-inf");
    else std::snprintf(tb, sizeof tb, "%+.2f", d.t);
    char wl[40];
    std::snprintf(wl, sizeof wl, "%zu/%zu/%zu", d.wins, d.losses, d.ties);
    std::printf("%-26s %+12.2f %9.2f %7s %10s %9s %+12.2f %+12.2f\n", vars[vi].name.c_str(), d.mean, d.se, tb, wl, lab::format_p(d.p_sign).c_str(), de.mean,
                di.mean);
  }
}

struct SweepArgs {
  long long markets, seed, requests;
  const BtConfig& bc;
  const risk::RiskConfig& rc;
  double maker_fee_bps;
  const std::string& events_dir;
  const std::string& json_path;
  lab::LatencyAxis axis;
  const std::vector<long long>& lats;   // the swept values: microseconds, or percent when `queue`
  bool queue;                           // sweep the queue penalty instead of a latency
};

// The same variants and markets at each delay (or each queue penalty). Prints a table per value, the mean PnL by value with what is
// left at the largest value, and (with variants) the difference from the base by value.
int latency_sweep(const SweepArgs& a, const std::vector<lab::Variant>& vars) {
  std::vector<std::vector<Row>> per(a.lats.size());
  std::uint64_t all = lab::kDigestSeed;
  int unhealthy = 0;
  for (std::size_t li = 0; li < a.lats.size(); ++li) {
    const BtConfig bc = a.queue ? lab::with_queue_penalty(a.bc, a.lats[li]) : lab::with_latency(a.bc, a.axis, a.lats[li]);
    const std::string tag = a.queue ? "q" + std::to_string(a.lats[li]) + "pct_" : "lat" + std::to_string(a.lats[li]) + "us_";
    if (const int st = lab::evaluate(vars, a.seed, a.markets, a.requests, bc, a.rc, MarketConfig{}, a.maker_fee_bps, a.events_dir, tag, per[li], all, unhealthy)) return st;
  }
  if (a.queue) {
    std::printf("hft_lab: %lld markets (seeds %lld..%lld) x %lld requests, %zu variants; queue penalty sweep over %zu values; md %llu us, ingress %llu us, egress %llu us, jitter %llu us\n",
                a.markets, a.seed, a.seed + a.markets - 1, a.requests, vars.size(), a.lats.size(), static_cast<unsigned long long>(a.bc.md_ns / 1000),
                static_cast<unsigned long long>(a.bc.ingress_ns / 1000), static_cast<unsigned long long>(a.bc.egress_ns / 1000), static_cast<unsigned long long>(a.bc.jitter_ns / 1000));
    std::printf("queue penalty p: every order is treated as if an extra p percent of its level's quantity were ahead of it when it joined, and only trades work that off\n"
                "(0 is the exact queue the tape shows); --queue-penalty-pct is replaced by each value\n");
  } else {
    std::printf("hft_lab: %lld markets (seeds %lld..%lld) x %lld requests, %zu variants; latency sweep over %zu delays, axis %s, jitter %llu us\n", a.markets, a.seed,
                a.seed + a.markets - 1, a.requests, vars.size(), a.lats.size(), lab::axis_name(a.axis), static_cast<unsigned long long>(a.bc.jitter_ns / 1000));
    std::printf("axis %s: %s is set to each delay in turn; the others stay as given (md %llu us, ingress %llu us, egress %llu us)\n", lab::axis_name(a.axis),
                a.axis == lab::LatencyAxis::All ? "market data, order entry and report return" : (a.axis == lab::LatencyAxis::Md ? "market data" : (a.axis == lab::LatencyAxis::Ingress ? "order entry" : "report return")),
                static_cast<unsigned long long>(a.bc.md_ns / 1000), static_cast<unsigned long long>(a.bc.ingress_ns / 1000), static_cast<unsigned long long>(a.bc.egress_ns / 1000));
  }
  if (a.maker_fee_bps != 0) std::printf("fees: %g bps of notional on every fill; PnL is after fees\n", a.maker_fee_bps);
  for (std::size_t li = 0; li < a.lats.size(); ++li) {
    if (a.queue) std::printf("\nqueue penalty %lld %%\n", a.lats[li]);
    else std::printf("\nlatency %lld us\n", a.lats[li]);
    print_table(vars, per[li], a.maker_fee_bps != 0);
  }
  std::printf(a.queue ? "\nmean PnL by queue penalty (percent)\n%-26s" : "\nmean PnL by delay (microseconds)\n%-26s", "variant");
  for (const long long us : a.lats) std::printf(" %9lld", us);
  std::printf(" %10s\n", "kept");
  for (std::size_t vi = 0; vi < vars.size(); ++vi) {
    std::printf("%-26s", vars[vi].name.c_str());
    std::vector<double> m;
    for (std::size_t li = 0; li < a.lats.size(); ++li) {
      m.push_back(lab::summarize(per[li][vi].pnl).mean);
      std::printf(" %+9.2f", m.back());
    }
    const double kept = lab::retained(m.front(), m.back());
    if (std::isnan(kept)) std::printf(" %10s\n", "n/a");
    else std::printf(" %9.0f%%\n", 100 * kept);
  }
  std::printf(a.queue ? "kept = mean PnL at the largest penalty over mean PnL at the smallest (n/a when the smallest made no money;\nunstable when it made very little).\n"
                      : "kept = mean PnL at the largest delay over mean PnL at the smallest (n/a when the smallest delay made no money;\nunstable when it made very little).\n");
  if (vars.size() > 1) {
    std::printf(a.queue ? "\ndifference from the base by queue penalty (variant minus base)\n%-26s" : "\ndifference from the base by delay (variant minus base)\n%-26s", "variant");
    for (const long long us : a.lats) std::printf(" %9lld", us);
    std::printf(" %9s %9s\n", "better in", "worst");
    for (std::size_t vi = 1; vi < vars.size(); ++vi) {
      const lab::AcrossRegimes x = lab::across_regimes(per, vi);
      std::printf("%-26s", vars[vi].name.c_str());
      for (const double d : x.d) std::printf(" %+9.2f", d);
      std::printf(" %4zu/%-4zu %+9.2f\n", x.better, a.lats.size(), x.worst);
    }
  }
  if (a.queue)
    std::printf("\nRead this with care: the queue penalty is a stress on one assumption of the fill model (where we stand in the queue), not a\n"
                "measurement of a real venue's queue. A variant whose PnL falls fast as the penalty grows depends on fills that a deeper\n"
                "queue would take away. Every value uses the same seeds, so the columns are paired by market but not tested.\n");
  else
  std::printf("\nRead this with care: the delays are added to a simulated venue whose fills come from a queue model, and the market does\n"
              "not react to our orders. A variant that holds its PnL as delay grows is less sensitive to latency in this simulator;\n"
              "it is not shown to be fast enough for a real venue. Every delay uses the same seeds, so the columns are paired by\n"
              "market but not tested.\n");
  std::printf("\nrun digest %016llx (changes if any run's result changes)\n", static_cast<unsigned long long>(all));
  if (!a.json_path.empty()) {
    std::FILE* f = std::fopen(a.json_path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "hft_lab: cannot write %s\n", a.json_path.c_str());
      return 2;
    }
    if (a.queue)
      std::fprintf(f, "{\n  \"markets\": %lld, \"requests\": %lld, \"seed\": %lld, \"maker_fee_bps\": %g, \"digest\": \"%016llx\",\n  \"queue_penalties\": [", a.markets, a.requests,
                   a.seed, a.maker_fee_bps, static_cast<unsigned long long>(all));
    else
      std::fprintf(f, "{\n  \"markets\": %lld, \"requests\": %lld, \"seed\": %lld, \"maker_fee_bps\": %g, \"latency_axis\": \"%s\", \"digest\": \"%016llx\",\n  \"latencies\": [", a.markets,
                   a.requests, a.seed, a.maker_fee_bps, lab::axis_name(a.axis), static_cast<unsigned long long>(all));
    for (std::size_t li = 0; li < a.lats.size(); ++li) {
      std::fprintf(f, a.queue ? "%s\n    {\"pct\": %lld, \"variants\": " : "%s\n    {\"us\": %lld, \"variants\": ", li ? "," : "", a.lats[li]);
      json_variants(f, vars, per[li]);
      std::fprintf(f, "}");
    }
    std::fprintf(f, "\n  ]\n}\n");
    std::fclose(f);
  }
  if (unhealthy) {
    std::fprintf(stderr, "hft_lab: %d run(s) failed health checks; do not use these numbers\n", unhealthy);
    return 3;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  long long requests = 200'000, markets = 8, seed = 1000, md = 5, ing = 10, egr = 10, jit = 2, qpen = 0;
  long long test_markets = 0, test_seed = -1, top_k = 3;
  bool top_set = false, have_grid = false;
  std::vector<lab::GridAxis> axes;
  double risk_penalty = 0, maker_fee_bps = 0;
  std::string json_path, events_dir, grid_spec, regime_spec;
  std::vector<const lab::Regime*> regimes;
  std::vector<long long> lats, qpens;
  lab::LatencyAxis lat_axis = lab::LatencyAxis::All;
  bool axis_set = false;
  std::vector<lab::Variant> vars;
  lab::Variant base;
  base.name = "base";
  vars.push_back(base);

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        usage((std::string(what) + " needs a value").c_str());
        return nullptr;
      }
      return argv[++i];
    };
    auto numeric = [&](const char* flag, long long lo, long long hi, long long& dst) -> bool {
      const char* v = next(flag);
      if (!v) return false;
      if (!num_arg(v, lo, hi, dst)) {
        usage((std::string(flag) + " wants an integer between " + std::to_string(lo) + " and " + std::to_string(hi)).c_str());
        return false;
      }
      return true;
    };
    if (a == "--requests") { if (!numeric("--requests", 1000, 50'000'000, requests)) return 1; }
    else if (a == "--markets") { if (!numeric("--markets", 1, 1000, markets)) return 1; }
    else if (a == "--seed") { if (!numeric("--seed", 0, 1'000'000'000, seed)) return 1; }
    else if (a == "--md-us") { if (!numeric("--md-us", 0, 100000, md)) return 1; }
    else if (a == "--ingress-us") { if (!numeric("--ingress-us", 0, 100000, ing)) return 1; }
    else if (a == "--egress-us") { if (!numeric("--egress-us", 0, 100000, egr)) return 1; }
    else if (a == "--jitter-us") { if (!numeric("--jitter-us", 0, 100000, jit)) return 1; }
    else if (a == "--queue-penalty-pct") { if (!numeric("--queue-penalty-pct", 0, 1000, qpen)) return 1; }
    else if (a == "--test-markets") { if (!numeric("--test-markets", 1, 1000, test_markets)) return 1; }
    else if (a == "--test-seed") { if (!numeric("--test-seed", 0, 1'000'000'000, test_seed)) return 1; }
    else if (a == "--top") { if (!numeric("--top", 1, 20, top_k)) return 1; top_set = true; }
    else if (a == "--maker-fee-bps") {
      const char* v = next("--maker-fee-bps");
      if (!v) return 1;
      if (!lab::parse_double(v, maker_fee_bps) || maker_fee_bps < -100 || maker_fee_bps > 1000) return usage("--maker-fee-bps wants a number between -100 and 1000");
    }
    else if (a == "--regimes") {
      const char* v = next("--regimes");
      if (!v) return 1;
      std::string err;
      if (!lab::parse_regimes(v, regimes, err)) return usage((err + " (known: default, volatile, quiet, informed, uninformed, thin, slow)").c_str());
      regime_spec = v;
    }
    else if (a == "--latency-sweep") {
      const char* v = next("--latency-sweep");
      if (!v) return 1;
      std::string err;
      if (!lab::parse_latencies(v, lats, err)) return usage(err.c_str());
    }
    else if (a == "--queue-sweep") {
      const char* v = next("--queue-sweep");
      if (!v) return 1;
      std::string err;
      if (!lab::parse_queue_penalties(v, qpens, err)) return usage(err.c_str());
    }
    else if (a == "--latency-axis") {
      const char* v = next("--latency-axis");
      if (!v) return 1;
      if (!lab::parse_latency_axis(v, lat_axis)) return usage("--latency-axis wants all, md, ingress or egress");
      axis_set = true;
    }
    else if (a == "--json") { const char* v = next("--json"); if (!v) return 1; json_path = v; }
    else if (a == "--events") { const char* v = next("--events"); if (!v) return 1; events_dir = v; }
    else if (a == "--risk-penalty") {
      const char* v = next("--risk-penalty");
      if (!v) return 1;
      if (!lab::parse_double(v, risk_penalty) || risk_penalty < 0 || risk_penalty > 1e6) return usage("--risk-penalty wants a number between 0 and 1000000");
    } else if (a == "--grid") {
      const char* v = next("--grid");
      if (!v) return 1;
      grid_spec = v;
      have_grid = true;
    } else if (a == "--variant") {
      const char* v = next("--variant");
      if (!v) return 1;
      lab::Variant x;
      std::string err;
      if (!lab::parse_variant(v, strat::MmConfig{}, x, err)) return usage(err.c_str());
      vars.push_back(x);
    } else if (a == "--sweep") {
      const char* v = next("--sweep");
      if (!v) return 1;
      const std::string s = v;
      const std::size_t eq = s.find('=');
      if (eq == std::string::npos) return usage("--sweep wants key=v1,v2,...");
      const std::string key = s.substr(0, eq);
      std::size_t pos = eq + 1;
      while (pos <= s.size()) {
        const std::size_t comma = s.find(',', pos);
        const std::string val = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        lab::Variant x;
        std::string err;
        if (!lab::parse_variant(key + "=" + val + ":" + key + "=" + val, strat::MmConfig{}, x, err)) return usage(err.c_str());
        vars.push_back(x);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (a == "--help" || a == "-h") {
      return usage("help");
    } else {
      return usage(("unknown option " + a).c_str());
    }
  }
  for (std::size_t i = 0; i < vars.size(); ++i)
    for (std::size_t j = i + 1; j < vars.size(); ++j)
      if (vars[i].name == vars[j].name) return usage(("duplicate variant name '" + vars[i].name + "'").c_str());

  if (!lats.empty() && !qpens.empty()) return usage("--latency-sweep and --queue-sweep cannot be combined");
  if ((!lats.empty() || !qpens.empty()) && (have_grid || !regimes.empty())) return usage("--latency-sweep and --queue-sweep cannot be combined with --grid or --regimes");
  if (axis_set && lats.empty()) return usage("--latency-axis needs --latency-sweep");
  if (have_grid && !regimes.empty()) return usage("--regimes cannot be combined with --grid (a search over regimes would spend the held-out markets)");
  if (have_grid) {
    if (vars.size() > 1) return usage("--grid cannot be combined with --variant or --sweep");
    std::vector<lab::Variant> g;
    std::string err;
    if (!lab::parse_grid(grid_spec, strat::MmConfig{}, g, err, &axes)) return usage(err.c_str());
    for (const lab::Variant& v : g) vars.push_back(v);
    if (test_seed < 0) test_seed = seed + 100000;
    if (test_markets == 0) test_markets = markets;
    if (seed < test_seed + test_markets && test_seed < seed + markets) return usage("training and held-out seeds overlap (--seed, --markets, --test-seed, --test-markets)");
    if (static_cast<std::size_t>(top_k) > vars.size() - 1) top_k = static_cast<long long>(vars.size() - 1);
  } else if (test_markets != 0 || test_seed >= 0 || top_set || risk_penalty != 0) {
    return usage("--top, --risk-penalty, --test-markets and --test-seed need --grid");
  }

  std::int64_t max_pos = 0;
  for (const lab::Variant& v : vars) max_pos = v.cfg.max_pos > max_pos ? v.cfg.max_pos : max_pos;
  const risk::RiskConfig rc = lab::lab_risk(max_pos);

  BtConfig bc;
  bc.md_ns = static_cast<std::uint64_t>(md) * 1000;
  bc.ingress_ns = static_cast<std::uint64_t>(ing) * 1000;
  bc.egress_ns = static_cast<std::uint64_t>(egr) * 1000;
  bc.jitter_ns = static_cast<std::uint64_t>(jit) * 1000;
  bc.queue_penalty_pct = static_cast<std::uint32_t>(qpen);
  bc.seed = 1;

  if (!lats.empty()) return latency_sweep(SweepArgs{markets, seed, requests, bc, rc, maker_fee_bps, events_dir, json_path, lat_axis, lats, false}, vars);
  if (!qpens.empty()) return latency_sweep(SweepArgs{markets, seed, requests, bc, rc, maker_fee_bps, events_dir, json_path, lat_axis, qpens, true}, vars);

  const bool have_regimes = !regimes.empty();
  if (!have_regimes) regimes.push_back(lab::find_regime("default"));
  std::vector<std::vector<Row>> per_regime(regimes.size());
  std::uint64_t all = lab::kDigestSeed;
  int unhealthy = 0;
  for (std::size_t ri = 0; ri < regimes.size(); ++ri) {
    // with several regimes the event files carry the regime name, so they cannot overwrite each other
    const std::string tag = have_regimes ? std::string(regimes[ri]->name) + "_" : std::string();
    if (const int st = lab::evaluate(vars, seed, markets, requests, bc, rc, lab::regime_config(*regimes[ri]), maker_fee_bps, events_dir, tag, per_regime[ri], all, unhealthy)) return st;
  }
  std::vector<Row>& rows = per_regime[0];

  std::printf("hft_lab: %lld markets (seeds %lld..%lld) x %lld requests, %zu variants; md %lld us, ingress %lld us, egress %lld us, jitter %lld us, queue penalty %lld%%\n",
              markets, seed, seed + markets - 1, requests, vars.size(), md, ing, egr, jit, qpen);
  std::printf("risk limits derived from the variants: max order qty %lld, max position %lld (the strategy cap should bind first)\n",
              static_cast<long long>(rc.max_order_qty), static_cast<long long>(rc.max_position));
  if (have_regimes) std::printf("regimes: %s (same seeds in each; a regime changes generator settings, not the generator)\n", regime_spec.c_str());
  if (maker_fee_bps != 0) std::printf("fees: %g bps of notional on every fill (all fills are maker fills here); PnL is after fees, edge is not\n", maker_fee_bps);
  std::printf("\n");

  std::vector<lab::Variant> tvars;
  std::vector<Row> trows;
  std::vector<std::size_t> picked;
  if (!have_grid) {
    for (std::size_t ri = 0; ri < regimes.size(); ++ri) {
      if (have_regimes) std::printf("%sregime %s: %s\n", ri ? "\n" : "", regimes[ri]->name, regimes[ri]->what);
      print_table(vars, per_regime[ri], maker_fee_bps != 0);
      if (vars.size() > 1 && have_regimes) {
        std::printf("\npaired against base, same markets (variant minus base)\n");
        print_paired(vars, per_regime[ri]);
      }
    }
    if (vars.size() > 1 && have_regimes && regimes.size() > 1) {
      std::printf("\nacross regimes: mean PnL difference from the base in each regime\n%-26s", "variant");
      for (const lab::Regime* r : regimes) std::printf(" %11s", r->name);
      std::printf(" %9s %11s\n", "better in", "worst");
      for (std::size_t vi = 1; vi < vars.size(); ++vi) {
        const lab::AcrossRegimes a = lab::across_regimes(per_regime, vi);
        std::printf("%-26s", vars[vi].name.c_str());
        for (const double x : a.d) std::printf(" %+11.2f", x);
        std::printf(" %5zu/%-3zu %+11.2f\n", a.better, regimes.size(), a.worst);
      }
      std::printf("A variant that wins in the regime it was tuned for and loses elsewhere is fitted to that regime. 'better in' counts\n"
                  "regimes where the mean difference is above zero; it is not a test, and every regime uses the same seeds.\n");
    }
    if (vars.size() > 1 && !have_regimes) {
      std::printf("\npaired against base, same markets (variant minus base)\n");
      print_paired(vars, rows);
    }
    if (vars.size() > 1) {
      std::printf("\nRead this with care: the fills are simulated (queue model, no market impact), the markets come from one synthetic\n"
                  "generator%s, and %zu comparisons were made without adjusting p for that. A small p here says a variant beat the base on\n"
                  "these generated markets, not that it would make money anywhere. PnL = edge + inventory: a gain that sits in the inventory\n"
                  "column is the price drifting under a position, which is luck as much as skill. With n markets the smallest possible sign p\n"
                  "is 2/2^n (%.3g here).\n",
                  have_regimes ? " (regimes change its settings, not its kind)" : "", vars.size() - 1, lab::sign_test_p(static_cast<std::size_t>(markets), 0));
    }
  } else {
    // Training: score every grid point, keep the best K.
    const std::vector<std::size_t> order = lab::rank(rows, 1, risk_penalty);
    for (long long k = 0; k < top_k; ++k) picked.push_back(order[static_cast<std::size_t>(k)]);
    std::printf("search: %zu grid points plus the base, trained on %lld markets; objective = mean PnL - %g x mean largest position\n",
                vars.size() - 1, markets, risk_penalty);
    std::printf("training results, best %lld of %zu by the objective (the base is row 0):\n", top_k, vars.size() - 1);
    std::vector<lab::Variant> shown;
    std::vector<Row> shown_rows;
    shown.push_back(vars[0]);
    shown_rows.push_back(rows[0]);
    for (const std::size_t ix : picked) {
      shown.push_back(vars[ix]);
      shown_rows.push_back(rows[ix]);
    }
    print_table(shown, shown_rows, maker_fee_bps != 0);
    // Held-out: only the chosen ones and the base.
    tvars = shown;
    if (const int st = lab::evaluate(tvars, test_seed, test_markets, requests, bc, rc, MarketConfig{}, maker_fee_bps, events_dir, "test_", trows, all, unhealthy)) return st;
    std::printf("\nheld-out markets (seeds %lld..%lld), chosen variants and base:\n", test_seed, test_seed + test_markets - 1);
    print_table(tvars, trows, maker_fee_bps != 0);
    std::printf("\npaired against base on the held-out markets (variant minus base)\n");
    print_paired(tvars, trows);
    const double base_tr = lab::score(rows[0], risk_penalty), base_te = lab::score(trows[0], risk_penalty);
    std::printf("\n%-26s %12s %12s %12s %12s\n", "chosen variant", "train gain", "test gain", "retained", "(objective gain over base)");
    double sum_tr = 0, sum_te = 0;
    for (std::size_t k = 1; k < tvars.size(); ++k) {
      const double g_tr = lab::score(rows[picked[k - 1]], risk_penalty) - base_tr, g_te = lab::score(trows[k], risk_penalty) - base_te;
      sum_tr += g_tr;
      sum_te += g_te;
      const double ret = lab::retained(g_tr, g_te);
      char rb[32];
      if (std::isnan(ret)) std::snprintf(rb, sizeof rb, "n/a");
      else std::snprintf(rb, sizeof rb, "%.0f%%", 100 * ret);
      std::printf("%-26s %+12.2f %+12.2f %12s\n", tvars[k].name.c_str(), g_tr, g_te, rb);
    }
    const double n = static_cast<double>(tvars.size() - 1);
    std::printf("average of the chosen: train gain %+.2f, held-out gain %+.2f, shrinkage (winner's curse) %+.2f\n", sum_tr / n, sum_te / n, (sum_tr - sum_te) / n);
    for (std::size_t k = 1; k < tvars.size(); ++k) {
      const std::vector<std::string> edge = lab::edge_axes(axes, tvars[k].cfg);
      if (edge.empty()) continue;
      std::printf("grid edge: %s sits at", tvars[k].name.c_str());
      for (std::size_t e = 0; e < edge.size(); ++e) std::printf("%s %s", e ? "," : "", edge[e].c_str());
      std::printf("; the best value may lie beyond the grid, so widen it there\n");
    }
    std::printf("\nRead this with care: the held-out markets were not used to choose, but this is one split from one generator, and a\n"
                "gain here is still a gain on simulated fills. If the gain shrinks a lot or turns negative, the training result was\n"
                "mostly selection. Do not rerun with the held-out seeds as training after looking: that spends them.\n");
  }
  std::printf("\nrun digest %016llx (changes if any run's result changes)\n", static_cast<unsigned long long>(all));

  if (!json_path.empty()) {
    std::FILE* f = std::fopen(json_path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "hft_lab: cannot write %s\n", json_path.c_str());
      return 2;
    }
    std::fprintf(f, "{\n  \"markets\": %lld, \"requests\": %lld, \"seed\": %lld, \"maker_fee_bps\": %g, \"digest\": \"%016llx\",\n  \"variants\": ", markets, requests, seed, maker_fee_bps,
                 static_cast<unsigned long long>(all));
    json_variants(f, vars, rows);
    if (have_regimes) {
      std::fprintf(f, ",\n  \"regimes\": [");
      for (std::size_t ri = 0; ri < regimes.size(); ++ri) {
        std::fprintf(f, "%s\n    {\"name\": \"%s\", \"variants\": ", ri ? "," : "", regimes[ri]->name);
        json_variants(f, vars, per_regime[ri]);
        std::fprintf(f, "}");
      }
      std::fprintf(f, "\n  ]");
    }
    if (have_grid) {
      std::fprintf(f, ",\n  \"test_seed\": %lld, \"test_markets\": %lld, \"risk_penalty\": %g,\n  \"selected\": [", test_seed, test_markets, risk_penalty);
      for (std::size_t k = 0; k < picked.size(); ++k) std::fprintf(f, "%s\"%s\"", k ? ", " : "", vars[picked[k]].name.c_str());
      std::fprintf(f, "],\n  \"test_variants\": ");
      json_variants(f, tvars, trows);
    }
    std::fprintf(f, "\n}\n");
    std::fclose(f);
  }
  if (unhealthy) {
    std::fprintf(stderr, "hft_lab: %d run(s) failed health checks; do not use these numbers\n", unhealthy);
    return 3;
  }
  return 0;
}
