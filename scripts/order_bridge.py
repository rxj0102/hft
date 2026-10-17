#!/usr/bin/env python3
"""Order-entry bridge: mirrors hft_live's order flow to the Coinbase Exchange SANDBOX over REST.

  hft_live --orders-fd 3 ... 3>pipe  |  order_bridge.py   (scripts/supervise.py --bridge wires this up)

Two modes. Default (mirror): described below. With --loop (closed loop): every request hft_live sends (new,
replace, cancel) is carried out and answered with exactly one report on stdout (a JSON line, read by
hft_live --exchange-fd): accepted / rejected / replaced / canceled / cancel_rejected / replace_rejected, plus
fills found by the periodic sync. Nothing is coalesced then: hft_live has at most one request in flight per
order, and what the exchange says is what its OMS believes. Reports carry the segment number of the request.
A replace is cancel + new; if the new order is refused (post-only would cross) the order is gone and the
answer is `canceled`. Fill prices are the order's limit price (the exchange's executed value is not used).

What the mirror mode is: a reconciler. hft_live's paper venue stays the source of truth for the strategy; this process
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
import decimal
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
WS_SANDBOX = "wss://ws-feed-public.sandbox.exchange.coinbase.com"


OT_IOC, OT_POST_ONLY = 2, 4  # the engine's order types on the wire


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


def check_ws_url(url, test_exchange):
    """The websocket twin of check_url: the sandbox feed, or ws://127.0.0.1:PORT for the local fake. Nothing else."""
    p = urllib.parse.urlparse(url)
    if test_exchange:
        if p.scheme == "ws" and p.hostname in ("127.0.0.1", "localhost") and p.port:
            return "ws://%s:%d" % (p.hostname, p.port)
        raise Fatal(2, "--test-exchange only allows ws://127.0.0.1:PORT or ws://localhost:PORT")
    if url.rstrip("/") == WS_SANDBOX:
        return WS_SANDBOX
    raise Fatal(2, "refusing websocket URL %r: only %s is allowed" % (url, WS_SANDBOX))


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


class WsFeed:
    """The exchange's websocket feed, `user` channel (our own orders), in a thread of its own.

    Every message goes to `messages`; the bridge's main thread takes them from there, so nothing here touches bridge state.
    A dropped or refused connection is retried with a growing pause (1 s to 15 s). `fresh` is raised after each successful
    subscription: events may have been missed while we were away, and the bridge answers with a REST sync.
    Authentication is the same HMAC as for REST, over timestamp + GET + /users/self/verify.
    """

    def __init__(self, url, product, key, secret, passphrase, log, test):
        self.url, self.product, self.key, self.secret, self.passphrase, self.log, self.test = url, product, key, secret, passphrase, log, test
        self.messages = collections.deque()
        self.subscribed = threading.Event()
        self.fresh = threading.Event()
        self.stats = collections.Counter()

    def start(self):
        threading.Thread(target=self._thread, daemon=True).start()

    def _thread(self):
        import asyncio
        asyncio.run(self._main())

    def subscribe_message(self):
        ts = "%.3f" % time.time()
        return {"type": "subscribe", "product_ids": [self.product], "channels": ["user"], "key": self.key,
                "passphrase": self.passphrase, "timestamp": ts, "signature": sign(self.secret, ts, "GET", "/users/self/verify", "")}

    async def _main(self):
        import asyncio
        import websockets
        pause = 1.0
        while True:
            try:
                kw = {"proxy": None} if self.test else {}
                async with websockets.connect(self.url, open_timeout=10, max_size=1 << 22, **kw) as ws:
                    self.stats["connects"] += 1
                    await ws.send(json.dumps(self.subscribe_message()))
                    async for raw in ws:
                        try:
                            m = json.loads(raw)
                        except ValueError:
                            self.stats["bad_messages"] += 1
                            continue
                        if not isinstance(m, dict):
                            self.stats["bad_messages"] += 1
                            continue
                        t = m.get("type")
                        if t == "subscriptions":
                            self.subscribed.set()
                            self.fresh.set()
                            pause = 1.0
                            self.log.emit("ws_subscribed", connects=self.stats["connects"])
                        elif t == "error":
                            self.stats["errors"] += 1
                            self.log.emit("ws_error", message=str(m.get("message"))[:120], reason=str(m.get("reason"))[:120])
                        else:
                            self.stats["messages"] += 1
                            self.messages.append(m)
                self.log.emit("ws_closed")
            except Exception as e:  # noqa: BLE001  any failure of the connection is retried
                self.stats["failures"] += 1
                self.log.emit("ws_failed", detail=type(e).__name__)
            self.subscribed.clear()
            await asyncio.sleep(pause)
            pause = min(pause * 2, 15.0)


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
        self.sync_now = False
        self.ws = None

    def drain_ws(self):
        pass  # the closed-loop bridge overrides this

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
    def req(self, method, path, body=None, signed=True):
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
            status, js, hdrs = self.c.call(method, path, body, signed)
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
        g("hft_bridge_ws_subscribed", "1 while the websocket user channel is subscribed.",
          int(self.ws.subscribed.is_set()) if self.ws else 0, "gauge")
        g("hft_bridge_seconds_since_ok", "Seconds since the last successful exchange call.", round(time.monotonic() - self.last_ok, 1), "gauge")
        for name in ("events_in", "requests", "placed", "cancels", "cancels_already_gone", "rejected", "rate_limited", "net_errors",
                     "http_5xx", "place_unknown", "refused_local", "refused_max_open", "vanished", "strays", "bad_lines",
                     "cancel_all_cancelled", "fills", "orphans", "reports_accepted", "reports_rejected", "reports_canceled",
                     "reports_replaced", "reports_fill", "reports_cancel_rejected", "reports_replace_rejected",
                     "fills_at_cancel", "cancel_unverified", "ioc_placed", "ioc_fills", "ioc_unsettled", "ioc_unknown",
                     "ioc_refused_band", "ioc_no_ticker", "refused_type",
                     "ws_connects", "ws_messages", "ws_bad_messages", "ws_errors", "ws_failures", "ws_fills", "ws_matches_unknown_order",
                     "ws_matches_bad", "ws_matches_already_reported"):
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
            if now >= next_sync or self.sync_now:
                self.sync_now = False
                self.sync()
                next_sync = time.monotonic() + self.a.sync_s
            if not did:
                time.sleep(0.01)


class LoopBridge(Bridge):
    """Closed loop: carry out each request, answer each with a report."""

    def __init__(self, a, client, log, out):
        super().__init__(a, client, log)
        self.out = out
        self.queue = collections.deque()   # requests in arrival order
        self.filled = {}                   # key -> lots already reported as filled
        self.ws_seen = {}                  # key -> lots the websocket has said were matched (never above what REST showed)
        self.reports_dead = False
        self.account = None                # the base currency's balance (Decimal), read once at start; None: not known
        self.account_asked = False

    # ---- the account (ADR 31) ------------------------------------------------------------------------------
    def fetch_account(self):
        """GET /accounts once: the balance in the product's base currency. hft_live compares it with its position file."""
        self.account_asked = True
        st, js = self.req("GET", "/accounts")
        cur = self.a.product.split("-")[0]
        if st == 200 and isinstance(js, list):
            for row in js:
                if isinstance(row, dict) and row.get("currency") == cur:
                    try:
                        self.account = decimal.Decimal(str(row["balance"]))
                    except (KeyError, decimal.InvalidOperation):
                        self.account = None
                    break
        self.log.emit("account", known=self.account is not None, status=st, currency=cur,
                      base_balance=str(self.account) if self.account is not None else None)

    def send_account(self):
        rec = {"t": "acct", "ok": 0}
        if self.account is not None:
            rec = {"t": "acct", "ok": 1, "base_raw": int((self.account * self.qty_scale).to_integral_value(decimal.ROUND_HALF_EVEN))}
        self.cnt["account_sent"] += 1
        if self.reports_dead:
            return
        try:
            self.out.write(json.dumps(rec, separators=(",", ":")) + "\n")
            self.out.flush()
        except (BrokenPipeError, ValueError, OSError):
            self.reports_dead = True
            self.log.emit("reports_pipe_closed")

    # ---- reports ------------------------------------------------------------------------------------------
    def report(self, key, rt, px=0, qty=0, leaves=0, reason="none", side=None):
        rec = {"t": "rep", "seg": key[0], "rt": rt, "id": key[1], "px": px, "qty": qty, "leaves": leaves, "reason": reason}
        if side:
            rec["side"] = side
        self.cnt["reports_" + rt] += 1
        if self.reports_dead:
            return
        try:
            self.out.write(json.dumps(rec, separators=(",", ":")) + "\n")
            self.out.flush()
        except (BrokenPipeError, ValueError, OSError):
            self.reports_dead = True
            self.log.emit("reports_pipe_closed")

    # ---- input --------------------------------------------------------------------------------------------
    def apply(self, m):
        t = m.get("t")
        with self.lock:
            self.cnt["events_in"] += 1
            if t == "hello":
                self.px_scale, self.qty_scale = int(m["px_scale"]), int(m["qty_scale"])
                if self.account_asked:
                    self.send_account()
            elif t in ("new", "rep", "can"):
                self.queue.append(m)
            elif t == "end":
                self.queue.append(m)
            else:
                self.cnt["unknown_events"] += 1

    # ---- carrying out one request ----------------------------------------------------------------------------------
    def post(self, k, side, px_raw, lots, ioc=False):
        """POST one order (post-only, or immediate-or-cancel when `ioc`).
        Returns ('ok', oid, coid) | ('refused', reason) | ('unknown',)"""
        px, sz = fmt_price(px_raw, self.px_scale), fmt_size(lots, self.qty_scale)
        notional = (px_raw / self.px_scale) * (lots / self.qty_scale)
        if px is None:
            self.cnt["refused_local"] += 1
            return ("refused", "badprice")
        if sz is None or notional > self.a.max_notional:
            self.cnt["refused_local"] += 1
            self.log.emit("refused_local", seg=k[0], id=k[1], notional=round(notional, 2))
            return ("refused", "badqty")
        self.placements[k] += 1
        coid = str(uuid.uuid5(NS, "%s:%d:%d:%d" % (self.run, k[0], k[1], self.placements[k])))
        body = {"type": "limit", "side": side, "product_id": self.a.product, "price": px, "size": sz, "post_only": True,
                "client_oid": coid}
        if ioc:
            del body["post_only"]
            body["time_in_force"] = "IOC"
        for _ in range(20):
            st, js = self.req("POST", "/orders", body)
            if st != 429:  # on 429 req() has already paused
                break
        else:
            st, js = None, None
        if st == 200 and isinstance(js, dict) and "id" in js:
            self.cnt["placed"] += 1
            return ("ok", js["id"], coid)
        if st is None:
            self.cnt["place_unknown"] += 1
            self.req("DELETE", "/orders/client:" + coid)  # the order may exist; make sure it does not
            return ("unknown",)
        self.cnt["rejected"] += 1
        msg = (js or {}).get("message", "") if isinstance(js, dict) else ""
        self.log.emit("order_rejected", seg=k[0], id=k[1], status=st, message=msg[:120])
        low = msg.lower()
        reason = "cross" if "post only" in low else "badqty" if "size" in low else "badprice" if "price" in low else "other"
        return ("refused", reason)

    def delete(self, oid):
        """DELETE one order. Returns 'ok' | 'gone' | 'error'"""
        for _ in range(20):
            st, _ = self.req("DELETE", "/orders/" + oid)
            if st != 429:
                break
        else:
            st = None
        if st == 200:
            return "ok"
        if st == 404:
            return "gone"
        return "error"

    def settle(self, k, v):
        """The exchange has accepted our DELETE of order k. How much of it had traded? The exchange's own record is the
        authority (the socket and the sync may not have told us yet): a fill that came just before the cancel is reported
        now, ahead of the cancel or replace report, so the OMS never sees a cancel for lots that were bought.
        Returns the lots filled in all (never more than the order's size)."""
        self.drain_ws()
        done = self.filled.get(k, 0)
        for _ in range(5):
            st, d = self.req("GET", "/orders/" + v["oid"])
            if st != 429:
                break
        else:
            st, d = None, None
        if st == 200 and isinstance(d, dict) and "filled_size" in d:
            try:
                theirs = min(self.lots_of(d["filled_size"]), v["qty"])
            except (ValueError, TypeError):
                theirs = done
                self.cnt["cancel_unverified"] += 1
            if theirs > done:
                self.cnt["fills_at_cancel"] += 1
                self.filled[k] = theirs
                self.report(k, "fill", px=v["px"], qty=theirs - done, leaves=v["qty"] - theirs, side=v["side"])
                done = theirs
        elif st != 404:  # 404: a cancelled order with no fills may no longer be on file; anything else we could not check
            self.cnt["cancel_unverified"] += 1
            self.log.emit("cancel_unverified", seg=k[0], id=k[1], status=st)
        return done

    def handle(self, m):
        t = m["t"]
        if t == "end":
            seg = m["seg"]
            self.log.emit("segment_end", seg=seg, reason=m.get("reason"))
            # anything still resting from that segment is an orphan (a flatten that failed): cancel it, no report
            for k in [k for k in self.live if k[0] == seg]:
                v = self.live.pop(k)
                self.cnt["orphans"] += 1
                self.log.emit("orphan_order", seg=seg, id=k[1])
                self.delete(v["oid"])
            return
        k = (m["seg"], m["id"])
        if t == "new":
            if len(self.live) >= self.a.max_open:
                self.cnt["refused_max_open"] += 1
                self.report(k, "rejected", reason="badqty", side=m["side"])
                return
            otype = m.get("type", OT_POST_ONLY)
            if otype == OT_IOC:
                self.close_order(k, m)
                return
            if otype != OT_POST_ONLY:  # the bridge carries out the strategy's quotes and the closing orders, nothing else
                self.cnt["refused_type"] += 1
                self.report(k, "rejected", px=m["px"], qty=m["qty"], reason="other", side=m["side"])
                return
            r = self.post(k, m["side"], m["px"], m["qty"])
            if r[0] == "ok":
                self.live[k] = {"oid": r[1], "coid": r[2], "px": m["px"], "qty": m["qty"], "side": m["side"]}
                self.filled[k] = 0
                self.ws_seen[k] = 0
                self.report(k, "accepted", px=m["px"], qty=m["qty"], leaves=m["qty"], side=m["side"])
            else:
                self.report(k, "rejected", px=m["px"], qty=m["qty"], reason=r[1] if r[0] == "refused" else "other", side=m["side"])
        elif t == "can":
            v = self.live.get(k)
            if v is None:
                self.report(k, "cancel_rejected", reason="unknown")
                return
            res = self.delete(v["oid"])
            if res == "ok":
                done = self.settle(k, v)  # a fill that came just before the cancel is reported first
                if k not in self.live or done >= v["qty"]:  # filled completely: the fill has been reported
                    self.live.pop(k, None)
                    self.report(k, "cancel_rejected", reason="unknown")
                    return
                left = v["qty"] - done
                self.live.pop(k)
                self.report(k, "canceled", px=v["px"], qty=left, leaves=0, reason="user", side=v["side"])
            else:  # gone (filled or cancelled elsewhere: the sync will say which) or an error: the order stays ours
                self.report(k, "cancel_rejected", reason="unknown")
        elif t == "rep":
            v = self.live.get(k)
            if v is None:
                self.report(k, "replace_rejected", reason="unknown")
                return
            res = self.delete(v["oid"])
            if res != "ok":
                self.report(k, "replace_rejected", reason="unknown")
                return
            done = self.settle(k, v)
            if k not in self.live or done >= v["qty"]:
                self.live.pop(k, None)
                self.report(k, "replace_rejected", reason="unknown")
                return
            self.live.pop(k)
            r = self.post(k, v["side"], m["px"], m["qty"])
            if r[0] == "ok":
                self.live[k] = {"oid": r[1], "coid": r[2], "px": m["px"], "qty": m["qty"], "side": v["side"]}
                self.filled[k] = 0
                self.ws_seen[k] = 0
                self.report(k, "replaced", px=m["px"], qty=m["qty"], leaves=m["qty"], side=v["side"])
            else:  # the old order is gone and the new one was refused or lost: the order no longer exists
                self.report(k, "canceled", px=v["px"], qty=m["qty"], leaves=0, reason=r[1] if r[0] == "refused" else "other", side=v["side"])

    def ticker_mid(self):
        """Mid of the exchange's own best bid and ask (public endpoint), or None if it cannot be had."""
        for _ in range(5):
            st, js = self.req("GET", "/products/%s/ticker" % self.a.product, signed=False)
            if st != 429:
                break
        else:
            st, js = None, None
        if st != 200 or not isinstance(js, dict):
            return None
        try:
            bid, ask = float(js["bid"]), float(js["ask"])
        except (KeyError, ValueError, TypeError):
            return None
        return (bid + ask) / 2 if 0 < bid <= ask else None

    def close_order(self, k, m):
        """An immediate-or-cancel order that the engine sends to close its position. It is the one order that takes liquidity,
        so before it goes out its price is checked against the exchange's own quote (the engine priced it from its local book,
        which may be wrong); then the exchange's record of the order says how much of it traded."""
        side, px_raw, lots = m["side"], m["px"], m["qty"]
        mid = self.ticker_mid()
        if mid is None:
            self.cnt["ioc_no_ticker"] += 1
            self.log.emit("ioc_refused", seg=k[0], id=k[1], why="no ticker")
            self.report(k, "rejected", px=px_raw, qty=lots, reason="other", side=side)
            return
        px = px_raw / self.px_scale
        if abs(px - mid) / mid * 1e4 > self.a.ioc_band_bps:
            self.cnt["ioc_refused_band"] += 1
            self.log.emit("ioc_refused", seg=k[0], id=k[1], why="outside band", px=round(px, 4), mid=round(mid, 4))
            self.report(k, "rejected", px=px_raw, qty=lots, reason="badprice", side=side)
            return
        r = self.post(k, side, px_raw, lots, ioc=True)
        if r[0] != "ok":
            self.report(k, "rejected", px=px_raw, qty=lots, reason=r[1] if r[0] == "refused" else "other", side=side)
            return
        oid = r[1]
        self.cnt["ioc_placed"] += 1
        self.report(k, "accepted", px=px_raw, qty=lots, leaves=lots, side=side)
        for _ in range(5):
            st, d = self.req("GET", "/orders/" + oid)
            if st != 429:
                break
        else:
            st, d = None, None
        v = {"oid": oid, "coid": r[2], "px": px_raw, "qty": lots, "side": side, "ioc": True, "reads": 1}
        self.filled[k] = 0
        self.ws_seen[k] = 0
        if not self.ioc_outcome(k, v, st, d):
            # The record is missing, unreadable or says the order is still working. An immediate-or-cancel order cannot rest,
            # so it has ended, but how is not known: keep it and let the sync read the final record (and the socket may say).
            self.cnt["ioc_unsettled"] += 1
            self.live[k] = v
            self.sync_now = True

    def ioc_outcome(self, k, v, st, d):
        """Reports how an immediate-or-cancel order ended, from the exchange's record. False if the record does not say."""
        if not (st == 200 and isinstance(d, dict) and "filled_size" in d and d.get("status") in ("done", "rejected", "cancelled", "canceled")):
            return False
        lots = v["qty"]
        avg = v["px"]
        try:
            n = min(max(self.lots_of(d["filled_size"]), 0), lots)
            if n > 0 and float(d.get("executed_value", 0)) > 0:
                avg = int(round(float(d["executed_value"]) / float(d["filled_size"]) * self.px_scale)) or v["px"]
        except (ValueError, TypeError, ZeroDivisionError):
            return False
        prev = self.filled.get(k, 0)  # lots the socket has already reported
        n = max(n, prev)
        if n > prev:
            self.cnt["ioc_fills"] += 1
            self.filled[k] = n
            self.report(k, "fill", px=avg, qty=n - prev, leaves=lots - n, side=v["side"])
        if n < lots:
            self.report(k, "canceled", px=v["px"], qty=lots - n, leaves=0, reason="ioc", side=v["side"])
        return True

    def step(self):
        self.drain_ws()
        with self.lock:
            m = self.queue.popleft() if self.queue else None
        if m is None:
            return False
        self.handle(m)
        return True

    # ---- fills from the websocket ------------------------------------------------------------------------------------
    def drain_ws(self):
        """Takes what the websocket thread has collected. A match for one of our orders becomes a fill report at once; the
        REST sync stays as the backstop and, because both only ever raise `filled`, reports each lot once."""
        ws = self.ws
        if ws is None:
            return
        if ws.fresh.is_set():  # (re)subscribed: whatever happened while we were away is found by a sync now
            ws.fresh.clear()
            self.sync_now = True
        while True:
            try:
                m = ws.messages.popleft()
            except IndexError:
                break
            t = m.get("type")
            if t == "match":
                self.ws_match(m)
            elif t == "done":
                oid = m.get("order_id")
                if any(v["oid"] == oid for v in self.live.values()):
                    self.sync_now = True  # an order of ours ended on the exchange: the sync works out how
        for key, val in list(ws.stats.items()):
            self.cnt["ws_" + key] = val

    def ws_match(self, m):
        by_oid = {v["oid"]: k for k, v in self.live.items()}
        k = by_oid.get(m.get("maker_order_id")) or by_oid.get(m.get("taker_order_id"))
        if k is None:
            self.cnt["ws_matches_unknown_order"] += 1
            return
        v = self.live[k]
        try:
            n = self.lots_of(m["size"])
            px = int(round(float(m["price"]) * self.px_scale))
        except (KeyError, ValueError, TypeError):
            self.cnt["ws_matches_bad"] += 1
            return
        if n <= 0 or px <= 0:
            self.cnt["ws_matches_bad"] += 1
            return
        self.ws_seen[k] = self.ws_seen.get(k, 0) + n
        total = min(self.ws_seen[k], v["qty"])
        delta = total - self.filled.get(k, 0)
        if delta <= 0:
            self.cnt["ws_matches_already_reported"] += 1
            return
        self.filled[k] = total
        self.cnt["ws_fills"] += 1
        self.report(k, "fill", px=px, qty=delta, leaves=v["qty"] - total, side=v["side"])
        if total >= v["qty"]:
            del self.live[k]

    # ---- finding fills and orders that left ------------------------------------------------------------------------------
    def lots_of(self, size_str):
        return int(round(float(size_str) * self.qty_scale))

    def sync(self):
        st, js = self.req("GET", "/orders?product_id=%s&status=open" % self.a.product)
        if st != 200 or not isinstance(js, list):
            return
        on_ex = {o.get("id"): o for o in js}
        for k, v in list(self.live.items()):
            o = on_ex.get(v["oid"])
            if o is not None:
                done = self.lots_of(o.get("filled_size", "0"))
                prev = self.filled.get(k, 0)
                if done > prev:
                    self.filled[k] = done
                    self.report(k, "fill", px=v["px"], qty=done - prev, leaves=v["qty"] - done, side=v["side"])
                continue
            # not open any more: ask how it ended
            st2, d = self.req("GET", "/orders/" + v["oid"])
            prev = self.filled.get(k, 0)
            if v.get("ioc"):
                # Never guess for these: a "cancelled" that was really a fill would make the engine sell again. If the exchange
                # will not say, report nothing; the engine's own deadline then fails the close and says so.
                v["reads"] = v.get("reads", 0) + 1
                if self.ioc_outcome(k, v, st2, d):
                    pass
                elif v["reads"] < 4:
                    self.sync_now = True
                    continue
                else:
                    self.cnt["ioc_unknown"] += 1
                    self.log.emit("ioc_outcome_unknown", seg=k[0], id=k[1], status=st2)
                del self.live[k]
                continue
            if st2 == 200 and isinstance(d, dict) and d.get("done_reason") == "filled":
                self.cnt["fills"] += 1
                self.report(k, "fill", px=v["px"], qty=v["qty"] - prev, leaves=0, side=v["side"])
            elif st2 in (200, 404):
                self.cnt["vanished"] += 1
                self.log.emit("order_vanished", seg=k[0], id=k[1])
                self.report(k, "canceled", px=v["px"], qty=v["qty"] - prev, leaves=0, reason="other", side=v["side"])
            else:
                continue  # could not tell; try again at the next sync
            del self.live[k]
        known = {v["oid"] for v in self.live.values()}
        for oid in on_ex:
            if oid not in known:
                self.cnt["strays"] += 1
                self.log.emit("stray_order", oid=oid)
                self.req("DELETE", "/orders/" + str(oid))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", default=SANDBOX_URL)
    ap.add_argument("--test-exchange", action="store_true", help="allow http://127.0.0.1:PORT (local fake exchange only)")
    ap.add_argument("--product", default="BTC-USD")
    ap.add_argument("--rate", type=float, default=8.0, help="requests per second (the sandbox limit is 15)")
    ap.add_argument("--max-open", type=int, default=6)
    ap.add_argument("--max-notional", type=float, default=25.0, help="USD per order")
    ap.add_argument("--ioc-band-bps", type=float, default=100.0,
                    help="a closing (immediate-or-cancel) order is refused if its price is further than this from the exchange's own mid")
    ap.add_argument("--max-errors", type=int, default=20, help="consecutive exchange errors before giving up")
    ap.add_argument("--sync-s", type=float, default=5.0)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--metrics", default=None)
    ap.add_argument("--metrics-s", type=float, default=1.0)
    ap.add_argument("--loop", action="store_true", help="closed loop: answer every request with a report on stdout")
    ap.add_argument("--ws-fills", action="store_true", help="with --loop: also take fills from the websocket user channel (needs the websockets package)")
    ap.add_argument("--ws-url", default=WS_SANDBOX)
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
        ws_url = None
        if a.ws_fills:
            if not a.loop:
                raise Fatal(2, "--ws-fills needs --loop")
            ws_url = check_ws_url(a.ws_url, a.test_exchange)
            try:
                import websockets  # noqa: F401
            except ImportError:
                raise Fatal(2, "--ws-fills needs the websockets package: pip install websockets")
    except Fatal as e:
        log.emit("fatal", code=e.code, message=str(e))
        print("order_bridge: " + str(e), file=sys.stderr)
        return e.code

    c = Client(base, key, secret, passphrase, a.timeout, a.test_exchange)
    b = LoopBridge(a, c, log, sys.stdout) if a.loop else Bridge(a, c, log)

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
        if a.loop:
            b.fetch_account()
        if ws_url:
            b.ws = WsFeed(ws_url, a.product, key, secret, passphrase, log, a.test_exchange)
            b.ws.start()
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
