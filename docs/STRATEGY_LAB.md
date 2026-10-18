# Strategy lab

`hft_lab` runs market maker parameter variants over generated markets and compares them. It exists so that a change to the strategy (a wider quote, a different inventory skew, no imbalance lean) can be judged by a number with an error bar instead of by one lucky run. It needs no exchange and no key.

```
cmake --build --preset release --target hft_lab
./build/release/hft_lab --markets 8 --requests 200000 \
    --variant noalpha:alpha16=0 --variant wide:half_spread16=32 --sweep skew16=0,4
```

## What it does

- Variant 0 is always `base`, the default `MmConfig`. `--variant name:key=value,...` adds a variant; `--sweep key=v1,v2,...` adds one variant per value (named `key=value`), each changing only that parameter.
- Parameters: `half_spread16 alpha16 skew16 depth_levels qty max_pos requote_ticks min_leaves min_interval_ns`. Unknown keys, non-integers and out-of-range values are refused with exit 1.
- Every variant runs on every one of `--markets` generated markets (seeds `--seed` upwards), through the same queue-aware backtester as `bench/bt_report`: the production gateway, OMS and risk engine, the real wire format, a venue that replays a public event tape with an exact queue model, and configurable latency (`--md-us --ingress-us --egress-us --jitter-us`) and queue penalty (`--queue-penalty-pct`). The same market is used for all variants, so the comparison is paired.
- The first table gives, per variant, the mean over markets of PnL (with its standard error), edge (spread capture), inventory (what the position earned as the mid moved), fills, fill rate and the largest position. PnL is edge plus inventory, exactly.
- The second table gives variant minus base on the same market: mean difference with standard error, a t statistic, wins/losses/ties, an exact two-sided sign test, and the mean differences of edge and inventory.
- `--json FILE` writes every per-market number and per-run digest. `--events DIR` writes one event log per run (`<variant>_m<i>.jsonl`) in the format of `hft_live`, which `scripts/pnl_report.py` reads, with `--qty-scale 1` because the quantities are shares:

```
python3 scripts/pnl_report.py DIR/base_m0.jsonl --qty-scale 1 --horizons 0.001,0.01,0.1
```

The PnL, cash-flow PnL and edge from `pnl_report.py` equal the lab's own figures for every run (tested for 20 runs), so the report that will judge real fills has been checked against an independent computation of the same fills.

Exit codes: 0 ok, 1 usage, 2 setup failure (unwritable output, backtester init), 3 at least one run failed its health checks (OMS anomalies or mismatches, queue inconsistency, overflow, tape errors, dropped samples, kill switch, or did not finish). Results are still printed on 3, and must not be used.

## Reading the output

- Look at edge and inventory separately. A variant that wins on PnL because of the inventory column is carrying more position through a price drift, which is luck in the sense that a different drift would reverse it. In the example below, `skew16=0` (no inventory skew) beat the base by 7.5 on PnL, but 4.9 of that was inventory, and it held positions up to its cap of 40 against 12 for the base.
- The sign test needs no distribution but has little power with few markets: with n markets the smallest possible p is 2/2^n (0.031 for 6, 0.0078 for 8). The t statistic assumes the differences are roughly normal; with fewer than about 10 markets treat it as a rough guide.
- K variants are K comparisons, not adjusted for each other. Run more markets, or confirm a promising variant on fresh seeds (`--seed`), before believing it.
- The numbers are in the synthetic market's currency units over a few seconds of market time. They show how the strategy responds to its parameters in a market with informed flow and queue priority. They are not returns.

## An example, with what it says and does not say

`./build/release/hft_lab --markets 16 --requests 200000 --variant noalpha:alpha16=0 --variant wide:half_spread16=32 --sweep skew16=0,4`, run on 2026-10-17 (about 9 s on the VM):

```
variant                        PnL mean     +- se    edge mean   inventory  fills/mkt   fill%  max|pos|
base                             +17.93      0.38        +8.39       +9.54        549    6.3%        12
noalpha                           +1.14      0.14        +3.13       -1.99        200    3.4%        12
wide                              +0.66      0.14        +0.52       +0.14         30    0.5%         7
skew16=0                         +25.37      2.80       +10.94      +14.44        704    7.3%        40
skew16=4                         +13.10      0.27        +7.40       +5.71        510    6.7%        11

variant                       dPnL mean     +- se       t      W/L/T    sign p   dEdge mean    dInv mean
noalpha                          -16.78      0.35  -47.51     0/16/0   <0.0001        -5.26       -11.53
wide                             -17.27      0.43  -40.39     0/16/0   <0.0001        -7.87        -9.40
skew16=0                          +7.45      2.65   +2.81     11/5/0    0.2101        +2.55        +4.90
skew16=4                          -4.82      0.21  -23.10     0/16/0   <0.0001        -0.99        -3.81
```

- Dropping the imbalance lean (`noalpha`) or quoting wider (`wide`) lost about 17 on 16 of 16 markets. On these markets the imbalance signal is doing real work, which is by construction: the generator has informed flow that the imbalance reveals.
- Doubling the skew (`skew16=4`, the default is 2) lost about 5 on 16 of 16 markets.
- Removing the skew (`skew16=0`) gained 7.5 with t 2.8 but won only 11 of 16 markets (sign p 0.21). On 16 other markets (`--seed 5000`) it gained 8.7, won 14 of 16 (p 0.004), and the same tradeoff held: its largest position was 40 (the cap) against 11. The honest reading is a higher expected PnL bought with a much larger position, and the lab's PnL figure does not price that risk.
- The first and second sets of markets agree in direction and roughly in size, which is what a result that is not an accident of the seeds looks like. They are still generated markets.

## Parameter search with held-out markets

```
./build/release/hft_lab --markets 8 --requests 200000 \
    --grid "alpha16=0,8,16,24;skew16=0,1,2,4;half_spread16=16,20,28" --top 3 --risk-penalty 0
```

`--grid` runs every combination (up to 500) plus the base on the training markets (seeds `--seed` and up), ranks them by an objective, keeps the best `--top` (default 3), and then runs only those and the base on held-out markets (`--test-seed`, default seed + 100000, and `--test-markets`, default the training count; overlapping seed ranges are refused). Grid points that were not chosen are never run on the held-out markets, so the held-out numbers cannot be used to pick a different winner. See ADR 34.

The objective is mean PnL minus `--risk-penalty` times the mean largest absolute position, so a penalty above zero prefers points that earn their PnL with less inventory. For each chosen point the report gives the training gain and the held-out gain over the base in that objective, the fraction retained, and the average shrinkage (training gain minus held-out gain: the winner's curse). It also warns when a chosen value is the smallest or largest on its axis, because then the best value may lie outside the grid.

Two runs on 2026-10-17 (seeds 1000 to 1007 for training, 101000 to 101007 held out; 42 s each on the VM):

| Grid, penalty | Best point chosen | Train gain | Held-out gain | Retained | Largest position (train, held out) |
|---|---|---|---|---|---|
| alpha 0,8,16,24; skew 0,1,2,4; half spread 16,20,28; penalty 0 | alpha 24, skew 0, half spread 16 | +33.5 | +34.9 | 104% | 40, 40 (base 12, 14) |
| the same grid, third pick | alpha 16, skew 0, half spread 16 | +18.2 | +16.4 | 90% | 40 |
| alpha 8,16,24,32; skew 1,2,4,8; half spread 12,16,20,28; penalty 1 | alpha 32, skew 1, half spread 12 | +41.1 | +40.3 | 98% | 29, 32 |

What this says and does not say:

- The gains survive on fresh markets. The choice is not an accident of the training seeds, and the shrinkage is about zero or slightly negative (noise). The grids here are small relative to the effect sizes, so there is little winner's curse to see.
- Every chosen point sits at the edge of its grid (the largest alpha, the smallest half spread, often the smallest skew), and widening the grid moved the optimum with it: more lean, tighter quotes, more fills (3,000 per market against 550 for the base), more position. That is a property of the generator, which has informed flow that the book imbalance reveals, not a finding about markets.
- With a penalty of 1 the winner still held 29 to 32 lots. The penalty is a way to express a preference; it does not make a point safe.
- Held-out markets from the same generator test the choice against sampling noise, not against a different market. Retention near 100 percent would not be expected on real data.
- If you adjust the grid after seeing held-out results, use fresh seeds, because the old ones are spent.

## What it cannot tell you

- The fills are simulated. The queue model is exact for the tape it replays, but the tape does not react to the strategy's orders (no market impact), so a strategy that would move the market looks better than it is.
- All markets come from one generator (`MarketGen`). A parameter tuned on it may be tuned to the generator.
- Latency is a fixed hop time plus bounded jitter.
- Fees are zero unless `--maker-fee-bps X` is given (ADR 35). Every simulated fill is a maker fill because the strategy only sends post-only orders, so there is no taker rate. With a fee, PnL in the tables, the ranking and the json is after fees (`pnl = edge + inventory - fees`), and edge stays gross. Example (4 markets of 20,000 requests, `--sweep requote_ticks=1,2`): gross, requote_ticks=2 beat the base, +2.38 against +1.79; at 1 bp it traded 157 fills a market against 46 and paid 3.13 against 0.87, so it lost, -0.74 against +0.92. Flat basis points of notional only: no tiers or per-order fees.
- Event logs have millisecond resolution and use the venue's mid, not a delayed strategy view.

## A finding: a requote loop

`requote_ticks=0` together with `min_interval_ns=0` makes the market maker answer every replace acknowledgement with another replace, forever. In the backtester that was a hang. `Backtester::run` now takes a step budget, the lab uses one (20 steps per tape event plus a million), and such a variant is reported as a failed run (exit 3, "did not finish"). Either setting alone is fine. In production the risk engine's rate limit would bound the message rate, but the strategy would still churn; the defaults (`requote_ticks=1`, `min_interval_ns=50000`) are not affected and the strategy code is unchanged.
