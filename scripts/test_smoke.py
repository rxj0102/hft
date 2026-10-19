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

    for mode, what in (("fail", "an unreadable /accounts"), ("nobase", "no entry for the base currency"), ("text", "a balance that is not a number")):
        ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
        ex.accounts_mode = mode
        r = run(ex)
        check(r.returncode == 1 and "FAIL  5 account" in r.stdout and "--position-file would refuse" in r.stdout and ex.open_orders() == [],
              "%s: step 5 fails, says what it means, and nothing was traded" % what)
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

    print("public-only mode: no key, no signed request, no order")
    nokey = {"HFT_CB_KEY": None, "HFT_CB_SECRET": None, "HFT_CB_PASSPHRASE": None}
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    rep = os.path.join(tempfile.mkdtemp(prefix="smk"), "pub.json")
    r = run(ex, ["--public-only", "--report", rep], env=nokey)
    out = r.stdout
    check(r.returncode == 0, "exit 0 with no environment variables at all (%s)" % out[-200:].replace("\n", " | "))
    check(out.count("PASS") == 5 and "FAIL" not in out, "environment, clock, product, ticker and public feed all pass (%d PASS)" % out.count("PASS"))
    check("public feed" in out and "snapshot of 3 bids and 3 asks" in out, "the public snapshot was read and compared with the ticker")
    check(ex.signed_seen == 0 and not ex.requests("POST") and not ex.requests("DELETE"), "no signed request, no POST, no DELETE reached the exchange")
    check(ex.ws_public_subs == 1 and ex.ws_subscriptions == 0, "one public subscription, no user-channel subscription")
    check("NOT tested" in out, "it says what it did not test")
    rj = json.load(open(rep))
    check(rj["extra"]["mode"] == "public-only" and rj["summary"]["failed"] == 0, "the report says public-only")
    check("public_snapshot" in rj["response_shapes"] and "bids" in rj["response_shapes"]["public_snapshot"], "the report holds the shape of the snapshot")
    check(KEY not in open(rep).read() and PASS not in open(rep).read(), "no credential in the report")
    n = len(ex.log)
    r = run(ex, ["--public-only", "--cancel-existing"], env=nokey)
    check(r.returncode == 2 and len(ex.log) == n, "--cancel-existing with --public-only: exit 2, nothing sent")
    r = run(ex, ["--public-only", "--skip-bridge"], env=nokey)
    check(r.returncode == 2, "--skip-bridge with --public-only: exit 2")
    r = run(ex, ["--public-only", "--skip-ws"], env=nokey)
    check(r.returncode == 0 and "PASS  4 public feed  SKIPPED on request" in r.stdout and "FAIL" not in r.stdout, "--skip-ws leaves the feed out, and that is a pass")
    # junk in the environment is not read and not needed
    r = run(ex, ["--public-only"], env={"HFT_CB_SECRET": "not base64 !!"})
    check(r.returncode == 0, "a bad secret in the environment does not matter: it is not read")
    for mode, what in (("auth_only", "without credentials"), ("bad", "not in the"), ("far", "more than 1 percent"), ("silent", "no snapshot"),
                       ("crossed", "not a valid book"), ("locked", "not a valid book"), ("zero", "not a valid book"),
                       ("badside", "more than 1 percent"), ("badbid", "more than 1 percent"), ("noasks", "not in the")):
        ex.ws_public_mode = mode
        extra = ["--public-only", "--timeout", "3"]
        r = run(ex, extra, env=nokey, timeout=60)
        check(r.returncode == 1 and "FAIL  4 public feed" in r.stdout and what in r.stdout, "public feed %s: step 4 fails (%s)" % (mode, what))
    ex.ws_public_mode = "near"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 0 and "PASS  4 public feed" in r.stdout, "a snapshot 0.2 percent from the ticker passes (the tolerance is not zero)")
    ex.ws_public_mode = "ok"
    check(set(map(tuple, ex.ws_public_channels)) == {("level2_batch",)}, "it subscribed to the level2_batch channel and nothing else, without a key")
    r = run(ex, ["--public-only", "--channel", "ticker"], env=nokey)
    check(r.returncode == 0 and ["ticker"] in ex.ws_public_channels, "--channel picks another public channel")
    sys.path.insert(0, HERE)
    import smoke_sandbox  # noqa: E402
    import order_bridge  # noqa: E402
    before = ex.signed_seen
    a = smoke_sandbox.Api(order_bridge.Client(ex.url, KEY, SECRET, PASS, 5, True), public=True)
    st, js = a.call("t", "GET", "/orders?product_id=BTC-USD&status=open")
    check(st is None and "refused" in js and ex.signed_seen == before, "the Api guard refuses a signed call in public mode, before it is sent")
    st, js = smoke_sandbox.Api(order_bridge.Client(ex.url, KEY, SECRET, PASS, 5, True)).call("t", "GET", "/orders?product_id=BTC-USD&status=open")
    check(st == 200 and ex.signed_seen == before + 1, "and the same call goes out in the normal mode")
    ex.product_status = "offline"
    subs = ex.ws_public_subs
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "FAIL  3 product" in r.stdout and ex.ws_public_subs == subs, "a product that is not online stops a public run at step 3")
    ex.product_status = "online"
    ex.ticker_bad = True
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "FAIL  4 ticker" in r.stdout and ex.ws_public_subs == subs, "a ticker of zeros stops a public run at step 4, before the feed")
    ex.ticker_bad = False
    r = run(ex, ["--public-only", "--ws-url", "ws://127.0.0.1:1", "--timeout", "3"], env=nokey)
    check(r.returncode == 1 and "FAIL  4 public feed" in r.stdout and "connection failed" in r.stdout, "a feed that cannot be reached fails step 4")
    proxy = {"HTTP_PROXY": "http://127.0.0.1:9", "http_proxy": "http://127.0.0.1:9", "ALL_PROXY": "http://127.0.0.1:9", "all_proxy": "http://127.0.0.1:9",
             "HTTPS_PROXY": "http://127.0.0.1:9", "https_proxy": "http://127.0.0.1:9", "NO_PROXY": "", "no_proxy": ""}
    r = run(ex, ["--public-only"], env=dict(nokey, **proxy))
    check(r.returncode == 0, "against the local fake an environment proxy is not used (REST and websocket)")
    # the Api that public_only builds refuses a signed call; reach it through a patched check
    import types
    seen = []
    orig = smoke_sandbox.check_clock
    smoke_sandbox.check_clock = lambda api: (seen.append(api.call("x", "GET", "/orders?product_id=BTC-USD&status=open")), False)[1]
    n_signed = ex.signed_seen
    ns = types.SimpleNamespace(test_exchange=True, timeout=5.0, product="BTC-USD", skip_ws=True, channel="level2_batch", report=None)
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        smoke_sandbox.public_only(ns, ex.url, None)
    smoke_sandbox.check_clock = orig
    check(len(seen) == 1 and seen[0][0] is None and "refused" in seen[0][1] and ex.signed_seen == n_signed,
          "the Api that public_only builds refuses a signed call")
    ex.skew = 60.0
    subs = ex.ws_public_subs
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "FAIL  2 clock" in r.stdout and ex.ws_public_subs == subs, "a clock 60 s off stops a public run at step 2, before the feed")
    ex.skew = 0.0
    ex.stop()
    r = subprocess.run([sys.executable, SMOKE, "--public-only", "--base-url", "https://api.exchange.coinbase.com"], capture_output=True, text=True, env=dict(os.environ, **{k: "" for k in nokey}))
    check(r.returncode == 2 and "refusing" in r.stderr, "the production URL is still refused in public-only mode")
    print("the product reply as the real sandbox sends it (no base_min_size)")
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start().start_ws()
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 0 and "no base_min_size in the reply" in r.stdout and "base_increment=0.00000001" in r.stdout
          and "quote_increment=0.01" in r.stdout and "tradable" in r.stdout, "a reply without base_min_size passes, shows the real values, and says the minimum is unknown")
    ex.product_min_size = "0.00001"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 0 and "base_min_size=0.00001" in r.stdout and "no base_min_size" not in r.stdout, "with base_min_size present it is shown and used")
    ex.product_min_size = "0.001"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "FAIL  3 product" in r.stdout and "DO NOT FIT" in r.stdout, "a minimum size above the bridge's 1e-5 lots: DO NOT FIT")
    ex.product_min_size = "0.0001"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 0, "a minimum size of exactly 0.0001 fits (the limit)")
    ex.product_min_size = "0.00011"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "DO NOT FIT" in r.stdout, "a minimum size just above 0.0001 does not fit")
    ex.product_min_size = None
    ex.product_base_increment = "0.00003"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "DO NOT FIT" in r.stdout, "a size step that 1e-5 is not a multiple of: DO NOT FIT")
    ex.product_base_increment = "0.00000001"
    ex.product_quote_increment = "0.05"
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "DO NOT FIT" in r.stdout, "a price step that a cent is not a multiple of: DO NOT FIT")
    ex.product_quote_increment = "0.01"
    ex.product_http = 503
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "FAIL  3 product" in r.stdout, "HTTP 503 with a well-formed body still fails step 3")
    ex.product_http = 200
    for flag in ("cancel_only", "auction_mode", "trading_disabled"):
        ex.product_flags = {flag: True}
        r = run(ex, ["--public-only"], env=nokey)
        check(r.returncode == 1 and "FAIL  3 product" in r.stdout and "NOT TRADABLE" in r.stdout and ("%s=True" % flag) in r.stdout,
              "%s: step 3 fails as not tradable, and the flag is shown" % flag)
    ex.product_flags = {"limit_only": True}
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 0 and "limit_only=True" in r.stdout, "limit_only is fine (the bridge sends limit orders), and it is shown")
    ex.product_flags = {}
    ex.product_broken = True
    r = run(ex, ["--public-only"], env=nokey)
    check(r.returncode == 1 and "unexpected reply" in r.stdout and "quote_increment=0.01" in r.stdout and "FAIL  3 product" in r.stdout,
          "a reply without base_increment fails and still shows the values it has")
    ex.product_broken = False
    r = run(ex)  # the full run (with keys) on the same reply shape
    check(r.returncode == 0, "the full run works with a reply that has no base_min_size")
    ex.stop()
    print("test_smoke: all checks passed" if not fails else "test_smoke: %d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
