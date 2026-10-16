# ADR 7: Book capacity is a latency parameter, sized per instrument

Status: Accepted

## Context
The id table competes for L1. Over-sizing it was costing real time (p99 388 ns at 65536 orders versus 254 ns at 1024).

## Decision
Size capacity at 3 x peak resting orders, power of two, minimum 1024. Overflow fails safe: one desync, quotes pulled, snapshot recovery.

## Consequences
Lower tail. Costs: capacity has to be known per instrument, and too small a value means feed losses rather than latency.

## Evidence
Capacity sensitivity table in LATENCY_BUDGET; `book_capacity_exceeded`.
