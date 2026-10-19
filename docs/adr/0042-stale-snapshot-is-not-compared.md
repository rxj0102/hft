# ADR 42: A snapshot taken long after the segment ended is not compared

Status: Accepted

## Context
`hft_replay --verify` rebuilds the book at the exchange snapshot's time (ADR 18, 41). A segment ends when the best price comes within 10% of the window edge, and nothing re-centres it (ADR 17). On the 10-minute capture of the real sandbox (2026-10-19, Phase 35) the first segment ended 11.1 s in, when the best ask jumped about 450 USD on a thin book. The exchange snapshot was taken at 594.5 s. The tool compared the book at 11.1 s with the exchange book at 594.5 s and printed "0 of 14 levels identical, 14 absent". That is a true count of a meaningless comparison, and the sandbox's own price had moved by hundreds of USD in between.

## Decision
- `verify_against_snapshot` takes the snapshot time. If the segment ended by leaving the window and the snapshot was taken more than `kVerifyMaxGapNs` (2 s) after the segment's last packet, it returns `stale` with the gap and compares nothing.
- A segment that did not end by the window is always compared, however late the snapshot: its book is current to the end of the capture.
- `hft_replay` prints "not comparable. The replay segment ended N s before the exchange snapshot was taken (the price left the window)". The exit code is unchanged.
- 2 s is a judgement taken from earlier notes (a snapshot 1-2 s after the end already lost 4 of 20 levels on the production feed, LIVE_FEED.md). It is not tuned on data.

## Consequences
A capture that leaves the window early can no longer produce a verify result, true or false. The check then says nothing about the book, which is the honest state. A supervised capture that re-subscribes on a window exit (not measured on the sandbox) would restore the comparison.

## Rejected
- Printing the old counts with a warning: the numbers would still be read.
- Verifying against a snapshot taken at the segment's end: the second connection would need to run at that moment, which the capture tool cannot know in advance.
