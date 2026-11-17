"""Independent top of book reconstruction from an ITCH 5.0 file, for one stock.

    python scripts/itch_reference_top.py FILE SYMBOL OUT.csv

Written separately from the C++ book and tools, with Python's struct
module and plain dictionaries, from the Nasdaq TotalView-ITCH 5.0
specification. It writes the same columns as `hft_itch_check`
(ts, index, bid, bid_qty, ask, ask_qty) whenever the best bid or ask, or the
displayed size at either, changes. Only ts and the four book fields are meant
to be compared: the index counts messages differently in the two programs.

It also prints counts of each message type for the stock and the last
closing cross (Q) price, so the reconstruction can be tied to the official
close.
"""
import heapq
import struct
import sys
from collections import Counter


def ts48(b):
    return int.from_bytes(b, "big")


def main(path, symbol, out_path):
    data = open(path, "rb").read()
    n = len(data)
    pos = 0
    locate = None
    orders = {}                     # ref -> [side, price, shares]
    levels = {"B": {}, "S": {}}     # side -> price -> shares
    heaps = {"B": [], "S": []}      # lazy heaps of prices: bids negated
    counts = Counter()
    cross = []
    last = None
    out = open(out_path, "w")
    out.write("ts,index,bid,bid_qty,ask,ask_qty\n")
    idx = 0

    def best(side):
        h, lv = heaps[side], levels[side]
        while h:
            p = -h[0] if side == "B" else h[0]
            if lv.get(p, 0) > 0:
                return p, lv[p]
            heapq.heappop(h)
        return None, None

    def add(ref, side, price, shares):
        orders[ref] = [side, price, shares]
        lv = levels[side]
        if lv.get(price, 0) == 0:
            heapq.heappush(heaps[side], -price if side == "B" else price)
        lv[price] = lv.get(price, 0) + shares

    def reduce(ref, shares):
        o = orders[ref]
        o[2] -= shares
        levels[o[0]][o[1]] -= shares
        if levels[o[0]][o[1]] == 0:
            del levels[o[0]][o[1]]
        if o[2] == 0:
            del orders[ref]

    while pos < n:
        length = int.from_bytes(data[pos:pos + 2], "big")
        m = data[pos + 2:pos + 2 + length]
        pos += 2 + length
        t = chr(m[0])
        if t == "R":
            if m[11:19].decode().rstrip() == symbol:
                locate = struct.unpack(">H", m[1:3])[0]
            continue
        if locate is None or length < 3 or struct.unpack(">H", m[1:3])[0] != locate:
            continue
        counts[t] += 1
        ts = ts48(m[5:11])
        if t in "AF":
            ref, side, shares, _stock, price = struct.unpack(">Qc I 8s I".replace(" ", ""), m[11:36])
            add(ref, side.decode(), price, shares)
        elif t == "E":
            ref, shares = struct.unpack(">QI", m[11:23])
            reduce(ref, shares)
        elif t == "C":
            ref, shares = struct.unpack(">QI", m[11:23])
            reduce(ref, shares)
        elif t == "X":
            ref, shares = struct.unpack(">QI", m[11:23])
            reduce(ref, shares)
        elif t == "D":
            ref = struct.unpack(">Q", m[11:19])[0]
            o = orders[ref]
            reduce(ref, o[2])
        elif t == "U":
            old, new, shares, price = struct.unpack(">QQII", m[11:35])
            side = orders[old][0]
            reduce(old, orders[old][2])
            add(new, side, price, shares)
        elif t == "Q":
            shares, _stock, price, _match, cross_type = struct.unpack(">Q8sIQc", m[11:40])
            cross.append((ts, chr(cross_type[0]), price, shares))
            continue
        else:
            continue
        idx += 1
        bp, bq = best("B")
        ap, aq = best("S")
        top = (bp, bq, ap, aq)
        if top != last:
            f = lambda x: "" if x is None else str(x)
            out.write(f"{ts},{idx},{f(bp)},{f(bq)},{f(ap)},{f(aq)}\n")
            last = top
    out.close()

    print(f"symbol {symbol} locate {locate}")
    print("messages " + " ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print(f"resting orders at end {len(orders)}")
    for ts, ctype, price, shares in cross:
        print(f"cross type {ctype} at {ts / 3.6e12:.4f} h: price {price / 1e4:.4f}, {shares} shares")


if __name__ == "__main__":
    main(*sys.argv[1:4])
