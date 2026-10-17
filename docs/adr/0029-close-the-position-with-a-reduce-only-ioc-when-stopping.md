# ADR 29: Close the position with one reduce-only IOC order, only when stopping for good

Status: Accepted

## Context
A flatten cancels orders; it does not touch the position (ADR 13, ADR 26). With the loop closed (Phases 13 to 16) fills are real, so a stop (SIGTERM, the kill switch) can leave the sandbox account long or short. That is directional risk nobody is watching, and the old summary line "flat" said only that no order was working.

Closing is different from everything else the engine sends: it takes liquidity, it is priced from the engine's local book, and it must work in exactly the states in which trading is forbidden (kill switch latched, loss limit hit).

## Decision
- Opt-in: `hft_live --close-position` (needs `--exchange-fd`; `--close-collar-bps`, default 30, 1 to 1000). `supervise.py --loop --close-position` passes it on.
- When: only for a stop for good (reason `kill` or `shutdown`, including one that arrives while a routine end is already flattening), and only after the exchange has confirmed that no order is working. A reconnect, a window exit or a stale feed do not close, because their cause is that the book cannot be trusted.
- What: an immediate-or-cancel limit order against the position, priced the collar beyond the opposite touch of the local book (sell below the bid, buy above the ask, rounded away from the market on the tick grid), for the whole position (the strategy's own cap is 40 lots, far below `max_order_qty`; a position that exceeded it would be refused by the risk engine, and the close would fail loudly). If it fills only partly, the next order covers what is left, up to `close_max_attempts` (5) in all; each attempt gets the full flatten timeout.
- Risk: `RiskEngine::check_close` is a separate check that ignores the kill switch and the loss limit, keeps the sanity limits (size, notional, price band against a reference set from the book's mid, rate limit), and demands that the order strictly reduces the position with no other order working. A closing order can never flip or increase a position.
- Preconditions that stop it, each recorded in the segment summary (`close_why_not`) rather than guessed around: the last feed message is older than the stale limit, the book has no bid or no ask, the risk engine refuses (`close_risk_code`), the attempts are used up.
- Bridge: `type` 2 is carried out as `time_in_force: IOC`, only after the price is compared with the exchange's own ticker mid (`--ioc-band-bps`, default 100; no ticker, no order), and the notional cap still applies. Any type other than post-only and IOC is rejected. After the POST the bridge reads the order record and reports `accepted`, a `fill` at the executed price and `canceled` for the rest. If the record cannot be read it keeps the order for the sync, and if the exchange never says it reports nothing at all.
- Exit: if the last segment still holds a position the process prints `POSITION NOT CLOSED`, logs `position_not_closed` and exits 4 (not restartable). The supervisor reports `stop_not_flat` when it was asked to stop.

## Consequences
The engine now sometimes pays the spread plus up to the collar, and a loss-limit kill can realise a loss on the way out; both are intended. The case that would hurt is a wrong guess about a closing order's fate: a "cancelled" report for lots that really traded makes the engine sell again and go short. So an unknown outcome sends no report and the segment ends not closed, loudly.

Closing has to finish within the stale limit of the last feed message. When the supervisor stops, the capture stops too, so with the default 3 s limit there is room for about the first attempt and a few retries; in the drill with a 0.6 s limit a closing order that was never filled gave up after three attempts with "feed not fresh".

Limits: as first written, a position was tracked per segment, so a position left by a routine segment end or by an earlier process was not known to a later stop. ADR 30 closes that gap (the position is carried across segments and, with `--position-file`, across runs).

## Evidence
CLOSED_LOOP.md; `tests/test_live.cpp` (close tests), `tests/test_oms.cpp` (`check_close`), `scripts/test_bridge.py`, `scripts/test_live_tool.py` section 12, drills `loop_close`, `loop_close_no_liquidity`, `loop_close_off_market`; mutation run.
