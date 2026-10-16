# ADR 4: Every latency measurement is gated on byte-exact replay

Status: Accepted

## Context
An optimisation that changes behaviour makes a faster number meaningless.

## Decision
Record what the strategy side of a Backtester run saw and sent. Replaying those inputs through the real `Pipeline` must produce the same wire bytes (FNV over frames). `bench_t2t` verifies first and exits 2 on a mismatch.

## Consequences
Optimisations were free to be aggressive. Costs: the oracle covers only recorded sessions, so behaviour outside them relies on tests and fuzzers.

## Evidence
`replay_equals_backtest` on three latency configs; the pinned backtest digest was unchanged across all Phase 7 optimisations.
