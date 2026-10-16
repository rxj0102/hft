# ADR 20: The paper venue acknowledges and rejects crossing post-only orders but never fills

Status: Accepted

## Context
Level data has no information about our queue position, so any fill model on it would be invented.

## Decision
AckVenue accepts, replaces and cancels after a fixed round trip and rejects post-only orders that would cross the replayed book. Fills belong to the queue-aware backtester.

## Consequences
The OMS, gateway and strategy see a realistic order lifecycle without fabricated PnL. Costs: no fills means no inventory dynamics on real data.

## Evidence
test_l2 venue tests; every request answered (frames == venue frames, gateway reports == reports delivered).
