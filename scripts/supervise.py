#!/usr/bin/env python3
"""Runs `capture_l2.py --out - | hft_live` and keeps it running.

  supervise.py [--run-dir DIR] [--live PATH] [--seconds N] [--stale-ms N] ...

What it does:
  * starts the two processes joined by a pipe, with a log, a metrics file and a
    resubscribe file in --run-dir;
  * restarts the pair when either dies, with exponential backoff (reset after a
    run of --stable-s seconds), and gives up (exit 3) if it restarts more than
    --max-restarts times within --window-s seconds;
  * does NOT restart when hft_live exits 4 (the book disagreed with the model, a
    flatten failed, a desync): that needs a person (exit 4);
  * watches the metrics file; if hft_live stops refreshing it for --watchdog-s
    seconds (hung, stopped) it is killed with SIGKILL and the pair restarted;
  * on SIGTERM or SIGINT, forwards SIGTERM to hft_live (kill switch latched, all
    quotes cancelled, summary), waits --term-grace-s, then SIGKILL, and exits 0.

Exit codes: 0 stopped on request, 1 usage, 3 crash loop, 4 hft_live reported an
integrity failure.

The paper venue lives inside hft_live. A SIGKILL of hft_live therefore loses the
venue's state along with the process; against a real exchange orders would stay
working there unless the venue cancels on disconnect. That case cannot be shown
with a paper venue and is not claimed to be handled.

Every event is one JSON line (--log, default stderr). Commands can be replaced
(--live-cmd, --capture-cmd) for tests; {resnap} {metrics} {log} are substituted.
"""
import argparse
import collections
import json
import os
import shlex
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

stopping = False


def on_signal(signum, frame):
    global stopping
    stopping = True


class Log:
    def __init__(self, path):
        self.f = sys.stderr if path in (None, "-") else open(path, "a", buffering=1)

    def emit(self, event, **kw):
        rec = {"ts_ms": int(time.time() * 1000), "event": event}
        rec.update(kw)
        self.f.write(json.dumps(rec) + "\n")
        self.f.flush()


def kill_group(p, sig):
    if p is None or p.poll() is not None:
        return
    try:
        os.killpg(p.pid, sig)  # children run in their own session, so the group is just the child
    except (ProcessLookupError, PermissionError):
        pass


def describe(rc):
    return {"returncode": rc, "signal": -rc if rc is not None and rc < 0 else None}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-dir", default="/tmp/hft-run")
    ap.add_argument("--live", default=os.path.join(ROOT, "build/release/hft_live"))
    ap.add_argument("--seconds", type=float, default=86400.0, help="capture length before it is restarted anyway")
    ap.add_argument("--stale-ms", type=float, default=3000.0)
    ap.add_argument("--url", default=None)
    ap.add_argument("--product", default="BTC-USD")
    ap.add_argument("--live-cmd", default=None, help="replace the hft_live command (shell-split)")
    ap.add_argument("--capture-cmd", default=None, help="replace the capture command (shell-split)")
    ap.add_argument("--log", default=None, help="supervisor event log (default stderr)")
    ap.add_argument("--max-restarts", type=int, default=5)
    ap.add_argument("--window-s", type=float, default=300.0)
    ap.add_argument("--backoff-s", type=float, default=0.5)
    ap.add_argument("--max-backoff-s", type=float, default=30.0)
    ap.add_argument("--stable-s", type=float, default=60.0, help="a run this long resets the backoff")
    ap.add_argument("--watchdog-s", type=float, default=30.0)
    ap.add_argument("--startup-grace-s", type=float, default=10.0)
    ap.add_argument("--term-grace-s", type=float, default=10.0)
    ap.add_argument("--metrics-s", type=float, default=2.0)
    a = ap.parse_args(argv)

    os.makedirs(a.run_dir, exist_ok=True)
    resnap = os.path.join(a.run_dir, "resnap")
    metrics = os.path.join(a.run_dir, "metrics.prom")
    events = os.path.join(a.run_dir, "events.jsonl")
    sub = {"resnap": resnap, "metrics": metrics, "log": events}
    log = Log(a.log)

    def fmt(cmd):
        return [x.format(**sub) for x in cmd]

    if a.capture_cmd:
        capture_cmd = fmt(shlex.split(a.capture_cmd))
    else:
        capture_cmd = [sys.executable, os.path.join(HERE, "capture_l2.py"), "--out", "-", "--seconds", str(a.seconds),
                       "--product", a.product, "--resnap-file", resnap]
        if a.url:
            capture_cmd += ["--url", a.url]
    if a.live_cmd:
        live_cmd = fmt(shlex.split(a.live_cmd))
    else:
        live_cmd = [a.live, "--resnap-file", resnap, "--log", events, "--metrics", metrics, "--metrics-s", str(a.metrics_s),
                    "--report-s", "60", "--stale-ms", str(a.stale_ms)]

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    log.emit("supervisor_start", run_dir=a.run_dir, capture=capture_cmd, live=live_cmd)

    starts = collections.deque()
    backoff = a.backoff_s
    run_no = 0
    exit_code = 0
    while True:
        run_no += 1
        for f in (resnap, metrics):
            try:
                os.remove(f)
            except FileNotFoundError:
                pass
        r, w = os.pipe()
        cap_err = open(os.path.join(a.run_dir, "capture.err"), "ab")
        live_out = open(os.path.join(a.run_dir, "live.out"), "ab")
        live_err = open(os.path.join(a.run_dir, "live.err"), "ab")
        cap = subprocess.Popen(capture_cmd, stdout=w, stderr=cap_err, start_new_session=True)
        os.close(w)
        live = subprocess.Popen(live_cmd, stdin=r, stdout=live_out, stderr=live_err, start_new_session=True)
        os.close(r)
        t0 = time.monotonic()
        starts.append(time.time())
        log.emit("pair_start", run=run_no, capture_pid=cap.pid, live_pid=live.pid)

        why = None
        cap_gone_at = None
        while why is None:
            time.sleep(0.1)
            now = time.monotonic()
            if stopping:
                why = "stop_requested"
            elif live.poll() is not None:
                why = "live_exited"
            elif cap.poll() is not None:
                # the capture ended; hft_live sees end of input and exits flat. Give it time to.
                if cap_gone_at is None:
                    cap_gone_at = now
                    log.emit("capture_exited", run=run_no, **describe(cap.returncode))
                elif now - cap_gone_at > 5.0:
                    why = "live_did_not_follow_capture"
            elif a.watchdog_s > 0 and now - t0 > a.startup_grace_s:
                try:
                    age = time.time() - os.stat(metrics).st_mtime
                except FileNotFoundError:
                    age = now - t0
                if age > a.watchdog_s:
                    why = "watchdog"

        if why == "stop_requested":
            log.emit("stop_requested", run=run_no)
            kill_group(live, signal.SIGTERM)  # the kill path: switch latched, quotes cancelled, summary
            try:
                live.wait(a.term_grace_s)
            except subprocess.TimeoutExpired:
                log.emit("live_ignored_sigterm", run=run_no)
                kill_group(live, signal.SIGKILL)
                live.wait()
            kill_group(cap, signal.SIGTERM)
        elif why in ("watchdog", "live_did_not_follow_capture"):
            log.emit(why, run=run_no)  # the cleanup below kills it
        elif why == "live_exited":
            log.emit("live_exited", run=run_no, **describe(live.returncode))
        # whatever is left of the pair goes now
        kill_group(cap, signal.SIGTERM)
        try:
            cap.wait(5)
        except subprocess.TimeoutExpired:
            kill_group(cap, signal.SIGKILL)
            cap.wait()
        if live.poll() is None:
            kill_group(live, signal.SIGKILL)
            live.wait()
        for f in (cap_err, live_out, live_err):
            f.close()
        ran = time.monotonic() - t0
        log.emit("pair_stopped", run=run_no, ran_s=round(ran, 2), why=why, live=describe(live.returncode),
                 capture=describe(cap.returncode))

        if why == "stop_requested":
            exit_code = 0
            break
        if live.returncode == 4:
            log.emit("fatal_halt", run=run_no, detail="hft_live reported an integrity failure; not restarting, a person must look")
            exit_code = 4
            break
        now_wall = time.time()
        while starts and now_wall - starts[0] > a.window_s:
            starts.popleft()
        if len(starts) > a.max_restarts:
            log.emit("giving_up", restarts_in_window=len(starts) - 1, window_s=a.window_s)
            exit_code = 3
            break
        if ran >= a.stable_s:
            backoff = a.backoff_s
        log.emit("restart_in", seconds=backoff)
        end = time.monotonic() + backoff
        while time.monotonic() < end and not stopping:
            time.sleep(0.05)
        if stopping:
            exit_code = 0
            break
        backoff = min(backoff * 2, a.max_backoff_s)
    log.emit("supervisor_exit", code=exit_code)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
