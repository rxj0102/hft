# Operations

How the live paper pair is run, watched and recovered. Everything here is for the paper venue: no order goes to an exchange, so what is shown is that the processes recover and that quotes are pulled, not what a real exchange would do.

```
python3 scripts/supervise.py --run-dir /tmp/hft-run          # capture_l2.py --out - | hft_live, restarted when needed
python3 scripts/drill.py                                     # fault drills against a local fake feed, about 25 s
```

## What runs

`supervise.py` starts `capture_l2.py` and `hft_live` joined by a pipe, each in its own session (so a terminal Ctrl-C reaches only the supervisor, which forwards it). Files in `--run-dir`:

| File | Content |
|---|---|
| `events.jsonl` | `hft_live`'s event log, appended across restarts |
| `metrics.prom` | `hft_live`'s metrics, Prometheus text format, rewritten every 2 s by rename |
| `capture.err`, `live.err`, `live.out` | the processes' own output, appended |
| the supervisor log (`--log`, default stderr) | one JSON line per supervisor decision |

## The event log (`hft_live --log`)

One JSON line per event, stamped with `ts_ms` (wall clock) and `mono_s`. Events: `start`, `segment_start`, `segment_end` (reason, packets, `flat`, `kill_latched`, `book_matched_model`, `flatten_rounds`, `flatten_sim_ns`, `open_before_flatten`), `stale_feed`, `resubscribe_requested`, `clock_regression`, `halted` (only for a halt nobody asked for: model check, final mismatch, failed flatten), `exit` (cause: `signal` or `input_ended`).

A process that was killed with SIGKILL writes no `segment_end` and no `exit`. A `segment_start` with no later `segment_end` before the next `start` is how a crash looks in the log.

## Metrics (`hft_live --metrics`)

Gauges: `hft_live_quoting`, `hft_live_halted`, `hft_live_open_orders`, `hft_live_uptime_seconds`, `hft_live_message_ns{q}`. Counters: lines, feed messages, bad messages, segments started, `hft_live_segments_ended_total{reason}`, stale feeds, resubscribe requests, model checks and failures, final mismatches, flatten failures, orders new/replace/cancel, risk rejects. The file is refreshed by the main loop every `--metrics-s` seconds even when no input arrives; the supervisor's watchdog relies on that.

Suggested alerts (not tested, not deployed anywhere): `hft_live_halted == 1`; `increase(hft_live_model_check_failures_total[5m]) > 0`; `increase(hft_live_flatten_failures_total[5m]) > 0`; `hft_live_quoting == 0` for more than a minute; `increase(hft_live_stale_feed_total[10m]) > 3`; the metrics file's age above 10 s.

## What the supervisor does

| Event | Response |
|---|---|
| `hft_live` exits for any reason except 4 | kill the capture, restart both after a backoff |
| the capture exits | `hft_live` sees end of input and exits flat; if it has not within 5 s it is killed; restart |
| `hft_live` exits 4 (model disagreement, failed flatten, desync) | do not restart; exit 4. A person looks first |
| metrics file not refreshed for `--watchdog-s` (30 s; after a 10 s start-up grace) | SIGKILL, restart |
| more than `--max-restarts` (5) starts inside `--window-s` (300 s) | give up, exit 3 |
| SIGTERM or SIGINT | SIGTERM to `hft_live` (kill switch latched, quotes cancelled), SIGKILL after `--term-grace-s` (10 s), exit 0 |

Backoff starts at 0.5 s, doubles to 30 s, and resets after a run of `--stable-s` (60 s). The reset is untested.

## Drills

`scripts/drill.py` runs each against a local fake feed (invented prices, a few updates a second, a heartbeat each second) with the real capture script and the real `hft_live`:

| Drill | Fault | Checked |
|---|---|---|
| drop | the server closes the connection | segment ends on the reconnect marker, flat, with quotes taken down; a new segment starts; no process restart |
| silent | the server stays connected and says nothing for 4 s | stale feed detected while the connection is open (drill stale limit 600 ms, silence 4 s; the measured delay is printed by the drill); flat; kill switch not latched; quoting resumes |
| kill_capture | kill -9 of the capture | `hft_live` ends flat on end of input, exits 0; the pair restarts; quoting again |
| kill_live | kill -9 of `hft_live` | recorded as signal 9; one restart; the old capture does not outlive its reader |
| hang | SIGSTOP of `hft_live` | the watchdog notices, kills it, restarts the pair |
| sigterm | SIGTERM to the supervisor | `hft_live` ends with reason `kill`, flat, switch latched; exit 0, no restart |
| term_ignored | a child that ignores SIGTERM | SIGKILL after the grace period; exit 0 |
| capture_dies_live_stuck | capture exits, `hft_live` does not follow | killed after 5 s and restarted |
| slow_flapping | failures spread wider than the window | not treated as a loop |
| crash_loop | `hft_live` exits 1 at once | gives up with exit 3 after the limit; backoff grows |
| integrity | `hft_live` exits 4 | not restarted; exit 4 |

Where the drills show orders being taken down, they also check `open_before_flatten > 0`, so "flat" is not a trivially true statement about an empty book.

A run on the real Coinbase feed through the supervisor: kill -9 of `hft_live` at 15 s (restart after 0.5 s), SIGTERM at 33 s (segment ended `kill`, 2 orders taken down, switch latched, exit 0).

## With the order bridge

`supervise.py --bridge` adds a third process (docs/SANDBOX.md). `bridge.jsonl` has its events (`bridge_start`, `clock_skew`, `cancel_all`, `start_flat`, `order_rejected`, `rate_limited`, `http_5xx`, `net_error`, `order_vanished`, `stray_order`, `fatal`, `exit_flat`, `exit`), `bridge.prom` its metrics (`hft_bridge_*`). If any of the three dies the trio restarts; the new bridge cancels everything first. Bridge exit 2 or 4 is fatal for the supervisor (exit 4). The extra drills: bridge_flow, kill_bridge, kill_live_bridge, exchange_down, bad_credentials, bridge_hang, all against `scripts/fake_exchange.py`.

## Closed loop (`supervise.py --loop`)

Adds a second pipe, bridge to `hft_live` (`--exchange-fd`). New `hft_live` exit code 6: the report channel closed (restartable). New supervisor event `stop_not_flat` (stop requested, `hft_live` exited 4: supervisor exit 4). New metrics: `hft_live_exchange_reports_total`, `hft_live_exchange_reports_stale_total`, `hft_live_snapshots_dropped_ending_total`, `hft_live_fills_total`, `hft_live_position_lots`. `segment_end` gains `position_lots` and `fills`; in this mode `flatten_sim_ns` is the real wait for confirmations. Details: CLOSED_LOOP.md.

### Closing the position at stop (`--close-position`)

`supervise.py --loop --close-position [--close-collar-bps N]` passes the option on to `hft_live`. On SIGTERM the order is: cancel everything, wait for the exchange, send up to five immediate-or-cancel orders against the position, then exit. If a position remains the exit is 4 (`stop_not_flat`, and `position_not_closed` in `events.jsonl`); read `segment_end` for `close_why_not`. Allow `--term-grace-s` for the extra round trips (about 0.5 s per attempt at `--rate 8`, more with a slow exchange). See CLOSED_LOOP.md and ADR 29.

## What this does not show

- A paper venue lives inside `hft_live`. After a SIGKILL its state is gone with the process. Against a real exchange the orders would stay working unless the exchange cancels on disconnect or a restarted process cancels what it finds. The drills cannot test that, and nothing here claims it is handled. It is the first thing real order entry must solve.
- Single host, no alert delivery, no log shipping, no metrics scraper. The files are written; nothing reads them but the watchdog.
- No hours-long run. The longest real run was minutes.
- Two of the drills depend on timing (stale limit 600 ms, watchdog 3 s) and could flake on a very loaded machine; margins are generous but not proven.
- The supervisor is a script, not a service manager: it does not survive a reboot, and it is a single point of failure.
