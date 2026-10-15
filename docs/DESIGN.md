# HFT Tick-to-Trade Engine: Design Document (Phase 0)

Status: draft for review. Language: C++20. Platform: Linux x86-64 (TSC, hugepages, perf).

## 1. Goals and non-goals

Goals
- A single-process tick-to-trade pipeline: market data in, book update, strategy decision, pre-trade risk, order out.
- Deterministic replay: the same captured event log always produces bit-identical output.
- Every performance number in this repo comes from a measurement in this repo.

Non-goals
- Real exchange connectivity or paid data. Feeds are synthetic or free public samples.
- Multi-venue smart order routing, FPGA offload, or microwave links.

## 2. Pipeline

```
 NIC/UDP -> [md] parse+normalize -> SPSC -> [book] update -> [strategy] signal
        -> [risk] pre-trade -> [oms] state machine -> [gateway] encode -> TCP/UDP out
                      \__________ binary event log (lock-free, background flush) __________/
```

Each arrow between threads is a single-producer single-consumer ring. Each stage owns its data (single-writer principle), so there are no locks on the hot path.

## 3. Hot path rules

| Rule | Enforcement |
|---|---|
| No heap allocation | Test-only global operator new trap that aborts if armed |
| No exceptions, no RTTI | Hot-path targets compile with -fno-exceptions -fno-rtti |
| No virtual dispatch | Strategies via templates and CRTP; reviewed in code review |
| No locks, no syscalls | Startup does mlock, pinning, prefaulting; hot loop is busy-spin |
| No floating point on prices | Fixed-point int64 price type (Phase 1) |

## 4. Modules

| Module | Purpose | Phase |
|---|---|---|
| core | types, fixed-point, TSC clock, histogram, pools, rings, logger | 0-1 |
| book | flat array price-level book, intrusive order lists, id hash map | 2 |
| md | ITCH-like and FIX 4.4 subset parsers, gap detect, A/B arbitration | 3 |
| sim | matching engine and exchange simulator | 4 |
| oms, risk, gateway | order state machine, pre-trade checks, wire encode | 5 |
| strategy, replay | market making, imbalance signal, backtester | 6 |
| bench, tools | harness, log decoder, latency report | 0, 7 |

## 5. Time and measurement

- Timestamps: RDTSC with lfence ordering (rdtsc_ordered) to stop the CPU reordering the read around the code under test.
- Calibration: TSC ticks per nanosecond measured against CLOCK_MONOTONIC_RAW over a fixed window. Invariant TSC is checked via CPUID (leaf 0x80000007, EDX bit 8); if absent we warn.
- Histogram: log-linear buckets (HdrHistogram style), fixed memory, O(1) record, no allocation after construction. Reports p50, p99, p99.9, max.
- Reported figures include the cost of the clock read itself, measured and subtracted as "timer overhead".

## 6. Build system

- CMake 3.25+ with presets: release, relwithdebinfo, asan, ubsan, tsan. MSan is added in Phase 1 (needs an instrumented libc++).
- Warnings: -Wall -Wextra -Wpedantic -Werror on all targets.
- Dependencies (GoogleTest, Google Benchmark) are added in Phase 1 through vcpkg manifest; Phase 0 has zero external dependencies so the skeleton builds anywhere.

## 7. Phase 0 gate

1. Clean configure and build with -Werror under release, asan, ubsan.
2. Self-checking test binary passes (clock monotonic, histogram percentiles correct on known data).
3. Benchmark harness prints timer overhead and a percentile table for a known workload.

## 8. Open questions (blocking Phase 1)

1. Venue flavor for the protocol work: equities ITCH-style, or crypto (Binance) feeds?
2. Target CPU: does the benchmark machine support AVX2 and invariant TSC? Phase 0 prints this.
3. Is vcpkg or Conan preferred for GoogleTest and Google Benchmark?

## 9. Tradeoff log

- Zero dependencies in Phase 0 vs using GoogleTest immediately: chose zero dependencies, so the gate is reproducible in any container. Cost: a tiny hand-rolled check macro until Phase 1.
- Log-linear histogram vs vendoring HdrHistogram_c: chose our own (about 80 lines), because it keeps the record path allocation-free and header-only. Cost: precision is bounded by our bucket scheme (about 1.6 percent relative error with 6 sub-bucket bits).

## 10. Phase 3: market data path

### Wire format (include/hft/md/itch_wire.hpp)
ITCH-style, not the real Nasdaq spec. A MoldUDP64-like packet frame (session[10], first seq u64, count u16, then length-prefixed messages, all big-endian) carries Add, Execute, Partial Cancel, Delete and Replace messages. A count-0 packet is a heartbeat whose seq is the sender's next sequence. Fields are read straight from the receive buffer with memcpy + bswap (one load, one byte swap each).

Validation is strict and whole-packet, done BEFORE anything is applied: bounds, per-type fixed lengths, side byte, sequence overflow, trailing bytes. A malformed packet therefore cannot half-apply. Unknown types of unknown length are skipped (forward compatibility) and still consume their sequence number.

### Handler (include/hft/md/md_handler.hpp)
```
              first copy of seq wins; later copies = duplicates
 line A ──┐                         ┌── in order ──► apply to book
          ├─► validate ─► sequence ─┤
 line B ──┘                         └── ahead of next_seq ──► park (fixed store)
                                              │ other line fills the hole: drain in order  (gap filled)
                                              └ timeout: declare gap ─► NeedSnapshot
 NeedSnapshot: park everything. Snapshot "as of S" (Begin/Add.../End) rebuilds the book,
 next_seq = S+1, parked packets replay (<= S discarded). Live again.
```
- Time is an argument (`now_ns`), never read from a clock, so a recorded session replays bit-identically.
- Any message the book rejects (unknown order, bad quantity, duplicate id) is treated as a desync: the book is no longer trusted, so we drop to NeedSnapshot rather than carry on with corrupt state.
- A feed Replace (new order id, back of queue) is applied as cancel + add on the same side; the new id is checked first so a failure cannot destroy the original order.
- Parked-packet policy when the store is full: evict the lowest sequence number. After an outage the packets right after a recent snapshot matter, not the oldest ones.
- Snapshot assembly is all-or-nothing: wrong count, mismatched End, a hole in the snapshot channel, or a rejected add aborts it and the handler keeps waiting for the next cycle.

### Price window
The book's price window can now be re-centred (`OrderBook::rebase`): it succeeds only if every resting order still fits, and is checked against the reference book in the property test. The handler does not call it automatically; deciding when to re-centre is a policy (Phase 6).

### Tradeoffs and limits
- Recovery is snapshot-only. There is no retransmission-request (MoldUDP rerequest) path; adding one would shrink recovery time but needs a bidirectional channel and its own state machine.
- One instrument per handler (filtered by locate). A multi-instrument feed runs one handler per instrument over the same packets, which parses each packet once per handler; a shared front end that dispatches by locate is a Phase 7 optimisation if profiling shows it matters.
- The session id is fixed by the first valid packet unless configured. A spoofed first packet could pin a wrong session; real deployments configure it.
- No SIMD in this phase: the format is length-prefixed, so there is no byte scanning to vectorise. AVX2 belongs with the FIX parser, which is deferred.
- Parked packets are copied (up to 1472 bytes each) because the receive buffer is reused. The copy only happens on the loss path.
