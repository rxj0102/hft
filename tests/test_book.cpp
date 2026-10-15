// Order book tests: building blocks, hand-written scenarios, and a property
// test that drives the real book and a slow reference with the same random
// events and requires identical results and identical full state.
#include <cstdint>
#include <cstdio>
#include <tuple>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "event_gen.hpp"
#include "hft/book/id_map.hpp"
#include "hft/book/level_bitmap.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "reference_book.hpp"

using namespace hft;
using namespace hft::book;
using core::Arena;
using core::Region;
using core::RegionOptions;
using testsupport::Event;
using testsupport::EventGen;
using testsupport::GenConfig;
using testsupport::RefBook;
using testsupport::Rng;

static Region make_region(std::size_t mb) {
  return Region::create(mb << 20, RegionOptions{false, false, true, -1});
}

static void test_bitmap() {
  Region r = make_region(1);
  Arena a(r);
  LevelBitmap bm;
  constexpr std::uint32_t kBits = 5000;  // several words and two summary words
  CHECK(bm.init(a, kBits));
  std::set<std::uint32_t> ref;
  Rng rng(42);
  for (int op = 0; op < 400000; ++op) {
    const std::uint32_t i = rng.below(kBits);
    // Bias toward sparse so long empty spans (the interesting case) are common.
    if (rng.below(100) < 45) {
      bm.set(i);
      ref.insert(i);
    } else {
      bm.clear(i);
      ref.erase(i);
    }
    const std::uint32_t q = rng.below(kBits);
    auto lo = ref.lower_bound(q);
    const std::uint32_t want_next = (lo == ref.end()) ? LevelBitmap::kNone : *lo;
    auto up = ref.upper_bound(q);
    const std::uint32_t want_prev = (up == ref.begin()) ? LevelBitmap::kNone : *std::prev(up);
    if (bm.next_ge(q) != want_next || bm.prev_le(q) != want_prev || bm.test(q) != (ref.count(q) == 1)) {
      CHECK(false);
      return;
    }
    if (ref.size() > 40) {  // keep it sparse
      const std::uint32_t victim = *ref.begin();
      bm.clear(victim);
      ref.erase(victim);
    }
  }
  // Edges.
  LevelBitmap e;
  CHECK(e.init(a, 64));
  CHECK(e.next_ge(0) == LevelBitmap::kNone && e.prev_le(63) == LevelBitmap::kNone);
  e.set(63);
  CHECK(e.next_ge(0) == 63 && e.prev_le(63) == 63 && e.prev_le(62) == LevelBitmap::kNone);
  e.set(0);
  CHECK(e.prev_le(62) == 0 && e.next_ge(1) == 63);
}

static void test_idmap() {
  Region r = make_region(1);
  Arena a(r);
  IdMap m;
  CHECK(!m.init(a, 3));  // not a power of two
  CHECK(m.init(a, 1024));
  CHECK(m.find(0) == IdMap::kNil);
  CHECK(!m.erase(0));
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::vector<std::uint64_t> keys;
  Rng rng(7);
  for (int op = 0; op < 2000000; ++op) {
    const std::uint32_t k = rng.below(100);
    if (k < 45 && ref.size() < 512) {
      // Small key space forces heavy collision and churn in a small table.
      const std::uint64_t key = 1 + rng.below(4000);
      if (!ref.count(key)) {
        const std::uint32_t v = static_cast<std::uint32_t>(op);
        m.insert(key, v);
        ref[key] = v;
      }
    } else if (k < 90) {
      const std::uint64_t key = 1 + rng.below(4000);
      const bool had = ref.erase(key) == 1;
      if (m.erase(key) != had) {
        CHECK(false);
        return;
      }
    } else {
      const std::uint64_t key = 1 + rng.below(4000);
      auto it = ref.find(key);
      const std::uint32_t want = it == ref.end() ? IdMap::kNil : it->second;
      if (m.find(key) != want) {
        CHECK(false);
        return;
      }
    }
    if (op % 50000 == 0) {
      for (const auto& [key, v] : ref) {
        if (m.find(key) != v) {
          CHECK(false);
          return;
        }
      }
    }
  }
}

static BookConfig small_cfg() {
  BookConfig c;
  c.base = Price::from_units(100);
  c.tick_raw = 100;
  c.num_levels = 1000;
  c.max_orders = 4;
  return c;
}

static Price px(std::int64_t ticks) { return Price::from_raw(Price::from_units(100).raw() + ticks * 100); }

static void test_book_basics() {
  Region r = make_region(1);
  Arena a(r);
  OrderBook b;
  CHECK(b.init(a, small_cfg()));
  CHECK(!b.best_bid().valid() && !b.best_ask().valid());

  CHECK(b.add(1, Side::Buy, px(10), 100) == BookResult::Ok);
  CHECK(b.add(2, Side::Buy, px(12), 50) == BookResult::Ok);
  CHECK(b.add(3, Side::Sell, px(20), 70) == BookResult::Ok);
  CHECK(b.best_bid() == px(12) && b.best_bid_qty() == 50);
  CHECK(b.best_ask() == px(20) && b.best_ask_qty() == 70);

  // Errors, each mapped to its own result.
  CHECK(b.add(0, Side::Buy, px(1), 1) == BookResult::BadId);
  CHECK(b.add(9, Side::Buy, px(1), 0) == BookResult::BadQty);
  CHECK(b.add(9, Side::Buy, px(-1), 1) == BookResult::BadPrice);
  CHECK(b.add(9, Side::Buy, px(1000), 1) == BookResult::BadPrice);
  CHECK(b.add(9, Side::Buy, Price::from_raw(px(1).raw() + 1), 1) == BookResult::BadPrice);
  CHECK(b.add(1, Side::Buy, px(1), 1) == BookResult::Duplicate);
  CHECK(b.cancel(99) == BookResult::NotFound);
  CHECK(b.execute(99, 1) == BookResult::NotFound);
  CHECK(b.execute(1, 0) == BookResult::BadQty && b.execute(1, 101) == BookResult::BadQty);
  CHECK(b.replace(99, px(1), 1) == BookResult::NotFound);
  CHECK(b.replace(1, px(1), 0) == BookResult::BadQty);

  // Cancel the best bid: next best is found through the bitmap.
  CHECK(b.cancel(2) == BookResult::Ok);
  CHECK(b.best_bid() == px(10));
  CHECK(!b.contains(2));

  // Partial then full execute.
  CHECK(b.execute(1, 40) == BookResult::Ok);
  CHECK(b.best_bid_qty() == 60 && b.order_count() == 2);
  CHECK(b.execute(1, 60) == BookResult::Ok);
  CHECK(!b.best_bid().valid() && b.order_count() == 1);

  // Fill to capacity (max_orders = 4), then Full, then room again after cancel.
  CHECK(b.add(10, Side::Buy, px(5), 1) == BookResult::Ok);
  CHECK(b.add(11, Side::Buy, px(5), 1) == BookResult::Ok);
  CHECK(b.add(12, Side::Buy, px(5), 1) == BookResult::Ok);
  CHECK(b.add(13, Side::Buy, px(5), 1) == BookResult::Full);
  CHECK(b.cancel(10) == BookResult::Ok);
  CHECK(b.add(13, Side::Buy, px(5), 1) == BookResult::Ok);
}

static void test_priority() {
  Region r = make_region(1);
  Arena a(r);
  BookConfig c = small_cfg();
  c.max_orders = 16;
  OrderBook b1, b2;
  CHECK(b1.init(a, c) && b2.init(a, c));

  // b1: A then B at one price; then grow A, which must send A behind B.
  b1.add(1, Side::Buy, px(10), 100);
  b1.add(2, Side::Buy, px(10), 100);
  CHECK(b1.replace(1, px(10), 150) == BookResult::Ok);
  b2.add(2, Side::Buy, px(10), 100);
  b2.add(1, Side::Buy, px(10), 150);
  CHECK(b1.digest() == b2.digest());

  // Shrinking in place keeps position: A stays ahead of B.
  OrderBook b3, b4;
  CHECK(b3.init(a, c) && b4.init(a, c));
  b3.add(1, Side::Sell, px(30), 100);
  b3.add(2, Side::Sell, px(30), 100);
  CHECK(b3.replace(1, px(30), 40) == BookResult::Ok);
  b4.add(1, Side::Sell, px(30), 40);
  b4.add(2, Side::Sell, px(30), 100);
  CHECK(b3.digest() == b4.digest());

  // Moving price also loses priority and updates best price.
  CHECK(b3.replace(1, px(25), 40) == BookResult::Ok);
  CHECK(b3.best_ask() == px(25) && b3.level_qty(Side::Sell, px(30)) == 100);
}

static void test_clear_side_rebase() {
  Region r = make_region(1);
  Arena a(r);
  BookConfig c = small_cfg();
  c.max_orders = 16;
  OrderBook b;
  CHECK(b.init(a, c));
  b.add(1, Side::Buy, px(10), 100);
  b.add(2, Side::Buy, px(10), 50);
  b.add(3, Side::Sell, px(20), 70);

  Side s = Side::Buy;
  CHECK(b.side_of(3, s) && s == Side::Sell);
  CHECK(b.side_of(1, s) && s == Side::Buy);
  CHECK(!b.side_of(99, s));

  // Rebase keeps prices, queue order and best prices; the digest uses absolute prices.
  const std::uint64_t before = b.digest();
  CHECK(b.rebase(Price::from_raw(Price::from_units(100).raw() + 5 * 100)));  // base up 5 ticks
  CHECK(b.digest() == before);
  CHECK(b.best_bid() == px(10) && b.best_ask() == px(20));
  CHECK(b.add(4, Side::Buy, px(7), 1) == BookResult::Ok);       // still inside the new window
  CHECK(b.add(5, Side::Buy, px(4), 1) == BookResult::BadPrice);  // now below the window
  CHECK(b.cancel(4) == BookResult::Ok);

  // Failure leaves the book untouched.
  CHECK(!b.rebase(px(15)));                                        // would strand the bid at px(10)
  CHECK(!b.rebase(Price::from_raw(px(5).raw() + 1)));              // not a whole tick
  CHECK(!b.rebase(Price::from_raw(px(5).raw() - 990 * 100)));      // ask would pass the top of the window
  CHECK(b.digest() == before);

  // Clear, then reuse the same ids.
  b.clear();
  CHECK(b.order_count() == 0 && !b.best_bid().valid() && !b.best_ask().valid());
  CHECK(!b.contains(1));
  CHECK(b.add(1, Side::Sell, px(30), 5) == BookResult::Ok);
  CHECK(b.best_ask() == px(30));

  // An empty book may move anywhere.
  b.clear();
  CHECK(b.rebase(px(500)));
  CHECK(b.add(1, Side::Buy, px(10), 1) == BookResult::BadPrice);
  CHECK(b.add(1, Side::Buy, px(600), 1) == BookResult::Ok);
}

struct Counts {
  std::uint64_t by_result[7] = {};
  std::uint64_t by_type[4] = {};
  std::uint64_t rebase_ok = 0, rebase_refused = 0;
};


// Independent oracle for OrderBook::depth: sum the quantity of every resting order
// within n ticks of the touch, straight from the reference book.
static hft::core::Qty ref_depth(const RefBook& ref, Side side, int n, std::int64_t tick) {
  const Price touch = side == Side::Buy ? ref.best_bid() : ref.best_ask();
  if (touch == Price::invalid() || n <= 0) return 0;
  const std::int64_t lo = side == Side::Buy ? touch.raw() - (n - 1) * tick : touch.raw();
  const std::int64_t hi = side == Side::Buy ? touch.raw() : touch.raw() + (n - 1) * tick;
  hft::core::Qty t = 0;
  ref.for_each_order([&](Side s, std::int64_t p, std::uint64_t, hft::core::Qty q) {
    if (s == side && p >= lo && p <= hi) t += q;
  });
  return t;
}

static void check_depth(const OrderBook& book, const RefBook& ref, std::int64_t tick) {
  for (const int n : {-1, 0, 1, 2, 3, 5, 40}) {
    CHECK(book.depth(Side::Buy, n) == ref_depth(ref, Side::Buy, n, tick));
    CHECK(book.depth(Side::Sell, n) == ref_depth(ref, Side::Sell, n, tick));
  }
}

// The touch at the very edges of the price grid: the window must stop at the grid, not wrap.
static void test_depth_at_grid_edges() {
  Region region = make_region(8);
  Arena arena(region);
  BookConfig cfg;
  cfg.base = Price::from_units(100);
  cfg.tick_raw = 100;
  cfg.num_levels = 64;
  cfg.max_orders = 64;
  OrderBook b;
  CHECK(b.init(arena, cfg));
  RefBook ref(cfg);
  CHECK(b.depth(Side::Buy, 3) == 0 && b.depth(Side::Sell, 3) == 0);
  auto lvl = [&](std::int64_t l) { return Price::from_raw(cfg.base.raw() + l * cfg.tick_raw); };
  for (const auto& o : {std::make_tuple(1u, Side::Buy, 0, 7), std::make_tuple(2u, Side::Buy, 1, 3),
                        std::make_tuple(3u, Side::Sell, 63, 11), std::make_tuple(4u, Side::Sell, 62, 5),
                        std::make_tuple(5u, Side::Sell, 62, 6)}) {
    CHECK(b.add(std::get<0>(o), std::get<1>(o), lvl(std::get<2>(o)), std::get<3>(o)) == BookResult::Ok);
    CHECK(ref.add(std::get<0>(o), std::get<1>(o), lvl(std::get<2>(o)), std::get<3>(o)) == BookResult::Ok);
  }
  CHECK(b.depth(Side::Buy, 1) == 3 && b.depth(Side::Buy, 2) == 10 && b.depth(Side::Buy, 50) == 10);  // stops at level 0
  CHECK(b.depth(Side::Sell, 1) == 11 && b.depth(Side::Sell, 2) == 22 && b.depth(Side::Sell, 50) == 22);  // stops at the last level
  check_depth(b, ref, cfg.tick_raw);
}

static void run_equivalence(std::uint64_t n_events, std::uint64_t seed, std::uint32_t num_levels,
                            std::uint32_t max_orders, std::uint32_t target_live) {
  Region region = make_region(64);
  Arena arena(region);
  BookConfig cfg;
  cfg.base = Price::from_units(100);
  cfg.tick_raw = 100;
  cfg.num_levels = num_levels;
  cfg.max_orders = max_orders;
  OrderBook book;
  CHECK(book.init(arena, cfg));
  RefBook ref(cfg);
  EventGen gen(GenConfig{cfg.base, cfg.tick_raw, num_levels, target_live, true}, seed);

  Counts counts;
  Rng meta(seed ^ 0xABCDEF);
  Price cur_base = cfg.base;
  std::uint64_t burst = 0, digest_checks = 0, max_live = 0;
  for (std::uint64_t i = 0; i < n_events; ++i) {
    // Periodic add-only bursts push the book into Full so that path is covered.
    if (i > 0 && i % 400000 == 0) burst = max_orders;

    // Periodically slide the price window by a few ticks. Both books must agree
    // on whether it is allowed and, if so, still hold identical state.
    if (i % 25000 == 12345) {
      std::int64_t delta = 1 + static_cast<std::int64_t>(meta.below(3));
      if (meta.below(2)) delta = -delta;
      const Price nb = Price::from_raw(cur_base.raw() + delta * cfg.tick_raw);
      const bool ra = book.rebase(nb);
      const bool rb = ref.rebase(nb);
      if (ra != rb) {
        std::fprintf(stderr, "REBASE MISMATCH at event %llu: book=%d ref=%d\n",
                     static_cast<unsigned long long>(i), ra, rb);
        CHECK(false);
        return;
      }
      if (ra) {
        cur_base = nb;
        gen.on_rebase(nb, delta);
        ++counts.rebase_ok;
      } else {
        ++counts.rebase_refused;
      }
      if (book.digest() != ref.digest()) {
        std::fprintf(stderr, "DIGEST MISMATCH after rebase at event %llu\n",
                     static_cast<unsigned long long>(i));
        CHECK(false);
        return;
      }
    }
    Event ev;
    if (burst) {
      ev = gen.next_add();
      --burst;
    } else {
      ev = gen.next();
    }
    const BookResult a = testsupport::apply(book, ev);
    const BookResult b = testsupport::apply(ref, ev);
    if (a != b) {
      std::fprintf(stderr, "MISMATCH at event %llu: type=%d book=%d ref=%d id=%llu\n",
                   static_cast<unsigned long long>(i), ev.type, static_cast<int>(a), static_cast<int>(b),
                   static_cast<unsigned long long>(ev.id));
      CHECK(false);
      return;
    }
    gen.commit(ev, b);
    ++counts.by_result[static_cast<int>(b)];
    ++counts.by_type[ev.type];
    if (book.order_count() != ref.order_count() || book.best_bid() != ref.best_bid() ||
        book.best_ask() != ref.best_ask()) {
      std::fprintf(stderr, "STATE MISMATCH at event %llu\n", static_cast<unsigned long long>(i));
      CHECK(false);
      return;
    }
    if (book.order_count() > max_live) max_live = book.order_count();
    if (i % 997 == 0) {
      check_depth(book, ref, cfg.tick_raw);
      ++digest_checks;
      if (book.digest() != ref.digest() || !book.check_invariants()) {
        std::fprintf(stderr, "DIGEST/INVARIANT MISMATCH at event %llu\n", static_cast<unsigned long long>(i));
        CHECK(false);
        return;
      }
    }
  }
  CHECK(book.digest() == ref.digest() && book.check_invariants());
  static const char* names[] = {"Ok", "BadId", "BadQty", "BadPrice", "Duplicate", "NotFound", "Full"};
  std::printf("equivalence: %llu events, seed %llu, %llu digest checks, peak live %llu, final live %u\n",
              static_cast<unsigned long long>(n_events), static_cast<unsigned long long>(seed),
              static_cast<unsigned long long>(digest_checks), static_cast<unsigned long long>(max_live),
              book.order_count());
  std::printf("  types  : add %llu cancel %llu exec %llu replace %llu\n",
              static_cast<unsigned long long>(counts.by_type[0]), static_cast<unsigned long long>(counts.by_type[1]),
              static_cast<unsigned long long>(counts.by_type[2]), static_cast<unsigned long long>(counts.by_type[3]));
  std::printf("  results:");
  for (int k = 0; k < 7; ++k) std::printf(" %s %llu", names[k], static_cast<unsigned long long>(counts.by_result[k]));
  std::printf("\n  rebases: %llu applied, %llu refused\n", static_cast<unsigned long long>(counts.rebase_ok),
              static_cast<unsigned long long>(counts.rebase_refused));
  if (n_events >= 500000) CHECK(counts.rebase_ok > 0);
  // Every result kind must actually have been exercised, or the test is weaker than it looks.
  for (int k = 0; k < 7; ++k) {
    if (n_events >= 500000) CHECK(counts.by_result[k] > 0);
  }
}

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000ULL;
  test_depth_at_grid_edges();
  test_bitmap();
  test_idmap();
  test_book_basics();
  test_priority();
  test_clear_side_rebase();
  run_equivalence(n, 12345, 4096, 4096, 2500);
  // A second, differently shaped book: tiny window, small capacity, many collisions.
  run_equivalence(n / 4, 987654321, 256, 512, 300);
  if (g_failures == 0) std::puts("test_book: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
