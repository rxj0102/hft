# Sandbox order entry (mirror)

`scripts/order_bridge.py` sends the orders `hft_live` decides on to the Coinbase Exchange **sandbox** over signed REST. It is a mirror, not a closed loop.

```
capture_l2.py --out - | hft_live --orders-fd 3 3>| order_bridge.py        (scripts/supervise.py --bridge starts all three)
```

## What it does and does not do

- `hft_live` still decides against its paper venue (acks, never fills). Every new, replace and cancel frame the gateway sends is also written, as one JSON line, to a pipe. The bridge keeps the sandbox account's resting orders equal to what `hft_live` believes is working, as far as the rate limit lets it.
- Nothing comes back. The sandbox's acks, rejects and fills do not reach the OMS or the strategy. The strategy is therefore not reacting to the exchange; the exchange is following the strategy. A closed loop (real reports into `Gateway::on_bytes`, flatten against a venue that answers in milliseconds) is a separate, larger step and is not done.
- Coinbase Exchange has no amend. A replace is a cancel and then a new order, and they are not atomic. Updates are coalesced: if a quote moves five times while the bridge waits for the rate limit, only the last position is sent.
- Cancels go first, then new orders. Orders are post-only limits; size is the strategy's lot (default 5 lots = 0.00005 BTC).

## Safety, as code

| Rule | Where |
|---|---|
| Keys only from `HFT_CB_KEY`, `HFT_CB_SECRET`, `HFT_CB_PASSPHRASE`. No option exists for them; they are not logged (tested by searching every output for them). Missing: exit 2 before any network use | `order_bridge.py` main |
| Base URL must be exactly `https://api-public.sandbox.exchange.coinbase.com`. Production and look-alike hosts are refused (exit 2). `--test-exchange` allows plain http to 127.0.0.1 only | `check_url` |
| Per-order notional cap (`--max-notional`, default 25 USD) and working-order cap (`--max-open`, default 6): refused locally, never sent | `Bridge.step` |
| Rate `--rate` (default 8/s) below the documented 15/s per profile | `Bridge.req` |
| Cancel-all for the product at start, at end of input and on SIGTERM. **This cancels every open order for the product on the profile, including ones you placed by hand.** Use a dedicated sandbox profile | `Bridge.cancel_all` |
| 401/403 or a clock more than 20 s from the exchange's: exit 4, never restarted | `Fatal(4)` |
| Reply lost to a POST: the order may exist, so it is cancelled by `client_oid` | `Bridge.step` |
| Orders that disappear from the exchange (filled, or cancelled elsewhere) are noticed every `--sync-s`; stray orders are cancelled | `Bridge.sync` |

## Running it (on your machine, with your own sandbox key)

Create an API key in the Coinbase Exchange sandbox with trade permission on a profile used only for this. Put the three values into environment variables in your shell. Do not paste them into a chat, a file in the repository, or a command line.

1. Smoke test, one order far below the market, post-only, cancelled after five seconds (notional about 0.10 USD):
```
(printf '{"t":"hello","v":1,"px_scale":10000,"qty_scale":100000}\n{"t":"new","seg":1,"id":1,"side":"buy","px":10000000,"qty":10,"type":4}\n'; sleep 5) | python3 scripts/order_bridge.py --log -
```
   Expected log: `bridge_start`, `clock_skew`, `cancel_all`, `start_flat` (flat true), a `placed` count of 1 in the final `exit` line, `exit_flat` (flat true). Anything else: stop and read it.
2. Supervised, on the sandbox feed so that prices match the sandbox's own book:
```
python3 scripts/supervise.py --run-dir /tmp/hft-sbx --bridge --url wss://ws-feed-public.sandbox.exchange.coinbase.com \
  --bridge-args "--rate 6 --max-notional 10 --max-open 4"
```
   Files: `bridge.jsonl` (events), `bridge.prom` (metrics), `bridge.err`.

## What was tested, and against what

`scripts/test_bridge.py` and the bridge drills in `scripts/drill.py` run against `scripts/fake_exchange.py`, a local stand-in written from the documented API (header names, signing recipe, 15 req/s limit, 30 s timestamp window, post-only refusal, `DELETE /orders`, `DELETE /orders/{id}`, `DELETE /orders/client:{client_oid}`, `GET /orders`). The fake checks signatures with separate code and the signing function is compared with openssl.

**Not tested: the real sandbox.** No API key was available in this work. Unknown until the smoke test above has been run by someone with a key: whether the sandbox accepts these signatures and headers; the exact response bodies (the bridge reads `id` from a POST reply and a list from `GET /orders`); the `status=open` query on `GET /orders`; `DELETE /orders/client:...`; the product's minimum size and increments; latency (each call opens a new connection, which adds a TLS handshake over a proxy); how the sandbox's own market behaves.

## Limits

- Mirror, not closed loop (above). No fills are modelled or handled; if the exchange fills a resting order the bridge only sees it as a vanished order at the next sync, and places it again because the strategy still wants it.
- Between a kill -9 of the bridge and the next bridge's first call, the orders keep resting. In the drills that gap is about 0.4 s (0.2 s backoff plus start-up); with the default backoff it is about 1 s, plus whatever time the new process takes to reach the exchange. Cancel-on-disconnect is a feature of other Coinbase interfaces and is not used or relied on here.
- Single account, single product, one process. If the supervisor itself dies, nothing cancels anything.

## Mutation check

28 injected bugs in `order_bridge.py` (signing, URL guard, missing keys, ordering of cancels and places, pacing, 401/403 handling, rejected-order retry, lost-reply handling, cancel-all at start and exit, segment end, notional and open-order caps, secret logging, price rounding, strays, vanished orders, clock skew, 429 pause, exit codes, segment keys, 5xx counting, replace and cancel handling, the URL guard's fall-through) and 15 in the C++ tap and mirror (tap call, segment number, dropped replace/cancel/end lines, side, overflow, write errors, final flush, partial writes, hello, loss reporting). Each had to make `test_bridge.py`, `test_live_tool.py` or `test_live` fail.

First runs: 6 of the 28 bridge bugs and 4 of the 15 C++ bugs survived. Each was a missing test, not an equivalent mutant, and got a test: a cancel must precede a new order that is waiting; 403 is fatal like 401; a segment end clears the segment without any cancel lines; a 429 must cause a pause (not just pacing); a 5xx in the middle of a run must stop the bridge by itself (before, the start-up failure hid it); the URL guard must not fall through; the side of an order; a late reader must receive the whole backlog; a partial write must not drop data; a loss at exit must be reported. All are caught now. (One mutation run also showed that the production-URL test, with a broken guard, would have sent a request to the production host; it now runs behind a dead local proxy.)
