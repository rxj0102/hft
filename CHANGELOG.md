# Changelog

## Unreleased

### Phase 3
- ITCH-style wire format: MoldUDP-like sequenced packet frame, Add/Execute/Partial Cancel/Delete/Replace plus snapshot Begin/End, big-endian zero-copy decode, strict whole-packet validation before apply.
- MdHandler: A/B line arbitration (first copy wins), out-of-order parking with a gap timer, heartbeat-based tail-loss detection, snapshot-based recovery with replay of parked packets, desync detection from book rejections, caller-supplied time for deterministic replay, per-line statistics, listener hook (no virtual dispatch).
- OrderBook gained clear(), side_of(), rebase() (price-window re-centring, verified against the reference) and check_invariants() (structural oracle).
- Generator bug found at scale: FeedGen's mix drifted upward above target and overran capacity on 2M-op runs; mix fixed to be net-removing above target.
- Tests: wire golden bytes and malformed-input cases, 12 handler scenarios, five whole-feed simulations (clean, lossy A/B, heavy loss, line B down, late join) checked against the reference book, zero-allocation proof for the handler, in-process mutation fuzzing. Six injected handler bugs and two rebase bugs were each caught.
- libFuzzer target (fuzz/fuzz_md.cpp, "fuzz" preset, ASan+UBSan) with a seed-corpus generator; oracle is check_invariants() after every input.
- bench_md: parse-only, single-line and A+B handler throughput; md headers compile under -fno-exceptions -fno-rtti.

### Phase 2
- Order book: flat per-side level arrays, intrusive FIFO order queues (32-bit pool indices), two-level bitmap for next-best-level search, open-addressing id map with backward-shift deletion. Operations: add, cancel, execute (partial/full), replace (priority kept only on same-price size reduction).
- Order-sensitive state digest (prices, quantities, queue order) for equivalence checking.
- Slow std::map/std::list reference book, seeded event generator with injected invalid events, property test (10M events on a 4096-level book plus 2.5M on a 256-level book, all seven result kinds exercised).
- Mutation check: three injected bugs (priority on grow, best-bid recompute, id-map shift) were each caught by the tests.
- hft_hotpath_check library compiles hot-path headers with -fno-exceptions -fno-rtti; it caught a missing <compare> include in fixed_point.hpp.
- bench_book: per-update latency by event type and untimed throughput.
- Known limitation: prices outside the configured window are rejected; window re-centring is deferred to Phase 3.

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
