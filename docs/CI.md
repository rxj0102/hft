# Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` or `master`, every pull request, and by hand (`workflow_dispatch`). It has run on GitHub (Phases 16 to 18); the logs found two faults that local runs had not (see CHANGELOG, Phase 18). Check the Actions tab for the current state: this file does not say whether the latest run is green.

## Jobs

| Job | What it does | Fails when |
|---|---|---|
| `build-test` (6 cells: gcc-13 and clang-18, each with `release`, `asan`, `ubsan`) | Builds with warnings as errors and runs `ctest`. The long "gate" tests (millions of events) run only in the `release` cells | a warning, a test, a sanitizer report |
| `build-test`, gcc `release` cell only | Python half: syntax check of `scripts/*.py`, then `test_capture.py`, `test_live_tool.py`, `test_bridge.py`, `test_smoke.py` and `drill.py` against local fakes (no network, no key). Python 3.13 and `websockets` from `scripts/requirements.txt`, the versions they were written with | a failed check; the timing-dependent ones get one retry |
| `tsan` | `ctest` under ThreadSanitizer | a data race |
| `fuzz-smoke` | 45 seconds on each of the 9 fuzz targets, starting from the seeds in `fuzz/seeds` | a crash, a sanitizer report, a determinism or oracle trap |
| `perf` | The instruction-count gate against the baseline (`perf_gate.py --strict`, valgrind: deterministic), then informational latency, flame graph and cache reports as an artifact | the gate: more than the allowed growth in instructions per input |

## Reading a red run

- **`build-test`, one cell red, the others green.** A compiler difference (usually a warning that is an error). Reproduce with `CC=clang-18 CXX=clang++-18 cmake --preset <preset> && cmake --build --preset <preset>`.
- **A sanitizer report.** The first stack in the log is the bug; the `asan` and `ubsan` cells run the same tests as `release` at default sizes.
- **Python step red.** The step name says which suite. Run `scripts/ci_local.sh` after building `build/release/hft_live` to reproduce them all. The drills and bridge tests depend on real time: a step that passes on its second try prints `PASSED ONLY ON THE SECOND TRY` and raises a *flaky test* warning annotation. Treat that warning as a bug report; it is not noise to ignore.
- **`perf` red.** The table in the log shows which counter grew. Never raise the baseline to make it pass without a note in the commit that says what was traded. The wall-clock report is informational only.
- **`fuzz-smoke` red.** The log has a `crash-...` file name and the input; the artifact is not uploaded, so rerun the target locally with the same seed corpus. Fuzz inputs that find a bug get a regression test, not just a fix.

## What CI does not tell you

- Anything about the real exchange or the real sandbox. The fake exchange checks what the documented API says; the smoke test (`scripts/smoke_sandbox.py`) is the only thing that touches the sandbox, and only when you run it with your own key. CI never has a key.
- Latency. The runner's wall clock is noise; only instruction counts are gated.
- Hours-long runs. Fuzzing is 45 seconds per target.
- Whether the retry hides a real race: a test that needs it is listed in the run's annotations.

## Local equivalents

```
cmake --preset release && cmake --build --preset release && ctest --test-dir build/release
python3 -m pip install -r scripts/requirements.txt
scripts/ci_local.sh
cmake --preset fuzz && cmake --build --preset fuzz     # clang only
python3 scripts/perf_gate.py --bench build/perf/bench/bench_t2t --strict   # after the perf preset build
```
