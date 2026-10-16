# ADR 1: Single-threaded, run-to-completion pipeline

Status: Accepted (supersedes the Phase 0 sketch of SPSC rings between stages)

## Context
The Phase 0 design drew an SPSC ring between each stage. The stages together take about 128 ns hot at p50; a cross-thread handoff costs of that order per hop and adds a tail.

## Decision
Run feed handler, book, strategy and gateway on one thread, in one call stack, per input. Keep `SpscRing` in the library (tested, benchmarked) for non-hot-path uses such as log flushing.

## Consequences
No locks or atomics on the path, and replay is deterministic because there is no scheduling. Costs: one slow stage delays the next input, and the design does not scale across cores without being redone.

## Evidence
bench_t2t headline; DESIGN section 14.
