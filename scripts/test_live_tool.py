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

if failures:
    sys.exit("%d live tool test failures" % failures)
print("test_live_tool: all checks passed")
