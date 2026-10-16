# ADR 8: Rejected: grouped-by-4 Fibonacci id hash

Status: Rejected

## Context
A grouped probe looked like a way to cut probe misses.

## Decision
Not adopted.

## Consequences
It cut p99 by 24-32% on an over-sized table but made it 4-7% worse on a right-sized one. ADR 7 removes the case where it helped.

## Evidence
A/B runs in the Phase 7 log.
