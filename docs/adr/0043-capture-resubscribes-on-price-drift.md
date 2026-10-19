# ADR 43: The capture can resubscribe on price drift by itself

Status: Accepted

## Context
The replay's price window is fixed per segment and is not re-centred (ADR 17). `hft_live` already asks the capture for a fresh snapshot, through `--resnap-file`, when the price leaves the window. A capture run alone (a file for later replay, as in Phases 33-35) has no such reader, so after the first window exit nothing was usable. On the 10-minute sandbox capture that was 3,203 of 3,870 messages (ADR 42, SANDBOX.md).

## Decision
- `capture_l2.py --resnap-drift-usd N` follows the best bid and ask of the stream in a small Python book (`DriftWatch`) and resubscribes, with the same `resnap` marker as the file request, when either has moved more than N USD from the mid of the last snapshot.
- Off by default. When on, 200 is a reasonable value: the replay's default grid (65,536 ticks of 0.01, 10% margin) ends a segment at about 262 USD from the snapshot mid. The tool does not read the replay's settings; a caller who changes the grid must change N.
- A drift resubscribe is counted separately (`drift_resnaps`), reconnects at once with no back-off and does not count towards giving up, like the file request.
- Messages that are not a snapshot or an l2update are ignored; unparseable input is ignored; a one-sided snapshot gives no anchor.

## Consequences
A jump larger than the limit in a single message cannot be pre-empted. On the real 10-minute capture the first message past the limit (the best ask moving about 320 USD from the mid) is the same message that ends the replay segment, at 11.5 s. The watch therefore does not prevent the exit; it shortens what follows it from the rest of the capture (583 s) to one reconnect. The messages between the jump and the new snapshot are still dropped by the replay ("before a snapshot"), and the exit message is in the old segment.
The Python book is a second implementation of the best-price rule; it is checked by tests and one real capture offline, not by a run against the sandbox with the option on (not done).

## Rejected
- Reading the replay window from the C++ side into the capture: couples a recording script to a replay setting.
- A periodic resubscribe (every N seconds): resubscribes when nothing is wrong, and misses a jump between ticks.
- Resubscribing at the replay's exit line (262 USD): a limit below it costs nothing extra on a jump and covers slow drift earlier.

## Measured (addendum, same day)
The first real run (SANDBOX.md) showed the rule is wrong for this book. 135 resubscribes in 600 s, 106 of them from a snapshot whose own half-spread was above the 200 USD limit: the watch measures drift from the snapshot's mid, so a snapshot with a wide touch fires on its first update, and the same wide snapshot returns. The watch works as designed on the unit tests and fake server; it does not suit a snapshot that is wider than its limit. Candidate changes, not made here: measure drift from the snapshot's own best bid and ask instead of its mid; skip or delay a resubscribe when the snapshot is already wider than the limit; a minimum interval between resubscribes. The replay has the same weakness (78 of 136 snapshots are wider than its exit line).
