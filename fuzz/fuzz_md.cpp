// libFuzzer target for the market data path: the packet parser, the A/B
// arbiter, gap handling, snapshot assembly and the order book underneath.
//
// Input framing: byte 0 is a flags byte (bit 0: start Live instead of waiting
// for a snapshot), then chunks of  kind(1) len(2, big-endian) bytes(len).
// kind & 3: 0 = packet on line A, 1 = packet on line B, 2 = snapshot-channel
// packet, 3 = advance time and poll. Chunks that run past the end are truncated.
//
// Oracles: no crash, no sanitizer report, no hang, and after every input the
// book must satisfy check_invariants() and respect its capacity. A feed handler
// that accepts hostile bytes but leaves a corrupt book is worse than one that
// crashes, so the structural check is the important one.
#include <cstddef>
#include <cstdint>

#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "hft/md/itch_wire.hpp"
#include "hft/md/md_handler.hpp"

namespace {

// Small book so every code path (full pool, level churn, rebase-free window
// edges) is reachable from short inputs, and each exec stays cheap.
constexpr std::uint32_t kLevels = 256;
constexpr std::uint32_t kMaxOrders = 128;

hft::core::Region& region() {
  static hft::core::Region r =
      hft::core::Region::create(4u << 20, hft::core::RegionOptions{false, false, true, -1});
  return r;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace hft;
  if (size < 1) return 0;

  // A fresh Arena over the same region each run: every init() below re-zeroes
  // what it uses, so no state leaks between inputs.
  core::Arena arena(region());
  book::OrderBook book;
  book::BookConfig bc;
  bc.base = core::Price::from_units(100);
  bc.tick_raw = 100;
  bc.num_levels = kLevels;
  bc.max_orders = kMaxOrders;
  if (!book.init(arena, bc)) __builtin_trap();

  md::MdConfig mc;
  mc.locate = 1;
  mc.pending_slots = 8;
  mc.gap_timeout_ns = 5000;
  md::MdHandler<> h;
  if (!h.init(arena, book, mc)) __builtin_trap();
  if (data[0] & 1) h.begin_live(1);

  std::uint64_t now = 0;
  std::size_t off = 1;
  while (off + 3 <= size) {
    const std::uint8_t kind = data[off];
    std::size_t len = md::be16(data + off + 1);
    off += 3;
    if (len > size - off) len = size - off;
    const std::uint8_t* chunk = data + off;
    off += len;
    now += 1000;
    switch (kind & 3) {
      case 0: h.on_packet(md::Line::A, chunk, len, now); break;
      case 1: h.on_packet(md::Line::B, chunk, len, now); break;
      case 2: h.on_snapshot_packet(chunk, len, now); break;
      default: h.poll(now + static_cast<std::uint64_t>(kind >> 2) * 2000); break;
    }
  }

  if (book.order_count() > kMaxOrders || !book.check_invariants()) __builtin_trap();
  return 0;
}
