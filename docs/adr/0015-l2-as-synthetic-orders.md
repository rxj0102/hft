# ADR 15: One synthetic order per price level for aggregated feeds

Status: Accepted

## Context
The public feed is aggregated by price level; the engine's book, wire format and strategy path are order by order, and the wire has Add, Execute, Partial Cancel and Delete but no replace.

## Decision
Represent each live level by one synthetic order: Add on appearance, Reduce on shrink, Delete then Add on growth, Delete on zero. Ids are private to the converter.

## Consequences
The production book, codec and pipeline run unchanged on real data. Costs: growth costs two messages, and there is no queue position in the data, so nothing can be said about where our order would stand.

## Evidence
test_l2 converter semantics; 12 random captures replayed against an independent model; real captures match the model.
