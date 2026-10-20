# ADR 45: Several exchange snapshots for the verify check

Status: Accepted (extends ADR 18, 41, 42)

## Context
The exchange-snapshot check takes one snapshot near the end of a capture. On the real sandbox about half the snapshots are wider than the replay's price window can use (33 of 54 in the third 600 s run, 78 of 136 in the second; half-spread above the 262 USD exit line). With one snapshot the check compared nothing in two of three real runs ("0 of 14" stale, then "0 of 0" with the touch outside the window) and four levels in the third, by luck: that snapshot's spread was 60 USD.

## Decision
- `capture_l2.py --verify --verify-count N` (default 1) fetches N snapshots, each on its own connection, about 2 s apart, the first started so that the last arrives about 6 s before the end of the capture (never before half way). The lines go to `<out>.verify`, one `<t_ns>\t<raw>` each, written once at the end and only if at least one was fetched. A failed fetch loses only itself.
- `hft_replay --verify` reads every line with a tab (others are skipped; none left gives "cannot read"), checks each against the book as of its time, as before (ADR 41, 42), and with more than one prints a header per snapshot ("exchange snapshot i of N, taken t s into the capture") and a summary: how many compared at least one level, had none inside the window, were not comparable (segment ended), had no usable book; and the levels identical, differing and absent summed over the compared ones, with the count outside the window. A file with one line prints exactly as before.
- Exit codes do not change: the check stays informational.

## Consequences
With N around 5 a capture has a real chance of at least one narrow snapshot even when most are wide. It does not make a wide snapshot comparable, and the sum is over a handful of moments, not a statistic. Each extra snapshot is one more public connection to the sandbox. Each check reloads the capture file, so the cost grows with N times the file size (fine for these sizes).

## Rejected
- Choosing the narrowest snapshot in the capture tool: it would hide how many were wide, which is a finding about the sandbox.
- A wider replay window for the check: ADR 41.
