# ADR 31: Check the position file against the exchange account before trading

Status: Accepted

## Context
ADR 30 lets a restarted process learn its position from a file, and says that the file is trusted as written. It can be stale (kill -9 between a fill and the next loop pass), shared by two processes, or wrong because someone traded the account by hand. Starting to trade, and later to close, on a wrong position is worse than not trading.

## Decision
- The loop bridge reads `GET /accounts` once at start and, after the hello (which carries `qty_scale`), writes one line to the report pipe: `{"t":"acct","ok":1,"base_raw":N}` with the base currency's total balance in lots (1e-5), or `{"t":"acct","ok":0}` if the list could not be read or has no entry for the base currency. Logged as the `account` event.
- `hft_live --position-file` keeps a third number in the file: the baseline, the balance minus the position (`<lots> <cash> <baseline>`). Until the account report has been looked at, `hft_live` does not read its input at all (no snapshot, no segment, no order) and writes nothing to the file.
  - Baseline known: the balance must equal baseline + position within one lot; otherwise `ACCOUNT CHECK FAILED`, log event `account_mismatch`, exit 4 (the supervisor does not restart it), the file is left alone.
  - No baseline (first run, or a file from ADR 30): the baseline is set from the balance and the position in the file, printed to stderr, and written with the first save.
  - `ok:0`, or no report within 10 s (an old bridge, a dead one): refused the same way. Fail closed.
- Without `--position-file` the report is ignored. A mirror (non-loop) bridge does not read the account.
- To start again after a mismatch the operator checks the account and deletes the file.
- `smoke_sandbox.py` step 5 `account` checks that `/accounts` answers with an entry for the base currency and a numeric balance, and the bridge step requires the account report.

## Consequences
The check catches a stale or foreign file, hand trades, and a bridge that missed fills while the engine was down (the exchange balance moved, the file did not). It cannot tell which of the two is right, only that they disagree, so a person decides. Deposits, withdrawals and fees charged in the base currency also move the balance and will trip it; this sandbox account is meant to be used for nothing else. Dust below half a lot is rounded away; a difference of one lot is tolerated. The balance is read before the first order of the run, so fills during the run cannot cause a false alarm.

Untested against the real sandbox: the shape of `GET /accounts` (a list of entries with `currency` and `balance`) is written from the documentation; smoke step 5 is the first check.

## Evidence
`scripts/test_live_tool.py` section 13 (refused and tolerated balances, unreadable, missing report, old file, no position file), `scripts/test_bridge.py::test_account`, `scripts/test_smoke.py`, drill `loop_account_mismatch`; mutation run.
