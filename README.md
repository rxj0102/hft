# hft

A tick-to-trade engine in C++20, built in phase gates with measurement at every step: ITCH-style market data in, order book, a market-making strategy, pre-trade risk, an OMS and an order-entry gateway, with a matching-engine exchange simulator and a queue-aware backtester around it.

It runs against a synthetic exchange. There is no live connectivity, no NIC driver and no deployable trading binary. Read "Limits" before quoting any number.

## Measured latency

Packet bytes in to reaction frames staged, hot caches, shared 2-vCPU VM at 2.8 GHz, synthetic market (7233 of 311k packets caused an order):

| | p50 | p99 | p99.9 |
|---|---|---|---|
| tick-to-trade | 128-160 ns | 230-380 ns | 450-560 ns |
| tick-to-decision (no order) | about 72 ns | about 150 ns | 370 ns |
| tick-to-trade, cold L1/L2 | about 0.97 us | 2.1-2.9 us | |

The range is run to run on the same VM, hours apart (128 ns early in the session, about 160 ns later, both gcc builds equal). A shorter 100,000-request run gave 248 ns because it has only 2414 order ticks and a cold start; use 300,000 requests or more. Per-stage budget, cache analysis and method are in [docs/LATENCY_BUDGET.md](docs/LATENCY_BUDGET.md). Cache and branch figures come from a simulator because the sandbox has no hardware counters, and are labelled that way.

## Real data

`hft_replay` runs recorded Coinbase BTC-USD level-2 data through the same pipeline and checks the book against an independent model and against the exchange's own snapshot (20/20, 18/20 and 20/20 identical levels on three live captures). Per feed message the engine costs about 60 ns on this VM; a real packet carries about 14 messages, so per-packet time is about 0.5 us at p50 and 3-4 us at p99. This is offline replay and the paper venue never fills. See [docs/LIVE_FEED.md](docs/LIVE_FEED.md).

`hft_live` runs the same pipeline on the live public feed in real time against that paper venue (`capture_l2.py --out - | hft_live`): it pulls every quote when the connection drops, the price leaves the grid or the feed goes silent for 3 s, checks the book against the independent model as it runs, and on Ctrl-C or SIGTERM latches the kill switch and cancels everything. Fed the same lines, it puts the same bytes on the wire as the offline replay. At real market spacing, one update message costs about 60-65 us end to end in this tool (JSON parsing included), against about 17-18 us when the same messages are replayed back to back. No order leaves the machine. See [docs/LIVE_PAPER.md](docs/LIVE_PAPER.md).

`scripts/supervise.py` keeps the capture and `hft_live` running (restart with backoff, crash-loop limit, a watchdog on the metrics file, SIGTERM as the kill path, no restart after an integrity verdict), and `hft_live` writes a JSON event log and Prometheus metrics. `scripts/drill.py` injects eleven faults (connection drop, silence, kill -9 of either process, a hang, SIGTERM, a crash loop, an integrity exit and others) against a local fake feed and checks the outcome. See [docs/OPERATIONS.md](docs/OPERATIONS.md). Everything runs against the paper venue; what a real exchange does with orders left working after a kill -9 is not shown.

## What is in the box

- Hot path with no heap, exceptions, RTTI, virtual calls, locks or syscalls (compiled with `-fno-exceptions -fno-rtti`; allocation-free by test; page-fault-free by bench check).
- Byte-exact replay oracle: a backtest is recorded and replayed through the production pipeline; the wire bytes must match before any latency number is printed.
- Order book, A/B feed arbitration with gap handling and snapshot recovery, OMS with an eight-state machine, pre-trade risk with a latched kill switch, length-prefixed order-entry protocol.
- Matching engine and exchange simulator, backtester with queue-position fills validated against the engine.
- Six libFuzzer targets, mutation checks at every phase, ASan/UBSan/TSan, gcc and clang, a callgrind instruction-count regression gate.

## Build

```
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release --output-on-failure -j2
./build/release/bench/bench_t2t --requests 300000
```

Presets: `release`, `relwithdebinfo`, `perf`, `asan`, `ubsan`, `tsan`, `fuzz`. Requires CMake 3.25+, gcc 13 or clang 18. `test_sim_gate` takes about 4 minutes.

## Documentation

| | |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | as-built architecture and module map |
| [docs/DESIGN.md](docs/DESIGN.md) | phase-by-phase design log with measurements |
| [docs/DECISIONS.md](docs/DECISIONS.md) | decision records, including rejected options |
| [docs/LIVE_FEED.md](docs/LIVE_FEED.md) | capture, convert and replay real market data; what is proven and what is not |
| [docs/LIVE_PAPER.md](docs/LIVE_PAPER.md) | the live paper session: guards, drills, real-feed runs, limits |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | logs, metrics, supervisor, fault drills, what they do not show |
| [docs/LATENCY_BUDGET.md](docs/LATENCY_BUDGET.md) | measured budget, method, noise, how to reproduce |
| [docs/FAILURE_MODES.md](docs/FAILURE_MODES.md) | what fails, what the engine does, what proves it |
| [docs/RUNBOOK.md](docs/RUNBOOK.md) | startup checks, alarms, kill switch, tooling triage |
| [CHANGELOG.md](CHANGELOG.md) | per-phase changes |

## Limits

- Synthetic data only. The mechanics are tested; the returns and the market are not real.
- Timings come from a shared VM with hypervisor stalls: p99.9 and max are mostly noise from the machine. Bare-metal confirmation has not been done.
- The measured path excludes the NIC, kernel and exchange.
- Not built: FIX 4.4 parser, AVX2, MSan, AF_XDP/DPDK ingress, GoogleTest/vcpkg wiring, multi-instrument operation.
- Real-data replay is offline and level 2 only (one product, one venue); there is no live trading path. Recorded exchange data is not committed.
- The GitHub Actions workflow has not been seen running on GitHub from the development sandbox; its commands were run locally.
- No automatic flatten after a kill; that is a human decision (see the runbook).
