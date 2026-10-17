# ADR 33: Compare strategy variants with paired runs in the backtester

Status: Accepted

## Context
`bench/bt_report` sums a few hand-picked sweeps over four markets and prints totals. That shows the machinery but cannot say whether a difference between two settings is larger than the market-to-market spread, and the PnL report built in Phase 21 had only ever seen fills from a fake exchange.

## Decision
- A new tool `hft_lab` runs named `MmConfig` variants over N generated markets, every variant on every market, and reports means with standard errors, paired differences against a base, an exact sign test and a t statistic. PnL is shown split into edge and inventory, because the inventory part is price drift under a position.
- Logic that can be wrong lives in `include/hft/bt/lab.hpp` (variant parsing, statistics, event writer, run wrapper, health check) so that C++ tests reach it directly; `src/tools/hft_lab.cpp` is argument handling and printing.
- The backtester gets two optional observer hooks, `fill(...)` and `mid(...)`, detected with a `requires` expression. Observers that do not define them (the replay recorder, the null observer) are unchanged. The lab uses them to write `hft_live`-format event logs, so `pnl_report.py` runs on simulated fills and its totals can be checked against the backtester's own accounting.
- `Backtester::run` takes an optional step budget and returns false when it runs out. The lab sets one. Reason: a variant with `requote_ticks=0` and `min_interval_ns=0` never lets the queues drain, and an unbounded loop is a hang, not a result. Such a run is reported as unhealthy (exit 3).
- Statistics are deliberately plain: mean, standard error, sign test. No bootstrap and no multiple-comparison correction; the tool says that in its output.
- `pnl_report.py` now keeps mid samples in file order when timestamps tie (stable sort), instead of ordering ties by value. With millisecond timestamps two samples in one millisecond are normal in simulated logs, and the old order could make the final mark a stale value.

## Consequences
A comparison is only as good as the generator and the queue model (see `docs/STRATEGY_LAB.md`): fills are simulated, the tape does not react, and one generator produces every market. The hot path and the replay oracle are unchanged (the perf gate and the replay hash check are the evidence).

## Rejected
- Extending `bt_report`: it is a gate report with fixed studies; a parameterised tool with paired statistics is a different job.
- A Python driver around `bt_report`: would duplicate the run setup and could not reach the accounting.
- Multiple-comparison corrections and confidence intervals from a bootstrap: more machinery than the data (a handful of markets) can support; the honest statement is that few markets give little power.

## Evidence
`tests/test_lab.cpp`, `scripts/test_lab.py` (including all 20 runs of a lab invocation read back through `pnl_report.py`), `scripts/test_pnl.py` tie cases, mutation run.
