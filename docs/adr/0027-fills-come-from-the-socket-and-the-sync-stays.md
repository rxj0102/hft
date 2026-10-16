# ADR 27: Fills come from the websocket, and the REST sync stays as the authority

Status: Accepted

## Context
Phase 13 found fills by polling: up to `--sync-s` seconds late, at the order's limit price, at a cost of two calls per working order per interval. A quote that has traded still looked alive, and the strategy kept a stale view of its position.

## Decision
With `--ws-fills` the bridge also subscribes to the authenticated `user` channel (same HMAC as REST, over `GET /users/self/verify`) in a thread of its own. The thread only collects messages; the main thread takes them, so no bridge state is shared. A `match` for a working order becomes a `fill` report at the match price. The REST sync is not removed. Both paths can only raise the lots already reported for an order (`filled`), and the socket's own tally (`ws_seen`) is never trusted above the order's size, so each lot is reported once whichever path sees it first, and a lost or late event is made up by the sync. Three things trigger a sync at once instead of waiting for the interval: a (re)subscription (events may have been missed), a `done` event for a working order (the sync decides whether it was filled or cancelled), and nothing else. A fill that comes just before a cancel or replace is taken from the socket after the DELETE has been answered and before the report is written.

A refused or dropped socket is not fatal. The bridge logs it, keeps retrying (1 s to 15 s) and carries on with the sync alone, which is the Phase 13 behaviour.

## Consequences
Fill latency falls from up to `--sync-s` to the socket's delay. The sync interval can then be long (30 s or more), which spends fewer of the 15 requests a second on polling. One more dependency (`websockets`) and one more thing that can disagree with REST; the arithmetic above keeps the disagreements from becoming double fills but an undercount between the two is only repaired at the next sync. Match messages carry the execution price; REST sync fills still carry the limit price.

## Evidence
`test_bridge.py` (websocket fills, 6 scenarios plus options), the `loop_fill_ws` and `loop_ws_refused` drills, a mutation run of the websocket code. Not evidence: any contact with the real feed. The message names (`match`, `maker_order_id`, `done`, `order_id`) are taken from the documentation, and the smoke test checks the subscription and the `received`/`open`/`done` events of a real order, but cannot check a real `match`.
