#!/usr/bin/env python3
"""Tests the exchange-snapshot comparison of hft_replay as a process, on small invented captures. No network.
Usage: test_replay_cli.py path/to/hft_replay"""
import os, re, subprocess, sys, tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/release/hft_replay"
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)
    else:
        print("  ok  ", what)


def px(c):
    return "%d.%02d" % (c // 100, c % 100)


def book(bid_sizes, ask_sizes, extra_bids=(), extra_asks=()):
    bids = ['["%s","%s"]' % (px(10000 - i), s) for i, s in enumerate(bid_sizes)] + ['["%s","%s"]' % e for e in extra_bids]
    asks = ['["%s","%s"]' % (px(10010 + i), s) for i, s in enumerate(ask_sizes)] + ['["%s","%s"]' % e for e in extra_asks]
    return '{"type":"snapshot","product_id":"X","bids":[%s],"asks":[%s]}' % (",".join(bids), ",".join(asks))


def run(verify_json, capture_book=None, exit_at=None, verify_at=None):
    d = tempfile.mkdtemp(prefix="rcli")
    cap, ver = os.path.join(d, "c.l2"), os.path.join(d, "c.l2.verify")
    sizes = [2 + i for i in range(8)]
    lines = ["#100\tstart\twss://x\tX", "1000000000\t" + (capture_book or book(sizes, sizes))]
    t = 1000000000
    for i in range(6):  # a few updates that change nothing near the verify moment
        t += 100000000
        lines.append('%d\t{"type":"l2update","product_id":"X","changes":[["buy","%s","%d"]]}' % (t, px(9990 - i), 3 + i))
    if exit_at is not None:  # every ask removed and one placed far above: the best ask leaves the window's safe zone
        t = exit_at
        gone = ",".join('["sell","%s","0"]' % px(10010 + i) for i in range(8))
        lines.append('%d\t{"type":"l2update","product_id":"X","changes":[%s,["sell","640.00","1"]]}' % (t, gone))
    lines.append("#%d\tstop" % (t + 1))
    open(cap, "w").write("\n".join(lines) + "\n")
    if isinstance(verify_json, list):  # several lines, written as given
        open(ver, "w").write("".join(x + "\n" for x in verify_json))
    elif verify_json is not None:
        open(ver, "w").write("%d\t%s\n" % (verify_at if verify_at is not None else t + 50000000, verify_json))
    args = [BIN] + (["--verify", ver] if verify_json is not None else []) + [cap]
    return subprocess.run(args, capture_output=True, text=True, timeout=60)


def counts(out):
    m = re.search(r"(\d+) of (\d+) levels identical, (\d+) differ in size, (\d+) absent from the replayed book", out)
    o = re.search(r"\((\d+) more of the nearest levels are outside the price window", out)
    return (tuple(int(x) for x in m.groups()) if m else None), (int(o.group(1)) if o else None)


sizes = [2 + i for i in range(8)]
# the book reproduced exactly, plus two asks far beyond the price window
r = run(book(sizes, sizes, extra_asks=[("5000.00", "1"), ("9000.00", "1")]))
c, o = counts(r.stdout)
check(r.returncode == 0 and c == (16, 16, 0, 0) and o == 2, "identical in the window; two far asks reported as outside, not as absent (%s %s)" % (c, o))
check("not compared" in r.stdout, "the note says they are not compared")
# nothing outside: no note
r = run(book(sizes, sizes))
c, o = counts(r.stdout)
check(r.returncode == 0 and c == (16, 16, 0, 0) and o is None and "outside the price window" not in r.stdout, "with nothing outside the window there is no note")
# a size that differs inside the window
r = run(book([7] + sizes[1:], sizes))
c, o = counts(r.stdout)
check(c == (15, 16, 1, 0) and o is None, "a differing size inside the window is counted as differing (%s)" % (c,))
# a level inside the window that the replay does not have
r = run(book(sizes, sizes, extra_bids=[("99.50", "1")]))
c, o = counts(r.stdout)
check(c == (16, 17, 0, 1) and o is None, "a missing level inside the window is counted as absent (%s)" % (c,))
# both at once
r = run(book([7] + sizes[1:], sizes, extra_bids=[("99.50", "1")], extra_asks=[("5000.00", "1")]))
c, o = counts(r.stdout)
check(c == (15, 17, 1, 1) and o == 1, "differing, absent and outside together (%s %s)" % (c, o))
# the segment ended long before the exchange snapshot: nothing is compared, and the output says why
fin = book(sizes, [], extra_asks=[("640.00", "1")])
r = run(fin, exit_at=2000000000, verify_at=9000000000)
c, o = counts(r.stdout)
check(r.returncode == 0 and c is None and "not comparable" in r.stdout and "7.0 s before the exchange snapshot" in r.stdout, "a snapshot 7 s after the segment ended is not comparable (%s)" % r.stdout[-300:].replace("\n", " | "))
# the same capture with the snapshot one second after the end is compared as usual
r = run(fin, exit_at=2000000000, verify_at=3000000000)
c, o = counts(r.stdout)
check(r.returncode == 0 and c is not None and "not comparable" not in r.stdout, "a snapshot one second after the end is compared (%s)" % (c,))
# a segment that did not end: a late snapshot is still compared
r = run(book(sizes, sizes), verify_at=900000000000)
c, o = counts(r.stdout)
check(c == (16, 16, 0, 0) and "not comparable" not in r.stdout, "a segment that did not end is compared however late the snapshot (%s)" % (c,))
# several exchange snapshots in one file: each is checked, and a summary adds them up
T = 1600000000  # the time of the last update in the fixture
def vl(js, at):
    return "%d\t%s" % (at, js)
ident = book(sizes, sizes)
r = run([vl(ident, T + 10000000), vl(ident, T + 300000000)])
c, o = counts(r.stdout)
check(r.returncode == 0 and "exchange snapshot 1 of 2, taken 1.6 s into the capture" in r.stdout and "exchange snapshot 2 of 2, taken 1.9 s into the capture" in r.stdout,
      "two snapshots: each is announced with its time (%s)" % r.stdout[-400:].replace("\n", " | "))
check("summary: 2 snapshots; 2 compared at least one level, 0 had none inside the price window," in r.stdout
      and "0 not comparable (the segment had ended), 0 without a usable book." in r.stdout
      and "over the compared ones: 32 of 32 levels identical, 0 differ in size, 0 absent from the replayed book" in r.stdout,
      "two identical snapshots add up to 32 of 32 (%s)" % r.stdout[-500:].replace("\n", " | "))
r = run([vl(ident, T + 10000000), vl(book([7] + sizes[1:], sizes), T + 300000000)])
check("over the compared ones: 31 of 32 levels identical, 1 differ in size, 0 absent from the replayed book" in r.stdout,
      "a difference in one of two snapshots shows in the sum (%s)" % r.stdout[-300:].replace("\n", " | "))
r = run([vl(ident, T + 10000000), vl(book(sizes, sizes, extra_bids=[("99.50", "1")]), T + 300000000)])
check("over the compared ones: 32 of 33 levels identical, 0 differ in size, 1 absent from the replayed book" in r.stdout,
      "a level the replay lacks in one of two snapshots shows in the sum (%s)" % r.stdout[-300:].replace("\n", " | "))
far = book([], [], extra_bids=[("4000.00", "1")], extra_asks=[("5000.00", "1")])
r = run([vl(ident, T + 10000000), vl(far, T + 300000000)])
check("1 compared at least one level, 1 had none inside the price window," in r.stdout
      and "over the compared ones: 16 of 16 levels identical" in r.stdout and "(2 more of the nearest levels were outside the window: not compared)" in r.stdout
      and "0 of 0 levels identical" in r.stdout,
      "a snapshot with nothing inside the window is counted apart, and adds nothing to the sum (%s)" % r.stdout[-500:].replace("\n", " | "))
r = run([vl(ident, T + 10000000), vl('{"type":"heartbeat"}', T + 300000000)])
check("1 compared at least one level, 0 had none inside the price window," in r.stdout and "0 not comparable (the segment had ended), 1 without a usable book." in r.stdout
      and "no usable snapshot or no book at that time" in r.stdout, "a line that is not a snapshot is counted as without a usable book (%s)" % r.stdout[-400:].replace("\n", " | "))
r = run([vl(fin, 3000000000), vl(fin, 9000000000)], exit_at=2000000000)
check("1 compared at least one level, 0 had none inside the price window," in r.stdout and "1 not comparable (the segment had ended)," in r.stdout
      and "not comparable. The replay segment ended 7.0 s before" in r.stdout, "one snapshot after the end is compared, one 7 s after is not (%s)" % r.stdout[-500:].replace("\n", " | "))
# a line without a tab is skipped; with one good line left it reads as a single snapshot, as before
r = run(["garbage without a tab", vl(ident, T + 10000000)])
c, o = counts(r.stdout)
check(c == (16, 16, 0, 0) and "exchange snapshot 1 of" not in r.stdout and "summary" not in r.stdout, "a line without a tab is skipped (%s)" % (c,))
r = run(["garbage without a tab", "more garbage"])
check("cannot read" in r.stdout, "a file with no usable line cannot be read")
# one snapshot: no per-snapshot header and no summary
r = run(ident)
check("exchange snapshot 1 of" not in r.stdout and "summary" not in r.stdout, "a single snapshot prints as before")
# no verify file given: no check printed
r = run(None)
check(r.returncode == 0 and "exchange snapshot check" not in r.stdout, "without --verify nothing is compared")
# an unreadable verify file
r = subprocess.run([BIN, "--verify", "/nonexistent/v", os.path.join(tempfile.mkdtemp(), "none")], capture_output=True, text=True, timeout=60)
check(r.returncode != 0, "a missing capture is refused")
print("test_replay_cli: all checks passed" if not failures else "test_replay_cli: %d FAILED" % failures)
sys.exit(1 if failures else 0)
