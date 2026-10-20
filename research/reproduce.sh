#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MPLBACKEND=Agg
export MPLCONFIGDIR="${TMPDIR:-/tmp}/hft-matplotlib"
mkdir -p research/logs
cmake --preset release > research/logs/configure.log 2>&1
cmake --build --preset release -j4 > research/logs/build.log 2>&1
ctest --test-dir build/release --output-on-failure -j2 > research/logs/ctest.log 2>&1
scripts/ci_local.sh > research/logs/ci-local.log 2>&1
python -m pytest research/test_analysis.py -q > research/logs/analysis-tests.log 2>&1
python research/run_experiments.py > research/logs/experiments.log 2>&1
python research/build_report.py > research/logs/report.log 2>&1
python research/run_spacing_study.py "${HFT_HOST_LABEL:-host}" > research/logs/spacing.log 2>&1
python research/analyze_spacing.py >> research/logs/spacing.log 2>&1
cd research/paper
pdflatex -interaction=nonstopmode -halt-on-error ieee_paper.tex > /tmp/hft-latex-pass1.log
bibtex ieee_paper > /tmp/hft-bibtex.log
pdflatex -interaction=nonstopmode -halt-on-error ieee_paper.tex > /tmp/hft-latex-pass2.log
pdflatex -interaction=nonstopmode -halt-on-error ieee_paper.tex > /tmp/hft-latex-pass3.log
