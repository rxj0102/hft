# ADR 11: A replace in flight reserves old leaves plus the new quantity

Status: Accepted

## Context
With max(leaves, new) a fill landing before the replace let position plus open exposure exceed the limit; the end-to-end test found it.

## Decision
While a replace is in flight the order reserves old leaves + new quantity, and the risk check needs headroom for the full new quantity.

## Consequences
The position limit holds under races. Costs: a replace temporarily uses more of the exposure budget than strictly necessary.

## Evidence
test_gw end-to-end exposure check after every step.
