#!/usr/bin/env python3
"""Tests the hft_live program as a process: files, pipes, signals, real time.
Usage: test_live_tool.py path/to/hft_live. All data is invented; no network."""
import difflib, os, re, signal, subprocess, sys, tempfile, time

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


NO_LIQ = False         # set to make the fake exchange leave the liq field out of its fill reports


class ScriptedExchange:
    """reads order lines, answers each with a report line, optionally with a defect"""
    def __init__(self, orders_r, reports_w, mode="ok", base=1_000_000, acct="ok", stale_fill=False):
        self.stale_fill = stale_fill           # one fill report for a segment that does not exist, before the first order is answered
        self.orders_r, self.reports_w, self.mode = orders_r, reports_w, mode
        self.base, self.acct = base, acct      # the account's base balance in lots; what the bridge says about it ("ok", "fail", "none")
        self.seen, self.sent = [], []
        self.garbage_events = 0
        self.filled_one = False
        self.th = threading.Thread(target=self.run)
        self.th.start()

    def reply(self, rec, rt, **kw):
        out = {"t": "rep", "seg": rec["seg"], "rt": rt, "id": rec["id"], "px": rec.get("px", 0), "qty": rec.get("qty", 5),
               "leaves": kw.pop("leaves", rec.get("qty", 5)), "reason": kw.pop("reason", "none"), "side": rec.get("side", "buy")}
        if rt == "fill" and NO_LIQ == "weird":
            out["liq"] = "both"
        elif rt == "fill" and not NO_LIQ:
            out["liq"] = "taker" if rec.get("type") == 2 else "maker"
        out.update(kw)
        if out.get("liq") is None:
            out.pop("liq", None)
        line = json.dumps(out) + "\n"
        self.sent.append(out)
        if rt == "fill":
            self.base += out["qty"] if out["side"] == "buy" else -out["qty"]
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
                if t == "hello" and self.acct == "slow":
                    time.sleep(1.0)                                    # the feed is already waiting when the report comes
                if t == "hello" and self.acct != "none":
                    msg = {"t": "acct", "ok": 1, "base_raw": self.base} if self.acct in ("ok", "slow", "twice") else {"t": "acct", "ok": 0}
                    os.write(self.reports_w, (json.dumps(msg) + "\n").encode())
                    if self.acct == "twice":                             # a second report (a bridge that saw a second hello) must not matter
                        os.write(self.reports_w, (json.dumps({"t": "acct", "ok": 1, "base_raw": 7}) + "\n").encode())
                if self.mode == "silent" or t in ("hello", "end"):
                    continue
                if self.mode == "garbage" and len(self.seen) % 3 == 0:
                    self.garbage_events += 1
                    os.write(self.reports_w, b"this is not json\n{\"t\":\"rep\",\"seg\":1}\n"
                             b"{\"t\":\"rep\",\"seg\":1,\"rt\":\"accepted\",\"id\":1,\"px\":5,\"qty\":-5,\"leaves\":5}\n")
                if t == "new" and self.stale_fill:
                    self.stale_fill = False
                    self.reply(rec, "fill", seg=rec["seg"] + 50, qty=3, leaves=0)
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


def run_external(mode, feed_text, flatten_s="2", close_exchange_early=False, pace=0.02, extra=(), base=1_000_000, acct="ok", stale_fill=False):
    o_r, o_w = os.pipe()          # hft_live -> exchange
    r_r, r_w = os.pipe()          # exchange -> hft_live
    proc = subprocess.Popen([BIN, "--report-s", "1000", "--stale-ms", "5000", "--orders-fd", str(o_w), "--exchange-fd", str(r_r),
                             "--flatten-timeout-s", flatten_s] + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, pass_fds=(o_w, r_r))
    os.close(o_w); os.close(r_r)
    ex = ScriptedExchange(o_r, r_w, mode, base, acct, stale_fill)
    try:
        for chunk in feed_text:
            proc.stdin.write(chunk); proc.stdin.flush()
            time.sleep(pace)
    except BrokenPipeError:   # hft_live refused to start (the account check) and closed its input
        pass
    if close_exchange_early:
        try:
            os.close(r_w)
        except OSError:
            pass
        time.sleep(0.3)
    try:
        proc.stdin.close()
    except BrokenPipeError:
        pass
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

# 13. positions across runs: --position-file (ADR 30)
pf = tempfile.mktemp(suffix=".pos")
lp = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("close_none", ext_feed(30), flatten_s="1", pace=0.03, extra=["--log", lp, "--close-position", "--position-file", pf])
check(rc == 4 and "POSITION NOT CLOSED" in out, "posfile: an unclosed position is exit 4 (rc=%s)\n%s" % (rc, out[-400:]))
lots, cash, baseline = (int(x) for x in open(pf).read().split())
check(baseline == 1_000_000, "posfile: the first run records the account's balance as the baseline (%d)" % baseline)
base_after = ex.base
check(base_after == baseline + lots, "posfile: and the account moved with the position (%d = %d + %d)" % (base_after, baseline, lots))
check(lots > 0 and cash < 0, "posfile: the file holds the position left at the exchange: %d lots, cash %d" % (lots, cash))
check(not os.path.exists(pf + ".tmp"), "posfile: no temporary file is left behind")

# a new process with nothing to trade on: it knows the position from the file, cannot price a close, and says so
rows_before = open(lp).read()
rc, out, ex = run_external("ok", [], extra=["--log", lp, "--close-position", "--position-file", pf], base=base_after)
check(rc == 4 and "POSITION NOT CLOSED: %d lots left (no live segment to price a close)" % lots in out, "posfile: no feed, a carried position: exit 4, reason named (rc=%s)\n%s" % (rc, out[-400:]))
check(int(open(pf).read().split()[0]) == lots, "posfile: and the file still holds it")

# a new process with a feed closes the position it was handed, in one order for the whole of it
rc, out, ex = run_external("close_ok", ext_feed(30), flatten_s="2", pace=0.03, extra=["--close-position", "--position-file", pf], base=base_after + 1)   # one lot off is tolerated
iocs = [r for r in ex.seen if r["t"] == "new" and r.get("type") == 2]
check(rc == 0 and len(iocs) == 1, "posfile: the carried position is closed (rc=%s, %d closing orders)\n%s" % (rc, len(iocs), out[-500:]))
check(iocs and iocs[0]["qty"] >= lots, "posfile: the closing order covers at least the carried %d lots (%s)" % (lots, iocs[:1]))
check(int(open(pf).read().split()[0]) == 0, "posfile: flat again, and the file says 0")
fills = [r for r in ex.sent if r["rt"] == "fill"]
want_cash = cash + sum(-(1 if r["side"] == "buy" else -1) * r["px"] * r["qty"] for r in fills)
check(int(open(pf).read().split()[1]) == want_cash, "posfile: the cash started from the file's (%d) and moved by the fills: file %s, expected %d" % (cash, open(pf).read().split()[1], want_cash))
os.remove(pf); os.remove(lp)

lp = tempfile.mktemp(suffix=".log")
NO_LIQ = True
rc, out, ex = run_external("close_ok", ext_feed(30), flatten_s="2", pace=0.03, extra=["--log", lp, "--close-position"])
NO_LIQ = False
fl = [json.loads(l) for l in open(lp)]
fl = [r for r in fl if r["event"] == "fill"]
check(rc == 0 and fl and all("liq" not in f for f in fl), "fills: a report with no liquidity mark gives an event with none (nothing guessed): %d fills (rc=%s)" % (len(fl), rc))
os.remove(lp)

lp = tempfile.mktemp(suffix=".log")
NO_LIQ = "weird"
rc, out, ex = run_external("close_ok", ext_feed(30), flatten_s="2", pace=0.03, extra=["--log", lp, "--close-position"])
NO_LIQ = False
fl = [r for r in (json.loads(l) for l in open(lp)) if r["event"] == "fill"]
check(rc == 0 and fl and all("liq" not in f for f in fl), "fills: a mark that is neither maker nor taker is not copied: %d fills (rc=%s)" % (len(fl), rc))
os.remove(lp)

# ADR 31: the file is checked against the account before anything is traded
def acct_case(content, base, acct="ok", feed=None, extra=()):
    open(pf, "w").write(content)
    r = run_external("ok", feed if feed is not None else ext_feed(10), flatten_s="1", pace=0.03,
                     extra=["--close-position", "--position-file", pf] + list(extra), base=base, acct=acct)
    return r

rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_095, acct="slow")
check(rc == 4 and "ACCOUNT CHECK FAILED" in out and "1000095" in out, "account: a balance far from baseline + position is refused (rc=%s)\n%s" % (rc, out[-300:]))
check(not [r for r in ex.seen if r["t"] != "hello"], "account: and nothing was sent to the exchange although the feed was waiting: %s" % [r["t"] for r in ex.seen])
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_002)
check(rc == 4 and "ACCOUNT CHECK FAILED" in out, "account: three lots too few is refused (rc=%s)" % rc)
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_003)
check(rc == 4 and "ACCOUNT CHECK FAILED" in out, "account: two lots too few is refused (rc=%s)" % rc)
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_005, acct="twice")
check("ACCOUNT CHECK FAILED" not in out and [r for r in ex.seen if r["t"] == "new"], "account: a second report is ignored once the first was looked at (rc=%s)" % rc)
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_006)
check("ACCOUNT CHECK FAILED" not in out, "account: one lot too many is tolerated (rc=%s)" % rc)
check(open(pf).read() == "5 -5000000 1000000\n", "account: and the file is untouched")
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_007)
check(rc == 4 and "ACCOUNT CHECK FAILED" in out, "account: two lots off is refused too (rc=%s)" % rc)
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_004)
check("ACCOUNT CHECK FAILED" not in out and [r for r in ex.seen if r["t"] == "new"], "account: one lot off is tolerated and trading starts (rc=%s)" % rc)
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_005, acct="fail")
check(rc == 4 and "ACCOUNT CHECK FAILED" in out and "could not be read" in out, "account: an unreadable balance is refused (rc=%s)" % rc)
t0 = time.time()
rc, out, ex = acct_case("5 -5000000 1000000\n", 1_000_005, acct="none")
check(rc == 4 and "ACCOUNT CHECK FAILED" in out and time.time() - t0 < 30, "account: no report within 10 s is refused (rc=%s, %.0f s)" % (rc, time.time() - t0))
rc, out, ex = acct_case("5 -5000000\n", 1_000_005, feed=[])
check("ACCOUNT CHECK FAILED" not in out and open(pf).read().split() == ["5", "-5000000", "1000000"],
      "account: a file from before the check gets its baseline from the first balance seen: %s" % open(pf).read().split())
open(pf, "w").write("5 -5000000 1000000\n")
rc, out, ex = run_external("ok", [], base=7)   # no --position-file: the report is not needed and nothing is checked
check(rc == 5 and "ACCOUNT CHECK FAILED" not in out, "account: without a position file nothing is checked (rc=%s)" % rc)
os.remove(pf)

# a file that cannot be written does not stop the run (the warning goes to stderr); the position is still reported
rc, out, ex = run_external("close_none", ext_feed(30), flatten_s="1", pace=0.03, extra=["--close-position", "--position-file", "/nonexistent-dir/hft.pos"])
check(rc == 4 and "POSITION NOT CLOSED" in out, "posfile: an unwritable file does not hide the position (rc=%s)" % rc)

# refusals: a file that is not a position, and no exchange to learn the truth from
open(pf, "w").write("hello\n")
rr, ww = os.pipe()
p_ = subprocess.run([BIN, "--orders-fd", str(ww), "--exchange-fd", str(rr), "--position-file", pf], capture_output=True, pass_fds=(rr, ww))
check(p_.returncode == 1 and b"refusing to start" in p_.stderr, "posfile: garbage in the file: exit 1, refuses (rc=%s)" % p_.returncode)
p_ = subprocess.run([BIN, "--orders-fd", str(ww), "--position-file", pf], capture_output=True, pass_fds=(rr, ww))
check(p_.returncode == 1, "posfile: without --exchange-fd: exit 1")
os.close(rr); os.close(ww); os.remove(pf)

# 14. fill events and mid samples for the PnL report (ADR 32)
import importlib.util
spec = importlib.util.spec_from_file_location("pnl_report", os.path.join(os.path.dirname(os.path.abspath(__file__)), "pnl_report.py"))
pnl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pnl)

lp = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("close_ok", ext_feed(40), flatten_s="2", pace=0.03, extra=["--log", lp, "--close-position", "--mid-sample-ms", "20"])
rows = [json.loads(l) for l in open(lp)]
fl = [r for r in rows if r["event"] == "fill"]
md = [r for r in rows if r["event"] == "mid"]
sent_fills = [r for r in ex.sent if r["rt"] == "fill"]
check(rc == 0 and len(fl) == len(sent_fills) and len(fl) >= 2, "fills: one event per fill report: %d events, %d reports (rc=%s)" % (len(fl), len(sent_fills), rc))
check(all(f["applied"] == 1 for f in fl), "fills: all applied by the session")
check([f.get("liq") for f in fl] == [r.get("liq") for r in sent_fills] and {f.get("liq") for f in fl} == {"maker", "taker"}, "fills: the liquidity mark is the report's own, the closing order is taker: %s" % [f.get("liq") for f in fl])
check([(f["side"], f["px"], f["qty"]) for f in fl] == [(r["side"], r["px"], r["qty"]) for r in sent_fills], "fills: side, price and quantity are the report's own: %s" % [(f["side"], f["px"], f["qty"]) for f in fl][:2])
want = 0
okpos = True
for f in fl:
    want += f["qty"] if f["side"] == "buy" else -f["qty"]
    okpos = okpos and f["pos"] == want
check(okpos and fl[-1]["pos"] == 0, "fills: the position after each fill is the running sum, and ends flat")
check(all(1_900_000 <= f["mid_x2"] <= 2_100_000 for f in fl), "fills: carry the book's mid (twice, in raw units): %s" % [f["mid_x2"] for f in fl][:3])
check(len(md) >= 2 and all(1_900_000 <= m["mid_x2"] <= 2_100_000 for m in md), "mid: samples while the book is two-sided (%d)" % len(md))
check(all(md[i]["mid_x2"] != md[i + 1]["mid_x2"] for i in range(len(md) - 1)), "mid: only changes are logged")
fills_, mids_, last_, bad_ = pnl.load([lp])
rep = pnl.analyse(fills_, mids_, last_, 10000, 100000, 0.0, [1.0])
check(rep["fills"] == len(fl) and rep["position_gaps"] == 0 and abs(rep["end_position_base"]) < 1e-12, "report: reads the log, no position gaps, ends flat")
check(abs(rep["cash_check_diff"]) < 1e-9, "report: the two PnL figures agree")
os.remove(lp)
lp = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("close_ok", ext_feed(30), flatten_s="2", pace=0.03, extra=["--log", lp, "--close-position"], stale_fill=True)
rows = [json.loads(l) for l in open(lp)]
st = [r for r in rows if r["event"] == "fill" and r["seg"] != 1]
check(len(st) == 1 and st[0]["applied"] == 0 and st[0]["qty"] == 3, "fills: a report for a segment that does not exist is logged as not applied: %s" % st)
check(len([r for r in rows if r["event"] == "fill" and r["applied"] == 1]) >= 2 and rc == 0, "fills: and it did not disturb the real ones (rc=%s)" % rc)
fills_, mids_, last_, bad_ = pnl.load([lp])
check(all(f["qty"] != 3 for f in fills_), "report: and the report ignores it")
os.remove(lp)
lp = tempfile.mktemp(suffix=".log")
rc, out, ex = run_external("close_ok", ext_feed(20), flatten_s="2", pace=0.03, extra=["--log", lp, "--close-position"])
rows = [json.loads(l) for l in open(lp)]
check([r for r in rows if r["event"] == "fill"] and not [r for r in rows if r["event"] == "mid"], "mid: no samples unless --mid-sample-ms is given; fills are logged regardless")
os.remove(lp)

# --clock-out / --clock-in: a live run, whose clock is the arrival time, reproduced exactly (ADR 49)
import random
def wait_idle(pid, quiet=0.15, limit=30):
    """returns when the process has used no CPU time for `quiet` seconds (it has worked through what it was given)"""
    def cpu():
        f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
        return int(f[11]) + int(f[12])
    last, since, t0 = cpu(), time.time(), time.time()
    while time.time() - t0 < limit:
        time.sleep(0.02)
        c = cpu()
        if c != last:
            last, since = c, time.time()
        elif time.time() - since >= quiet:
            return


def live_with_clock(text, how, seed):
    """feeds text to hft_live on stdin in bursts with small pauses; how: 'eof' (input ends) or 'term' (SIGTERM)"""
    rnd = random.Random(seed)
    stream = tempfile.mktemp(suffix=".l2"); clk = tempfile.mktemp(suffix=".clk")
    open(stream, "w").write(text)
    proc = subprocess.Popen([BIN, "--report-s", "1000", "--clock-out", clk], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    lines = text.splitlines(True)
    i = 0
    while i < len(lines):
        n = rnd.randint(1, 40)
        proc.stdin.write("".join(lines[i:i + n])); proc.stdin.flush()
        i += n
        time.sleep(rnd.choice([0, 0, 0.0003, 0.002]))
    wait_idle(proc.pid)  # quiet: hft_live polls its timers while nothing arrives
    if how == "term":
        proc.send_signal(signal.SIGTERM)
        out, err = proc.communicate(timeout=30)
    else:
        out, err = proc.communicate(timeout=30)  # communicate closes stdin: end of input
    return proc.returncode, out, stream, clk

text_ck = churn(300)
hashes = set()
for how, seed in (("eof", 1), ("eof", 2), ("eof", 3), ("term", 4)):
    rc, out, stream, clk = live_with_clock(text_ck, how, seed)
    ends = [l.split()[0] for l in open(clk)]
    rep = subprocess.run([BIN, "--from", stream, "--clock-in", clk, "--report-s", "1000"], capture_output=True, text=True, timeout=60)
    tag = "clock %s/%d" % (how, seed)
    check(rc == 0 and rep.returncode == 0, tag + ": both exit 0 (%s, %s)\n%s" % (rc, rep.returncode, rep.stderr))
    check(wire_hash(out) and wire_hash(out) == wire_hash(rep.stdout), tag + ": replay with the clock gives the live hash (%s vs %s)" % (wire_hash(out), wire_hash(rep.stdout)))
    orders_line = lambda o: [l.strip() for l in o.splitlines() if l.strip().startswith("orders:")]
    check(orders_line(out) == orders_line(rep.stdout) and orders_line(out), tag + ": and the same orders line, the kill sweep included: %s vs %s" % (orders_line(out), orders_line(rep.stdout)))
    check(ends[-1] == ("K" if how == "term" else "S") and ends.count("L") == len(text_ck.splitlines()), tag + ": clock file: one L per line, ends with %s (%s, %d L of %d lines)" % ("K" if how == "term" else "S", ends[-1], ends.count("L"), len(text_ck.splitlines())))
    check("P" in ends, tag + ": polls are recorded")
    check(re.search(r"orders: [1-9]\d* new", out) is not None and re.search(r"orders: \d+ new, [1-9]\d* replace", out) is not None, tag + ": a real order flow")
    if how == "term":
        check("by kill" in out and re.search(r"\(\d+ by pulls, [1-9]\d* by kill\)", out), tag + ": the live run had a kill sweep")
    hashes.add(wire_hash(out))
    os.remove(stream); os.remove(clk)
# refusals
rfd, wfd = os.pipe()
for args, what, word in ((["--clock-in", "/dev/null"], "--clock-in without --from", "--clock-in needs --from"),
                         (["--clock-out", "/tmp/x.clk", "--from", "/dev/null"], "--clock-out with --from", "--clock-out records"),
                         (["--clock-out", "/tmp/x.clk", "--position-file", "/tmp/x.pos"], "--clock-out with --position-file", "--clock-out records"),
                         (["--clock-out", "/nonexistent-dir/x.clk"], "unwritable --clock-out", "cannot write")):
    p_ = subprocess.run([BIN] + args, capture_output=True, text=True, timeout=20, stdin=subprocess.DEVNULL, pass_fds=(rfd,))
    check(p_.returncode == 1 and word in p_.stderr, "clock: %s: exit 1 with the reason (rc=%d) %s" % (what, p_.returncode, p_.stderr[:80]))
os.close(rfd); os.close(wfd)
# the clock of a killed process has no final line: the replay still runs to the end of what was recorded
rc, out, stream, clk = live_with_clock(text_ck, "eof", 9)
full = open(clk).read().splitlines(True)
open(clk, "w").write("".join(full[:-1]))
rep = subprocess.run([BIN, "--from", stream, "--clock-in", clk, "--report-s", "1000"], capture_output=True, text=True, timeout=60)
check(rep.returncode == 0 and wire_hash(out) == wire_hash(rep.stdout), "clock: no final line: replay still gives the same hash (%s vs %s)" % (wire_hash(out), wire_hash(rep.stdout)))
os.remove(stream); os.remove(clk)
# a clock file without the "H" line (written before there was one) is a run without an exchange
rc, out, stream, clk = live_with_clock(text_ck, "eof", 11)
lines_ = open(clk).read().splitlines(True)
check(lines_[0] == "H 0\n", "clock: a plain live run starts with H 0 (%r)" % lines_[0])
open(clk, "w").write("".join(lines_[1:]))
rep = subprocess.run([BIN, "--from", stream, "--clock-in", clk, "--report-s", "1000"], capture_output=True, text=True, timeout=60)
check(rep.returncode == 0 and wire_hash(out) == wire_hash(rep.stdout), "clock: a file without the H line still replays (%s vs %s)" % (wire_hash(out), wire_hash(rep.stdout)))
os.remove(stream); os.remove(clk)
# a clock file with more lines than the stream
sp = tempfile.mktemp(suffix=".l2"); cp = tempfile.mktemp(suffix=".clk")
open(sp, "w").write(snap(1_000_000)); open(cp, "w").write("L 1000000\nL 1000100\nS 1000200\n")
p_ = subprocess.run([BIN, "--from", sp, "--clock-in", cp], capture_output=True, text=True, timeout=20)
check(p_.returncode == 1 and "more lines" in p_.stderr, "clock: a clock file longer than the stream is refused (rc=%d)" % p_.returncode)
os.remove(sp); os.remove(cp)

# the exchange's reports are in the clock file too: a run with an exchange behind it replays exactly (ADR 50)
def ext_clock_case(mode, flatten_s, extra=(), feed=None, pace=0.03):
    clk = tempfile.mktemp(suffix=".clk")
    feed = feed or ext_feed(30)
    rc, out, ex = run_external(mode, feed, flatten_s=flatten_s, pace=pace, extra=["--clock-out", clk] + list(extra))
    stream = tempfile.mktemp(suffix=".l2"); open(stream, "w").write("".join(feed))
    rep = subprocess.run([BIN, "--from", stream, "--clock-in", clk, "--report-s", "1000", "--stale-ms", "5000", "--flatten-timeout-s", flatten_s] + list(extra),
                         capture_output=True, text=True, timeout=60)
    rows = open(clk).read().splitlines()
    os.remove(stream); os.remove(clk)
    return rc, out, ex, rep, rows

# report lines that do not parse are counted but change nothing, so they are not recorded: that count is the one difference
summ = lambda o: [re.sub(r"\d+ unparsable;", "N unparsable;", l) for l in o.splitlines() if "wall-clock" not in l and "per update" not in l and "segment start" not in l and "per message" not in l]
for mode, flat, extra, want_rc in (("ok", "2", [], 0), ("close_ok", "3", ["--close-position"], 0), ("close_none", "3", ["--close-position"], 4),
                                   ("silent", "0.5", [], 4), ("garbage", "2", [], 0), ("wrongseg", "0.5", [], 4)):
    rc, out, ex, rep, rows = ext_clock_case(mode, flat, extra)
    tag = "clock exchange/%s" % mode
    check(rc == want_rc, tag + ": live exit %d (rc=%s)" % (want_rc, rc))
    check(rep.returncode == rc, tag + ": the replay exits the same way (%s vs %s)\n%s" % (rep.returncode, rc, rep.stderr[-300:]))
    check(wire_hash(out) and wire_hash(out) == wire_hash(rep.stdout), tag + ": same wire hash (%s vs %s)" % (wire_hash(out), wire_hash(rep.stdout)))
    check(summ(out) == summ(rep.stdout), tag + ": and the same summary\n" + "\n".join(a for a in difflib.unified_diff(summ(out), summ(rep.stdout), lineterm="", n=0)))
    check(rows[0] == "H 1", tag + ": the clock file says there was an exchange (%s)" % rows[0])
    if mode not in ("silent",):
        check(any(r.startswith("R ") for r in rows), tag + ": reports are recorded")
    else:
        check(not any(r.startswith("R ") for r in rows), tag + ": a silent exchange leaves no reports in the file")

if failures:
    sys.exit("%d live tool test failures" % failures)
print("test_live_tool: all checks passed")
