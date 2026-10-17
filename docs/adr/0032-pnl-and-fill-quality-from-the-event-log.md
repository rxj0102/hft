# ADR 32: PnL and fill quality come from the event log, computed offline

Status: Accepted

## Context
With real fills (Phases 13 to 16) the engine can trade, but nothing said whether it makes or loses money, or why. The OMS keeps cash and position, but not when a fill happened, where the market was, or what the market did next.

## Decision
- `hft_live` writes one `fill` event to its event log for every fill report the exchange sends: `side`, `px`, `qty` (raw), `mid_x2` (best bid + best ask of the local book at the moment before the report was applied, -1 if the book has no two sides), `pos` (the session's position after the report) and `applied` (0 if the session ignored the report, for example for a segment that no longer exists). Written from the report reader, outside the tick-to-trade path.
- `--mid-sample-ms N` (supervisor: `--mid-sample-ms`, default 200 with `--loop`, 0 turns it off) adds `mid` events: the same quantity, only when it changed, at most every N ms.
- `scripts/pnl_report.py` reads one or more event logs and reports: PnL by average cost (inventory carried into the run is valued at the first fill's mid) cross-checked against a cash-flow figure; edge against the mid at the fill; markouts at 1, 5 and 30 s (positive means the market moved the fill's way, negative is adverse selection); inventory (largest, time-weighted mean); and a consistency check of the engine's position against the sum of the fills.
- The mid is stored doubled so that no rounding happens in C++. Fees are one rate on all notional (`--fee-bps`, default 0); maker and taker fills are not distinguished.

## Consequences
The report is only as good as the local book: if the feed lags the exchange, edge and markouts are measured against a stale mid. A markout needs mid samples that reach past its horizon, so the last seconds of a run have none (it says so instead of inventing one). With the default sampling the log grows by roughly 40 MB a day on a busy book. A fill that the bridge never reported (a lost websocket event that the sync did not find either) is not in the log, which the position check turns into a visible warning rather than a silent error.

The hot path is unchanged.

## Evidence
`scripts/test_pnl.py` (hand-computed round trip, adverse selection, crossing zero, carried inventory, ignored and unreadable input), `scripts/test_live_tool.py` section 14, drill `loop_close` (the report on a real supervised run), mutation run.
