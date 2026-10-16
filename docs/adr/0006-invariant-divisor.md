# ADR 6: Reciprocal multiplication for the tick division

Status: Accepted

## Context
Price to tick index divides by a divisor fixed at init; a hardware 64-bit divide is slow.

## Decision
`core::InvariantDivisor`: M = floor((2^64-1)/d), high multiply, one correction step. Negative offsets use plain division.

## Consequences
Hot p50 144 -> 123 ns together with the direct depth walk. Exact for all 64-bit inputs (proof in the header). Costs: one more type to trust, covered by edge values and 3M random pairs against hardware division, and by mutants.

## Evidence
test_core `test_invariant_divisor`.
