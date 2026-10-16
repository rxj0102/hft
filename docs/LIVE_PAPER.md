# Live paper mode

What this is: the production pipeline running on a live public market-data feed in real time, against a simulated venue, with the guards a live process needs. What it is not: order entry to any exchange. No order leaves this repository, no API key is read, and the venue never fills.

```
python3 scripts/capture_l2.py --out - --seconds 600 --resnap-file /tmp/resnap | ./build/release/hft_live --resnap-file /tmp/resnap
```

The capture script (Python: TLS, WebSocket, reconnect) writes one line per feed message to the pipe. `hft_live` (C++) reads them on its own monotonic clock and runs the same converter, pipeline and paper venue as the offline replay (`docs/LIVE_FEED.md`). Press Ctrl-C or send SIGTERM to stop it.

## What the live session adds over the replay

Replay runs a finished file. A live process has to decide, with no one watching, what to do when the feed stops being trustworthy. `feed::LiveSession` (`include/hft/feed/live.hpp`) takes one call per input line and one per quiet moment, with the time as an argument, and handles:

| Situation | Response |
|---|---|
| connection drops, idle timeout, error (a marker line from the capture script) | flatten, wait for the next snapshot |
| a new snapshot on a running segment | flatten the old segment, start a new one |
| the price nears the edge of the 655-wide grid | flatten, create the resubscribe file; the capture script reconnects and the new snapshot is centred on the market |
| nothing at all for `--stale-ms` (default 3000) | flatten, create the resubscribe file. Heartbeats count as life |
| the book disagrees with the independent model (checked every 100 packets and at the end of every segment) | latch the kill switch, flatten, halt |
| cannot get flat within 64 rounds | latch the kill switch, halt |
| SIGINT or SIGTERM | latch the kill switch, flatten, print the summary, exit |
| input ends | flatten (no kill switch), print the summary, exit |

"Flatten" is one routine for all of them (ADR 22): cancel everything cancellable, answer the cancels, repeat until the OMS and the paper venue both show nothing working. An order that is still PendingNew cannot be cancelled until its ack comes back, which is why it takes rounds. The strategy does not get to react to the reports during a flatten, or it would quote again.

Quoting restarts only at the next snapshot. A reconnect, a stale feed and a window exit are routine and do not latch the kill switch (ADR 10); a failed model check and a kill do.

## What proves it

1. **Equivalence with the replay.** Fed the same lines with the same stamps, the live session puts the same bytes on the wire as `replay_segment` (FNV hash up to the last packet), builds the same book and takes the same decisions. This is tested on 10 random captures with damaged lines, three segments each and acknowledgement delays of 0, 150 ns and 50 us (`test_live`). On real data, the same 60 s capture gave the same 2 new orders and 1,568 replaces through `hft_replay`, `hft_live --from file` and a live pipe.
2. **Drills with a stated expected outcome** (`test_live`), each with the clock supplied by the test so it is exactly reproducible: orders still in flight when the connection drops; silence one nanosecond short of the limit, and at it; a heartbeat every second for 20 s keeping the feed alive; the next line arriving after the silence; kill with acks long back; kill versus shutdown (only the kill latches the switch); price leaving the window; a second snapshot on a running segment; an injected model error (`debug_forget_best_bid`) found by the periodic check and, with the periodic check off, by the end-of-segment comparison; a forced flatten failure; a snapshot of 6,000 levels and an update of 400 changes (many packets per message: the comparison with the model must happen between messages, not inside one).
3. **The process, as a process** (`scripts/test_live_tool.py`): files and pipes, SIGTERM, EOF, silence on an open pipe measured on the real clock, heartbeats on a real clock, the window exit writing the resubscribe file, exit codes. The test fails against a stub binary.
4. **Capture script** (`scripts/test_capture.py`, fake server): `--out -` streams whole lines and exits quietly when the reader goes away; the resubscribe file produces a new connection and snapshot without counting towards the reconnect limit.
5. **Fuzzing** (`fuzz_live`): arbitrary text plus control lines that jump the clock, kill and shut down. Oracles: every segment ends flat, the model always matches, no desync or book error, cancels never exceed accepted orders, nothing is left working between segments, and the same input twice gives the same order flow. About 48,000 clean runs over three sessions. A first fuzz run of 100 s (16,000 runs) did NOT find the mid-message comparison bug when it was put back in on purpose: random mutation does not build a JSON update with dozens of changes quickly. A seed with an 800-level snapshot and a 150-change update (`fuzz/seeds/live/seed4`, invented prices) makes it fail at once, so that seed is part of the target.
6. **Mutation check**: injected bugs in the live session and the tool, each of which must make a test fail. Results are at the end of this file.

A real bug was found by running it, not by the tests that existed: the first version compared the book with the model every 100 packets wherever that landed, but a feed message can span several packets (a busy update of 40-odd changes is about three), so on real data the comparison soon landed inside a message, the half-applied book disagreed with the model, a healthy book was called an integrity failure after 300 packets and the session killed itself. The comparison now happens between feed messages, and a test with a 6,000-level snapshot and a 400-change update covers it. (The tests' random captures have small messages, and none of them hit it.) A second bug was found by a test written for a mutant: the kill-switch flag in the segment summary was read before the kill that a failed flatten triggers.

## Results on the real feed

BTC-USD, Coinbase production public feed, through the sandbox's network, gcc release build on a shared 2-vCPU VM.

| Run | Result |
|---|---|
| 90 s through a pipe | 1,543 messages, 68,520 engine messages applied, one segment of 2,135 packets, 21 comparisons with the model and 0 failures, final book matches, 2 new orders + 2,240 replaces, then 2 cancels at the end, 0 risk rejects, 0 desyncs, 0 book errors |
| 60 s through a pipe, saved with `tee` | the same file replayed with `hft_live --from` gave the same 2 new + 1,568 replace; `hft_replay` saw 1,570 orders |
| 25 s then SIGTERM | stopped mid-run: 2 cancels by the kill sweep, segment flat, kill switch on, book matched, exit 0; the capture script saw the pipe close and exited without an error |

Time per feed message is the first thing a live mode can measure that replay cannot: it includes JSON parsing, conversion, the whole pipeline, and arrival at real market spacing. Wall-clock per update message, one process, steady clock (about 20-30 ns of timer overhead included), same 60 s capture:

| Messages replayed with this much time between them | p50 | p99 |
|---|---|---|
| none (back to back) | 18.4 us | 103 us |
| 0.06 ms | 16.6 us | 78 us |
| 0.3 ms | 19.7 us | 104 us |
| 3 ms | 43.5 us | 184 us |
| 14 ms | 56.8 us | 178 us |
| as recorded (about 57 ms), spinning between messages | 63.5 us | 201 us |
| as recorded, sleeping between messages | 66.6 us | 315 us |
| live pipe (two runs) | 64.5 and 61.4 us | 221 and 250 us |

So the number depends strongly on how the messages arrive. Back to back is about 3.5 times faster than at real spacing, and spinning instead of sleeping did not change it. The cause is not isolated: the sandbox has no hardware performance counters, and the pattern fits state (caches, branch predictors) decaying between messages, but a shared VM could also be the reason. The pipeline itself costs about 1.3 us per packet in the replay hot loop on the same data, so most of the 17 us even back to back is JSON parsing and conversion, which are plain unoptimised code here. This is why the offline hot-loop figures (120-160 ns tick-to-trade on synthetic data) are not a statement about a live system.

Starting a segment takes 108-127 ms on this VM, from the snapshot line to the pipeline being ready (converting and applying about 500 in-window levels, and allocating and initialising a book for 65,536 grid levels and 131,072 orders; not broken down further). No quotes are out during that time, which is the price of every reconnect and window exit.

## Limits

- It is a paper venue. Its 50 us round trip is an assumption; nothing is ever filled, so there is no position, no PnL and no fill-driven behaviour.
- Level 2 only, one product, one venue. The grid window is 655 wide and fixed per segment. A window exit costs a reconnect (about 120 ms to quote again plus the connection time).
- The process has two parts and the pipe between them is a failure point. If the Python side dies, `hft_live` sees EOF and shuts down flat. If `hft_live` dies, the Python side sees the closed pipe and exits. Neither restarts the other; there is no supervisor yet (that is an operations question, not built).
- Timing excludes the network and TLS (they happen in Python), and the Python side costs more than the engine.
- The stale limit (3 s) and the model-check interval (100 packets) are chosen, not tuned.
- Two of the guard checks in `end_segment` are redundant with each other in a closed system with this venue (the OMS has no working order, the venue has no live order); they are kept as a cross-check against a venue that disagrees with the OMS and are not exercised by any test.
- Real-clock behaviour was observed over runs of 25 to 90 seconds, not hours.

## Running it

```
python3 -m pip install websockets
cmake --preset release && cmake --build --preset release
python3 scripts/capture_l2.py --out - --seconds 600 --resnap-file /tmp/hft.resnap | ./build/release/hft_live --resnap-file /tmp/hft.resnap
./build/release/hft_live --from data/btc.l2            # a recording, through the same code, with its own stamps as the clock
./build/release/hft_live --from data/btc.l2 --pace 1   # the same, at the recorded speed
```

Options: `--stale-ms`, `--rtt-us`, `--lots`, `--levels`, `--max-orders`, `--check-every`, `--report-s`, `--max-seconds`, `--resnap-file`, `--spin`. Exit codes: 0 clean, 1 usage or I/O, 4 integrity failure (the book differed from the model, a flatten failed, a desync or book error), 5 no segment ever started. A status line goes to stderr every `--report-s` seconds.

## Mutation check

50 injected bugs in `live.hpp` and `hft_live.cpp` (stale boundary and refresh, markers, snapshot handling, flatten rounds and clocks, kill and shutdown semantics, integrity and final comparison, window exit, clock regression, input validation, timers, summaries and totals, the tool's signal, EOF, polling and resubscribe handling). Each must make `test_live` or `scripts/test_live_tool.py` fail. 47 are caught.

The first run caught 33 of 49 and flagged 10 survivors; five mutants did not compile and were rewritten. Working through the survivors:
- Gaps in the tests, now closed: a flatten after a long quiet period used the venue's stale clock (stale and kill paths); a time step backwards was clamped but nothing checked the deadline built on it; a model/book disagreement found only at the end of a segment was not shown to halt the session; the level-by-level end comparison was shadowed by the level count until a test changed a size without changing the count.
- Code no test could reach, removed rather than excused: a call into the feed handler's timers inside `poll`, and a loop in the tool that drove recorded-file timers (the next line's own stale check does the same thing).
- Three survivors left, all redundant checks rather than test gaps: "OMS shows no working order" and "venue shows no live order" each imply the other with this venue, and are kept as a cross-check for a venue that disagrees with the OMS; the explicit halt after a failed periodic check is also made by the end-of-segment comparison that follows it. A test that makes them differ would need a venue that lies, which does not exist here.
