// Drive the order book with one stock from a real Nasdaq TotalView-ITCH 5.0
// file and log the top of book after every change.
//
//   hft_itch_check FILE SYMBOL OUT.csv
//
// FILE is in Nasdaq's BinaryFILE framing (2-byte big-endian length, then the
// message), either a full day or a file cut down to a few stocks. SYMBOL is
// matched against the stock directory (R) messages. OUT.csv gets
// ts,index,bid,bid_qty,ask,ask_qty after every message that changes the best
// bid or ask or the size at either, the format scripts/compare_top.py reads.
//
// The project's own feed format is ITCH-style but not ITCH (include/hft/md),
// so this tool reads the real format itself. Mapping onto the book:
//   A, F  add          E, C  execute        D  cancel
//   X     replace at the same price with the remaining size (keeps priority)
//   U     cancel the old id, add the new id on the same side
// The price window is centred on the median add price; orders outside it are
// rejected by the book (BadPrice) and counted, as are later messages that name
// them. This is offline verification, not the hot path.
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"

using hft::book::BookConfig;
using hft::book::BookResult;
using hft::book::OrderBook;
using hft::core::Arena;
using hft::core::Price;
using hft::core::Qty;
using hft::core::Region;
using hft::core::RegionOptions;
using hft::core::Side;

namespace {

std::uint64_t be(const unsigned char* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

struct Msg {
  const unsigned char* p;
  unsigned len;
};

// Walk the frames, calling f(type, msg) for messages of the given locate (and
// for every R message). Returns false on a truncated frame.
template <class F>
bool for_each(const std::vector<unsigned char>& d, F&& f) {
  std::size_t pos = 0;
  while (pos + 2 <= d.size()) {
    const unsigned len = static_cast<unsigned>(be(&d[pos], 2));
    if (len == 0 || pos + 2 + len > d.size()) return false;
    f(Msg{&d[pos + 2], len});
    pos += 2 + len;
  }
  return pos == d.size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: hft_itch_check FILE SYMBOL OUT.csv\n");
    return 2;
  }
  std::FILE* in = std::fopen(argv[1], "rb");
  if (!in) {
    std::perror(argv[1]);
    return 2;
  }
  std::vector<unsigned char> data;
  {
    unsigned char buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(in);
  }
  const std::string symbol = argv[2];

  // Pass 1: the stock's locate code and the median add price.
  int locate = -1;
  std::vector<std::int64_t> prices;
  bool ok = for_each(data, [&](Msg m) {
    const char t = static_cast<char>(m.p[0]);
    if (t == 'R' && m.len >= 19) {
      std::string s(reinterpret_cast<const char*>(m.p + 11), 8);
      s.erase(s.find_last_not_of(' ') + 1);
      if (s == symbol) locate = static_cast<int>(be(m.p + 1, 2));
      return;
    }
    if (locate < 0 || m.len < 3 || static_cast<int>(be(m.p + 1, 2)) != locate) return;
    if ((t == 'A' || t == 'F') && m.len >= 36) prices.push_back(static_cast<std::int64_t>(be(m.p + 32, 4)));
  });
  if (!ok || locate < 0 || prices.empty()) {
    std::fprintf(stderr, "no add messages for %s (or a truncated file)\n", symbol.c_str());
    return 1;
  }
  auto mid = prices.begin() + static_cast<std::ptrdiff_t>(prices.size() / 2);
  std::nth_element(prices.begin(), mid, prices.end());

  BookConfig cfg;
  cfg.tick_raw = 100;  // one cent in ITCH's 1e-4 dollar price units
  cfg.num_levels = 65536;
  cfg.max_orders = 1u << 21;
  std::int64_t base = *mid - static_cast<std::int64_t>(cfg.num_levels / 2) * cfg.tick_raw;
  if (base < 0) base = 0;
  base -= base % cfg.tick_raw;
  cfg.base = Price::from_raw(base);

  Region region = Region::create(std::size_t{512} << 20, RegionOptions{false, false, true, -1});
  Arena arena(region);
  OrderBook book;
  if (!book.init(arena, cfg)) {
    std::fprintf(stderr, "book init failed\n");
    return 1;
  }

  std::FILE* out = std::fopen(argv[3], "w");
  if (!out) {
    std::perror(argv[3]);
    return 2;
  }
  std::fputs("ts,index,bid,bid_qty,ask,ask_qty\n", out);

  std::uint64_t applied = 0, adds = 0, bad_price = 0, not_found = 0, other_err = 0;
  std::int64_t pb = -1, pa = -1, qb = -1, qa = -1;
  auto count = [&](BookResult r) {
    if (r == BookResult::Ok) return;
    if (r == BookResult::BadPrice) ++bad_price;
    else if (r == BookResult::NotFound) ++not_found;
    else ++other_err;
  };

  for_each(data, [&](Msg m) {
    const char t = static_cast<char>(m.p[0]);
    if (m.len < 11 || static_cast<int>(be(m.p + 1, 2)) != locate || t == 'R') return;
    const std::uint64_t ts = be(m.p + 5, 6);
    switch (t) {
      case 'A':
      case 'F': {
        const auto id = be(m.p + 11, 8);
        const Side side = m.p[19] == 'B' ? Side::Buy : Side::Sell;
        const auto qty = static_cast<Qty>(be(m.p + 20, 4));
        const auto px = Price::from_raw(static_cast<std::int64_t>(be(m.p + 32, 4)));
        ++adds;
        count(book.add(id, side, px, qty));
        break;
      }
      case 'E':
      case 'C':
        count(book.execute(be(m.p + 11, 8), static_cast<Qty>(be(m.p + 19, 4))));
        break;
      case 'X': {
        const auto id = be(m.p + 11, 8);
        const auto cut = static_cast<Qty>(be(m.p + 19, 4));
        // The book has no partial cancel: replace at the same price with what
        // is left, which keeps queue position, or cancel if nothing is left.
        Price px;
        Qty left;
        if (!book.order_price_qty(id, px, left)) {
          ++not_found;
          break;
        }
        count(left - cut > 0 ? book.replace(id, px, left - cut) : book.cancel(id));
        break;
      }
      case 'D':
        count(book.cancel(be(m.p + 11, 8)));
        break;
      case 'U': {
        const auto old_id = be(m.p + 11, 8);
        const auto new_id = be(m.p + 19, 8);
        const auto qty = static_cast<Qty>(be(m.p + 27, 4));
        const auto px = Price::from_raw(static_cast<std::int64_t>(be(m.p + 31, 4)));
        Side side;
        if (!book.side_of(old_id, side)) {
          ++not_found;
          break;
        }
        count(book.cancel(old_id));
        count(book.add(new_id, side, px, qty));
        break;
      }
      default:
        return;  // not a book message
    }
    ++applied;
    const bool hb = book.best_bid_qty() > 0, ha = book.best_ask_qty() > 0;
    const std::int64_t bb = hb ? book.best_bid().raw() : -1, ba = ha ? book.best_ask().raw() : -1;
    const std::int64_t bq = hb ? book.best_bid_qty() : -1, aq = ha ? book.best_ask_qty() : -1;
    if (bb != pb || ba != pa || bq != qb || aq != qa) {
      std::fprintf(out, "%" PRIu64 ",%" PRIu64 ",", ts, applied);
      if (hb) std::fprintf(out, "%" PRId64 ",%" PRId64 ",", bb, bq);
      else std::fputs(",,", out);
      if (ha) std::fprintf(out, "%" PRId64 ",%" PRId64 "\n", ba, aq);
      else std::fputs(",\n", out);
      pb = bb, pa = ba, qb = bq, qa = aq;
    }
  });
  std::fclose(out);

  std::printf("%s locate %d: %" PRIu64 " book messages, %" PRIu64 " adds\n", symbol.c_str(), locate,
              applied, adds);
  std::printf("price window %.2f to %.2f dollars around the median add price %.4f\n",
              static_cast<double>(base) / 1e4,
              static_cast<double>(base + static_cast<std::int64_t>(cfg.num_levels) * cfg.tick_raw) / 1e4,
              static_cast<double>(*mid) / 1e4);
  std::printf("rejected: %" PRIu64 " outside the window or off tick, %" PRIu64
              " naming an unknown order, %" PRIu64 " other\n",
              bad_price, not_found, other_err);
  std::printf("resting orders at end %u\n", book.order_count());
  return 0;
}
