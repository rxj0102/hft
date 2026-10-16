# ADR 26: The external venue is a mode of the session, and flatten becomes a state

Status: Accepted

## Context
ADR 22 made every end of trust one flatten routine that runs the paper venue forward in simulated time. A real exchange answers in real time, from another process, and may not answer.

## Decision
`LiveSession` keeps one code path for all seven ways a segment ends. With an external venue, the routine's first half (cancel everything cancellable) runs at once and the second half (wait until the OMS shows nothing working) becomes a state of the segment, finished by reports or by a deadline on the caller's clock. The caller drives it with `on_exchange_report` and `poll`. The paper mode is unchanged and remains the deterministic, replayable one.

While a segment is ending it ignores the feed and drops snapshots (asking for a new one), and the strategy is not shown reports. A missed deadline is a failed flatten: kill switch, halt, exit 4. A lost report channel is not an integrity failure: exit 6, restartable, and the next bridge cancels everything first.

## Consequences
Quoting restarts later than in paper mode (cancel confirmations, then a new snapshot). The session's decisions depend on timing and are not reproducible byte for byte. A stop that could not confirm flatness is reported as such by the supervisor (exit 4) instead of as a clean stop.

## Evidence
CLOSED_LOOP.md; `test_live`, `test_live_tool.py`, `test_bridge.py`, the loop drills; mutation check.
