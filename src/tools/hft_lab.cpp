// hft_lab: compare market maker parameter variants over generated markets (ADR 33).
//
//   hft_lab [--requests N] [--markets M] [--seed S] [--variant name:k=v,k=v]... [--sweep key=v1,v2,...]...
//           [--md-us U] [--ingress-us U] [--egress-us U] [--jitter-us U] [--queue-penalty-pct P]
//           [--json FILE] [--events DIR]
//
// Variant 0 is always "base" (the default strategy configuration). Every variant runs on every market, so the
// comparison is paired. --events DIR writes one hft_live-style event log per run for scripts/pnl_report.py.
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

namespace {

struct Row {
  std::vector<double> pnl, edge, inventory, fill_rate, adverse1ms, sharpe;
  std::vector<std::uint64_t> fills, digest, risk_rejects;
  std::vector<std::int64_t> max_pos;
};

int usage(const char* msg) {
  std::fprintf(stderr, "hft_lab: %s\n", msg);
  std::fprintf(stderr,
               "usage: hft_lab [--requests N] [--markets M] [--seed S] [--variant name:k=v,...]... [--sweep key=v1,v2,...]...\n"
               "               [--md-us U] [--ingress-us U] [--egress-us U] [--jitter-us U] [--queue-penalty-pct P]\n"
               "               [--json FILE] [--events DIR]\n");
  return 1;
}

bool num_arg(const char* s, long long lo, long long hi, long long& out) {
  if (!lab::parse_int(s, out)) return false;
  return out >= lo && out <= hi;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return h * 0x100000001B3ULL;
}

void json_vec(std::FILE* f, const char* key, const std::vector<double>& v) {
  std::fprintf(f, "      \"%s\": [", key);
  for (std::size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s%.6f", i ? ", " : "", v[i]);
  std::fprintf(f, "],\n");
}

}  // namespace

int main(int argc, char** argv) {
  long long requests = 200'000, markets = 8, seed = 1000, md = 5, ing = 10, egr = 10, jit = 2, qpen = 0;
  std::string json_path, events_dir;
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
    else if (a == "--json") { const char* v = next("--json"); if (!v) return 1; json_path = v; }
    else if (a == "--events") { const char* v = next("--events"); if (!v) return 1; events_dir = v; }
    else if (a == "--variant") {
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

  std::vector<Row> rows(vars.size());
  std::uint64_t all = 1469598103934665603ULL;
  int unhealthy = 0;
  for (long long mi = 0; mi < markets; ++mi) {
    bt::MarketConfig mc;
    mc.requests = static_cast<std::uint64_t>(requests);
    mc.seed = static_cast<std::uint64_t>(seed + mi);
    const bt::Market m = bt::MarketGen::generate(mc);
    for (std::size_t vi = 0; vi < vars.size(); ++vi) {
      std::FILE* ev = nullptr;
      if (!events_dir.empty()) {
        const std::string path = events_dir + "/" + vars[vi].name + "_m" + std::to_string(mi) + ".jsonl";
        ev = std::fopen(path.c_str(), "w");
        if (!ev) {
          std::fprintf(stderr, "hft_lab: cannot write %s\n", path.c_str());
          return 2;
        }
      }
      const lab::RunResult r = lab::run_one(m, bc, vars[vi].cfg, rc, ev);
      if (ev) std::fclose(ev);
      if (!r.ok) {
        std::fprintf(stderr, "hft_lab: backtester setup failed\n");
        return 2;
      }
      const std::string bad = lab::run_problem(r);
      if (!bad.empty()) {
        std::fprintf(stderr, "hft_lab: UNHEALTHY run (variant %s, market %lld): %s\n", vars[vi].name.c_str(), mi, bad.c_str());
        ++unhealthy;
      }
      Row& w = rows[vi];
      w.pnl.push_back(r.rep.pnl);
      w.edge.push_back(r.rep.edge);
      w.inventory.push_back(r.rep.inventory);
      w.fill_rate.push_back(r.rep.fill_rate_quotes);
      w.adverse1ms.push_back(r.rep.adverse[1]);
      w.sharpe.push_back(r.rep.sharpe_per_bin);
      w.fills.push_back(r.rep.fills);
      w.digest.push_back(r.rep.digest);
      w.risk_rejects.push_back(r.rep.risk_rejects);
      w.max_pos.push_back(r.rep.max_abs_pos);
      all = mix(all, r.rep.digest);
    }
  }

  std::printf("hft_lab: %lld markets (seeds %lld..%lld) x %lld requests, %zu variants; md %lld us, ingress %lld us, egress %lld us, jitter %lld us, queue penalty %lld%%\n",
              markets, seed, seed + markets - 1, requests, vars.size(), md, ing, egr, jit, qpen);
  std::printf("risk limits derived from the variants: max order qty %lld, max position %lld (the strategy cap should bind first)\n\n",
              static_cast<long long>(rc.max_order_qty), static_cast<long long>(rc.max_position));
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
  if (vars.size() > 1) {
    std::printf("\npaired against base, same markets (variant minus base)\n");
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
      std::printf("%-26s %+12.2f %9.2f %7s %10s %9s %+12.2f %+12.2f\n", vars[vi].name.c_str(), d.mean, d.se, tb, wl, lab::format_p(d.p_sign).c_str(), de.mean, di.mean);
    }
    std::printf("\nRead this with care: the fills are simulated (queue model, no market impact), the markets come from one synthetic\n"
                "generator, and %zu comparisons were made without adjusting p for that. A small p here says a variant beat the base on\n"
                "these generated markets, not that it would make money anywhere. PnL = edge + inventory: a gain that sits in the inventory\n"
                "column is the price drifting under a position, which is luck as much as skill. With n markets the smallest possible sign p\n"
                "is 2/2^n (%.3g here).\n", vars.size() - 1, lab::sign_test_p(static_cast<std::size_t>(markets), 0));
  }
  std::printf("\nrun digest %016llx (changes if any run's result changes)\n", static_cast<unsigned long long>(all));

  if (!json_path.empty()) {
    std::FILE* f = std::fopen(json_path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "hft_lab: cannot write %s\n", json_path.c_str());
      return 2;
    }
    std::fprintf(f, "{\n  \"markets\": %lld, \"requests\": %lld, \"seed\": %lld, \"digest\": \"%016llx\",\n  \"variants\": [\n", markets, requests,
                 seed, static_cast<unsigned long long>(all));
    for (std::size_t vi = 0; vi < vars.size(); ++vi) {
      const Row& w = rows[vi];
      std::fprintf(f, "    {\n      \"name\": \"%s\",\n", vars[vi].name.c_str());
      json_vec(f, "pnl", w.pnl);
      json_vec(f, "edge", w.edge);
      json_vec(f, "inventory", w.inventory);
      std::fprintf(f, "      \"fills\": [");
      for (std::size_t k = 0; k < w.fills.size(); ++k) std::fprintf(f, "%s%llu", k ? ", " : "", static_cast<unsigned long long>(w.fills[k]));
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
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
  }
  if (unhealthy) {
    std::fprintf(stderr, "hft_lab: %d run(s) failed health checks; do not use these numbers\n", unhealthy);
    return 3;
  }
  return 0;
}
