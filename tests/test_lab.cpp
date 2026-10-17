// Phase 23 tests: variant parsing, the statistics against hand-computed answers, the event writer, and the lab runs
// (determinism, hooks do not change results, the event log agrees with the report).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "hft/bt/lab.hpp"

using namespace hft;
using namespace hft::bt;

namespace {

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

void test_parse_int() {
  long long v = 0;
  CHECK(lab::parse_int("0", v) && v == 0);
  CHECK(lab::parse_int("-17", v) && v == -17);
  CHECK(lab::parse_int("123456789012", v) && v == 123456789012LL);
  CHECK(!lab::parse_int("", v));
  CHECK(!lab::parse_int("12x", v));
  CHECK(!lab::parse_int("1.5", v));
  CHECK(!lab::parse_int(" 3", v));
  CHECK(!lab::parse_int("+3", v));
  CHECK(!lab::parse_int("-", v));
  CHECK(!lab::parse_int("99999999999999999999", v));  // overflow
  CHECK(!lab::parse_int("9223372036854775808", v));   // one past the largest 64-bit value
  CHECK(lab::parse_int("9223372036854775807", v) && v == 9223372036854775807LL);
  CHECK(!lab::parse_int("-9223372036854775809", v));
}

void test_apply_param() {
  strat::MmConfig c;
  std::string err;
  CHECK(lab::apply_param(c, "half_spread16", "33", err) && c.half_spread16 == 33);
  CHECK(lab::apply_param(c, "alpha16", "-5", err) && c.alpha16 == -5);
  CHECK(lab::apply_param(c, "skew16", "0", err) && c.skew16 == 0);
  CHECK(lab::apply_param(c, "depth_levels", "7", err) && c.depth_levels == 7);
  CHECK(lab::apply_param(c, "qty", "9", err) && c.qty == 9);
  CHECK(lab::apply_param(c, "max_pos", "55", err) && c.max_pos == 55);
  CHECK(lab::apply_param(c, "requote_ticks", "3", err) && c.requote_ticks == 3);
  CHECK(lab::apply_param(c, "min_leaves", "4", err) && c.min_leaves == 4);
  CHECK(lab::apply_param(c, "min_interval_ns", "123000", err) && c.min_interval_ns == 123000);
  // bounds are inclusive and checked on both ends, and a failed call leaves the field alone
  strat::MmConfig d;
  CHECK(!lab::apply_param(d, "qty", "0", err) && d.qty == strat::MmConfig{}.qty);
  CHECK(lab::apply_param(d, "qty", "1", err) && d.qty == 1);
  CHECK(lab::apply_param(d, "qty", "100", err) && d.qty == 100);
  CHECK(!lab::apply_param(d, "qty", "101", err));
  CHECK(!lab::apply_param(d, "depth_levels", "0", err));
  CHECK(lab::apply_param(d, "depth_levels", "64", err));
  CHECK(!lab::apply_param(d, "depth_levels", "65", err));
  CHECK(!lab::apply_param(d, "half_spread16", "-1", err));
  CHECK(lab::apply_param(d, "half_spread16", "0", err));
  CHECK(lab::apply_param(d, "half_spread16", "1048576", err) && !lab::apply_param(d, "half_spread16", "1048577", err));
  CHECK(lab::apply_param(d, "skew16", "1048576", err) && !lab::apply_param(d, "skew16", "1048577", err));
  CHECK(lab::apply_param(d, "requote_ticks", "1000", err) && !lab::apply_param(d, "requote_ticks", "1001", err));
  CHECK(lab::apply_param(d, "requote_ticks", "0", err) && d.requote_ticks == 0);
  CHECK(lab::apply_param(d, "min_leaves", "0", err) && d.min_leaves == 0);
  CHECK(lab::apply_param(d, "min_interval_ns", "0", err) && d.min_interval_ns == 0);
  CHECK(!lab::apply_param(d, "skew16", "-1", err));
  CHECK(!lab::apply_param(d, "max_pos", "0", err));
  CHECK(!lab::apply_param(d, "max_pos", "10001", err));
  CHECK(lab::apply_param(d, "max_pos", "10000", err));
  CHECK(!lab::apply_param(d, "requote_ticks", "-1", err));
  CHECK(!lab::apply_param(d, "min_leaves", "101", err));
  CHECK(!lab::apply_param(d, "min_interval_ns", "1000000001", err));
  CHECK(lab::apply_param(d, "alpha16", "1048576", err) && !lab::apply_param(d, "alpha16", "1048577", err));
  CHECK(lab::apply_param(d, "alpha16", "-1048576", err) && !lab::apply_param(d, "alpha16", "-1048577", err));
  err.clear();
  CHECK(!lab::apply_param(d, "alhpa16", "1", err) && err.find("unknown parameter") != std::string::npos);
  err.clear();
  CHECK(!lab::apply_param(d, "alpha16", "x", err) && err.find("not an integer") != std::string::npos);
}

void test_parse_variant() {
  const strat::MmConfig base;
  lab::Variant v;
  std::string err;
  CHECK(lab::parse_variant("plain", base, v, err) && v.name == "plain" && v.cfg.alpha16 == base.alpha16);
  CHECK(lab::parse_variant("w:half_spread16=40,skew16=3", base, v, err));
  CHECK(v.name == "w" && v.cfg.half_spread16 == 40 && v.cfg.skew16 == 3 && v.cfg.alpha16 == base.alpha16);
  CHECK(lab::parse_variant("k=1.x-y_z:qty=2", base, v, err) && v.name == "k=1.x-y_z");
  CHECK(lab::parse_variant("azAZ09:qty=2", base, v, err) && v.name == "azAZ09");   // both ends of every character range
  for (const char* n : {"a`", "a{", "a@", "a[", "a/", "a:", "a~"}) CHECK(!lab::parse_variant(std::string(n) + ":qty=1", base, v, err));  // just outside each range
  CHECK(!lab::parse_variant("", base, v, err));
  CHECK(!lab::parse_variant(":qty=1", base, v, err));
  CHECK(!lab::parse_variant("a b:qty=1", base, v, err));
  CHECK(!lab::parse_variant("a/b:qty=1", base, v, err));
  CHECK(!lab::parse_variant(std::string(49, 'a'), base, v, err));
  CHECK(lab::parse_variant(std::string(48, 'a'), base, v, err));
  CHECK(!lab::parse_variant("a:", base, v, err));
  CHECK(!lab::parse_variant("a:qty", base, v, err));
  CHECK(!lab::parse_variant("a:=3", base, v, err));
  CHECK(!lab::parse_variant("a:qty=2,", base, v, err));
  CHECK(!lab::parse_variant("a:qty=2,,skew16=1", base, v, err));
  CHECK(!lab::parse_variant("a:qty=2,nope=1", base, v, err));
  CHECK(!lab::parse_variant("a:qty=200", base, v, err));
  // a later key wins over an earlier one, and the base config is not touched
  CHECK(lab::parse_variant("a:qty=2,qty=4", base, v, err) && v.cfg.qty == 4 && base.qty == strat::MmConfig{}.qty);
}

void test_stats() {
  const lab::Summary e = lab::summarize({});
  CHECK(e.n == 0 && e.mean == 0 && e.se == 0);
  const lab::Summary one = lab::summarize({5.0});
  CHECK(one.n == 1 && one.mean == 5.0 && one.sd == 0 && one.se == 0);
  const lab::Summary s = lab::summarize({1, 2, 3, 4});
  CHECK(s.n == 4 && near(s.mean, 2.5) && near(s.sd, std::sqrt(5.0 / 3.0)) && near(s.se, std::sqrt(5.0 / 3.0) / 2.0));

  CHECK(near(lab::sign_test_p(0, 0), 1.0));
  CHECK(near(lab::sign_test_p(0, 6), 2.0 / 64));
  CHECK(near(lab::sign_test_p(6, 0), 2.0 / 64));
  CHECK(near(lab::sign_test_p(3, 3), 1.0));
  CHECK(near(lab::sign_test_p(1, 5), 2.0 * (1 + 6) / 64));
  CHECK(near(lab::sign_test_p(2, 8), 2.0 * (1 + 10 + 45) / 1024));
  CHECK(near(lab::sign_test_p(8, 2), lab::sign_test_p(2, 8)));
  CHECK(near(lab::sign_test_p(4, 4), 1.0));    // the doubled tail would be 1.27: capped
  CHECK(near(lab::sign_test_p(0, 1), 1.0));

  // paired: differences +1, +3, -1, 0, +2 -> mean 1, wins 3, losses 1, ties 1
  const lab::Paired p = lab::paired({11, 13, 9, 10, 12}, {10, 10, 10, 10, 10});
  CHECK(p.n == 5 && p.wins == 3 && p.losses == 1 && p.ties == 1);
  const double sd = std::sqrt((0.0 + 4.0 + 4.0 + 1.0 + 1.0) / 4.0), se = sd / std::sqrt(5.0);
  CHECK(near(p.mean, 1.0) && near(p.se, se) && near(p.t, 1.0 / se) && p.t_defined);
  CHECK(near(p.p_sign, lab::sign_test_p(3, 1)));
  // identical vectors: zero everywhere, t = 0 and defined
  const lab::Paired z = lab::paired({1, 2, 3}, {1, 2, 3});
  CHECK(z.n == 3 && z.ties == 3 && z.mean == 0 && z.t == 0 && z.t_defined && z.p_sign == 1.0);
  // constant nonzero difference: no spread, t infinite with the sign of the difference
  const lab::Paired up = lab::paired({2, 3, 4}, {1, 2, 3}), dn = lab::paired({0, 1, 2}, {1, 2, 3});
  CHECK(std::isinf(up.t) && up.t > 0 && std::isinf(dn.t) && dn.t < 0);
  // one market: t is not defined
  const lab::Paired one_m = lab::paired({2}, {1});
  CHECK(!one_m.t_defined && one_m.n == 1 && one_m.wins == 1);
  // the base is subtracted (variant minus base), not the other way round
  CHECK(lab::paired({5, 5}, {1, 1}).mean == 4.0);
  // mismatched lengths use the common prefix
  CHECK(lab::paired({1, 2, 3}, {1, 2}).n == 2);
}

std::vector<std::string> read_lines(std::FILE* f) {
  std::vector<std::string> out;
  std::rewind(f);
  char b[512];
  while (std::fgets(b, sizeof b, f)) out.emplace_back(b);
  return out;
}

void test_format_p() {
  CHECK(lab::format_p(0.5) == "0.5000" && lab::format_p(1.0) == "1.0000" && lab::format_p(0.0001) == "0.0001");
  CHECK(lab::format_p(0.00009) == "<0.0001" && lab::format_p(0.0) == "<0.0001" && lab::format_p(0.03125) == "0.0312" + std::string(""));
}

void test_event_obs() {
  {
    lab::EventObs o;  // no file: everything is a no-op
    o.fill(1, 1, 2, 3, 4, 5);
    o.mid(1, true, 2);
    o.finish();
  }
  std::FILE* f = std::tmpfile();
  CHECK(f != nullptr);
  lab::EventObs o;
  o.open(f);
  o.mid(500'000, true, 2000);       // ms 0
  o.mid(900'000, true, 2002);       // ms 0, replaces the pending one
  o.mid(950'000, false, 7777);      // invalid book: ignored
  o.fill(1'200'000, 1, 1'000'100, 3, 2002, 3);   // ms 1
  o.mid(1'400'000, true, 2004);     // ms 1: writes ms 0's last mid
  o.mid(2'000'000, true, 2006);     // ms 2: writes ms 1's last mid
  o.fill(2'500'000, -1, 1'000'300, 2, 2006, 1);
  o.finish();                       // writes ms 2's mid
  o.finish();                       // second finish writes nothing
  const std::vector<std::string> L = read_lines(f);
  CHECK(L.size() == 5);
  if (L.size() == 5) {
    CHECK(L[0] == "{\"ts_ms\":0,\"event\":\"mid\",\"mid_x2\":2002}\n" || L[0].find("\"event\":\"fill\"") != std::string::npos);
    int fills = 0, mids = 0;
    for (const std::string& l : L) {
      fills += l.find("\"event\":\"fill\"") != std::string::npos;
      mids += l.find("\"event\":\"mid\"") != std::string::npos;
    }
    CHECK(fills == 2 && mids == 3);
    const std::string expect_fill1 =
        "{\"ts_ms\":1,\"event\":\"fill\",\"seg\":1,\"side\":\"buy\",\"px\":1000100,\"qty\":3,\"mid_x2\":2002,\"pos\":3,\"applied\":1}\n";
    const std::string expect_fill2 =
        "{\"ts_ms\":2,\"event\":\"fill\",\"seg\":1,\"side\":\"sell\",\"px\":1000300,\"qty\":2,\"mid_x2\":2006,\"pos\":1,\"applied\":1}\n";
    bool a = false, b = false, m0 = false, m1 = false, m2 = false;
    for (const std::string& l : L) {
      a |= l == expect_fill1;
      b |= l == expect_fill2;
      m0 |= l == "{\"ts_ms\":0,\"event\":\"mid\",\"mid_x2\":2002}\n";
      m1 |= l == "{\"ts_ms\":1,\"event\":\"mid\",\"mid_x2\":2004}\n";
      m2 |= l == "{\"ts_ms\":2,\"event\":\"mid\",\"mid_x2\":2006}\n";
    }
    CHECK(a && b && m0 && m1 && m2);
  }
  std::fclose(f);
}

bt::Market small_market(std::uint64_t seed, std::uint64_t requests = 30000) {
  bt::MarketConfig c;
  c.requests = requests;
  c.seed = seed;
  return bt::MarketGen::generate(c);
}

BtConfig lat() {
  BtConfig b;
  b.md_ns = 5000;
  b.ingress_ns = 10000;
  b.egress_ns = 10000;
  b.jitter_ns = 2000;
  b.seed = 1;
  return b;
}

void test_runs() {
  const bt::Market m = small_market(4242);
  const risk::RiskConfig rc = lab::lab_risk(40);
  CHECK(rc.max_order_qty == 100 && rc.max_position == 80);
  CHECK(lab::lab_risk(300).max_position == 600);
  CHECK(lab::lab_risk(39).max_position == 80);
  CHECK(lab::lab_risk(41).max_position == 82);

  const lab::RunResult a = lab::run_one(m, lat(), strat::MmConfig{}, rc, nullptr);
  const lab::RunResult b = lab::run_one(m, lat(), strat::MmConfig{}, rc, nullptr);
  CHECK(a.ok && b.ok && a.rep.digest == b.rep.digest && a.rep.pnl == b.rep.pnl);
  CHECK(a.rep.fills > 20);
  CHECK(lab::health_problem(a.rep).empty());

  // the observer hooks change nothing: the same run through the plain backtester gives the same digest
  {
    core::Region region = core::Region::create(128u << 20, core::RegionOptions{false, false, true, -1});
    core::Arena arena(region);
    strat::MarketMaker mm{strat::MmConfig{}};
    BtConfig c = lat();
    c.risk = rc;
    c.tick_raw = m.cfg.tick_raw;
    book::BookConfig bc;
    bc.base = m.cfg.base;
    bc.tick_raw = m.cfg.tick_raw;
    bc.num_levels = m.cfg.num_levels;
    auto* plain = new Backtester<strat::MarketMaker>();
    CHECK(plain->init(arena, m.tape.data(), m.tape.size(), bc, c, &mm));
    plain->run();
    CHECK(plain->report().digest == a.rep.digest);
    delete plain;
  }

  CHECK(a.finished && lab::run_problem(a).empty());
  // requote_ticks 0 with no pacing answers every replace ack with another replace: the budget turns the hang into a failure
  {
    strat::MmConfig loop;
    loop.requote_ticks = 0;
    loop.min_interval_ns = 0;
    const lab::RunResult l = lab::run_one(small_market(77, 3000), lat(), loop, rc, nullptr);
    CHECK(l.ok && !l.finished);
    CHECK(lab::run_problem(l).find("did not finish") == 0);
    // with pacing the same requote setting is fine
    strat::MmConfig paced;
    paced.requote_ticks = 0;
    CHECK(lab::run_one(small_market(77, 3000), lat(), paced, rc, nullptr).finished);
  }
  // a variant that equals the base is bit-identical; a different one is not
  strat::MmConfig same;
  CHECK(lab::run_one(m, lat(), same, rc, nullptr).rep.digest == a.rep.digest);
  strat::MmConfig noalpha;
  noalpha.alpha16 = 0;
  const lab::RunResult c0 = lab::run_one(m, lat(), noalpha, rc, nullptr);
  CHECK(c0.ok && c0.rep.digest != a.rep.digest);

  // the event log agrees with the report: fill count, final position, and PnL rebuilt from the lines
  std::FILE* f = std::tmpfile();
  CHECK(f != nullptr);
  const lab::RunResult e = lab::run_one(m, lat(), strat::MmConfig{}, rc, f);
  CHECK(e.rep.digest == a.rep.digest);
  std::rewind(f);
  char line[512];
  std::uint64_t nfill = 0, nmid = 0;
  long long pos = 0, last_fill_pos = 0, last_mid2 = 0, edge2 = 0;
  long double cash2 = 0;   // doubled cash, raw units
  long long prev_ms = -1;
  bool ordered = true;
  while (std::fgets(line, sizeof line, f)) {
    unsigned long long ts = 0;
    long long px = 0, qty = 0, mid2 = 0, ps = 0;
    char side[8];
    if (std::sscanf(line, "{\"ts_ms\":%llu,\"event\":\"fill\",\"seg\":1,\"side\":\"%7[a-z]\",\"px\":%lld,\"qty\":%lld,\"mid_x2\":%lld,\"pos\":%lld", &ts,
                    side, &px, &qty, &mid2, &ps) == 6) {
      const long long s = std::strcmp(side, "buy") == 0 ? 1 : -1;
      pos += s * qty;
      cash2 -= 2.0L * static_cast<long double>(s * px * qty);
      edge2 += s * qty * (mid2 - 2 * px);
      CHECK(ps == pos);
      last_fill_pos = ps;
      ++nfill;
    } else if (std::sscanf(line, "{\"ts_ms\":%llu,\"event\":\"mid\",\"mid_x2\":%lld", &ts, &mid2) == 2) {
      last_mid2 = mid2;
      ++nmid;
    } else {
      ordered = false;
    }
    ordered = ordered && static_cast<long long>(ts) >= prev_ms - 1;   // fills may sit one ms ahead of the pending mid
    prev_ms = static_cast<long long>(ts);
  }
  CHECK(ordered);
  CHECK(nfill == e.rep.fills && nmid > 5);
  CHECK(last_fill_pos == e.rep.final_pos || nfill == 0);
  const double pnl = static_cast<double>((cash2 + static_cast<long double>(pos) * static_cast<long double>(last_mid2)) / 2.0L / 10000.0L);
  CHECK(near(pnl, e.rep.pnl, 1e-6));
  CHECK(near(static_cast<double>(edge2) / 2.0 / 10000.0, e.rep.edge, 1e-6));
  std::fclose(f);
}

void test_health() {
  BtReport r;
  CHECK(lab::health_problem(r).empty());
  BtReport x = r; x.oms_anomalies = 1;       CHECK(lab::health_problem(x).find("oms anomalies 1") != std::string::npos);
  x = r; x.oms_mismatches = 2;               CHECK(lab::health_problem(x).find("oms mismatches 2") != std::string::npos);
  x = r; x.queue_inconsistent = 3;           CHECK(lab::health_problem(x).find("queue inconsistencies 3") != std::string::npos);
  x = r; x.queue_overflows = 4;              CHECK(lab::health_problem(x).find("queue overflows 4") != std::string::npos);
  x = r; x.tape_errors = 5;                  CHECK(lab::health_problem(x).find("tape errors 5") != std::string::npos);
  x = r; x.dropped_samples = 6;              CHECK(lab::health_problem(x).find("dropped samples 6") != std::string::npos);
  x = r; x.kill_reason = 7;                  CHECK(lab::health_problem(x).find("kill switch reason 7") != std::string::npos);
  x = r; x.oms_anomalies = 1; x.tape_errors = 2;
  CHECK(lab::health_problem(x) == "oms anomalies 1, tape errors 2");
  lab::RunResult rr;
  rr.finished = true;
  CHECK(lab::run_problem(rr).empty());
  rr.rep.tape_errors = 2;
  CHECK(lab::run_problem(rr) == "tape errors 2");
  rr.finished = false;
  CHECK(lab::run_problem(rr).find("did not finish within the step budget") == 0 && lab::run_problem(rr).find(", tape errors 2") != std::string::npos);
}

}  // namespace

int main() {
  test_parse_int();
  test_apply_param();
  test_parse_variant();
  test_stats();
  test_format_p();
  test_event_obs();
  test_runs();
  test_health();
  if (g_failures == 0) std::puts("test_lab: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
