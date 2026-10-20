# ADR 52: Record only the timer polls that can act

Status: Accepted

## Context
ADR 49 recorded every idle timer poll (`P`), about 1,000 a second. In the 2-hour soak of Phase 49 the clock files were 91 MB against 6.7 MB of stream, almost all of it polls that did nothing.

## Decision
- `LiveSession::poll_idle(now)` is true when `poll(now)` would change nothing: time does not run backwards, no segment is ending (flatten or close waiting for the exchange), the session is halted or has no segment, or the stale deadline is not reached and no paper-venue report is due. It is conservative: anything else is "not idle".
- `hft_live --clock-out` writes a `P` record only for a poll that is not idle. It still makes every poll.
- The file format is unchanged. A replay of an older file that has all the polls works as before (the extra polls change nothing).
- A property test (`test_live`) runs 4,000 steps with random gaps against two sessions, one polling every time and one only when not idle: an idle poll leaves every counter as it was, and the two end with the same segment hashes, frame counts and reasons. A tool test checks that 1.5 s of silence past a 1 s stale limit records one or a few polls, pulls the quotes, and replays to the same hash.

## Consequences
- The clock file is now about one line per input line, plus the few polls that mattered.
- Reasoning behind it: a paper-venue report is delivered with its own due time, not the poll's, and the next input line would deliver the same reports first, so a poll that finds nothing due does not change the run. The tests above are what hold this; a new kind of time-driven behaviour in `poll()` must be added to `poll_idle()` or replays will differ.
