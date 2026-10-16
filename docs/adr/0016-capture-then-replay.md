# ADR 16: Capture and replay offline before any live connection

Status: Accepted

## Context
A live connection adds clocks, reconnects and TLS before the book is known to be right on real data.

## Decision
Phase 9 records raw feed messages with receive times and replays them. A live paper mode comes after replay is trusted.

## Consequences
Every real-data result is reproducible from a file, runs deterministically, and can be fuzzed. Costs: this does not exercise a live connection, and timing excludes the network path.

## Evidence
Determinism check on every run; identical wire hashes on gcc and clang.
