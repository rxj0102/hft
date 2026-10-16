# ADR 5: Count parked packets instead of scanning the pending store

Status: Accepted

## Context
The pending store is 64 slots of about 1.5 KB. Scanning it for every in-order packet touched 64 cache lines and was the largest cost on the tick.

## Decision
`MdHandler` keeps `parked_`. The drain runs only when `parked_ != 0 || gap_active_`.

## Consequences
t2t hot p50 240 -> 163 ns, idle p50 164 -> 97 ns. The second condition is required: a heartbeat ahead of our sequence opens a gap with nothing parked, and the late in-order packet closes it. I asserted the opposite at first; the fuzzer disproved it and `test_heartbeat_gap_closed_by_late_packet` now pins it.

## Evidence
Interleaved A/B, median of 5; mutation check (18 of 18 caught).
