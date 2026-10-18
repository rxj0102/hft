# ADR 36: Compare variants across named market regimes

Status: Accepted

## Context
Every lab comparison so far came from one set of generator settings. A variant that is best there may be best only there: in the first lab runs the variant with no inventory skew won on PnL while holding five times the position, and a weak signal or a fast market could reverse that. A held-out split (ADR 34) uses fresh seeds from the same settings, so it cannot show this.

## Decision
- `hft_lab --regimes a,b,...` runs the same variants on the same seeds under each named regime and prints one table (and, with variants, one paired comparison) per regime.
- A regime is a set of existing `MarketConfig` fields; the generator is unchanged. Seven are defined: `default` (the generator's own settings), `volatile` (jump period 100 instead of 400), `quiet` (1600), `informed` (informed taker chance 70 instead of 40 and lean 55 instead of 35), `uninformed` (10 and 10), `thin` (60 resting orders instead of 200), `slow` (requests 40 us apart instead of 10).
- With two or more regimes and at least one variant, a summary shows each variant's mean PnL difference from the base per regime, in how many regimes it is above zero, and its worst regime. "Better in" counts regimes; it is not a test, and every regime uses the same seeds.
- Names are case sensitive; unknown names, empty names and repeats are refused. `--regimes` cannot be combined with `--grid`: searching across regimes would let the held-out comparison choose.
- `--json` gains a `regimes` array (name and variants per regime); the top-level `variants` stay the first regime's. With `--events`, files are prefixed with the regime name. The run digest chains the regimes in the order given.
- Without `--regimes` the output is byte-identical to before (checked on the Phase 25 example command).

## Consequences
The regimes change the generator's settings, not its kind: all of them still have a latent value with jumps, informed takers, and noise. A variant that survives all seven has survived seven settings of one simulator, nothing more. The values were chosen to move one thing at a time by a factor of two to four; they are not calibrated to any real market. Several regimes at once means many comparisons, and the summary does not adjust for them.

The hot path and the replay oracle are untouched.

## Rejected
- New generator behaviours (trends, volatility clustering, regime switching inside a market): they would change what a market is and need their own tests of the generator; the settings already there move the properties the strategy depends on.
- A combined score across regimes: it would hide the one regime where a variant fails, which is the thing to look at.
- Regimes inside `--grid`: see above.

## Evidence
`tests/test_lab.cpp` (table pinned value by value, parsing and its errors, the across-regime summary by hand, `evaluate` with a regime equals the single run on that regime's market), `scripts/test_lab.py` (option checks, one regime equals the plain table, three regimes with headings, json, the summary recomputed from the json, event file names, digest repeats and depends on order), mutation run.
