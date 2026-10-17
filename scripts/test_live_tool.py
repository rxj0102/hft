#!/usr/bin/env python3
"""Tests the hft_live program as a process: files, pipes, signals, real time.
Usage: test_live_tool.py path/to/hft_live. All data is invented; no network."""
import os, signal, subprocess, sys, tempfile, time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/release/hft_live"
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)


def snap(t, shift=0):
    px = lambda c: "%d.%02d" % ((c + shift) // 100, (c + shift) % 100)
    bids = ",".join('["%s","%d"]' % (px(10000 - i), 2 + i) for i in range(8))
    asks = ",".join('["%s","%d"]' % (px(10010 + i), 2 + i) for i in range(8))
    return '%d\t{"type":"snapshot","product_id":"X","bids":[%s],"asks":[%s]}\n' % (t, bids, asks)


def upd(t, side, price, size):
    return '%d\t{"type":"l2update","product_id":"X","changes":[["%s","%s","%s"]]}\n' % (t, side, price, size)


def beat(t):
    return '%d\t{"type":"heartbeat","sequence":1}\n' % t


def run_file(text, *args):
    path = tempfile.mktemp(suffix=".l2")
    open(path, "w").write(text)
    p = subprocess.run([BIN, "--from", path, "--report-s", "1000"] + list(args), capture_output=True, text=True, timeout=60)
    os.remove(path)
    return p


def basic_session(t0=1_000_000):
    s = "#%d\tstart\tu\tX\n" % t0 + snap(t0 + 1000)
    for i, sz in enumerate(["3", "4", "5", "6"]):
        s += upd(t0 + 2000 + 1000 * i, "buy", "100.00", sz)
    return s


# 1. a file with a reconnect: one clean segment
p = run_file(basic_session() + "#2000000\treconnect\n#2000100\tstop\n")
check(p.returncode == 0, "file: clean run exits 0 (got %d)\n%s" % (p.returncode, p.stderr))
check("1 started" in p.stdout and "1 ended: reconnect/marker" in p.stdout, "file: one segment ended by the marker\n" + p.stdout)
check("flat," in p.stdout and "NOT FLAT" not in p.stdout and "matched the model" in p.stdout, "file: ended flat, book matched\n" + p.stdout)

# 2. silence in the recording: the stale-feed guard fires at the recorded time, deterministically
text = basic_session() + snap(20_000_000_000)  # 20 s later: a new snapshot, after the quiet
a = run_file(text, "--stale-ms", "1000")
b = run_file(text, "--stale-ms", "1000")
check("1 stale-feed pulls" in a.stdout and "stale feed" in a.stdout, "file: stale feed pulled the quotes\n" + a.stdout)
check("2 started" in a.stdout, "file: quoting resumed at the next snapshot\n" + a.stdout)
sig = lambda o: [l for l in o.splitlines() if "wall-clock" not in l and "per update" not in l and "segment start" not in l]
check(sig(a.stdout) == sig(b.stdout), "file: the drill is reproducible")
c = run_file(text, "--stale-ms", "60000")
check("0 stale-feed pulls" in c.stdout, "file: a longer stale limit does not fire")

# 3. exit codes
check(run_file("").returncode == 5, "file: nothing usable exits 5")
check(subprocess.run([BIN, "--from", "/nonexistent/x"], capture_output=True).returncode == 1, "missing file exits 1")
check(subprocess.run([BIN, "--bogus"], capture_output=True).returncode == 1, "bad option exits 1")

# 4. a pipe and a signal: SIGTERM latches the kill switch and cancels everything
proc = subprocess.Popen([BIN, "--report-s", "1000"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
proc.stdin.write("#1\tstart\tu\tX\n" + snap(2))
for i, sz in enumerate(["3", "4", "5", "6"]):
    proc.stdin.write(upd(3 + i, "buy", "100.00", sz))
proc.stdin.flush()
time.sleep(0.5)
proc.send_signal(signal.SIGTERM)
out, err = proc.communicate(timeout=10)
check(proc.returncode == 0, "pipe: SIGTERM exits 0 (got %s)\n%s" % (proc.returncode, err))
check("kill switch latched" in out and "1 ended: kill" in out, "pipe: SIGTERM is a kill\n" + out)
check("flat (kill switch on)" in out, "pipe: flat, kill switch on\n" + out)

# 5. a pipe closing normally is a shutdown, not a kill
proc = subprocess.Popen([BIN, "--report-s", "1000"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
proc.stdin.write(snap(2) + upd(3, "buy", "100.00", "5"))
proc.stdin.close()
out, err = proc.communicate(timeout=10)
check(proc.returncode == 0 and "input ended" in out and "1 ended: shutdown" in out, "pipe: EOF is a shutdown\n" + out)
check("(kill switch on)" not in out, "pipe: EOF does not latch the kill switch\n" + out)

# 6. real time: the feed goes quiet on a live pipe; the guard fires on the process's own clock, with no input to trigger it
proc = subprocess.Popen([BIN, "--report-s", "1000", "--stale-ms", "300"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
proc.stdin.write(snap(2) + upd(3, "buy", "100.00", "5"))
proc.stdin.flush()
time.sleep(1.2)  # nothing arrives; stdin stays open
proc.stdin.close()
out, err = proc.communicate(timeout=10)
check("1 stale-feed pulls" in out and "stale feed" in out, "pipe: silence on an open pipe pulls the quotes\n" + out)

# 7. a heartbeat keeps the feed alive
proc = subprocess.Popen([BIN, "--report-s", "1000", "--stale-ms", "400"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
proc.stdin.write(snap(2) + upd(3, "buy", "100.00", "5"))
proc.stdin.flush()
for i in range(6):
    time.sleep(0.2)
    proc.stdin.write(beat(10 + i))
    proc.stdin.flush()
proc.stdin.close()
out, err = proc.communicate(timeout=10)
check("0 stale-feed pulls" in out, "pipe: heartbeats keep the feed alive\n" + out)

# 8. the price leaves the window: a resubscribe is requested through the file
flag = tempfile.mktemp(suffix=".resnap")
proc = subprocess.Popen([BIN, "--report-s", "1000", "--levels", "4096", "--resnap-file", flag], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
proc.stdin.write(snap(2))
for i in range(8):
    proc.stdin.write(upd(3 + i, "sell", "100.%02d" % (10 + i), "0"))
proc.stdin.write(upd(20, "sell", "118.00", "1"))
proc.stdin.flush()
time.sleep(0.5)
check(os.path.exists(flag), "pipe: leaving the window asks for a resubscribe (file written)")
proc.stdin.close()
out, err = proc.communicate(timeout=10)
check("1 ended: left price window" in out and "1 resubscribe requests" in out, "pipe: ended by the window\n" + out)
if os.path.exists(flag):
    os.remove(flag)

# 9. operations output: JSON event log and metrics file
import json
logp = tempfile.mktemp(suffix=".log"); metp = tempfile.mktemp(suffix=".prom")
p = run_file(basic_session() + "#2000000\treconnect\n", "--log", logp, "--metrics", metp)
ev = [json.loads(l) for l in open(logp)]
names = [e["event"] for e in ev]
check(names == ["start", "segment_start", "segment_end", "exit"], "log: events in order, no spurious halt: %s" % names)
end = [e for e in ev if e["event"] == "segment_end"][0]
check(end["reason"] == "reconnect/marker" and end["flat"] == 1 and end["kill_latched"] == 0 and end["book_matched_model"] == 1,
      "log: segment_end fields %s" % end)
check(all("ts_ms" in e and "mono_s" in e for e in ev), "log: every line is stamped")
m = open(metp).read()
vals = {}
for l in m.splitlines():
    if l and not l.startswith("#"):
        k, v = l.rsplit(" ", 1)
        vals[k] = float(v)
check(vals["hft_live_segments_started_total"] == 1 and vals["hft_live_feed_messages_total"] == 5, "metrics: counters %s" % vals)
check(vals['hft_live_segments_ended_total{reason="reconnect/marker"}'] == 1 and vals["hft_live_model_check_failures_total"] == 0, "metrics: labelled counter")
check(vals["hft_live_orders_new_total"] > 0 and vals["hft_live_quoting"] == 0, "metrics: orders counted, not quoting after the end")
check(not os.path.exists(metp + ".tmp"), "metrics: temporary file renamed away")

# stale and kill appear in the log; the metrics file is refreshed while the feed is quiet
logp2 = tempfile.mktemp(suffix=".log"); metp2 = tempfile.mktemp(suffix=".prom")
proc = subprocess.Popen([BIN, "--report-s", "1000", "--stale-ms", "300", "--log", logp2, "--metrics", metp2, "--metrics-s", "0.2"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
proc.stdin.write(snap(2) + upd(3, "buy", "100.00", "5"))
proc.stdin.flush()
time.sleep(0.8)
t1 = os.stat(metp2).st_mtime_ns
time.sleep(0.5)
t2 = os.stat(metp2).st_mtime_ns
check(t2 > t1, "metrics: file keeps being refreshed with no input (this is what a watchdog watches)")
proc.send_signal(signal.SIGTERM)
out, err = proc.communicate(timeout=10)
ev = [json.loads(l) for l in open(logp2)]
names = [e["event"] for e in ev]
check("stale_feed" in names and "resubscribe_requested" in names, "log: stale feed recorded: %s" % names)
ends = [e for e in ev if e["event"] == "segment_end"]
check(len(ends) == 1 and ends[0]["reason"] == "stale feed", "log: segment ended by stale feed: %s" % ends)
check(names[-1] == "exit" and ev[-1]["cause"] == "signal", "log: exit by signal recorded")
for f_ in (logp, metp, logp2, metp2):
    if os.path.exists(f_): os.remove(f_)

# 10. the order mirror (--orders-fd): lines, neutrality, reader gone, reader stalled
def churn(n):
    """a snapshot, then n updates that make the best bid jump up and down, so the strategy keeps replacing its quotes"""
    out = [snap(1_000_000)]
    t = 1_001_000
    for i in range(n):
        out.append(upd(t, "buy", "100.05", "5" if i % 2 == 0 else "0"))
        t += 100_000  # acks (50 us away) are back before the next move, so the strategy replaces rather than waits
    return "".join(out)


def run_with_pipe(text, reader, *extra):
    """runs hft_live --from with --orders-fd attached to a pipe. reader: 'read' (drained), 'closed', 'stalled'"""
    path = tempfile.mktemp(suffix=".l2"); logp_ = tempfile.mktemp(suffix=".log")
    open(path, "w").write(text)
    r, w = os.pipe()
    cmd = [BIN, "--from", path, "--report-s", "1000", "--log", logp_, "--orders-fd", str(w)] + list(extra)
    got = []
    if reader == "closed":
        os.close(r)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, pass_fds=(w,))
    os.close(w)
    if reader == "read":
        import threading
        def drain():
            with os.fdopen(r, "r") as f:
                for line in f:
                    got.append(line)
        th = threading.Thread(target=drain); th.start()
    out, err = proc.communicate(timeout=120)
    if reader == "read":
        th.join()
    elif reader == "stalled":
        os.close(r)
    ev = [json.loads(l) for l in open(logp_)]
    os.remove(path); os.remove(logp_)
    return proc.returncode, out, ev, got


def wire_hash(out):
    for l in out.splitlines():
        if "wire hash" in l:
            return l.split()[-1]
    return None


text = churn(300)
base = run_file(text)
rc, out, ev, got = run_with_pipe(text, "read")
rows = [json.loads(l) for l in got]
kinds = [r["t"] for r in rows]
check(rc == 0 and kinds[0] == "hello" and rows[0]["px_scale"] == 10000 and rows[0]["qty_scale"] == 100000, "mirror: starts with a hello that states the units")
check(kinds.count("new") >= 2 and kinds.count("rep") > 100 and kinds.count("can") >= 2, "mirror: new/rep/can lines present: %s" % {k: kinds.count(k) for k in set(kinds)})
check(kinds[-1] == "end" and rows[-1]["seg"] == 1, "mirror: the segment end marker comes after the final cancels")
news = {r["id"] for r in rows if r["t"] == "new"}; cans = {r["id"] for r in rows if r["t"] == "can"}
check(news and news <= cans, "mirror: every order that was sent was cancelled before the segment ended")
check(all(r["px"] % 100 == 0 for r in rows if "px" in r), "mirror: prices are whole ticks")
check(wire_hash(out) is not None and wire_hash(out) == wire_hash(base.stdout), "mirror: attaching the pipe does not change the order flow (wire hash %s)" % wire_hash(out))
check(not any(e["event"] == "orders_out_lost" for e in ev), "mirror: nothing lost with a reader")

rc, out, ev, _ = run_with_pipe(text, "closed")
lost = [e for e in ev if e["event"] == "orders_out_lost"]
check(rc == 0 and len(lost) == 1 and lost[0]["why"] == "write_failed", "mirror: reader gone -> orders_out_lost(write_failed), session unaffected, exit 0: %s" % lost)
check(wire_hash(out) == wire_hash(base.stdout), "mirror: and the order flow is still the same")

big = churn(20000)
base_big = run_file(big)
rc, out, ev, _ = run_with_pipe(big, "stalled")
lost = [e for e in ev if e["event"] == "orders_out_lost"]
check(rc == 0 and len(lost) == 1 and lost[0]["why"] == "overflow", "mirror: reader stalled -> orders_out_lost(overflow), exit 0: %s" % lost)
check(wire_hash(out) == wire_hash(base_big.stdout), "mirror: a stalled reader does not change the order flow (%s)" % wire_hash(out))

# a reader that starts late: the backlog (more than a pipe holds) must all arrive, in order, including the final cancels and the end marker
def slow_reader_run(text):
    path = tempfile.mktemp(suffix=".l2"); open(path, "w").write(text)
    r, w = os.pipe()
    proc = subprocess.Popen([BIN, "--from", path, "--report-s", "1000", "--orders-fd", str(w)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, pass_fds=(w,))
    os.close(w)
    time.sleep(1.0)  # hft_live has finished its input and is waiting to flush
    data = b""
    with os.fdopen(r, "rb") as f:
        data = f.read()
    out, err = proc.communicate(timeout=60)
    os.remove(path)
    return proc.returncode, [json.loads(l) for l in data.decode().splitlines()]

mid = churn(3000)
rc, _, _, fast = run_with_pipe(mid, "read")
rc2, slow = slow_reader_run(mid)
check(rc2 == 0 and len(slow) == len(fast) and len(slow) > 3000, "mirror: a late reader receives every line (%d of %d)" % (len(slow), len(fast)))
check(slow == [json.loads(l) for l in fast] and slow[-1]["t"] == "end", "mirror: same lines in the same order, the end marker last")

# a reader that never reads and a backlog that fits the buffer: the exit waits two seconds, then reports the loss
t0 = time.time()
rc, out, ev, _ = run_with_pipe(mid, "stalled")
lost = [e for e in ev if e["event"] == "orders_out_lost"]
check(rc == 0 and len(lost) == 1 and lost[0]["why"] == "unflushed_at_exit" and time.time() - t0 >= 2.0,
      "mirror: unread backlog at exit -> orders_out_lost(unflushed_at_exit) after the wait: %s" % lost)

# the two quotes are on the right sides of the market
firsts = [r for r in rows if r["t"] == "new"][:2]
sides = {r["side"]: r["px"] for r in firsts}
check(set(sides) == {"buy", "sell"} and sides["buy"] < sides["sell"], "mirror: buy below sell in the first two orders: %s" % sides)

rc = subprocess.run([BIN, "--from", "/dev/null", "--orders-fd", "55"], capture_output=True).returncode
check(rc == 1, "mirror: --orders-fd on a descriptor that is not open: exit 1")

# 11. external venue: --exchange-fd. A scripted exchange in this process answers the orders hft_live writes.
import threading


class ScriptedExchange:
    """reads order lines, answers each with a report line, optionally with a defect"""
    def __init__(self, orders_r, reports_w, mode="ok"):
        self.orders_r, self.reports_w, self.mode = orders_r, reports_w, mode
        self.seen, self.sent = [], []
        self.garbage_events = 0
        self.filled_one = False
        self.th = threading.Thread(target=self.run)
        self.th.start()

    def reply(self, rec, rt, **kw):
        out = {"t": "rep", "seg": rec["seg"], "rt": rt, "id": rec["id"], "px": rec.get("px", 0), "qty": rec.get("qty", 5),
               "leaves": kw.pop("leaves", rec.get("qty", 5)), "reason": kw.pop("reason", "none"), "side": rec.get("side", "buy")}
        out.update(kw)
        line = json.dumps(out) + "\n"
        self.sent.append(out)
        try:
            os.write(self.reports_w, line.encode())
        except OSError:
            pass

    def run(self):
        with os.fdopen(self.orders_r, "r") as f:
            for line in f:
                rec = json.loads(line)
                self.seen.append(rec)
                t = rec["t"]
                if self.mode == "silent" or t in ("hello", "end"):
                    continue
                if self.mode == "garbage" and len(self.seen) % 3 == 0:
                    self.garbage_events += 1
                    os.write(self.reports_w, b"this is not json\n{\"t\":\"rep\",\"seg\":1}\n"
                             b"{\"t\":\"rep\",\"seg\":1,\"rt\":\"accepted\",\"id\":1,\"px\":5,\"qty\":-5,\"leaves\":5}\n")
                if t == "new" and self.mode in ("close_ok", "close_none", "close_slow"):
                    self.reply(rec, "accepted")
                    if rec.get("type") == 2:                                   # a closing order: trades in full, or not at all
                        if self.mode == "close_slow":                          # one lot at a time, a second after it was sent
                            time.sleep(1.0)
                            self.reply(rec, "fill", qty=1, leaves=rec["qty"] - 1)
                            self.reply(rec, "canceled", qty=rec["qty"] - 1, leaves=0, reason="ioc")
                        elif self.mode == "close_ok":
                            self.reply(rec, "fill", leaves=0)
                        else:
                            self.reply(rec, "canceled", leaves=0, reason="ioc")
                    elif not self.filled_one:                                  # the first quote that rests trades: a position
                        self.filled_one = True
                        self.reply(rec, "fill", leaves=0)
                elif t == "new":
                    self.reply(rec, "accepted", seg=rec["seg"] + (50 if self.mode == "wrongseg" else 0))
                elif t == "rep":
                    self.reply(rec, "replaced")
                elif t == "can":
                    self.reply(rec, "canceled", leaves=0, reason="user")
        if self.mode != "keepopen":
            try:
                os.close(self.reports_w)
            except OSError:
                pass


def run_external(mode, feed_text, flatten_s="2", close_exchange_early=False, pace=0.02, extra=()):
    o_r, o_w = os.pipe()          # hft_live -> exchange
    r_r, r_w = os.pipe()          # exchange -> hft_live
    proc = subprocess.Popen([BIN, "--report-s", "1000", "--stale-ms", "5000", "--orders-fd", str(o_w), "--exchange-fd", str(r_r),
                             "--flatten-timeout-s", flatten_s] + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, pass_fds=(o_w, r_r))
    os.close(o_w); os.close(r_r)
    ex = ScriptedExchange(o_r, r_w, mode)
    for chunk in feed_text:
        proc.stdin.write(chunk); proc.stdin.flush()
        time.sleep(pace)
    if close_exchange_early:
        try:
            os.close(r_w)
        except OSError:
            pass
        time.sleep(0.3)
    proc.stdin.close()
    out, err = proc.communicate(timeout=60)
    ex.th.join(timeout=10)
    return proc.returncode, out, ex


def ext_feed(n=60):
    chunks = [snap(1)]
    for i in range(n):
        chunks.append(upd(2 + i, "buy", "100.05", "5" if i % 2 == 0 else "0"))
    return chunks


def summary_numbers(out):
    for l in out.splitlines():
        if l.startswith("  exchange:"):
            nums = [int(x) for x in re.findall(r"(\d+) ", l)]
            return nums
    return None


import re
rc, out, ex = run_external("ok", ext_feed(), pace=0.03)
kinds = [r["t"] for r in ex.seen]
check(rc == 0, "external: scripted exchange, input ends -> exit 0 (rc=%s)" % rc)
check(kinds.count("new") >= 2 and kinds.count("rep") >= 2 and kinds.count("can") >= 2, "external: the strategy traded against it: %s" % {k: kinds.count(k) for k in set(kinds)})
nm = summary_numbers(out)
check(nm and nm[0] == len(ex.sent) and nm[2] == 0, "external: every report was applied (%s of %d), none unparsable" % (nm and nm[0], len(ex.sent)))
check("flat" in out and "NOT FLAT" not in out, "external: segment ended flat")
newids = {r["id"] for r in ex.seen if r["t"] == "new"}; canids = {r["id"] for r in ex.seen if r["t"] == "can"}
check(newids and newids <= canids, "external: every order was cancelled before the end")

rc, out, ex = run_external("garbage", ext_feed(30), pace=0.03)
nm = summary_numbers(out)
check(rc == 0 and nm and ex.garbage_events >= 3 and nm[2] == 3 * ex.garbage_events,
      "external: unparsable report lines (not JSON, missing fields, negative size) are counted and ignored (%s, %d bursts)" % (nm, ex.garbage_events))

rc, out, ex = run_external("wrongseg", ext_feed(10), flatten_s="0.5", pace=0.03)
nm = summary_numbers(out)
check(nm and nm[1] >= 2 and nm[0] == 0, "external: reports for another segment are not applied (%s)" % (nm,))
check(rc == 4 and "NOT FLAT" in out, "external: and so the segment cannot be flat: exit 4 (rc=%s)" % rc)

t0 = time.time()
rc, out, ex = run_external("silent", ext_feed(10), flatten_s="0.5", pace=0.03)
check(rc == 4 and "NOT FLAT" in out and time.time() - t0 >= 0.5, "external: an exchange that never answers -> flatten times out, NOT FLAT, exit 4 (rc=%s)" % rc)

logp3 = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("keepopen", ext_feed(10), flatten_s="0.5", close_exchange_early=True, pace=0.03, extra=["--log", logp3])
names = [json.loads(l)["event"] for l in open(logp3)]
check(rc == 6 and "exchange_lost" in names, "external: exchange pipe closed -> exchange_lost, exit 6 (restartable) (rc=%s)" % rc)
os.remove(logp3)

# 12. closing the position at the end: --close-position
for mode, flag, want_rc, closed in (("close_ok", True, 0, True), ("close_ok", False, 0, False), ("close_none", True, 4, False)):
    lp = tempfile.mktemp(suffix=".log")
    rc, out, ex = run_external(mode, ext_feed(30), flatten_s="3", pace=0.03, extra=["--log", lp] + (["--close-position"] if flag else []))
    rows = [json.loads(l) for l in open(lp)]
    os.remove(lp)
    iocs = [r for r in ex.seen if r["t"] == "new" and r.get("type") == 2]
    fills = [r for r in ex.sent if r["rt"] == "fill"]
    tag = "close: %s%s" % (mode, " with --close-position" if flag else " without it")
    check(len(fills) >= 1, tag + ": the strategy got a position (%d fills)" % len(fills))
    check(rc == want_rc, tag + ": exit %d (rc=%s)\n%s" % (want_rc, rc, out[-600:]))
    seg_end = [r for r in rows if r["event"] == "segment_end"]
    if not flag:
        check(not iocs, tag + ": no closing order is ever sent")
        check(seg_end and seg_end[-1].get("close_attempts", 0) == 0, tag + ": no attempt recorded")
        continue
    check(iocs and all(r["side"] == "sell" for r in iocs), tag + ": a sell through the bid: %s" % [(r["side"], r["px"], r["qty"]) for r in iocs[:2]])
    check(iocs and all(r["px"] % 100 == 0 and 0 < r["px"] < 1000_000 for r in iocs), tag + ": on the tick grid, below the touch")
    if closed:
        check(len(iocs) == 1 and iocs[0]["qty"] == fills[0]["qty"], tag + ": one order for the whole position")
        check(seg_end and seg_end[-1]["close_attempts"] == 1 and seg_end[-1]["closed_lots"] == fills[0]["qty"] and not seg_end[-1]["close_failed"], tag + ": the segment says so: %s" % seg_end[-1:])
        check("POSITION NOT CLOSED" not in out and not [r for r in rows if r["event"] == "position_not_closed"], tag + ": no alarm")
    else:
        check(len(iocs) == 5, tag + ": five attempts, then it gives up (%d)" % len(iocs))
        check(seg_end and seg_end[-1]["close_failed"] and seg_end[-1]["close_why_not"] == "attempts used up" and seg_end[-1]["closed_lots"] == 0, tag + ": the segment says why: %s" % seg_end[-1:])
        check("POSITION NOT CLOSED" in out and [r for r in rows if r["event"] == "position_not_closed"], tag + ": loud, in the output and the log")

# five closing orders of one second each outlast the flatten timeout (1.5 s); the session must wait for all of them
lp = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("close_slow", ext_feed(30), flatten_s="1.5", pace=0.03, extra=["--log", lp, "--close-position"])
rows = [json.loads(l) for l in open(lp)]
os.remove(lp)
iocs = [r for r in ex.seen if r["t"] == "new" and r.get("type") == 2]
seg_end = [r for r in rows if r["event"] == "segment_end"]
check(rc == 0 and len(iocs) == 5, "close: five one-lot fills, each a second apart, are waited for (rc=%s, %d orders)\n%s" % (rc, len(iocs), out[-500:]))
check(seg_end and seg_end[-1]["closed_lots"] == 5 and seg_end[-1]["close_attempts"] == 5 and not seg_end[-1]["close_failed"], "close: and the segment says 5 lots in 5 attempts: %s" % seg_end[-1:])

for extra, what in ((["--close-position"], "without --exchange-fd"), (["--close-position", "--exchange-fd", "3", "--close-collar-bps", "0"], "collar 0"),
                    (["--close-position", "--exchange-fd", "3", "--close-collar-bps", "1001"], "collar 1001")):
    rr, ww = os.pipe()
    p_ = subprocess.run([BIN, "--orders-fd", "4"] + extra, capture_output=True, pass_fds=(rr, ww))
    check(p_.returncode == 1, "close: %s: exit 1 (got %d)" % (what, p_.returncode))
    os.close(rr); os.close(ww)

for args, what in (([BIN, "--exchange-fd", "3"], "without --orders-fd"), ([BIN, "--from", "/dev/null", "--orders-fd", "3", "--exchange-fd", "4"], "with --from")):
    rr, ww = os.pipe()
    p_ = subprocess.run(args, capture_output=True, pass_fds=(rr, ww))
    check(p_.returncode == 1, "external: --exchange-fd %s: exit 1" % what)
    os.close(rr); os.close(ww)

if failures:
    sys.exit("%d live tool test failures" % failures)
print("test_live_tool: all checks passed")
