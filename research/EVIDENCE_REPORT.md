# HFT evidence and submission review

## Scope and changes

This is a C++20 systems/synthetic backtesting study. The supplied manuscript's extra optimal-control, cache-proof, formal-verification and real-tape claims have been removed. Local branch: `research/hft-evidence-revision`; no push or submission is authorized or performed.

Corrected nearest-rank quantiles (`ceil(q*n)`), with odd-count regression coverage. Added read-only histogram exports with counts and tick units, preserving the timed recording path and fixed storage. Fixed closed-stdin Python integration harness behavior without suppressing process/EOF/signal assertions. Added a mixed-operation benchmark against the existing independent `std::map`/`std::list` reference. Added independent Python paired inference, exact sign tests, market bootstrap CIs and Holm multiplicity correction with tests against known values.

## Evidence map

| Result | Evidence | Boundary |
|---|---|---|
| Release builds with warnings-as-errors | `logs/build.log`, configuration in metadata | GCC 14 on this host |
| Core functionality and simulation gates | `logs/ctest.log` | 19 CTest suites, including regression changes |
| Integration behavior and fault recovery | `logs/ci-local.log` | Python suites and 28 local fake-exchange drills |
| Research statistics | `logs/analysis-tests.log`, `test_analysis.py` | Three independent known-value/validation cases |
| Replay consistency | `benchmark_*.txt` verification and return codes | Same recorded synthetic session; not exchange truth |
| Timing distributions | `benchmark_*.json`, histogram reconstruction checks | Binned in-memory timings, five runs; shared VM |
| Matched book-update baseline | `book_baseline.txt`, `book_baseline_summary.csv` | Same mixed operations, periodic and final state checks; narrow update scope |
| Primary/confirmation ablations | `primary.json`, `confirmation.json`, `paired_statistics.csv` | 16 paired markets per family; four Holm-adjusted sign tests |
| Generator robustness | `regimes.json`, `regime_statistics.csv` | Seven settings, same generator; 28-test Holm family |
| Latency/queue/fee effects | `latency.json`, `queue.json`, `fee_*.json`, `sensitivity_statistics.csv` | Synthetic fills; delay is per hop |
| Training-only selection | `selection.json`, including selected and test variants | Disjoint market seeds; six-point grid, two retained candidates |
| Determinism | `primary_repeat.json` equals primary JSON | Same binary/configuration; excludes timing rows |
| Manuscript traceability | `build_report.py`, table/figure sources and macros | Every plotted/reported numerical result is generated from raw outputs |

The baseline PnL differences in the repository's 16-market example reproduce to reported precision. This supports those specific simulated outcomes; it does not independently validate the generator as a market model. The predecessor's Xeon wall-clock timings are not expected to reproduce on this EPYC host. Five fresh measurements replace them. Timing output is taken after other task-started CPU-heavy tests finish; uncontrolled host noise remains. The metadata records source and binary hashes and CMake flags.

## Failed, skipped and unrun checks

The onboarding baseline had a deterministic live-tool test failure on both Python 3.12 and 3.13 (closed stdin then `communicate`). It is corrected and the original assertions are retained. One optional order-bridge case requiring a recorded capture skips because no capture is supplied under data/. Fake-exchange integration does not replace that real-tape coverage.

Long fuzz campaigns, mutation re-execution, TSan/ASan/UBSan configurations, the Valgrind/Callgrind instruction gate and real Coinbase sandbox operations are not newly executed by this research pipeline. Historical mutation kill percentages and instruction counts are not claimed as reproduced. No external tape, real exchange fills, network/NIC latency, endogenous market impact or live profitability is verified.

## Statistical and methodological limits

Paired markets, not fills, are sampling units. Student-t mean intervals assume an appropriate distribution of differences; exact sign tests ask about sign balance and can disagree with mean intervals. The multiple-testing family is explicit. Bootstrap intervals use market resampling and a fixed seed, not pseudoreplicated fills. Holm adjustment does not establish a correct market model. Same-generator holdouts test seed generalization only. Fee and queue sweeps are sensitivities, not a post hoc grid for claiming an optimum. The informed-flow generator explicitly rewards imbalance; noalpha is an internal ablation, not a comparison to an independently calibrated Avellaneda–Stoikov controller.

Zero-copy hot-cache processing, timing floors, fixed pools and compile-time layout constraints do not prove deterministic latency. The exported histograms preserve distributions within binning but not individual samples or temporal dependence. Five runs on one shared host cannot establish tail guarantees or universal throughput. The map/list reference is deliberately simple and is not a tuned commercial or open-source competing engine. No cost-adjusted Sharpe ratio or profitability claim is warranted.

## Novelty and reviewer recommendation

Novelty: an integrated, inspectable systems artifact with coherent book/OMS/risk/replay checks, a synthetic research lab and a reproducible evidence chain. Cache-aware structures, event loops, inventory heuristics and standard statistical methods are established. No original HJB derivation, formal proof, or new market-making theory is established.

IEEE reviewer recommendation: **major revision / not submission-ready**. Strengths are working code, preserved experiments, independent state comparisons, clear measurement boundaries and corrected statistics. Weaknesses are modest algorithmic novelty, no tuned external end-to-end baseline, synthetic-only markets, one host, few timing repetitions, incomplete current literature, and absent independent artifact replication. A serious systems submission should add a matched external engine and cross-host measurements; a financial-method paper needs a distinct validated scientific contribution.

Venue recommendations: IEEE HPEC is a possible systems/performance fit after stronger external baselines; IEEE CIFEr is a possible synthetic-finance fit if an active call matches; IEEE Access is broad but still needs novelty and independent validation. Current active calls, page limits, fees, anonymity and data/code policies are unverified. Avoid claims about journal impact factors or review speed.

## Submission checklist

- [x] Separate local branch and source changes.
- [x] Release build and regression tests retained.
- [x] Prespecified primary/confirmation seeds and full synthetic raw outputs.
- [x] Holm-adjusted paired inference, bootstrap intervals, and decomposition.
- [x] Regime, latency, queue, fees and held-out selection experiments.
- [x] Histograms and matched reference-book operation benchmark.
- [x] Generated IEEE-format manuscript, tables, figures and PDF.
- [x] Unsupported empirical/formal claims removed.
- [x] No real orders, repository push, deletion, or submission.
- [ ] Comprehensive current literature and publisher verification of bibliography.
- [ ] Stronger contribution argument and tuned external engine baseline.
- [ ] Independent artifact replication and cross-host timing.
- [ ] Real-data/exchange validation for any future empirical claims.
- [ ] Verify selected venue's current official author kit and rules.
- [ ] Complete author names, affiliations, funding/conflicts and user-approved final submission.


## Addendum: spacing study and second-host replication

Added `bench_t2t --gap-sweep` (idle spin before each tick, outside the timed interval, matched gap-0 control over the same inputs), `run_spacing_study.py` and `analyze_spacing.py`. Host B (Intel Xeon 2.8 GHz, 2 vCPU, GCC 13.3, shared VM) ran five sequential repeats with every replay hash verified. Median no-order tick p50 versus control: 1.0x at 1 us, 1.1x at 5 us, 1.7x at 20 us, 3.1x at 100 us, 6.4x at 1 ms, 19x at 10 ms (300 inputs only; per-run range 15-37x). Hot-cache order-tick p50 was 158-161 ns on Host B versus 96-104 ns on Host A; book speedup 4.89x versus 4.87x. Mechanism not isolated; no hardware counters. The live-feed spacing observation (docs/LIVE_PAPER.md) is cited as corroboration only: different VM, capture not in repository, not reproduced here.

Still open: systematic literature search for prior controlled spacing measurements; verification of Budish et al. and Holm from primary records; bare-metal replication with counters; independent artifact replication; author block, affiliation and AI-use disclosure details; current venue rules.
