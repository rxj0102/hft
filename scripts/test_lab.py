#!/usr/bin/env python3
"""Tests for build/release/hft_lab: option checking, determinism, paired output, and the event logs against pnl_report."""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import pnl_report  # noqa: E402

LAB = os.environ.get("HFT_LAB", os.path.join(ROOT, "build", "release", "hft_lab"))
failures = 0


def check(cond, what):
    global failures
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures += 1


def lab(*args, timeout=120):
    p = subprocess.run([LAB, *args], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


SMALL = ["--requests", "20000", "--markets", "4"]

print("option checking")
for args, why in [
    (["--bogus"], "unknown option"),
    (["--requests"], "missing value"),
    (["--requests", "abc"], "non-numeric requests"),
    (["--requests", "10"], "requests below the minimum"),
    (["--markets", "0"], "zero markets"),
    (["--markets", "1001"], "too many markets"),
    (["--seed", "-1"], "negative seed"),
    (["--md-us", "100001"], "latency out of range"),
    (["--queue-penalty-pct", "1001"], "penalty out of range"),
    (["--variant", "a:nope=1"], "unknown strategy parameter"),
    (["--variant", "a:qty=1000"], "parameter out of range"),
    (["--variant", "a b:qty=1"], "bad variant name"),
    (["--variant", "base:qty=1"], "variant named base collides"),
    (["--variant", "a:qty=1", "--variant", "a:qty=2"], "duplicate names"),
    (["--sweep", "skew16"], "sweep without values"),
    (["--sweep", "skew16="], "sweep with empty value list"),
    (["--sweep", "=5"], "sweep with empty key"),
    (["--sweep", "skew16=1,,2"], "sweep with an empty value"),
    (["--sweep", "skew16=1,x"], "sweep with a bad value"),
]:
    rc, out, err = lab(*args)
    check(rc == 1 and "usage" in err and out == "", "exit 1 and usage for " + why)

print("a run, twice")
with tempfile.TemporaryDirectory() as d:
    ev = os.path.join(d, "ev")
    os.mkdir(ev)
    js = os.path.join(d, "r.json")
    args = SMALL + ["--variant", "noalpha:alpha16=0", "--variant", "same", "--sweep", "skew16=0,4", "--json", js, "--events", ev]
    rc, out1, err1 = lab(*args)
    check(rc == 0, "exit 0 (stderr: %s)" % err1.strip()[:80])
    rc2, out2, _ = lab(*SMALL, "--variant", "noalpha:alpha16=0", "--variant", "same", "--sweep", "skew16=0,4")
    check(rc2 == 0 and out1 == out2, "same arguments, byte-identical report (events and json do not change it)")
    check("4 markets (seeds 1000..1003)" in out1 and "5 variants" in out1, "header counts markets, seeds and variants")
    names = re.findall(r"^(\S+)\s+[+-]\d", out1, re.M)
    check(names[:5] == ["base", "noalpha", "same", "skew16=0", "skew16=4"], "variants listed in order: " + " ".join(names[:5]))
    paired_same = re.search(r"^same\s+\+0\.00\s+0\.00\s+\+0\.00\s+0/0/4\s+1\.0000", out1, re.M)
    check(paired_same is not None, "a copy of the base has zero difference, 0 wins 0 losses 4 ties, p 1")
    check(re.search(r"^noalpha\s+[+-]\d+\.\d+\s+\d+\.\d+\s+[+-]\d+\.\d+\s+\d/\d/\d", out1, re.M) is not None, "paired line for noalpha")
    check("(0.125 here)" in out1, "says what the smallest sign p is with 4 markets (0.125)")
    check(re.search(r"run digest [0-9a-f]{16}", out1) is not None, "run digest printed")

    mm = re.search(r"^noalpha\s+([+-]\d+\.\d+)\s+(\d+\.\d+)", out1.split("paired against base")[1], re.M)
    r = json.load(open(js))
    check(mm is not None and abs(float(mm.group(1)) - r["variants"][1]["vs_base"]["mean"]) < 0.006 and abs(float(mm.group(2)) - r["variants"][1]["vs_base"]["se"]) < 0.006,
          "the printed paired difference and its se match the json for noalpha")
    check(r["markets"] == 4 and len(r["variants"]) == 5 and r["variants"][0]["name"] == "base", "json: markets and variants")
    check(all(len(v["pnl"]) == 4 and len(v["digest"]) == 4 for v in r["variants"]), "json: one entry per market")
    check(r["variants"][2]["digest"] == r["variants"][0]["digest"], "json: the copy has identical per-run digests")
    check(r["variants"][1]["digest"] != r["variants"][0]["digest"], "json: a different strategy has different digests")
    v1 = r["variants"][1]
    diffs = [a - b for a, b in zip(v1["pnl"], r["variants"][0]["pnl"])]
    check(abs(sum(diffs) / 4 - v1["vs_base"]["mean"]) < 1e-4, "json: vs_base mean matches the per-market numbers")
    check(v1["vs_base"]["wins"] + v1["vs_base"]["losses"] + v1["vs_base"]["ties"] == 4, "json: wins, losses and ties add up")

    print("event logs read by pnl_report")
    files = sorted(os.listdir(ev))
    check(len(files) == 5 * 4 and "base_m0.jsonl" in files and "skew16=4_m3.jsonl" in files, "one event file per variant and market")
    ok_all = True
    for vi, v in enumerate(r["variants"]):
        for mi in range(4):
            fills, mids, last, bad = pnl_report.load([os.path.join(ev, "%s_m%d.jsonl" % (v["name"], mi))])
            if not fills:
                continue
            rep = pnl_report.analyse(fills, mids, last, 10000, 1, 0.0, [0.01])
            same = abs(rep["total_quote"] - v["pnl"][mi]) < 1e-5 and abs(rep["total_from_cash_quote"] - v["pnl"][mi]) < 1e-5
            same = same and abs(rep["edge_total_quote"] - v["edge"][mi]) < 1e-5 and rep["position_gaps"] == 0 and bad == 0
            same = same and rep["fills"] == v["fills"][mi]
            if not same:
                ok_all = False
                print("    mismatch", v["name"], mi, rep["total_quote"], v["pnl"][mi], rep["fills"], v["fills"][mi])
    check(ok_all, "all 20 runs: pnl_report's PnL, cash-flow PnL, edge, fill count and position match the lab's own report")

print("the report from the command line")
with tempfile.TemporaryDirectory() as d:
    ev = os.path.join(d, "ev")
    os.mkdir(ev)
    lab(*SMALL, "--events", ev)
    p = subprocess.run([sys.executable, os.path.join(HERE, "pnl_report.py"), os.path.join(ev, "base_m0.jsonl"), "--qty-scale", "1", "--horizons", "0.001,0.01"],
                       capture_output=True, text=True)
    check(p.returncode == 0 and "matched the sum of the fills after every fill" in p.stdout and "markout 0.01s" in p.stdout, "pnl_report --qty-scale 1 reads a lab log")

print("setup failures")
rc, out, err = lab(*SMALL, "--events", "/nonexistent_dir_for_lab")
check(rc == 2 and "cannot write" in err, "an events directory that does not exist: exit 2")
rc, out, err = lab(*SMALL, "--json", "/nonexistent_dir_for_lab/x.json")
check(rc == 2 and "cannot write" in err, "an unwritable json path: exit 2")

print("a run that cannot finish is reported, not hung")
rc, out, err = lab("--requests", "5000", "--markets", "2", "--variant", "loop:requote_ticks=0,min_interval_ns=0", timeout=60)
check(rc == 3 and "UNHEALTHY run (variant loop, market 0)" in err and "did not finish" in err and "do not use these numbers" in err, "livelocked variant: exit 3 naming the variant")
check("loop" in out, "the report is still printed")
rc, out, err = lab("--requests", "5000", "--markets", "2", "--variant", "paced:requote_ticks=0", timeout=60)
check(rc == 0, "the same requote setting with pacing is fine")

print("the other settings change results")
_, a, _ = lab(*SMALL, "--variant", "w:half_spread16=30")
_, b, _ = lab(*SMALL, "--variant", "w:half_spread16=30", "--ingress-us", "200")
_, c, _ = lab(*SMALL, "--variant", "w:half_spread16=30", "--queue-penalty-pct", "200")
_, e, _ = lab(*SMALL, "--variant", "w:half_spread16=30", "--seed", "5000")
dg = [re.search(r"run digest (\w+)", x).group(1) for x in (a, b, c, e)]
check(len(set(dg)) == 4, "latency, queue penalty and seed each change the result")
base_dg = re.search(r"run digest (\w+)", lab(*SMALL, "--variant", "w:half_spread16=30")[1]).group(1)
for flag, val in [("--md-us", "300"), ("--egress-us", "300"), ("--jitter-us", "300"), ("--ingress-us", "300")]:
    d2 = re.search(r"run digest (\w+)", lab(*SMALL, "--variant", "w:half_spread16=30", flag, val)[1]).group(1)
    check(d2 != base_dg, flag + " changes the result")
rc, out, _ = lab(*SMALL, "--variant", "big:max_pos=100")
check("max position 200" in out, "risk limit follows the largest max_pos")
rc, out, _ = lab(*SMALL)
check("max position 80" in out, "risk limit floor with defaults")
check("ingress 200 us" in b and "queue penalty 200%" in c and "seeds 5000..5003" in e, "header shows the settings")

print("")
print("test_lab: all checks passed" if failures == 0 else "test_lab: %d FAILED" % failures)
sys.exit(1 if failures else 0)
