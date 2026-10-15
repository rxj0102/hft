#!/usr/bin/env python3
"""Performance regression gate for the tick-to-trade path.

Wall-clock latency on a shared CI runner is too noisy to gate on. Instruction
counts are not: under valgrind the same binary executes the same instructions
every time. So the gate runs the replay once under callgrind (only the replay
is counted, not setup) and compares instructions per input, branch mispredicts
per input and simulated cache misses per input with a checked-in baseline.

  scripts/perf_gate.py --bench build/perf/bench/bench_t2t            check
  scripts/perf_gate.py --bench build/perf/bench/bench_t2t --update   rewrite the baseline

Exit status 1 on a regression beyond the tolerance. The cache and branch
numbers are SIMULATED (valgrind's cache model and a simple branch predictor):
they find where a change moves misses, not what the hardware will do.
Instruction counts depend on the compiler, so the baseline records its major
version and the gate skips (with a notice) on a different one unless --strict.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "..", "bench", "baselines", "t2t_instr.json")

REQUESTS = 100000
CACHE = ["--cache-sim=yes", "--branch-sim=yes", "--I1=32768,8,64", "--D1=32768,8,64",
         "--LL=1048576,16,64"]  # LL models the 1 MiB L2 of the target core, not the L3


def run_callgrind(bench, out):
    cmd = ["valgrind", "--tool=callgrind", *CACHE, "--toggle-collect=*collect_replay*",
           f"--callgrind-out-file={out}", bench, "--requests", str(REQUESTS), "--workload", "100000000"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"callgrind failed:\n{r.stderr[-2000:]}")
    inputs = frames = cap = 0
    comp = ""
    for line in r.stdout.splitlines():
        m = re.match(r"workload: (\d+) inputs, (\d+) frames, capacity (\d+)", line)
        if m:
            inputs, frames, cap = map(int, m.groups())
        if line.startswith("compiler: "):
            comp = line[10:].strip()
    m = re.search(r"Collected : (.*)", r.stderr)
    ev = re.search(r"Events    : (.*)", r.stderr)
    if not (m and ev and inputs):
        sys.exit("could not parse callgrind output")
    vals = dict(zip(ev.group(1).split(), map(int, m.group(1).split())))
    return inputs, frames, cap, comp, vals


def components(out):
    """Instructions by the component the code is written in (source file group)."""
    ann = subprocess.run(["callgrind_annotate", "--auto=no", "--inclusive=no", "--threshold=100", out],
                         capture_output=True, text=True).stdout
    groups = [("feed handler", ("md/md_handler", "md/itch_wire", "md/itch_encoder")),
              ("book", ("book/", "core/fast_div")),
              ("strategy", ("strat/",)),
              ("gateway, risk, OMS, codec", ("gw/", "risk/", "oms/")),
              ("pipeline glue", ("t2t/pipeline", "t2t/session", "bench_t2t", "stl_", "bits/")),
              ("memory and clock", ("core/memory", "core/clock"))]
    tot = collections.Counter()
    for line in ann.splitlines():
        m = re.match(r"\s*([\d,]+) \(\s*[\d.]+%\)\s+(?:[\d,.]+ \(.*?\)\s+|\.\s+|0\s+)*.*?\s(\S+\.(?:hpp|cpp|h)):", line)
        if not m:
            m2 = re.match(r"\s*([\d,]+) \(\s*[\d.]+%\)\s+(\S+\.(?:hpp|cpp|h)):", line)
            if not m2:
                continue
            ir, path = int(m2.group(1).replace(",", "")), m2.group(2)
        else:
            ir, path = int(m.group(1).replace(",", "")), m.group(2)
        for name, keys in groups:
            if any(k in path for k in keys):
                tot[name] += ir
                break
        else:
            tot["other"] += ir
    return tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--tolerance", type=float, default=2.0, help="percent increase allowed in instructions per input")
    ap.add_argument("--strict", action="store_true", help="fail instead of skipping when the compiler major differs")
    ap.add_argument("--keep", help="keep the callgrind output at this path")
    a = ap.parse_args()

    out = a.keep or tempfile.mktemp(suffix=".callgrind")
    inputs, frames, cap, comp, v = run_callgrind(a.bench, out)
    cur = {
        "ir_per_input": v["Ir"] / inputs,
        "mispredicts_per_input": (v.get("Bcm", 0) + v.get("Bim", 0)) / inputs,
        "d1_misses_per_input": (v.get("D1mr", 0) + v.get("D1mw", 0)) / inputs,
        "l2_misses_per_input": (v.get("DLmr", 0) + v.get("DLmw", 0)) / inputs,
    }
    major = re.match(r"(\d+)", comp).group(1) if re.match(r"(\d+)", comp) else "?"
    print(f"workload: {inputs} inputs, {frames} order frames, book capacity {cap}, compiler {comp}")
    for k, x in cur.items():
        print(f"  {k:24s} {x:10.2f}")
    comp_ir = components(out)
    total = sum(comp_ir.values()) or 1
    print("  instructions by component (where the code lives):")
    for name, ir in sorted(comp_ir.items(), key=lambda kv: -kv[1]):
        print(f"    {name:28s} {ir / inputs:8.1f} per input  {100.0 * ir / total:5.1f}%")

    if a.update:
        doc = {"compiler_major": major, "compiler": comp, "isa": "x86-64-v3", "requests": REQUESTS, "inputs": inputs,
               "frames": frames, "capacity": cap, "tolerance_pct": a.tolerance, **{k: round(x, 3) for k, x in cur.items()}}
        with open(BASELINE, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print(f"baseline written to {os.path.relpath(BASELINE)}")
        return 0

    base = json.load(open(BASELINE))
    if base["compiler_major"] != major:
        msg = f"baseline was recorded with compiler major {base['compiler_major']}, this is {major}: instruction counts are not comparable"
        if a.strict:
            print("FAIL: " + msg)
            return 1
        print("SKIP: " + msg + " (use --update on this toolchain to rebase)")
        return 0
    if base["inputs"] != inputs or base["frames"] != frames:
        print(f"FAIL: the workload changed (baseline {base['inputs']} inputs/{base['frames']} frames, now {inputs}/{frames});"
              " the generator or the strategy changed behaviour, rebase on purpose with --update")
        return 1
    bad = False
    print(f"\n  {'metric':24s} {'baseline':>10s} {'now':>10s}  change")
    for k, x in cur.items():
        b = base[k]
        ch = 100.0 * (x / b - 1) if b else 0.0
        gate = k == "ir_per_input"
        flag = ""
        if gate and ch > a.tolerance:
            flag, bad = f"  REGRESSION (limit +{a.tolerance:.1f}%)", True
        elif k != "ir_per_input" and ch > 10.0:
            flag = "  note: simulated metric moved, look at it"
        elif ch < -5.0:
            flag = "  improved: rebase with --update to lock it in"
        print(f"  {k:24s} {b:10.2f} {x:10.2f}  {ch:+6.2f}%{flag}")
    print("PASS" if not bad else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
