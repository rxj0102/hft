# ADR 39: A position-dependent quote size was tried, judged by the lab, and not adopted

Status: Accepted (the change is rejected; the experiment is kept)

## Context
The regime study (ADR 36) showed that removing the inventory skew earns more PnL in 5 of 7 regimes, but it holds 37 to 41 lots against 6 to 23, and it loses in the informed and thin regimes. The question was whether a different control on inventory could keep the extra PnL with a smaller position. The candidate: shrink the quote on the side that adds to the position, linearly with the position, down to one lot at the cap (`taper_pct`, 0 = off, 100 = full taper). It touches the hot path (`MarketMaker::side`).

## Decision
Do not adopt it. The strategy source is unchanged. The change is kept as `docs/experiments/taper_pct.patch` (it applies with `git apply`), with the outputs in `docs/experiments/taper_grid.txt` and `taper_regimes.txt`.

The criteria were written down before the first run:
1. Grid `taper_pct` 0,25,50,75,100 by `skew16` 0,1,2, risk penalty 1, chosen on training markets and judged on held-out markets.
2. The chosen point beats the base in at least 5 of 7 regimes and its worst regime is no worse than the base's.
3. It stays ahead of the base across the latency and queue sweeps.
Failing any one means a negative result. The default would have stayed 0 even on a win, to keep the digest and the perf baseline.

## Result
Command: `hft_lab --grid "taper_pct=0,25,50,75,100;skew16=0,1,2" --top 15 --risk-penalty 1 --markets 6 --test-markets 6 --requests 20000` (training seeds 1000..1005, held-out 101000..101005).
- At every skew, `taper_pct=0` had the highest or equal training PnL. Held-out, at skew 0: no taper +2.57, taper 25 +2.22, 50 +2.17, 75 +2.03, 100 +1.92. At the default skew 2 the position stays near 8 lots against a cap of 40, so the taper barely engages and the results are the base within noise (+1.86 against +1.87).
- The max position did not fall: at skew 0 it was 39 with taper 50 and 39 without on the held-out markets (39 and 40 on the training markets). Quoting a smaller size does not stop the position building, it only builds it in smaller steps.
- Regimes (`--regimes`, 7 regimes, 6 markets of 20,000 requests), difference from the base: no skew better in 5/7 (worst -0.51); no skew with taper 50 better in 5/7 (worst -0.64) and below no skew in 6 of 7 regimes; taper 50 at the default skew better in 2/7 (worst -0.05).
Criterion 1 failed, so criteria 2 and 3 were not pursued further; the latency and queue sweeps were not run for it.

## Consequences
The hot path, the digest, the replay oracle and the perf baseline are untouched, since nothing changed in the source. The skew remains the only inventory control. The result narrows the finding of ADR 36: the extra PnL of the no-skew variant is mostly inventory drift that a size taper does not remove.

## Rejected
- Keeping `taper_pct` in the source at default 0: it adds a branch and a field to the hot path for a feature the lab says is not useful.
- Tuning the taper shape (non-linear, or a threshold) after seeing the result: that is fitting to the held-out markets.
- A taper that also cancels the resting order when the size changes: more requests, more queue loss, and untested in the lab.

## Evidence
The saved outputs and patch above; the lab is the same one used in ADRs 33 to 38. Caveats: simulated fills from one generator; 6 markets per cell, so differences under about 0.3 are inside the standard errors (+-0.1 to +-0.7); a size change alone does not trigger a replace, so the taper applies on the next move or refill, and an immediate resize might behave differently.
