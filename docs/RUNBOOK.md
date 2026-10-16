# Runbook

Scope, stated plainly: this repository is an engine library with tests, benches and fuzzers. There is no deployable trading binary and no live connectivity. This runbook covers (1) what a host process embedding `t2t::Pipeline` must do and watch, (2) what each alarm means and the first response, and (3) how to run and triage the build, bench and CI tooling. Nothing here has been exercised against a real exchange.

## 1. Before the engine starts

| Check | How | If it fails |
|---|---|---|
| Invariant TSC | `bench_harness` prints it (CPUID 0x80000007 EDX bit 8) | timings are not trustworthy; do not trade |
| Memory region | log `core::RegionReport` after `Region::create`: `huge_explicit`, `huge_thp_hint`, `locked`, `numa_bound` | start is allowed, but expect worse latency; record why (ulimit -l for mlock, hugepages reserved) |
| Dedicated core | pin the thread to an isolated core (`bench_t2t --cpu C` shows the method) | expect a noisier tail |
| Book sizing | `BookConfig::max_orders` = 3 x the instrument's peak resting orders, rounded to a power of two, minimum 1024; `num_levels` covers the day's price range; `tick_raw` matches the instrument | undersize gives feed losses, oversize gives latency |
| Risk limits | set every field of `RiskConfig` explicitly; the defaults are for tests. `max_loss_raw = 0` disables the loss limit, so set it | no loss limit means no automatic stop |
| MD session | set `MdConfig::locate` and, in production, `check_session = true` with the session id | otherwise the first packet fixes the session |
| Snapshot source | the host must be able to deliver a snapshot (`ingest_snapshot` / `on_snapshot`) to recover from a feed loss | without it the engine stays out of the market after the first loss |

Initialise through `Pipeline::init`. It allocates from the arena once; after that the path allocates nothing.

## 2. What to watch

All counters are plain integers readable at any time from the same thread.

| Counter | Where | Meaning |
|---|---|---|
| `feed_losses`, `pull_sweeps` | `Pipeline::stats()` | book became untrusted; quotes pulled |
| `gaps_filled` | `feed().stats()` | a loss was covered by the other line (healthy) |
| `gaps_declared`, `desyncs`, `book_errors` | `feed().stats()` | real loss or book rejection; snapshot needed |
| `malformed`, `bad_session` | `feed().stats()` | bad input on the feed |
| `pending_evicted`, `pending_dropped` | `feed().stats()` | out-of-order burst larger than `pending_slots` |
| `pulled_cancels` | `gateway().stats()` | cancels sent because the feed was untrusted |
| `kill_cancels`, `seq_gaps`, `broken_streams`, `bad_messages` | `gateway().stats()` | order-entry faults |
| `risk_rejects` | `gateway().stats()` | an order was refused by a limit |
| `anomalies`, `orphan_fills`, `overfills`, `mismatches`, `unknown_reports` | `gateway().oms().stats()` | the exchange and the OMS disagree |
| `risk().killed()`, `risk().kill_reason()` | gateway | kill switch state and first cause |

## 3. Alarms and first response

### Feed lost (`feed_losses` rising, `pulled_cancels` rising)
1. This is expected and automatic. The engine cancels working quotes and stays quiet. It is not the kill switch.
2. Confirm quotes are down: `gateway().oms().position().open_buy` and `open_sell` fall to 0 after the cancel reports arrive. Orders still PendingNew are cancelled after their next report; the sweep repeats.
3. Request or wait for a snapshot and feed it in. Quoting resumes by itself.
4. If it repeats, look at `gaps_declared` versus `desyncs`. Gaps point to the network or one line; desyncs point to book capacity or a feed bug. If `book_errors` grows with a stable market, capacity is too small: raise `max_orders` and restart.

### Kill switch tripped (`killed()` true)
`kill_reason()` is the first cause only.
- `MaxLoss`: mark-to-market loss passed the limit. Orders were cancelled. **The position is still on.** The engine does not flatten. A person decides: read the position (`oms().position().pos`), check the book is sane, then flatten manually or hold on purpose.
- `SessionGap`: the exchange sequence number skipped. Treat the session as untrusted. Reconnect, reconcile open orders and position with the exchange, then reset.
- `Disconnect`: the byte stream broke or a message was malformed. Same procedure.
- `Manual`: someone called `Gateway::kill`.

Reset is `risk().reset_kill()`. Do it only after reconciling the position and open orders with the exchange; the OMS state may be wrong after a sequence break. The kill switch is latched on purpose.

### OMS disagreement (`orphan_fills`, `overfills`, `mismatches`)
Treat any `orphan_fills` as serious: a fill arrived for an order the OMS does not hold, so the position may be wrong. Kill manually, reconcile position with the exchange, and keep the capture for analysis.

### Many `risk_rejects`
Find which check (`RiskResult`): rate limit means the strategy is too chatty or the bucket is too small; price band means the reference price is stale; position or open-orders means the engine is at its limits. Do not raise limits during an incident.

## 4. Tooling procedures

### Build and test
```
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release --output-on-failure -j2
```
`test_sim_gate` takes about 4 minutes on the development VM. Sanitizer presets: `asan`, `ubsan`, `tsan`. Fuzz: `cmake --preset fuzz` (needs clang); run a target with `-max_total_time=SECONDS`.

### Latency bench exit codes (`bench_t2t`)
| Exit | Meaning | Action |
|---|---|---|
| 0 | ran | read the tables; compare only like with like (same machine, same preset) |
| 1 | could not record or set up the session | check the printed error |
| 2 | VERIFY FAILED: replay bytes differ from the backtest | a behaviour change; no timing is meaningful. Find the change before anything else |
| 3 | page faults in the timed loop | something touched memory it should not have; check capacity and prefault |

### Perf gate failure
`python3 scripts/perf_gate.py --bench build/perf/bench/bench_t2t --strict` prints instructions per input by component. A fail means the hot path executes more than 2% more instructions than the baseline. Find the change (the component split points at the area). Update the baseline (`--update`) only for a deliberate change, and put the reason and the before and after in the commit message. The baseline is keyed on the compiler major version (13); a different major skips the gate unless `--strict`.

### Flame graph
```
./build/perf/bench/bench_t2t --requests 200000 --profile t2t.prof --seconds 6
python3 scripts/flamegraph.py --top 14 t2t.prof t2t_flame.svg
```
Needs the `perf` preset (frame pointers kept, `-g`).

### Fuzz crash
The crashing input is written as `crash-<hash>` in the working directory. Rerun with `./build/fuzz/fuzz/fuzz_<target> crash-<hash>`. Fix the bug or, if the oracle was wrong, fix the oracle and say so (this happened once: a gap can be open with nothing parked). Add a regression test before closing it.

### Capturing and replaying real data
```
python3 -m pip install websockets
python3 scripts/capture_l2.py --seconds 90 --verify --out data/btc.l2
./build/release/hft_replay --verify data/btc.l2.verify data/btc.l2
```
Read the output in this order:
1. `model check ... DIFFERS` or exit 4: the pipeline's book disagrees with the independent model. Treat as a bug. Save the capture and the output; do not trade or tune on that run.
2. `determinism ... DIFFERENT` or exit 2: same.
3. `exchange snapshot check`: expect nearly all of the 20 levels identical (live runs gave 20, 18, 20). A result near zero means the feed was read wrongly or the snapshot is from another time.
4. `outside window` large and `ended: price left the window`: the market moved out of the grid. Recapture, or raise `--levels`, knowing that a bigger grid costs cache.
5. Several `segments`: the connection dropped and reconnected; each segment is replayed on its own.
Do not commit anything under `data/`.

## 5. Known operational gaps
No automatic flatten. No order-entry heartbeats or retransmission. One instrument per pipeline. Snapshot delivery is the host's job. No live-trading mode has been tested.
