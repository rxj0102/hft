#!/usr/bin/env python3
"""Smoke test of the order bridge against the Coinbase Exchange SANDBOX. Run it yourself, with your own key.

  export HFT_CB_KEY=...  HFT_CB_SECRET=...  HFT_CB_PASSPHRASE=...      (a sandbox key, trade permission, a profile used only for this)
  python3 scripts/smoke_sandbox.py --report smoke_report.json

What it does, in order, and stops at the first step that breaks the next one:
  1 environment   the three variables exist and the secret is base64 (values are never printed)
  2 clock         GET /time against the local clock
  3 product       GET /products/BTC-USD: minimum size, increments, status; compared with what the bridge assumes
  4 ticker        GET /products/BTC-USD/ticker: a bid and an ask
  5 auth          signed GET /orders?status=open (the signature, headers and passphrase are accepted)
  6 flat          nothing is working on the product (refuses to continue otherwise, unless --cancel-existing)
  6w websocket    (needs the `websockets` package; --skip-ws to leave it out) the authenticated `user` channel accepts the subscription
  7 place         one post-only buy at half the bid, minimum size; reply has an id; it shows in the open list and in GET /orders/{id}
  8 cross         one post-only buy above the ask: must be refused, and the message is recorded (the bridge reads it to name the reason)
  9 cancel        DELETE /orders/{id}; GET /orders/{id} then says done/canceled
 10 client id     place again, cancel with DELETE /orders/client:{client_oid}
 11 cancel all    DELETE /orders?product_id=, then the open list is empty
 7w ws events    the websocket delivers `received`/`open` for the order of step 7, and 9w `done` (canceled) for step 9
 12 bridge        the real bridge (--loop, with --ws-fills when the websocket worked) as a child process: new, replace, cancel, end;
                  reports must be accepted, replaced, canceled
 13 flat          nothing is working at the end
Latency of every call is measured. The report file holds step results, response field names, error messages and latencies. It holds
no key, secret, passphrase, signature or account id, so you can share it.

Safety: only the sandbox URL is accepted (the check is the bridge's own); every order is the minimum size with a notional capped by
--max-notional (default 5 USD, hard limit 50); the only order that could trade is the cross test, which is post-only and is refused
by a working exchange; if it is accepted anyway the script says so, cancels it, and stops.
Exit code: 0 all steps passed, 1 a step failed, 2 usage or keys.
"""
import argparse
import base64
import json
import os
import statistics
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import order_bridge as ob  # noqa: E402

results, lat, shapes = [], [], {}


class ListLog:
    """Collects the events a WsFeed would write to the bridge's log."""

    def __init__(self):
        self.events = []

    def emit(self, event, **kw):
        self.events.append((event, kw))

touched = {"api": None, "product": None}  # set once the first order is placed, so that any failure cleans up


def step(n, name, ok, detail):
    results.append({"step": n, "name": name, "ok": bool(ok), "detail": detail})
    print("%s %2d %-12s %s" % ("PASS" if ok else "FAIL", n, name, detail), flush=True)
    return ok


def shape(js):
    """Field names and types only: enough to see the API's shape, nothing that identifies an account."""
    if isinstance(js, dict):
        return {k: type(v).__name__ for k, v in sorted(js.items()) if k not in ("id", "profile_id", "client_oid")}
    if isinstance(js, list):
        return ["list of %d" % len(js)] + ([shape(js[0])] if js else [])
    return type(js).__name__


class Api:
    def __init__(self, client, public=False):
        self.c = client
        self.public = public  # public-only mode: a signed request is refused here, before it is built

    def call(self, label, method, path, body=None, signed=True):
        if self.public and signed:
            return None, {"refused": "signed request in public-only mode"}
        t = time.monotonic()
        try:
            st, js, _ = self.c.call(method, path, body, signed)
        except OSError as e:
            lat.append((label, None))
            return None, {"network_error": type(e).__name__}
        dt = time.monotonic() - t
        lat.append((label, dt))
        time.sleep(0.12)  # about 8 requests a second at most, under the documented limit of 15
        return st, js

    def msg(self, js):
        return str(js.get("message", ""))[:160] if isinstance(js, dict) else ""


def finish(code, args, extra=None):
    if touched["api"] and code != 0:  # a failed run must not leave orders behind
        st, _ = touched["api"].call("cleanup", "DELETE", "/orders?product_id=" + touched["product"])
        print("cleanup: cancel-all on %s -> HTTP %s" % (touched["product"], st))
    ts = [d for _, d in lat if d is not None]
    summary = {"passed": sum(r["ok"] for r in results), "failed": sum(not r["ok"] for r in results), "calls": len(lat),
               "network_errors": sum(d is None for _, d in lat)}
    if ts:
        ts.sort()
        summary.update({"latency_ms_p50": round(1000 * statistics.median(ts)), "latency_ms_max": round(1000 * ts[-1]),
                        "latency_ms_p90": round(1000 * ts[min(len(ts) - 1, int(0.9 * len(ts)))])})
    print("\nsummary:", json.dumps(summary))
    if args.report:
        with open(args.report, "w") as f:
            json.dump({"when": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "base": args.base_url if args.test_exchange else "sandbox",
                       "steps": results, "response_shapes": shapes, "summary": summary, "extra": extra or {}}, f, indent=1)
        print("report written to", args.report)
    return code


def check_clock(api):
    st, js = api.call("time", "GET", "/time", signed=False)
    if not (st == 200 and isinstance(js, dict) and "epoch" in js):
        step(2, "clock", False, "GET /time gave %s %s" % (st, shape(js)))
        return False
    shapes["time"] = shape(js)
    skew = float(js["epoch"]) - time.time()
    return step(2, "clock", abs(skew) < 5, "local clock is %.2f s from the exchange's (bridge stops at 20 s, exchange refuses at 30 s)" % skew)


def check_product(api, P):
    """Returns (ok, minimum size)."""
    st, js = api.call("product", "GET", "/products/" + P, signed=False)
    shapes["product"] = shape(js)
    try:
        minsz, binc, qinc = float(js["base_min_size"]), float(js["base_increment"]), float(js["quote_increment"])
        online = js.get("status") == "online" and not js.get("trading_disabled", False)
        # the bridge sends sizes in 1e-5 steps and prices in cents
        fits = minsz <= 0.0001 and abs(0.00001 / binc - round(0.00001 / binc)) < 1e-9 and abs(0.01 / qinc - round(0.01 / qinc)) < 1e-9
        ok = st == 200 and online and fits
        detail = "min size %s, size step %s, price step %s, status %s; the bridge's 1e-5 sizes and cent prices %s" % (
            js["base_min_size"], js["base_increment"], js["quote_increment"], js.get("status"), "fit" if fits else "DO NOT FIT")
    except (KeyError, TypeError, ValueError):
        ok, detail, minsz = False, "unexpected reply %s %s" % (st, shape(js)), 0.0
    return step(3, "product", ok, detail), minsz


def check_ticker(api, P):
    """Returns (ok, bid, ask)."""
    st, js = api.call("ticker", "GET", "/products/%s/ticker" % P, signed=False)
    shapes["ticker"] = shape(js)
    try:
        bid, ask = float(js["bid"]), float(js["ask"])
        ok = st == 200 and 0 < bid <= ask
        detail = "bid %.2f ask %.2f" % (bid, ask)
    except (KeyError, TypeError, ValueError):
        ok, detail, bid, ask = False, "unexpected reply %s %s" % (st, shape(js)), 0.0, 0.0
    return step(4, "ticker", ok, detail), bid, ask


def check_public_feed(ws_url, P, channel, bid, ask, test, seconds=10.0):
    """Subscribe to the public level-2 channel without credentials and read the first snapshot.
    Returns (ok, detail). The snapshot must have both sides in [price, size] form and sit near the REST ticker."""
    import asyncio

    import websockets

    async def go():
        kw = {"proxy": None} if test else {}
        got = {"errors": [], "types": []}
        async with websockets.connect(ws_url, open_timeout=10, max_size=1 << 24, **kw) as ws:
            await ws.send(json.dumps({"type": "subscribe", "product_ids": [P], "channels": [channel]}))
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=max(end - time.monotonic(), 0.01))
                except asyncio.TimeoutError:
                    break
                try:
                    m = json.loads(raw)
                except ValueError:
                    continue
                if not isinstance(m, dict):
                    continue
                got["types"].append(str(m.get("type")))
                if m.get("type") == "error":
                    got["errors"].append("%s / %s" % (str(m.get("message"))[:100], str(m.get("reason"))[:100]))
                    return got
                if m.get("type") == "snapshot":
                    got["snapshot"] = m
                    return got
        return got

    try:
        got = asyncio.run(go())
    except Exception as e:  # noqa: BLE001  any failure is a result to report
        return False, "connection failed: %s %s" % (type(e).__name__, str(e)[:100])
    if got["errors"]:
        return False, "the exchange refused the subscription to %s without credentials: %s" % (channel, got["errors"][0])
    snap = got.get("snapshot")
    if snap is None:
        return False, "no snapshot within %.0f s on %s; message types seen %s" % (seconds, channel, sorted(set(got["types"])))
    shapes["public_snapshot"] = shape(snap)
    try:
        bids = [(float(p), float(q)) for p, q in snap["bids"]]
        asks = [(float(p), float(q)) for p, q in snap["asks"]]
        bb, ba = max(bids)[0], min(asks)[0]
        ok = 0 < bb < ba and all(p > 0 and q > 0 for p, q in bids + asks)  # max/min above raise on an empty side
    except (KeyError, TypeError, ValueError):
        return False, "the snapshot is not in the [price, size] shape the capture reads: fields %s" % shape(snap)
    if not ok:
        return False, "snapshot best bid %s, best ask %s: not a valid book" % (bb, ba)
    mid_rest = (bid + ask) / 2.0
    near = abs(bb - bid) <= 0.01 * mid_rest and abs(ba - ask) <= 0.01 * mid_rest  # each side, so a book too wide around the right mid fails
    return near, "snapshot of %d bids and %d asks, best %.2f / %.2f, REST ticker %.2f / %.2f%s" % (
        len(bids), len(asks), bb, ba, bid, ask, "" if near else " (more than 1 percent apart)")


def public_only(args, base, ws_url):
    """Everything that needs no key: no environment variables are read, no signed request is made, no order is placed."""
    api = Api(ob.Client(base, "", "", "", args.timeout, args.test_exchange), public=True)
    step(1, "environment", True, "public-only: no credentials read, no signed request, no order; target %s" % ("local fake" if args.test_exchange else "the sandbox"))
    if not check_clock(api):
        return finish(1, args, {"mode": "public-only"})
    ok, _ = check_product(api, args.product)
    if not ok:
        return finish(1, args, {"mode": "public-only"})
    ok, bid, ask = check_ticker(api, args.product)
    if not ok:
        return finish(1, args, {"mode": "public-only"})
    if args.skip_ws:
        step(4, "public feed", True, "SKIPPED on request")
        results[-1]["skipped"] = True
    else:
        try:
            import websockets  # noqa: F401
        except ImportError:
            step(4, "public feed", True, "SKIPPED: the websockets package is not installed (pip install websockets)")
            results[-1]["skipped"] = True
        else:
            ok, detail = check_public_feed(ws_url, args.product, args.channel, bid, ask, args.test_exchange, seconds=args.timeout)
            if not step(4, "public feed", ok, detail):
                return finish(1, args, {"mode": "public-only", "channel": args.channel})
    print("\npublic checks done. Orders, the signature, the account and the user channel were NOT tested: they need a key.")
    return finish(0, args, {"mode": "public-only", "channel": args.channel})



def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", default=ob.SANDBOX_URL)
    ap.add_argument("--test-exchange", action="store_true", help="local fake exchange only (http://127.0.0.1:PORT)")
    ap.add_argument("--product", default="BTC-USD")
    ap.add_argument("--max-notional", type=float, default=5.0)
    ap.add_argument("--cancel-existing", action="store_true", help="allow step 6 to cancel orders already working on the product")
    ap.add_argument("--ws-url", default=ob.WS_SANDBOX)
    ap.add_argument("--skip-ws", action="store_true", help="skip the websocket steps")
    ap.add_argument("--skip-bridge", action="store_true", help="skip step 12")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--public-only", action="store_true", help="no key needed: clock, product, ticker and the public level-2 snapshot; no signed request, no order")
    ap.add_argument("--channel", default="level2_batch", help="public channel for --public-only")
    ap.add_argument("--report", default=None)
    args = ap.parse_args(argv)
    if not (0 < args.max_notional <= 50):
        print("smoke: --max-notional must be in (0, 50]", file=sys.stderr)
        return 2

    # 1 ---------------------------------------------------------------------------------------------------------------
    try:
        base = ob.check_url(args.base_url, args.test_exchange)
    except ob.Fatal as e:
        print("smoke: " + str(e), file=sys.stderr)
        return 2
    ws_url = None
    if not args.skip_ws:
        try:
            ws_url = ob.check_ws_url(args.ws_url, args.test_exchange)
        except ob.Fatal as e:
            print("smoke: " + str(e), file=sys.stderr)
            return 2
    if args.public_only:
        if args.cancel_existing or args.skip_bridge:
            print("smoke: --public-only places nothing, so --cancel-existing and --skip-bridge do not apply", file=sys.stderr)
            return 2
        return public_only(args, base, ws_url)
    missing = [k for k in ob.ENV_KEYS if not os.environ.get(k)]
    if missing:
        print("smoke: missing environment variables: " + ", ".join(missing), file=sys.stderr)
        return 2
    key, secret, passphrase = (os.environ[k] for k in ob.ENV_KEYS)
    try:
        base64.b64decode(secret, validate=True)
    except ValueError:
        print("smoke: HFT_CB_SECRET is not base64", file=sys.stderr)
        return 2
    step(1, "environment", True, "three variables present, secret is base64, target %s" % ("local fake" if args.test_exchange else "the sandbox"))
    api = Api(ob.Client(base, key, secret, passphrase, args.timeout, args.test_exchange))
    P = args.product

    # 2 ---------------------------------------------------------------------------------------------------------------
    if not check_clock(api):
        return finish(1, args)

    # 3 ---------------------------------------------------------------------------------------------------------------
    ok, minsz = check_product(api, P)
    if not ok:
        return finish(1, args)
    size = max(minsz, 0.00001)
    size_s = "%.5f" % (round(size / 0.00001 + 0.4999) * 0.00001)

    # 4 ---------------------------------------------------------------------------------------------------------------
    ok, bid, ask = check_ticker(api, P)
    if not ok:
        return finish(1, args)
    far = int(bid * 0.5 * 100) / 100.0
    notional = far * float(size_s)
    near_cross = int(ask * 1.01 * 100) / 100.0 + 0.01
    if notional > args.max_notional or near_cross * float(size_s) > args.max_notional:
        step(4, "ticker", False, "an order would be worth %.2f USD, over --max-notional %.2f; the sandbox book looks unusual" % (
            max(notional, near_cross * float(size_s)), args.max_notional))
        return finish(1, args)

    # 5 ---------------------------------------------------------------------------------------------------------------
    st, js = api.call("auth", "GET", "/orders?product_id=%s&status=open" % P)
    shapes["open_orders"] = shape(js)
    if not step(5, "auth", st == 200 and isinstance(js, list), (
            "signed request accepted, HTTP %s, %s" % (st, shape(js)) if st == 200 else
            "HTTP %s: %s (key, secret, passphrase, permission or an IP allow-list?)" % (st, api.msg(js)))):
        return finish(1, args)

    # 5b: the account (the engine compares its position file with the base currency's balance, ADR 31) -------------------
    st_a, acc = api.call("accounts", "GET", "/accounts")
    base_cur = P.split("-")[0]
    row = next((r for r in acc if isinstance(r, dict) and r.get("currency") == base_cur), None) if isinstance(acc, list) else None
    shapes["account_row"] = shape(row) if row else None
    try:
        float(row["balance"])
        numeric = True
    except (TypeError, KeyError, ValueError):
        numeric = False
    if not step(5, "account", numeric,   # numeric implies an entry in a list, which implies a 200
                ("GET /accounts HTTP 200, a %s entry with a numeric balance (fields %s)" % (base_cur, sorted(row.keys())) if row and numeric else
                 "GET /accounts HTTP %s, %s entry %s: --position-file would refuse to start" % (st_a, base_cur, "found, balance not numeric" if row else "NOT found"))):
        return finish(1, args)

    # 6 ---------------------------------------------------------------------------------------------------------------
    if js:
        if not args.cancel_existing:
            step(6, "flat", False, "%d orders already working on %s; run with --cancel-existing to let this script cancel them" % (len(js), P))
            return finish(1, args)
        api.call("cancel_all", "DELETE", "/orders?product_id=" + P)
        st, js = api.call("open", "GET", "/orders?product_id=%s&status=open" % P)
    if not step(6, "flat", st == 200 and js == [], "nothing working on %s" % P):
        return finish(1, args)

    # 6w --------------------------------------------------------------------------------------------------------------
    feed = wl = None
    ws_ok = False
    if not args.skip_ws:
        try:
            import websockets  # noqa: F401
        except ImportError:
            step(6, "websocket", True, "SKIPPED: the websockets package is not installed (pip install websockets); the bridge will run without --ws-fills")
            results[-1]["skipped"] = True
        else:
            wl = ListLog()
            feed = ob.WsFeed(ws_url, P, key, secret, passphrase, wl, args.test_exchange)
            feed.start()
            ws_ok = feed.subscribed.wait(10)
            errs = [kw.get("message") for e, kw in wl.events if e == "ws_error"]
            fails = [kw.get("detail") for e, kw in wl.events if e == "ws_failed"]
            step(6, "websocket", ws_ok, "subscribed to the user channel" if ws_ok else
                 "no subscription within 10 s; errors %s, connection failures %s" % (errs, fails))

    def ws_types(oid):
        return [m.get("type") for m in list(feed.messages) if m.get("order_id") == oid] if feed else []

    def ws_wait(oid, want, seconds=6.0):
        t = time.monotonic()
        while time.monotonic() - t < seconds:
            if want(ws_types(oid)):
                return True
            time.sleep(0.1)
        return False

    def place(price, label, coid=None, post_only=True):
        coid = coid or str(uuid.uuid4())
        body = {"type": "limit", "side": "buy", "product_id": P, "price": "%.2f" % price, "size": size_s, "post_only": post_only,
                "client_oid": coid}
        st, js = api.call(label, "POST", "/orders", body)
        return st, js, coid

    # 7 ---------------------------------------------------------------------------------------------------------------
    touched["api"], touched["product"] = api, P  # from here on a failure cancels what is working (step 6 showed there was nothing else)
    st, js, coid = place(far, "place")
    shapes["post_reply"] = shape(js)
    oid = js.get("id") if isinstance(js, dict) else None
    if not step(7, "place", st == 200 and oid, "post-only buy %s at %.2f -> HTTP %s %s" % (size_s, far, st, api.msg(js) or "accepted")):
        return finish(1, args)
    st, js = api.call("open", "GET", "/orders?product_id=%s&status=open" % P)
    listed = st == 200 and isinstance(js, list) and any(o.get("id") == oid for o in js)
    st2, one = api.call("get_order", "GET", "/orders/" + oid)
    shapes["order"] = shape(one)
    fields = isinstance(one, dict) and "filled_size" in one
    if not step(7, "place", listed and st2 == 200 and fields, "it shows in the open list: %s; GET /orders/{id}: HTTP %s, filled_size field %s" % (
            listed, st2, "present" if fields else "MISSING (the closed loop needs it)")):
        return finish(1, args)

    if ws_ok:
        seen = ws_wait(oid, lambda t: "open" in t or "received" in t)
        ex_msg = next((m for m in list(feed.messages) if m.get("order_id") == oid), None)
        shapes["ws_order_event"] = shape(ex_msg) if ex_msg else None
        step(7, "ws events", seen, "the socket delivered %s for the new order" % ws_types(oid) if seen else
             "nothing for the new order within 6 s (types seen for it: %s; all messages: %d)" % (ws_types(oid), len(feed.messages)))

    # 8 ---------------------------------------------------------------------------------------------------------------
    st, js, coid_x = place(near_cross, "cross")
    msg = api.msg(js)
    refused = st is not None and 400 <= st < 500
    if st == 200:
        api.call("cancel_all", "DELETE", "/orders?product_id=" + P)
        step(8, "cross", False, "a crossing post-only order was ACCEPTED (post_only not honoured?); cancelled it; stopping here")
        return finish(1, args)
    named = "post only" in msg.lower()
    step(8, "cross", refused, "refused with HTTP %s, message %r; the bridge %s it as a cross" % (st, msg, "recognises" if named else "does NOT recognise"))
    if not refused:
        return finish(1, args)
    results[-1]["bridge_recognises_cross_message"] = named

    # 9 ---------------------------------------------------------------------------------------------------------------
    st, js = api.call("cancel", "DELETE", "/orders/" + oid)
    st2, one = api.call("get_order", "GET", "/orders/" + oid)
    done = isinstance(one, dict) and one.get("done_reason") == "canceled" and "filled_size" in one
    if not step(9, "cancel", st == 200 and st2 == 200 and done, "DELETE HTTP %s; afterwards HTTP %s, status %s, done_reason %s, filled_size %s (the bridge reads it after every cancel)" % (
            st, st2, one.get("status") if isinstance(one, dict) else None, one.get("done_reason") if isinstance(one, dict) else None,
            "present" if isinstance(one, dict) and "filled_size" in one else "MISSING")):
        return finish(1, args)

    if ws_ok:
        done = ws_wait(oid, lambda t: "done" in t)
        d = next((m for m in list(feed.messages) if m.get("order_id") == oid and m.get("type") == "done"), None)
        shapes["ws_done_event"] = shape(d) if d else None
        step(9, "ws events", done and d.get("reason") == "canceled", "after the cancel the socket delivered %s" % ws_types(oid) if done else
             "no done event within 6 s (%s)" % ws_types(oid))

    # 10 --------------------------------------------------------------------------------------------------------------
    st, js, coid = place(far, "place")
    oid2 = js.get("id") if isinstance(js, dict) else None
    st2, js2 = api.call("cancel_coid", "DELETE", "/orders/client:" + coid)
    st3, lst = api.call("open", "GET", "/orders?product_id=%s&status=open" % P)
    gone = st3 == 200 and isinstance(lst, list) and not any(o.get("id") == oid2 for o in lst)
    if not step(10, "client id", st == 200 and st2 == 200 and gone, "placed HTTP %s, DELETE /orders/client:... HTTP %s, no longer open: %s" % (st, st2, gone)):
        api.call("cancel_all", "DELETE", "/orders?product_id=" + P)
        return finish(1, args)

    # 11 --------------------------------------------------------------------------------------------------------------
    for _ in range(3):
        place(far, "place")
    st, js = api.call("cancel_all", "DELETE", "/orders?product_id=" + P)
    shapes["cancel_all_reply"] = shape(js)
    st2, lst = api.call("open", "GET", "/orders?product_id=%s&status=open" % P)
    if not step(11, "cancel all", st == 200 and st2 == 200 and lst == [], "DELETE /orders?product_id: HTTP %s %s; open list afterwards %s" % (
            st, shape(js), "empty" if lst == [] else shape(lst))):
        return finish(1, args)

    # 12 --------------------------------------------------------------------------------------------------------------
    if args.skip_bridge:
        step(12, "bridge", True, "skipped on request")
    else:
        ok, detail = bridge_loop(args, far, size_s, ws_ok and ws_url)
        if not step(12, "bridge", ok, detail):
            api.call("cancel_all", "DELETE", "/orders?product_id=" + P)
            return finish(1, args)

    # 13 --------------------------------------------------------------------------------------------------------------
    st, lst = api.call("open", "GET", "/orders?product_id=%s&status=open" % P)
    ok = step(13, "flat", st == 200 and lst == [], "nothing working at the end")
    return finish(0 if ok and all(r["ok"] for r in results) else 1, args)


def bridge_loop(args, far, size_s, ws_url=None):
    """Runs the real bridge in --loop mode: new, replace to another price, cancel, end of segment."""
    cmd = [sys.executable, os.path.join(HERE, "order_bridge.py"), "--loop", "--rate", "6", "--sync-s", "30",
           "--max-notional", str(args.max_notional), "--product", args.product, "--timeout", str(args.timeout), "--log", "-"]
    if args.test_exchange:
        cmd += ["--test-exchange", "--base-url", args.base_url]
    if ws_url:
        cmd += ["--ws-fills", "--ws-url", ws_url]
    lots = int(round(float(size_s) * 100000))
    px_raw = int(round(far * 10000))
    lines = [{"t": "hello", "v": 1, "px_scale": 10000, "qty_scale": 100000},
             {"t": "new", "seg": 1, "id": 1, "side": "buy", "px": px_raw, "qty": lots, "type": 4},
             {"t": "rep", "seg": 1, "id": 1, "px": px_raw - 100000, "qty": lots},
             {"t": "can", "seg": 1, "id": 1},
             {"t": "end", "seg": 1, "reason": "smoke"}]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(os.environ), text=True)
    t0 = time.monotonic()
    got = []
    acct = []
    try:
        for m in lines:
            p.stdin.write(json.dumps(m) + "\n")
            p.stdin.flush()
            if m["t"] in ("new", "rep", "can"):
                while True:
                    line = p.stdout.readline()  # the loop bridge answers each request with exactly one report (and once, after the hello, about the account)
                    if not line:
                        break
                    try:
                        rec = json.loads(line)
                    except ValueError:
                        rec = {"rt": "unparsable"}
                    if rec.get("t") == "acct":
                        acct.append(rec)
                        continue
                    got.append(rec)
                    break
                if not line:
                    break
        p.stdin.close()
        rc = p.wait(timeout=30)
    except Exception as e:  # noqa: BLE001
        p.kill()
        return False, "bridge child failed: %s %s" % (type(e).__name__, str(e)[:80])
    types = [g.get("rt") for g in got]
    want = ["accepted", "replaced", "canceled"]
    shapes["bridge_reports"] = [sorted(g.keys()) for g in got][:1]
    errtxt = p.stderr.read()
    clean = ('"event": "exit_flat"' in errtxt and '"flat": true' in errtxt)
    acct_ok = len(acct) == 1 and acct[0].get("ok") == 1
    ok = types == want and rc == 0 and acct_ok
    return ok, "reports %s (wanted %s), account report %s, bridge exit %s, flat at exit %s, websocket fills %s, %.1f s" % (
        types, want, "ok" if acct_ok else "MISSING or unknown: %s" % acct, rc, clean, "on" if ws_url else "off", time.monotonic() - t0)


if __name__ == "__main__":
    sys.exit(main())
