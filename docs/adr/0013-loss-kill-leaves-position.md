# ADR 13: The loss-limit kill cancels orders but does not flatten

Status: Accepted

## Context
Flattening is a trading decision with its own risk (crossing the spread in a bad market), not a safe default.

## Decision
The kill switch sweeps working orders and latches. The position is left and reported; the runbook says what a person does next.

## Consequences
No automated action that can make a bad day worse. Costs: exposure persists until a person acts.

## Evidence
test_bt loss-limit kill; RUNBOOK.
