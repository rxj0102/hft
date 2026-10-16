# ADR 9: Gate CI on callgrind instructions per input, not wall time

Status: Accepted

## Context
A shared CI runner cannot hold a 2% wall-clock threshold.

## Decision
`scripts/perf_gate.py` runs callgrind on the replay only and compares instructions per input with `bench/baselines/t2t_instr.json` at +2%. Mispredicts and simulated cache misses are reported, not gated. The baseline is keyed on the compiler major version.

## Consequences
Deterministic (rerun +0.00%) and proven to fail on the old code (+120.7%). Costs: it does not see memory-system effects, and the baseline must be regenerated on a deliberate compiler upgrade.

## Evidence
Gate output in the Phase 7 log.
