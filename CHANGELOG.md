# Changelog

## Unreleased

### Phase 5
- OMS (include/hft/oms/oms.hpp): eight-state order machine driven by one table (8 states x 9 events, each cell Apply, Ignore, Anomaly or Refuse). Tracks leaves, cum, per-order exposure reservation, position, open buy/sell exposure, cash (128-bit) and mark-to-market PnL. Fixed-size order pool with a retention ring that evicts the oldest finished order; no allocation after init.
- Risk engine (include/hft/risk/risk.hpp): ordered checks (kill switch, loss limit, parameters, order size, notional, price band, position, open orders, token-bucket rate limit last so refused orders spend no token); latched kill switch that keeps the first cause; loss limit by mark-to-market.
- Wire protocol (include/hft/gw/order_entry.hpp): length-prefixed big-endian frames, sequence number per direction, exact-length decoders with enum range checks, StreamDecoder that survives any byte split.
- Gateway (gw/gateway.hpp): risk, then OMS, then encode, then send. Kill sweeps cancel every working order and re-sweep after each inbound report until nothing is left. Sequence gap, malformed message or broken stream trips the kill switch. ExchangeEdge (gw/exchange_edge.hpp) is the exchange-side adapter onto the Phase 4 simulator.
- Replace exposure: while a replace is in flight the order reserves old leaves plus the new quantity, and the risk check requires headroom for the full new quantity. Found by the end-to-end test: with the earlier max(leaves, new) rule, a fill landing before the replace let position plus open exposure exceed the limit.
- Tests: test_oms (independently written 110-cell transition spec checked against the table, effects, exposure accounting, eviction churn, risk boundaries, rate limit, kill/loss, differential vs a slow reference risk engine over 200 random configs); test_gw (codec golden bytes, round-trip, malformed input, stream decoder under random splits, gateway session faults, and end-to-end runs through the simulator at zero latency, with latency and jitter, with chunked bytes, on a slow network, under tight limits, with manual kill, with loss-limit kill; checks client books against the exchange, an independent fill tally, and exposure after every step; zero-allocation proof).
- libFuzzer targets: fuzz_oms (invariants after every step) found a real bug in the first minute: a Replaced report with leaves <= 0 corrupted the books. Now counted as an anomaly and the old order is kept; regression test added. fuzz_codec (decoder consistency whole vs split, canonical re-encode, hostile bytes into gateway and edge).
- Mutation check: 20 injected bugs across risk, OMS table, exposure, gateway sequencing and codec; 19 caught at first, the survivor (ack quantity mismatch counter) now has a test, 20 of 20.
- bench_risk: Phase 5 gate. Risk check p99 is about 30-40 ns (gate: under 1000 ns) on gcc and clang, on accepting, replace and rejecting paths. Whole gateway submit (risk + OMS + encode) p99 about 30-45 ns. Shared 2-vCPU VM, so indicative only.
- Limits: one outstanding request per order, finished orders are evicted oldest-first (a late report for an evicted id is counted as unknown), cancels bypass risk, no heartbeats or retransmission, the rate limiter is checked last.

### Phase 4
- Matching engine (include/hft/sim/matching_engine.hpp): price-time priority; Limit, Market, IOC, FOK (read-only feasibility check, then all-or-nothing) and PostOnly; five self-trade-prevention modes (none, cancel newest, cancel oldest, cancel both, decrement-and-cancel); cancel; cancel/replace (shrink at the same price keeps queue position, anything else loses it and re-enters as a Limit that may trade). Trades print at the resting order's price. Storage reuses the Phase 2 flat-level design plus an owner field; no allocation after init.
- Two event streams per request: private execution reports (Accepted, Rejected, Fill x2 per trade, Canceled with a reason, Replaced, CancelRejected, ReplaceRejected) and public book events (Add, Exec, Reduce, Delete). The order of events within a request is part of the contract.
- Exchange simulator (exchange_sim.hpp): configurable ingress, egress, market-data and processing latency plus seeded jitter, caller-supplied time, per-link FIFO (jitter delays but never reorders one participant), deterministic tie-breaking, bounded event queue with visible back-pressure.
- ITCH publisher (feed_publisher.hpp): public events become Phase 3 wire messages, so the simulator can drive the real market data handler.
- Tests (tests/test_sim.cpp): 26 conformance scenarios that pin each rule; a slow reference matcher (flat vector, dry-run FOK) with differential runs of engine vs reference over all five STP modes on a tiny (forces BookFull) and a large book, compared per request on every report, public event and the book digest; per-request properties (two equal sides per trade, one Exec per trade, rejects leave no trace, killed FOK leaves the book untouched, PostOnly never trades); engine feed rebuilds an identical book in the Phase 3 handler (flush every 1, 5, 37 ops); simulator latency arithmetic, cross-participant races, FIFO under jitter, refusals, determinism, zero-latency equivalence to the bare engine, full pipeline engine -> latency -> ITCH -> handler -> book, and a zero-allocation proof.
- Mutation check: 15 injected engine bugs; 13 caught, the other 2 are equivalent mutants (cannot change behaviour).
- libFuzzer target fuzz_match runs the engine and the reference in lock step and traps on any difference.
- bench_match: throughput and per-request latency by request kind.
- Rules documented as limits: ids are unique among resting orders only (finished ids can be reused), a replace re-enters as plain Limit even if the original was PostOnly, a book-full remainder is cancelled with reason BookFull after matching, and processing_ns is added latency, not a serialising queue.

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
