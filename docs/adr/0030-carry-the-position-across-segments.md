# ADR 30: Carry the position across segments, optionally across runs

Status: Accepted

## Context
ADR 29 closes the position when the process stops for good, but each segment had its own OMS and started at zero. A segment that ended for a routine reason (reconnect, stale feed, window exit) with a position left it behind, the next segment did not know about it, a stop that arrived between segments had nothing to close and the exit code looked only at the last segment. A restarted process had no idea what the previous one left at the exchange. Strategy position cap and risk limits therefore saw only the lots of the current segment.

## Decision
- With the external venue the session carries the signed position and the cash (sum of -side x price x qty, raw units) from one segment to the next: `finish_segment` copies them from the OMS, `start_segment` seeds the new OMS with `Oms::carry_in`. `carry_in` is valid only while no order works and nothing has filled, and leaves the fill counters alone, so `SegmentSummary::fills` still means this segment's own fills.
- `SegmentSummary::net_position_lots` is now the session total at the end of the segment. `closed_lots` is unchanged (before minus after). The stop closes the whole position, whichever segment produced it, and the strategy cap, risk limits and the (disabled by default) loss limit see the carried numbers.
- `LiveSession::net_position()` / `net_cash()`: the running segment's, or the carried ones between segments.
- Exit: `hft_live --close-position` exits 4 with `POSITION NOT CLOSED` when the session total is not zero at exit. New reason when there is no segment to price a close with: `no live segment to price a close` (a stop between segments, or a start that never got a snapshot).
- `hft_live --position-file PATH` (needs `--exchange-fd`): the process reads `<lots> <cash>` from the file at start (if it exists; a file that does not parse is refused, exit 1; a non-zero start is printed to stderr) and rewrites it, with a temporary file and rename, whenever the total changes and at exit. Written from the main loop, not from the hot path.
- The paper venue never fills, so nothing is carried there.

## Consequences
What the file says is trusted. It is wrong after a kill -9 in the window between a fill and the next loop pass, after a crash of the disk, if two processes share it, or if someone trades the account by hand. The exchange is the authority; reading the account's real position at start is not done. A position found at start can only be closed if the process gets a two-sided fresh book within the stale limit; otherwise the exit is 4. The loss limit is not enabled by the live tool, so cash continuity matters only if it is turned on later.

The hot path is unchanged (carry happens at segment boundaries).

## Evidence
`tests/test_live.cpp` (carry across a reconnect and closing it in segment 2, a stop with no segment, seeding, paper venue), `tests/test_oms.cpp::test_carry_in`, `scripts/test_live_tool.py` section 13; mutation run.
