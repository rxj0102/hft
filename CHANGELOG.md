# Changelog

## Unreleased

### Phase 1
- Core types, 128-bit overflow-safe notional math, fixed-point Price, price-band check.
- Region (mmap, hugepage, mlock, prefault, best-effort NUMA bind), Arena, index-based Pool.
- SPSC ring (Disruptor-style sequences, cached indices, claim/publish zero-copy API) with per-order memory-order justification.
- Lock-free binary logger with background flush thread and offline reader.
- Test-only allocation trap (per-thread) proving zero hot-path allocations.
- bench_ring: two-core pinned throughput, 7 trials, min/median/max.
- vcpkg.json manifest added; builtin-baseline is a placeholder to be pinned on the dev machine. GoogleTest and Google Benchmark are not yet used (tests still use the dependency-free CHECK macro).

### Phase 0
- Repo skeleton, design doc, CMake presets.
- TSC clock with calibration, log-linear latency histogram, benchmark harness.
