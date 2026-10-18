# ADR 38: Sweep the queue penalty to stress the fill model

Status: Accepted

## Context
Every simulated fill in the lab rests on one assumption: where our order stands in the queue. The tape gives the exact queue ahead of us at the moment we join, and the venue model lets only trades (not cancels) work it off. A real venue can differ (hidden size, priority rules, our order arriving later than modelled). The backtester already has `queue_penalty_pct`, which pretends an extra p percent of the level's quantity was ahead of us. `hft_lab` set it once per run, so it could not show how much a result depends on it.

## Decision
- `hft_lab --queue-sweep pct1,pct2,...` runs the same variants on the same markets once per penalty (strictly increasing integers, 0 to 1000 percent, at most 12). `--queue-penalty-pct` is replaced by each value; the latencies, jitter and fee are as given.
- It uses the same output as the latency sweep (ADR 37): a table per value, the mean PnL of every variant by value with `kept` (the largest penalty over the smallest), and each variant's difference from the base by value with the count above zero and the worst. The code is shared; the latency sweep's output is unchanged.
- `--json` writes `queue_penalties` (each with `pct` and variants); `--events` files are prefixed `q<pct>pct_`.
- It cannot be combined with `--latency-sweep`, `--grid`, `--regimes`, or `--latency-axis`.

## Consequences
The result is a curve of simulated PnL against a stress on the queue assumption. It shows which variants depend on fills that a deeper queue would remove (typically the ones that quote tight and trade often), and it gives a floor: a variant whose advantage over the base disappears at modest penalties has no robust advantage here. It does not say what the real queue is, and it adds the same fraction for every order. A penalty above 100 percent is a deliberately pessimistic world.

The hot path and the replay oracle are untouched.

## Rejected
- Calibrating the penalty to a real venue: it needs recorded fills from that venue, which this project does not have (the sandbox was never run).
- A separate penalty per side or per level: more parameters than the evidence could pin down.
- A "breakeven penalty" single number: the curve is noisy and not always monotone; the table shows it.

## Evidence
`tests/test_lab.cpp` (parsing and its errors, the latency messages unchanged, `with_queue_penalty` sets only that field, a penalty changes the run and equals the single run), `scripts/test_lab.py` (option checks and every conflict, tables per value, json, means, kept and differences recomputed from the json, event names, digest, a sweep point equals the plain run and ignores `--queue-penalty-pct`, fees apply, configured latencies kept), mutation run.
