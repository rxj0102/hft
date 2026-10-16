# ADR 19: Recorded exchange data is not committed

Status: Accepted

## Context
Redistribution of exchange market data may be restricted, and the repository may be public.

## Decision
`data/` is ignored; fixtures are hand-made with invented prices; seeds for fuzzing are synthetic.

## Consequences
No licensing exposure. Costs: results on real data are reproduced by recapturing, not by cloning.

## Evidence
.gitignore; fuzz/seeds/l2.
