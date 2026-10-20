# ADR 50: Record the exchange's reports in the clock file

Status: Accepted

## Context
ADR 49 recorded the clock of a live run so that it could be replayed exactly, but left out the exchange's reports. With `--exchange-fd` (the closed loop) the strategy and the flatten and close logic react to reports that arrive at times of their own, so such a run could not be reproduced.

## Decision
- The clock file begins with `H 1` when the run had an exchange behind it (`--exchange-fd`) and `H 0` otherwise. A file without that line is read as `H 0`.
- Each exchange report that parses is written as `R <ns> <report line>`, with the same time value that was handed to the session. Lines that do not parse are counted by the live run but change nothing, so they are not recorded; the replay's `unparsable` count is therefore 0.
- `--from STREAM --clock-in FILE` on an `H 1` file runs the session with an external venue, applies each `R` as a report at its time, and carries on after the `K` or `S` record, because a flatten or a close waits for confirmations that come after it. A file that ends without a `K` or `S` (a killed process) is ended by a shutdown at the last time.
- `--close-position` is accepted for such a replay; give the same flags as the live run (`--stale-ms`, `--flatten-timeout-s`, `--close-position`, `--close-collar-bps`), they change decisions.
- `--clock-out` is refused with `--position-file`: the account check (`acct` report, and the 10 s wall-clock limit on it) is not recorded. `supervise.py --record-dir` writes the clock in bridge and closed-loop modes unless a position file is in use.

## Consequences
- A closed-loop run against the scripted exchange replays to the same summary (every line except wall-clock cost, and the unparsable count), the same hash and the same exit code, in six modes: normal, close in full, close that fails, a silent exchange, garbage reports, reports for the wrong segment.
- The replay prints only the summary; it does not write orders anywhere or call the exchange.
- Not covered: the account check, a real exchange (never run), a SIGKILLed process beyond a file cut by hand.
