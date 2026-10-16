# ADR 12: Backtester is passive-only with queue-position fills and no impact model

Status: Accepted

## Context
Honest fills for a market maker depend on queue position, not on price touching.

## Decision
L3 replay: each tape order carries an arrival number so the orders ahead of or behind a simulated order are known. Orders that would take liquidity are rejected. No market impact.

## Consequences
Validated against the real matching engine (never earlier or larger than the engine; pessimistic where a sweep ends exactly on our order). Costs: results are not valid for size, and returns on synthetic data are not predictions.

## Evidence
250 random injections versus the engine; test_bt.
