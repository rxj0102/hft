# ADR 21: The live session reuses the replay path, and is checked against it

Status: Accepted

## Context
A live paper mode could have been a second implementation of "feed to pipeline" tuned for streaming. Two implementations drift, and the live one cannot be checked against anything.

## Decision
`feed::LiveSession` uses the same `L2Converter`, `ItchPublisher`, `Pipeline`, `AckVenue`, wire hash and model comparison as `replay_segment`. Time is an argument (`now`) of every call; the session never reads a clock. The only difference in behaviour is how a segment ends (see ADR 22).

## Consequences
Equivalence is a test, not a hope: for 10 random captures (including damaged lines, three segments each, ack delays from 0 to 50 us) the live session puts the same bytes on the wire as `replay_segment` up to the last packet, builds the same book and makes the same decisions. The same recorded file gave the same orders through `hft_replay`, `hft_live --from` and a live pipe. Cost: the live path inherits the replay path's limits (level 2, fixed window per segment, a paper venue).

## Evidence
`test_live` (equivalence on 10 captures), the real-data runs in LIVE_PAPER.md.
