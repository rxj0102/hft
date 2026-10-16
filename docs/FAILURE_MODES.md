# Failure mode analysis

For each failure: what triggers it, what the engine does, what an operator sees, and which test proves it. "Latched" means it stays until a human resets it.

## Market data

| Failure | Engine response | Visible as | Evidence |
|---|---|---|---|
| Packet lost on one line | other line supplies it; arbitration closes the gap | `gaps_filled`, `dups[line]` | `gap_closed_by_other_line` |
| Packet lost on both lines | after `gap_timeout_ns`: state becomes NeedSnapshot, book marked untrusted, quotes pulled | `gaps_declared`, `Pipeline` stats `feed_losses`, `pull_sweeps`, Gateway `pulled_cancels` | `feed_loss_pulls_quotes_and_recovers` |
| Tail of stream lost (heartbeat ahead of our sequence) | a gap opens with nothing parked; a late in-order packet closes it, otherwise the timeout fires | `gap_pending`, then `gaps_filled` or `gaps_declared` | `test_heartbeat_gap_closed_by_late_packet` |
| Book rejects a message (bad order id, price off the grid, capacity) | desync: NeedSnapshot, quotes pulled | `desyncs`, `book_errors` | `book_capacity_exceeded`, fuzz_md |
| Malformed packet | counted and dropped | `malformed` | test_md, fuzz_md |
| Wrong session | dropped | `bad_session` | test_md |
| Pending store full | evict the lowest sequence or drop the new packet | `pending_evicted`, `pending_dropped` | test_md pending policy |
| Book capacity too small for the instrument | exactly one feed loss then recovery attempts; no new or replace frames while untrusted | `book_errors`, `feed_losses` | `book_capacity_exceeded` |

While the book is untrusted the engine sends cancels only. This is checked by an oracle in `fuzz_pipeline` (no 'O' or 'U' frame while the feed is not valid) and by the mutant "no early return in decide when invalid", which the fuzzer caught inside 120 s.

Recovery is automatic after a good snapshot. It is not the kill switch.

## Order entry

| Failure | Engine response | Visible as | Evidence |
|---|---|---|---|
| Exchange sequence gap | kill switch (SessionGap), sweep | `seq_gaps`, `killed()` | test_gw |
| Malformed message or broken stream | kill switch (Disconnect) | `bad_messages`, `broken_streams` | test_gw, fuzz_codec |
| Duplicate exchange message | ignored | `seq_dups` | test_gw |
| Report for an unknown order | counted; a fill for an unknown id is flagged as the dangerous kind | `unknown_reports`, `orphan_fills` | test_oms, fuzz_oms |
| Fill larger than leaves | counted as an anomaly, order state protected | `overfills` | fuzz_oms regression |
| Exchange and local leaves disagree | counted | `mismatches` | test_oms |
| Order not cancellable yet (PendingNew) | sweep repeats after each later report until nothing is left | `kill_cancels` / `pulled_cancels` grow | test_gw, test_t2t |

## Risk

Checks run in this order: kill switch, loss limit, parameters, order size, notional, price band, position, open orders, rate limit. The rate limit is last so refused orders do not spend a token.

| Condition | Result |
|---|---|
| Mark-to-market PnL below `-max_loss_raw` | `MaxLoss`: kill switch trips and stays latched; working orders are cancelled; the position is left as it is |
| Manual `Gateway::kill` | latched, sweep |
| Any limit breach | the single order is refused (`risk_rejects`), nothing else changes |

Cancels bypass risk, so quotes can always be pulled.

## Process

| Failure | Response |
|---|---|
| Allocation on the hot path | operator-new trap fails the test; not detectable at runtime in production |
| Page fault in the timed loop | `bench_t2t` exits 3 |
| Behaviour change in an optimisation | wire hash mismatch; `bench_t2t` exits 2 before printing a timing |
| Latency regression | `perf_gate.py` fails above +2% instructions per input |
| Hugepage or mlock unavailable at startup | startup continues; `RegionReport` shows what was achieved; latency will be worse |

## Real-data path (offline capture and replay, Phase 9)

| Failure | Response | Visible as | Evidence |
|---|---|---|---|
| WebSocket drops | capture reconnects with backoff, writes a `reconnect` marker; the server sends a new snapshot, which starts a new segment | markers in the file; segment count in `hft_replay` | `scripts/test_capture.py` |
| Server goes silent | after `--idle-timeout` the connection is treated as dead and reconnected | `idle_timeout` marker | test_capture |
| Too many reconnects | the capture gives up and writes `give_up` | marker, exit with the stop record | test_capture |
| Damaged or unknown message | counted, skipped; never produces book changes | `bad`, `other` in the load line | test_l2 (20,000 damaged messages), fuzz_l2 |
| Update before any snapshot | dropped and counted | `dropped before a snapshot` | test_l2 |
| Snapshot with one side only | cannot be centred; unusable | `unusable snapshots` | test_l2 |
| Price off the tick grid | dropped, counted | `off-tick` | test_l2 |
| Price outside the grid window | dropped, counted | `outside window` (about 65% of changes on a real BTC capture: far-from-market levels) | test_l2 |
| Touch nears the window edge | the segment ends; later messages are not replayed | `ended: price left the window` | test_l2 scripted exit |
| Size finer than one lot | floored, counted | `floored sub-lot sizes` | test_l2 |
| Size beyond the 32-bit wire field | clamped, counted | `clamped` | test_l2 |
| Converter emits something the book rejects | would show as a desync or a model mismatch; both are fuzz oracles | `desyncs`, `model check` failures, exit 4 | fuzz_l2, test_l2 |
| Replay not deterministic | exit 2 | `determinism: DIFFERENT` | every `hft_replay` run |

Not handled: re-centring the grid when the market leaves it (the segment simply ends), gaps inside a connection (the feed has no sequence numbers to detect them), and any behaviour on a live connection.

## Known gaps

- Position is not flattened automatically after a kill. That is a human decision (see the runbook).
- One outstanding request per order; a late report for an evicted order id is counted as unknown.
- No heartbeat or retransmission on order entry.
- No handling of exchange halts, auctions, or crossed/locked books beyond what the synthetic exchange produces.
- Single instrument per pipeline instance.
- Nothing here has been exercised against a real exchange or real data.
