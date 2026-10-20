# ADR 47: The kill sweep is not part of the order flow the feed caused

Status: Accepted

## Context
`hft_live` prints "wire hash of the order flow the feed caused (flatten excluded)". Phase 41 found that a run stopped by SIGTERM printed a hash (bf84af5b3079820a) that differed from the replay of its own recording (7190b960b701952c), while runs that ended by themselves matched.

Cause: a normal end (end of input, `--max-seconds`) leaves the cancels to the flatten step, which the hash excludes. A SIGTERM, a stop request or `kill()` first sends the kill switch's cancel sweep through the same gateway, and those cancel frames were counted in `hash_before_flatten` and `frames_before_flatten`. The replay of the same input ends normally, so it does not contain them. A unit test reproduced it before the fix (3 checks failed): two sessions fed the same input, one ended by shutdown and one by kill, gave different hashes.

## Decision
- `Live::kill()` records the segment's hash and frame count before it latches the switch, unless the segment is already ending (it has already taken its numbers). `end_segment` reports those numbers.
- The kill sweep's frames still count in the totals (`frames_total`, order counters, `by kill`), because they are real orders; only the "caused by the feed" hash and its frame count leave them out.

## Consequences
- A run stopped by SIGTERM now replays to its live hash when its recording is cut to the `input: N lines` in its summary (the line the process was reading when it stopped may not be in its summary).
- Hashes printed by older builds for killed runs include the sweep and are not comparable.
- A drill (`record_term`) stops a recorded supervised run with SIGTERM and checks the cut recording replays to the live hash; with the fix reverted it fails.
- Unchanged: the hot path. The change is in the stop path only.
