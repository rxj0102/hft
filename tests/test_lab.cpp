// Phase 23 tests: variant parsing, the statistics against hand-computed answers, the event writer, and the lab runs
// (determinism, hooks do not change results, the event log agrees with the report).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

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


void test_get_param() {
  strat::MmConfig c;
  std::string err;
  const char* keys[] = {"half_spread16", "alpha16", "skew16", "depth_levels", "qty", "max_pos", "requote_ticks", "min_leaves", "min_interval_ns"};
  long long k = 3;
  for (const char* key : keys) {
    CHECK(lab::apply_param(c, key, std::to_string(k), err));
    long long got = -1;
    CHECK(lab::get_param(c, key, got) && got == k);
    k += 2;
  }
  long long got = 0;
  CHECK(!lab::get_param(c, "nope", got));
}

void test_parse_double() {
  double v = 0;
  CHECK(lab::parse_double("0.5", v) && v == 0.5);
  CHECK(lab::parse_double("1e3", v) && v == 1000.0);
  CHECK(lab::parse_double("-2", v) && v == -2.0);
  CHECK(!lab::parse_double("", v) && !lab::parse_double("x", v) && !lab::parse_double("1x", v));
  CHECK(!lab::parse_double("nan", v) && !lab::parse_double("inf", v) && !lab::parse_double("1e999", v));
}

void test_grid() {
  const strat::MmConfig base;
  std::vector<lab::Variant> g;
  std::vector<lab::GridAxis> axes;
  std::string err;
  CHECK(lab::parse_grid("skew16=0,4", base, g, err, &axes));
  CHECK(g.size() == 2 && g[0].name == "skew16=0" && g[1].name == "skew16=4" && g[0].cfg.skew16 == 0 && g[1].cfg.skew16 == 4);
  CHECK(g[0].cfg.alpha16 == base.alpha16);
  CHECK(axes.size() == 1 && axes[0].key == "skew16" && axes[0].vals.size() == 2 && axes[0].vals[1] == 4);
  // two axes: the first changes slowest
  CHECK(lab::parse_grid("alpha16=0,8;skew16=1,2,3", base, g, err, &axes));
  CHECK(g.size() == 6 && axes.size() == 2);
  CHECK(g[0].name == "alpha16=0_skew16=1" && g[1].name == "alpha16=0_skew16=2" && g[2].name == "alpha16=0_skew16=3" && g[3].name == "alpha16=8_skew16=1" &&
        g[5].name == "alpha16=8_skew16=3");
  CHECK(g[4].cfg.alpha16 == 8 && g[4].cfg.skew16 == 2 && g[4].cfg.half_spread16 == base.half_spread16);
  // a single value is a grid of one; negative values work
  CHECK(lab::parse_grid("alpha16=-8", base, g, err) && g.size() == 1 && g[0].name == "alpha16=-8" && g[0].cfg.alpha16 == -8);
  // the axes pointer is optional and axes are cleared on every call
  CHECK(lab::parse_grid("qty=1", base, g, err, &axes) && axes.size() == 1);
  // refusals
  const char* bad[] = {"", "skew16", "skew16=", "=3", "skew16=1;", ";skew16=1", "skew16=1,,2", "skew16=x", "skew16=1;skew16=2", "nope=1", "qty=0", "qty=101",
                       "alpha16=0,8;skew16=1,-1"};
  for (const char* b : bad) CHECK(!lab::parse_grid(b, base, g, err));
  err.clear();
  CHECK(!lab::parse_grid("skew16=1;skew16=2", base, g, err) && err.find("repeated") != std::string::npos);
  // size limits: 50 values on an axis is fine, 51 is not; 500 points is fine, more is not; 8 axes fine, 9 not
  std::string v50 = "min_interval_ns=0", v51;
  for (int i = 1; i < 50; ++i) v50 += "," + std::to_string(i);
  v51 = v50 + ",50";
  CHECK(lab::parse_grid(v50, base, g, err) && g.size() == 50);
  CHECK(!lab::parse_grid(v51, base, g, err) && err.find("50 values") != std::string::npos);
  CHECK(lab::parse_grid("qty=1,2,3,4,5;max_pos=1,2,3,4,5,6,7,8,9,10;requote_ticks=0,1,2,3,4,5,6,7,8,9", base, g, err) && g.size() == 500);
  CHECK(!lab::parse_grid("qty=1,2,3,4,5,6,7;max_pos=1,2,3,4,5,6,7,8;requote_ticks=0,1,2,3,4,5,6,7,8", base, g, err) && err.find("500") != std::string::npos);
  // eight axes pass the axis limit (the point name is then too long); nine stop at the axis limit
  err.clear();
  CHECK(!lab::parse_grid("half_spread16=1;alpha16=1;skew16=1;depth_levels=1;qty=1;max_pos=1;requote_ticks=1;min_leaves=1", base, g, err) &&
        err.find("too long") != std::string::npos);
  err.clear();
  CHECK(!lab::parse_grid("half_spread16=1;alpha16=1;skew16=1;depth_levels=1;qty=1;max_pos=1;requote_ticks=1;min_leaves=1;min_interval_ns=1", base, g, err) &&
        err.find("8 grid axes") != std::string::npos);
  // a name over 48 characters is refused with a hint
  err.clear();
  CHECK(!lab::parse_grid("half_spread16=1048576;alpha16=-1048576;skew16=1048576;min_interval_ns=1000000", base, g, err) && err.find("too long") != std::string::npos);
}

lab::Row row_of(std::vector<double> pnl, std::vector<std::int64_t> pos) {
  lab::Row r;
  r.pnl = std::move(pnl);
  r.max_pos = std::move(pos);
  return r;
}

void test_rank() {
  std::vector<lab::Row> rows;
  rows.push_back(row_of({0, 0}, {5, 5}));       // 0: base
  rows.push_back(row_of({10, 12}, {40, 40}));   // 1: mean 11, pos 40
  rows.push_back(row_of({8, 8}, {10, 10}));     // 2: mean 8, pos 10
  rows.push_back(row_of({9, 7}, {10, 10}));     // 3: mean 8, pos 10 (ties 2)
  rows.push_back(row_of({3, 3}, {2, 2}));       // 4: mean 3, pos 2
  CHECK(near(lab::score(rows[1], 0), 11.0) && near(lab::score(rows[1], 0.5), 11.0 - 20.0) && near(lab::score(rows[4], 1.0), 1.0));
  std::vector<std::size_t> o = lab::rank(rows, 1, 0);
  CHECK(o.size() == 4 && o[0] == 1 && o[1] == 2 && o[2] == 3 && o[3] == 4);   // tie between 2 and 3 keeps index order
  o = lab::rank(rows, 1, 0.5);   // 1: -9, 2: 3, 3: 3, 4: 2
  CHECK(o[0] == 2 && o[1] == 3 && o[2] == 4 && o[3] == 1);
  o = lab::rank(rows, 0, 0);     // the base can be ranked too when asked
  CHECK(o.size() == 5 && o[4] == 0);
  CHECK(lab::rank(rows, 5, 0).empty());
  // many equal scores keep their order (std::sort would not guarantee it)
  std::vector<lab::Row> eq;
  eq.push_back(row_of({0}, {0}));
  for (int i = 0; i < 60; ++i) eq.push_back(row_of({1}, {1}));
  o = lab::rank(eq, 1, 0);
  bool in_order = o.size() == 60;
  for (std::size_t i = 0; in_order && i < o.size(); ++i) in_order = o[i] == i + 1;
  CHECK(in_order);
  // retained
  CHECK(near(lab::retained(10, 5), 0.5) && near(lab::retained(4, 6), 1.5) && near(lab::retained(10, -5), -0.5));
  CHECK(std::isnan(lab::retained(0, 5)) && std::isnan(lab::retained(-1, 5)));
}

void test_edge_axes() {
  std::vector<lab::GridAxis> axes = {{"alpha16", {0, 8, 16}}, {"skew16", {2}}, {"nope", {1, 2}}, {"qty", {5, 1, 3}}};
  strat::MmConfig c;
  c.alpha16 = 16;
  c.skew16 = 2;
  c.qty = 3;
  std::vector<std::string> e = lab::edge_axes(axes, c);
  CHECK(e.size() == 1 && e[0] == "alpha16 (largest)");
  c.alpha16 = 0;
  c.qty = 1;   // unsorted axis: 1 is its smallest
  e = lab::edge_axes(axes, c);
  CHECK(e.size() == 2 && e[0] == "alpha16 (smallest)" && e[1] == "qty (smallest)");
  c.alpha16 = 8;
  c.qty = 5;
  e = lab::edge_axes(axes, c);
  CHECK(e.size() == 1 && e[0] == "qty (largest)");
  c.qty = 3;
  CHECK(lab::edge_axes(axes, c).empty());
}

void test_regimes() {
  // "default" is the generator's own configuration: a run without --regimes is the same run
  const bt::MarketConfig d;
  const lab::Regime* def = lab::find_regime("default");
  CHECK(def != nullptr);
  const bt::MarketConfig dc = lab::regime_config(*def);
  CHECK(dc.jump_period == d.jump_period && dc.target_orders == d.target_orders && dc.informed_pct == d.informed_pct && dc.skew_pct == d.skew_pct &&
        dc.mean_gap_ns == d.mean_gap_ns);
  // every other regime differs from the default in exactly one setting group, and the names are unique
  std::size_t n = 0;
  for (const lab::Regime& r : lab::kRegimes) {
    ++n;
    CHECK(lab::find_regime(r.name) == &r);
    const bt::MarketConfig c = lab::regime_config(r);
    CHECK(c.jump_period == r.jump_period && c.target_orders == r.target_orders && c.informed_pct == r.informed_pct && c.skew_pct == r.skew_pct && c.mean_gap_ns == r.mean_gap_ns);
    CHECK(c.informed_pct <= 100 && c.skew_pct <= 100 && c.jump_period >= 1 && c.target_orders >= 1 && c.mean_gap_ns >= 2);
    if (&r != def) CHECK(c.jump_period != d.jump_period || c.target_orders != d.target_orders || c.informed_pct != d.informed_pct || c.skew_pct != d.skew_pct || c.mean_gap_ns != d.mean_gap_ns);
  }
  CHECK(n == 7);
  // the table, pinned: the documentation says "four times as often" and so on
  auto pin = [](const char* name, std::uint32_t jp, std::uint32_t tg, std::uint32_t inf, std::uint32_t sk, std::uint64_t gap) {
    const lab::Regime* r = lab::find_regime(name);
    return r && r->jump_period == jp && r->target_orders == tg && r->informed_pct == inf && r->skew_pct == sk && r->mean_gap_ns == gap;
  };
  CHECK(pin("default", 400, 200, 40, 35, 10'000) && pin("volatile", 100, 200, 40, 35, 10'000) && pin("quiet", 1600, 200, 40, 35, 10'000));
  CHECK(pin("informed", 400, 200, 70, 55, 10'000) && pin("uninformed", 400, 200, 10, 10, 10'000) && pin("thin", 400, 60, 40, 35, 10'000) &&
        pin("slow", 400, 200, 40, 35, 40'000));
  CHECK(lab::find_regime("") == nullptr && lab::find_regime("Default") == nullptr && lab::find_regime("quiet ") == nullptr);
  CHECK(lab::regime_config(*lab::find_regime("volatile")).jump_period < d.jump_period && lab::regime_config(*lab::find_regime("quiet")).jump_period > d.jump_period);
  CHECK(lab::regime_config(*lab::find_regime("thin")).target_orders < d.target_orders && lab::regime_config(*lab::find_regime("slow")).mean_gap_ns > d.mean_gap_ns);
  CHECK(lab::regime_config(*lab::find_regime("informed")).informed_pct > d.informed_pct && lab::regime_config(*lab::find_regime("uninformed")).informed_pct < d.informed_pct);

  // parsing
  std::vector<const lab::Regime*> out;
  std::string err;
  CHECK(lab::parse_regimes("quiet", out, err) && out.size() == 1 && out[0] == lab::find_regime("quiet"));
  CHECK(lab::parse_regimes("thin,default,slow", out, err) && out.size() == 3 && out[0]->name == std::string("thin") && out[1] == def && out[2]->name == std::string("slow"));
  CHECK(!lab::parse_regimes("", out, err) && err.find("unknown regime ''") != std::string::npos);
  CHECK(!lab::parse_regimes("quiet,", out, err) && err.find("unknown regime ''") != std::string::npos);
  CHECK(!lab::parse_regimes(",quiet", out, err));
  CHECK(!lab::parse_regimes("quiet,bogus", out, err) && err.find("'bogus'") != std::string::npos);
  CHECK(!lab::parse_regimes("quiet,thin,quiet", out, err) && err.find("'quiet' listed twice") != std::string::npos);
  CHECK(lab::parse_regimes("slow", out, err) && out.size() == 1);   // a failed parse does not leave stale entries behind
  CHECK(!lab::parse_regimes("nope", out, err) && out.empty());

  // across_regimes, by hand: base pnl {1,1} and variant pnl {3,1} (+1 mean), {0,0} (-1), {2,2} (+1)
  auto mk = [](double a, double b, double c, double e) {
    std::vector<lab::Row> rows(2);
    rows[0].pnl = {a, b};
    rows[1].pnl = {c, e};
    return rows;
  };
  std::vector<std::vector<lab::Row>> pr = {mk(1, 1, 3, 1), mk(1, 1, 0, 0), mk(1, 1, 2, 2)};
  const lab::AcrossRegimes a = lab::across_regimes(pr, 1);
  CHECK(a.d.size() == 3 && a.d[0] == 1.0 && a.d[1] == -1.0 && a.d[2] == 1.0 && a.better == 2 && a.worst == -1.0);
  pr = {mk(0, 0, 0, 0)};     // a tie is not better
  const lab::AcrossRegimes t = lab::across_regimes(pr, 1);
  CHECK(t.better == 0 && t.worst == 0.0 && t.d[0] == 0.0);
  pr = {mk(1, 1, 5, 5), mk(1, 1, 4, 4)};     // worst is the smallest, not the last or the first
  CHECK(lab::across_regimes(pr, 1).worst == 3.0);
  pr = {mk(1, 1, 0, 0), mk(1, 1, 5, 5), mk(1, 1, 4, 4)};
  CHECK(lab::across_regimes(pr, 1).worst == -1.0 && lab::across_regimes(pr, 1).better == 2);
  CHECK(lab::across_regimes({}, 1).d.empty() && lab::across_regimes({}, 1).worst == 0.0);

  // evaluate with a regime is the single run on that regime's market, and not the default's
  std::vector<lab::Variant> vars(1);
  vars[0].name = "base";
  const risk::RiskConfig rc = lab::lab_risk(40);
  std::vector<lab::Row> rd, rv;
  std::uint64_t dg1 = lab::kDigestSeed, dg2 = lab::kDigestSeed;
  int bad = 0;
  const bt::MarketConfig vol = lab::regime_config(*lab::find_regime("volatile"));
  CHECK(lab::evaluate(vars, 900, 2, 8000, lat(), rc, d, 0.0, "", "", rd, dg1, bad) == 0 && lab::evaluate(vars, 900, 2, 8000, lat(), rc, vol, 0.0, "", "", rv, dg2, bad) == 0);
  CHECK(dg1 != dg2 && rd[0].digest != rv[0].digest);
  for (std::size_t mi = 0; mi < 2; ++mi) {
    bt::MarketConfig mc = vol;
    mc.requests = 8000;
    mc.seed = 900 + mi;
    const bt::Market m = bt::MarketGen::generate(mc);
    const lab::RunResult r = lab::run_one(m, lat(), vars[0].cfg, rc, nullptr);
    CHECK(r.rep.digest == rv[0].digest[mi] && r.rep.pnl == rv[0].pnl[mi]);
  }
}

void test_latency() {
  std::vector<long long> v;
  std::string err;
  CHECK(lab::parse_latencies("0", v, err) && v.size() == 1 && v[0] == 0);
  CHECK(lab::parse_latencies("0,5,20,100000", v, err) && v.size() == 4 && v[0] == 0 && v[1] == 5 && v[2] == 20 && v[3] == 100000);
  CHECK(lab::parse_latencies("1,2,3,4,5,6,7,8,9,10,11,12", v, err) && v.size() == 12);
  CHECK(!lab::parse_latencies("1,2,3,4,5,6,7,8,9,10,11,12,13", v, err) && err == "at most 12 latencies");
  CHECK(!lab::parse_latencies("", v, err) && err.find("latency ''") != std::string::npos);
  CHECK(!lab::parse_latencies("5,", v, err) && err.find("latency ''") != std::string::npos);
  CHECK(!lab::parse_latencies(",5", v, err));
  CHECK(!lab::parse_latencies("5,x", v, err) && err.find("'x'") != std::string::npos);
  CHECK(!lab::parse_latencies("-1", v, err) && !lab::parse_latencies("100001", v, err) && !lab::parse_latencies("1.5", v, err) && !lab::parse_latencies(" 5", v, err));
  CHECK(!lab::parse_latencies("5,5", v, err) && err.find("strictly increasing") != std::string::npos);
  CHECK(!lab::parse_latencies("5,3", v, err) && err.find("'3' follows 5") != std::string::npos);
  CHECK(!lab::parse_latencies("zz", v, err) && v.empty());       // a failed parse leaves nothing behind

  lab::LatencyAxis ax = lab::LatencyAxis::Md;
  CHECK(lab::parse_latency_axis("all", ax) && ax == lab::LatencyAxis::All);
  CHECK(lab::parse_latency_axis("md", ax) && ax == lab::LatencyAxis::Md);
  CHECK(lab::parse_latency_axis("ingress", ax) && ax == lab::LatencyAxis::Ingress);
  CHECK(lab::parse_latency_axis("egress", ax) && ax == lab::LatencyAxis::Egress);
  ax = lab::LatencyAxis::Egress;
  CHECK(!lab::parse_latency_axis("", ax) && !lab::parse_latency_axis("ALL", ax) && !lab::parse_latency_axis("md ", ax) && ax == lab::LatencyAxis::Egress);
  CHECK(std::string(lab::axis_name(lab::LatencyAxis::All)) == "all" && std::string(lab::axis_name(lab::LatencyAxis::Md)) == "md" &&
        std::string(lab::axis_name(lab::LatencyAxis::Ingress)) == "ingress" && std::string(lab::axis_name(lab::LatencyAxis::Egress)) == "egress");

  BtConfig b;
  b.md_ns = 1000;
  b.ingress_ns = 2000;
  b.egress_ns = 3000;
  b.jitter_ns = 4000;
  b.queue_penalty_pct = 7;
  b.seed = 9;
  const BtConfig a = lab::with_latency(b, lab::LatencyAxis::All, 50);
  CHECK(a.md_ns == 50000 && a.ingress_ns == 50000 && a.egress_ns == 50000 && a.jitter_ns == 4000 && a.queue_penalty_pct == 7 && a.seed == 9);
  const BtConfig m = lab::with_latency(b, lab::LatencyAxis::Md, 50);
  CHECK(m.md_ns == 50000 && m.ingress_ns == 2000 && m.egress_ns == 3000);
  const BtConfig i = lab::with_latency(b, lab::LatencyAxis::Ingress, 50);
  CHECK(i.md_ns == 1000 && i.ingress_ns == 50000 && i.egress_ns == 3000);
  const BtConfig e = lab::with_latency(b, lab::LatencyAxis::Egress, 0);
  CHECK(e.md_ns == 1000 && e.ingress_ns == 2000 && e.egress_ns == 0);
  CHECK(b.md_ns == 1000);                                         // the argument is a copy

  // a longer delay on every hop changes the run, and the delayed run is what evaluate sees
  std::vector<lab::Variant> vars(1);
  vars[0].name = "base";
  const risk::RiskConfig rc = lab::lab_risk(40);
  std::vector<lab::Row> r0, r1;
  std::uint64_t d0 = lab::kDigestSeed, d1 = lab::kDigestSeed;
  int bad = 0;
  CHECK(lab::evaluate(vars, 900, 2, 8000, lab::with_latency(lat(), lab::LatencyAxis::All, 5), rc, bt::MarketConfig{}, 0.0, "", "", r0, d0, bad) == 0);
  CHECK(lab::evaluate(vars, 900, 2, 8000, lab::with_latency(lat(), lab::LatencyAxis::All, 200), rc, bt::MarketConfig{}, 0.0, "", "", r1, d1, bad) == 0);
  CHECK(d0 != d1 && r0[0].digest != r1[0].digest);
  const bt::Market mk = small_market(900, 8000);
  CHECK(lab::run_one(mk, lab::with_latency(lat(), lab::LatencyAxis::All, 200), vars[0].cfg, rc, nullptr).rep.digest == r1[0].digest[0]);
}

void test_evaluate() {
  std::vector<lab::Variant> vars(2);
  vars[0].name = "base";
  vars[1].name = "noalpha";
  vars[1].cfg.alpha16 = 0;
  const risk::RiskConfig rc = lab::lab_risk(40);
  std::vector<lab::Row> rows;
  std::uint64_t dg = lab::kDigestSeed;
  int bad = 0;
  CHECK(lab::evaluate(vars, 900, 2, 8000, lat(), rc, bt::MarketConfig{}, 0.0, "", "", rows, dg, bad) == 0 && bad == 0);
  CHECK(rows.size() == 2 && rows[0].pnl.size() == 2 && rows[1].digest.size() == 2 && rows[0].fills.size() == 2 && rows[0].max_pos.size() == 2);
  // each entry is the single run of that variant on that market, and the digest chains them market-major
  std::uint64_t want = lab::kDigestSeed;
  for (long long mi = 0; mi < 2; ++mi) {
    const bt::Market m = small_market(static_cast<std::uint64_t>(900 + mi), 8000);
    for (std::size_t vi = 0; vi < 2; ++vi) {
      const lab::RunResult r = lab::run_one(m, lat(), vars[vi].cfg, rc, nullptr);
      CHECK(r.rep.pnl == rows[vi].pnl[static_cast<std::size_t>(mi)] && r.rep.digest == rows[vi].digest[static_cast<std::size_t>(mi)]);
      CHECK(r.rep.max_abs_pos == rows[vi].max_pos[static_cast<std::size_t>(mi)] && r.rep.edge == rows[vi].edge[static_cast<std::size_t>(mi)]);
      want = lab::mix64(want, r.rep.digest);
    }
  }
  CHECK(dg == want);
  // the second market has a different seed from the first
  CHECK(rows[0].digest[0] != rows[0].digest[1]);
  // rows are rebuilt, not appended to
  std::uint64_t dg2 = lab::kDigestSeed;
  CHECK(lab::evaluate(vars, 900, 2, 8000, lat(), rc, bt::MarketConfig{}, 0.0, "", "", rows, dg2, bad) == 0 && rows[0].pnl.size() == 2 && dg2 == dg);
  // fees: pnl is the gross result less fee_for(volume), edge is untouched, a rebate is the negative, the digest does not move
  CHECK(lab::fee_for(200000.0, 2.5) == 50.0 && lab::fee_for(200000.0, -2.5) == -50.0 && lab::fee_for(0.0, 9.0) == 0.0);
  {
    std::vector<lab::Row> fr, rr;
    std::uint64_t dgf = lab::kDigestSeed, dgr = lab::kDigestSeed;
    CHECK(lab::evaluate(vars, 900, 2, 8000, lat(), rc, bt::MarketConfig{}, 3.0, "", "", fr, dgf, bad) == 0 && lab::evaluate(vars, 900, 2, 8000, lat(), rc, bt::MarketConfig{}, -3.0, "", "", rr, dgr, bad) == 0);
    CHECK(dgf == dg && dgr == dg);
    for (std::size_t vi = 0; vi < 2; ++vi)
      for (std::size_t mi = 0; mi < 2; ++mi) {
        const lab::RunResult r = lab::run_one(small_market(static_cast<std::uint64_t>(900 + mi), 8000), lat(), vars[vi].cfg, rc, nullptr);
        CHECK(fr[vi].fees[mi] == r.rep.volume * 3.0 / 1e4 && rr[vi].fees[mi] == -fr[vi].fees[mi] && fr[vi].fees[mi] > 0);
        CHECK(fr[vi].pnl[mi] == r.rep.pnl - fr[vi].fees[mi] && rr[vi].pnl[mi] == r.rep.pnl + fr[vi].fees[mi]);
        CHECK(fr[vi].edge[mi] == r.rep.edge && rows[vi].fees[mi] == 0.0);
      }
  }
  // an events directory that cannot be written is a setup failure
  CHECK(lab::evaluate(vars, 900, 1, 8000, lat(), rc, bt::MarketConfig{}, 0.0, "/nonexistent_dir_for_lab", "t_", rows, dg2, bad) == 2);
  // with a directory, one file per variant and market, named with the tag
  char tmpl[] = "/tmp/lab_test_XXXXXX";
  const char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir) {
    CHECK(lab::evaluate(vars, 900, 2, 8000, lat(), rc, bt::MarketConfig{}, 0.0, dir, "t_", rows, dg2, bad) == 0);
    for (const char* f : {"/t_base_m0.jsonl", "/t_base_m1.jsonl", "/t_noalpha_m0.jsonl", "/t_noalpha_m1.jsonl"}) {
      const std::string path = std::string(dir) + f;
      std::FILE* fp = std::fopen(path.c_str(), "r");
      CHECK(fp != nullptr);
      if (fp) {
        std::fclose(fp);
        std::remove(path.c_str());
      }
    }
    CHECK(rmdir(dir) == 0);
  }
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
  test_get_param();
  test_parse_double();
  test_grid();
  test_rank();
  test_edge_axes();
  test_regimes();
  test_latency();
  test_evaluate();
  test_health();
  if (g_failures == 0) std::puts("test_lab: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
