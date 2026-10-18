#!/usr/bin/env python3
"""PnL and fill-quality report from hft_live's event log (ADR 32).

    hft_live ... --log events.jsonl --mid-sample-ms 100 ...        (scripts/supervise.py --loop does this by default)
    python3 scripts/pnl_report.py events.jsonl [more.jsonl ...] [--fee-bps 0] [--maker-fee-bps X] [--taker-fee-bps Y]
                                  [--horizons 1,5,30] [--json out.json]

Reads two kinds of events:
  fill  {"ts_ms", "side", "px", "qty", "mid_x2", "pos", "applied", "liq"}   one per fill the exchange reported; liq is "maker" or
        "taker" when the bridge could tell (from the order type: post-only quotes add liquidity, the closing IOC takes it)
  mid   {"ts_ms", "mid_x2"}                                          the local book's mid, sampled when it changes

Prices are raw (1e-4 quote units), quantities raw (1e-5 base units), mid_x2 is bid + ask (twice the mid, no rounding).

What it computes, and what it does not:
  * PnL by average cost, with the inventory the run started with valued at the mid of its first fill (the cost of
    carried inventory is not known here), marked at the last mid sample. A second figure from the cash flows must agree.
  * Edge: how far each fill was from the mid when it happened (positive: bought below / sold above the mid).
  * Markout: how the mid moved against the fill afterwards, at each horizon (positive: the market moved the fill's
    way; negative: adverse selection). Needs mid samples that reach past the horizon.
  * Inventory: largest and time-weighted mean absolute position.
  * Consistency: the position after each fill, as the engine reported it, against the sum of the fills.
Fees: none unless a rate is given. --maker-fee-bps and --taker-fee-bps apply to fills marked maker or taker (a negative maker rate is
a rebate); --fee-bps applies to every fill that is not marked, and to marked fills whose own rate was not given. Rates are in basis
points of notional. The report says how many fills and how much volume each rate was applied to.
Not included: funding, anything the engine did not log. A mid from the local book is not the exchange's mid when the feed lags.
"""
import argparse
import bisect
import json
import sys


def load(paths):
    fills, mids, last_ts, bad = [], [], 0, 0
    for p in paths:
        with open(p) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    ts = int(e["ts_ms"])
                except (ValueError, KeyError, TypeError):
                    bad += 1
                    continue
                last_ts = max(last_ts, ts)
                ev = e.get("event")
                try:
                    if ev == "fill" and e.get("applied") == 1:
                        side = e["side"]
                        if side not in ("buy", "sell"):
                            raise ValueError
                        liq = e.get("liq") if e.get("liq") in ("maker", "taker") else None
                        fills.append({"ts": ts, "sign": 1 if side == "buy" else -1, "px": int(e["px"]), "qty": int(e["qty"]),
                                      "mid_x2": int(e.get("mid_x2", -1)), "pos": int(e["pos"]), "liq": liq})
                        if fills[-1]["mid_x2"] > 0:    # in file order, so that of two samples in one millisecond the later line wins
                            mids.append((ts, fills[-1]["mid_x2"]))
                    elif ev == "mid" and int(e["mid_x2"]) > 0:
                        mids.append((ts, int(e["mid_x2"])))
                except (KeyError, ValueError, TypeError):
                    bad += 1
    fills.sort(key=lambda x: x["ts"])
    mids.sort(key=lambda m: m[0])     # stable: samples with the same timestamp keep the order they were logged in
    return fills, mids, last_ts, bad


def analyse(fills, mids, last_ts, px_scale, qty_scale, fee_bps, horizons, maker_bps=None, taker_bps=None):
    r = {"fills": len(fills)}
    if not fills:
        return r
    mid_ts = [m[0] for m in mids]

    def mid_at(ts):
        i = bisect.bisect_right(mid_ts, ts) - 1
        return mids[i][1] / 2.0 / px_scale if i >= 0 else None

    # position the run started with, from the first fill's own report
    f0 = fills[0]
    pos0_lots = f0["pos"] - f0["sign"] * f0["qty"]
    pos = pos0_lots / qty_scale
    first_mid = f0["mid_x2"] / 2.0 / px_scale if f0["mid_x2"] > 0 else f0["px"] / px_scale
    avg = first_mid
    realised = 0.0
    cash = 0.0
    volume = 0.0
    fees = 0.0
    by_liq = {k: {"n": 0, "volume": 0.0, "fees": 0.0, "bps": None} for k in ("maker", "taker", "unmarked")}
    rate = {"maker": fee_bps if maker_bps is None else maker_bps, "taker": fee_bps if taker_bps is None else taker_bps, "unmarked": fee_bps}
    edge_sum = 0.0            # quote units
    edge_bps_w = 0.0          # notional-weighted
    edge_n = 0
    gaps = 0
    run_lots = pos0_lots
    buys = sells = 0
    inv_area, prev_ts, prev_pos = 0.0, f0["ts"], abs(pos)
    max_abs = abs(pos)
    marks = {h: {"n": 0, "sum_bps": 0.0, "w_bps": 0.0, "w": 0.0, "neg": 0} for h in horizons}
    for f in fills:
        px = f["px"] / px_scale
        q = f["qty"] / qty_scale
        s = f["sign"]
        notional = px * q
        volume += notional
        kind = f["liq"] or "unmarked"
        fee = notional * rate[kind] / 1e4
        fees += fee
        by_liq[kind]["n"] += 1
        by_liq[kind]["volume"] += notional
        by_liq[kind]["fees"] += fee
        by_liq[kind]["bps"] = rate[kind]
        cash -= s * notional
        buys += s > 0
        sells += s < 0
        # time-weighted inventory
        inv_area += prev_pos * (f["ts"] - prev_ts)
        prev_ts = f["ts"]
        # average cost
        if pos == 0 or (pos > 0) == (s > 0):
            avg = (avg * abs(pos) + px * q) / (abs(pos) + q)
            pos += s * q
        else:
            closing = min(q, abs(pos))
            realised += (1 if pos > 0 else -1) * (px - avg) * closing
            rest = q - closing
            pos += s * closing
            if rest > 1e-12:
                avg = px
                pos += s * rest
        prev_pos = abs(pos)
        max_abs = max(max_abs, prev_pos)
        run_lots += s * f["qty"]
        if run_lots != f["pos"]:
            gaps += 1
        # edge against the mid at the moment of the fill
        if f["mid_x2"] > 0:
            m = f["mid_x2"] / 2.0 / px_scale
            e = s * (m - px) * q
            edge_sum += e
            edge_bps_w += s * (m - px) / m * 1e4 * notional
            edge_n += 1
        # markouts
        for h in horizons:
            t = f["ts"] + int(h * 1000)
            if last_ts < t or not mids:   # the log must reach the horizon; a mid sample is only logged when it changes, so the last one still holds
                continue
            mh = mid_at(t)
            if mh is None:
                continue
            bps = s * (mh - px) / px * 1e4
            d = marks[h]
            d["n"] += 1
            d["sum_bps"] += bps
            d["w_bps"] += bps * notional
            d["w"] += notional
            d["neg"] += bps < 0
    end_ts = max(last_ts, prev_ts)
    inv_area += prev_pos * (end_ts - prev_ts)
    span_ms = end_ts - f0["ts"]
    m_last = mids[-1][1] / 2.0 / px_scale if mids else fills[-1]["px"] / px_scale
    unreal = pos * (m_last - avg)
    total = realised + unreal - fees
    total_cash = cash + pos * m_last - (pos0_lots / qty_scale) * first_mid - fees
    r.update({
        "buys": buys, "sells": sells, "volume_quote": volume, "start_position_base": pos0_lots / qty_scale, "end_position_base": pos,
        "realised_quote": realised, "unrealised_quote": unreal, "fees_quote": fees, "fees_by_liquidity": by_liq, "total_quote": total, "total_from_cash_quote": total_cash,
        "cash_check_diff": total - total_cash, "last_mid": m_last,
        "edge_total_quote": edge_sum, "edge_fills": edge_n,
        "edge_bps_notional_weighted": (edge_bps_w / volume) if edge_n and volume else None,
        "max_abs_position_base": max_abs,
        "mean_abs_position_base": (inv_area / span_ms) if span_ms > 0 else abs(pos0_lots / qty_scale),
        "span_s": span_ms / 1000.0, "position_gaps": gaps,
        "markout": {("%g" % h): {"n": d["n"], "mean_bps": d["sum_bps"] / d["n"] if d["n"] else None,
                             "weighted_bps": d["w_bps"] / d["w"] if d["w"] else None,
                             "share_negative": d["neg"] / d["n"] if d["n"] else None} for h, d in marks.items()},
    })
    return r


def text(r):
    if r["fills"] == 0:
        return "no fills in the log: nothing to report"
    L = []
    L.append("== PnL and fill quality (%d fills over %.0f s)" % (r["fills"], r["span_s"]))
    L.append("  volume %.2f quote units in %d buys and %d sells" % (r["volume_quote"], r["buys"], r["sells"]))
    L.append("  position: started %+.5f, ended %+.5f, largest %.5f, time-weighted mean %.5f (base units)" % (
        r["start_position_base"], r["end_position_base"], r["max_abs_position_base"], r["mean_abs_position_base"]))
    L.append("  PnL (average cost, last mid %.2f): realised %+.4f, unrealised %+.4f, fees %.4f (%.2f bps), total %+.4f" % (
        r["last_mid"], r["realised_quote"], r["unrealised_quote"], r["fees_quote"], (1e4 * r["fees_quote"] / r["volume_quote"]) if r["volume_quote"] else 0.0, r["total_quote"]))
    parts = []
    for kind in ("maker", "taker", "unmarked"):
        d = r["fees_by_liquidity"][kind]
        if d["n"]:
            parts.append("%s %d fills, %.2f volume, %+.4f at %g bps" % (kind, d["n"], d["volume"], d["fees"], d["bps"]))
    if parts:
        L.append("  fees by liquidity: " + "; ".join(parts))
    L.append("  cross-check from cash flows: %+.4f (difference %.2e)" % (r["total_from_cash_quote"], r["cash_check_diff"]))
    if r["edge_fills"]:
        L.append("  edge against the mid at the fill: %+.4f in total, %+.2f bps notional-weighted (%d fills had a mid)" % (
            r["edge_total_quote"], r["edge_bps_notional_weighted"], r["edge_fills"]))
        L.append("  so of the total, %+.4f is edge and %+.4f is the market moving against or for the inventory" % (
            r["edge_total_quote"], r["total_quote"] - r["edge_total_quote"]))
    else:
        L.append("  edge: no fill had a mid (was --mid-sample-ms on, and was the book two-sided?)")
    for h, d in r["markout"].items():
        if d["n"]:
            L.append("  markout %ss: %+.2f bps mean, %+.2f bps notional-weighted, %.0f%% of %d fills went against us" % (
                h, d["mean_bps"], d["weighted_bps"], 100 * d["share_negative"], d["n"]))
        else:
            L.append("  markout %ss: no fill had a mid sample that far ahead" % h)
    if r["position_gaps"]:
        L.append("  WARNING: the engine's position disagreed with the sum of the fills after %d fills (missed or repeated fills?)" % r["position_gaps"])
    else:
        L.append("  consistency: the engine's position matched the sum of the fills after every fill")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--px-scale", type=int, default=10000)
    ap.add_argument("--qty-scale", type=int, default=100000)
    ap.add_argument("--fee-bps", type=float, default=0.0, help="rate on fills not marked maker or taker, and the fallback for a marked kind without its own rate (default 0: not modelled)")
    ap.add_argument("--maker-fee-bps", type=float, default=None, help="rate on fills marked maker; negative is a rebate")
    ap.add_argument("--taker-fee-bps", type=float, default=None, help="rate on fills marked taker")
    ap.add_argument("--horizons", default="1,5,30", help="markout horizons in seconds")
    ap.add_argument("--json", default=None)
    a = ap.parse_args(argv)
    try:
        horizons = [float(x) for x in a.horizons.split(",") if x]
    except ValueError:
        ap.error("--horizons wants numbers")
    fills, mids, last_ts, bad = load(a.logs)
    r = analyse(fills, mids, last_ts, a.px_scale, a.qty_scale, a.fee_bps, horizons, a.maker_fee_bps, a.taker_fee_bps)
    r["unreadable_lines"] = bad
    print(text(r))
    if bad:
        print("  (%d lines could not be read and were skipped)" % bad)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
