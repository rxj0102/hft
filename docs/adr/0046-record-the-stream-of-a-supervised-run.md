# ADR 46: Record the stream of a supervised run, and replay it to check the order flow

Status: Accepted (its hash-equality expectation is limited by ADR 48)

## Context
`hft_live` prints a hash of the order flow the feed caused. A recorded stream replayed through `hft_live --from` should give the same hash as the live run, which checks that the live path is deterministic given its input. The supervised sessions on the sandbox (Phase 40) did not save the stream, so that check could not be made on them.

## Decision
- `capture_l2.py --tee PATH` writes a copy of everything it writes (to a file or to stdout) into PATH, flushed with each message when streaming, and replaces an existing file.
- `supervise.py --record-dir DIR` starts the capture of each run with `--tee DIR/stream.<run>.txt` (the directory is made), and logs the path in `pair_start`. A capture command of your own must accept `--tee`.
- A drill (`record`) checks on a fake feed that a run that ends by itself replays to the live hash.

## Consequences
Only a run that ends by itself (the capture's `--seconds` elapsed, hft_live read to the end of its input) is comparable. A killed process prints no summary. A run stopped by SIGTERM prints one; in Phase 41 it did not reproduce (below), which ADR 47 explains and fixes.
The recording grows with the stream (about 0.5 MB per 10 minutes on the sandbox); like other captures it must not be committed.

## Measured (sandbox, 18 minutes, five runs)
Runs 1, 3 and 4 ended by themselves; each replayed to exactly its live hash (7266456570e0ee3c, 68a7b3e3a6c31f18, 1d4961a55f1ee582). Run 2 (SIGKILL) has no live summary. Run 5 (SIGTERM) printed one (bf84af5b3079820a) that differs from its replay (7190b960b701952c); cutting the recording to the 407 lines hft_live had read changed nothing. The likely cause is that the kill sweep's cancels are in the pre-flatten hash while a normal end puts them in the flatten, which is excluded; this was not verified.

## Rejected
- Reconstructing the stream from the log: the log has summaries, not the messages.
- Recording inside `hft_live`: it would put file output on the live path.
