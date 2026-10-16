# ADR 25: Order entry is a mirror in a separate process, not part of the hot path

Status: Accepted

## Context
Sending orders to a real venue means HTTPS, signing, rate limits and failures measured in seconds. None of that may sit on the tick-to-trade path, and the flatten logic (ADR 22) depends on a venue that answers in simulated microseconds.

## Decision
`hft_live` gets one optional hook (`LiveConfig::order_tap`) that sees every frame the gateway sends. The tool formats it into a fixed buffer inside the hook and writes it to an inherited pipe outside the hook, non-blocking; if the reader is gone or a megabyte behind, the pipe is closed and the event logged. A separate Python process (`scripts/order_bridge.py`) reads the pipe and reconciles the sandbox account towards the wanted orders: coalesced, cancels first, paced below the documented limit, guarded as in SANDBOX.md. It cancels everything for the product at start and at exit. The supervisor starts it as a third process and restarts the trio when any member dies, so a new bridge always begins with a cancel-all. Exit codes 2 and 4 from the bridge are not restarted.

The strategy keeps trading against the paper venue. Exchange reports are not fed back.

## Consequences
The decisions stay deterministic and replayable (the order flow is unchanged by the tap; the perf gate and the wire hashes are unaffected). The exchange lags the strategy by the rate limit and the network. Fills are not seen by the strategy. Closing the loop is later work and will need its own flatten semantics.

## Evidence
`scripts/test_bridge.py`, bridge drills in `scripts/drill.py` (against a fake exchange only), mutation check of the bridge.
