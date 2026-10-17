# ADR 28: When a cancel is accepted, ask the exchange what had traded

Status: Accepted

## Context
Phase 13 and 15 learned about fills from the REST sync and the websocket. Both can lag the exchange. An order that traded 4 lots a few milliseconds before our DELETE was processed was cancelled successfully; the bridge then reported `canceled` for the whole size, the OMS closed the order at zero fills, and the 4 lots were never in the position. The websocket drain after the DELETE narrowed that window but did not close it, because the order of a socket event and an HTTP answer is not guaranteed.

## Decision
After an accepted DELETE (cancel, and the first half of a replace) the bridge reads the order once and takes `filled_size` as the authority. Missing lots are reported as a fill before the cancel or replace report; the cancel report carries the size that was left. A record that says the order filled completely turns the cancel into `cancel_rejected`. The record is capped at the order's size, a 404 invents nothing, and an unreadable record is counted and logged, then the bridge's own tally is used.

## Consequences
A cancel costs two calls and a replace three, so a quote move is a round trip slower. The check works the same with or without the websocket. It depends on the exchange keeping the record of a cancelled order for at least a moment; the smoke test checks that. A fill priced from this record carries the order's limit price, not the execution price.

## Evidence
CLOSED_LOOP.md; `test_bridge.py`; mutation run.
