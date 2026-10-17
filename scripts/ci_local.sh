#!/usr/bin/env bash
# The Python half of the CI job "build-test (gcc, release)", run locally. Needs build/release/hft_live
# (cmake --preset release && cmake --build build/release) and `pip install -r scripts/requirements.txt`.
# usage: scripts/ci_local.sh
set -u
cd "$(dirname "$0")/.."
LIVE=build/release/hft_live
[ -x "$LIVE" ] || { echo "build $LIVE first"; exit 2; }
fail=0
step() { name="$1"; shift; echo "==== $name"; if ! "$@"; then echo "FAILED: $name"; fail=1; fi; }
step "syntax" python3 -m py_compile scripts/*.py
step "pnl report tests" python3 scripts/test_pnl.py
step "strategy lab tests" python3 scripts/test_lab.py
step "capture script tests" scripts/ci_retry.sh test_capture python3 scripts/test_capture.py
step "live tool tests" scripts/ci_retry.sh test_live_tool python3 scripts/test_live_tool.py "$LIVE"
step "order bridge tests" scripts/ci_retry.sh test_bridge python3 scripts/test_bridge.py "$LIVE"
step "sandbox smoke test (local fake exchange)" scripts/ci_retry.sh test_smoke python3 scripts/test_smoke.py
step "fault drills" scripts/ci_retry.sh drill python3 scripts/drill.py --live "$LIVE"
[ "$fail" = 0 ] && echo "ci_local: all steps passed" || echo "ci_local: FAILURES"
exit "$fail"
