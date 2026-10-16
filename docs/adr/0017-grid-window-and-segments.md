# ADR 17: Fixed grid window per segment; the segment ends when the market leaves it

Status: Accepted

## Context
BTC at $82,000 with a $0.01 tick would need millions of levels; the book's grid is fixed at init and the id table and level arrays compete for cache.

## Decision
Centre a window of 65,536 ticks (about $655) on the first mid. Drop and count changes outside it. End the segment when the touch comes within 10% of an edge.

## Consequences
Bounded memory and a book that is never silently wrong. Costs: about 65% of feed changes were dropped on a real capture (levels far from the market); there is no re-centring yet.

## Evidence
Window boundary tests; scripted window exit replayed through the pipeline; fuzz_l2.
