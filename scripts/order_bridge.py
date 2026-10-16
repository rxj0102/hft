#!/usr/bin/env python3
"""Order-entry bridge: mirrors hft_live's order flow to the Coinbase Exchange SANDBOX over REST.

  hft_live --orders-fd 3 ... 3>pipe  |  order_bridge.py   (scripts/supervise.py --bridge wires this up)

What it is: a reconciler. hft_live's paper venue stays the source of truth for the strategy; this process
keeps the sandbox account's resting orders equal to the orders hft_live believes are working, as far as the
exchange's rate limit allows. It does not feed acks or fills back to hft_live (see docs/SANDBOX.md).

  * Coinbase Exchange has no amend. A replace is cancel, then a new order; the two are not atomic.
  * Updates are coalesced: only the latest wanted state of an order is sent. The sandbox allows 15 private
    requests per second per profile; --rate (default 8) stays well below it.
  * Cancels go before new orders. Orders are post-only.
  * At start, and when input ends or on SIGTERM, it cancels ALL open orders for --product on the account.
    Use a dedicated sandbox profile.

Safety, in code, not in convention:
  * Keys come from the environment only: HFT_CB_KEY, HFT_CB_SECRET, HFT_CB_PASSPHRASE. There is no option
    for them, and they are never logged. Missing keys: exit 2 before any network use.
  * The base URL must be exactly https://api-public.sandbox.exchange.coinbase.com. Production is refused.
    --test-exchange allows plain http to 127.0.0.1/localhost, for the local fake exchange only.
  * Orders above --max-notional USD, or beyond --max-open working orders, are refused, not sent.

Exit codes: 0 stopped (flat), 1 could not get flat or too many errors (restartable), 2 usage or keys missing,
4 authentication refused or clock skew (not restartable: a person must look).
"""
import argparse
import base64
import collections
import hashlib
import hmac
import json
import os
import queue
import signal
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

SANDBOX_URL = "https://api-public.sandbox.exchange.coinbase.com"
NS = uuid.UUID("5c0ffee5-0000-4000-8000-0000000b01d9")
ENV_KEYS = ("HFT_CB_KEY", "HFT_CB_SECRET", "HFT_CB_PASSPHRASE")


class Fatal(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def check_url(url, test_exchange):
    """Returns the normalised base URL or raises Fatal(2). Production can never pass."""
    p = urllib.parse.urlparse(url)
    if test_exchange:
        if p.scheme == "http" and p.hostname in ("127.0.0.1", "localhost") and p.port:
            return "http://%s:%d" % (p.hostname, p.port)
        raise Fatal(2, "--test-exchange only allows http://127.0.0.1:PORT or http://localhost:PORT")
    if url.rstrip("/") == SANDBOX_URL:
        return SANDBOX_URL
    raise Fatal(2, "refusing base URL %r: only %s is allowed" % (url, SANDBOX_URL))


def sign(secret_b64, timestamp, method, path, body):
    """CB-ACCESS-SIGN: base64(HMAC-SHA256(base64decode(secret), timestamp + METHOD + path + body))."""
    key = base64.b64decode(secret_b64)
    msg = (timestamp + method.upper() + path + body).encode()
    return base64.b64encode(hmac.new(key, msg, hashlib.sha256).digest()).decode()


def fmt_price(raw, px_scale):
    """raw / px_scale with 2 decimals, or None when the price is not on a cent."""
    cent = px_scale // 100
    if raw <= 0 or raw % cent != 0:
        return None
    c = raw // cent
    return "%d.%02d" % (c // 100, c % 100)


def fmt_size(lots, qty_scale):
    """lots / qty_scale as a decimal string (5 decimals for a 1e5 scale)."""
    if lots <= 0:
        return None
    digits = len(str(qty_scale)) - 1
    whole, frac = divmod(lots, qty_scale)
    return "%d.%0*d" % (whole, digits, frac)


class Log:
    def __init__(self, path):
        self.f = sys.stderr if path in (None, "-") else open(path, "a", buffering=1)
        self.t0 = time.monotonic()

    def emit(self, event, **kw):
        rec = {"ts_ms": int(time.time() * 1000), "mono_s": round(time.monotonic() - self.t0, 3), "event": event}
        rec.update(kw)
        self.f.write(json.dumps(rec) + "\n")


class Client:
    """Signed REST calls. Returns (status, parsed_json_or_None). Network errors raise OSError."""

    def __init__(self, base, key, secret, passphrase, timeout, test):
        self.base, self.key, self.secret, self.passphrase, self.timeout = base, key, secret, passphrase, timeout
        # In test mode never go through an environment proxy: the fake exchange is on localhost.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({})) if test else urllib.request.build_opener()

    def call(self, method, path, body=None, signed=True):
        data = json.dumps(body, separators=(",", ":")) if body is not None else ""
        headers = {"Content-Type": "application/json", "User-Agent": "hft-order-bridge/1"}
        if signed:
            ts = "%.3f" % time.time()
            headers.update({"CB-ACCESS-KEY": self.key, "CB-ACCESS-SIGN": sign(self.secret, ts, method, path, data),
                            "CB-ACCESS-TIMESTAMP": ts, "CB-ACCESS-PASSPHRASE": self.passphrase})
        req = urllib.request.Request(self.base + path, data=data.encode() if data else None, method=method, headers=headers)
        try:
            with self.opener.open(req, timeout=self.timeout) as r:
                status, raw, hdrs = r.status, r.read(), r.headers
        except urllib.error.HTTPError as e:
            status, raw, hdrs = e.code, e.read(), e.headers
        try:
            js = json.loads(raw) if raw else None
        except ValueError:
            js = None
        return status, js, hdrs


class Bridge:
    def __init__(self, a, client, log):
        self.a, self.c, self.log = a, client, log
        self.lock = threading.Lock()
        self.desired = {}          # (seg, id) -> dict(side, px, qty)
        self.live = {}             # (seg, id) -> dict(oid, coid, px, qty)   what we believe rests on the exchange
        self.rejected = {}         # (seg, id) -> (px, qty) that the exchange refused; not retried unchanged
        self.placements = collections.Counter()
        self.run = uuid.uuid4().hex[:8]
        self.px_scale, self.qty_scale = 10000, 100000
        self.cnt = collections.Counter()
        self.pause_until = 0.0
        self.backoff = 0.0
        self.consec_err = 0
        self.last_req = 0.0
        self.last_ok = time.monotonic()
        self.next_metrics = 0.0
        self.eof = threading.Event()
        self.stop = threading.Event()

    # ---- input ---------------------------------------------------------------------------------------------
    def reader(self, f):
        try:
            for line in f:
                try:
                    m = json.loads(line)
                except ValueError:
                    self.cnt["bad_lines"] += 1
                    continue
                self.apply(m)
        finally:
            self.eof.set()

    def apply(self, m):
        t = m.get("t")
        with self.lock:
            self.cnt["events_in"] += 1
            if t == "hello":
                self.px_scale, self.qty_scale = int(m["px_scale"]), int(m["qty_scale"])
            elif t == "new":
                self.desired[(m["seg"], m["id"])] = {"side": m["side"], "px": m["px"], "qty": m["qty"]}
            elif t == "rep":
                d = self.desired.get((m["seg"], m["id"]))
                if d:
                    d["px"], d["qty"] = m["px"], m["qty"]
            elif t == "can":
                self.desired.pop((m["seg"], m["id"]), None)
            elif t == "end":
                seg = m["seg"]
                for k in [k for k in self.desired if k[0] == seg]:
                    del self.desired[k]
                self.log.emit("segment_end", seg=seg, reason=m.get("reason"))
            else:
                self.cnt["unknown_events"] += 1

    # ---- exchange calls ---------------------------------------------------------------------------------------
    def req(self, method, path, body=None):
        """One paced request. Returns (status, json) or (None, None) on a network/5xx problem."""
        gap = 1.0 / self.a.rate
        now = time.monotonic()
        wait = max(self.last_req + gap - now, self.pause_until - now, 0.0)
        end = now + wait
        while True:  # sleep in slices: the supervisor's watchdog reads the metrics file, so it must keep being refreshed
            self.tick()
            left = end - time.monotonic()
            if left <= 0:
                break
            time.sleep(min(left, 0.2))
        self.last_req = time.monotonic()
        self.cnt["requests"] += 1
        try:
            status, js, hdrs = self.c.call(method, path, body)
        except OSError as e:
            self.cnt["net_errors"] += 1
            self.fail("net_error", detail=type(e).__name__)
            return None, None
        if status in (401, 403):
            raise Fatal(4, "exchange refused our credentials (HTTP %d)" % status)
        if status == 429:
            self.cnt["rate_limited"] += 1
            self.backoff = min(max(self.backoff * 2, 1.0), 8.0)
            self.pause_until = time.monotonic() + self.backoff
            self.log.emit("rate_limited", backoff_s=self.backoff)
            return 429, js
        if status >= 500:
            self.cnt["http_5xx"] += 1
            self.fail("http_5xx", status=status)
            return None, None
        self.consec_err = 0
        self.backoff = 0.0
        self.last_ok = time.monotonic()
        return status, js

    def tick(self):
        now = time.monotonic()
        if self.a.metrics and now >= self.next_metrics:
            self.metrics(self.a.metrics)
            self.next_metrics = now + self.a.metrics_s

    def fail(self, what, **kw):
        self.consec_err += 1
        self.backoff = min(max(self.backoff * 2, 0.5), 10.0)
        self.pause_until = time.monotonic() + self.backoff
        self.log.emit(what, consecutive=self.consec_err, backoff_s=self.backoff, **kw)
        if self.consec_err >= self.a.max_errors:
            raise Fatal(1, "%d consecutive exchange errors" % self.consec_err)

    def cancel_all(self):
        """DELETE /orders?product_id=. Returns True once GET /orders shows nothing working (or after retries: False)."""
        for attempt in range(6):
            st, js = self.req("DELETE", "/orders?product_id=" + self.a.product)
            if st == 200:
                n = len(js) if isinstance(js, list) else 0
                self.cnt["cancel_all_cancelled"] += n
                self.log.emit("cancel_all", cancelled=n)
            st, js = self.req("GET", "/orders?product_id=%s&status=open" % self.a.product)
            if st == 200 and isinstance(js, list) and len(js) == 0:
                self.live.clear()
                return True
            time.sleep(0.3)
        return False

    def sync(self):
        """Compares the exchange's open orders with ours: forgets orders that are gone (filled or cancelled
        elsewhere), cancels strays. Returns after at most a few requests."""
        st, js = self.req("GET", "/orders?product_id=%s&status=open" % self.a.product)
        if st != 200 or not isinstance(js, list):
            return
        on_ex = {o.get("id"): o for o in js}
        for k, v in list(self.live.items()):
            if v["oid"] not in on_ex:
                self.cnt["vanished"] += 1  # filled, or cancelled behind our back
                self.log.emit("order_vanished", seg=k[0], id=k[1])
                del self.live[k]
        known = {v["oid"] for v in self.live.values()}
        for oid in on_ex:
            if oid not in known:
                self.cnt["strays"] += 1
                self.log.emit("stray_order", oid=oid)
                self.req("DELETE", "/orders/" + str(oid))

    # ---- reconcile ----------------------------------------------------------------------------------------------
    def next_action(self):
        with self.lock:
            for k, v in self.live.items():
                d = self.desired.get(k)
                if d is None or d["px"] != v["px"] or d["qty"] != v["qty"]:
                    return ("cancel", k, v)
            for k, d in self.desired.items():
                if k in self.live or self.rejected.get(k) == (d["px"], d["qty"]):
                    continue
                return ("place", k, dict(d))
        return None

    def step(self):
        act = self.next_action()
        if act is None:
            return False
        kind, k, v = act
        if kind == "cancel":
            st, js = self.req("DELETE", "/orders/" + v["oid"])
            if st in (200, 404):  # 404: already gone
                self.cnt["cancels" if st == 200 else "cancels_already_gone"] += 1
                self.live.pop(k, None)
            return True
        # place
        if len(self.live) >= self.a.max_open:
            self.cnt["refused_max_open"] += 1
            self.rejected[k] = (v["px"], v["qty"])
            return True
        px, sz = fmt_price(v["px"], self.px_scale), fmt_size(v["qty"], self.qty_scale)
        notional = (v["px"] / self.px_scale) * (v["qty"] / self.qty_scale)
        if px is None or sz is None or notional > self.a.max_notional:
            self.cnt["refused_local"] += 1
            self.log.emit("refused_local", seg=k[0], id=k[1], notional=round(notional, 2))
            self.rejected[k] = (v["px"], v["qty"])
            return True
        self.placements[k] += 1
        coid = str(uuid.uuid5(NS, "%s:%d:%d:%d" % (self.run, k[0], k[1], self.placements[k])))
        body = {"type": "limit", "side": v["side"], "product_id": self.a.product, "price": px, "size": sz,
                "post_only": True, "client_oid": coid}
        st, js = self.req("POST", "/orders", body)
        if st == 200 and isinstance(js, dict) and "id" in js:
            self.cnt["placed"] += 1
            self.live[k] = {"oid": js["id"], "coid": coid, "px": v["px"], "qty": v["qty"]}
        elif st is None:
            # Outcome unknown (timeout, 5xx): the order may rest on the exchange. Cancel by client id; 404 is fine.
            self.cnt["place_unknown"] += 1
            self.req("DELETE", "/orders/client:" + coid)
        elif st == 429:
            pass  # try again after the pause
        else:
            self.cnt["rejected"] += 1
            self.rejected[k] = (v["px"], v["qty"])
            self.log.emit("order_rejected", seg=k[0], id=k[1], status=st, message=(js or {}).get("message", "")[:120] if isinstance(js, dict) else "")
        return True

    def metrics(self, path):
        if not path:
            return
        with self.lock:
            want = len(self.desired)
        lines = []

        def g(name, help_, val, typ="counter"):
            lines.append("# HELP %s %s\n# TYPE %s %s\n%s %s" % (name, help_, name, typ, name, val))
        g("hft_bridge_working_orders", "Orders believed resting on the exchange.", len(self.live), "gauge")
        g("hft_bridge_wanted_orders", "Orders hft_live wants working.", want, "gauge")
        g("hft_bridge_seconds_since_ok", "Seconds since the last successful exchange call.", round(time.monotonic() - self.last_ok, 1), "gauge")
        for name in ("events_in", "requests", "placed", "cancels", "cancels_already_gone", "rejected", "rate_limited", "net_errors",
                     "http_5xx", "place_unknown", "refused_local", "refused_max_open", "vanished", "strays", "bad_lines",
                     "cancel_all_cancelled"):
            g("hft_bridge_%s_total" % name, name.replace("_", " ") + ".", self.cnt[name])
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(tmp, path)

    # ---- main loop ------------------------------------------------------------------------------------------------
    def serve(self, stdin):
        threading.Thread(target=self.reader, args=(stdin,), daemon=True).start()
        next_sync = time.monotonic() + self.a.sync_s
        while not (self.eof.is_set() or self.stop.is_set()):
            did = self.step()
            now = time.monotonic()
            self.tick()
            if now >= next_sync:
                self.sync()
                next_sync = time.monotonic() + self.a.sync_s
            if not did:
                time.sleep(0.01)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", default=SANDBOX_URL)
    ap.add_argument("--test-exchange", action="store_true", help="allow http://127.0.0.1:PORT (local fake exchange only)")
    ap.add_argument("--product", default="BTC-USD")
    ap.add_argument("--rate", type=float, default=8.0, help="requests per second (the sandbox limit is 15)")
    ap.add_argument("--max-open", type=int, default=6)
    ap.add_argument("--max-notional", type=float, default=25.0, help="USD per order")
    ap.add_argument("--max-errors", type=int, default=20, help="consecutive exchange errors before giving up")
    ap.add_argument("--sync-s", type=float, default=5.0)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--metrics", default=None)
    ap.add_argument("--metrics-s", type=float, default=1.0)
    ap.add_argument("--no-cancel-on-start", action="store_true", help="tests only")
    a = ap.parse_args(argv)

    log = Log(a.log)
    try:
        base = check_url(a.base_url, a.test_exchange)
        missing = [k for k in ENV_KEYS if not os.environ.get(k)]
        if missing:
            raise Fatal(2, "missing environment variables: " + ", ".join(missing))
        key, secret, passphrase = (os.environ[k] for k in ENV_KEYS)
        try:
            base64.b64decode(secret, validate=True)
        except ValueError:
            raise Fatal(2, "HFT_CB_SECRET is not base64")
    except Fatal as e:
        log.emit("fatal", code=e.code, message=str(e))
        print("order_bridge: " + str(e), file=sys.stderr)
        return e.code

    c = Client(base, key, secret, passphrase, a.timeout, a.test_exchange)
    b = Bridge(a, c, log)

    def on_sig(signum, frame):
        b.stop.set()
    signal.signal(signal.SIGTERM, on_sig)
    signal.signal(signal.SIGINT, on_sig)

    code = 0
    log.emit("bridge_start", base_url=base, product=a.product, rate=a.rate, max_open=a.max_open, max_notional=a.max_notional)
    try:
        try:
            st, js, _ = c.call("GET", "/time", signed=False)
        except OSError as e:  # unreachable now; the next calls count the errors and give up properly
            st, js = None, None
            log.emit("time_unreachable", detail=type(e).__name__)
        if st == 200 and isinstance(js, dict) and "epoch" in js:
            skew = float(js["epoch"]) - time.time()
            log.emit("clock_skew", seconds=round(skew, 3))
            if abs(skew) > 20:
                raise Fatal(4, "clock differs from the exchange by %.1f s (limit 30)" % skew)
        if not a.no_cancel_on_start:
            ok = b.cancel_all()
            log.emit("start_flat", flat=ok)
            if not ok:
                raise Fatal(1, "could not clear existing orders at start")
        b.serve(sys.stdin)
        log.emit("stopping", cause="signal" if b.stop.is_set() else "input_ended", working=len(b.live))
    except Fatal as e:
        code = e.code
        log.emit("fatal", code=e.code, message=str(e))
        print("order_bridge: " + str(e), file=sys.stderr)
    # leave nothing behind, whatever ended us (except lost credentials: nothing can be sent then)
    if code != 4:
        try:
            b.consec_err = 0
            b.backoff = 0.0
            a.max_errors = 4  # an exchange that is down must not hold the shutdown for minutes
            flat = b.cancel_all()
        except Fatal as e:
            flat = False
            code = code or e.code
        log.emit("exit_flat", flat=flat)
        if not flat:
            code = code or 1
    b.metrics(a.metrics)
    log.emit("exit", code=code, **{k: v for k, v in b.cnt.items() if k in ("placed", "cancels", "rejected", "rate_limited")})
    return code


if __name__ == "__main__":
    sys.exit(main())
