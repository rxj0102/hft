#!/usr/bin/env python3
"""Tests capture_l2.py against a local fake server: normal flow, an abrupt
disconnect, a silent connection (idle timeout), and give-up after too many
reconnects. No network access needed."""
import asyncio, json, os, sys, tempfile, time
os.environ["NO_PROXY"] = "127.0.0.1,localhost"
os.environ["no_proxy"] = "127.0.0.1,localhost"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import websockets
import capture_l2

SNAP = json.dumps({"type": "snapshot", "product_id": "BTC-USD", "bids": [["100.00", "1.0"]], "asks": [["101.00", "2.0"]]})
def upd(i):
    return json.dumps({"type": "l2update", "product_id": "BTC-USD", "changes": [["buy", "100.00", "%d.0" % i]], "time": "t"})

failures = 0
def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)

def read(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        rows.append(line)
    return rows

async def run_scenario(handler, seconds, **kw):
    async with websockets.serve(handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        path = tempfile.mktemp(suffix=".l2")
        st = await capture_l2.capture("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", seconds, path,
                                      kw.get("idle", 0.5), kw.get("max_reconnects", 20))
        return st, read(path)

async def main():
    calls = {"n": 0}

    async def flaky(ws):
        calls["n"] += 1
        await ws.recv()  # the subscribe message
        n = calls["n"]
        await ws.send(SNAP)
        for i in range(3):
            await ws.send(upd(i))
        if n == 1:
            await ws.close(code=1011)       # server drops us
        elif n == 2:
            await asyncio.sleep(5)          # goes silent: client must time out
        else:
            for i in range(3, 6):
                await ws.send(upd(i))
            await asyncio.sleep(5)

    st, rows = await run_scenario(flaky, 6.0, idle=0.4)
    msgs = [r for r in rows if not r.startswith("#")]
    marks = [r.split("\t")[1] for r in rows if r.startswith("#")]
    check(st["messages"] == len(msgs), "message count matches file")
    check(sum(1 for m in msgs if '"snapshot"' in m) >= 3, "a snapshot after every (re)connect")
    check("reconnect" in marks and "idle_timeout" in marks and marks[0] == "start" and marks[-1] == "stop", "markers: %s" % marks)
    check(st["reconnects"] >= 2, "reconnected after drop and after silence")
    ts = [int(r.split("\t")[0].lstrip("#")) for r in rows]
    check(ts == sorted(ts), "timestamps never go backwards")
    check(all(json.loads(m.split("\t", 1)[1]) for m in msgs), "every recorded message is intact JSON")

    async def refuse(ws):
        await ws.recv()
        await ws.close()
    st, rows = await run_scenario(refuse, 20.0, max_reconnects=2)
    marks = [r.split("\t")[1] for r in rows if r.startswith("#")]
    check("give_up" in marks and marks[-1] == "stop", "gives up after max reconnects: %s" % marks)

    st, rows = await run_scenario(lambda ws: ws.wait_closed(), 0.3)
    check(rows[0].split("\t")[1] == "start" and rows[-1].split("\t")[1] == "stop", "short quiet run starts and stops cleanly")

async def verify_test():
    async def serve(ws):
        await ws.recv()
        await ws.send(json.dumps({"type": "subscriptions", "channels": []}))
        await ws.send(SNAP)
        await asyncio.sleep(2)
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        path = tempfile.mktemp(suffix=".verify")
        ok = await capture_l2.fetch_verify_snapshot("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", path, timeout=3.0)
        check(ok, "verify snapshot fetched")
        rows = read(path)
        check(len(rows) == 1 and '"snapshot"' in rows[0], "verify file holds exactly the snapshot")
    # concurrent with a capture: the verify line carries the main clock
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        out = tempfile.mktemp(suffix=".l2")
        st = await capture_l2.capture("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", 4.0, out, 3.0, 5,
                                      verify_path=out + ".verify")
        check(st.get("verify") is True, "capture with verify reports success")
        vt = int(read(out + ".verify")[0].split("\t")[0])
        stop_t = int(read(out)[-1].split("\t")[0].lstrip("#"))
        check(1.5e9 <= vt <= stop_t, "verify timestamp is on the main capture clock (%d vs stop %d)" % (vt, stop_t))
    # no server at all: reports failure instead of raising
    ok = await capture_l2.fetch_verify_snapshot("ws://127.0.0.1:9", "BTC-USD", "level2_batch", tempfile.mktemp(), timeout=1.0)
    check(not ok, "verify fails cleanly when the server is unreachable")

async def resnap_test():
    """A file appearing makes the capture reconnect at once and get a fresh snapshot."""
    conns = {"n": 0}

    async def serve(ws):
        conns["n"] += 1
        await ws.recv()
        await ws.send(SNAP)
        i = 0
        try:
            while True:
                await ws.send(upd(i)); i += 1
                await asyncio.sleep(0.05)
        except Exception:
            pass
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        out = tempfile.mktemp(suffix=".l2")
        flag = tempfile.mktemp(suffix=".flag")

        async def poke():
            await asyncio.sleep(0.6)
            open(flag, "w").write("x")
            await asyncio.sleep(0.8)
            open(flag, "w").write("x")
        pk = asyncio.create_task(poke())
        st = await capture_l2.capture("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", 2.5, out, 5.0, 0,
                                      resnap_file=flag, poll_step=0.05)
        await pk
        rows = read(out)
        marks = [r.split("\t")[1] for r in rows if r.startswith("#")]
        snaps = sum(1 for r in rows if not r.startswith("#") and '"snapshot"' in r)
        check(st["resnaps"] == 2 and marks.count("resnap") == 2, "two resubscribes recorded: %s" % marks)
        check(snaps == 3 and conns["n"] == 3, "a fresh snapshot on a fresh connection each time (%d, %d)" % (snaps, conns["n"]))
        check(not os.path.exists(flag), "the request file is consumed")
        check(marks[-1] == "stop" and "give_up" not in marks, "resubscribing does not count towards giving up (max_reconnects=0)")
        # the marker comes before the new snapshot: a reader ends its segment, then starts the next
        order = [("m" if r.startswith("#") else ("s" if '"snapshot"' in r else "u")) for r in rows]
        i = [k for k, r in enumerate(rows) if r.startswith("#") and "resnap" in r][0]
        check("s" in order[i + 1:i + 6] and "u" not in order[i + 1:i + 1 + order[i + 1:].index("s")], "snapshot follows the marker directly")


def snap_msg(bid, ask):
    return json.dumps({"type": "snapshot", "product_id": "BTC-USD", "bids": [["%.2f" % bid, "1"], ["%.2f" % (bid - 5), "2"]],
                       "asks": [["%.2f" % ask, "1"], ["%.2f" % (ask + 5), "2"]]})

def chg(*c):
    return json.dumps({"type": "l2update", "product_id": "BTC-USD", "changes": [list(x) for x in c]})

def drift_unit_test():
    """DriftWatch: the limit is measured from the snapshot's mid, on either side, and nothing odd crashes it."""
    def fresh():
        w = capture_l2.DriftWatch(200)
        check(not w.feed(snap_msg(1000, 1010)), "a snapshot never triggers")  # mid 1005; bids 1000, 995; asks 1010, 1015
        return w
    w = fresh()
    check(not w.feed(chg(("sell", "1010.00", "0"), ("sell", "1015.00", "0"))), "no ask left: no best, no trigger")
    check(not w.feed(chg(("sell", "1205.00", "1"))), "best ask exactly mid + 200: not past the limit")
    check(w.feed(chg(("sell", "1205.00", "0"), ("sell", "1205.01", "1"))), "best ask 1205.01 is 200.01 above the mid: triggers")
    w = fresh()
    check(not w.feed(chg(("buy", "1000.00", "0"), ("buy", "995.00", "0"), ("buy", "805.00", "1"))), "best bid exactly mid - 200: not past the limit")
    check(w.feed(chg(("buy", "805.00", "0"), ("buy", "804.99", "1"))), "best bid 804.99 is 200.01 below the mid: triggers")
    check(not w.feed(snap_msg(805, 815)), "a new snapshot resets the anchor, and does not trigger")
    check(not w.feed(chg(("buy", "805.00", "2"))), "after it the anchor is the new mid (810)")
    # the best moving back inside the limit clears the condition; removing a worse level does not change the best
    w = fresh()
    check(not w.feed(chg(("sell", "1015.00", "0"))), "removing a level behind the best changes nothing")
    check(w.feed(chg(("sell", "1010.00", "0"), ("sell", "1300.00", "1"))), "the asks gone but one at 1300: triggers")
    check(not w.feed(chg(("sell", "1100.00", "1"))), "a new ask at 1100 is the best again, inside the limit")
    for junk in ["not json {", '{"type":"snapshot"', '{"type":"l2update","changes":5}', '{"type":"l2update","changes":[["buy"]]}',
                 '{"type":"heartbeat"}', '{"type":"l2update","changes":[["buy","x","1"]]}', "", json.dumps([1, 2])]:
        check(w.feed(junk) is False, "ignored without raising: %r" % junk)
    w = fresh()
    w.feed(json.dumps({"type": "snapshot", "bids": [["3000.00", "1"]], "asks": []}))
    check(w.mid is None, "a one-sided snapshot after a full one does not keep the old levels: no anchor")
    check(not w.feed(json.dumps({"type": "error", "message": "l2update failed",
                                                                  "changes": [["sell", "1010.00", "0"], ["sell", "1015.00", "0"], ["sell", "9999.00", "1"]]})),
          "only snapshot and l2update messages are read")
    w = fresh()
    check(not w.feed(json.dumps({"type": "error", "message": "l2update failed",
                                                                  "changes": [["sell", "1010.00", "0"], ["sell", "1015.00", "0"], ["sell", "9999.00", "1"]]})),
          "a message of another type with changes in it is not applied")
    w = capture_l2.DriftWatch(200)
    check(not w.feed(chg(("sell", "9999.00", "1"))), "an update before any snapshot is ignored")
    w.feed(json.dumps({"type": "snapshot", "bids": [["1000.00", "1"]], "asks": []}))
    check(not w.feed(chg(("sell", "5000.00", "1"))), "a one-sided snapshot gives no anchor, so no trigger")
    w = capture_l2.DriftWatch(200)
    w.feed(json.dumps({"type": "snapshot", "bids": [["1000.00", "0"], ["990.00", "1"]], "asks": [["1010.00", "1"]]}))  # mid 1000
    check(w.feed(chg(("sell", "1010.00", "0"), ("sell", "1201.00", "1"))), "a zero-size snapshot level is not a level: the mid is 1000")


async def drift_test():
    """A price jump on the feed makes the capture reconnect for a fresh snapshot, with the same marker."""
    conns = {"n": 0}

    async def serve(ws):
        conns["n"] += 1
        n = conns["n"]
        conns.setdefault("t", []).append(time.monotonic())
        await ws.recv()
        base = 1000 if n == 1 else 1600  # the second snapshot is centred on the new price
        await ws.send(snap_msg(base, base + 10))
        try:
            for i in range(4):
                await ws.send(chg(("buy", "%.2f" % (base - 1), "%d" % (i + 1))))
                await asyncio.sleep(0.02)
            if n == 1:
                await ws.send(chg(("sell", "%.2f" % (base + 10), "0"), ("sell", "%.2f" % (base + 15), "0"),
                                      ("sell", "1500.00", "1")))  # the best ask is now 495 above the mid
            await asyncio.sleep(3)
        except Exception:
            pass
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        out = tempfile.mktemp(suffix=".l2")
        st = await capture_l2.capture("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", 1.5, out, 5.0, 0,
                                      poll_step=0.05, drift_usd=200)
        rows = read(out)
        marks = [r.split("\t")[1] for r in rows if r.startswith("#")]
        check(st["resnaps"] == 1 and st["drift_resnaps"] == 1 and marks.count("resnap") == 1, "one drift resubscribe: %s %s" % (st, marks))
        check(conns["n"] == 2 and sum(1 for r in rows if not r.startswith("#") and '"snapshot"' in r) == 2, "a second connection, a second snapshot")
        check("give_up" not in marks and marks[-1] == "stop", "a drift resubscribe does not count towards giving up (max_reconnects=0)")
        check(len(conns["t"]) == 2 and 0.0 < conns["t"][1] - conns["t"][0] < 0.4, "the second connection comes at once, without the 0.5 s back-off (%s)" % conns["t"])
        i = [k for k, r in enumerate(rows) if r.startswith("#") and "resnap" in r][0]
        check("\"snapshot\"" in "".join(rows[i + 1:i + 4]) and "\"snapshot\"" not in "".join(rows[i - 1:i]), "the marker comes right after the jump and before the new snapshot")
    # off by default: the same feed without the option never resubscribes
    conns["n"] = 0
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        out = tempfile.mktemp(suffix=".l2")
        st = await capture_l2.capture("ws://127.0.0.1:%d" % port, "BTC-USD", "level2_batch", 1.0, out, 5.0, 0, poll_step=0.05)
        check(st["resnaps"] == 0 and conns["n"] == 1, "without the option there is no resubscribe")


async def stdout_test():
    """`--out -` streams line by line and stops quietly when the reader goes away."""
    import subprocess
    async def serve(ws):
        await ws.recv()
        await ws.send(SNAP)
        i = 0
        try:
            while True:
                await ws.send(upd(i)); i += 1
                await asyncio.sleep(0.02)
        except Exception:
            pass
    async with websockets.serve(serve, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        env = dict(os.environ, NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
        cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "capture_l2.py"),
               "--url", "ws://127.0.0.1:%d" % port, "--out", "-", "--seconds", "30"]
        p = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, env=env)
        lines = []
        while len(lines) < 12:
            lines.append((await asyncio.wait_for(p.stdout.readline(), 10)).decode())
        check(lines[0].startswith("#") and "\tstart\t" in lines[0], "stream starts with the start marker")
        check(any('"snapshot"' in l for l in lines), "snapshot is streamed")
        check(all(l.endswith("\n") and (l.startswith("#") or "\t{" in l) for l in lines), "whole lines only")
        p.stdout._transport.close()          # the reader goes away
        try:
            await asyncio.wait_for(p.wait(), 10)
            check(True, "capture exits when the reader goes away")
        except asyncio.TimeoutError:
            p.kill()
            check(False, "capture did not exit after the reader went away")
        err = (await p.stderr.read()).decode()
        check("Traceback" not in err and "Error" not in err and "ignored" not in err, "quiet exit when the reader goes away: %s" % err[-300:])


asyncio.run(main())
asyncio.run(verify_test())
asyncio.run(resnap_test())
drift_unit_test()
asyncio.run(drift_test())
asyncio.run(stdout_test())
if failures:
    sys.exit("%d capture test failures" % failures)
print("test_capture: all checks passed")
