# ADR 37: Sweep latency to see how fast a variant decays

Status: Accepted

## Context
The backtester has had configurable delays from the start (market data in, order in, report out), and `hft_lab` set them once per run. Whether a variant's edge survives a slower path is the question latency work is for, and one setting cannot answer it. It also matters which hop is slow: a stale view of the book and a late order are different problems.

## Decision
- `hft_lab --latency-sweep us1,us2,...` runs the same variants on the same markets and seeds once per delay (strictly increasing, 0 to 100,000 microseconds, at most 12). `--latency-axis all|md|ingress|egress` (default `all`) says which hop gets the delay; the other hops keep the values from `--md-us`, `--ingress-us`, `--egress-us`. Jitter and the queue penalty are unchanged.
- Output: a table per delay, the mean PnL of every variant by delay with `kept` (mean PnL at the largest delay over mean PnL at the smallest, n/a when the smallest made no money), and, with variants, each variant's difference from the base by delay with the count of delays where it is above zero and its worst.
- `--json` writes `latency_axis` and a `latencies` array (delay and variants); with `--events` the files are prefixed `lat<us>us_`.
- It cannot be combined with `--grid` or `--regimes`, and `--latency-axis` needs `--latency-sweep`. The markets are the default regime.
- Fees (`--maker-fee-bps`) apply as in other runs.

## Consequences
The result is a curve of simulated PnL against delay. The venue is a queue model and the tape does not react to our orders, so this says how much of a variant's simulated PnL depends on being quick, not how quick a real system must be. `kept` is a ratio and is unstable when the smallest-delay PnL is small. Delays are applied to the strategy-to-venue path as the backtester models it; the 300 ns of the engine itself is not part of this (it is far below the smallest nonzero delay).

The hot path and the replay oracle are untouched.

## Rejected
- Reporting latency as one number ("breakeven latency"): the curve is not monotone in a finite sample and a breakeven would hide that.
- Sweeping jitter as well: jitter is a different question (variance, not level) and would double the output.
- A significance test per delay: the columns share seeds and the variants share markets; the table shows the means, and the paired sign tests of the normal run are available at any single delay.

## Evidence
`tests/test_lab.cpp` (parsing and every error, axis names, which fields each axis changes and which it keeps, a longer delay changes the run and equals the single run), `scripts/test_lab.py` (option checks, tables per delay, json, means, kept and the differences recomputed from the json, event names, digest, the three axes give three results, other hops keep their values), mutation run.
