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
    def __init__(self, feed, port, extra=(), live_cmd=None, capture_cmd=None):
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
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def sup(self, name=None):
        ev = read_jsonl(self.sup_log)
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


DRILLS = [("drop", drill_drop), ("silent", drill_silent), ("kill_capture", drill_kill_capture),
          ("kill_live", drill_kill_live), ("hang", drill_hang), ("sigterm", drill_sigterm), ("term_ignored", drill_term_ignored),
          ("capture_dies_live_stuck", drill_capture_dies_live_stuck), ("slow_flapping", drill_slow_flapping), ("crash_loop", drill_crash_loop), ("integrity", drill_integrity)]


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
