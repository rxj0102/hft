// hft_lab: compare market maker parameter variants over generated markets (ADR 33, 34).
//
//   hft_lab [--requests N] [--markets M] [--seed S] [--variant name:k=v,k=v]... [--sweep key=v1,v2,...]...
//           [--md-us U] [--ingress-us U] [--egress-us U] [--jitter-us U] [--queue-penalty-pct P]
//           [--json FILE] [--events DIR]
//   hft_lab --grid "key=v1,v2;key2=w1,w2" [--top K] [--risk-penalty X] [--test-markets N] [--test-seed T] [same options]
//
// Variant 0 is always "base" (the default strategy configuration). Every variant runs on every market, so the
// comparison is paired. --events DIR writes one hft_live-style event log per run for scripts/pnl_report.py.
// --grid chooses parameters on the training markets (seeds S..S+M-1): it keeps the best K by mean PnL minus X times
// the mean largest position, then runs only those and the base on held-out markets (seeds T..T+N-1, T defaults to
// S+100000) and reports how much of the gain over the base survived.
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

void print_table(const std::vector<lab::Variant>& vars, const std::vector<Row>& rows) {
  std::printf("%-26s %12s %9s %12s %11s %10s %7s %9s\n", "variant", "PnL mean", "+- se", "edge mean", "inventory", "fills/mkt", "fill%", "max|pos|");
  for (std::size_t vi = 0; vi < vars.size(); ++vi) {
    const Row& w = rows[vi];
    const lab::Summary p = lab::summarize(w.pnl), e = lab::summarize(w.edge), iv = lab::summarize(w.inventory), fr = lab::summarize(w.fill_rate);
    double fl = 0, mp = 0;
    for (std::size_t k = 0; k < w.fills.size(); ++k) {
      fl += static_cast<double>(w.fills[k]);
      mp = static_cast<double>(w.max_pos[k]) > mp ? static_cast<double>(w.max_pos[k]) : mp;
    }
    std::printf("%-26s %+12.2f %9.2f %+12.2f %+11.2f %10.0f %6.1f%% %9.0f\n", vars[vi].name.c_str(), p.mean, p.se, e.mean, iv.mean,
                fl / static_cast<double>(w.fills.size()), 100 * fr.mean, mp);
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

}  // namespace

int main(int argc, char** argv) {
  long long requests = 200'000, markets = 8, seed = 1000, md = 5, ing = 10, egr = 10, jit = 2, qpen = 0;
  long long test_markets = 0, test_seed = -1, top_k = 3;
  bool top_set = false, have_grid = false;
  std::vector<lab::GridAxis> axes;
  double risk_penalty = 0;
  std::string json_path, events_dir, grid_spec;
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

  std::vector<Row> rows;
  std::uint64_t all = lab::kDigestSeed;
  int unhealthy = 0;
  if (const int st = lab::evaluate(vars, seed, markets, requests, bc, rc, events_dir, "", rows, all, unhealthy)) return st;

  std::printf("hft_lab: %lld markets (seeds %lld..%lld) x %lld requests, %zu variants; md %lld us, ingress %lld us, egress %lld us, jitter %lld us, queue penalty %lld%%\n",
              markets, seed, seed + markets - 1, requests, vars.size(), md, ing, egr, jit, qpen);
  std::printf("risk limits derived from the variants: max order qty %lld, max position %lld (the strategy cap should bind first)\n\n",
              static_cast<long long>(rc.max_order_qty), static_cast<long long>(rc.max_position));

  std::vector<lab::Variant> tvars;
  std::vector<Row> trows;
  std::vector<std::size_t> picked;
  if (!have_grid) {
    print_table(vars, rows);
    if (vars.size() > 1) {
      std::printf("\npaired against base, same markets (variant minus base)\n");
      print_paired(vars, rows);
      std::printf("\nRead this with care: the fills are simulated (queue model, no market impact), the markets come from one synthetic\n"
                  "generator, and %zu comparisons were made without adjusting p for that. A small p here says a variant beat the base on\n"
                  "these generated markets, not that it would make money anywhere. PnL = edge + inventory: a gain that sits in the inventory\n"
                  "column is the price drifting under a position, which is luck as much as skill. With n markets the smallest possible sign p\n"
                  "is 2/2^n (%.3g here).\n",
                  vars.size() - 1, lab::sign_test_p(static_cast<std::size_t>(markets), 0));
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
    print_table(shown, shown_rows);
    // Held-out: only the chosen ones and the base.
    tvars = shown;
    if (const int st = lab::evaluate(tvars, test_seed, test_markets, requests, bc, rc, events_dir, "test_", trows, all, unhealthy)) return st;
    std::printf("\nheld-out markets (seeds %lld..%lld), chosen variants and base:\n", test_seed, test_seed + test_markets - 1);
    print_table(tvars, trows);
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
    std::fprintf(f, "{\n  \"markets\": %lld, \"requests\": %lld, \"seed\": %lld, \"digest\": \"%016llx\",\n  \"variants\": ", markets, requests, seed,
                 static_cast<unsigned long long>(all));
    json_variants(f, vars, rows);
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
