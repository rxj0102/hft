# Closed loop: the exchange's reports drive the OMS

Phase 12 mirrored the strategy's orders to the sandbox. Phase 13 closes the loop: the exchange's answers (accepted, rejected, replaced, cancelled, filled) go back into the gateway and OMS, the strategy reacts to them, and an end of trust is over only when the exchange has confirmed that the orders are gone.

```
capture_l2.py --out - | hft_live --orders-fd 3 --exchange-fd 4  3>| order_bridge.py --loop |4<     (scripts/supervise.py --loop starts all three)
                          orders ------------------>             reports <-----------------
```

Everything below was run against `scripts/fake_exchange.py` only. **The real sandbox has not been used** (no key). Treat the report mapping, the fill detection and the timings as unverified against Coinbase.

## What changed in the live session

`LiveConfig::external_venue` (set by `--exchange-fd`):
- Orders go only to the tap; the paper venue is not used (its state stays empty, a test checks `venue_live() == 0`).
- `LiveSession::on_exchange_report(seg, report, now)` takes a report for segment `seg`, numbers it for the segment's gateway (which insists on a gapless sequence) and delivers it. Reports for a segment that no longer exists are counted and dropped.
- An end of trust (reconnect marker, new snapshot, window exit, stale feed, integrity failure, kill, shutdown) no longer finishes in one call. It cancels what can be cancelled and the segment stays `busy()` until the OMS shows nothing working, or `--flatten-timeout-s` (5 s) passes. Each report and each `poll()` re-issues cancels for orders whose ack has just arrived (an order that is still PendingNew cannot be cancelled). The strategy does not see reports during this wait, so it cannot quote again.
- While a segment is busy, feed updates are ignored (counted) and a snapshot is dropped and a resubscribe requested, so the next snapshot comes after the segment has finished.
- Timeout: the segment ends NOT FLAT, the kill switch is latched and the session halts. `hft_live` exits 4. This is the same rule as the paper venue's 64-round limit, now in time.
- If the bridge's report pipe closes, `hft_live` logs `exchange_lost`, kills, waits for the flatten to time out and exits 6 (restartable; a new bridge cancels everything on the exchange first).
- `segment_end` records `position_lots` and `fills`. The flatten cancels orders; it does not close a position. `flatten_sim_ns` is the real wait in this mode.

The paper path is untouched: the same tests, the same wire hashes, the perf gate (797.80).

## What the bridge does with `--loop`

Every request is carried out and answered by exactly one report line on stdout (`order_bridge.py --loop`), tagged with the segment of the request.

| Request | Exchange calls | Report |
|---|---|---|
| new | POST (post-only limit) | `accepted`; `rejected` with reason `cross` (post-only), `badqty`/`badprice` (exchange text, or the local size and notional caps), `other` (a reply that was lost: the order is also cancelled by client id) |
| replace | DELETE old, GET old (what traded?), POST new | any fill found in the record first, then `replaced`; if the new order is refused the old one is already gone, so `canceled`; if the cancel finds nothing, `replace_rejected` (the sync then says what happened) |
| cancel | DELETE, GET the order (what traded?) | any fill found in the record first, then `canceled` (the size that was left); `cancel_rejected` when the order is unknown, already gone, or turns out to have filled completely |
| (sync, every `--sync-s`) | GET open orders, GET one order | `fill` for new fills (price is the order's limit price, not the exchange's executed value), `canceled` for an order that vanished, stray orders cancelled silently |
| end of segment | DELETE for orders still resting | none (orphans from a flatten that failed) |

The OMS allows one request in flight per order, so nothing is coalesced here; the rate limit (`--rate`, default 8/s) just queues requests. A replace costs three calls (cancel, a look at what traded, new order), so each quote move takes at least three round trips; a cancel costs two. The look is what keeps a fill that came just before the cancel from being lost (ADR 28).

## What it costs, honestly

- The strategy now trades against exchange latency. A quote move takes the time of three REST calls; in the drills with 0.4 s per call, a stop took about 1.9 s to confirm. Against the real sandbox this is unmeasured.
- A reconnect or a window exit used to take about 120 ms to quote again; it now also includes the wait for cancel confirmations.
- Without `--ws-fills`, fills are found by polling. With `--sync-s 5` a filled order can look alive for up to five seconds. Use a shorter interval in loop mode (`--sync-s 1`), at the price of more calls. With `--ws-fills` (below) the socket delivers fills at once and the sync can be slow.
- There is no replay of an exchange run: it depends on timing. The paper mode stays the deterministic one.
- A position built by fills is closed only when you ask for it and only when the session stops for good (`--close-position`, below). Risk limits cap it in between; the metric `hft_live_position_lots` and `segment_end.position_lots` show it.

## Evidence

- `test_live` (C++): flatten waits for acks then cancels then confirmations and no new order goes out; updates are ignored and a snapshot dropped with a resubscribe; timeout one nanosecond short and at the limit; kill and shutdown wait for the exchange; fill moves the position; reports for another segment are not applied.
- `test_live_tool.py`: a scripted exchange over real pipes; garbage report lines; reports for another segment; an exchange that never answers (NOT FLAT, exit 4); the report pipe closing (exit 6); bad option combinations.
- `test_bridge.py`: one report per request, replace as cancel then new, refusals, a lost reply, partial and full fills, an order cancelled elsewhere, fill while cancelling, orphans at segment end, a slow exchange, the same id in two segments.
- Drills (`scripts/drill.py`): `loop_flow` (acks reach hft_live; a reconnect ends flat after 60 ms of real confirmations; SIGTERM flat), `loop_fill` (a fill moves the position to +5 lots, no restart), `loop_slow` (0.4 s per call: flat, 1.9 s), `loop_dead_at_stop` (exchange down: NOT FLAT, supervisor exits 4 with `stop_not_flat`).
- Mutation check: see CHANGELOG, Phase 13.

## Not done

Real sandbox run; a position that the file does not know about (it is carried across segments and, with `--position-file`, across runs, but never read from the account, ADR 30); a venue reject taxonomy beyond what the fake produces; replay or fuzzing of the exchange link; any run longer than minutes.

## Fills from the websocket (`--ws-fills`)

`order_bridge.py --loop --ws-fills [--ws-url wss://ws-feed-public.sandbox.exchange.coinbase.com]` (needs `pip install websockets`; the URL is checked like the REST one, so production is refused) subscribes to the authenticated `user` channel. See ADR 27.

| From the socket | Result |
|---|---|
| `match` for a working order | `fill` report at once, quantity = new lots only, price = the match price |
| `match` for an unknown order, a size that is not a positive number, or a price of zero | counted (`ws_matches_unknown_order`, `ws_matches_bad`), nothing reported |
| a `match` for lots the sync already reported | counted (`ws_matches_already_reported`), nothing reported |
| `done` for a working order | a sync now, which reports whether it was filled or cancelled |
| subscription (first or after a drop) | a sync now, to find what happened while the socket was away |
| `error` (for example authentication) | logged as `ws_error`; retried with a pause of 1 to 15 s; the sync carries on alone |

A `match` never takes an order past its size. A fill that arrives just before a cancel or replace is taken from the socket after the DELETE has been answered, and then the exchange's own record of the order is read (ADR 28): whichever shows more lots decides, and anything the socket missed is reported before the cancel or replace report.

Metrics: `hft_bridge_ws_subscribed` (gauge), `ws_connects`, `ws_messages`, `ws_bad_messages`, `ws_errors`, `ws_failures`, `ws_fills` and the three counters above. Events in `bridge.jsonl`: `ws_subscribed`, `ws_error`, `ws_failed`, `ws_closed`.

Evidence: six scenarios in `test_bridge.py` (fill without a sync, socket and sync together, a lost event, a fill racing a cancel and a replace, an order cancelled elsewhere, a refused socket, a reconnection) against a fake `user` channel that checks the signature with its own code; the drills `loop_fill_ws` (sync a minute away, position in hft_live within seconds) and `loop_ws_refused`. Not shown: the real feed, its message order relative to HTTP answers, or its behaviour under load.

## A fill around a cancel (ADR 28)

After the exchange accepts a DELETE (cancel or the first half of a replace) the bridge reads the order (`GET /orders/{id}`) and compares `filled_size` with the lots it has reported. Lots it had not heard of are reported as a `fill` (at the order's limit price) before the `canceled` or `replaced` report, so the OMS never sees a cancel for lots that were bought, and the cancel report carries only what was left. An order the record shows as filled completely gets `cancel_rejected` (or `replace_rejected`). The record is capped at the order's size. If the record is not found (404: a cancelled order without fills may no longer be on file) nothing is invented; if it cannot be read (5xx, timeout, malformed) the cancel is reported from the bridge's own tally, logged as `cancel_unverified` and counted (`hft_bridge_cancel_unverified_total`), and the next sync is the only other chance to see the fill. Counter `hft_bridge_fills_at_cancel_total` counts fills found this way. The smoke test checks that `GET /orders/{id}` after a cancel returns `done_reason` and `filled_size`.

Evidence: `test_bridge.py` (cancel and replace with a fill just before the DELETE, with and without the socket, a record that says more than the order's size, a record that is missing or unreadable, a fill part from the socket and part from the record); a mutation run of the new code. Not shown: the real exchange's behaviour for a cancelled order's record.

## Closing the position when stopping (`--close-position`, ADR 29)

`hft_live --exchange-fd N --close-position [--close-collar-bps 30]` (or `supervise.py --loop --close-position [--close-collar-bps N]`). Off by default.

When it runs: the session is stopping for good (SIGTERM, end of input, or the kill switch), the exchange has confirmed that no order is working, and the position is not zero. A reconnect, a window exit, a stale feed or an integrity failure do not close: their cause is that the book cannot be trusted, and the closing price comes from the book.

What it sends: one immediate-or-cancel limit order against the position (a sell for a long, a buy for a short), for the whole position, priced `--close-collar-bps` (default 30) beyond the opposite touch of the local book and rounded away from the market on the tick grid. A partial fill is followed by another order for the rest, up to 5 orders in all; each gets the full `--flatten-timeout-s` from the moment it is sent, and no cancel is sent for it meanwhile (it cannot rest).

What stops it, each written to the segment summary (`close_why_not`) and the log (`segment_end`: `close_attempts`, `closed_lots`, `close_failed`, `close_why_not`):

| `close_why_not` | Meaning |
|---|---|
| `attempts used up` | five orders and the position is still not zero |
| `feed not fresh` | the last feed message is older than `--stale-ms`: the book is not a price to trade against. After a SIGTERM the capture stops as well, so closing has to finish within `--stale-ms` of the last message (default 3 s) |
| `no two-sided book` | no bid or no ask |
| `refused by risk or OMS` | `close_risk_code` has the check (for example the collar is wider than the risk engine's price band) |

The risk check for a closing order (`RiskEngine::check_close`) ignores the kill switch and the loss limit, because those are the states it exists for. It keeps the size, notional, price-band (against the strategy's last reference), no-reference and rate checks, and it accepts an order only if it strictly reduces the position and no other order is working, so it can neither flip nor grow a position.

The bridge carries the order out as `{"type": "limit", "time_in_force": "IOC"}` with no post-only flag, after two checks the engine cannot make: the notional cap (`--max-notional`) and the price against the exchange's own ticker mid (`--ioc-band-bps`, default 100; no usable ticker means no order). It then reads the order record (`GET /orders/{id}`) and reports `accepted`, a `fill` for the lots that traded at the executed average price, and `canceled` for the rest. If the record is missing, unreadable or does not say the order has ended, the order is kept for the sync (and the socket) to settle; if the exchange never says, the bridge sends nothing and logs `ioc_outcome_unknown`, because reporting a cancel for lots that really traded would make the engine sell again and go short. The engine's own timeout then fails the close. Any other type than post-only (4) or IOC (2) is rejected without being sent. Counters: `hft_bridge_ioc_placed_total`, `ioc_fills`, `ioc_unsettled`, `ioc_unknown`, `ioc_refused_band`, `ioc_no_ticker`, `refused_type`; in `hft_live`: `hft_live_close_orders_total`, `hft_live_close_failures_total`.

Exit: if the session still has a position (the total over all segments, ADR 30), `hft_live` prints `POSITION NOT CLOSED: N lots left (reason)`, logs `position_not_closed` and exits 4 (the supervisor reports `stop_not_flat` if it was asked to stop). The position and cash are carried from segment to segment (ADR 30), so a stop between segments, or one for a position a routine end left behind, is still counted; with no segment the reason is `no live segment to price a close`. `--position-file PATH` also carries them across runs: `<lots> <cash>`, read at start (printed if non-zero; a garbage file is refused), rewritten atomically when the total changes. It is trusted as written: stale after a kill -9 between a fill and the next loop pass, wrong if shared by two processes or if the account is traded by hand.

Evidence: six C++ test functions in `test_live` (both directions, kill and shutdown, no close on routine ends or when off, a late stop during a routine end, partial fills, attempts used up, a stale feed, a collar the risk band refuses, a timeout, prices on the grid, a one-sided book, the wait for an acknowledged order) and `check_close` in `test_oms` and `test_gw`; `test_bridge.py` (fill, partial fill, no fill, band, notional, other types, no ticker, refused POST, unreadable or hidden record, socket plus record, a capped record); `test_live_tool.py` section 12 (exit codes, event fields, five slow one-lot orders); drills `loop_close`, `loop_close_no_liquidity`, `loop_close_off_market`; a mutation run (CHANGELOG, Phase 17). Not shown: the real sandbox's behaviour for IOC orders (its status values and whether the record is there immediately), fees, and slippage beyond the collar.

Fuzzing: `fuzz_external` (the input also plays the exchange: answers, fills, bad reports; oracles: a closing order always goes against the position and never exceeds it, at most `close_max_attempts` per segment, the session is never left busy once every deadline has passed, same input twice gives the same frames). 47,000 clean runs in two minutes; of two bugs put into the closing code on purpose it caught the extra attempt and not an order slightly larger than the position (the engine only ever sends the whole position, so that one is covered by the unit tests, not the fuzzer).

