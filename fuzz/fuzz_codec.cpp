// libFuzzer target for the order-entry wire path. Arbitrary bytes are
//   1. fed to a StreamDecoder whole and again in input-driven pieces: both must
//      yield the same frames (same digest) and the same Broken/Ok outcome;
//   2. decoded as client messages and as reports; anything that decodes must
//      re-encode to exactly the bytes it came from (canonical form);
//   3. pushed into a Gateway (inbound) and an ExchangeEdge (client side) so
//      hostile bytes cannot crash them or corrupt the OMS.
// Input: byte 0 seeds the chunk sizes, the rest is the stream.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/core/memory.hpp"
#include "hft/gw/exchange_edge.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/gw/order_entry.hpp"

namespace {

using namespace hft;

hft::core::Region& region() {
  static hft::core::Region r =
      hft::core::Region::create(8u << 20, hft::core::RegionOptions{false, false, true, -1});
  return r;
}

struct Digest {
  std::uint64_t h = 1469598103934665603ULL;
  std::uint64_t frames = 0;
  void on_frame(const std::uint8_t* b, std::size_t n) {
    ++frames;
    h = (h ^ n) * 1099511628211ULL;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    check(b, n);
  }
  static void check(const std::uint8_t* b, std::size_t n) {
    std::uint8_t out[gw::kMaxFrame];
    gw::ClientMsg m;
    if (gw::decode_client(b, n, m)) {
      std::size_t len = 0;
      if (m.kind == gw::kMsgNew) len = gw::encode_new(out, m.seq, m.id, m.side, m.otype, m.qty, m.price);
      if (m.kind == gw::kMsgCancel) len = gw::encode_cancel(out, m.seq, m.id);
      if (m.kind == gw::kMsgReplace) len = gw::encode_replace(out, m.seq, m.id, m.qty, m.price);
      if (len != n + 2 || std::memcmp(out + 2, b, n) != 0) __builtin_trap();
    }
    sim::Report r;
    std::uint64_t seq = 0;
    if (gw::decode_report(b, n, r, seq)) {
      const std::size_t len = gw::encode_report(out, seq, r);
      if (len != n + 2 || std::memcmp(out + 2, b, n) != 0) __builtin_trap();
    }
  }
};

struct NullSend {
  void operator()(const std::uint8_t*, std::size_t) noexcept {}
};
struct NullOut {
  void operator()(sim::Owner, const std::uint8_t*, std::size_t, std::uint64_t) noexcept {}
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  const std::uint8_t seed = data[0];
  const std::uint8_t* p = data + 1;
  const std::size_t n = size - 1;

  Digest whole, parts;
  gw::StreamDecoder a, b;
  const auto sa = a.feed(p, n, whole);
  std::size_t off = 0, k = seed;
  gw::StreamDecoder::Status sb = gw::StreamDecoder::Status::Ok;
  while (off < n) {
    k = (k * 131 + 7) & 0xff;
    const std::size_t take = 1 + (k % 11) < n - off ? 1 + (k % 11) : n - off;
    sb = b.feed(p + off, take, parts);
    off += take;
  }
  if (sa != sb || whole.h != parts.h || whole.frames != parts.frames) __builtin_trap();

  hft::core::Arena arena(region());
  gw::Gateway<NullSend> g{NullSend{}};
  risk::RiskConfig rc;
  if (!g.init(arena, oms::OmsConfig{64}, rc)) __builtin_trap();
  g.risk().set_reference(core::Price::from_raw(1'200'000));
  g.new_order(core::Side::Buy, sim::OrderType::Limit, core::Price::from_raw(1'200'000), 5, 1);
  g.on_bytes(p, n, 2);
  if (!g.oms().check_invariants()) __builtin_trap();

  // Server side: the same hostile bytes as an arbitrary client's stream.
  sim::ExchangeSim sim;
  sim::SimConfig sc;
  sc.engine.base = core::Price::from_raw(1'000'000);
  sc.engine.tick_raw = 100;
  sc.engine.num_levels = 4096;
  sc.engine.max_orders = 256;
  sc.queue_capacity = 256;
  sc.max_participants = 4;
  if (!sim.init(arena, sc)) __builtin_trap();
  gw::ExchangeEdge<NullOut> e(sim, NullOut{});
  if (!e.init(arena, 4)) __builtin_trap();
  e.on_client_bytes(1, p, n, 1);
  struct Sink {
    gw::ExchangeEdge<NullOut>* e;
    void on_report(sim::Owner o, const sim::Report& r, std::uint64_t t) { e->on_report(o, r, t); }
    void on_public(const sim::PublicEvent&, std::uint64_t) {}
  } sink{&e};
  sim.advance_to(1000, sink);
  if (!sim.engine().check_invariants()) __builtin_trap();
  return 0;
}
