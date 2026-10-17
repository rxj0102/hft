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
its grid window). If the reader goes away the capture stops quietly.
"""
import argparse
import asyncio
import json
import os
import sys
import time

DEFAULT_URL = "wss://ws-feed.exchange.coinbase.com"


async def capture(url, product, channel, seconds, out_path, idle_timeout, max_reconnects, connect_fn=None,
                  verify_path=None, resnap_file=None, poll_step=0.25):
    """Returns a dict of counters. `connect_fn` lets tests substitute a local server.
    `out_path` "-" streams to stdout. `resnap_file`: when this file exists, remove it and
    resubscribe (a new snapshot) on a fresh connection."""
    import websockets

    connect = connect_fn or (lambda u: websockets.connect(u, open_timeout=15, max_size=None))
    t0 = time.monotonic_ns()
    now = lambda: time.monotonic_ns() - t0
    deadline = t0 + int(seconds * 1e9)
    stats = {"messages": 0, "reconnects": 0, "errors": 0, "resnaps": 0}
    streaming = out_path == "-"
    backoff = 0.5
    vtask = None
    if verify_path:
        # a second connection, opened while this one is live, near the end of the capture
        vtask = asyncio.create_task(fetch_verify_snapshot(url, product, channel, verify_path, connect_fn,
                                                          delay=max(seconds - 6.0, seconds * 0.5), t0_ns=t0))
    f = sys.stdout if streaming else open(out_path, "w", encoding="utf-8", newline="\n")
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
        if not streaming:
            f.close()
    if vtask:
        stats["verify"] = await vtask
    return stats


async def fetch_verify_snapshot(url, product, channel, verify_path, connect_fn=None, timeout=15.0, delay=0.0, t0_ns=None):
    """Opens a second connection (after `delay` seconds) and writes the exchange's own
    snapshot of the book to `verify_path` as one `<t_ns>\t<raw>` line, with t_ns on the
    main capture's clock when `t0_ns` is given. The replay tool compares the book it
    reconstructed as of that time with it: that checks that the update messages were
    read correctly, which a self-consistency test cannot. Returns True on success."""
    import websockets

    connect = connect_fn or (lambda u: websockets.connect(u, open_timeout=15, max_size=None))
    t0 = t0_ns if t0_ns is not None else time.monotonic_ns()
    if delay > 0:
        await asyncio.sleep(delay)
    try:
        async with connect(url) as ws:
            await ws.send(json.dumps({"type": "subscribe", "product_ids": [product], "channels": [channel]}))
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                msg = await asyncio.wait_for(ws.recv(), timeout=max(end - time.monotonic(), 0.01))
                if isinstance(msg, bytes):
                    msg = msg.decode("utf-8", "replace")
                if '"type":"snapshot"' in msg.replace(" ", ""):
                    with open(verify_path, "w", encoding="utf-8", newline="\n") as f:
                        f.write("%d\t%s\n" % (time.monotonic_ns() - t0, msg.replace("\n", " ")))
                    return True
    except Exception:
        pass
    return False


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
                             verify_path=(a.out + ".verify") if a.verify else None, resnap_file=a.resnap_file))
    out = sys.stderr if a.out == "-" else sys.stdout
    print("captured %(messages)d messages, %(reconnects)d reconnects, %(errors)d errors, %(resnaps)d resubscribes" % st,
          file=out)
    if a.verify:
        print("verify snapshot: %s" % ("written to " + a.out + ".verify" if st.get("verify") else "FAILED"))
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
