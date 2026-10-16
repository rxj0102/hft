# ADR 3: Flat price-level array with intrusive order lists and an id table

Status: Accepted

## Context
A map-based book allocates and chases pointers.

## Decision
Levels are a flat array over a fixed price grid (`BookConfig`), orders live in a pool and link into per-level lists, a bitmap finds the best level, and `IdMap` maps order id to slot.

## Consequences
No allocation, cheap best-price lookup and cancel. Costs: the price grid must be sized in advance, and an update outside it is a desync. The id table is a latency parameter (ADR 7).

## Evidence
test_book against an independent `RefBook`; fuzz_md.
