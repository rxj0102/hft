#!/usr/bin/env python3
"""Tests of scripts/smoke_sandbox.py against the local fake exchange. No network, no real key.

  python3 scripts/test_smoke.py
"""
import base64
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fake_exchange  # noqa: E402

SMOKE = os.path.join(HERE, "smoke_sandbox.py")
KEY, PASS = "key-SmOkE-123", "pass-SmOkE-456"
SECRET = base64.b64encode(b"smoke-secret-bytes-0123456789-abcdef").decode()
fails = 0


def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails += 1


def run(ex, extra=(), env=None, url=True, timeout=120):
    e = dict(os.environ)
    e.update({"HFT_CB_KEY": KEY, "HFT_CB_SECRET": SECRET, "HFT_CB_PASSPHRASE": PASS})
    e.update(env or {})
    for k in [k for k, v in e.items() if v is None]:
        del e[k]
    cmd = [sys.executable, SMOKE] + (["--test-exchange", "--base-url", ex.url] if url else []) + (["--ws-url", ex.ws_url] if url and ex.ws_port else []) + list(extra)
    return subprocess.run(cmd, capture_output=True, text=True, env=e, timeout=timeout)


def main():
    print("a healthy exchange")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    rep = os.path.join(tempfile.mkdtemp(prefix="smk"), "r.json")
    r = run(ex, ["--report", rep])
    check(r.returncode == 0, "exit 0 (%s)" % r.stdout[-300:].replace("\n", " | "))
    steps = [l.split()[1] for l in r.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]
    check(r.stdout.count("PASS") >= 17 and "FAIL" not in r.stdout, "every step passes: %d PASS" % r.stdout.count("PASS"))
    check(ex.open_orders() == [], "the exchange is flat afterwards")
    posts = ex.requests("POST", "/orders")
    check(len(posts) >= 6, "orders were placed (%d POSTs)" % len(posts))
    cross = [p for p in ex.posted if p[0] > ex.best_ask]
    rest = [p for p in ex.posted if p[0] <= ex.best_ask]
    check(len(cross) == 1 and len(rest) == len(ex.posted) - 1, "exactly one order is above the ask (the crossing test)")
    check(all(p[0] <= ex.best_bid * 0.5 + 0.01 for p in rest), "every other order is at half the bid or lower (largest %.2f, bid %.2f)" % (max(p[0] for p in rest), ex.best_bid))
    check(all(p[2] for p in ex.posted) and all(abs(p[1] - 0.00001) < 1e-12 for p in ex.posted), "all post-only, all the minimum size")
    blob = open(rep).read() + r.stdout + r.stderr
    check(KEY not in blob and SECRET not in blob and PASS not in blob, "no key, secret or passphrase in the output or the report")
    d = json.load(open(rep))
    check(d["summary"]["failed"] == 0 and d["summary"]["calls"] >= 15 and "latency_ms_p50" in d["summary"], "report has the summary and latencies")
    check(d["response_shapes"].get("order", {}).get("filled_size") == "str", "report records the response shapes")
    ex.stop()

    print("an exchange that misbehaves")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.cancel_noop = True
    r = run(ex)
    check(r.returncode == 1 and "FAIL  9 cancel" in r.stdout and ex.open_orders() == [] and "cleanup" in r.stdout,
          "a cancel that does nothing: step 9 fails, and the cleanup leaves the exchange flat")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.omit_filled_size = True
    r = run(ex)
    check(r.returncode == 1 and "filled_size field MISSING" in r.stdout, "orders without filled_size: step 7 fails and says why")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.cross_http = 500
    r = run(ex)
    check(r.returncode == 1 and "FAIL  8 cross" in r.stdout and ex.open_orders() == [], "a 500 on the crossing order is not a refusal")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.fail_post_number = 8   # the bridge's replace
    r = run(ex)
    check(r.returncode == 1 and "FAIL 12 bridge" in r.stdout and ex.open_orders() == [], "a replace that the exchange refuses: the bridge step fails")
    ex.stop()
    base = fake_exchange.FakeExchange(KEY, SECRET, PASS).start()
    run(base, ["--skip-ws"])
    k_requests = base.nreq   # without the socket the request count is fixed (the socket makes the bridge sync more often)
    base.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start()
    ex.ghost_at = k_requests
    r = run(ex, ["--skip-ws"])
    check(r.returncode == 1 and "FAIL 13 flat" in r.stdout, "an order that appears before the last check: step 13 fails (stray %s)" % [o["id"] for o in ex.open_orders()])
    ex.stop()

    print("the websocket steps")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.ws_mode = "reject_auth"
    r = run(ex)
    check(r.returncode == 1 and "FAIL  6 websocket" in r.stdout and "Authentication Failed" in r.stdout and "PASS 13 flat" in r.stdout,
          "a refused socket fails its own step, shows the exchange's message, and the REST steps still run")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.ws_silent = True
    r = run(ex)
    check(r.returncode == 1 and "FAIL  7 ws events" in r.stdout and "PASS  9 cancel" in r.stdout, "a socket that subscribes but delivers nothing: step 7 says so")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.ws_cancel_reason = "filled"
    r = run(ex)
    check(r.returncode == 1 and "FAIL  9 ws events" in r.stdout, "a done event that is not a cancel after our cancel: step 9 fails")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    r = run(ex, ["--skip-ws"])
    check(r.returncode == 0 and "websocket fills off" in r.stdout and "6 websocket" not in r.stdout and not ex.ws_subscriptions, "--skip-ws: no socket is opened")
    r = run(ex)
    check(r.returncode == 0 and "websocket fills on" in r.stdout and ex.ws_subscriptions >= 2, "with the socket: the bridge step runs with --ws-fills")
    ex.stop()
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start()
    r = run(ex, ["--ws-url", "ws://127.0.0.1:9"])   # nothing listens there
    check(r.returncode == 1 and "FAIL  6 websocket" in r.stdout, "an unreachable socket fails step 6")
    ex.stop()

    print("refusals before any request")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    r = run(ex, url=False, env={"HTTPS_PROXY": "http://127.0.0.1:9", "https_proxy": "http://127.0.0.1:9"}, extra=["--base-url", "https://api.exchange.coinbase.com"])
    check(r.returncode == 2 and "refusing base URL" in r.stderr, "the production URL is refused (exit 2)")
    r = run(ex, env={"HFT_CB_KEY": None})
    check(r.returncode == 2 and "HFT_CB_KEY" in r.stderr and not ex.requests(), "a missing variable: exit 2, no request sent")
    r = run(ex, env={"HFT_CB_SECRET": "not base64!!"})
    check(r.returncode == 2 and not ex.requests(), "a secret that is not base64: exit 2")
    r = run(ex, ["--ws-url", "wss://ws-feed.exchange.coinbase.com"], url=True)
    check(r.returncode == 2 and "only allows ws://" in r.stderr and not ex.requests(), "a production websocket URL in test mode: exit 2, no request sent")
    r = run(ex, ["--max-notional", "500"])
    check(r.returncode == 2 and not ex.requests(), "a notional cap over 50 USD: exit 2")
    ex.stop()

    print("things going wrong")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.mode = "auth"
    r = run(ex)
    check(r.returncode == 1 and "FAIL  5 auth" in r.stdout and not ex.requests("POST"), "bad credentials stop at step 5, nothing placed")
    ex.mode = "ok"
    ex.skew = 60.0
    r = run(ex)
    check(r.returncode == 1 and "FAIL  2 clock" in r.stdout and not ex.requests("POST"), "a clock 60 s off stops at step 2")
    ex.skew = 0.0
    ex.stop()

    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    with ex.lock:
        ex.orders["pre"] = {"id": "pre", "client_oid": "pre", "side": "buy", "price": "1.00", "size": "0.1", "product_id": "BTC-USD",
                            "filled_size": "0", "executed_value": "0"}
    r = run(ex)
    check(r.returncode == 1 and "FAIL  6 flat" in r.stdout and "pre" in {o["id"] for o in ex.open_orders()},
          "an order already working: refused, and left alone")
    r = run(ex, ["--cancel-existing"])
    check(r.returncode == 0, "with --cancel-existing it proceeds")
    ex.stop()

    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.honour_post_only = False
    r = run(ex)
    check(r.returncode == 1 and "ACCEPTED" in r.stdout and ex.open_orders() == [], "a crossing order that is accepted: fail, and cancelled")
    ex.stop()

    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    ex.best_bid, ex.best_ask = 800000.0, 800000.1
    r = run(ex, ["--max-notional", "1"])
    check(r.returncode == 1 and "max-notional" in r.stdout and not ex.requests("POST"), "an order over the notional cap: nothing placed")
    ex.stop()

    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    r = run(ex, ["--skip-bridge"])
    check(r.returncode == 0 and "skipped" in r.stdout, "--skip-bridge")
    ex.stop()
    print("test_smoke: all checks passed" if not fails else "test_smoke: %d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
