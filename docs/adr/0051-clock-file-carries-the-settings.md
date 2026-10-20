# ADR 51: The clock file carries the settings that change decisions

Status: Accepted

## Context
ADR 50 asked the person replaying a recorded run to repeat the live run's flags (`--stale-ms`, `--flatten-timeout-s`, `--close-position` and others). A flag forgotten or mistyped gives a different order flow with no sign of why, which looks like a nondeterminism bug. On a recorded stream, replaying with `--rtt-us 30` instead of 50 changed the order count (ADR 48).

## Decision
- A `--clock-out` file has a second header line after `H`: `C stale=<ns> rtt=<ns> lots=<n> levels=<n> maxord=<n> chk=<n> flat=<ns> close=<0|1> collar=<bps>`. These are the nine settings that change decisions (`--stale-ms`, `--rtt-us`, `--lots`, `--levels`, `--max-orders`, `--check-every`, `--flatten-timeout-s`, `--close-position`, `--close-collar-bps`).
- `--clock-in` applies them. If any of those flags was also given by hand and the values differ from the file's, the file wins and one line on stderr names both sets. Flags equal to the file's say nothing.
- A file without a `C` line (written by ADR 49 or 50 builds) uses the flags given. A damaged `C` line is refused (exit 1).
- Flags that do not change decisions (`--log`, `--metrics`, `--report-s`, `--max-seconds`) are not recorded.

## Consequences
- Replaying a recorded run needs no flags: `hft_live --from STREAM --clock-in CLOCK`. The tests and the `record_modes` drill now do exactly that.
- Settings added to the program later must be added to this line or the replay will silently differ; the test that replays without flags (with every setting at a non-default value) is what would show it, so a new setting needs a case there.
- `max-orders` is recorded and applied, but no test can show that it matters: it did not change the output on any stream tried (book capacity is never reached). If it is ever reached the run would already be failing a snapshot or desyncing.
