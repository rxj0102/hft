// libFuzzer target for the whole tick-to-trade pipeline: hostile feed packets,
// valid-looking feed events built from the input, hostile exchange bytes and
// well-formed exchange reports, all through MdHandler -> book -> MarketMaker ->
// Gateway -> OMS, with a small book and tight risk limits so every failure
// path is a few bytes away.
//
// Input: a flags byte (bit 0: start Live), then chunks  kind(1) len(2 BE) bytes(len):
//   kind & 7:  0 raw packet on line A   1 raw packet on line B   2 snapshot packet
//              3 raw exchange bytes     4 advance time and poll
//              5 structured feed event  (op, id, side, price, qty from the bytes,
//                                        stamped with the sequence number the handler expects)
//              6 structured report      (type, order id, quantities from the bytes,
//                                        stamped with the sequence number the gateway expects)
//   bit 3 of kind (kind & 8) on a structured chunk corrupts its sequence number;
//   bit 4 (kind & 16) on a raw packet stamps the right session id over its first 10 bytes,
//   so mutated packets get past the session check and reach the sequence logic.
//
// Oracles, after every input: no crash or sanitizer report; the book, the parked
// packet counter and the OMS pass their invariants; every frame the pipeline put
// on the wire decodes, carries the next sequence number, and a new order or a
// replace is never sent while the feed handler does not vouch for the book.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/core/memory.hpp"
#include "hft/gw/order_entry.hpp"
#include "hft/md/itch_encoder.hpp"
#include "hft/t2t/pipeline.hpp"

namespace {

using namespace hft;

constexpr std::uint32_t kLevels = 256;
constexpr std::uint32_t kMaxOrders = 64;
constexpr std::int64_t kBaseRaw = 1'000'000;  // Price::from_units(100)
constexpr char kSession[] = "FUZZSESS01";

core::Region& region() {
  static core::Region r = core::Region::create(8u << 20, core::RegionOptions{false, false, true, -1});
  return r;
}

struct Wire {
  std::uint64_t frames = 0, expect_seq = 1;
  bool bad = false;
  const md::MdHandler<t2t::detail::FeedWatch>* feed = nullptr;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    ++frames;
    if (n < 2 + 9 || md::be16(d) != n - 2) bad = true;
    gw::ClientMsg m;
    if (!gw::decode_client(d + 2, n - 2, m) || m.seq != expect_seq) bad = true;
    ++expect_seq;
    if ((d[2] == gw::kMsgNew || d[2] == gw::kMsgReplace) && feed && !feed->book_valid()) bad = true;
  }
};
struct WireRef {
  Wire* w;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { (*w)(d, n); }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  core::Arena arena(region());

  t2t::PipelineConfig cfg;
  cfg.book.base = core::Price::from_units(100);
  cfg.book.tick_raw = 100;
  cfg.book.num_levels = kLevels;
  cfg.book.max_orders = kMaxOrders;
  cfg.md.locate = 1;
  cfg.md.pending_slots = 8;
  cfg.md.gap_timeout_ns = 5000;
  cfg.md.check_session = true;
  std::memcpy(cfg.md.session, kSession, md::kSessionBytes);
  cfg.oms = oms::OmsConfig{64};
  cfg.risk.max_order_qty = 50;
  cfg.risk.max_order_notional_raw = 100'000'000'000LL;
  cfg.risk.price_band_bps = 5000;
  cfg.risk.max_position = 40;
  cfg.risk.max_open_orders = 6;
  cfg.risk.rate_per_sec = 1'000'000;
  cfg.risk.burst = 20;
  cfg.live_at_start = (data[0] & 1) != 0;

  Wire wire;
  t2t::Pipeline<WireRef> p(WireRef{&wire}, cfg.mm);
  wire.feed = &p.feed();
  if (!p.init(arena, cfg)) __builtin_trap();

  std::uint64_t now = 1000, rseq = 1;
  std::size_t off = 1;
  while (off + 3 <= size) {
    const std::uint8_t kind = data[off];
    std::size_t len = md::be16(data + off + 1);
    off += 3;
    if (len > size - off) len = size - off;
    const std::uint8_t* c = data + off;
    off += len;
    now += 1000;
    const bool corrupt = (kind & 8) != 0;
    switch (kind & 7) {
      case 0:
      case 1: {
        std::uint8_t buf[md::kMaxPacketBytes + 64];
        const std::size_t n = len < sizeof buf ? len : sizeof buf;
        std::memcpy(buf, c, n);
        if ((kind & 16) && n >= md::kSessionBytes) std::memcpy(buf, kSession, md::kSessionBytes);
        p.on_packet((kind & 7) == 0 ? md::Line::A : md::Line::B, buf, n, now);
        break;
      }
      case 2: p.on_snapshot(c, len, now); break;
      case 3: p.on_report(c, len, now); break;
      case 4: p.poll(now + static_cast<std::uint64_t>(len) * 1000); break;
      case 5: {
        // One feed event, valid on the wire. The book may still reject it (unknown id, full pool).
        if (len < 5) break;
        md::PacketBuilder pb(kSession);
        const std::uint64_t seq = p.feed().next_seq() + (corrupt ? 3 : 0);
        pb.reset(seq);
        const std::uint64_t id = 1 + c[1] % 40;
        const core::Side side = (c[2] & 1) ? core::Side::Sell : core::Side::Buy;
        const auto px = static_cast<std::uint32_t>(kBaseRaw + 100 * (c[3] % 200));
        const std::uint32_t qty = 1u + c[4] % 30;
        switch (c[0] & 3) {
          case 0: pb.add_message(36, [&](std::uint8_t* d) { md::encode_add(d, 1, now, id, side, qty, px); }); break;
          case 1: pb.add_message(31, [&](std::uint8_t* d) { md::encode_exec(d, 1, now, id, qty); }); break;
          case 2: pb.add_message(23, [&](std::uint8_t* d) { md::encode_partial_cancel(d, 1, now, id, qty); }); break;
          default: pb.add_message(19, [&](std::uint8_t* d) { md::encode_delete(d, 1, now, id); }); break;
        }
        const std::uint8_t* d = pb.finish();
        p.on_packet(md::Line::A, d, pb.size(), now);
        break;
      }
      default: {
        // One exchange report for an order id the gateway has issued (or one it has not).
        if (len < 6) break;
        sim::Report r{};
        r.type = static_cast<sim::ReportType>(c[0] % 7);
        r.owner = 1;
        r.id = 1 + c[1] % 24;
        r.side = (c[2] & 1) ? core::Side::Sell : core::Side::Buy;
        r.price = core::Price::from_raw(kBaseRaw + 100 * (c[3] % 200));
        r.qty = static_cast<core::Qty>(c[4] % 40);
        r.leaves = static_cast<core::Qty>(c[5] % 40);
        r.seq = 1;
        r.ts = now;
        std::uint8_t buf[gw::kMaxFrame];
        const std::size_t n = gw::encode_report(buf, rseq + (corrupt ? 2 : 0), r);
        ++rseq;
        p.on_report(buf, n, now);
        break;
      }
    }
  }

  if (wire.bad) __builtin_trap();
  if (p.book().order_count() > kMaxOrders || !p.book().check_invariants() || !p.feed().check_invariants() ||
      !p.gateway().oms().check_invariants())
    __builtin_trap();
  return 0;
}
