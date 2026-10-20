# ADR 49: Record the live clock so a live run can be reproduced exactly

Status: Accepted (extended by ADR 50: header line and exchange reports; ADR 52: only polls that can act are recorded)

## Context
ADR 48 found that a live run's order flow depends on message arrival times, which replay does not have (it uses the capture's stamps), so live and replay hashes mostly differ. To check that the live code path is deterministic given its inputs, the inputs must include the clock.

## Decision
- `hft_live --clock-out FILE` writes, in order, every time value the program hands to the session: `L <ns>` before each input line, `P <ns>` for each timer poll while idle, and a last `K <ns>` (stopped by a signal) or `S <ns>` (input ended). One line per call, text, buffered, closed at exit. A SIGKILLed process leaves a truncated file without a final line.
- `hft_live --from STREAM --clock-in FILE` replays the stream with exactly those values: each `L` takes the next stream line, each `P` is a poll, `K` or `S` ends the run the same way. The replay prints only the summary (no periodic status lines, no metrics, no orders pipe). A clock file with more `L` entries than the stream has lines is refused (exit 1).
- `--clock-out` is refused with `--from`; `--clock-in` needs `--from`. (ADR 50 later allows `--exchange-fd` and refuses `--position-file`.)
- `supervise.py --record-dir DIR` also passes `--clock-out DIR/clock.<run>.txt` (ADR 50: also with `--loop`), and logs it in `pair_start`.

## Consequences
- A recorded run replays to its live hash exactly, including when it ended by SIGTERM, whatever the arrival timing was. Tested: the same stream fed live with different burst patterns gives different hashes, and each replays exactly with its own clock.
- Costs when the flag is on: one buffered `fprintf` per input line and per idle poll in the tool's loop, outside the engine. Off by default: a null check. The perf gate was not rerun; the engine is untouched.
- Size: as first written, idle polls (about 1,000 a second) dominated the file; in a 2-hour soak the clocks were 91 MB against 6.7 MB of stream. ADR 52 records only polls that can act.
- Not covered: reports from a real or fake exchange (`--exchange-fd`), the wall-clock-dependent logging and metrics (they do not feed decisions), and a process killed by SIGKILL.
