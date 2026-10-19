# ADR 41: The exchange-snapshot check compares only levels inside the price window

Status: Accepted

## Context
`hft_replay --verify` compares the 10 nearest levels per side of the exchange's own snapshot with the replayed book (ADR 18). The replay keeps only a price window (65,536 ticks, 655 USD, centred on the first mid) and drops changes beyond it by design (ADR 17). On the dense production book the nearest 10 levels sit inside the window. The first capture of the real Coinbase sandbox (2026-10-19, Phase 34) has a sparse book: its 10 nearest bids ran from 81787 to 79759 and its 10 nearest asks from 81837 to 83520. The tool printed "6 of 20 levels identical, 14 absent from the replayed book". By hand, exactly 6 of the 20 levels were inside the window and they matched; the 14 absent ones were all outside it. The line was true and misleading, and it would have hidden a real miss among the 14.

## Decision
- `verify_against_snapshot` still takes the N nearest levels per side of the exchange snapshot. A level whose tick index is outside `[base, base + num_levels)` of the segment is counted in a new field, `outside_window`, and not compared. The others are compared as before (identical, differing size, absent).
- A level outside the window still uses up one of the N, so "the 10 nearest" keeps its meaning; the compared count can be below 20.
- `hft_replay` prints the old line over the compared levels, and a second line, only when there are such levels: "N more of the nearest levels are outside the price window, which drops them by design: not compared".
- The exit code is unchanged: the comparison stays informational (a mismatch does not fail the run), as before.

## Consequences
On the real sandbox capture the line reads "6 of 6 levels identical, 0 differ in size, 0 absent" with 14 outside the window, which says what was and was not checked. The check says nothing about the levels beyond the window, and with a sparse book it covers few levels; a short window or a thin book can make it cover none (compared = 0), which the output shows as "0 of 0".

## Rejected
- Widening the window: it changes the engine's grid and memory, for the sake of a diagnostic.
- Choosing the N nearest levels that are inside the window: it would compare levels farther from the touch than "nearest 10", and hide that the nearest ones were out of reach.
- Failing the run on a mismatch: not decided here; the skew between two connections makes small differences normal (LIVE_FEED.md).

## Evidence
`tests/test_l2.cpp` (`test_verify_outside_window`: both edges of the window, the nearest-N rule with an outside level first, counts), `scripts/test_replay_cli.py` (the process output: identical with far asks, no note when nothing is outside, a differing size, an absent level, all together, no `--verify`, a missing capture), run in the gcc release cell of CI. Mutation checks: 16 mutants (window edges, the base index, the taken count, each counter, the note and the order of the printed numbers), all killed. Real data: `docs/SANDBOX.md`. Caveat: one 90-second capture of a book that is not a real market.
