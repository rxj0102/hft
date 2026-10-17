#!/usr/bin/env python3
"""A local stand-in for the Coinbase Exchange REST order endpoints, for tests and drills. Invented prices.

It checks what a real exchange would check (signature, key, passphrase, timestamp window, rate limit, post-only
against a configured book, size and tick) and can be told to misbehave. It is NOT the sandbox: it shows that the
bridge behaves against the documented API, not that the real sandbox agrees.

  ex = FakeExchange(key, secret_b64, passphrase); ex.start(); ex.url
  ex.best_bid / ex.best_ask : post-only orders that would cross are refused with 400
  ex.mode : "ok" | "down" (500) | "auth" (401) | "forbidden" (403) | "slow" (sleeps ex.delay before answering, still applies the effect)
            | "drop" (applies the effect, then closes the socket without answering)
  ex.skew : seconds added to the time it reports on /time
  ex.start_ws(); ex.ws_url : the `user` channel of the websocket feed (authenticated like the real one; sends received/open on
            a new order, match on a fill, done on completion or cancel). ex.ws_mode "ok" | "reject_auth"; ex.ws_mute holds events back
            until ex.ws_flush(); ex.ws_drop() closes every client; ex.before_cancel(oid) is a hook run just before a cancel by id.
"""
import base64
import hashlib
import hmac
import json
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class FakeExchange:
    def __init__(self, key, secret_b64, passphrase, rate=15.0, burst=30.0):
        self.key, self.secret, self.passphrase = key, base64.b64decode(secret_b64), passphrase
        self.lock = threading.Lock()
        self.orders = {}        # id -> dict (open orders only)
        self.done = {}          # id -> dict (finished orders: done_reason "filled" or "canceled")
        self.latency = 0.0      # seconds added to every answer
        self.by_coid = {}
        self.log = []           # (t, method, path, status)
        self.mode, self.delay, self.skew = "ok", 0.0, 0.0
        self.best_bid, self.best_ask = 81000.00, 81000.10
        self.rate, self.burst, self.tokens, self.t_tok = rate, burst, burst, time.monotonic()
        self.enforce_rate = True
        # faults for the smoke test's tests
        self.cancel_noop = False       # DELETE /orders/{id} answers 200 and does nothing
        self.omit_filled_size = False  # order records lack filled_size
        self.cross_http = None         # status returned for a crossing post-only order
        self.fail_post_number = None   # the Nth POST /orders is refused
        self.ghost_at = None           # at the Nth signed request a stray order appears
        self.nreq = 0
        self.nposts = 0
        self.posted = []               # (price, size, post_only) of every POST /orders
        self.ws_port, self.ws_loop, self.ws_clients = None, None, set()
        self.ws_mode, self.ws_mute, self.ws_unsent = "ok", False, []
        self.hide_done = False         # GET /orders/{id} of a finished order answers 404
        self.fail_get_order = False    # GET /orders/{id} answers 500
        self.ws_cancel_reason = "canceled"
        self.ws_silent = False         # subscribes fine, then never sends an event
        self.ws_subscriptions, self.ws_rejected = 0, 0
        self.before_cancel = None
        self.trade_id = 0
        self.honour_post_only = True   # False: a crossing "post-only" order is accepted (a broken exchange, for the smoke test's tests)
        self.httpd = None

    # ---- helpers for tests ----
    @property
    def url(self):
        return "http://127.0.0.1:%d" % self.httpd.server_address[1]

    def fill(self, oid, size=None):
        """The market trades through a resting order. size=None: fill the rest. Returns True if it was open."""
        with self.lock:
            o = self.orders.get(oid)
            if not o:
                return False
            total, done = float(o["size"]), float(o["filled_size"])
            add = total - done if size is None else min(float(size), total - done)
            done += add
            o["filled_size"] = "%.8f" % done
            o["executed_value"] = "%.8f" % (done * float(o["price"]))
            full = done >= total - 1e-12
            if full:
                o["status"] = "done"
                o["done_reason"] = "filled"
                self.done[oid] = self.orders.pop(oid)
            self.trade_id += 1
            match = {"type": "match", "trade_id": self.trade_id, "maker_order_id": oid, "taker_order_id": "other-" + str(self.trade_id),
                     "product_id": o["product_id"], "size": "%.8f" % add, "price": o["price"], "side": o["side"]}
            fin = {"type": "done", "reason": "filled", "order_id": oid, "remaining_size": "0"} if full else None
        self.ws_send(match)
        if fin:
            self.ws_send(fin)
        return True

    def cancel_elsewhere(self, oid):
        """Cancelled behind the bridge's back (by the exchange or another client)."""
        with self.lock:
            o = self.orders.pop(oid, None)
            if o:
                o["status"] = "done"
                o["done_reason"] = "canceled"
                self.done[oid] = o
        if o:
            self.ws_send({"type": "done", "reason": self.ws_cancel_reason, "order_id": oid, "remaining_size": o["size"]})
        return o is not None

    # ---- websocket `user` channel ----
    @property
    def ws_url(self):
        return "ws://127.0.0.1:%d" % self.ws_port

    def start_ws(self):
        import asyncio
        import websockets
        ready = threading.Event()
        ex = self

        async def handler(conn):
            try:
                sub = json.loads(await conn.recv())
            except Exception:  # noqa: BLE001
                return
            ts = str(sub.get("timestamp", ""))
            want = base64.b64encode(hmac.new(ex.secret, (ts + "GET/users/self/verify").encode(), hashlib.sha256).digest()).decode()
            good = (sub.get("type") == "subscribe" and "user" in (sub.get("channels") or []) and sub.get("key") == ex.key
                    and sub.get("passphrase") == ex.passphrase and hmac.compare_digest(str(sub.get("signature", "")), want))
            if not good or ex.ws_mode == "reject_auth":
                ex.ws_rejected += 1
                await conn.send(json.dumps({"type": "error", "message": "Authentication Failed", "reason": "invalid signature"}))
                await conn.close()
                return
            ex.ws_subscriptions += 1
            await conn.send(json.dumps({"type": "subscriptions", "channels": [{"name": "user", "product_ids": sub.get("product_ids", [])}]}))
            ex.ws_clients.add(conn)
            try:
                async for _ in conn:
                    pass
            except Exception:  # noqa: BLE001  a client that vanishes is normal
                pass
            finally:
                ex.ws_clients.discard(conn)

        async def main():
            ex.ws_loop = asyncio.get_running_loop()
            async with websockets.serve(handler, "127.0.0.1", 0) as server:
                ex.ws_port = server.sockets[0].getsockname()[1]
                ready.set()
                await asyncio.Future()

        threading.Thread(target=lambda: asyncio.run(main()), daemon=True).start()
        ready.wait(5)
        return self

    def _bcast(self, text):
        import asyncio

        async def go():
            for c in list(self.ws_clients):
                try:
                    await c.send(text)
                except Exception:  # noqa: BLE001
                    pass
        return asyncio.run_coroutine_threadsafe(go(), self.ws_loop)

    def ws_send(self, msg, wait=False):
        if self.ws_loop is None or self.ws_silent:
            return
        if self.ws_mute:
            self.ws_unsent.append(msg)
            return
        f = self._bcast(json.dumps(msg))
        if wait:
            f.result(2)

    def ws_flush(self):
        msgs, self.ws_unsent = self.ws_unsent, []
        self.ws_mute = False
        for m in msgs:
            self.ws_send(m, wait=True)

    def ws_drop(self):
        import asyncio

        async def go():
            for c in list(self.ws_clients):
                await c.close(1011)
        asyncio.run_coroutine_threadsafe(go(), self.ws_loop).result(2)

    def open_orders(self):
        with self.lock:
            return list(self.orders.values())

    def max_rate(self, window=1.0):
        with self.lock:
            ts = [t for t, m, p, s in self.log if p != "/time"]
        best = 0
        j = 0
        for i, t in enumerate(ts):
            while ts[j] < t - window:
                j += 1
            best = max(best, i - j + 1)
        return best

    def requests(self, method=None, prefix=None):
        with self.lock:
            return [r for r in self.log if (method is None or r[1] == method) and (prefix is None or r[2].startswith(prefix))]

    def start(self):
        ex = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def handle_any(self):
                u = urlparse(self.path)
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode() if n else ""
                status, payload, drop = ex.dispatch(self.command, self.path, u, body, self.headers)
                with ex.lock:
                    ex.log.append((time.monotonic(), self.command, u.path, status))
                if drop:
                    self.close_connection = True
                    self.connection.close()
                    return
                if ex.latency:
                    time.sleep(ex.latency)
                data = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = do_DELETE = handle_any

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()

    # ---- the API ----
    def dispatch(self, method, rawpath, u, body, h):
        if u.path == "/time":
            return 200, {"iso": "", "epoch": time.time() + self.skew}, False
        if u.path.startswith("/products/"):  # public market data, no signature needed
            parts = u.path.split("/")
            if len(parts) == 3:
                return 200, {"id": parts[2], "base_min_size": "0.00001", "base_increment": "0.00000001", "quote_increment": "0.01",
                             "status": "online", "post_only": False, "trading_disabled": False}, False
            if len(parts) == 4 and parts[3] == "ticker":
                return 200, {"bid": "%.2f" % self.best_bid, "ask": "%.2f" % self.best_ask, "price": "%.2f" % self.best_bid}, False
        if self.mode == "down":
            return 500, {"message": "internal"}, False
        if self.mode == "auth":
            return 401, {"message": "Invalid API Key"}, False
        if self.mode == "forbidden":
            return 403, {"message": "Forbidden"}, False
        # authentication: independent re-implementation of the documented algorithm
        ts = h.get("CB-ACCESS-TIMESTAMP", "")
        want = base64.b64encode(hmac.new(self.secret, (ts + method + rawpath + body).encode(), hashlib.sha256).digest()).decode()
        try:
            fresh = abs(float(ts) - (time.time() + self.skew)) <= 30
        except ValueError:
            fresh = False
        if (h.get("CB-ACCESS-KEY") != self.key or h.get("CB-ACCESS-PASSPHRASE") != self.passphrase
                or not hmac.compare_digest(h.get("CB-ACCESS-SIGN", ""), want)):
            return 401, {"message": "invalid signature or credentials"}, False
        if not fresh:
            return 400, {"message": "request timestamp expired"}, False
        with self.lock:
            now = time.monotonic()
            if self.enforce_rate:
                self.tokens = min(self.burst, self.tokens + (now - self.t_tok) * self.rate)
                self.t_tok = now
                if self.tokens < 1:
                    return 429, {"message": "Too Many Requests"}, False
                self.tokens -= 1
        if self.mode == "slow":
            time.sleep(self.delay)
        if method == "DELETE" and u.path.startswith("/orders/") and not u.path.startswith("/orders/client:") and self.before_cancel:
            self.before_cancel(u.path[len("/orders/"):])
            time.sleep(0.1)  # the websocket events of the hook reach the client before the HTTP answer does
        status, payload = self.apply(method, u, body)
        return status, payload, self.mode == "drop"

    def apply(self, method, u, body):
        q = parse_qs(u.query)
        with self.lock:
            self.nreq += 1
            if self.ghost_at and self.nreq == self.ghost_at:
                self.orders["ghost"] = {"id": "ghost", "client_oid": "ghost", "side": "buy", "price": "1.00", "size": "0.1",
                                        "product_id": "BTC-USD", "status": "open", "filled_size": "0", "executed_value": "0"}
            if method == "POST" and u.path == "/orders":
                self.nposts += 1
                if self.fail_post_number and self.nposts == self.fail_post_number:
                    return 400, {"message": "size or price increment invalid"}
                try:
                    o = json.loads(body)
                    price, size = float(o["price"]), float(o["size"])
                except (ValueError, KeyError, TypeError):
                    return 400, {"message": "invalid order"}
                if o.get("type") != "limit" or o.get("side") not in ("buy", "sell") or not o.get("client_oid"):
                    return 400, {"message": "invalid order"}
                if size < 0.00001 or abs(price * 100 - round(price * 100)) > 1e-6:
                    return 400, {"message": "size or price increment invalid"}
                self.posted.append((price, size, bool(o.get("post_only"))))
                if o.get("post_only") and self.honour_post_only and ((o["side"] == "buy" and price >= self.best_ask) or (o["side"] == "sell" and price <= self.best_bid)):
                    if self.cross_http:
                        return self.cross_http, {"message": "exchange trouble"}
                    return 400, {"message": "Post only mode"}
                if o["client_oid"] in self.by_coid:
                    return 400, {"message": "client_oid already used"}
                oid = str(uuid.uuid4())
                rec = {"id": oid, "client_oid": o["client_oid"], "side": o["side"], "price": o["price"], "size": o["size"],
                       "product_id": o.get("product_id"), "status": "open", "filled_size": "0.00000000", "executed_value": "0.00000000"}
                self.orders[oid] = rec
                self.by_coid[o["client_oid"]] = oid
                self.ws_send({"type": "received", "order_id": oid, "client_oid": o["client_oid"], "side": o["side"], "price": o["price"], "size": o["size"]})
                self.ws_send({"type": "open", "order_id": oid, "price": o["price"], "remaining_size": o["size"], "side": o["side"]})
                return 200, rec
            if method == "DELETE" and u.path == "/orders":
                pid = (q.get("product_id") or [None])[0]
                ids = [i for i, r in self.orders.items() if pid is None or r["product_id"] == pid]
                for i in ids:
                    del self.orders[i]
                    self.ws_send({"type": "done", "reason": self.ws_cancel_reason, "order_id": i, "remaining_size": "0"})
                return 200, ids
            if method == "DELETE" and u.path.startswith("/orders/client:"):
                oid = self.by_coid.get(u.path[len("/orders/client:"):])
                if oid in self.orders:
                    del self.orders[oid]
                    self.ws_send({"type": "done", "reason": self.ws_cancel_reason, "order_id": oid, "remaining_size": "0"})
                    return 200, oid
                return 404, {"message": "NotFound"}
            if method == "DELETE" and u.path.startswith("/orders/"):
                oid = u.path[len("/orders/"):]
                if oid in self.orders and self.cancel_noop:
                    return 200, oid
                if oid in self.orders:
                    o = self.orders.pop(oid)
                    o["status"], o["done_reason"] = "done", "canceled"
                    self.done[oid] = o
                    self.ws_send({"type": "done", "reason": self.ws_cancel_reason, "order_id": oid, "remaining_size": o["size"]})
                    return 200, oid
                return 404, {"message": "NotFound"}
            if method == "GET" and u.path.startswith("/orders/") and not u.path.startswith("/orders/client:"):
                oid = u.path[len("/orders/"):]
                if self.fail_get_order:
                    return 500, {"message": "internal"}
                rec = self.orders.get(oid) or (None if self.hide_done else self.done.get(oid))
                if rec and self.omit_filled_size:
                    rec = {k: v for k, v in rec.items() if k != "filled_size"}
                return (200, rec) if rec else (404, {"message": "NotFound"})
            if method == "GET" and u.path == "/orders":
                pid = (q.get("product_id") or [None])[0]
                return 200, [r for r in self.orders.values() if pid is None or r["product_id"] == pid]
        return 404, {"message": "NotFound"}
