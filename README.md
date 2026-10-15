# hft

Production-style tick-to-trade engine in C++20. Built in phase gates; see docs/DESIGN.md.

## Build

```
cmake --preset release && cmake --build --preset release
ctest --preset release
./build/release/bench/bench_harness
```

Other presets: relwithdebinfo, asan, ubsan, tsan.

## Status

Phase 0 (skeleton, clock, histogram, harness). No benchmark claims are made in this README; run the harness to produce numbers for your machine.
