"""Compare two top of book logs written by hft_itch_check and
scripts/itch_reference_top.py.

    python scripts/compare_top.py A.csv B.csv

Consecutive rows with the same book fields are collapsed first (the two
programs count message indexes differently), then the sequences of
(ts, bid, bid_qty, ask, ask_qty) must be identical. Exit 1 at the first
difference, printed with its neighbours.
"""
import csv
import sys


def rows(path):
    out, last = [], None
    with open(path) as f:
        for r in csv.DictReader(f):
            book = (r["bid"], r["bid_qty"], r["ask"], r["ask_qty"])
            if book != last:
                out.append((r["ts"],) + book)
                last = book
    return out


def main(a_path, b_path):
    a, b = rows(a_path), rows(b_path)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"first difference at change {i}")
            for j in range(max(0, i - 2), min(i + 3, len(a), len(b))):
                print(f"  {a[j]}  |  {b[j]}")
            sys.exit(1)
    if len(a) != len(b):
        print(f"same prefix, different lengths: {len(a)} and {len(b)}")
        sys.exit(1)
    print(f"identical: {len(a)} top of book changes")


if __name__ == "__main__":
    main(*sys.argv[1:3])
