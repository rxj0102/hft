# ADR 34: Choose parameters on training markets, judge them on held-out markets

Status: Accepted

## Context
With the strategy lab (ADR 33) it is easy to run many variants. The best of 48 grid points on 8 markets looks good partly because it was the best of 48: the winner's curse. Reporting the training figure as the result would repeat the mistake the lab exists to avoid. A comparison also says nothing about position risk: in the first lab runs the variant with no inventory skew won on PnL while holding five times the position.

## Decision
- `hft_lab --grid "key=v1,v2;key2=w1,w2"` expands the cartesian product (at most 8 axes, 50 values per axis, 500 points; each point named `key=v_key2=w`). It cannot be mixed with `--variant` or `--sweep`.
- Training: every grid point and the base run on the training markets (`--seed`, `--markets`). Points are ranked by an objective: mean PnL minus `--risk-penalty` times the mean largest absolute position (penalty default 0, so plain mean PnL). The best `--top K` (default 3) are kept.
- Validation: only the K chosen and the base run on held-out markets (`--test-seed`, default seed + 100000; `--test-markets`, default the training count). The seed ranges must not overlap; the tool refuses overlapping ranges. Grid points that were not chosen are never run on the held-out markets, so the held-out result cannot be used to pick a different winner by accident.
- The report shows, for each chosen point, the training gain and the held-out gain in the objective over the base, and the fraction retained; the average shrinkage; the paired held-out comparison with the base (the ADR 33 statistics); and a warning when a chosen value is the smallest or largest on its axis (the optimum may lie outside the grid).
- Events (`--events`) are written for held-out runs with a `test_` file prefix.

## Consequences
The held-out markets come from the same generator as the training markets, so retention near 100 percent says the choice is not an accident of the seeds, not that it would survive a different market. Retention is only a number about sampling noise and selection among the grid. Using the held-out seeds again to adjust the grid spends them; the report says so, and the remedy is fresh seeds. A larger penalty picks lower-inventory points at the cost of PnL; the right value is a judgement the tool does not make.

The hot path and the replay oracle are untouched.

## Rejected
- Cross-validation folds: more machinery than a handful of markets supports, and the fold split of one generator's iid markets adds nothing over a single honest split.
- Running the whole grid on the held-out markets as well, to print a "random point" baseline: it would put every point's held-out result on screen, which invites picking from it.
- Bayesian or evolutionary search: with nine integer parameters and runs in the tenths of a second a grid is enough, and its output is easier to audit.

## Evidence
`tests/test_lab.cpp` (grid expansion and limits, ranking and ties, penalty, retention, edge detection, `evaluate` against single runs and the digest chain), `scripts/test_lab.py` (option checks, selection recomputed independently from the json, gains and retention recomputed, seed ranges, event file names, penalty changes the order), mutation run.
