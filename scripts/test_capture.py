#!/usr/bin/env python3
"""Tests capture_l2.py against a local fake server: normal flow, an abrupt
disconnect, a silent connection (idle timeout), and give-up after too many
reconnects. No network access needed."""
import asyncio, json, os, sys, tempfile
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

asyncio.run(main())
asyncio.run(verify_test())
if failures:
    sys.exit("%d capture test failures" % failures)
print("test_capture: all checks passed")
