#!/usr/bin/env python3
"""Fault drills for the live pair, run against a local fake feed (no network, invented prices).

  drill.py [--live build/release/hft_live] [only-this-drill ...]

Each drill starts its own supervisor (scripts/supervise.py) with the real
capture_l2.py and the real hft_live, injects one fault, and checks what the logs
and exit codes say happened. The paper venue is inside hft_live, so these drills
show that the process pair recovers and that quotes are pulled; they cannot show
what a real exchange would do with orders left working after a SIGKILL.
"""
import asyncio, json, os, random, shutil, signal, subprocess, sys, tempfile, time

os.environ["NO_PROXY"] = "127.0.0.1,localhost"
os.environ["no_proxy"] = "127.0.0.1,localhost"
HERE = os.path.dirname(os.path.abspath(__file__))
import websockets
sys.path.insert(0, HERE)
import fake_exchange

LIVE = os.path.join(os.path.dirname(HERE), "build/release/hft_live")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("  FAIL", what, flush=True)
    else:
        print("  ok  ", what, flush=True)


class Feed:
    """A fake exchange feed. Faults are switched from the drill."""
    def __init__(self):
        self.rng = random.Random(7)
        self.drop = False        # close every open connection
        self.silent_until = 0.0  # send nothing (heartbeats included) until then, connection stays open
        self.connections = 0

    def px(self, cents):
        return "%d.%02d" % (cents // 100, cents % 100)

    def snapshot(self):
        bids = ",".join('["%s","%d"]' % (self.px(10000 - i), 2 + i) for i in range(8))
        asks = ",".join('["%s","%d"]' % (self.px(10010 + i), 2 + i) for i in range(8))
        return '{"type":"snapshot","product_id":"BTC-USD","bids":[%s],"asks":[%s]}' % (bids, asks)

    def update(self):
        i = self.rng.randrange(8)
        if self.rng.random() < 0.5:
            c = '["buy","%s","%d"]' % (self.px(10000 - i), 1 + self.rng.randrange(9))
        else:
            c = '["sell","%s","%d"]' % (self.px(10010 + i), 1 + self.rng.randrange(9))
        return '{"type":"l2update","product_id":"BTC-USD","changes":[%s],"time":"t"}' % c

    async def handler(self, ws):
        self.connections += 1
        try:
            await ws.recv()
            await ws.send(self.snapshot())
            last_beat = time.monotonic()
            while True:
                if self.drop:
                    await ws.close(code=1011)
                    return
                now = time.monotonic()
                if now >= self.silent_until:
                    await ws.send(self.update())
                    if now - last_beat >= 1.0:
                        await ws.send('{"type":"heartbeat","sequence":1}')
                        last_beat = now
                await asyncio.sleep(0.05)
        except Exception:
            return


def read_jsonl(path):
    out = []
    try:
        for line in open(path):
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    return out


async def until(pred, timeout, step=0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        await asyncio.sleep(step)
    return pred()


class Run:
    def __init__(self, feed, port, extra=(), live_cmd=None, capture_cmd=None, env=None):
        self.dir = tempfile.mkdtemp(prefix="drill-")
        self.sup_log = os.path.join(self.dir, "sup.jsonl")
        self.events = os.path.join(self.dir, "events.jsonl")
        cap = capture_cmd or ("%s %s --url ws://127.0.0.1:%d --out - --seconds 600 --idle-timeout 2 --resnap-file {resnap}" %
                              (sys.executable, os.path.join(HERE, "capture_l2.py"), port))
        cmd = [sys.executable, os.path.join(HERE, "supervise.py"), "--run-dir", self.dir, "--log", self.sup_log,
               "--live", LIVE, "--stale-ms", "600", "--metrics-s", "0.3", "--capture-cmd", cap,
               "--backoff-s", "0.2", "--watchdog-s", "3", "--startup-grace-s", "2", "--term-grace-s", "5"]
        if live_cmd:
            cmd += ["--live-cmd", live_cmd]
        cmd += list(extra)
        e = dict(os.environ)
        e.update(env or {})
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=e)

    def sup(self, name=None):
        ev = read_jsonl(self.sup_log)
        return [e for e in ev if name is None or e["event"] == name]

    def bridge_events(self, name=None):
        ev = read_jsonl(os.path.join(self.dir, "bridge.jsonl"))
        return [e for e in ev if name is None or e["event"] == name]

    def live_events(self, name=None):
        ev = read_jsonl(self.events)
        return [e for e in ev if name is None or e["event"] == name]

    def pid(self, which, run):
        for e in self.sup("pair_start"):
            if e["run"] == run:
                return e[which + "_pid"]

    async def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                await asyncio.get_event_loop().run_in_executor(None, lambda: self.p.wait(20))
            except subprocess.TimeoutExpired:
                self.p.kill()
        return self.p.returncode

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


async def quoting(run, n_segments=1, timeout=15):
    """true once n segments have started and orders were placed"""
    return await until(lambda: len(run.live_events("segment_start")) >= n_segments, timeout)


async def drill_drop(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    feed.drop = True
    await asyncio.sleep(0.4)
    feed.drop = False
    ok = await until(lambda: len(r.live_events("segment_start")) >= 2, 15)
    check(ok, "a second segment starts after the connection drop")
    ends = r.live_events("segment_end")
    check(ends and ends[0]["reason"] == "reconnect/marker" and ends[0]["flat"] == 1, "the first segment ended flat on the reconnect marker: %s" % (ends[:1],))
    check(ends and ends[0]["open_before_flatten"] > 0, "and there were quotes out to take down")
    check(len(r.sup("pair_start")) == 1, "the supervisor did not need to restart anything")
    check(await r.stop() == 0, "supervisor exits 0 on SIGTERM")
    r.cleanup()


async def drill_silent(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    feed.silent_until = time.monotonic() + 4.0
    t0 = time.monotonic()
    ok = await until(lambda: r.live_events("stale_feed"), 4)
    check(ok, "stale feed detected while the connection is still open (%.1f s after the silence began)" % (time.monotonic() - t0))
    ends = r.live_events("segment_end")
    check(ends and ends[0]["reason"] == "stale feed" and ends[0]["flat"] == 1 and ends[0]["kill_latched"] == 0,
          "quotes pulled and flat, kill switch not latched: %s" % (ends[:1],))
    check(ends and ends[0]["open_before_flatten"] > 0, "and there were quotes out to take down")
    ok = await until(lambda: len(r.live_events("segment_start")) >= 2, 15)
    check(ok, "quoting resumes once the feed is back")
    check(len(r.sup("pair_start")) == 1, "no process restart was needed")
    check(await r.stop() == 0, "supervisor exits 0")
    r.cleanup()


async def drill_kill_capture(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    os.kill(r.pid("capture", 1), signal.SIGKILL)
    ok = await until(lambda: len(r.sup("pair_start")) >= 2, 15)
    check(ok, "the pair is restarted after kill -9 of the capture")
    ends = r.live_events("segment_end")
    check(ends and ends[0]["reason"] == "shutdown" and ends[0]["flat"] == 1, "hft_live saw end of input and ended flat: %s" % (ends[:1],))
    check(ends and ends[0]["open_before_flatten"] > 0, "and there were quotes out to take down")
    check(await until(lambda: len(r.live_events("segment_start")) >= 2, 15), "quoting again in the new run")
    stopped = r.sup("pair_stopped")
    check(stopped and stopped[0]["live"]["returncode"] == 0, "hft_live exited 0 (clean) when its input vanished")
    check(await r.stop() == 0, "supervisor exits 0")
    r.cleanup()


async def drill_kill_live(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    old_cap = r.pid("capture", 1)
    os.kill(r.pid("live", 1), signal.SIGKILL)
    ok = await until(lambda: len(r.sup("pair_start")) >= 2, 15)
    check(ok, "the pair is restarted after kill -9 of hft_live")
    stopped = r.sup("pair_stopped")
    check(stopped and stopped[0]["live"]["signal"] == 9, "recorded as killed by signal 9: %s" % (stopped[:1],))
    check(await until(lambda: len(r.live_events("segment_start")) >= 2, 15), "quoting again in the new run")
    check(await until(lambda: len(r.sup("pair_start")) == 2 and len(r.sup("pair_stopped")) == 1, 3),
          "exactly one restart")
    def gone(pid):
        try:
            os.kill(pid, 0)
            return False
        except ProcessLookupError:
            return True
    check(await until(lambda: gone(old_cap), 5), "the old capture process did not outlive its reader")
    check(await r.stop() == 0, "supervisor exits 0")
    r.cleanup()


async def drill_hang(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    pid = r.pid("live", 1)
    os.kill(pid, signal.SIGSTOP)  # alive but doing nothing: no crash, no exit
    t0 = time.monotonic()
    ok = await until(lambda: r.sup("watchdog"), 15)
    check(ok, "the watchdog notices the stopped process (%.1f s)" % (time.monotonic() - t0))
    check(await until(lambda: len(r.sup("pair_start")) >= 2, 15), "the pair is restarted")
    check(await until(lambda: len(r.live_events("segment_start")) >= 2, 15), "quoting again in the new run")
    try:
        os.kill(pid, 0)
        check(False, "the stopped process was killed, not left behind")
    except ProcessLookupError:
        check(True, "the stopped process was killed, not left behind")
    check(await r.stop() == 0, "supervisor exits 0")
    r.cleanup()


async def drill_sigterm(feed, port):
    r = Run(feed, port)
    check(await quoting(r), "first segment starts")
    await asyncio.sleep(1.0)
    rc = await r.stop()
    check(rc == 0, "supervisor exits 0")
    ends = r.live_events("segment_end")
    check(ends and ends[-1]["reason"] == "kill" and ends[-1]["flat"] == 1 and ends[-1]["kill_latched"] == 1,
          "SIGTERM reached hft_live as a kill: flat, switch latched: %s" % (ends[-1:],))
    check(ends and ends[-1]["open_before_flatten"] > 0, "and there were quotes out to take down")
    ex = r.live_events("exit")
    check(ex and ex[-1]["cause"] == "signal", "hft_live recorded the signal")
    check(len(r.sup("pair_start")) == 1 and not r.sup("restart_in"), "and was not restarted")
    r.cleanup()


async def drill_term_ignored(feed, port):
    # a hft_live that ignores SIGTERM (stuck in its handler, say): the supervisor must not wait for ever
    r = Run(feed, port, extra=["--term-grace-s", "1"], live_cmd="sh -c 'trap \"\" TERM; while true; do sleep 1; done'",
            capture_cmd="sleep 60")
    await until(lambda: r.sup("pair_start"), 5)
    t0 = time.monotonic()
    rc = await r.stop()
    check(rc == 0, "supervisor still exits 0")
    check(r.sup("live_ignored_sigterm"), "recorded that SIGTERM was ignored")
    check(time.monotonic() - t0 < 8, "and gave up waiting after the grace period (%.1f s)" % (time.monotonic() - t0))
    stopped = r.sup("pair_stopped")
    check(stopped and stopped[-1]["live"]["signal"] == 9, "the stuck process was killed with SIGKILL")
    r.cleanup()


async def drill_capture_dies_live_stuck(feed, port):
    # the capture exits but hft_live does not notice (it should see end of input): do not wait for ever
    r = Run(feed, port, live_cmd="sh -c 'while true; do sleep 1; done'", capture_cmd="sh -c 'exit 0'")
    ok = await until(lambda: r.sup("live_did_not_follow_capture"), 12)
    check(r.sup("capture_exited") and ok, "notices the capture is gone and that hft_live did not follow")
    check(await until(lambda: len(r.sup("pair_start")) >= 2, 5), "and restarts the pair")
    await r.stop()
    r.cleanup()


async def drill_slow_flapping(feed, port):
    # failures spread out over time are not a crash loop: the window must forget old restarts
    r = Run(feed, port, extra=["--max-restarts", "2", "--window-s", "0.5"], live_cmd="sh -c 'exit 1'", capture_cmd="sleep 30")
    await asyncio.sleep(5.0)
    check(r.p.poll() is None and not r.sup("giving_up"), "still supervising after 5 s of spaced-out failures")
    check(len(r.sup("pair_start")) >= 4, "restarted %d times, more than max-restarts, because they were spread out" % len(r.sup("pair_start")))
    await r.stop()
    r.cleanup()


async def drill_crash_loop(feed, port):
    r = Run(feed, port, extra=["--max-restarts", "3", "--window-s", "60"], live_cmd="sh -c 'exit 1'",
            capture_cmd="sleep 30")
    rc = await asyncio.get_event_loop().run_in_executor(None, lambda: r.p.wait(30))
    check(rc == 3, "gives up with exit 3 (got %s)" % rc)
    check(len(r.sup("pair_start")) == 4 and r.sup("giving_up"), "after 3 restarts: %d starts" % len(r.sup("pair_start")))
    delays = [e["seconds"] for e in r.sup("restart_in")]
    check(delays == sorted(delays) and delays[-1] > delays[0], "backoff grows: %s" % delays)
    r.cleanup()


async def drill_integrity(feed, port):
    r = Run(feed, port, live_cmd="sh -c 'exit 4'", capture_cmd="sleep 30")
    rc = await asyncio.get_event_loop().run_in_executor(None, lambda: r.p.wait(30))
    check(rc == 4, "exit 4 passed on to the operator (got %s)" % rc)
    check(len(r.sup("pair_start")) == 1 and r.sup("fatal_halt"), "not restarted: a person must look")
    r.cleanup()


# ---- order bridge drills: the sandbox REST API is played by scripts/fake_exchange.py -------------------------------
# These show what the processes do around a fake exchange. They do not show what the real sandbox does.
KEY, PASS = "drill-key", "drill-pass"
SECRET = __import__("base64").b64encode(b"drill-secret-bytes-0123456789abcdef").decode()


def bridge_run(feed, port, ex, **kw):
    env = {"HFT_CB_KEY": KEY, "HFT_CB_SECRET": SECRET, "HFT_CB_PASSPHRASE": PASS}
    env.update(kw.pop("env", {}))
    args = "--test-exchange --base-url %s --rate 20 --max-notional 100000 --max-errors 3 --sync-s 1" % ex.url
    return Run(feed, port, extra=["--bridge", "--bridge-args", args] + kw.pop("extra", []), env=env, **kw)


def new_exchange():
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start()
    ex.best_bid, ex.best_ask = 0.0, 1e12  # the invented feed's prices are unrelated to the exchange's: never cross
    return ex


async def drill_bridge_flow(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex)
    ok = await until(lambda: ex.open_orders(), 20)
    check(ok, "the strategy's quotes reach the exchange: %d resting" % len(ex.open_orders()))
    check(len(ex.open_orders()) <= 6, "and stay within max-open")
    rc = await r.stop()
    check(rc == 0, "supervisor exits 0 on SIGTERM")
    check(len(ex.open_orders()) == 0, "nothing left resting on the exchange")
    ends = r.live_events("segment_end")
    check(r.bridge_events("exit_flat") and r.bridge_events("exit_flat")[-1]["flat"], "bridge reported flat")
    check(ends and ends[-1]["reason"] == "kill" and ends[-1]["open_before_flatten"] > 0, "hft_live ended with the kill path and quotes out")
    ex.stop()
    r.cleanup()


async def drill_kill_bridge(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex)
    await until(lambda: ex.open_orders(), 20)
    n = len(ex.open_orders())
    pid = r.pid("bridge", 1)
    t0 = time.monotonic()
    os.kill(pid, signal.SIGKILL)
    ok = await until(lambda: len(r.sup("pair_start")) >= 2, 15)
    check(ok, "the trio restarts after kill -9 of the bridge")
    cleared = await until(lambda: any(e.get("cancelled", 0) >= 1 for e in r.bridge_events("cancel_all")), 15)
    check(cleared, "the new bridge's first act is cancel-all, and it found %s stranded orders (%d before the kill)" %
          ([e["cancelled"] for e in r.bridge_events("cancel_all")], n))
    print("      orders stranded for about %.1f s (kill to cancel-all)" % (time.monotonic() - t0))
    check(await until(lambda: ex.open_orders(), 15), "quoting resumes on the exchange")
    await r.stop()
    check(len(ex.open_orders()) == 0, "flat at the end")
    ex.stop()
    r.cleanup()


async def drill_kill_live_bridge(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex)
    await until(lambda: ex.open_orders(), 20)
    check(len(ex.open_orders()) > 0, "orders resting before the kill")
    os.kill(r.pid("live", 1), signal.SIGKILL)
    ok = await until(lambda: r.sup("pair_stopped"), 15)
    check(ok, "pair stopped")
    ps = r.sup("pair_stopped")[0]
    check(ps["live"]["signal"] == 9 and ps["bridge"]["returncode"] == 0, "hft_live died by SIGKILL; the bridge saw end of input and left cleanly: %s" % ps["bridge"])
    # at the moment the supervisor reports the stop, the old bridge is gone: its cancel-all must have run
    check(r.bridge_events("exit_flat") and r.bridge_events("exit_flat")[0]["flat"], "bridge cancelled everything before exiting")
    check(await until(lambda: len(r.sup("pair_start")) >= 2, 15), "restarted")
    check(await until(lambda: ex.open_orders(), 15), "quoting again")
    await r.stop()
    check(len(ex.open_orders()) == 0, "flat at the end")
    ex.stop()
    r.cleanup()


async def drill_exchange_down(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex)
    await until(lambda: ex.open_orders(), 20)
    ex.mode = "down"
    ok = await until(lambda: r.sup("bridge_exited"), 30)
    check(ok, "the bridge gives up on a dead exchange (exit %s)" % (r.sup("bridge_exited")[0]["returncode"] if ok else None))
    check(r.sup("bridge_exited") and r.sup("bridge_exited")[0]["returncode"] == 1, "with the restartable code 1")
    check(not r.sup("fatal_halt"), "and the supervisor does not treat it as fatal")
    ex.mode = "ok"
    check(await until(lambda: len(r.sup("pair_start")) >= 2 and ex.open_orders(), 30), "when the exchange is back, quoting resumes")
    await r.stop()
    check(len(ex.open_orders()) == 0, "flat at the end")
    ex.stop()
    r.cleanup()


async def drill_bad_credentials(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex, env={"HFT_CB_PASSPHRASE": "wrong"})
    rc = await asyncio.get_event_loop().run_in_executor(None, lambda: r.p.wait(30))
    check(rc == 4, "supervisor exits 4 (got %s)" % rc)
    check(len(r.sup("pair_start")) == 1 and r.sup("fatal_halt") and r.sup("fatal_halt")[0].get("who") == "bridge", "not restarted: a person must look")
    check(len(ex.requests("POST")) == 0, "no order was placed")
    ex.stop()
    r.cleanup()


async def drill_bridge_hang(feed, port):
    ex = new_exchange()
    r = bridge_run(feed, port, ex)
    await until(lambda: ex.open_orders(), 20)
    os.kill(r.pid("bridge", 1), signal.SIGSTOP)
    ok = await until(lambda: r.sup("watchdog"), 20)
    check(ok and r.sup("watchdog_target")[0]["who"] == "bridge", "the watchdog names the bridge: %s" % r.sup("watchdog_target")[:1])
    check(await until(lambda: len(r.sup("pair_start")) >= 2, 15), "trio restarted")
    check(await until(lambda: any(e.get("cancelled", 0) >= 1 for e in r.bridge_events("cancel_all")), 15), "the stranded orders were cancelled by the new bridge")
    await r.stop()
    check(len(ex.open_orders()) == 0, "flat at the end")
    ex.stop()
    r.cleanup()


# ---- closed loop: the exchange's reports drive the OMS ---------------------------------------------------------
def loop_run(feed, port, ex, bridge_extra="", **kw):
    env = {"HFT_CB_KEY": KEY, "HFT_CB_SECRET": SECRET, "HFT_CB_PASSPHRASE": PASS}
    env.update(kw.pop("env", {}))
    args = "--test-exchange --base-url %s --rate 20 --max-notional 100000 --max-errors 3 --sync-s 0.5 %s" % (ex.url, bridge_extra)
    return Run(feed, port, extra=["--loop", "--bridge-args", args] + kw.pop("extra", []), env=env, **kw)


def live_metric(r, name):
    try:
        for l in open(os.path.join(r.dir, "metrics.prom")):
            if l.startswith(name + " "):
                return float(l.split()[1])
    except OSError:
        pass
    return None


async def drill_loop_flow(feed, port):
    ex = new_exchange()
    r = loop_run(feed, port, ex)
    check(await until(lambda: ex.open_orders(), 20), "orders rest on the exchange")
    check(await until(lambda: (live_metric(r, "hft_live_exchange_reports_total") or 0) >= 2, 15), "and their acknowledgements reached hft_live (%s reports)" % live_metric(r, "hft_live_exchange_reports_total"))
    # a reconnect: the segment ends only when the exchange has confirmed the cancels
    feed.drop = True
    await asyncio.sleep(0.4)
    feed.drop = False
    ok = await until(lambda: r.live_events("segment_end"), 20)
    check(ok, "segment ended")
    e = r.live_events("segment_end")[0]
    check(e["flat"] == 1 and e["open_before_flatten"] > 0 and e["flatten_sim_ns"] > 1_000_000, "flat after real confirmations (%.0f ms), orders were out: %s" % (e["flatten_sim_ns"] / 1e6, e))
    check(await until(lambda: len(r.live_events("segment_start")) >= 2 and ex.open_orders(), 20), "a new segment quotes again")
    check(len(r.sup("pair_start")) == 1, "no process restart")
    rc = await r.stop()
    ends = r.live_events("segment_end")
    check(rc == 0 and ends[-1]["reason"] == "kill" and ends[-1]["flat"] == 1 and ends[-1]["kill_latched"] == 1, "SIGTERM: kill path, flat after confirmations: %s" % ends[-1])
    check(len(ex.open_orders()) == 0, "nothing left on the exchange")
    ex.stop()
    r.cleanup()


async def drill_loop_fill(feed, port):
    ex = new_exchange()
    r = loop_run(feed, port, ex)
    await until(lambda: len(ex.open_orders()) >= 1, 20)
    n0 = live_metric(r, "hft_live_exchange_reports_total") or 0
    victim = ex.open_orders()[0]
    check(ex.fill(victim["id"]), "the market trades through one of our orders (%s %s)" % (victim["side"], victim["price"]))
    check(await until(lambda: (live_metric(r, "hft_live_exchange_reports_total") or 0) > n0, 15), "the fill report reached hft_live")
    want = 5 if victim["side"] == "buy" else -5
    check(await until(lambda: live_metric(r, "hft_live_position_lots") == want, 10), "and its position is %+d lots (metric says %s)" % (want, live_metric(r, "hft_live_position_lots")))
    check(await until(lambda: len(r.sup("pair_start")) == 1 and not r.sup("pair_stopped"), 1), "no restart")
    rc = await r.stop()
    ends = r.live_events("segment_end")
    check(rc == 0 and len(ex.open_orders()) == 0, "clean stop, no orders left")
    check(ends[-1]["position_lots"] == want and ends[-1]["fills"] == 1, "the segment end records the position it ends with: %s" % {k: ends[-1][k] for k in ("flat", "position_lots", "fills")})
    ex.stop()
    r.cleanup()


async def drill_loop_fill_ws(feed, port):
    # the REST sync is a minute away: only the websocket can deliver this fill in time
    ex = new_exchange()
    ex.start_ws()
    r = loop_run(feed, port, ex, bridge_extra="--sync-s 60 --ws-fills --ws-url " + ex.ws_url)
    check(await until(lambda: ex.ws_subscriptions >= 1, 20), "the bridge subscribed to the user channel")
    await until(lambda: len(ex.open_orders()) >= 1, 20)
    victim = ex.open_orders()[0]
    t0 = time.monotonic()
    check(ex.fill(victim["id"]), "the market trades through one of our orders (%s %s)" % (victim["side"], victim["price"]))
    want = 5 if victim["side"] == "buy" else -5
    ok = await until(lambda: live_metric(r, "hft_live_position_lots") == want, 6)
    dt = time.monotonic() - t0
    check(ok and dt < 4, "hft_live's position is %+d lots %.2f s after the fill, with the sync a minute away (metric %s)" % (want, dt, live_metric(r, "hft_live_position_lots")))
    check(len(r.sup("pair_start")) == 1 and not r.sup("pair_stopped"), "no restart")
    rc = await r.stop()
    ends = r.live_events("segment_end")
    check(rc == 0 and len(ex.open_orders()) == 0 and ends[-1]["position_lots"] == want, "clean stop, nothing left, position kept: %s" % {k: ends[-1][k] for k in ("flat", "position_lots", "fills")})
    ex.stop()
    r.cleanup()


async def drill_loop_ws_refused(feed, port):
    # the exchange refuses the socket: fills still arrive, by the sync
    ex = new_exchange()
    ex.start_ws()
    ex.ws_mode = "reject_auth"
    r = loop_run(feed, port, ex, bridge_extra="--ws-fills --ws-url " + ex.ws_url)
    await until(lambda: len(ex.open_orders()) >= 1, 20)
    check(await until(lambda: ex.ws_rejected >= 1, 10), "the socket was refused")
    victim = ex.open_orders()[0]
    ex.fill(victim["id"])
    want = 5 if victim["side"] == "buy" else -5
    check(await until(lambda: live_metric(r, "hft_live_position_lots") == want, 15), "the fill still reaches hft_live through the sync (position %s)" % live_metric(r, "hft_live_position_lots"))
    rc = await r.stop()
    check(rc == 0 and len(ex.open_orders()) == 0, "clean stop")
    ex.stop()
    r.cleanup()


async def drill_loop_exchange_slow(feed, port):
    ex = new_exchange()
    ex.latency = 0.4
    r = loop_run(feed, port, ex, extra=["--watchdog-s", "20"])
    check(await until(lambda: ex.open_orders(), 30), "orders rest on a slow exchange")
    rc = await r.stop()
    ends = r.live_events("segment_end")
    check(rc == 0 and ends and ends[-1]["flat"] == 1, "SIGTERM with 0.4 s per request: still flat: %s" % (ends[-1:] ,))
    check(ends[-1]["flatten_sim_ns"] > 400_000_000, "and the flatten took real time (%.0f ms)" % (ends[-1]["flatten_sim_ns"] / 1e6))
    check(len(ex.open_orders()) == 0, "nothing left on the exchange")
    ex.stop()
    r.cleanup()


async def drill_loop_exchange_dead_at_stop(feed, port):
    ex = new_exchange()
    r = loop_run(feed, port, ex, extra=["--term-grace-s", "20"])
    await until(lambda: ex.open_orders(), 20)
    ex.mode = "down"
    await asyncio.sleep(0.3)
    t0 = time.monotonic()
    rc = await r.stop()
    ends = r.live_events("segment_end")
    check(rc == 4 and r.sup("stop_not_flat"), "the supervisor does not call that a clean stop (exit %s)" % rc)
    check(ends and ends[-1]["flat"] == 0, "the exchange is down, so hft_live cannot call itself flat: %s" % (ends[-1:],))
    check(r.live_events("halted") or ends[-1]["kill_latched"] == 1, "and it stopped trading")
    print("      stop took %.1f s" % (time.monotonic() - t0))
    ex.mode = "ok"
    ex.stop()
    r.cleanup()


async def close_drill(feed, port, tune, want_rc, closed):
    """The strategy gets a fill, then SIGTERM arrives while it holds a position and --close-position is on."""
    ex = new_exchange()
    ex.start_ws()
    r = loop_run(feed, port, ex, extra=["--close-position"], bridge_extra="--ws-fills --ws-url " + ex.ws_url)
    await until(lambda: len(ex.open_orders()) >= 1, 20)
    victim = ex.open_orders()[0]
    ex.best_bid, ex.best_ask = 100.00, 100.10   # from now on the exchange quotes where the invented feed does
    tune(ex)
    ex.fill(victim["id"])
    want = 5 if victim["side"] == "buy" else -5
    check(await until(lambda: live_metric(r, "hft_live_position_lots") == want, 10), "the fill reached hft_live: position %+d lots" % want)
    rc = await r.stop()
    ends = r.live_events("segment_end")
    e = ends[-1]
    check(rc == want_rc, "supervisor exit %d (got %s)" % (want_rc, rc))
    check(len(ex.open_orders()) == 0, "nothing left resting on the exchange")
    if closed:
        check(len(ex.ioc_orders) == 1 and ex.ioc_orders[0][0] == ("sell" if want > 0 else "buy"), "one closing order, against the position: %s" % ex.ioc_orders)
        check(e["position_lots"] == 0 and e["closed_lots"] == abs(want) and not e["close_failed"], "the segment ends flat in position: %s" % {k: e[k] for k in ("position_lots", "closed_lots", "close_attempts", "close_failed")})
        check(not r.sup("stop_not_flat") and not r.live_events("position_not_closed"), "no alarm")
    else:
        check(e["position_lots"] == want and e["closed_lots"] == 0 and e["close_failed"] == 1, "the position stays and the segment says the close failed: %s" % {k: e[k] for k in ("position_lots", "closed_lots", "close_attempts", "close_failed", "close_why_not")})
        check(bool(r.sup("stop_not_flat")) and bool(r.live_events("position_not_closed")), "and it is loud: stop_not_flat and position_not_closed")
    ex.stop()
    r.cleanup()


async def drill_loop_close(feed, port):
    await close_drill(feed, port, lambda ex: None, 0, True)


async def drill_loop_close_no_liquidity(feed, port):
    await close_drill(feed, port, lambda ex: setattr(ex, "ioc_fill_fraction", 0.0), 4, False)


async def drill_loop_close_off_the_market(feed, port):
    # the exchange's own quote is 20% away from the local book (a stale or wrong feed): the bridge refuses to send the order
    def far(ex):
        ex.best_bid, ex.best_ask = 80.00, 80.10
    await close_drill(feed, port, far, 4, False)
    # (best prices are set again inside close_drill before tune() runs, so `far` wins)


DRILLS = [("drop", drill_drop), ("silent", drill_silent), ("kill_capture", drill_kill_capture),
          ("kill_live", drill_kill_live), ("hang", drill_hang), ("sigterm", drill_sigterm), ("term_ignored", drill_term_ignored),
          ("capture_dies_live_stuck", drill_capture_dies_live_stuck), ("slow_flapping", drill_slow_flapping), ("crash_loop", drill_crash_loop), ("integrity", drill_integrity),
          ("bridge_flow", drill_bridge_flow), ("kill_bridge", drill_kill_bridge), ("kill_live_bridge", drill_kill_live_bridge),
          ("exchange_down", drill_exchange_down), ("bad_credentials", drill_bad_credentials), ("bridge_hang", drill_bridge_hang),
          ("loop_flow", drill_loop_flow), ("loop_fill", drill_loop_fill), ("loop_fill_ws", drill_loop_fill_ws), ("loop_ws_refused", drill_loop_ws_refused), ("loop_slow", drill_loop_exchange_slow),
          ("loop_dead_at_stop", drill_loop_exchange_dead_at_stop),
          ("loop_close", drill_loop_close), ("loop_close_no_liquidity", drill_loop_close_no_liquidity), ("loop_close_off_market", drill_loop_close_off_the_market)]


async def main(only):
    global LIVE
    feed = Feed()
    async with websockets.serve(feed.handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        for name, fn in DRILLS:
            if only and name not in only:
                continue
            print("drill:", name, flush=True)
            await fn(feed, port)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--live":
        LIVE = os.path.abspath(args[1])
        args = args[2:]
    asyncio.run(main(args))
    if failures:
        sys.exit("%d drill failures" % failures)
    print("drill: all checks passed")
