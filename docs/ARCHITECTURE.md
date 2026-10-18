# Architecture

This is the as-built architecture. The core below (everything up to the gateway) is as released in 0.1.0; since then phases 10 to 21 added the live paper session, the supervisor, the sandbox bridge, the closed loop, closing orders, carried positions, the account check and the PnL report. [architecture.svg](architecture.svg) shows the whole system as it is now and [../README.md](../README.md) summarises the later pieces; the text below describes the single-process core. `DESIGN.md` is the phase-by-phase log with the measurements behind each piece; this document is the map.

## What this is, and what it is not

A tick-to-trade engine in C++20, built as header-only libraries plus tests, benches and fuzzers. Everything runs against a synthetic exchange (and, offline, against recorded real market data): a matching engine and ITCH-style feed publisher in `sim/`, an order-entry wire protocol in `gw/`, and a queue-aware backtester in `bt/`.

It is not a deployable trading binary. There is no NIC driver, no kernel-bypass code and no FIX parser. The live connections added after 0.1.0 (a public feed and a sandbox bridge) are Python and sit outside the measured path; the bridge has never run against the real sandbox. Those are listed under "Not built" below. The measured path starts when packet bytes are handed to the pipeline and ends when the reaction frames are staged in a buffer.

## Data flow

```
 packet bytes (line A or B)
        |
        v
 md::MdHandler        parse, session check, A/B arbitration, sequencing,
        |             gap detection, snapshot recovery
        v  (listener callback per message)
 book::OrderBook      flat price-level array, intrusive order lists, id table
        |
        v
 strat::MarketMaker   imbalance signal, inventory skew, quote logic
        |             (reads the book through book::BookView, orders through the OMS)
        v
 gw::Gateway          risk::RiskEngine -> oms::Oms -> encode -> send callback
        |
        v
 frames staged for the wire

 exchange reports  --> Gateway::on_bytes --> Oms --> (next decide)
```

`t2t::Pipeline<Send>` owns all of it as one object. `Pipeline::decide` runs the strategy after every packet and every report. If the feed handler no longer trusts the book, `decide` pulls the working quotes (`Gateway::cancel_all`) and returns before the strategy runs.

## Threading model

Single thread, run to completion: a packet comes in, the whole reaction happens on the same stack, nothing is handed to another thread. There are no locks, atomics or queues on this path.

`core::SpscRing` exists, is tested and has its own bench, but the pipeline does not use it. The Phase 0 design drew SPSC rings between stages; the measurements said a single-thread pipeline at about 128 ns hot p50 had nothing to gain from stage handoff, which costs more than the stages. A multi-thread split would be a different design with its own measurements.

## Time

Time is an input. Every call that needs "now" takes a `std::uint64_t now` argument. In a backtest or replay it is the recorded logical time; the TSC is read only by the benches, for measurement. This is what makes replay bit-exact.

## Memory

All state is allocated once at startup from `core::Region` (an mmap'd region, optionally hugepage-backed, `mlock`'d and prefaulted) through `core::Arena`. After init the hot path allocates nothing. This is checked by an operator-new trap in the tests (`no_allocation`) and by a hard page-fault check in `bench_t2t` (exit 3).

Sizes that matter for latency, all fixed at init: book price grid (`BookConfig::num_levels`), id table (16 B x a power of two of at least 2 x `max_orders`), MD pending-packet store (`MdConfig::pending_slots`, default 64 slots of about 1.5 KB), OMS order pool (`OmsConfig::max_orders`).

## Modules

| Directory | Contents |
|---|---|
| `core/` | types, fixed-point `Price`, TSC clock, histogram, `Region`/`Arena`, SPSC ring, `InvariantDivisor` |
| `book/` | `OrderBook`, `IdMap`, level bitmap, `BookView`, digest |
| `md/` | ITCH-style wire, encoder, `MdHandler` |
| `sim/` | matching engine, exchange simulator, feed publisher |
| `oms/` | order state machine, position and exposure accounting |
| `risk/` | pre-trade checks, kill switch |
| `gw/` | order-entry wire protocol, `Gateway`, exchange-side edge |
| `strat/` | `MarketMaker` |
| `bt/` | backtester, virtual venue with queue-position fills, synthetic market, metrics, strategy lab (`lab.hpp`, tool `hft_lab`: variants over many markets, paired statistics, held-out parameter search, a maker fee, named market regimes, latency and queue-penalty sweeps, event logs for `pnl_report.py`) |
| `t2t/` | `Pipeline`, `Session` (record/replay), stage probes |
| `feed/` | real-data tooling (not hot path): JSON/decimal parsing, level-2 to ITCH conversion, capture loader, paper venue, replay runner |

## Correctness machinery

- Byte-exact replay oracle. A Backtester run is recorded (what the strategy side saw and sent); replaying the inputs through the real `Pipeline` must reproduce the same wire bytes. Every latency pass is gated on this.
- Invariant checkers (`check_invariants`) on the book, the MD handler and the OMS, called by tests and fuzzers after every step.
- Independent oracles: a slow reference book (`RefBook`), a reference risk engine, a 110-cell transition spec for the OMS written separately from the table, and the real matching engine as the oracle for queue-position fills.
- Six libFuzzer targets: codec, match, md, oms, pipeline, venue.
- Mutation checks at the end of each phase, with every surviving mutant turned into a test.

## Build and CI

CMake presets: `release`, `relwithdebinfo`, `perf`, `asan`, `ubsan`, `tsan`, `fuzz`. Hot-path headers are compiled with `-fno-exceptions -fno-rtti` by `hft_hotpath_check`. `.github/workflows/ci.yml` runs gcc-13 and clang-18 across release/asan/ubsan, tsan, a fuzz smoke and the perf gate. That workflow has not been run on GitHub from the development sandbox; its commands were run locally.

## Real data

A recorded Coinbase level-2 capture can be converted into the engine's ITCH-style packets and replayed through the unchanged `Pipeline` (`hft_replay`, see LIVE_FEED.md). This is offline tooling: the capture is a Python script, the conversion allocates freely, and nothing in `feed/` is on the hot path.

`feed::LiveSession` (`feed/live.hpp`, tool `hft_live`) runs the same chain one line at a time with the time passed in, and owns the guards a live process needs: stale-feed detection, reconnect and window-exit handling, the periodic comparison with the independent model, kill and shutdown. Every way of losing trust in the feed ends in one flatten routine that cancels through the gateway without giving the strategy a look. See LIVE_PAPER.md. `scripts/supervise.py` keeps the capture and `hft_live` running and `hft_live` emits a JSON event log and metrics (OPERATIONS.md). With `--bridge`, `hft_live` also mirrors its orders over a pipe to `scripts/order_bridge.py`, which keeps a Coinbase sandbox account in step (SANDBOX.md). With `--loop` and `--exchange-fd` the exchange's reports come back into the gateway and OMS and a flatten waits for real cancel confirmations (CLOSED_LOOP.md).

## Not built

Live ingestion and live order entry against a real venue, FIX 4.4 parser, AVX2 paths, MSan, AF_XDP/DPDK ingress, GoogleTest/vcpkg wiring (tests use a small check macro), multi-instrument or multi-venue operation, any real exchange connectivity, and bare-metal latency confirmation.
