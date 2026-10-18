# Measured results

Every number on this page was produced by a command listed next to it. Where a number was measured in an earlier phase and not re-run for this page, it says so. Nothing here is an estimate, and nothing here was measured against a real exchange, because the sandbox has never been run (no API key was available during development).

Machine for the runs of 2026-10-17: shared VM, Intel Xeon at 2.10 GHz, 2 vCPUs, Linux 6.18, gcc 13.3, release build (NDEBUG), one core pinned, hugepage hint, mlock and prefault on. A do-nothing spin on this VM lost the CPU for more than 1 us about 2,600 times a second and for up to 700 us once, so p99.9 and max are mostly the machine.

## Tick-to-trade latency

Command: `./build/release/bench/bench_t2t --requests 300000`, run three times on 2026-10-17; the table shows the two runs whose headline lines I kept (the third gave 64.6 ns per input sustained and a cold-cache p50 of 1,020 ns). Packet bytes handed to the pipeline, to reaction frames staged in a buffer. Synthetic session: 310,987 feed packets, 7,233 of which caused an order frame (2.3 percent).

| Path (hot caches, packets replayed back to back) | p50 | p99 | p99.9 |
|---|---|---|---|
| tick to order frame, run 1 | 114.8 ns | 212.9 ns | 464.3 ns |
| tick to order frame, run 2 | 127.1 ns | 270.0 ns | 445.2 ns |
| tick to decision, no order (run 2) | 71.0 ns | 150.0 ns | 292.9 ns |
| tick to order frame, cold L1 and L2 (4 MiB walk before each tick, run 2) | 1,142 ns | 4,144 ns | not meaningful (865 samples) |

Sustained: 59.2 ns per input (16.9 million inputs per second) in run 2 and 64.6 ns (15.5 million) in the third run, no probes in the loop.

Read this with care:

- The spread between runs on the same machine is real and is part of the result. A shorter 100,000-request run gives a worse p50 because it has fewer order ticks and a cold start.
- Hot caches is the best case. The cold-cache row is what a tick costs when the book is not in L1 or L2 (it stays in L3).
- The measured path excludes the NIC, the kernel network stack, TLS, JSON parsing and the exchange. A real deployment would pay for all of them.
- The machine is a shared VM. These are not bare-metal numbers and no bare-metal run has been done.
- The replay check printed before any latency number: the production pipeline put the backtest's exact bytes on the wire (hash d8420c58f2152785). If that check fails the benchmark prints no latency.
- Per-stage budget, the cache analysis (from a simulator, because the VM has no hardware counters) and the method are in [LATENCY_BUDGET.md](LATENCY_BUDGET.md).

## Instruction-count regression gate

Command: `python3 scripts/perf_gate.py --bench build/perf/bench/bench_t2t --strict` (callgrind, deterministic). Result at Phase 19, the last run: 797.80 instructions per input, 2.95 mispredicts per input, identical to the stored baseline. No hot-path code has changed since, and the gate runs in CI on every push.

## Strategy variants on simulated fills

Command: `./build/release/hft_lab --markets 16 --requests 200000 --variant noalpha:alpha16=0 --variant wide:half_spread16=32 --sweep skew16=0,4`, run twice, on seeds 1000 to 1015 and 5000 to 5015, 2026-10-17. Tables and reading are in [STRATEGY_LAB.md](STRATEGY_LAB.md). In short: removing the imbalance lean or quoting wider lost about 17 per market on every one of 32 markets; removing the inventory skew gained 7.5 and 8.7 per market but held positions up to the cap of 40 (the base peaked near 12), and most of its gain was inventory. These are simulated fills in generated markets, in the generator's currency units over about 2 s of market time per market; they are not returns, and the report from the same fills through `pnl_report.py` agrees with the backtester's own accounting to rounding (tested on 20 runs).

Latency sweep (`--latency-sweep`, ADR 37), 2026-10-17: 8 markets of 40,000 requests, delay on all three hops. The base kept 61 percent of its simulated PnL at 100 us against 0 us (under 5 percent lost up to 20 us); the no-skew variant's lead over the base shrank from +2.90 to +0.10. Table and reading in [STRATEGY_LAB.md](STRATEGY_LAB.md). Simulated fills, one generator; not a statement about a real venue.

Queue penalty sweep (`--queue-sweep`, ADR 38), 2026-10-17: 8 markets of 40,000 requests. The base kept 45 percent of its simulated PnL at a penalty of 400 percent against 0 (about a fifth lost already at 25 percent); the no-skew variant stayed ahead of the base at every penalty. Table and reading in [STRATEGY_LAB.md](STRATEGY_LAB.md). A stress on one assumption of the simulator, not a measurement of a real queue.

Parameter search with held-out markets (`--grid`, ADR 34), 2026-10-17: grids of 48 and 64 points on 8 training markets, the best 3 re-run on 8 fresh markets. The best points kept 90 to 104 percent of their training gain over the base on the held-out markets, so there was almost no winner's curse in these grids, and every chosen point sat at the edge of the grid (more imbalance lean, tighter quotes, more inventory). Details and the reading are in [STRATEGY_LAB.md](STRATEGY_LAB.md). Same caveats: simulated fills, one generator.

## What the checks add up to

| Check | Result | Command |
|---|---|---|
| C++ unit, property and gate tests | 19 of 19 pass (release) | `ctest --test-dir build/release` |
| Sanitizers | ASan, UBSan and TSan builds pass the suites (CI cells) | presets `asan`, `ubsan`, `tsan` |
| Compilers | gcc 13 and clang 18, release, ASan and UBSan cells | CI matrix |
| Fuzzing | 9 libFuzzer targets, 45 s each in CI; longer local runs were clean after the fixes listed in the changelog | `fuzz-smoke` job |
| Python suites | capture script, live tool, order bridge, sandbox smoke test, PnL report, strategy lab, all against local fakes | `scripts/ci_local.sh` |
| Fault drills | 28 drills against a fake feed and a fake exchange: connection drop, silence, kill -9 of each process, hang, crash loop, exchange down, bad credentials, close with and without liquidity, restart with a carried position, account mismatch | `python3 scripts/drill.py` |
| GitHub Actions | 9 jobs, green on the latest push to master and main at the time of writing | `gh run list` |

The drills are timing-based. One of them failed once in a local run during Phase 21; the cause was a race in the drill itself (it counted orders before the old process had died), it was fixed, and it passed three repeats afterwards.

## Mutation testing

At every phase from the start, bugs were injected into the new code on purpose and a test had to fail. Survivors were either killed with a new test or the code they lived in was removed as unobservable. Counts from the latest phases, from the changelog:

| Phase | What | Mutants | Survived the first run |
|---|---|---|---|
| 19 | position carried across segments | 31 | 7 |
| 20 | account check against the position file | 23 | 3 |
| 21 | PnL report and fill events | 30 | 7 |
| 23 | strategy lab, backtester hooks, report tie order | 133 | 24 of 95 in the first round, 3 of 33 in the second (equivalent) |
| 24 | grid search with held-out markets | 60 | 17 of 60 in the first round (3 equivalent, 7 did not compile and were rewritten) |

A few survivors in each phase were equivalent mutants (the changed code behaves the same) and are said to be so in the changelog.

## Recorded real market data (measured in an earlier phase, not re-run for this page)

Offline replay of Coinbase BTC-USD level-2 captures through the same pipeline, with the book checked against an independent model and against the exchange's own snapshot: 20 of 20, 18 of 20 and 20 of 20 identical levels on three live captures. About 60 ns of engine time per feed message on the VM of that phase. The captures are not in the repository. Details and limits are in [LIVE_FEED.md](LIVE_FEED.md).

## Size

About 21,800 lines of C++ (include, src, tests, bench, fuzz) and about 6,100 lines of Python (scripts), counted with `wc -l` on 2026-10-17. 34 architecture decision records, listed in [DECISIONS.md](DECISIONS.md).

## What has not been measured

- Anything against the real Coinbase sandbox. The order bridge, the closed loop, the closing orders, the account check and the PnL report have only ever run against a local fake exchange written from the public documentation. The first real run is an experiment and will probably find differences.
- Whether the strategy makes money. The only PnL figures are from simulated fills in generated markets; there is no run against real fills. The report exists so that a run can be judged, and it is tested against hand-computed answers.
- Bare-metal latency, NIC and kernel-bypass paths, multi-instrument operation, or any run longer than minutes.
