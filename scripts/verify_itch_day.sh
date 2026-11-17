#!/bin/sh
# Rebuild the book of a few stocks from a real Nasdaq TotalView-ITCH 5.0 day
# with the project's order book, and compare the top of book after every
# change with an independent Python reconstruction of the same messages.
#
#   scripts/verify_itch_day.sh 12302019.NASDAQ_ITCH50.gz AAPL,MSFT,AMZN,TSLA,QQQ [WORKDIR]
#
# Day files come from https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (several GB,
# not committed). Needs a release build:
#   cmake --preset release && cmake --build --preset release
set -eu
GZ=$1
SYMBOLS=$2
WORK=${3:-itch_work}
B=build/release
mkdir -p "$WORK"
gzip -dc "$GZ" | $B/itch_extract --symbols "$SYMBOLS" --out "$WORK/day.bin" > "$WORK/extract.txt"
head -1 "$WORK/extract.txt"
for s in $(echo "$SYMBOLS" | tr ',' ' '); do
  echo "== $s"
  $B/hft_itch_check "$WORK/day.bin" "$s" "$WORK/top_$s.hft.csv"
  python3 scripts/itch_reference_top.py "$WORK/day.bin" "$s" "$WORK/top_$s.ref.csv" > /dev/null
  python3 scripts/compare_top.py "$WORK/top_$s.hft.csv" "$WORK/top_$s.ref.csv"
done
