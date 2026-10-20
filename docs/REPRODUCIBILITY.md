# Reproducing a live run

Status after Phases 41 to 47 (2026-10-20). This page says what can be reproduced exactly, how, what was shown, what was withdrawn and what is still open. Details are in ADRs 46 to 51 and the CHANGELOG; measured runs are in [SANDBOX.md](SANDBOX.md).

## What "reproduced" means here

`hft_live` prints a wire hash of the order flow the feed caused (flatten and the kill sweep excluded). Two runs are the same when the hash, the summary and the order frames match. A live run is **not** reproducible from its input lines alone, because its clock is the arrival time of each line (ADR 48). Replaying the stream with the capture's stamps (`--from STREAM`) is deterministic, but it is a different run from the live one, and usually has a different hash.

## How to reproduce a run exactly

1. Record the run: `python3 scripts/supervise.py --record-dir DIR ...` (or `capture_l2.py --tee STREAM | hft_live --clock-out CLOCK`). This keeps `DIR/stream.<run>.txt` and `DIR/clock.<run>.txt`. Do not commit them.
2. Replay it: `hft_live --from DIR/stream.N.txt --clock-in DIR/clock.N.txt`. No other flags are needed.

The clock file holds, in order: whether an exchange was behind the run (`H`), the nine settings that change decisions (`C`: stale-ms, rtt-us, lots, levels, max-orders, check-every, flatten-timeout, close-position, close-collar-bps), and then one line per time value handed to the session: input line (`L`), idle poll (`P`), exchange report with its text (`R`), and the end (`K` for a signal, `S` for end of input). A flag given by hand that conflicts with the file loses, with one warning. The replay prints only the summary.

## What was shown

| Claim | Evidence | Sample |
|---|---|---|
| The kill sweep is not part of the feed hash (ADR 47) | A unit test failed 3 checks on the old code and passes now | one test |
| Live and stamp replay agree only if no decision depends on timing (ADR 48) | Phase 44: 1 of 8 runs matched. The same file piped into live mode 3 times gave 3 hashes; replay twice gave one | 8 runs, 30 min, one feed |
| With the clock, a live run replays exactly (ADR 49) | The 8 Phase 44 streams piped live and replayed from their clocks: 8 of 8. Two sandbox runs (one ended by SIGTERM): 2 of 2. Four test runs fed in random bursts: 4 of 4 | small; the 8 were piped at file speed |
| With the exchange's reports, a closed loop replays exactly (ADR 50) | Six scripted-exchange cases (normal, close, failed close, silent, garbage, wrong segment) match in hash, summary, exit code and order frames; 3 recorded streams driven through the scripted exchange, with and without closing: 6 of 6 | fake exchange only |
| The settings travel with the clock (ADR 51) | A replay with no flags matches a live run with non-default settings; each of the nine flags alone against the file warns and loses | tests |

Mutation checks were run for each step; the counts and the survivors are in the CHANGELOG.

## Withdrawn or corrected

- Phase 41 said a run that ends by itself replays to its live hash. That held for the runs of that session and not for Phase 44; it is not a general claim (ADR 48). Whether Phase 41's SIGTERM mismatch was the kill sweep, timing, or both is not known. The sweep was a real cause.
- The Phase 42 and 44 runs were described as sandbox runs. They used `capture_l2.py`'s default URL, which is the **production public market-data feed** (read-only, no keys, no orders). The Phase 42 and 44 text in SANDBOX.md is corrected. Phase 41, the first Phase 42 attempt, and the Phase 45 live runs used the sandbox URL given explicitly. Pass `--url wss://ws-feed-public.sandbox.exchange.coinbase.com` when the sandbox is meant.
- An earlier note said the container could not reach the sandbox. It can; that was an untested assumption.

## Not covered

- A SIGKILLed process: no summary, and its clock is cut off without an end record. Only tested on a file cut by hand.
- `--position-file` (the account check is not recorded; `--clock-out` is refused with it).
- Any real or sandbox exchange behind the loop. The signed side has never been run (no secret or passphrase). Everything in the closed-loop rows above is the repo's scripted exchange.
- `--max-orders` is recorded and applied but no test can show it matters.
- A new setting added to the program must also be added to the `C` line, or replays will silently differ (ADR 51).
- Real arrival patterns: the 8 streams in the strongest evidence were piped at file speed, not received in real time.
- Paper venue only: it never fills.

## Related

[RUNBOOK.md](RUNBOOK.md) (running and recording), [SANDBOX.md](SANDBOX.md) (the runs), ADRs [46](adr/0046-record-the-stream-of-a-supervised-run.md), [47](adr/0047-kill-sweep-is-not-in-the-feed-hash.md), [48](adr/0048-live-hash-equals-replay-hash-only-without-timing-effects.md), [49](adr/0049-record-the-live-clock-to-reproduce-a-live-run.md), [50](adr/0050-record-the-exchanges-reports-in-the-clock-file.md), [51](adr/0051-clock-file-carries-the-settings.md).
