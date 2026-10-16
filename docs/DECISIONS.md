# Decision records

Each record: context, decision, consequences, evidence. Numbers refer to measurements in this repository (see LATENCY_BUDGET.md and DESIGN.md).

1. [Single-threaded, run-to-completion pipeline](adr/0001-single-thread-run-to-completion.md) (Accepted)
2. [Prices are int64 fixed point; no floating point on the path](adr/0002-fixed-point-prices.md) (Accepted)
3. [Flat price-level array with intrusive order lists and an id table](adr/0003-flat-book.md) (Accepted)
4. [Every latency measurement is gated on byte-exact replay](adr/0004-byte-exact-replay-oracle.md) (Accepted)
5. [Count parked packets instead of scanning the pending store](adr/0005-parked-counter.md) (Accepted)
6. [Reciprocal multiplication for the tick division](adr/0006-invariant-divisor.md) (Accepted)
7. [Book capacity is a latency parameter, sized per instrument](adr/0007-right-size-book.md) (Accepted)
8. [Rejected: grouped-by-4 Fibonacci id hash](adr/0008-rejected-grouped-hash.md) (Rejected)
9. [Gate CI on callgrind instructions per input, not wall time](adr/0009-instruction-count-gate.md) (Accepted)
10. [Feed loss pulls quotes; it does not trip the kill switch](adr/0010-feed-loss-is-not-kill.md) (Accepted)
11. [A replace in flight reserves old leaves plus the new quantity](adr/0011-replace-exposure.md) (Accepted)
12. [Backtester is passive-only with queue-position fills and no impact model](adr/0012-passive-only-queue-fills.md) (Accepted)
13. [The loss-limit kill cancels orders but does not flatten](adr/0013-loss-kill-leaves-position.md) (Accepted)
14. [Own log-linear histogram, no third-party dependencies in the core](adr/0014-own-histogram-no-deps.md) (Accepted)
