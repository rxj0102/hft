# ADR 48: A live run replays to its hash only when no decision depends on message timing

Status: Accepted

## Context
ADR 46 recorded each supervised run's stream and expected `hft_live --from` to reproduce the live order-flow hash. In Phase 41 three runs did, and ADR 47 fixed a real cause of one mismatch (the kill sweep). Phase 44 ran 30 minutes with eight recorded runs and only one matched; seven ended by themselves or by SIGTERM with live and replay hashes that differed, with small count differences (for example 4,420 against 4,421 replaces).

Live mode takes its clock from the arrival time of each line; replay takes it from the recorded stamps (`src/tools/hft_live.cpp`). The paper venue acknowledges after a round trip (50 us), and the strategy and OMS act on what has been acknowledged, so the number and order of replaces depends on the gaps between packets.

Tests on the Phase 44 recording of run 1 (4,208 lines):
- Replay twice: the same hash both times (deterministic).
- The same file piped into live mode three times: three different hashes (2,383, 2,812 and 2,444 replaces), because the arrival gaps differ from run to run.
- Replay with `--rtt-us 30`, 50 and 80: 3,924, 4,421 and 4,421 replaces, so the hash depends on the clock.

## Decision
- A live-versus-replay hash match is not a determinism check of the live path. A match shows no decision in that run depended on timing; a mismatch shows nothing broken by itself.
- Determinism is checked by replay against replay (same input, same hash) and by unit tests and drills with a controlled clock.
- RUNBOOK and SANDBOX say so. The `record` and `record_term` drills stay: on the fake feed their gaps are wide enough that they match, and they check the recording and the cut.

## Consequences
- The Phase 41 expectation "a run that ends by itself replays to its live hash" is withdrawn as a general claim. It held for four runs and failed for seven in Phase 44.
- It is not known whether Phase 41's SIGTERM mismatch was the kill sweep, timing, or both. The sweep was a real cause (unit test); the timing effect was not known then.
- A live run that must be reproduced exactly needs the arrival clock recorded, or a capture stamp used as the live clock. Not built.
