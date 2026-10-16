# ADR 23: Silence is a feed loss, detected on the process's own clock

Status: Accepted

## Context
The level-2 feed has no sequence numbers, so the usual way of noticing a gap does not exist. A dead connection can also stay open for a long time before the socket reports it. A quote resting on a book that stopped updating is the most expensive kind of wrong.

## Decision
The session records the time of the last feed line of any kind (a heartbeat counts: the capture subscribes to the heartbeat channel, which proves the connection is alive). If `stale_ns` (default 3 s) passes with nothing, the session flattens and asks the capture process to resubscribe. `poll(now)` is how time passes with no input; the tool calls it every millisecond on a pipe, and on a recorded file it is driven by the recorded stamps (`next_deadline`), so the same drill is reproducible offline.

## Consequences
A quiet market with heartbeats off would be treated as dead. The default relies on the heartbeat channel. The threshold is a parameter with no tuning behind it; 3 s is a guess that is long compared with the feed's 50 ms batch interval and short compared with the capture's 10 s idle timeout.

## Evidence
`test_live` (boundary, heartbeats, found by the next line), `scripts/test_live_tool.py` (real-time silence on an open pipe).
