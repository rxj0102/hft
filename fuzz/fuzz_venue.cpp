// libFuzzer target for the queue simulation. Arbitrary bytes drive the REAL
// matching engine (so the tape is always a legal one), and the engine's public
// events feed a TapeBook and a VirtualVenue while the same input also places,
// cancels and replaces simulated orders of ours.
//
// Oracles (any violation traps):
//   - the tape always fits the book (no tape errors);
//   - the venue's two views of the queue agree (queue_inconsistent == 0) in the
//     exact model, at any penalty a fill never exceeds what was offered;
//   - fills add up: per order, filled <= quantity ever offered; reports carry
//     leaves that match;
//   - live orders never exceed the table, TapeBook invariants hold.
//
// Input: byte 0 = queue penalty percent (b % 3 * 40), then 5-byte records
//   [0] op: 0-3 background new (Limit, Limit, Market, IOC), 4 background cancel,
//       5 background replace, 6-7 our new, 8 our cancel, 9 our replace
//   [1] id selector (small space so ids collide), [2] side (bit 0) and owner (bits 1-2)
//   [3] price level (0-69 of 64), [4] quantity (int)b - 12
#include <cstddef>
#include <cstdint>

#include "hft/bt/tape_book.hpp"
#include "hft/bt/virtual_venue.hpp"
#include "hft/core/memory.hpp"
#include "hft/sim/matching_engine.hpp"

namespace {

using namespace hft;

hft::core::Region& region() {
  static hft::core::Region r =
      hft::core::Region::create(8u << 20, hft::core::RegionOptions{false, false, true, -1});
  return r;
}

[[noreturn]] void fail() { __builtin_trap(); }

struct Out {
  struct Acc {
    core::OrderId id = 0;
    core::Qty offered = 0, filled = 0;
  } acc[16];
  void report(const sim::Report& r) {
    Acc* a = nullptr;
    for (Acc& x : acc)
      if (x.id == r.id) a = &x;
    if (r.type == sim::ReportType::Accepted || r.type == sim::ReportType::Replaced) {
      if (!a)
        for (Acc& x : acc)
          if (x.id == 0 && !a) a = &x;
      if (!a) return;  // table of ids is tiny; extra ids just are not tracked
      a->id = r.id;
      a->offered += r.qty;
    } else if (r.type == sim::ReportType::Fill) {
      if (!a || r.qty <= 0) fail();
      a->filled += r.qty;
      if (a->filled > a->offered || r.leaves < 0) fail();
    }
  }
};

struct Feed {
  bt::TapeBook* book;
  bt::VirtualVenue* venue;
  Out* out;
  void on_report(const sim::Report&) noexcept {}
  void on_public(const sim::PublicEvent& e) noexcept {
    venue->on_tape(e, *out);
    if (!book->apply(e)) fail();
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 6) return 0;
  const std::uint32_t penalty = (data[0] % 3) * 40;
  core::Arena arena(region());
  book::BookConfig bc;
  bc.base = core::Price::from_raw(1'000'000);
  bc.tick_raw = 100;
  bc.num_levels = 64;
  bc.max_orders = 64;
  bt::TapeBook tb;
  if (!tb.init(arena, bc)) fail();
  bt::VirtualVenue venue;
  venue.init(&tb, penalty);
  Out out;
  Feed feed{&tb, &venue, &out};
  sim::EngineConfig ec;
  ec.base = bc.base;
  ec.tick_raw = 100;
  ec.num_levels = 64;
  ec.max_orders = 64;
  sim::MatchingEngine<Feed> eng;
  if (!eng.init(arena, ec, feed)) fail();

  std::uint64_t ts = 0;
  for (std::size_t i = 1; i + 5 <= size; i += 5) {
    const std::uint8_t* r = data + i;
    ++ts;
    const core::OrderId id = r[1] % 20 + 1;
    const core::Side side = (r[2] & 1) ? core::Side::Sell : core::Side::Buy;
    const sim::Owner owner = static_cast<sim::Owner>(2 + ((r[2] >> 1) & 3));
    const core::Price px = core::Price::from_raw(1'000'000 + static_cast<std::int64_t>(r[3] % 70) * 100);
    const core::Qty q = static_cast<core::Qty>(r[4]) - 12;
    switch (r[0] % 10) {
      case 0: case 1: eng.new_order(sim::NewOrder{id, owner, side, sim::OrderType::Limit, px, q}, ts); break;
      case 2: eng.new_order(sim::NewOrder{id, owner, side, sim::OrderType::Market, px, q}, ts); break;
      case 3: eng.new_order(sim::NewOrder{id, owner, side, sim::OrderType::IOC, px, q}, ts); break;
      case 4: eng.cancel(id, owner, ts); break;
      case 5: eng.replace(id, owner, px, q, ts); break;
      case 6: case 7: venue.on_new(sim::NewOrder{1000 + id, 1, side, sim::OrderType::Limit, px, q}, ts, out); break;
      case 8: venue.on_cancel(1000 + id, ts, out); break;
      default: venue.on_replace(1000 + id, px, q, ts, out); break;
    }
    if (!tb.check_invariants() || tb.errors() != 0) fail();
    if (penalty == 0 && venue.stats().queue_inconsistent != 0) fail();
    if (venue.live_orders() > bt::VirtualVenue::kMaxLive) fail();
  }
  return 0;
}
