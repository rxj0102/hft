# Case study: a tick-to-trade engine, built to be checked

This is a five-minute read. Every number here comes from [RESULTS.md](RESULTS.md) or [REPRODUCIBILITY.md](REPRODUCIBILITY.md), where the command, the machine and the caveats are written down.

## What it is

A C++20 engine that takes market data in and puts an order frame out: feed handler, order book, market maker, pre-trade risk, order management and an order-entry protocol, all in one thread with no heap, exceptions, virtual calls, locks or syscalls on the path. Around it sit an exchange simulator and backtester, a strategy lab, a live paper session on the public Coinbase feed, a supervisor, and a bridge to the Coinbase Exchange sandbox.

It was built in phase gates. Each phase had to end with a measurement or a test, and each claim had to be one I could reproduce on demand.

## The numbers, and what they exclude

On a shared 2-vCPU VM at 2.1 GHz, one core pinned, the median from packet bytes in to order frame staged is 115 to 127 ns with hot caches, and 71 ns when no order results. Cold L1 and L2 it is about 1.0 to 1.1 us. Sustained throughput is 15.5 to 16.9 million inputs per second.

These exclude the NIC, the kernel, TLS, JSON parsing and the exchange. The tail (p99.9 and max) is mostly hypervisor noise, so CI gates on an instruction count from callgrind instead of wall-clock time.

## Five things worth telling

**1. Every latency number sits behind a byte-exact check.** A backtest is recorded, then replayed through the production pipeline, and the wire bytes must be identical before the benchmark prints a single latency (ADR 4). An optimisation that quietly changes behaviour cannot produce a faster number. The cost is that the oracle only covers recorded sessions, which is why fuzzers and tests cover the rest.

**2. A mismatch in a replay turned out to be a real bug.** A run stopped by SIGTERM printed a different order-flow hash from the replay of its own recording. The cause was that the kill switch sends a cancel sweep, and those cancel frames were counted in a hash meant to describe only what the feed caused. A unit test reproduced it (3 checks failed on the old code) and the fix is in ADR 47.

**3. Part of an earlier claim did not survive, and it is withdrawn in writing.** I had said a run that ends by itself replays to its live hash. Over 8 recorded runs, only 1 matched. The reason is that live mode uses the arrival time of each line as its clock, so timing can change decisions. The fix was to record the clock (ADRs 48 to 52). With it, 23 of 23 runs of a 2-hour session and 5 of 5 of a 30-minute one replay to the same hash and summary. The first clock format was about 14 times the size of the stream (91 MB against 6.7 MB over 2 hours); after recording only the polls that can act, a 30-minute run had clocks of 159 KB against 1.5 MB of stream. Separately, runs I had described as sandbox runs had used the production public market-data feed by default (read-only, no keys, no orders), and I corrected the record.

**4. Tests were tested.** At every phase I injected bugs on purpose and required a test to fail. Survivors were either killed with a new test or removed as unobservable code. For example, the first pass on the live clock had 7 survivors out of 25 mutants, and the strategy lab round had 24 survivors out of 95. The tables are in RESULTS.md. One survivor, `--max-orders`, cannot be shown to matter by any stream, and the docs say so rather than hiding it.

**5. A negative result is kept.** A position-dependent quote size was tried, judged by the lab on held-out markets against criteria written down before the first run, and rejected (ADR 39). It did not reduce the maximum position (39 lots against 39 without it). The patch and outputs stay in the repo as an experiment.

## How it is checked

ASan, UBSan and TSan builds with gcc and clang, 19 CTest suites, 9 libFuzzer targets (one where the fuzz input plays the exchange), 31 drills that inject faults into a fake feed and a fake exchange, and GitHub Actions with 9 jobs. CI found faults that local runs had missed, such as a missing Python package in five build cells and a race in a test (see [CI.md](CI.md)). There are 52 decision records, including rejected options.

## What I would not claim

- **The signed sandbox side has never run.** I had a key ID but no secret or passphrase. The bridge, the closed loop, closing orders, the account check and PnL on real fills have only run against a local fake exchange written from the public API documentation. The first real run will find differences.
- **No claim about profitability.** The market maker exists to exercise the engine. The strategy lab compares variants only on simulated fills in generated markets, and the paper venue never fills.
- **No bare-metal result.** The timings are from a shared VM, with one product and one venue, and no kernel bypass.
- **Replay evidence is thin in places.** The strongest set of streams was piped at file speed, not received in real time, and a process killed with SIGKILL leaves nothing to compare against. The full list is in REPRODUCIBILITY.md under Not covered.

## Where to look next

[README.md](../README.md) for the build and the map of the docs, [ARCHITECTURE.md](ARCHITECTURE.md) for the data flow, [DECISIONS.md](DECISIONS.md) for the decision records, and [REPRODUCIBILITY.md](REPRODUCIBILITY.md) for the replay evidence.
