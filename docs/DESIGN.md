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

## 11. Phase 4: matching engine and exchange simulator

### Rules
Everything below is pinned by a named test in tests/test_sim.cpp.

| Topic | Rule |
|---|---|
| Priority | Best price first; within a price, earliest arrival. Trade price is the resting order's price. |
| Limit | Matches what crosses, rests the remainder. |
| Market | Matches at any price, remainder cancelled (MarketRemainder), never rests. The price field is ignored. |
| IOC | Limit price, remainder cancelled (IocRemainder). |
| FOK | Decided before anything changes. If the full quantity cannot trade now: Accepted, then Canceled(FokUnfillable), book untouched. |
| PostOnly | Equal price counts as crossing; a crossing post-only is Rejected(WouldCross) with no Accepted. |
| Validation order | id 0, then quantity, then price (window and tick), then duplicate id, then post-only cross. First failure wins. |
| Replace | Shrink at the same price keeps the queue position (public Reduce). Any other change removes the order (public Delete) and re-enters it as a Limit with the same id (it may trade). |
| Cancel / replace auth | Only the owner may touch an order; otherwise CancelRejected / ReplaceRejected(UnknownOrder). |

### Self-trade prevention
Applied when an incoming order meets a resting order of the same owner.

| Mode | Effect |
|---|---|
| None | The trade happens. |
| CancelNewest | Incoming remainder is cancelled; the resting order stays. |
| CancelOldest | Resting order is cancelled and matching continues behind it. |
| CancelBoth | Both are cancelled. |
| DecrementAndCancel | Both shrink by the smaller quantity with no trade; the one that reaches zero is removed. |

A FOK order is only feasible if STP would not stop or shrink it: CancelOldest skips own orders (they get cancelled when the order actually runs), the other modes make a FOK that meets its own order unfillable.

### Event contract
Per request, in this order: Accepted; for each trade the taker Fill then the maker Fill, then the public Exec; terminal Canceled for any remainder; public Add if the remainder rests. Rejected requests emit exactly one report and no public events. The reference matcher produces the identical sequence, including sequence numbers.

### Exchange simulator
```
 participant --ingress--> [engine] --egress--> participant   (private reports)
                                   --md------> subscribers   (public events -> ITCH -> Phase 3 handler)
```
Time is always an argument. Each leg is base + uniform[0, jitter]; each (participant, direction) link is FIFO, so jitter cannot reorder one session. Messages from different participants race, and ties resolve by queue order, so a run is repeatable bit for bit for a given seed. Zero latency makes it equal to calling the engine directly (tested).

### Tradeoffs and limits
- One instrument per engine, same as the book. Multi-instrument is one engine per instrument.
- Ids are unique among resting orders only. A real exchange forbids reuse for the day; enforcing that needs unbounded memory or a Bloom-style filter, which is not worth it for a simulator.
- A replace always re-enters as a plain Limit. Original post-only intent is not remembered (an extra byte per order and a policy choice nobody asked for).
- If the pool is full when a remainder should rest, the remainder is cancelled with BookFull after the matching it earned. Pre-checking would have to guess how many slots matching frees.
- processing_ns adds delay but does not serialise requests; there is no matching-engine queueing model. Queueing delay is Phase 7's concern, measured on the real pipeline rather than modelled here.
- The simulator's output queue cannot refuse events, so overflow is counted (dropped_out) instead of back-pressured; tests assert it stays zero.
- Auctions, iceberg/hidden orders, stop orders, price bands and circuit breakers are out of scope.

## 12. Phase 5: OMS, risk and gateway

```
 strategy --> Gateway --> RiskEngine.check --> Oms.on_send --> encode --> wire --> ExchangeEdge --> ExchangeSim
                ^                                                                          |
                +---- Oms.on_report <-- decode <-- StreamDecoder <-- wire <-- encode <-----+
```

### Order state machine
States: PendingNew, New, PartiallyFilled, PendingCancel, PendingReplace, Filled, Canceled, Rejected. Events: Ack, Reject, Fill, Cancel, Replaced, CancelReject, ReplaceReject, RequestCancel, RequestReplace. One constexpr table gives the action for every (state, event) pair:

| Action | Meaning |
|---|---|
| Apply | Normal transition. |
| Ignore | Legitimate late message (a cancel reject after the order already filled). Counted, no effect. |
| Anomaly | The exchange said something impossible in this state. Counted, order untouched, never trusted. |
| Refuse | Our own request is not allowed in this state (second request while one is pending, request on a finished order). |

Fills keep PendingCancel and PendingReplace pending, because the exchange may match before it sees the request. A self-trade-decrement cancel with leaves left shrinks the order instead of ending it. The tests carry a second, independently written copy of the table and compare all 110 cells.

### Exposure and risk
Each live order reserves exposure: PendingNew reserves the full quantity, a working order its leaves, and an order with a replace in flight leaves plus the new quantity (a fill can land before the replace does, so the old size is still at risk, then the new size rests). Position limit check: pos + open_buy + qty <= max (mirror for sells). Because fills move quantity from reservation to position one for one, the sum never grows from fills; the e2e test asserts the bound after every step.

Check order: kill switch, loss limit, bad params, max order qty, no reference price, notional, price band, position, open orders, rate limit. The token bucket is last so an order refused for any other reason spends no token. Loss limit uses mark-to-market against the reference price and latches the kill switch.

### Kill switch
Latched; first cause kept (Manual, MaxLoss, SessionGap, Disconnect). On kill the gateway cancels every working order, then repeats the sweep after every inbound report until none remain, because a cancel can cross a replace-accept or leave a new order that was in flight. New orders and replaces are refused. Cancels bypass risk so the switch can always flatten. Reset is an explicit call.

### Wire format
Frame: u16 length, then body: type u8, seq u64, payload (all big-endian). Types: O new (35 bytes), X cancel (17), U replace (33), R report (71). Decoders require exact lengths and valid enums. A zero or oversize length breaks the stream; it stays broken until reset. Sequence handling on the client: a duplicate is dropped, a gap or malformed message trips the kill switch.

### Results
- risk check p99 about 30-40 ns, gate under 1000 ns (bench_risk, gcc and clang, shared VM).
- fuzz_oms found the Replaced leaves <= 0 bug; mutation 20 of 20 caught.

### Tradeoffs and limits
- One outstanding request per order: simpler state machine, and it matches how venues treat cancel/replace chains in practice, but a strategy cannot queue a cancel behind a replace.
- A replace reserves old plus new quantity. Conservative: a client near its limit cannot replace even a shrink. The alternative (reserve the max) was shown unsound by the e2e test.
- Replace quantity means the new open quantity, not the new total.
- Finished orders are retained in a ring and evicted oldest-first. A report for an evicted id is counted as unknown, never applied.
- Cancels bypass risk and the rate limit.
- No heartbeats, logon or retransmission: a gap is a kill, not a recovery. Real sessions (FIX or a binary venue protocol) need those; they are Phase 8 runbook items.
- The reference price for the band and loss limit is supplied by the caller (the e2e test uses the engine mid); a stale reference is the caller's risk.

## 13. Phase 6: strategy and backtester

```
 tape (public events) ---+--> venue book --> VirtualVenue (our orders, queue position)
                         |                        ^                    |
                         |                    ingress                  | fills, acks
                         |                        |                  egress
                         +--(md_ns)--> strategy book --> MarketMaker --> Gateway --> OMS --> Risk
                                                         ^                              |
                                                         +------------------------------+
```
The strategy runs through the same Gateway, OMS, risk engine and wire format as in production. Only the venue is simulated.

### Why the data is synthetic
The sandbox has no licensed market data, so the tape comes from a generator that drives the real matching engine. A latent fair value jumps every few hundred requests. Informed takers send market orders toward it while the mid lags; passive flow leans toward it before that (more bids added, asks cancelled), which is what makes order book imbalance predictive. The test suite measures the property instead of assuming it: on the default seed the imbalance points the right way for the next mid move 84% of the time. That is far stronger than real markets give. Every PnL number in this chapter therefore shows how the machinery behaves and how effects scale, not what anyone could earn.

### Queue-aware fills
Our orders are not on the tape, so each carries a simulated queue position. TapeBook keeps an arrival number for every resting tape order, which makes "ahead of us" and "behind us" exact (L3):
- an order ahead of us trades or is cancelled or cut: `ahead` shrinks;
- an order behind us trades: price-time priority says we traded first, so we fill min(leaves, print size);
- a print at a worse price than ours: whoever sold passed through us, so we fill min(leaves, print size) (the provable part);
- new orders and replaces that lose priority join behind the level as it is at arrival.
Orders that would take liquidity are rejected. Filling them honestly needs a market impact model, which this chapter does not have.

Two independent derivations of the queue exist (arrival order decides fills, tracked quantity gives `ahead`). They must agree: nothing ahead when someone behind trades, and `ahead` never shrinks by more than it holds. Violations are counted (`queue_inconsistent`); the tests, the fuzzer and every backtest require zero.

`queue_penalty_pct` pretends p% more of the level was ahead of us at arrival, drained by trades only. It exists because the exact model is exact only about the tape; the real queue also depends on things the tape cannot show.

### Validation against the real engine
Until our order first trades, the real engine and the simulation are the same world. So the test injects one passive order at a random moment into (a) the real engine replaying the same script of other participants and (b) the simulation. Over 250 injections: the engine filled 212 times, the exact simulation 212, at the same moment in 209 and for the same size in 189. It never filled earlier or for more than the engine did. Where it differs it is late or small: a sweep that ends exactly on our order, with nothing behind us to show it, leaves no trace on the tape. That is the one known source of missed fills, and it errs in the safe direction.

### Event ordering and determinism
Time comes only from the tape and the configured latencies; jitter comes from a seeded generator and cannot reorder a link. Ties go to: tape event at the venue, order message arriving at the venue, tape event reaching the strategy, report reaching the strategy. A message arriving at the instant of a tape event is therefore handled after it (the order is exposed to the event; tested). A run is a pure function of tape, config and seed. Reports are byte-identical across two runs, across gcc and clang, and a pinned digest guards every change in semantics. The cross-compiler check found a real bug in the generator: random draws inside one argument list are evaluated in different orders by gcc and clang, which produced two different markets from one seed. Draws now sit in separate statements.

### Strategy
fair = mid + alpha * I - skew * position, I = (bid depth - ask depth) / (bid depth + ask depth) over the top 3 price levels, all in 1/16 ticks (integers). bid = floor(fair - half spread), ask = ceil(fair + half spread); clamped so a PostOnly never takes and the two quotes never cross. A side is not quoted when filling it would take position plus working orders past the cap. A quote moves (replace) only when the target is a full tick away, at most once per 50 us per side, because every replace sends it to the back of the queue. The strategy keeps no order state of its own: it reads the OMS.

### Accounting
Exact, in half raw price units: value = 2*cash + pos*mid2, and
- at a fill: dV = side*qty*(mid2 - 2*price) - 2*fee*qty (spread capture, fees);
- at a mid change: dV = pos*(mid2_new - mid2_old) (inventory).
So value = spread capture + inventory + fees without remainder; the tests assert it to the last unit on random configs and against the OMS cash and position (which agree with the venue once everything drains).
Adverse selection is read against the mid 100 us, 1 ms and 10 ms after each fill. Realized spread = side*(mid_later - price): the edge actually kept. Post-fill move = side*(mid_later - mid_at_fill): negative means the market moved against the fill. Fill rate is per quote (an accepted order, or a replace that lost its place, is a fresh offer) and per quoted share. Sharpe is computed from 1 ms PnL bins and annualised as mean/std * sqrt(bins per 6.5 h x 252 d); on synthetic data this is illustrative only.

### Results (bench/bt_report, 4 synthetic markets of 1M requests each, ~10 s of market time)
Headline run, market data 5 us, ingress 10 us, egress 10 us, 2 us jitter:

| | |
|---|---|
| PnL | +92.85 (spread capture +43.26, inventory +49.59) |
| fills | 2793 fills, 4597 shares |
| fill rate | 6.4% of quotes traded, 3.7% of quoted shares |
| effective edge | +0.94 ticks per share at the fill |
| post-fill move at 100 us / 1 ms / 10 ms | +0.02 / +0.28 / +1.29 ticks per share |
| Sharpe | 0.30 per 1 ms bin |
| max position | 9 shares |
| replay speed | 4.3 M tape events/s including gateway, OMS, risk and wire codec (shared VM) |

Signal ablation (sum over 4 markets, same latencies): imbalance lean of 0, 0.5, 1, 2 ticks gives PnL +25.5, +158.5, +365.1, +752.9; the post-fill move at 1 ms goes from -0.43 (adverse selection: fills are the ones the market was about to run over) to +0.38. Without the lean the strategy is adversely selected; with it the signal does the work.

Latency versus alpha decay (round trip = md + ingress + egress, split evenly):

| round trip | PnL | spread capture | inventory | post-fill move 1 ms |
|---|---|---|---|---|
| 0 us | +374.8 | +166.0 | +208.8 | +0.32 |
| 30 us | +360.2 | +165.9 | +194.2 | +0.27 |
| 120 us | +297.1 | +160.5 | +136.7 | +0.17 |
| 250 us | +223.1 | +144.9 | +78.1 | +0.06 |
| 500 us | +121.7 | +126.9 | -5.2 | -0.12 |
| 1000 us | +7.3 | +131.3 | -124.1 | -0.32 |

The inventory term, the part that comes from the signal, falls to zero near 500 us and goes negative beyond, while spread capture holds up. In this market the edge is gone after about half a millisecond; fill rate rises with latency because stale quotes get hit more often, which is the same adverse selection seen from the other side.

Queue depth sensitivity (pretend p% more of the level was ahead): PnL +365.1, +316.7, +297.9, +262.7, +217.0 for p = 0, 25, 50, 100, 200. A deeper queue costs fills but the result degrades gracefully.

### Tradeoffs and limits
- Synthetic data only. The mechanics are real and tested; the returns are not predictions.
- No market impact. Our fills do not change what others do. Fine for small passive quantities; wrong for size.
- Passive only. Taking liquidity is rejected rather than modelled.
- One known source of missed fills (sweep ends exactly on us) and no source of invented ones, as far as the engine comparison shows.
- The strategy has no timers: it acts on tape events and reports, so a quiet market means stale quotes.
- Positions are marked at the final mid, not unwound; the loss-limit kill sweeps orders but leaves the position (flattening is a decision for the runbook in Phase 8).
- One instrument, one strategy, flat per-share fees.

