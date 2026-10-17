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
    ap.add_argument("--bridge", action="store_true", help="also run scripts/order_bridge.py (sandbox order entry; keys from HFT_CB_* env)")
    ap.add_argument("--loop", action="store_true", help="closed loop (implies --bridge): the exchange's reports go back into hft_live")
    ap.add_argument("--bridge-args", default="", help="extra order_bridge.py options, shell-split (e.g. --rate 6 --max-notional 10)")
    ap.add_argument("--close-position", action="store_true",
                    help="with --loop: when hft_live stops for good (shutdown or kill) it first sells or buys back its position with an "
                         "immediate-or-cancel order; if it cannot, it exits 4 and says POSITION NOT CLOSED")
    ap.add_argument("--close-collar-bps", type=int, default=None, help="how far past the touch the closing order is priced (hft_live default 30)")
    ap.add_argument("--bridge-cmd", default=None, help="replace the bridge command (shell-split)")
    a = ap.parse_args(argv)
    if (a.close_position or a.close_collar_bps is not None) and not (a.loop and not a.live_cmd):
        ap.error("--close-position and --close-collar-bps need --loop and the default hft_live command (add the options to --live-cmd yourself otherwise)")

    os.makedirs(a.run_dir, exist_ok=True)
    resnap = os.path.join(a.run_dir, "resnap")
    metrics = os.path.join(a.run_dir, "metrics.prom")
    events = os.path.join(a.run_dir, "events.jsonl")
    bridge_events = os.path.join(a.run_dir, "bridge.jsonl")
    bridge_metrics = os.path.join(a.run_dir, "bridge.prom")
    sub = {"resnap": resnap, "metrics": metrics, "log": events, "bridge_metrics": bridge_metrics, "bridge_log": bridge_events}
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
        if a.close_position:
            live_cmd.append("--close-position")
        if a.close_collar_bps is not None:
            live_cmd += ["--close-collar-bps", str(a.close_collar_bps)]

    bridge_cmd = None
    if a.loop:
        a.bridge = True
    if a.bridge or a.bridge_cmd:
        if a.bridge_cmd:
            bridge_cmd = fmt(shlex.split(a.bridge_cmd))
        else:
            bridge_cmd = [sys.executable, os.path.join(HERE, "order_bridge.py"), "--log", bridge_events, "--metrics", bridge_metrics,
                          "--metrics-s", "1"] + (["--loop"] if a.loop else []) + shlex.split(a.bridge_args)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    log.emit("supervisor_start", run_dir=a.run_dir, capture=capture_cmd, live=live_cmd, bridge=bridge_cmd)

    starts = collections.deque()
    backoff = a.backoff_s
    run_no = 0
    exit_code = 0
    while True:
        run_no += 1
        for f in (resnap, metrics, bridge_metrics):
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
        bridge = None
        bridge_out = bridge_err = None
        if bridge_cmd:
            # hft_live writes its order flow into this pipe; the bridge reads it. Neither can be re-attached to a new
            # partner, so if either dies the trio restarts (the bridge cancels everything on the exchange at start).
            orr, orw = os.pipe()
            extra_live, keep = ["--orders-fd", str(orw)], [orw]
            rr = rw = None
            if a.loop:  # the bridge's reports come back to hft_live through a second pipe
                rr, rw = os.pipe()
                extra_live += ["--exchange-fd", str(rr)]
                keep.append(rr)
            live = subprocess.Popen(live_cmd + extra_live, stdin=r, stdout=live_out, stderr=live_err,
                                    start_new_session=True, pass_fds=tuple(keep))
            bridge_out = open(os.path.join(a.run_dir, "bridge.out"), "ab")
            bridge_err = open(os.path.join(a.run_dir, "bridge.err"), "ab")
            bridge = subprocess.Popen(bridge_cmd, stdin=orr, stdout=rw if rw is not None else bridge_out, stderr=bridge_err,
                                      start_new_session=True)
            os.close(orr)
            os.close(orw)
            if a.loop:
                os.close(rr)
                os.close(rw)
        else:
            live = subprocess.Popen(live_cmd, stdin=r, stdout=live_out, stderr=live_err, start_new_session=True)
        os.close(r)
        t0 = time.monotonic()
        starts.append(time.time())
        log.emit("pair_start", run=run_no, capture_pid=cap.pid, live_pid=live.pid, bridge_pid=bridge.pid if bridge else None)

        why = None
        wd_who = None
        cap_gone_at = None
        while why is None:
            time.sleep(0.1)
            now = time.monotonic()
            if stopping:
                why = "stop_requested"
            elif live.poll() is not None:
                why = "live_exited"
            elif bridge is not None and bridge.poll() is not None:
                why = "bridge_exited"
            elif cap.poll() is not None:
                # the capture ended; hft_live sees end of input and exits flat. Give it time to.
                if cap_gone_at is None:
                    cap_gone_at = now
                    log.emit("capture_exited", run=run_no, **describe(cap.returncode))
                elif now - cap_gone_at > 5.0:
                    why = "live_did_not_follow_capture"
            elif a.watchdog_s > 0 and now - t0 > a.startup_grace_s:
                for who, path in (("live", metrics), ("bridge", bridge_metrics if bridge else None)):
                    if path is None:
                        continue
                    try:
                        age = time.time() - os.stat(path).st_mtime
                    except FileNotFoundError:
                        age = now - t0
                    if age > a.watchdog_s:
                        why = "watchdog"
                        wd_who = who
                        log.emit("watchdog_target", run=run_no, who=who, age_s=round(age, 1))

        if why == "stop_requested":
            log.emit("stop_requested", run=run_no)
            kill_group(live, signal.SIGTERM)  # the kill path: switch latched, quotes cancelled, summary
            try:
                live.wait(a.term_grace_s)
            except subprocess.TimeoutExpired:
                log.emit("live_ignored_sigterm", run=run_no)
                kill_group(live, signal.SIGKILL)
                live.wait()
            if bridge is not None:  # hft_live is gone, so the bridge sees end of input and cancels everything
                try:
                    bridge.wait(a.term_grace_s)
                except subprocess.TimeoutExpired:
                    log.emit("bridge_ignored_eof", run=run_no)
                    kill_group(bridge, signal.SIGKILL)
                    bridge.wait()
            kill_group(cap, signal.SIGTERM)
        elif why in ("watchdog", "live_did_not_follow_capture"):
            log.emit(why, run=run_no)  # the cleanup below kills it
        elif why == "live_exited":
            log.emit("live_exited", run=run_no, **describe(live.returncode))
        elif why == "bridge_exited":
            log.emit("bridge_exited", run=run_no, **describe(bridge.returncode))
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
        if bridge is not None and bridge.poll() is None:
            if why == "watchdog" and wd_who == "bridge":  # it is the hung one: it cannot cancel anything
                kill_group(bridge, signal.SIGKILL)
            else:  # hft_live is gone, so the bridge sees end of input: let it cancel and leave
                try:
                    bridge.wait(a.term_grace_s)
                except subprocess.TimeoutExpired:
                    log.emit("bridge_ignored_eof", run=run_no)
                    kill_group(bridge, signal.SIGKILL)
            bridge.wait()
        for f in (cap_err, live_out, live_err, bridge_out, bridge_err):
            if f:
                f.close()
        ran = time.monotonic() - t0
        log.emit("pair_stopped", run=run_no, ran_s=round(ran, 2), why=why, live=describe(live.returncode),
                 capture=describe(cap.returncode), bridge=describe(bridge.returncode) if bridge else None)

        if why == "stop_requested":
            exit_code = 0
            if live.returncode == 4:  # asked to stop, and it could not say it was flat: do not report a clean stop
                log.emit("stop_not_flat", run=run_no, detail="hft_live exited 4 during the stop; orders may still rest on the exchange, or a position may remain open")
                exit_code = 4
            break
        if live.returncode == 4:
            log.emit("fatal_halt", run=run_no, detail="hft_live exited 4 (an integrity failure, a flatten that failed, or a position it could not close); not restarting, a person must look")
            exit_code = 4
            break
        if bridge is not None and bridge.returncode in (2, 4):
            log.emit("fatal_halt", run=run_no, who="bridge", code=bridge.returncode,
                     detail="order bridge refused to run (configuration, credentials or clock); not restarting")
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
