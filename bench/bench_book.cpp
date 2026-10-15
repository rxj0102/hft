// Order book update latency. Phase 2 gate: median under 100 ns per update.
//
// Method: pre-generate a steady-state event stream (so generation cost is not
// timed), warm the book to its target depth, then time each update with
// ordered rdtsc and subtract the measured timer overhead. Reports per event
// type and overall, plus untimed throughput. Events replay from a flat array,
// so instruction/data access to the stream is sequential and cache-friendly;
// real feed handling adds parse and network cost (Phase 3 and 7 measure that).
#include <cstdint>
#include <cstdio>
#include <vector>

#include "event_gen.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"

using namespace hft;
using core::Histogram;
using core::TscCalibration;
using testsupport::Event;

namespace {

void print_row(const char* name, const Histogram& h, const TscCalibration& cal) {
  std::printf("%-10s %9llu  p50 %7.1f  p99 %7.1f  p99.9 %8.1f  max %10.1f  (ns)\n", name,
              static_cast<unsigned long long>(h.count()), cal.to_ns(h.quantile(0.50)),
              cal.to_ns(h.quantile(0.99)), cal.to_ns(h.quantile(0.999)), cal.to_ns(h.max()));
}

struct Setup {
  core::Region region;
  core::Arena arena;
  book::OrderBook book;
};

bool make_book(Setup& s, const book::BookConfig& cfg) {
  s.region = core::Region::create(128u << 20, core::RegionOptions{true, true, true, -1});
  if (!s.region) return false;
  s.arena = core::Arena(s.region);
  return s.book.init(s.arena, cfg);
}

}  // namespace

int main() {
  const TscCalibration cal;
  std::printf("TSC %.4f ticks/ns, invariant=%s\n", cal.ticks_per_ns(),
              core::has_invariant_tsc() ? "yes" : "NO");

  book::BookConfig cfg;
  cfg.base = core::Price::from_units(100);
  cfg.tick_raw = 100;
  cfg.num_levels = 4096;
  cfg.max_orders = 1u << 16;

  constexpr std::uint32_t kTarget = 20000;  // resting orders at steady state
  constexpr std::size_t kWarm = 300000, kTimed = 4000000;

  // Generate once with a scratch book so the live-order bookkeeping is exact,
  // then replay the identical stream into fresh books for each measurement.
  std::vector<Event> events;
  events.reserve(kWarm + kTimed);
  {
    Setup s;
    if (!make_book(s, cfg)) return 1;
    testsupport::EventGen gen({cfg.base, cfg.tick_raw, cfg.num_levels, kTarget, false}, 2024);
    for (std::size_t i = 0; i < kWarm + kTimed; ++i) {
      Event e = gen.next();
      gen.commit(e, testsupport::apply(s.book, e));
      events.push_back(e);
    }
    std::printf("steady-state resting orders after generation: %u\n", s.book.order_count());
  }

  // Timer overhead: floor of back-to-back ordered reads.
  Histogram oh;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t a = core::rdtsc_ordered();
    const std::uint64_t b = core::rdtsc_ordered();
    oh.record(b - a);
  }
  const std::uint64_t overhead = oh.quantile(0.50);
  std::printf("timer overhead %.1f ns (subtracted)\n\n", cal.to_ns(overhead));

  // Per-update latency.
  Setup s;
  if (!make_book(s, cfg)) return 1;
  for (std::size_t i = 0; i < kWarm; ++i) testsupport::apply(s.book, events[i]);

  Histogram by_type[4], all;
  std::uint64_t not_ok = 0;
  for (std::size_t i = kWarm; i < kWarm + kTimed; ++i) {
    const Event& e = events[i];
    const std::uint64_t t0 = core::rdtsc_ordered();
    const book::BookResult r = testsupport::apply(s.book, e);
    const std::uint64_t t1 = core::rdtsc_ordered();
    not_ok += (r != book::BookResult::Ok);
    const std::uint64_t d = (t1 - t0) > overhead ? (t1 - t0) - overhead : 0;
    by_type[e.type].record(d);
    all.record(d);
  }
  std::printf("results not Ok during timed run: %llu (expected 0)\n\n",
              static_cast<unsigned long long>(not_ok));
  print_row("add", by_type[Event::Add], cal);
  print_row("cancel", by_type[Event::Cancel], cal);
  print_row("exec", by_type[Event::Exec], cal);
  print_row("replace", by_type[Event::Replace], cal);
  print_row("ALL", all, cal);

  // Untimed throughput over a fresh book: no per-event clock reads at all.
  Setup t;
  if (!make_book(t, cfg)) return 1;
  for (std::size_t i = 0; i < kWarm; ++i) testsupport::apply(t.book, events[i]);
  const std::uint64_t c0 = core::rdtsc_ordered();
  for (std::size_t i = kWarm; i < kWarm + kTimed; ++i) testsupport::apply(t.book, events[i]);
  const std::uint64_t c1 = core::rdtsc_ordered();
  const double secs = cal.to_ns(c1 - c0) * 1e-9;
  std::printf("\nthroughput: %.1fM updates/sec (%.1f ns/update mean, untimed loop)\n",
              static_cast<double>(kTimed) / secs / 1e6, secs * 1e9 / static_cast<double>(kTimed));
  return 0;
}
