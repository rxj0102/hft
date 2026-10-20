#!/usr/bin/env python3
"""Record a Coinbase-style level-2 WebSocket feed to a text file.

Public market data only: no API keys are used or accepted.

File format, one record per line:
    <t_ns>\t<raw JSON message exactly as received>
    #<t_ns>\t<event>              (event: start, reconnect, stop, error ...)
t_ns is nanoseconds since the capture started (monotonic clock), taken when the
message was received by this process. After a reconnect the server sends a fresh
snapshot, so the consumer starts a new segment at the next snapshot.

Do not commit captures to a public repository: the exchange's terms may not
allow redistributing its market data. The repo ignores data/ for that reason.

Usage:
    capture_l2.py --seconds 120 --out data/btc.l2
    capture_l2.py --url wss://ws-feed-public.sandbox.exchange.coinbase.com --product BTC-USD

Live use: `--out -` writes the same lines to stdout, flushed one by one, for
`hft_live` to read from a pipe (status goes to stderr). `--resnap-file PATH`
makes the capture drop its connection and subscribe again, to get a fresh
snapshot, whenever PATH appears (hft_live creates it when the price has left
its grid window). Without hft_live, `--resnap-drift-usd N` does the same when the best price
has moved N USD from the last snapshot's mid (262 matches the replay's exit; see ADR 43).
`--tee PATH` keeps a copy of everything written, for replaying a live session afterwards. If the reader goes away the capture stops quietly.
"""
import argparse
import asyncio
import json
import os
import sys
import time

DEFAULT_URL = "wss://ws-feed.exchange.coinbase.com"


class _Tee:
    """Writes every line to two files. Used to keep a copy of the stream that goes to hft_live."""

    def __init__(self, a, b):
        self.a, self.b = a, b

    def write(self, s):
        self.a.write(s)
        self.b.write(s)

    def flush(self):
        self.a.flush()
        self.b.flush()


class DriftWatch:
    """Follows the best bid and ask in the stream and says when either has moved more than
    `drift_usd` from the mid of the last snapshot. The replay grid is centred on that mid and
    its segment ends at about 262 USD (10% of 65,536 ticks of 0.01 from each edge, hft::feed
    L2Config), so a drift limit below that asks for a fresh snapshot before the segment ends.
    The limit is a number the caller chooses; this class does not know the replay's settings."""

    def __init__(self, drift_usd):
        self.limit = float(drift_usd)
        self.book = ({}, {})  # bids, asks: price -> size
        self.mid = None

    def _best(self):
        bids, asks = self.book
        if not bids or not asks:
            return None
        return max(bids), min(asks)

    def feed(self, msg):
        """True when the stream has drifted past the limit. Anything unparseable is ignored."""
        try:
            m = json.loads(msg)
            if m.get("type") == "snapshot":
                self.book = ({}, {})
                for k, side in (("bids", 0), ("asks", 1)):
                    for px, sz in m.get(k, []):
                        if float(sz) > 0:
                            self.book[side][float(px)] = float(sz)
                b = self._best()
                self.mid = (b[0] + b[1]) / 2 if b else None
                return False
            if m.get("type") != "l2update" or self.mid is None:
                return False
            for side_name, px, sz in m.get("changes", []):
                side = 0 if side_name == "buy" else 1
                if float(sz) > 0:
                    self.book[side][float(px)] = float(sz)
                else:
                    self.book[side].pop(float(px), None)
        except (ValueError, TypeError, KeyError, AttributeError):
            return False
        b = self._best()
        if b is None:
            return False
        return b[0] < self.mid - self.limit or b[1] > self.mid + self.limit


async def capture(url, product, channel, seconds, out_path, idle_timeout, max_reconnects, connect_fn=None,
                  verify_path=None, resnap_file=None, poll_step=0.25, drift_usd=None, drift_min_s=1.0, verify_count=1, verify_spacing=2.0, tee_path=None):
    """Returns a dict of counters. `connect_fn` lets tests substitute a local server.
    `out_path` "-" streams to stdout. `resnap_file`: when this file exists, remove it and
    resubscribe (a new snapshot) on a fresh connection. `drift_usd`: do the same when the best
    bid or ask has moved more than that from the mid of the last snapshot (see DriftWatch),
    but at most once per `drift_min_s` seconds."""
    import websockets

    connect = connect_fn or (lambda u: websockets.connect(u, open_timeout=15, max_size=None))
    t0 = time.monotonic_ns()
    now = lambda: time.monotonic_ns() - t0
    deadline = t0 + int(seconds * 1e9)
    stats = {"messages": 0, "reconnects": 0, "errors": 0, "resnaps": 0, "drift_resnaps": 0}
    watch = DriftWatch(drift_usd) if drift_usd else None
    last_drift = -1e9  # monotonic seconds of the last drift resubscribe
    streaming = out_path == "-"
    backoff = 0.5
    vtask = None
    if verify_path:
        # a second connection, opened while this one is live, near the end of the capture
        vtask = asyncio.create_task(fetch_verify_snapshots(
            url, product, channel, verify_path, connect_fn, delay=verify_delay(seconds, verify_count, verify_spacing),
            t0_ns=t0, count=verify_count, spacing=verify_spacing))
    f = sys.stdout if streaming else open(out_path, "w", encoding="utf-8", newline="\n")
    tee_file = None
    if tee_path:
        tee_file = open(tee_path, "w", encoding="utf-8", newline="\n")
        f = _Tee(f, tee_file)
    try:
        f.write("#%d\tstart\t%s\t%s\n" % (now(), url, product))
        resnapped = False
        while time.monotonic_ns() < deadline:
            resnapped = False
            try:
                async with connect(url) as ws:
                    await ws.send(json.dumps({"type": "subscribe", "product_ids": [product],
                                              "channels": [channel, "heartbeat"]}))
                    backoff = 0.5
                    last_rx = time.monotonic()
                    while time.monotonic_ns() < deadline:
                        if resnap_file and os.path.exists(resnap_file):
                            try:
                                os.remove(resnap_file)
                            except OSError:
                                pass
                            stats["resnaps"] += 1
                            f.write("#%d\tresnap\n" % now())
                            resnapped = True
                            break
                        left = (deadline - time.monotonic_ns()) / 1e9
                        try:
                            msg = await asyncio.wait_for(ws.recv(), timeout=min(poll_step, max(left, 0.01)))
                        except asyncio.TimeoutError:
                            if time.monotonic_ns() >= deadline:
                                break
                            if time.monotonic() - last_rx >= idle_timeout:
                                raise
                            continue
                        last_rx = time.monotonic()
                        t = now()
                        if isinstance(msg, bytes):
                            msg = msg.decode("utf-8", "replace")
                        msg = msg.replace("\n", " ").replace("\r", " ")
                        f.write("%d\t%s\n" % (t, msg))
                        if streaming:
                            f.flush()
                        stats["messages"] += 1
                        if watch and watch.feed(msg) and time.monotonic() - last_drift >= drift_min_s:
                            last_drift = time.monotonic()
                            stats["resnaps"] += 1
                            stats["drift_resnaps"] += 1
                            f.write("#%d\tresnap\n" % now())
                            resnapped = True
                            break
            except BrokenPipeError:
                stats["reader_gone"] = True
                break
            except asyncio.TimeoutError:
                if time.monotonic_ns() >= deadline:
                    break
                # silence longer than idle_timeout: treat the connection as dead
                f.write("#%d\tidle_timeout\n" % now())
            except Exception as e:  # connection closed, reset, TLS error, ...
                stats["errors"] += 1
                f.write("#%d\terror\t%s\n" % (now(), type(e).__name__))
            if time.monotonic_ns() >= deadline:
                break
            stats["reconnects"] += 1
            f.write("#%d\treconnect\n" % now())
            f.flush()
            if resnapped:
                continue  # asked for: reconnect at once, no backoff, and it does not count towards giving up
            if stats["reconnects"] - stats["resnaps"] > max_reconnects:
                f.write("#%d\tgive_up\n" % now())
                break
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 8.0)
        f.write("#%d\tstop\n" % now())
        f.flush()
    except BrokenPipeError:
        stats["reader_gone"] = True
    finally:
        if tee_file:
            tee_file.close()
        if not streaming:
            (f.a if tee_file else f).close()
    if vtask:
        stats["verify_n"] = await vtask
        stats["verify"] = stats["verify_n"] > 0
    return stats


def verify_delay(seconds, count, spacing):
    """Seconds into the capture at which the first verify snapshot is asked for: late enough that the last one
    arrives about 6 s before the end, but never earlier than half way."""
    return max(seconds - 6.0 - (count - 1) * spacing, seconds * 0.5)


async def fetch_one_snapshot(url, product, channel, connect_fn=None, timeout=15.0):
    """One connection, one snapshot: returns (arrival time in monotonic ns, raw message) or None."""
    import websockets

    connect = connect_fn or (lambda u: websockets.connect(u, open_timeout=15, max_size=None))
    try:
        async with connect(url) as ws:
            await ws.send(json.dumps({"type": "subscribe", "product_ids": [product], "channels": [channel]}))
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                msg = await asyncio.wait_for(ws.recv(), timeout=max(end - time.monotonic(), 0.01))
                if isinstance(msg, bytes):
                    msg = msg.decode("utf-8", "replace")
                if '"type":"snapshot"' in msg.replace(" ", ""):
                    return time.monotonic_ns(), msg.replace("\n", " ")
    except Exception:
        pass
    return None


async def fetch_verify_snapshots(url, product, channel, verify_path, connect_fn=None, timeout=15.0, delay=0.0, t0_ns=None,
                                 count=1, spacing=2.0):
    """Opens `count` further connections, one after the other `spacing` seconds apart (the first after `delay`
    seconds), and writes the exchange's own snapshots of the book to `verify_path`, one `<t_ns>\t<raw>` line each, with
    t_ns on the main capture's clock when `t0_ns` is given. The replay tool compares the book it reconstructed as of
    each time with it: that checks that the update messages were read correctly, which a self-consistency test cannot.
    Several snapshots are asked for because the sandbox's snapshots are often wider than the replay's price window,
    and such a snapshot cannot be compared. The file is written once at the end, and only if at least one snapshot was
    fetched. Returns how many were written."""
    t0 = t0_ns if t0_ns is not None else time.monotonic_ns()
    if delay > 0:
        await asyncio.sleep(delay)
    lines = []
    for k in range(count):
        if k:
            await asyncio.sleep(spacing)
        got = await fetch_one_snapshot(url, product, channel, connect_fn, timeout)
        if got:
            lines.append("%d\t%s\n" % (got[0] - t0, got[1]))
    if lines:
        with open(verify_path, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
    return len(lines)


async def fetch_verify_snapshot(url, product, channel, verify_path, connect_fn=None, timeout=15.0, delay=0.0, t0_ns=None):
    """One snapshot; True on success."""
    return await fetch_verify_snapshots(url, product, channel, verify_path, connect_fn, timeout, delay, t0_ns) == 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--product", default="BTC-USD")
    ap.add_argument("--channel", default="level2_batch")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--idle-timeout", type=float, default=10.0)
    ap.add_argument("--max-reconnects", type=int, default=20)
    ap.add_argument("--resnap-file", default=None,
                    help="when this file appears, drop the connection and subscribe again for a fresh snapshot")
    ap.add_argument("--resnap-drift-usd", type=float, default=None,
                    help="subscribe again for a fresh snapshot when the best bid or ask has moved more than this many USD "
                         "from the last snapshot's mid. The replay's window ends a segment at about 262 USD for the "
                         "default grid, so 262 mirrors the replay's own exit; a smaller value cuts segments the replay could have kept (measured: ADR 43). Off by default")
    ap.add_argument("--resnap-min-s", type=float, default=1.0,
                    help="at most one drift resubscribe per this many seconds (default 1). The sandbox sends a snapshot whose "
                         "touch is already wider than the replay window about half the time; without this the capture "
                         "reconnects as fast as it can until a narrow one comes")
    ap.add_argument("--tee", default=None,
                    help="also write every line to this file (a copy of the stream sent to hft_live, to replay afterwards "
                         "with `hft_live --from`)")
    ap.add_argument("--verify-count", type=int, default=1,
                    help="with --verify: take this many exchange snapshots, about 2 s apart, ending near the end of the "
                         "capture. The sandbox's snapshots are often wider than the replay window, and those cannot be compared")
    ap.add_argument("--verify", action="store_true",
                    help="near the end of the capture, fetch the exchange's own snapshot on a second connection into "
                         "<out>.verify, to check the replayed book against")
    a = ap.parse_args(argv)
    try:
        import websockets  # noqa: F401
    except ImportError:
        sys.exit("needs the websockets package: pip install websockets")
    if a.verify and a.out == "-":
        sys.exit("--verify needs a file, not stdout")
    st = asyncio.run(capture(a.url, a.product, a.channel, a.seconds, a.out, a.idle_timeout, a.max_reconnects,
                             verify_path=(a.out + ".verify") if a.verify else None, resnap_file=a.resnap_file, drift_usd=a.resnap_drift_usd, drift_min_s=a.resnap_min_s, verify_count=a.verify_count, tee_path=a.tee))
    out = sys.stderr if a.out == "-" else sys.stdout
    print("captured %(messages)d messages, %(reconnects)d reconnects, %(errors)d errors, %(resnaps)d resubscribes "
          "(%(drift_resnaps)d for price drift)" % st,
          file=out)
    if a.verify:
        print("verify snapshots: %s" % ("%d of %d written to %s.verify" % (st["verify_n"], a.verify_count, a.out) if st.get("verify") else "FAILED"))
    if a.out == "-":
        # If the reader went away, what is still buffered would be flushed at interpreter exit into the broken pipe and
        # print "Exception ignored ... BrokenPipeError". Flush now, and on failure point stdout at /dev/null (the recipe
        # from the Python documentation) so that the exit is quiet. (A CI runner showed the noise; it never showed locally.)
        try:
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())


if __name__ == "__main__":
    main()
