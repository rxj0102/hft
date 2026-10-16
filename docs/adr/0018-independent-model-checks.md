# ADR 18: Check the pipeline's book against an independent model, then against the exchange

Status: Accepted

## Context
A self-consistency test cannot show that the feed was read correctly; an exchange comparison alone cannot localise a bug.

## Decision
Two layers. The converter's own level map is compared with the pipeline's book at checkpoints and at the end. A snapshot from a second connection is compared with the book rebuilt as of the same moment.

## Consequences
The first layer finds converter, codec and book bugs; the second would find a wrong reading of the update messages. Costs: the exchange check is approximate by network skew (20/20, 18/20, 20/20 on live runs).

## Evidence
Tests that tamper with each model field and require the checker to fail; a 0 of 20 control against a snapshot from a different time.
