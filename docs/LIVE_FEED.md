# Real market data: capture, convert, replay

What this is: a way to run the production pipeline on recorded real exchange data. What it is not: a live trading connection. Nothing in this repository sends an order to a real exchange.

## The source

Coinbase Exchange public WebSocket (`wss://ws-feed.exchange.coinbase.com`, and the sandbox `wss://ws-feed-public.sandbox.exchange.coinbase.com`). No API keys are used. The sandbox and the production feed were both reachable from the development sandbox; Binance, including its testnet, answered HTTP 451 there.

Three properties of this feed shaped the design:

1. It is aggregated by price level (level 2), not order by order. A message says "this price now has this total size".
2. The `l2update` messages carry no sequence number. Loss detection is therefore a property of the TCP connection (a drop means reconnect and a fresh snapshot), not of per-message numbering as with the engine's ITCH-style feed.
3. Sizes have 8 decimals and the book spans thousands of levels, many of them far from the market.

## Pipeline

```
 scripts/capture_l2.py            record raw JSON + receive time, reconnect, mark segments
        |  data/*.l2                (kept out of git, see Licensing)
        v
 feed::CaptureLoader              parse, split into segments (snapshot -> reconnect/end/window exit)
        |   feed::L2Converter     aggregated levels -> synthetic orders -> PublicEvents
        |   sim::ItchPublisher    -> the engine's ITCH-style packets, one packet per feed message
        v
 t2t::Pipeline  (unchanged)       <---- reports ---- feed::AckVenue (paper venue, never fills)
        |
        v
 independent model of the book    checkpoints every N packets + final comparison
```

`hft_replay` runs it and prints the result. The hot path is not modified: the same `Pipeline` that was measured in Phase 7 consumes the converted packets.

## Conversion rules

One synthetic order stands for one price level.

| Feed says | Converter emits |
|---|---|
| a level appears | Add |
| the level's size goes down | Reduce by the difference |
| the level's size goes up | Delete, then Add (the wire has no replace) |
| the level's size goes to zero | Delete |
| nothing changed | nothing |

Units: price raw = price x 10^4; one lot = 10^-5 of the base asset (sub-lot size is floored and counted); the wire carries 32-bit price and quantity, so larger sizes are clamped and counted.

The engine's price grid is a window of `num_levels` ticks (default 65,536 x $0.01 = $655) centred on the first mid. Changes outside the window are dropped and counted. If the best price gets within 10% of a window edge the segment ends: a book that is allowed to drift out of its grid is a book that is wrong.

Because this is level data, there is no queue position. Nothing here can say where an order of ours would stand.

## What proves it is right

1. **Independent model.** The converter keeps its own picture of the aggregated book. The pipeline's book, built only from the ITCH bytes, is compared with it at checkpoints (best prices, touch sizes, depth over 5 levels, live level count) and level by level at the end. This catches errors in the converter, the codec, the feed handler and the book. It cannot catch a wrong reading of the feed, because both sides share my reading of it.
2. **Exchange snapshot.** For that, `capture_l2.py --verify` opens a second connection near the end of the capture and stores the exchange's own snapshot. `hft_replay --verify` rebuilds the book as of that moment and compares the 10 nearest levels per side. Levels beyond the replay's price window are not compared and are reported on a second line (ADR 41), because the window drops them by design; a sparse book such as the Coinbase sandbox's has most of its nearest levels outside it. Three live runs gave 20 of 20, 18 of 20 and 20 of 20 identical; the residue is the skew between two connections on a book that changes many times a second. A snapshot fetched 1-2 s after the capture ended gave 16 of 20, which is why the check is done concurrently. As a control, comparing a book with a snapshot from a different time gave 0 of 20, so the check can fail. Without `hft_live`, `capture_l2.py --resnap-drift-usd 262` asks for a fresh snapshot when the best price drifts that far from the last snapshot's mid (ADR 43, 44). If the segment ended by leaving the window more than 2 s before the snapshot, the check prints "not comparable" instead of counts (ADR 42).
3. **Determinism.** Replaying twice gives identical wire bytes (checked every run). gcc and clang give identical wire hashes on the same capture (checked on three captures).
4. **Tests.** `test_l2` (parsers, converter semantics, hostile input, 12 random captures replayed against the model, window exits, the paper venue, and tests that tamper with the model to prove the checker fails). `scripts/test_capture.py` (fake server: drop, silence, give-up, verify snapshot). `fuzz_l2`. Mutation check: 35 injected bugs, all caught.

## Results on real data

BTC-USD, production feed, 90 s capture, 1,556 messages, one segment. gcc release build, shared 2-vCPU VM.

| | |
|---|---|
| packets replayed | 1,516 (21,916 engine messages) |
| changes in the feed | 66,349; 43,367 (65%) outside the 655-wide window and dropped; 43,867 (66%) had size digits finer than one lot (floored); 0 off tick; 0 crossed |
| peak live levels in window | 515 |
| model comparison | 17 checkpoints (every 100 packets), 0 failures; final book matches |
| feed handler | 0 desyncs, 0 book errors, 0 feed losses |
| strategy (default parameters) | 1,934 decisions, 418 orders (2 new, 416 replace, 0 cancel), 0 risk rejects |
| paper venue (50 us round trip, never fills) | 418 reports delivered, 0 rejected as crossing |

Processing time per packet (feed handler, book, strategy, gateway; hot loop; three runs):

| | p50 | p99 | p99.9 |
|---|---|---|---|
| every packet | 519-535 ns | 3.2-3.7 us | 6.6-15.6 us |
| packets that made the strategy send | 1.36-1.44 us | 3.4-4.1 us | |
| per feed message applied (mean) | 57-61 ns | | |

This is not comparable with the synthetic 128 ns headline. A real packet carries about 14 messages (21,916 messages in 1,516 packets), and the per-message cost of about 60 ns is consistent with the synthetic per-message cost of about 72 ns. It also does not include the network, TLS, WebSocket framing, JSON parsing, or the Python capture process, all of which sit in front of the engine in a live setup and cost far more than the engine does.

## Licensing

Do not commit captures. The exchange's terms may not allow redistributing its market data. `data/` is in `.gitignore`, and the test and fuzz fixtures are hand-made with invented prices.

## Limits

- The replay is offline: capture, then replay. The live paper mode (LIVE_PAPER.md) streams the capture into the same pipeline in real time. Neither has order entry to an exchange.
- One product (BTC-USD), one venue, level 2 only.
- The paper venue never fills, and its 50 us round trip is a stated assumption, not a measurement.
- Dropped far-from-market levels and floored sub-lot sizes mean the replayed book is the near-market book at lot resolution. The strategy reads three levels, so this does not change what it sees at the touch, but it is not the exchange's full book.
- The window is fixed per segment; a market that moves out of it ends the segment (there is no re-centring).
- Timing is for this VM and this build; it varied by about 25% over the session.

## How to run

```
python3 -m pip install websockets
python3 scripts/capture_l2.py --seconds 90 --verify --out data/btc.l2     # production public feed
cmake --preset release && cmake --build --preset release
./build/release/hft_replay --verify data/btc.l2.verify data/btc.l2
```

`hft_replay` exit codes: 0 ok, 1 usage or I/O, 2 replay not deterministic, 4 book differs from the model, 5 nothing usable in the capture.
