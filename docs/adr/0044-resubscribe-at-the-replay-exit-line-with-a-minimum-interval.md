# ADR 44: Resubscribe at the replay's own exit line, and no faster than once a second

Status: Accepted (amends ADR 43)

## Context
The first real run of `--resnap-drift-usd 200` (ADR 43 addendum, SANDBOX.md) made 135 resubscribes in 600 s. Two things were wrong. The limit was below the replay's exit line, and the sandbox sends a snapshot whose touch is wider than the replay's exit line about half the time (78 of 136 snapshots, half-spread above 262 USD), so the watch asked again at once and the capture reconnected as fast as it could.
Three rules were compared offline on the 136 segments of that capture (each segment is a snapshot and the updates that followed it). The replay's own exit (best bid below mid - 262.14 USD or best ask at or above mid + 262.14, from the window of 65,536 ticks and the 10% margin) fires in 103 segments, 81 of them within a second of the snapshot. The 200 USD mid rule fires in 135, so in at least 32 segments it cut a segment the replay could have kept. A rule measured from the snapshot's own touch (the idea in the ADR 43 addendum) fires in 28 segments and in 78 never fires although the replay's exit does: those are the wide snapshots, where the replay's segment ends at the first update. It would leave the capture dead after exactly those.

## Decision
- The recommended limit is the replay's exit, 262 USD (`--resnap-drift-usd 262`), not 200. The option stays a number the caller sets; the script still does not read the replay's settings.
- New `--resnap-min-s S` (default 1): at most one drift resubscribe per S seconds. The first request is not held back. While held back the stream continues into the old segment, which the replay has already dropped.
- The touch-based rule is rejected, on the offline numbers above.

## Consequences
Fewer reconnects (at most one per second), and no segment cut that the replay could have kept. The price: after a wide snapshot the capture can wait up to a second for the next one, and about half the snapshots are wide, so some dead time remains (expected, not measured: the time to a usable snapshot is a second or two). The offline comparison uses segments that the 200 USD run itself produced, so it says what each rule would do with those segments, not what a new run would give.
The Python rule is an approximation of the C++ exit (float mid, the boundary within a tick); the number 262.14 is copied from the default grid and is not read from the code.

## Rejected
- A rule measured from the snapshot's touch: see Context.
- Skipping the resubscribe when the snapshot is wider than the limit: that is the case the replay cannot use, and the only fix is another snapshot.
- Reading the window from the C++ side: as in ADR 43.

## Measured (addendum, same day)
One run with the new settings (SANDBOX.md): 53 resubscribes in 600 s (135 before), none under a second apart; 12 segments of 100 or more packets cover 539.4 s (520.9 s before); the exchange check compared 4 levels and all 4 matched. 33 of 54 snapshots were still wider than the exit line. Two runs at different times are not a controlled comparison.
