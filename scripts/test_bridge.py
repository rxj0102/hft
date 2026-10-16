#!/usr/bin/env python3
"""Tests of scripts/order_bridge.py against the local fake exchange (scripts/fake_exchange.py). No network.

  python3 scripts/test_bridge.py

Each case states the expected outcome first. The fake exchange checks signatures with its own code; the signing
function is also compared with openssl, a third implementation. None of this talks to the real sandbox.
"""
import base64
import json
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fake_exchange  # noqa: E402
import order_bridge  # noqa: E402

BRIDGE = os.path.join(HERE, "order_bridge.py")
KEY, PASS = "key-AbC123xyz", "pass-QwErTy987"
SECRET = base64.b64encode(b"s3cret-bytes-for-tests-0123456789-abcdef").decode()
fails = 0


def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails += 1


def until(fn, timeout=8.0, step=0.02):
    t = time.monotonic() + timeout
    while time.monotonic() < t:
        if fn():
            return True
        time.sleep(step)
    return fn()


class Bridge:
    def __init__(self, ex, tmp, extra=(), env=None, test=True):
        self.log = os.path.join(tmp, "bridge.jsonl")
        e = dict(os.environ)
        e.pop("PYTHONUNBUFFERED", None)  # a bridge that forgets to flush must show up as silence, not be rescued by the environment
        e.update({"HFT_CB_KEY": KEY, "HFT_CB_SECRET": SECRET, "HFT_CB_PASSPHRASE": PASS})
        e.update(env or {})
        cmd = [sys.executable, BRIDGE, "--log", self.log, "--rate", "20", "--metrics-s", "0.2",
               "--metrics", os.path.join(tmp, "b.prom")] + (["--test-exchange", "--base-url", ex.url] if test else []) + list(extra)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e)

    def collect_reports(self):
        import threading
        self.reports = []

        def run():
            for l in self.p.stdout:
                try:
                    self.reports.append(json.loads(l))
                except ValueError:
                    pass
        self.reader = threading.Thread(target=run, daemon=True)
        self.reader.start()

    def reps(self, rt=None):
        return [r for r in list(self.reports) if rt is None or r["rt"] == rt]

    def send(self, *objs):
        for o in objs:
            self.p.stdin.write((json.dumps(o) + "\n").encode())
        self.p.stdin.flush()

    def events(self, name=None):
        try:
            rows = [json.loads(l) for l in open(self.log)]
        except FileNotFoundError:
            return []
        return [r for r in rows if name is None or r["event"] == name]

    def finish(self, how="eof", timeout=15):
        if how == "eof":
            self.p.stdin.close()
        elif how == "term":
            self.p.send_signal(signal.SIGTERM)
        elif how == "kill":
            self.p.kill()
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        err = self.p.stderr.read().decode()
        return self.p.returncode, err


def new(i, px, qty=5, side="buy", seg=1):
    return {"t": "new", "seg": seg, "id": i, "side": side, "px": px, "qty": qty, "type": 4}


def hello():
    return {"t": "hello", "v": 1, "px_scale": 10000, "qty_scale": 100000}


def fresh(tmp_name):
    import tempfile
    tmp = tempfile.mkdtemp(prefix=tmp_name)
    ex = fake_exchange.FakeExchange(KEY, SECRET, PASS).start()
    return ex, tmp


def test_signing():
    print("signing and units")
    ts, method, path, body = "1700000000.123", "POST", "/orders", '{"a":1}'
    mine = order_bridge.sign(SECRET, ts, method, path, body)
    keyhex = base64.b64decode(SECRET).hex()
    out = subprocess.run(["openssl", "dgst", "-sha256", "-mac", "HMAC", "-macopt", "hexkey:" + keyhex, "-binary"],
                         input=(ts + method + path + body).encode(), capture_output=True)
    check(out.returncode == 0 and base64.b64encode(out.stdout).decode() == mine, "signature equals openssl HMAC-SHA256 over timestamp+method+path+body")
    check(order_bridge.sign(SECRET, ts, "post", path, body) == mine, "method is upper-cased before signing")
    check(order_bridge.sign(SECRET, ts, method, path, body + " ") != mine, "a different body gives a different signature")
    check(order_bridge.fmt_price(819080300, 10000) == "81908.03", "price 819080300 -> 81908.03")
    check(order_bridge.fmt_price(819080350, 10000) is None, "a price off the cent is refused")
    check(order_bridge.fmt_size(5, 100000) == "0.00005", "5 lots -> 0.00005")
    check(order_bridge.fmt_size(123456, 100000) == "1.23456", "123456 lots -> 1.23456")
    check(order_bridge.fmt_size(0, 100000) is None, "zero size refused")


def test_guards():
    print("guards: URL, keys")
    ex, tmp = fresh("g1")
    for url, test in [("https://api.exchange.coinbase.com", False), ("https://api-public.sandbox.exchange.coinbase.com.evil.com", False),
                      ("http://api-public.sandbox.exchange.coinbase.com", False), ("http://10.0.0.5:80", True), ("https://127.0.0.1:9", True)]:
        try:
            order_bridge.check_url(url, test)
            ok = False
        except order_bridge.Fatal as e:
            ok = e.code == 2
        check(ok, "refused: %s%s" % (url, " (test mode)" if test else ""))
    check(order_bridge.check_url("https://api-public.sandbox.exchange.coinbase.com/", False) == order_bridge.SANDBOX_URL, "the sandbox URL is accepted")
    # production URL through the real entry point: exit 2, nothing sent anywhere
    # If this guard were broken the bridge would try to talk to production. The proxy below points at a dead local
    # port, so even then nothing could leave the machine: the test fails by connection refused, not by a real request.
    dead = dict(os.environ, HFT_CB_KEY=KEY, HFT_CB_SECRET=SECRET, HFT_CB_PASSPHRASE=PASS, HTTPS_PROXY="http://127.0.0.1:9",
                https_proxy="http://127.0.0.1:9", HTTP_PROXY="http://127.0.0.1:9", http_proxy="http://127.0.0.1:9", NO_PROXY="", no_proxy="")
    p = subprocess.run([sys.executable, BRIDGE, "--base-url", "https://api.exchange.coinbase.com", "--max-errors", "1"],
                       capture_output=True, env=dead, input=b"", timeout=30)
    check(p.returncode == 2 and b"refusing base URL" in p.stderr, "production URL: exit 2")
    env = {k: v for k, v in os.environ.items() if not k.startswith("HFT_CB_")}
    p = subprocess.run([sys.executable, BRIDGE, "--test-exchange", "--base-url", ex.url], capture_output=True, env=env, input=b"")
    check(p.returncode == 2 and b"HFT_CB_KEY" in p.stderr, "missing keys: exit 2, names the variables")
    check(len(ex.requests()) == 0, "no request reached the exchange in either case")
    ex.stop()


def test_flow():
    print("order flow: place, replace, cancel, coalesce, flat at EOF")
    ex, tmp = fresh("f1")
    ex.orders["stale1"] = {"id": "stale1", "client_oid": "x", "side": "buy", "price": "1.00", "size": "0.1", "product_id": "BTC-USD"}
    b = Bridge(ex, tmp)
    check(until(lambda: b.events("start_flat")), "started")
    check(len(ex.open_orders()) == 0 and b.events("start_flat")[0]["flat"], "orders already on the account are cancelled at start")
    b.send(hello(), new(1, 810000000), new(2, 810200000, side="sell"))
    check(until(lambda: len(ex.open_orders()) == 2), "two orders reach the exchange")
    o = {r["side"]: r for r in ex.open_orders()}
    check(o["buy"]["price"] == "81000.00" and o["sell"]["price"] == "81020.00" and o["buy"]["size"] == "0.00005", "prices and sizes are in dollars and coins")
    check(all(r["client_oid"] for r in o.values()) and len({r["client_oid"] for r in o.values()}) == 2, "each has its own client_oid")
    n_before = len(ex.requests())
    for k in range(60):  # a burst of replaces: only the last should matter
        b.send({"t": "rep", "seg": 1, "id": 1, "px": 810000000 - 100 * (k + 1), "qty": 5})
    check(until(lambda: any(abs(float(r["price"]) - (81000.00 - 0.60)) < 1e-9 for r in ex.open_orders() if r["side"] == "buy")),
          "the exchange ends at the last price (81000.00 - 0.60)")
    sent = len(ex.requests()) - n_before
    check(sent < 30, "60 replaces became %d requests (coalesced)" % sent)
    b.send({"t": "can", "seg": 1, "id": 2})
    check(until(lambda: len(ex.open_orders()) == 1), "cancel removes the order")
    rc, err = b.finish("eof")
    check(rc == 0 and len(ex.open_orders()) == 0, "end of input: cancel-all, nothing left working, exit 0 (rc=%s)" % rc)
    check(b.events("exit_flat")[-1]["flat"] is True, "exit_flat logged")
    ex.stop()


def test_pacing():
    print("pacing: --rate 8 holds under a stream of changes")
    ex, tmp = fresh("p1")
    b = Bridge(ex, tmp, ["--rate", "8"])
    until(lambda: b.events("start_flat"))
    b.send(hello(), new(1, 810000000), new(2, 810200000, side="sell"))
    t_end = time.monotonic() + 3.0
    k = 0
    while time.monotonic() < t_end:  # both quotes move all the time
        k += 1
        b.send({"t": "rep", "seg": 1, "id": 1, "px": 810000000 - 100 * (k % 50), "qty": 5},
               {"t": "rep", "seg": 1, "id": 2, "px": 810200000 + 100 * (k % 50), "qty": 5})
        time.sleep(0.01)
    mx = ex.max_rate()
    check(mx <= 9, "at most %d requests in any second (limit set 8, exchange allows 15)" % mx)
    check(len(ex.requests("POST")) >= 8, "and it kept working (%d POSTs)" % len(ex.requests("POST")))
    b.finish("eof", 30)
    ex.stop()


def test_cancel_before_place():
    print("a replace is a cancel then a new order, never the reverse")
    ex, tmp = fresh("c1")
    b = Bridge(ex, tmp, ["--rate", "50"])
    b.send(hello(), new(1, 810000000))
    until(lambda: len(ex.open_orders()) == 1)
    mark = len(ex.requests())
    b.send({"t": "rep", "seg": 1, "id": 1, "px": 809900000, "qty": 5})
    until(lambda: any(r["price"] == "80990.00" for r in ex.open_orders()))
    seq = [(m, p) for t, m, p, s in ex.requests()[mark:]]
    di = next((i for i, (m, p) in enumerate(seq) if m == "DELETE" and p.startswith("/orders/")), None)
    pi = next((i for i, (m, p) in enumerate(seq) if m == "POST"), None)
    check(di is not None and pi is not None and di < pi, "DELETE %s before POST %s" % (di, pi))
    check(len(ex.open_orders()) == 1, "one order afterwards, not two")
    b.finish("eof")
    ex.stop()


def test_segment_end_and_priority():
    print("segment end clears the segment; a cancel goes before a new order that is waiting")
    ex, tmp = fresh("z1")
    b = Bridge(ex, tmp, ["--rate", "3"])
    until(lambda: b.events("start_flat"))
    b.send(hello(), new(1, 810000000), new(2, 810200000, side="sell"))
    until(lambda: len(ex.open_orders()) == 2, 10)
    b.send({"t": "end", "seg": 1, "reason": "stale feed"})
    check(until(lambda: len(ex.open_orders()) == 0, 10), "segment end: the orders are cancelled without any cancel lines")
    b.send(new(1, 809000000, seg=2))  # ids restart in a new segment
    check(until(lambda: len(ex.open_orders()) == 1 and ex.open_orders()[0]["price"] == "80900.00", 10), "a new segment may reuse id 1")
    mark = len(ex.requests())
    # one batch: move order 1 and add order 3. The cancel (risk down) must precede the POST.
    b.send({"t": "rep", "seg": 2, "id": 1, "px": 808000000, "qty": 5}, new(3, 807000000, seg=2))
    until(lambda: {o["price"] for o in ex.open_orders()} == {"80800.00", "80700.00"}, 10)
    seq = [m for t, m, p, st in ex.requests()[mark:] if m in ("DELETE", "POST")]
    check(seq[:1] == ["DELETE"], "first request after the batch: %s" % seq[:3])
    b.finish("eof", 20)
    ex.stop()


def test_signals():
    print("SIGTERM and SIGKILL")
    ex, tmp = fresh("s1")
    b = Bridge(ex, tmp)
    b.send(hello(), new(1, 810000000), new(2, 810200000, side="sell"))
    until(lambda: len(ex.open_orders()) == 2)
    rc, _ = b.finish("term")
    check(rc == 0 and len(ex.open_orders()) == 0, "SIGTERM: flat, exit 0")
    # kill -9 leaves the orders; the next start clears them (this is the gap the supervisor's restart closes)
    b = Bridge(ex, tmp)
    b.send(hello(), new(1, 810000000))
    until(lambda: len(ex.open_orders()) == 1)
    b.finish("kill")
    check(len(ex.open_orders()) == 1, "SIGKILL: the order is still resting (expected, nothing can run)")
    b2 = Bridge(ex, tmp)
    check(until(lambda: len(ex.open_orders()) == 0), "the next start cancels it")
    b2.finish("eof")
    ex.stop()


def test_errors():
    print("exchange misbehaviour")
    # 1. auth refused: exit 4, not retried
    ex, tmp = fresh("e1")
    ex.mode = "auth"
    b = Bridge(ex, tmp)
    rc, err = b.finish("eof")
    check(rc == 4, "401 at start: exit 4 (rc=%s)" % rc)
    ex.stop()
    ex, tmp = fresh("e2")
    b = Bridge(ex, tmp, env={"HFT_CB_PASSPHRASE": "wrong"})
    rc, _ = b.finish("eof")
    check(rc == 4, "wrong passphrase: exit 4 (rc=%s)" % rc)
    ex.stop()
    ex, tmp = fresh("e1b")
    ex.mode = "forbidden"
    b = Bridge(ex, tmp)
    rc, _ = b.finish("eof")
    check(rc == 4, "403 at start: exit 4 (rc=%s)" % rc)
    ex.stop()
    # 2. exchange down
    ex, tmp = fresh("e3")
    ex.mode = "down"
    b = Bridge(ex, tmp, ["--max-errors", "3"])
    t0 = time.monotonic()
    rc, _ = b.finish("eof", 30)
    check(rc == 1, "5xx forever: exit 1 after 3 errors, restartable (rc=%s, %.1f s)" % (rc, time.monotonic() - t0))
    ex.stop()
    # 2b. exchange goes down in the middle of a run: errors are counted, the bridge gives up with a restartable code
    ex, tmp = fresh("e3b")
    b = Bridge(ex, tmp, ["--max-errors", "3", "--sync-s", "0.3"])
    b.send(hello(), new(1, 810000000))
    until(lambda: len(ex.open_orders()) == 1)
    ex.mode = "down"
    t0 = time.monotonic()
    until(lambda: b.p.poll() is not None, 40)  # it must stop by itself, input is still open
    rc = b.p.poll()
    b.finish("eof", 20)
    check(rc == 1 and b.events("fatal"), "5xx in mid-run: gives up by itself with exit 1 (rc=%s, %.1f s)" % (rc, time.monotonic() - t0))
    check(len(b.events("http_5xx")) >= 3 and b.events("http_5xx")[2]["consecutive"] == 3, "the errors were counted")
    ex.stop()
    # 3. clock skew
    ex, tmp = fresh("e4")
    ex.skew = 100
    b = Bridge(ex, tmp)
    rc, _ = b.finish("eof")
    check(rc == 4 and not ex.requests("POST"), "clock 100 s off the exchange: exit 4, no order sent (rc=%s)" % rc)
    ex.stop()
    # 4. 429 from a tight exchange limit: backs off, still gets there
    ex, tmp = fresh("e5")
    ex.rate, ex.burst, ex.tokens = 2.0, 2.0, 2.0
    b = Bridge(ex, tmp, ["--rate", "40"])
    b.send(hello(), new(1, 810000000), new(2, 810200000, side="sell"), new(3, 809900000))
    ok = until(lambda: len(ex.open_orders()) == 3, 20)
    check(ok and b.events("rate_limited"), "429s seen (%d) and all three orders placed after backing off" % len(b.events("rate_limited")))
    got429 = sum(1 for r in ex.requests() if r[3] == 429)
    check(got429 <= 6, "the exchange refused %d requests, not a stream of them" % got429)
    bo = [e["backoff_s"] for e in b.events("rate_limited")]
    check(bo and min(bo) >= 1.0, "each 429 pauses at least a second: %s" % bo)
    b.finish("eof", 30)
    check(len(ex.open_orders()) == 0, "and flat afterwards")
    ex.stop()
    # 5. post-only refusal: not retried unchanged; a new price is tried
    ex, tmp = fresh("e6")
    ex.best_ask = 80000.00  # the bid at 81000.00 would cross
    b = Bridge(ex, tmp)
    b.send(hello(), new(1, 810000000))
    until(lambda: b.events("order_rejected"))
    time.sleep(0.5)
    posts = len(ex.requests("POST"))
    check(posts == 1, "refused once, not retried unchanged (%d POSTs)" % posts)
    b.send({"t": "rep", "seg": 1, "id": 1, "px": 799000000, "qty": 5})
    check(until(lambda: len(ex.open_orders()) == 1), "a new price is tried and rests")
    b.finish("eof")
    ex.stop()
    # 6. lost reply to a POST: the order may exist; it must not be left behind
    ex, tmp = fresh("e7")
    b = Bridge(ex, tmp, ["--timeout", "1"])
    until(lambda: b.events("start_flat"))
    ex.mode = "drop"
    b.send(hello(), new(1, 810000000))
    until(lambda: b.events("net_error"))
    ex.mode = "ok"
    time.sleep(1.5)
    check(b.events("net_error") and len(ex.requests("DELETE", "/orders/client:")) >= 1, "unknown outcome -> cancel by client_oid")
    rc, _ = b.finish("eof")
    check(rc == 0 and len(ex.open_orders()) == 0, "nothing left on the exchange")
    ex.stop()
    # 7. an order that vanished (filled elsewhere) is noticed
    ex, tmp = fresh("e8")
    b = Bridge(ex, tmp, ["--sync-s", "0.3"])
    b.send(hello(), new(1, 810000000))
    until(lambda: len(ex.open_orders()) == 1)
    first = ex.open_orders()[0]["id"]
    with ex.lock:
        ex.orders.clear()
    check(until(lambda: b.events("order_vanished")), "order_vanished logged")
    check(until(lambda: len(ex.open_orders()) == 1 and ex.open_orders()[0]["id"] != first), "wanted order placed again (the strategy still wants it)")
    # a stray order placed behind our back is cancelled
    with ex.lock:
        ex.orders["stray"] = {"id": "stray", "client_oid": "z", "side": "buy", "price": "1.00", "size": "0.1", "product_id": "BTC-USD"}
    check(until(lambda: "stray" not in {o["id"] for o in ex.open_orders()}), "a stray order is cancelled")
    b.finish("eof")
    ex.stop()


def test_limits_and_secrets():
    print("local limits and secrets")
    ex, tmp = fresh("l1")
    b = Bridge(ex, tmp, ["--max-notional", "10", "--max-open", "2"])
    until(lambda: b.events("start_flat"))
    # 5 lots at 81000 = $4.05; 1000 lots = $810
    b.send(hello(), new(1, 810000000, qty=1000), new(2, 810000000, qty=5), new(3, 809900000, qty=5), new(4, 809800000, qty=5),
           new(5, 810005000 + 50, qty=5))
    until(lambda: len(ex.open_orders()) == 2)
    time.sleep(0.5)
    check(len(ex.open_orders()) == 2, "max-open 2 holds (%d resting)" % len(ex.open_orders()))
    check(not any(r["size"] == "0.01000" for r in ex.open_orders()), "the $810 order was not sent")
    check(len(ex.requests("POST")) == 2, "and no refused order cost a request (%d POSTs)" % len(ex.requests("POST")))
    rc, err = b.finish("eof")
    blob = open(b.log).read() + err + open(os.path.join(tmp, "b.prom")).read()
    check(KEY not in blob and SECRET not in blob and PASS not in blob, "key, secret and passphrase appear in neither log, stderr nor metrics")
    prom = open(os.path.join(tmp, "b.prom")).read()
    check("hft_bridge_placed_total 2" in prom and "hft_bridge_refused_local_total" in prom, "metrics file written")
    ex.stop()


def loop_bridge(ex, tmp, extra=()):
    b = Bridge(ex, tmp, ["--loop"] + list(extra))
    b.collect_reports()
    return b


def px(dollars):
    return int(round(dollars * 10000))


def test_loop():
    print("closed loop: every request gets exactly one report")
    ex, tmp = fresh("lp1")
    b = loop_bridge(ex, tmp, ["--sync-s", "0.3"])
    until(lambda: b.events("start_flat"))
    # new -> accepted
    b.send(hello(), new(1, px(81000.00)), new(2, px(81020.00), side="sell"))
    check(until(lambda: len(b.reps("accepted")) == 2), "two orders: two accepted reports")
    a = {r["id"]: r for r in b.reps("accepted")}
    check(a[1]["seg"] == 1 and a[1]["px"] == px(81000.00) and a[1]["qty"] == 5 and a[1]["leaves"] == 5 and a[1]["side"] == "buy", "accepted carries segment, price, size, side: %s" % a[1])
    check(len(ex.open_orders()) == 2, "and both rest on the exchange")
    # replace -> cancel then new, one replaced report
    mark = len(ex.requests())
    b.send({"t": "rep", "seg": 1, "id": 1, "px": px(80990.00), "qty": 7})
    check(until(lambda: len(b.reps("replaced")) == 1), "replace -> replaced")
    r = b.reps("replaced")[0]
    check(r["px"] == px(80990.00) and r["qty"] == 7 and r["leaves"] == 7, "replaced carries the new price and open size: %s" % r)
    seq = [m for t, m, p, s_ in ex.requests()[mark:] if m in ("DELETE", "POST")]
    check(seq[:2] == ["DELETE", "POST"], "as a cancel and then a new order: %s" % seq)
    check(sorted(o["price"] for o in ex.open_orders()) == ["80990.00", "81020.00"], "and the exchange shows the new price")
    # cancel -> canceled
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: len(b.reps("canceled")) == 1), "cancel -> canceled")
    c = b.reps("canceled")[0]
    check(c["id"] == 1 and c["leaves"] == 0 and c["qty"] == 7 and c["reason"] == "user", "canceled carries the removed size: %s" % c)
    check(len(ex.open_orders()) == 1, "one order left")
    # unknown ids
    b.send({"t": "can", "seg": 1, "id": 99}, {"t": "rep", "seg": 1, "id": 98, "px": px(1.0), "qty": 5})
    check(until(lambda: len(b.reps("cancel_rejected")) == 1 and len(b.reps("replace_rejected")) == 1), "unknown order: cancel_rejected and replace_rejected")
    # every request got exactly one report
    n_req = 6
    check(len(b.reports) == n_req, "6 requests, %d reports" % len(b.reports))
    rc, _ = b.finish("eof")
    check(rc == 0 and not ex.open_orders(), "end of input: flat, exit 0")
    ex.stop()


def test_loop_refusals():
    print("closed loop: refusals and losses")
    ex, tmp = fresh("lp2")
    ex.best_ask = 80000.00
    b = loop_bridge(ex, tmp, ["--max-open", "1", "--max-notional", "10"])
    until(lambda: b.events("start_flat"))
    b.send(hello(), new(1, px(81000.00)))                     # would cross: refused by the exchange
    check(until(lambda: b.reps("rejected")), "post-only cross -> rejected")
    r = b.reps("rejected")[0]
    check(r["reason"] == "cross" and r["id"] == 1, "reason cross: %s" % r)
    check(len(ex.requests("POST")) == 1, "not retried")
    b.send(new(2, px(79000.00)))                              # fine
    check(until(lambda: b.reps("accepted")), "a valid order is accepted")
    b.send(new(3, px(79001.00)))                              # beyond max-open 1
    check(until(lambda: len(b.reps("rejected")) == 2), "beyond --max-open -> rejected, nothing sent")
    check(b.reps("rejected")[1]["reason"] == "badqty" and len(ex.requests("POST")) == 2, "reason badqty, no request: %s" % b.reps("rejected")[1])
    b.send({"t": "can", "seg": 1, "id": 2}, new(4, px(79000.00), qty=1000))   # $790 > $10 cap
    check(until(lambda: len(b.reps("rejected")) == 3), "beyond --max-notional -> rejected")
    check(b.reps("rejected")[2]["reason"] == "badqty" and not ex.open_orders(), "nothing resting")
    # replace into a cross: the old order is gone, the answer is canceled
    b.send(new(5, px(79000.00)))
    until(lambda: len(b.reps("accepted")) == 2)
    ex.best_bid, ex.best_ask = 0.0, 78000.00
    ncan = len(b.reps("canceled"))
    b.send({"t": "rep", "seg": 1, "id": 5, "px": px(79500.00), "qty": 5})
    check(until(lambda: len(b.reps("canceled")) == ncan + 1), "replace refused by post-only -> canceled (the order is gone)")
    check(b.reps("canceled")[-1]["reason"] == "cross" and not ex.open_orders(), "reason cross, exchange empty")
    # a lost reply to a POST: the order may exist; the answer is rejected and nothing is left behind
    ex.best_ask = 1e9
    ex.mode = "drop"
    b.send(new(6, px(79000.00)))
    until(lambda: len(b.reps("rejected")) == 4, 15)
    ex.mode = "ok"
    time.sleep(0.5)
    check(len(b.reps("rejected")) == 4 and b.reps("rejected")[3]["reason"] == "other", "lost reply -> rejected(other): %s" % [(r["rt"], r["id"], r["reason"]) for r in b.reports])
    check(not ex.open_orders(), "and the order that may have been created was cancelled by client id")
    rc, _ = b.finish("eof")
    check(rc == 0, "exit 0")
    ex.stop()


def test_loop_fills_and_vanished():
    print("closed loop: fills and orders that leave")
    ex, tmp = fresh("lp3")
    b = loop_bridge(ex, tmp, ["--sync-s", "0.3"])
    until(lambda: b.events("start_flat"))
    b.send(hello(), new(1, px(81000.00), qty=10), new(2, px(81020.00), qty=10, side="sell"), new(3, px(80900.00), qty=10))
    until(lambda: len(b.reps("accepted")) == 3)
    oid = {o["client_oid"]: o["id"] for o in ex.open_orders()}
    by_side = {o["price"]: o["id"] for o in ex.open_orders()}
    ex.fill(by_side["81000.00"], 0.00004)                     # 4 of 10 lots
    check(until(lambda: b.reps("fill")), "partial fill is found by the sync")
    f = b.reps("fill")[0]
    check(f["id"] == 1 and f["qty"] == 4 and f["leaves"] == 6 and f["px"] == px(81000.00) and f["side"] == "buy", "fill: 4 lots, 6 left: %s" % f)
    time.sleep(0.8)
    check(len(b.reps("fill")) == 1, "and reported once, not at every sync")
    ex.fill(by_side["81000.00"], 0.00003)                     # 3 more: the report carries the new part only
    check(until(lambda: len(b.reps("fill")) == 2), "a second partial fill")
    f = b.reps("fill")[1]
    check(f["qty"] == 3 and f["leaves"] == 3, "3 lots, 3 left: %s" % f)
    # cancelling a part-filled order removes only what is left
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: b.reps("canceled")), "cancel of the part-filled order")
    c = b.reps("canceled")[0]
    check(c["id"] == 1 and c["qty"] == 3 and c["leaves"] == 0, "the cancel removes the 3 lots that were left: %s" % c)
    b.send(new(1, px(81000.00), qty=10))                      # reuse the id for the old fill-to-the-end check
    until(lambda: len(b.reps("accepted")) == 4)
    by_side = {o["price"]: o["id"] for o in ex.open_orders()}
    ex.fill(by_side["81000.00"])
    check(until(lambda: len(b.reps("fill")) == 3), "an order filled in one go")
    f = b.reps("fill")[2]
    check(f["qty"] == 10 and f["leaves"] == 0, "10 lots, nothing left: %s" % f)
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: b.reps("cancel_rejected")), "cancelling a filled order is refused")
    ex.cancel_elsewhere(by_side["81020.00"])
    check(until(lambda: len(b.reps("canceled")) == 2), "an order cancelled elsewhere is reported cancelled")
    c = b.reps("canceled")[1]
    check(c["id"] == 2 and c["leaves"] == 0 and c["qty"] == 10 and c["reason"] == "other", "cancelled: %s" % c)
    # an order that finished while we were cancelling it
    ex.fill(by_side["80900.00"])
    b.send({"t": "can", "seg": 1, "id": 3})
    check(until(lambda: len(b.reps("fill")) == 4 or len(b.reps("canceled")) == 3), "filled while being cancelled: resolved")
    # a stray order placed behind our back is cancelled by the sync (loop mode has its own sync)
    with ex.lock:
        ex.orders["stray"] = {"id": "stray", "client_oid": "z", "side": "buy", "price": "1.00", "size": "0.1", "product_id": "BTC-USD",
                              "filled_size": "0", "executed_value": "0"}
    check(until(lambda: "stray" not in {o["id"] for o in ex.open_orders()}), "a stray order is cancelled")
    rc, _ = b.finish("eof")
    check(rc == 0, "exit 0")
    ex.stop()

    # end of segment with an order still resting: cancelled, no report. The sync is far away, so only the
    # segment-end handling can do it.
    ex, tmp = fresh("lp3b")
    b = loop_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("start_flat"))
    b.send(hello(), new(4, px(80800.00)))
    until(lambda: len(b.reps("accepted")) == 1)
    n = len(b.reports)
    b.send({"t": "end", "seg": 1, "reason": "test"})
    check(until(lambda: not ex.open_orders()), "segment end: an orphan is cancelled")
    time.sleep(0.3)
    check(len(b.reports) == n and b.events("orphan_order"), "without a report")
    rc, _ = b.finish("eof")
    check(rc == 0, "exit 0")
    ex.stop()


def test_loop_latency_and_segments():
    print("closed loop: slow exchange, several segments")
    ex, tmp = fresh("lp4")
    ex.latency = 0.15
    b = loop_bridge(ex, tmp)
    until(lambda: b.events("start_flat"), 20)
    b.send(hello(), new(1, px(81000.00)), new(1, px(79000.00), seg=2))
    check(until(lambda: len(b.reps("accepted")) == 2, 20), "same id in two segments: two accepted, each tagged")
    check(sorted(r["seg"] for r in b.reps("accepted")) == [1, 2], "segments 1 and 2")
    t0 = time.monotonic()
    b.send({"t": "rep", "seg": 2, "id": 1, "px": px(79010.00), "qty": 5})
    until(lambda: b.reps("replaced"), 20)
    check(time.monotonic() - t0 >= 0.3, "a replace takes two round trips (%.2f s at 0.15 s each way)" % (time.monotonic() - t0))
    ids = [(r["seg"], r["id"], r["rt"]) for r in b.reports]
    check(ids == [(1, 1, "accepted"), (2, 1, "accepted"), (2, 1, "replaced")], "reports in request order: %s" % ids)
    rc, _ = b.finish("eof", 30)
    check(rc == 0 and not ex.open_orders(), "flat")
    ex.stop()


def test_hft_live_mirror(live):
    if not live:
        return
    print("hft_live --orders-fd into the bridge, on a recorded capture")
    cap = os.path.join(HERE, "..", "data", "btc2.l2")
    if not os.path.exists(cap):
        print("  skip (no recorded capture in data/)")
        return
    ex, tmp = fresh("m1")
    ex.best_bid, ex.best_ask = 0.0, 1e9  # never cross
    bl = os.path.join(tmp, "bridge.jsonl")
    env = dict(os.environ, HFT_CB_KEY=KEY, HFT_CB_SECRET=SECRET, HFT_CB_PASSPHRASE=PASS)
    r, w = os.pipe()
    bp = subprocess.Popen([sys.executable, BRIDGE, "--test-exchange", "--base-url", ex.url, "--log", bl, "--rate", "50", "--max-notional", "50"],
                          stdin=r, env=env, stderr=subprocess.PIPE)
    os.close(r)
    lp = subprocess.run([live, "--from", cap, "--pace", "4", "--orders-fd", str(w)], pass_fds=(w,), capture_output=True)
    os.close(w)
    bp.wait(60)
    check(lp.returncode == 0, "hft_live exit 0")
    check(bp.returncode == 0 and len(ex.open_orders()) == 0, "bridge exit 0, exchange flat afterwards (rc=%s)" % bp.returncode)
    posts, dels = len(ex.requests("POST")), len(ex.requests("DELETE"))
    check(posts >= 4 and dels >= 3, "orders followed the strategy during the run: %d POST, %d DELETE" % (posts, dels))
    ex.stop()


def prom_value(tmp, name):
    try:
        for l in open(os.path.join(tmp, "b.prom")):
            if l.startswith(name + " "):
                return float(l.split()[1])
    except OSError:
        pass
    return None


def ws_bridge(ex, tmp, extra=()):
    return loop_bridge(ex, tmp, ["--ws-fills", "--ws-url", ex.ws_url] + list(extra))


def test_ws_fills():
    print("websocket fills")
    # 1. a fill is reported at once by the socket, with the match price, and the sync (600 s away) has nothing to do with it
    ex, tmp = fresh("ws1")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    check(until(lambda: b.events("ws_subscribed")), "the bridge authenticates and subscribes (the fake checks the signature itself)")
    b.send(hello(), new(1, px(81000.00), qty=10))
    check(until(lambda: b.reps("accepted")), "order accepted")
    by = {o["price"]: o["id"] for o in ex.open_orders()}
    t0 = time.monotonic()
    ex.fill(by["81000.00"], 0.00004)
    check(until(lambda: b.reps("fill"), 2.0), "a partial fill is reported without waiting for a sync")
    lat = time.monotonic() - t0
    f = b.reps("fill")[0] if b.reps("fill") else {}
    check(f.get("qty") == 4 and f.get("leaves") == 6 and f.get("px") == px(81000.00), "4 lots, 6 left, at the match price: %s" % f)
    check(lat < 1.0, "in %.0f ms" % (lat * 1000))
    ex.fill(by["81000.00"])
    check(until(lambda: len(b.reps("fill")) == 2), "the rest")
    f = b.reps("fill")[1]
    check(f["qty"] == 6 and f["leaves"] == 0, "6 lots, nothing left: %s" % f)
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: b.reps("cancel_rejected")), "a filled order cannot be cancelled")
    time.sleep(0.6)
    check(len(b.reps("fill")) == 2, "and nothing is reported twice")
    # events that do not concern a working order are counted, not reported
    n = len(b.reports)
    ex.ws_send({"type": "match", "maker_order_id": "somebody-else", "taker_order_id": "x", "size": "1", "price": "1"})
    b.send(new(2, px(80990.00), qty=10))
    check(until(lambda: len(b.reps("accepted")) == 2), "second order")
    by2 = [o["id"] for o in ex.open_orders()][0]
    ex.ws_send({"type": "match", "maker_order_id": by2, "taker_order_id": "x", "size": "abc", "price": "81000.00"})
    ex.ws_send({"type": "match", "maker_order_id": by2, "taker_order_id": "x", "size": "-1", "price": "81000.00"})
    ex.ws_send({"type": "match", "maker_order_id": by2, "taker_order_id": "x", "size": "0.00001", "price": "0"})
    time.sleep(0.6)
    check(len(b.reps("fill")) == 2, "unknown orders, a size that is not a number, a negative size and a zero price report nothing")
    check(until(lambda: (prom_value(tmp, "hft_bridge_ws_matches_unknown_order_total") or 0) >= 1 and (prom_value(tmp, "hft_bridge_ws_matches_bad_total") or 0) >= 3, 3.0),
          "and are counted (%s unknown, %s bad)" % (prom_value(tmp, "hft_bridge_ws_matches_unknown_order_total"), prom_value(tmp, "hft_bridge_ws_matches_bad_total")))
    check(prom_value(tmp, "hft_bridge_ws_fills_total") == 2 and prom_value(tmp, "hft_bridge_ws_subscribed") == 1, "metrics: two websocket fills, subscribed")
    check(not ex.requests("DELETE", "/orders/"), "no cancel was sent for the order that had filled")
    ex.ws_send({"type": "match", "maker_order_id": by2, "taker_order_id": "x", "size": "0.00020", "price": "80990.00"})   # more than the order's size
    check(until(lambda: len(b.reps("fill")) == 3), "a match larger than the order")
    f = b.reps("fill")[2]
    check(f["qty"] == 10 and f["leaves"] == 0, "is capped at the order's size: %s" % f)
    ex._bcast("this is not json").result(2)
    ex._bcast("[1, 2, 3]").result(2)
    ex._bcast(json.dumps({"type": "match"})).result(2)
    time.sleep(0.5)
    b.send(new(3, px(80980.00), qty=10))
    check(until(lambda: len(b.reps("accepted")) == 3), "frames that are not JSON, not an object, or have no fields do not stop the bridge")
    check((prom_value(tmp, "hft_bridge_ws_bad_messages_total") or 0) >= 2, "and the first two are counted (%s)" % prom_value(tmp, "hft_bridge_ws_bad_messages_total"))
    # the same key used again by a new order starts from nothing; and our order may be named as the taker
    b.send(new(1, px(80970.00), qty=10))
    check(until(lambda: len(b.reps("accepted")) == 4), "an id used again")
    again = [o for o in ex.open_orders() if o["price"] == "80970.00"][0]["id"]
    ex.fill(again, 0.00004)
    check(until(lambda: len(b.reps("fill")) == 4), "gets a fill")
    f = b.reps("fill")[3]
    check(f["qty"] == 4 and f["leaves"] == 6, "of its own 4 lots, not of the earlier order's tally: %s" % f)
    ex.ws_send({"type": "match", "maker_order_id": "someone", "taker_order_id": again, "size": "0.00002", "price": "80970.00"})
    check(until(lambda: len(b.reps("fill")) == 5), "a match that names our order as the taker counts as well")
    f = b.reps("fill")[4]
    check(f["qty"] == 2 and f["leaves"] == 4, "2 lots: %s" % f)
    rc, _ = b.finish("eof")
    check(rc == 0, "exit 0")
    ex.stop()

    # 2. the socket and the sync report the same lots once
    ex, tmp = fresh("ws2")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "0.3"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    oid = ex.open_orders()[0]["id"]
    ex.ws_mute = True                                             # the event is held back, the sync sees the fill first
    ex.fill(oid, 0.00004)
    check(until(lambda: b.reps("fill")), "the sync finds the fill when the socket is silent")
    ex.ws_flush()                                                 # the late event arrives
    time.sleep(0.8)
    check(len(b.reps("fill")) == 1 and b.reps("fill")[0]["qty"] == 4, "and the late event adds nothing: %s" % b.reps("fill"))
    check(prom_value(tmp, "hft_bridge_ws_matches_already_reported_total") == 1, "it is counted as already reported")
    ex.fill(oid, 0.00003)                                         # the socket now leads
    check(until(lambda: len(b.reps("fill")) == 2), "a later fill comes from the socket")
    time.sleep(0.8)                                               # several syncs go by
    fl = b.reps("fill")
    check(len(fl) == 2 and fl[1]["qty"] == 3 and fl[1]["leaves"] == 3 and sum(x["qty"] for x in fl) == 7, "7 lots in all, each once: %s" % [(x["qty"], x["leaves"]) for x in fl])
    # a websocket event that never arrives: the sync fills the gap, and the lots add up
    ex.ws_mute = True
    ex.fill(oid, 0.00001)                                         # 1 lot, the event is lost
    ex.ws_unsent.clear()
    ex.ws_mute = False
    check(until(lambda: sum(x["qty"] for x in b.reps("fill")) == 8, 4.0), "a lost event is made up by the sync")
    ex.fill(oid, 0.00001)                                         # 1 more lot, this event arrives
    time.sleep(0.9)
    tot = sum(x["qty"] for x in b.reps("fill"))
    check(tot == 9 and b.reps("fill")[-1]["leaves"] == 1, "9 lots matched, 9 reported, 1 left: %s" % [(x["qty"], x["leaves"]) for x in b.reps("fill")])
    b.finish("eof")
    ex.stop()

    # 3. a fill that comes just before a cancel is not lost
    ex, tmp = fresh("ws3")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    ex.before_cancel = lambda oid: ex.fill(oid, 0.00004)
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: b.reps("canceled")), "cancel answered")
    kinds = [(r["rt"], r["qty"], r["leaves"]) for r in b.reports if r["rt"] in ("fill", "canceled")]
    check(kinds == [("fill", 4, 6), ("canceled", 6, 0)], "the fill that raced the cancel is reported first, then the cancel for what was left: %s" % kinds)
    b.finish("eof")
    ex.stop()

    # 3a. the socket says the order filled completely while the exchange still accepted our cancel: the fill is reported
    # and the cancel is not (the OMS must not be told that a filled order was cancelled)
    ex, tmp = fresh("ws3a")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    oid = ex.open_orders()[0]["id"]
    ex.before_cancel = lambda o: ex.ws_send({"type": "match", "maker_order_id": o, "taker_order_id": "x", "size": "0.00010", "price": "81000.00"})
    b.send({"t": "can", "seg": 1, "id": 1})
    check(until(lambda: b.reps("cancel_rejected")), "cancel answered")
    kinds = [(r["rt"], r["qty"], r["leaves"]) for r in b.reports if r["rt"] in ("fill", "canceled", "cancel_rejected")]
    check(kinds == [("fill", 10, 0), ("cancel_rejected", 0, 0)], "fill, then cancel refused: %s" % kinds)
    b.finish("eof")
    ex.stop()

    # 3b. the same for a replace
    ex, tmp = fresh("ws3b")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    ex.before_cancel = lambda oid: ex.fill(oid, 0.00004)
    b.send({"t": "rep", "seg": 1, "id": 1, "px": px(80990.00), "qty": 10})
    check(until(lambda: b.reps("replaced")), "replace answered")
    kinds = [(r["rt"], r["qty"], r["leaves"]) for r in b.reports if r["rt"] in ("fill", "replaced")]
    check(kinds == [("fill", 4, 6), ("replaced", 10, 10)], "the fill that raced the replace is reported before it: %s" % kinds)
    b.finish("eof")
    ex.stop()

    # 3c. an order that ends on the exchange is looked into at once, not at the next sync
    ex, tmp = fresh("ws3c")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    ex.cancel_elsewhere(ex.open_orders()[0]["id"])
    check(until(lambda: b.reps("canceled"), 3.0), "an order cancelled elsewhere is reported from the done event (sync is 600 s away)")
    b.finish("eof")
    ex.stop()

    # 4. refused authentication: no fills from the socket, the sync still works, the bridge goes on
    ex, tmp = fresh("ws4")
    ex.start_ws()
    ex.ws_mode = "reject_auth"
    t_start = time.monotonic()
    b = ws_bridge(ex, tmp, ["--sync-s", "0.3"])
    check(until(lambda: b.events("ws_error")), "an authentication error is logged: %s" % [e.get("message") for e in b.events("ws_error")][:1])
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    ex.fill(ex.open_orders()[0]["id"], 0.00004)
    check(until(lambda: b.reps("fill")), "the REST sync still finds the fill")
    check(until(lambda: ex.ws_rejected >= 2, 6.0), "and the bridge keeps trying to subscribe (%d refusals)" % ex.ws_rejected)
    time.sleep(max(0.0, t_start + 6.5 - time.monotonic()))
    check(2 <= ex.ws_rejected <= 4, "with growing pauses (1, 2, 4 s): %d attempts in 6.5 s, not one a second" % ex.ws_rejected)
    check(prom_value(tmp, "hft_bridge_ws_subscribed") == 0 and (prom_value(tmp, "hft_bridge_ws_errors_total") or 0) >= 1, "metrics say not subscribed, with the errors counted")
    rc, _ = b.finish("eof")
    check(rc == 0, "exit 0")
    ex.stop()

    # 5. a dropped connection: resubscribed, and a fill missed in the gap is found at once (the sync is 600 s away)
    ex, tmp = fresh("ws5")
    ex.start_ws()
    b = ws_bridge(ex, tmp, ["--sync-s", "600"])
    until(lambda: b.events("ws_subscribed"))
    b.send(hello(), new(1, px(81000.00), qty=10))
    until(lambda: b.reps("accepted"))
    oid = ex.open_orders()[0]["id"]
    ex.ws_drop()
    check(until(lambda: not ex.ws_clients, 3.0), "connection closed by the exchange")
    ex.fill(oid, 0.00004)                                         # nobody is listening
    check(until(lambda: len(b.events("ws_subscribed")) >= 2, 8.0), "the bridge resubscribes")
    check(until(lambda: b.reps("fill"), 3.0), "and the fill from the gap is found by the sync that follows")
    check(b.reps("fill")[0]["qty"] == 4, "4 lots: %s" % b.reps("fill")[:1])
    check(until(lambda: (prom_value(tmp, "hft_bridge_ws_connects_total") or 0) >= 2, 2.0), "metrics count the reconnection (%s connects)" % prom_value(tmp, "hft_bridge_ws_connects_total"))
    b.finish("eof")
    ex.stop()

    # 6. options
    print("websocket options")
    ex, tmp = fresh("ws6")
    ex.start_ws()
    for extra, want, label in [
            (["--ws-fills"], "needs --loop", "--ws-fills without --loop"),
            (["--loop", "--ws-fills", "--ws-url", "wss://ws-feed.exchange.coinbase.com"], "only allows ws://", "a production socket in test mode"),
            (["--loop", "--ws-fills", "--ws-url", "ws://example.com:80"], "only allows ws://", "a foreign host in test mode")]:
        b = Bridge(ex, tmp, extra)
        rc, err = b.finish("eof")
        check(rc == 2 and want in err, "%s: exit 2 (%s)" % (label, err.strip()[:80]))
    e = dict(os.environ, HFT_CB_KEY=KEY, HFT_CB_SECRET=SECRET, HFT_CB_PASSPHRASE=PASS, HTTPS_PROXY="http://127.0.0.1:9", https_proxy="http://127.0.0.1:9")
    for url in ["wss://ws-feed.exchange.coinbase.com", "wss://evil.example.com"]:
        r = subprocess.run([sys.executable, BRIDGE, "--loop", "--ws-fills", "--ws-url", url], capture_output=True, text=True, env=e, input="", timeout=20)
        check(r.returncode == 2 and "refusing websocket URL" in r.stderr, "outside test mode %s is refused (exit 2)" % url)
    ex.stop()


if __name__ == "__main__":
    live = sys.argv[1] if len(sys.argv) > 1 else None
    test_signing()
    test_guards()
    test_flow()
    test_pacing()
    test_cancel_before_place()
    test_segment_end_and_priority()
    test_signals()
    test_errors()
    test_limits_and_secrets()
    test_loop()
    test_loop_refusals()
    test_loop_fills_and_vanished()
    test_loop_latency_and_segments()
    test_ws_fills()
    test_hft_live_mirror(live)
    print("test_bridge: " + ("all checks passed" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)
