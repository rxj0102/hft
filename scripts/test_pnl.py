#!/usr/bin/env python3
"""Tests for scripts/pnl_report.py against hand-computed answers."""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pnl_report  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures += 1


def near(a, b, tol=1e-6):
    return a is not None and abs(a - b) <= tol


def fill(ts, side, px, qty, mid, pos, applied=1):
    return {"ts_ms": ts, "mono_s": ts / 1000.0, "event": "fill", "seg": 1, "side": side, "px": px, "qty": qty, "mid_x2": mid, "pos": pos, "applied": applied}


def mid(ts, m2):
    return {"ts_ms": ts, "mono_s": ts / 1000.0, "event": "mid", "mid_x2": m2}


def write(rows, raw_lines=()):
    fd, p = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
        for l in raw_lines:
            f.write(l + "\n")
    return p


def report(rows, raw=(), fee=0.0, horizons=(1, 5, 30)):
    p = write(rows, raw)
    fills, mids, last, bad = pnl_report.load([p])
    os.remove(p)
    r = pnl_report.analyse(fills, mids, last, 10000, 100000, fee, list(horizons))
    r["bad"] = bad
    return r


print("two samples in the same millisecond: the later line wins, whatever the values")
r = report([fill(1000, "buy", 1_000_000, 100_000, 2_010_000, 100_000), mid(1000, 2_000_000)])
check(near(r["last_mid"], 100.0) and near(r["unrealised_quote"], 0.0), "a mid logged after a fill in the same ms is the last mid")
r = report([mid(1000, 2_020_000), mid(1000, 2_000_000), fill(500, "buy", 1_000_000, 100_000, 2_000_000, 100_000)])
check(near(r["last_mid"], 100.0), "of two mid lines with one timestamp the later line is the last mid")
r = report([mid(1000, 2_000_000), fill(1000, "buy", 1_000_000, 100_000, 2_030_000, 100_000)])
check(near(r["last_mid"], 101.5), "a fill's own mid, logged after a mid line in the same ms, is the last mid")

print("a round trip, by hand")
rows = [mid(0, 2_000_000),
        fill(1000, "buy", 999_000, 100_000, 2_000_000, 100_000),       # buy 1.0 at 99.90, mid 100.00
        mid(2000, 2_004_000),
        fill(3000, "sell", 1_003_000, 40_000, 2_004_000, 60_000),      # sell 0.4 at 100.30, mid 100.20
        mid(7000, 2_002_000),
        mid(40000, 2_010_000)]
r = report(rows)
check(r["fills"] == 2 and r["buys"] == 1 and r["sells"] == 1, "two fills")
check(near(r["volume_quote"], 99.90 + 40.12), "volume %.4f" % r["volume_quote"])
check(near(r["realised_quote"], 0.16), "realised 0.4 x (100.30 - 99.90) = 0.16: %.6f" % r["realised_quote"])
check(near(r["unrealised_quote"], 0.36) and near(r["last_mid"], 100.50), "unrealised 0.6 x (100.50 - 99.90) = 0.36")
check(near(r["total_quote"], 0.52) and near(r["total_from_cash_quote"], 0.52) and abs(r["cash_check_diff"]) < 1e-9, "total 0.52 both ways")
check(near(r["edge_total_quote"], 0.14) and r["edge_fills"] == 2, "edge 0.10 + 0.04 = 0.14")
check(near(r["edge_bps_notional_weighted"], (10.0 * 99.90 + (0.10 / 100.20 * 1e4) * 40.12) / (99.90 + 40.12), 1e-6), "edge in bps, weighted by notional: %.4f" % r["edge_bps_notional_weighted"])
m1, m5, m30 = r["markout"]["1"], r["markout"]["5"], r["markout"]["30"]
check(near(m1["mean_bps"], (0.30 / 99.90 * 1e4 + 0.10 / 100.30 * 1e4) / 2, 1e-4), "1 s markout: %.4f bps" % m1["mean_bps"])
check(near(m5["mean_bps"], (0.30 / 99.90 * 1e4 + 0.20 / 100.30 * 1e4) / 2, 1e-4), "5 s markout (the mid moved to 100.10 before the second fill's horizon): %.4f" % m5["mean_bps"])
check(near(m30["mean_bps"], (0.20 / 99.90 * 1e4 + 0.20 / 100.30 * 1e4) / 2, 1e-4) and m30["share_negative"] == 0, "30 s markout: %.4f" % m30["mean_bps"])
check(near(r["max_abs_position_base"], 1.0) and near(r["mean_abs_position_base"], 24200 / 39000.0, 1e-9), "inventory: max 1.0, time-weighted mean %.5f" % r["mean_abs_position_base"])
check(near(r["span_s"], 39.0) and r["position_gaps"] == 0 and near(r["end_position_base"], 0.6), "span, no gaps, ends 0.6 long")

w1 = (0.30 / 99.90 * 1e4 * 99.90 + 0.10 / 100.30 * 1e4 * 40.12) / (99.90 + 40.12)
check(near(m1["weighted_bps"], w1, 1e-4), "1 s markout weighted by notional: %.4f" % m1["weighted_bps"])

print("adverse selection and fees")
rows = [fill(1000, "buy", 1_000_000, 100_000, 2_000_000, 100_000), mid(2000, 1_990_000), mid(10000, 1_980_000)]
r = report(rows, fee=10.0, horizons=(1, 5))
check(near(r["markout"]["1"]["mean_bps"], -50.0, 1e-6) and r["markout"]["1"]["share_negative"] == 1.0, "a buy followed by a fall: -50 bps at 1 s")
check(near(r["markout"]["5"]["mean_bps"], -50.0, 1e-6), "and at 5 s the mid at that time is the last one before it (99.50 = 99.50): -50 bps")
check(near(r["fees_quote"], 0.1) and near(r["total_quote"], (99.0 - 100.0) - 0.1), "10 bps fee on 100 of notional is 0.10; total %.4f" % r["total_quote"])
r = report([fill(1000, "buy", 1_000_000, 100_000, 2_000_000, 100_000), mid(1500, 2_000_000)], horizons=(1, 5))
check(r["markout"]["1"]["n"] == 0 and r["markout"]["5"]["n"] == 0, "a horizon the log does not reach gives no markout (nothing invented)")

print("crossing zero, carried inventory")
r = report([fill(1000, "buy", 1_000_000, 100_000, 2_000_000, 100_000), fill(2000, "sell", 1_010_000, 150_000, 2_020_000, -50_000), mid(3000, 2_010_000)])
check(near(r["realised_quote"], 1.0) and near(r["unrealised_quote"], 0.25) and near(r["total_quote"], 1.25) and near(r["total_from_cash_quote"], 1.25),
      "long 1 at 100, sell 1.5 at 101: realised 1.00, short 0.5 from 101 marked at 100.50 gives 0.25, total 1.25 (%.4f / %.4f / %.4f)" % (r["realised_quote"], r["unrealised_quote"], r["total_quote"]))
r = report([fill(1000, "sell", 1_000_000, 100_000, 2_000_000, -100_000), fill(2000, "buy", 990_000, 50_000, 1_990_000, -50_000), mid(3000, 1_980_000)])
check(near(r["realised_quote"], 0.5) and near(r["unrealised_quote"], -0.5 * (99.0 - 100.0)) and near(r["total_quote"], 1.0) and near(r["total_from_cash_quote"], 1.0),
      "short 1 at 100, buy back 0.5 at 99: realised 0.50, the rest (short 0.5 from 100, mid 99.00) 0.50, total 1.00 (%.4f / %.4f)" % (r["realised_quote"], r["total_quote"]))
r = report([fill(1000, "buy", 1_000_000, 100_000, 2_000_000, 100_000), mid(1500, 2_010_000), {"ts_ms": 20000, "event": "start"}], horizons=(5,))
check(r["markout"]["5"]["n"] == 1 and near(r["markout"]["5"]["mean_bps"], 50.0, 1e-6), "the mid only gets logged when it changes: a quiet log still reaches the horizon, the last mid holds")
r = report([fill(1000, "buy", 999_000, 100_000, 2_000_000, 200_000), mid(2000, 2_010_000)])    # the run started with 1.0 already held
check(near(r["start_position_base"], 1.0) and near(r["total_quote"], 1.10) and near(r["total_from_cash_quote"], 1.10),
      "started with 1.0, valued at the first fill's mid (100.00): total 1.10 (%.4f)" % r["total_quote"])

print("what is not trusted")
r = report([fill(1000, "buy", 1_000_000, 100_000, 2_000_000, 100_000), fill(2000, "buy", 1_000_000, 100_000, 2_000_000, 100_000, applied=0),
            fill(3000, "sell", 1_000_000, 100_000, 2_000_000, 100_000)], raw=["not json", '{"event":"fill"}', '{"ts_ms":5,"event":"fill","side":"up","px":1,"qty":1,"pos":1,"applied":1}'])
check(r["fills"] == 2 and r["bad"] == 3, "a fill the session ignored is not counted; unreadable lines are counted (%d fills, %d bad)" % (r["fills"], r["bad"]))
check(r["position_gaps"] == 1, "a position that disagrees with the fills is flagged (%d)" % r["position_gaps"])
r = report([])
check(r == {"fills": 0, "bad": 0}, "no fills: nothing to report")
r = report([fill(1000, "buy", 1_000_000, 100_000, -1, 100_000), mid(2000, 2_000_000)])
check(r["edge_fills"] == 0 and r["edge_bps_notional_weighted"] is None, "a fill without a mid has no edge")

print("the command line")
p = write(rows)
out = tempfile.mktemp(suffix=".json")
x = subprocess.run([sys.executable, os.path.join(HERE, "pnl_report.py"), p, "--json", out, "--horizons", "1"], capture_output=True, text=True)
check(x.returncode == 0 and "PnL and fill quality" in x.stdout and "markout 1s" in x.stdout, "text report: %s" % x.stdout[:80].replace("\n", " | "))
check(json.load(open(out))["fills"] == 1, "json report")
x = subprocess.run([sys.executable, os.path.join(HERE, "pnl_report.py"), write([])], capture_output=True, text=True)
check(x.returncode == 0 and "no fills" in x.stdout, "an empty log says so")
os.remove(p); os.remove(out)

if failures:
    sys.exit("%d pnl test failures" % failures)
print("test_pnl: all checks passed")
