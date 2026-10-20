# HFT research artifact

Branch: `research/hft-evidence-revision`. The source checkout is preserved; no repository is pushed or paper submitted. Start with `GAP_ANALYSIS.md`, then `EVIDENCE_REPORT.md` and `paper/ieee_paper.pdf`.

## Reproduce

Needs GCC 14, Python 3.12, TeX Live (`pdflatex`, `bibtex`, `ieeetr.bst`, standard math/graphics/booktabs), and package-registry access for installation. IEEEtran is vendored with its license header.

```bash
python3 -m venv /tmp/hft-research
source /tmp/hft-research/bin/activate
python -m pip install -r research/requirements-lock.txt
bash research/reproduce.sh
```

Run from the repository root. The script builds the release preset with four jobs, runs the 19 CTest suites, Python integration suites and 28 fault drills, runs analysis-unit tests, then performs timing/lab experiments sequentially, generates results and compiles the IEEE-format draft. Stop other task-started CPU-heavy jobs before benchmark measurement. Shared-VM background scheduling noise still applies. Required tools and pinned dependencies are recorded; no credentials or exchange connection are needed. Existing scripts use local fake services. An optional recorded-capture bridge case is skipped if data/ has no recording.

`config.json` prespecifies sample counts, seeds and scope; `run_experiments.py` records full commands, source/binary hashes, compiler/CMake configuration and results. `analysis.py` and `build_report.py` recompute paired mean intervals, exact sign tests, bootstrap intervals and Holm corrections. `paired_statistics.csv` uses a family of four tests per primary/confirmation run, and `regime_statistics.csv` uses a family of 28. The delay axis is **per hop**, across three hops. Every lab result must exit zero; a health failure is not a usable result. A deterministic primary repeat must match the complete original JSON.

Benchmark JSON retains histogram counts and tick conversion in addition to percentile summaries. Quantiles are checked by reconstructing the rank from bins. These are binned latency distributions, not individual timestamp traces. The book-operation baseline runs identical mixed-operation tapes through the fast book and the existing independent map/list reference, checks state digests and alternates execution order. Its timing scope is narrower than tick-to-trade and its reference is not an external tuned engine.

Timing rows vary on reruns; synthetic market outcomes should be identical for the same configuration. Full raw logs and outputs are retained. The supplied manuscript's hard-coded illustrations are not reused as evidence. No proof of optimal control, formal verification, real-venue latency, or empirical profitability is claimed.
