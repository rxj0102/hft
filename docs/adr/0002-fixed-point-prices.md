# ADR 2: Prices are int64 fixed point; no floating point on the path

Status: Accepted

## Context
Float rounding makes price comparison and replay digests unreliable.

## Decision
`Price` is a raw int64 with a per-instrument tick in raw units. Strategy math is integer, in 1/16 tick units.

## Consequences
Exact comparisons and bit-identical replay across gcc and clang. Costs: every conversion is explicit, and a price off the grid is rejected rather than rounded.

## Evidence
Golden backtest digest pinned in test_bt; gcc and clang reports are bit-identical.
