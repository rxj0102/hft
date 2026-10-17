#!/usr/bin/env bash
# Runs a command; if it fails, runs it once more. A pass on the second try is not silent: it prints a warning
# (a GitHub annotation when run by Actions), because a test that needs a retry is a flaky test and someone should know.
# Use it only for tests that depend on real time (processes, pipes, sockets), never for the deterministic C++ suites.
# usage: scripts/ci_retry.sh NAME COMMAND [ARGS...]
name="$1"; shift
if "$@"; then exit 0; fi
echo "ci_retry: $name failed, running it once more" >&2
if "$@"; then
  if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning title=flaky test::$name passed only on the second try"; fi
  echo "ci_retry: $name PASSED ONLY ON THE SECOND TRY" >&2
  exit 0
fi
echo "ci_retry: $name failed twice" >&2
exit 1
