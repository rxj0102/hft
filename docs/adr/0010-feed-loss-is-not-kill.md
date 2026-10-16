# ADR 10: Feed loss pulls quotes; it does not trip the kill switch

Status: Accepted

## Context
Losing market data is routine and recoverable; the kill switch is latched and needs a person.

## Decision
On an untrusted book `Pipeline::decide` calls `Gateway::cancel_all` (counted as `pulled_cancels`) and returns before the strategy runs. Quoting resumes by itself after a snapshot.

## Consequences
The engine never quotes on a book it cannot trust, and does not need a human for a dropped packet. Costs: a flapping feed means flapping quotes; the counters (`feed_losses`, `pull_sweeps`) are how to see it.

## Evidence
`feed_loss_pulls_quotes_and_recovers`; fuzz_pipeline oracle.
