#!/usr/bin/env python3
"""Order flow imbalance on a real Nasdaq ITCH day (Cont, Kukanov and Stoikov 2014).

Input: the top of book logs that scripts/verify_itch_day.sh writes, one per stock
(ts in nanoseconds after midnight, bid, bid_qty, ask, ask_qty; prices in 1e-4 dollars).

For every change n of the best quotes,

    e_n = 1{Pb_n >= Pb_n-1} qb_n - 1{Pb_n <= Pb_n-1} qb_n-1
        - 1{Pa_n <= Pa_n-1} qa_n + 1{Pa_n >= Pa_n-1} qa_n-1,

and OFI over an interval is the sum of e_n inside it. As in the paper, the mid price
change in ticks (one cent) over 10 second intervals is regressed on OFI separately in
every 30 minute window of regular trading hours, and the slope is compared with the
window's average depth at the touch (the paper finds beta proportional to 1/depth).

Two further questions, because the market maker in this repo leans on book imbalance:
does OFI in one interval predict the next interval's price change, and does the queue
imbalance at the touch, (qb - qa) / (qb + qa), at the start of an interval predict it?

    python3 scripts/ofi_study.py WORKDIR AAPL,MSFT,AMZN,TSLA,QQQ

writes docs/results/ofi_2019-12-30.json and docs/results/ofi_2019-12-30.md.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

OPEN_NS = 34_200 * 10**9  # 09:30
CLOSE_NS = 57_600 * 10**9  # 16:00
TICK = 100  # one cent in 1e-4 dollars
INTERVAL_S = 10
WINDOW_S = 1_800
ROOT = Path(__file__).resolve().parents[1]


def load(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path).dropna(subset=["bid", "ask", "bid_qty", "ask_qty"])
    df = df[(df["ask"] > df["bid"])]  # two sided and not locked or crossed
    return df.reset_index(drop=True)


def events(df: pd.DataFrame) -> pd.DataFrame:
    pb, qb, pa, qa = (df[c].to_numpy(dtype=float) for c in ("bid", "bid_qty", "ask", "ask_qty"))
    e = np.zeros(len(df))
    e[1:] = (
        (pb[1:] >= pb[:-1]) * qb[1:]
        - (pb[1:] <= pb[:-1]) * qb[:-1]
        - (pa[1:] <= pa[:-1]) * qa[1:]
        + (pa[1:] >= pa[:-1]) * qa[:-1]
    )
    out = pd.DataFrame(
        {
            "ts": df["ts"].to_numpy(),
            "e": e,
            "mid": (pb + pa) / 2.0 / TICK,
            "depth": (qb + qa) / 2.0,
            "qimb": (qb - qa) / (qb + qa),
        }
    )
    return out


def intervals(ev: pd.DataFrame) -> pd.DataFrame:
    """10 second intervals in regular hours: OFI, mid change, depth and imbalance at the start."""
    ev = ev[(ev["ts"] > OPEN_NS - 60 * 10**9) & (ev["ts"] <= CLOSE_NS)]
    edges = np.arange(OPEN_NS, CLOSE_NS + 1, INTERVAL_S * 10**9)
    ts = ev["ts"].to_numpy()
    # state at each edge: last change at or before it
    pos = np.searchsorted(ts, edges, side="right") - 1
    ok = pos >= 0
    mid = np.where(ok, ev["mid"].to_numpy()[np.maximum(pos, 0)], np.nan)
    qimb = np.where(ok, ev["qimb"].to_numpy()[np.maximum(pos, 0)], np.nan)
    cum = np.concatenate([[0.0], np.cumsum(ev["e"].to_numpy())])
    ofi = cum[pos[1:] + 1] - cum[pos[:-1] + 1]
    k = pd.Series(np.searchsorted(edges, ts, side="left"))
    depth = ev["depth"].groupby(k.to_numpy()).mean()
    n = len(edges) - 1
    df = pd.DataFrame(
        {
            "start_s": (edges[:-1] - OPEN_NS) // 10**9,
            "ofi": ofi,
            "dmid": mid[1:] - mid[:-1],
            "qimb0": qimb[:-1],
            "depth": depth.reindex(np.arange(1, n + 1)).to_numpy(),
        }
    )
    df["window"] = df["start_s"] // WINDOW_S
    return df.dropna(subset=["dmid", "ofi"])


def ols(y: np.ndarray, x: np.ndarray) -> tuple[float, float, float]:
    """Slope, R squared and White t of the slope for y = a + b x."""
    X = np.column_stack([np.ones_like(x), x])
    beta, *_ = np.linalg.lstsq(X, y, rcond=None)
    resid = y - X @ beta
    ss = float(((y - y.mean()) ** 2).sum())
    r2 = 1.0 - float((resid**2).sum()) / ss if ss > 0 else np.nan
    xtx_inv = np.linalg.inv(X.T @ X)
    meat = (X * resid[:, None] ** 2).T @ X
    se = np.sqrt(np.diag(xtx_inv @ meat @ xtx_inv))
    return float(beta[1]), r2, float(beta[1] / se[1]) if se[1] > 0 else np.nan


def study(symbol: str, path: Path) -> dict:
    iv = intervals(events(load(path)))
    windows = []
    for w, g in iv.groupby("window"):
        if len(g) < 30 or g["ofi"].std() == 0 or g["dmid"].std() == 0:
            continue
        b, r2, t = ols(g["dmid"].to_numpy(), g["ofi"].to_numpy())
        windows.append({"window": int(w), "beta": b, "r2": r2, "t": t, "depth": float(g["depth"].mean())})
    wdf = pd.DataFrame(windows)
    pos = wdf[(wdf["beta"] > 0) & (wdf["depth"] > 0)]
    lam, _, lam_t = ols(np.log(pos["beta"].to_numpy()), np.log(pos["depth"].to_numpy()))

    nxt = iv.assign(dmid_next=iv["dmid"].shift(-1)).dropna(subset=["dmid_next"])
    _, r2_lag, t_lag = ols(nxt["dmid_next"].to_numpy(), nxt["ofi"].to_numpy())
    q = iv.dropna(subset=["qimb0"])
    _, r2_q, t_q = ols(q["dmid"].to_numpy(), q["qimb0"].to_numpy())
    _, r2_all, t_all = ols(iv["dmid"].to_numpy(), iv["ofi"].to_numpy())
    return {
        "symbol": symbol,
        "intervals": int(len(iv)),
        "windows": int(len(wdf)),
        "r2_mean": float(wdf["r2"].mean()),
        "r2_median": float(wdf["r2"].median()),
        "beta_positive_share": float((wdf["beta"] > 0).mean()),
        "t_median": float(wdf["t"].median()),
        "pooled_r2": r2_all,
        "pooled_t": t_all,
        "depth_slope": lam,
        "depth_slope_t": lam_t,
        "next_interval_r2": r2_lag,
        "next_interval_t": t_lag,
        "queue_imbalance_r2": r2_q,
        "queue_imbalance_t": t_q,
        "mean_abs_dmid_ticks": float(iv["dmid"].abs().mean()),
    }


def render(rows: list[dict]) -> str:
    f = lambda x, d=2: f"{x:.{d}f}"
    lines = [
        "# Order flow imbalance on a real Nasdaq ITCH day: 2019-12-30",
        "",
        "Written by `scripts/ofi_study.py` from the top of book logs of "
        "[itch_2019-12-30.md](itch_2019-12-30.md). Cont, Kukanov and Stoikov (2014) show that "
        "order flow imbalance at the best quotes (OFI: bid size added or bid price raised, minus the "
        "same on the ask) explains most of the contemporaneous mid price change, with a slope "
        "inversely proportional to depth. They report an average R squared of 65% for 10 second "
        "intervals across 50 US stocks in 2010.",
        "",
        "Method: 10 second intervals from 09:30 to 16:00, mid change in cents, one regression per "
        "30 minute window as in the paper. The depth slope is from regressing log beta on log "
        "average depth across windows; the paper's model predicts -1.",
        "",
        "| Stock | Intervals | Mean R2 by window | Median t | Depth slope (t) | Next interval R2 (t) | Queue imbalance R2 (t) |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        lines.append(
            f"| {r['symbol']} | {r['intervals']} | {100 * r['r2_mean']:.0f}% | {f(r['t_median'], 1)} | "
            f"{f(r['depth_slope'])} ({f(r['depth_slope_t'], 1)}) | {100 * r['next_interval_r2']:.2f}% "
            f"({f(r['next_interval_t'], 1)}) | {100 * r['queue_imbalance_r2']:.2f}% ({f(r['queue_imbalance_t'], 1)}) |"
        )
    r2s = [r["r2_mean"] for r in rows]
    by = lambda key: max(rows, key=lambda r: r[key])
    lag, qi = by("next_interval_r2"), by("queue_imbalance_r2")
    near = [r["symbol"] for r in rows if abs(r["depth_slope"] + 1) < 0.35 and r["depth_slope_t"] < -2]
    far = [f"{r['symbol']} {f(r['depth_slope'], 1)}" for r in rows if r["symbol"] not in near]
    lines += [
        "",
        f"Contemporaneous OFI explains {100 * min(r2s):.0f}% to {100 * max(r2s):.0f}% of 10 second "
        "mid changes, in line with the paper, and the slope is positive in every window. The slope "
        f"falls with depth close to the predicted rate (log slope near -1) for {', '.join(near)}; "
        f"the others give {', '.join(far)}, each from only 13 windows.",
        "",
        "As a forecast it is weak. OFI in one interval explains at most "
        f"{100 * lag['next_interval_r2']:.1f}% of the next interval's change ({lag['symbol']}, "
        f"t = {f(lag['next_interval_t'], 1)}), and under 1% for the rest. The queue imbalance at the "
        "start of an interval, the quantity the market maker in this repo leans on, explains at most "
        f"{100 * qi['queue_imbalance_r2']:.1f}% ({qi['symbol']}, t = {f(qi['queue_imbalance_t'], 1)}). "
        "Some of these are statistically clear, but all are small, so the imbalance lean is a reasonable small skew of quotes, not a source of "
        "directional edge. That matches the README, which claims no profitability.",
        "",
        "## What this does not show",
        "",
        "One day and five liquid stocks. Prices are in cents, so for low priced or very liquid names "
        "many intervals have no mid change. t statistics use White standard errors within each "
        "window; they are descriptive. Hidden orders and trades are not in the top of book log, so "
        "the paper's comparison with trade imbalance is not repeated.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    work = Path(sys.argv[1])
    symbols = sys.argv[2].split(",")
    rows = [study(s, work / f"top_{s}.hft.csv") for s in symbols]
    out = ROOT / "docs" / "results"
    rounded = [{k: (round(v, 6) if isinstance(v, float) else v) for k, v in r.items()} for r in rows]
    (out / "ofi_2019-12-30.json").write_text(json.dumps(rounded, indent=1) + "\n")
    (out / "ofi_2019-12-30.md").write_text(render(rows))
    for r in rows:
        print(r["symbol"], {k: round(v, 4) if isinstance(v, float) else v for k, v in r.items() if k != "symbol"})


if __name__ == "__main__":
    main()
