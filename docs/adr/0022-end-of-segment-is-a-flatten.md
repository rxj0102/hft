# ADR 22: Every end of trust is the same flatten, and the strategy does not get a vote

Status: Accepted

## Context
A live feed stops being trustworthy in several ways: the connection drops, a new snapshot replaces the book, the price leaves the grid, the feed goes silent without the socket noticing, the book disagrees with the independent model, an operator kills the process. Handling each separately invites a path that forgets to cancel.

## Decision
All of them call one routine, `end_segment`: cancel everything cancellable, answer the cancels from the paper venue, repeat until neither the OMS nor the venue has a working order. Orders still PendingNew cannot be cancelled until their ack returns, hence the rounds. The reports during a flatten go to the gateway only (`on_bytes`), not through `Pipeline::on_report`, because that would let the strategy quote again. If it cannot get flat within `max_flatten_rounds` it latches the kill switch and halts. Quoting restarts only at the next snapshot.

Two of the endings are stronger than a pull: a failed model comparison and `kill()` latch the risk engine's kill switch and halt the session, so a process that has lost its book, or been told to stop, cannot send another order. A reconnect, a stale feed and a window exit do not latch it (ADR 10: losing data is routine).

## Consequences
One place to test and to fuzz. The flatten runs on the paper venue's simulated clock, so it takes no wall time; with a real venue it would take at least a round trip per round, and the same loop would need real waiting. Cost: a window exit ends quoting until the capture process resubscribes (it is asked to, through a file).

## Evidence
`test_live` drills for each ending (orders in flight, stale, kill, window, snapshot, integrity, forced flatten failure); `fuzz_live` oracle (every segment ends flat).
