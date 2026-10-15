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
