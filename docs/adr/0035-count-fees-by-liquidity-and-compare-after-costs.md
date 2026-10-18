# ADR 35: Count fees by liquidity and compare variants after costs

Status: Accepted

## Context
Until now fees were one flat rate in `pnl_report.py` and zero in `hft_lab`. A market maker lives on small edges, so a rate that looks small can flip a comparison: a variant that trades three times as often pays three times the fees. Exchanges also price the two sides differently (a maker may pay less, nothing, or be paid a rebate; a taker pays more), and the engine's one taker action is the closing immediate-or-cancel order.

## Decision
- The bridge marks each fill report with `liq`: `taker` if the order was an immediate-or-cancel order, otherwise `maker`. This is worked out from the order type we sent (a post-only order cannot take liquidity; the closing order is priced to cross). It is not read from the exchange.
- `hft_live` copies `liq` into the `fill` event only when the report carries `maker` or `taker`. A report without it gives an event without it; nothing is guessed.
- `pnl_report.py` takes `--maker-fee-bps` and `--taker-fee-bps` (negative maker rate is a rebate). A marked fill whose own rate was not given uses `--fee-bps`, and so does an unmarked fill. The report prints the effective rate (fees over volume) and a breakdown of fills, volume and fees by liquidity.
- `hft_lab` takes `--maker-fee-bps X` (range -100 to 1000). Fees are `volume * X / 1e4` per run, computed after the run. `pnl` in every table, ranking, score, paired comparison and the json is after fees; `edge` stays gross; `pnl = edge + inventory - fees` holds per run. The json gains `fees` per variant and `maker_fee_bps`. With no fee the output is unchanged. The run digest does not depend on the fee, because it describes the simulation.
- The lab has no taker rate: the strategy only sends post-only orders, so every simulated fill is a maker fill.

## Consequences
Variants and real fills are compared after costs with the same convention. The liquidity mark can be wrong if the exchange fills a post-only order as a taker (it should reject instead), or if the bridge's closing order rests; the bridge's own rules make both unlikely but this was not checked against the real sandbox. Fee rates are flat basis points of notional: tiers, minimums, and per-order fees are not modelled. In the lab a fee also does not change what the simulator does (a real fee schedule would change the best quote width, and the grid search sees that only if the fee is set).

The hot path is untouched.

## Rejected
- Reading `liquidity` from the exchange's fill message: the field name and presence in the sandbox were never seen, and the parse would need a test against a response format we cannot check.
- A taker rate in the lab: nothing in a lab run takes, so the option would be a number that does nothing.
- Folding the fee into the simulator (`BtConfig::fee_raw_per_unit`): the post-hoc calculation is exact for a flat rate and keeps the digest and the existing accounting intact.

## Evidence
`scripts/test_pnl.py` (rates by kind, rebate, fallbacks, unmarked and unknown marks, breakdown, flags), `scripts/test_bridge.py` (maker on a resting fill, taker on both IOC fills, no mark on other reports), `scripts/test_live_tool.py` (mark copied, absent when absent, closing fill taker), `tests/test_lab.cpp` and `scripts/test_lab.py` (fee arithmetic, rebate is the negative, identity per run, edge gross, digest unchanged, ranges, table and json), mutation run.
