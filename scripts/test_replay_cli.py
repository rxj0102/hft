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
    if verify_json is not None:
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
# no verify file given: no check printed
r = run(None)
check(r.returncode == 0 and "exchange snapshot check" not in r.stdout, "without --verify nothing is compared")
# an unreadable verify file
r = subprocess.run([BIN, "--verify", "/nonexistent/v", os.path.join(tempfile.mkdtemp(), "none")], capture_output=True, text=True, timeout=60)
check(r.returncode != 0, "a missing capture is refused")
print("test_replay_cli: all checks passed" if not failures else "test_replay_cli: %d FAILED" % failures)
sys.exit(1 if failures else 0)
