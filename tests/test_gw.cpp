// Phase 5 tests, part 2: the order-entry wire codec and stream reassembly, the
// gateway's session handling and kill switch, and the whole client path
// (risk -> OMS -> bytes -> exchange -> bytes -> OMS) against the exchange
// simulator with latency, jitter and competing flow.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "alloc_trap.hpp"  // replaces global operator new; keep in this one TU
#include "check.hpp"
#include "hft/core/memory.hpp"
#include "hft/gw/exchange_edge.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/gw/order_entry.hpp"
#include "hft/sim/exchange_sim.hpp"
#include "order_gen.hpp"

using namespace hft;
using namespace hft::gw;
using hft::oms::State;
using hft::sim::ExchangeSim;
using hft::sim::Liquidity;
using hft::sim::Owner;
using hft::sim::Reason;
using hft::sim::ReportType;
using hft::sim::SimConfig;
using testsupport::Rng;

namespace {

constexpr std::int64_t kBase = 1'000'000;
constexpr std::int64_t kTick = 100;
constexpr std::uint32_t kLevels = 4096;

Price PX(std::int64_t raw) { return Price::from_raw(raw); }
core::Region make_region(std::size_t mb) {
  return core::Region::create(mb << 20, core::RegionOptions{false, false, true, -1});
}

using Bytes = std::vector<std::uint8_t>;

Bytes frame_new(std::uint64_t seq, OrderId id, Side s, OrderType t, Qty q, Price p) {
  Bytes b(kMaxFrame);
  b.resize(encode_new(b.data(), seq, id, s, t, q, p));
  return b;
}
Bytes frame_cancel(std::uint64_t seq, OrderId id) {
  Bytes b(kMaxFrame);
  b.resize(encode_cancel(b.data(), seq, id));
  return b;
}
Bytes frame_replace(std::uint64_t seq, OrderId id, Qty q, Price p) {
  Bytes b(kMaxFrame);
  b.resize(encode_replace(b.data(), seq, id, q, p));
  return b;
}
Bytes frame_report(std::uint64_t seq, const Report& r) {
  Bytes b(kMaxFrame);
  b.resize(encode_report(b.data(), seq, r));
  return b;
}

Report mkrep(ReportType t, OrderId id, Qty qty, Qty leaves, std::int64_t px = 1'200'000, Reason why = Reason::None,
             Side side = Side::Buy) {
  Report r{};
  r.type = t;
  r.reason = why;
  r.liq = Liquidity::None;
  r.side = side;
  r.owner = 1;
  r.id = id;
  r.price = PX(px);
  r.qty = qty;
  r.leaves = leaves;
  r.seq = 1;
  r.ts = 5;
  return r;
}

// ---- codec ---------------------------------------------------------------------

void test_codec_golden() {
  Bytes f = frame_new(0x0102030405060708ULL, 0x1122334455667788ULL, Side::Sell, OrderType::IOC, 100, PX(1234567));
  const std::uint8_t want[] = {0x00, 0x23,                                                // length 35
                               'O',  1,    2,    3,    4,    5,    6,    7,    8,          // type, seq
                               0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,           // id
                               1,    2,                                                  // side, type
                               0,    0,    0,    0,    0,    0,    0,    100,            // qty
                               0,    0,    0,    0,    0,    0x12, 0xD6, 0x87};          // price
  CHECK(f.size() == sizeof(want) && std::memcmp(f.data(), want, sizeof(want)) == 0);
  ClientMsg m;
  CHECK(decode_client(f.data() + 2, f.size() - 2, m));
  CHECK(m.kind == kMsgNew && m.seq == 0x0102030405060708ULL && m.id == 0x1122334455667788ULL &&
        m.side == Side::Sell && m.otype == OrderType::IOC && m.qty == 100 && m.price.raw() == 1234567);

  Bytes c = frame_cancel(9, 0xAABBCCDDEEFF0011ULL);
  CHECK(c.size() == 19 && c[0] == 0 && c[1] == 17 && c[2] == 'X' && c[10] == 9 && c[11] == 0xAA && c[18] == 0x11);
  Bytes u = frame_replace(1, 2, -5, PX(-7));
  CHECK(u.size() == 35 && u[1] == 33 && u[2] == 'U');
  CHECK(decode_client(u.data() + 2, u.size() - 2, m) && m.qty == -5 && m.price.raw() == -7 && m.id == 2);
  Bytes r = frame_report(3, mkrep(ReportType::Fill, 5, 7, 8));
  CHECK(r.size() == 73 && r[1] == 71 && r[2] == 'R' && r[11] == static_cast<std::uint8_t>(ReportType::Fill));
}

Report random_report(Rng& rng) {
  Report r{};
  r.type = static_cast<ReportType>(rng.below(7));
  r.reason = static_cast<Reason>(rng.below(14));
  r.liq = static_cast<Liquidity>(rng.below(3));
  r.side = rng.below(2) ? Side::Sell : Side::Buy;
  r.owner = static_cast<Owner>(rng.below(65536));
  r.id = rng.next();
  r.counter_id = rng.next();
  r.price = PX(static_cast<std::int64_t>(rng.next()));
  r.qty = static_cast<Qty>(rng.next());
  r.leaves = static_cast<Qty>(rng.next());
  r.seq = rng.next();
  r.ts = rng.next();
  return r;
}

bool same_report(const Report& a, const Report& b) {
  return a.type == b.type && a.reason == b.reason && a.liq == b.liq && a.side == b.side && a.owner == b.owner &&
         a.id == b.id && a.counter_id == b.counter_id && a.price == b.price && a.qty == b.qty &&
         a.leaves == b.leaves && a.seq == b.seq && a.ts == b.ts;
}

void test_codec_roundtrip(std::uint64_t n) {
  Rng rng(99);
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t seq = rng.next();
    const Report r = random_report(rng);
    Bytes f = frame_report(seq, r);
    Report back{};
    std::uint64_t bseq = 0;
    CHECK(decode_report(f.data() + 2, f.size() - 2, back, bseq) && bseq == seq && same_report(r, back));

    const OrderId id = rng.next();
    const Side s = rng.below(2) ? Side::Sell : Side::Buy;
    const OrderType t = static_cast<OrderType>(rng.below(5));
    const Qty q = static_cast<Qty>(rng.next());
    const Price p = PX(static_cast<std::int64_t>(rng.next()));
    ClientMsg m;
    Bytes a = frame_new(seq, id, s, t, q, p);
    CHECK(decode_client(a.data() + 2, a.size() - 2, m) && m.seq == seq && m.id == id && m.side == s &&
          m.otype == t && m.qty == q && m.price == p);
    Bytes b = frame_cancel(seq, id);
    CHECK(decode_client(b.data() + 2, b.size() - 2, m) && m.kind == kMsgCancel && m.id == id);
    Bytes c = frame_replace(seq, id, q, p);
    CHECK(decode_client(c.data() + 2, c.size() - 2, m) && m.kind == kMsgReplace && m.id == id && m.qty == q &&
          m.price == p);
  }
}

void test_codec_malformed() {
  ClientMsg m;
  Report r{};
  std::uint64_t seq;
  const Bytes frames[] = {frame_new(1, 2, Side::Buy, OrderType::Limit, 5, PX(6)), frame_cancel(1, 2),
                          frame_replace(1, 2, 5, PX(6))};
  for (const Bytes& f : frames) {
    const std::uint8_t* body = f.data() + 2;
    const std::size_t len = f.size() - 2;
    CHECK(decode_client(body, len, m));
    for (std::size_t k = 0; k < len; ++k) CHECK(!decode_client(body, k, m));  // every truncation
    Bytes longer(body, body + len);
    longer.push_back(0);
    CHECK(!decode_client(longer.data(), longer.size(), m));  // trailing byte
    Bytes bad(body, body + len);
    bad[0] = 'Z';
    CHECK(!decode_client(bad.data(), bad.size(), m));
    bad[0] = 'R';  // a server message type is not a valid client message
    CHECK(!decode_client(bad.data(), bad.size(), m));
  }
  Bytes n = frame_new(1, 2, Side::Buy, OrderType::Limit, 5, PX(6));
  n[2 + 9 + 8] = 2;  // side out of range
  CHECK(!decode_client(n.data() + 2, n.size() - 2, m));
  n[2 + 9 + 8] = 0;
  n[2 + 9 + 9] = 5;  // order type out of range
  CHECK(!decode_client(n.data() + 2, n.size() - 2, m));
  n[2 + 9 + 9] = 4;
  CHECK(decode_client(n.data() + 2, n.size() - 2, m));

  Bytes rp = frame_report(1, mkrep(ReportType::Accepted, 3, 4, 4));
  const std::uint8_t* body = rp.data() + 2;
  CHECK(decode_report(body, rp.size() - 2, r, seq));
  for (std::size_t k = 0; k < rp.size() - 2; ++k) CHECK(!decode_report(body, k, r, seq));
  Bytes bad(body, body + rp.size() - 2);
  bad[9] = 7;  // report type
  CHECK(!decode_report(bad.data(), bad.size(), r, seq));
  bad[9] = 0;
  bad[10] = 14;  // reason
  CHECK(!decode_report(bad.data(), bad.size(), r, seq));
  bad[10] = 0;
  bad[11] = 3;  // liquidity
  CHECK(!decode_report(bad.data(), bad.size(), r, seq));
  bad[11] = 0;
  bad[12] = 2;  // side
  CHECK(!decode_report(bad.data(), bad.size(), r, seq));
  bad[12] = 0;
  bad[0] = 'O';
  CHECK(!decode_report(bad.data(), bad.size(), r, seq));
}

// ---- stream reassembly -----------------------------------------------------------

struct Collect {
  std::vector<Bytes> frames;
  void on_frame(const std::uint8_t* b, std::size_t n) { frames.emplace_back(b, b + n); }
};

void test_stream_decoder(std::uint64_t n) {
  Rng rng(7);
  Bytes stream;
  std::vector<Bytes> bodies;
  for (std::uint64_t i = 0; i < n / 50 + 10; ++i) {
    Bytes f;
    switch (rng.below(4)) {
      case 0: f = frame_new(i, rng.next(), Side::Buy, OrderType::Limit, 5, PX(6)); break;
      case 1: f = frame_cancel(i, rng.next()); break;
      case 2: f = frame_replace(i, rng.next(), 5, PX(6)); break;
      default: f = frame_report(i, random_report(rng)); break;
    }
    bodies.emplace_back(f.begin() + 2, f.end());
    stream.insert(stream.end(), f.begin(), f.end());
  }
  auto run = [&](auto chunker) {
    StreamDecoder d;
    Collect c;
    std::size_t pos = 0;
    while (pos < stream.size()) {
      const std::size_t k = std::min(chunker(), stream.size() - pos);
      CHECK(d.feed(stream.data() + pos, k, c) == StreamDecoder::Status::Ok);
      pos += k;
    }
    CHECK(d.buffered() == 0);
    CHECK(c.frames.size() == bodies.size());
    for (std::size_t i = 0; i < c.frames.size() && i < bodies.size(); ++i) CHECK(c.frames[i] == bodies[i]);
  };
  run([&] { return stream.size(); });                     // all at once
  run([&] { return std::size_t{1}; });                    // a byte at a time
  run([&] { return std::size_t{1 + rng.below(7)}; });     // tiny chunks straddling headers and bodies
  run([&] { return std::size_t{1 + rng.below(300)}; });   // mixed
  // Every possible single split point.
  for (std::size_t cut = 1; cut < std::min<std::size_t>(stream.size(), 400); ++cut) {
    StreamDecoder d;
    Collect c;
    d.feed(stream.data(), cut, c);
    d.feed(stream.data() + cut, stream.size() - cut, c);
    CHECK(c.frames.size() == bodies.size());
  }
  {  // corrupt streams
    StreamDecoder d;
    Collect c;
    const std::uint8_t zero_len[] = {0, 0, 1, 2};
    CHECK(d.feed(zero_len, sizeof zero_len, c) == StreamDecoder::Status::Broken && d.broken());
    CHECK(d.feed(stream.data(), stream.size(), c) == StreamDecoder::Status::Broken && c.frames.empty());
    d.reset();
    CHECK(d.feed(stream.data(), stream.size(), c) == StreamDecoder::Status::Ok && c.frames.size() == bodies.size());
    StreamDecoder e;
    Collect c2;
    const std::uint8_t too_long[] = {0, 72};
    CHECK(e.feed(too_long, 1, c2) == StreamDecoder::Status::Ok);  // length not complete yet
    CHECK(e.feed(too_long + 1, 1, c2) == StreamDecoder::Status::Broken);
    StreamDecoder f2;
    const std::uint8_t partial[] = {0, 17, 'X', 1};
    f2.feed(partial, sizeof partial, c2);
    CHECK(f2.buffered() == 4 && !f2.broken() && c2.frames.empty());
  }
}

// ---- gateway unit tests ---------------------------------------------------------------

// GCC 13 at -O1 cannot see that only the encoded prefix of the gateway's stack buffer is read.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
struct Captured {
  Bytes bytes;
  void operator()(const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }
};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
struct CapSend {
  Captured* c;
  void operator()(const std::uint8_t* d, std::size_t n) { (*c)(d, n); }
};

struct Decoded {
  std::vector<ClientMsg> msgs;
  void on_frame(const std::uint8_t* b, std::size_t n) {
    ClientMsg m;
    CHECK(decode_client(b, n, m));
    msgs.push_back(m);
  }
};
Decoded decode_all(const Bytes& b) {
  Decoded d;
  StreamDecoder s;
  CHECK(s.feed(b.data(), b.size(), d) == StreamDecoder::Status::Ok);
  return d;
}

risk::RiskConfig gw_risk() {
  risk::RiskConfig c;
  c.max_order_qty = 100;
  c.max_order_notional_raw = 1'000'000'000'000LL;
  c.price_band_bps = 500;
  c.max_position = 300;
  c.max_open_orders = 50;
  c.rate_per_sec = 1'000'000'000;
  c.burst = 1000;
  c.max_loss_raw = 0;
  return c;
}

struct GwRig {
  core::Region region = make_region(8);
  core::Arena arena{region};
  Captured cap;
  Gateway<CapSend> gw{CapSend{&cap}};
  std::uint64_t in_seq = 0;
  explicit GwRig(risk::RiskConfig rc = gw_risk()) {
    CHECK(gw.init(arena, oms::OmsConfig{256}, rc));
    gw.risk().set_reference(PX(1'200'000));
  }
  void deliver(const Report& r) {
    Bytes f = frame_report(++in_seq, r);
    gw.on_bytes(f.data(), f.size(), 10);
  }
};

void test_gateway_basic() {
  GwRig g;
  const SubmitResult a = g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 10, 1);
  CHECK(a.ok() && a.id == 1);
  const SubmitResult b = g.gw.new_order(Side::Sell, OrderType::Market, PX(55), 5, 2);  // price field is dropped
  CHECK(b.ok() && b.id == 2);
  Decoded d = decode_all(g.cap.bytes);
  CHECK(d.msgs.size() == 2);
  CHECK(d.msgs[0].kind == kMsgNew && d.msgs[0].seq == 1 && d.msgs[0].id == 1 && d.msgs[0].qty == 10);
  CHECK(d.msgs[0].side == Side::Buy && d.msgs[0].otype == OrderType::Limit && d.msgs[0].price.raw() == 1'200'000);
  CHECK(d.msgs[1].seq == 2 && d.msgs[1].otype == OrderType::Market && d.msgs[1].price.raw() == 0);
  CHECK(g.gw.oms().find(1)->state == State::PendingNew && g.gw.oms().position().open_buy == 10);

  // risk rejects nothing reaches the wire or the OMS
  const std::size_t sent = g.cap.bytes.size();
  const SubmitResult c = g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 101, 3);
  CHECK(!c.ok() && c.risk == RiskResult::MaxOrderQty && c.id == 0);
  const SubmitResult e = g.gw.new_order(Side::Buy, OrderType::Limit, PX(2'000'000), 1, 3);
  CHECK(e.risk == RiskResult::PriceBand);
  CHECK(g.cap.bytes.size() == sent && g.gw.oms().open_orders() == 2 && g.gw.stats().risk_rejects == 2);
  CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 1, 3).id == 3);  // ids stay dense

  // A cancel or replace is refused while the order is still PendingNew, and nothing is sent.
  const std::size_t before = g.cap.bytes.size();
  CHECK(g.gw.cancel(1, 4) == oms::Request::NotAllowed);
  CHECK(g.gw.replace(1, PX(1'200'000), 5, 4).req == oms::Request::NotAllowed);
  CHECK(g.cap.bytes.size() == before);
  g.deliver(mkrep(ReportType::Accepted, 1, 10, 10));
  g.deliver(mkrep(ReportType::Accepted, 3, 1, 1));
  CHECK(g.gw.cancel(1, 5) == oms::Request::Ok);
  CHECK(g.gw.cancel(1, 5) == oms::Request::NotAllowed);  // one request at a time
  const ReplaceResult rr = g.gw.replace(3, PX(1'200'100), 20, 6);
  CHECK(rr.ok());
  CHECK(g.gw.replace(99, PX(1'200'100), 20, 6).req == oms::Request::Unknown);
  const std::size_t after_ok = g.cap.bytes.size();
  CHECK(g.gw.replace(3, PX(1'200'100), 20, 7).req == oms::Request::NotAllowed);  // still pending
  g.deliver(mkrep(ReportType::Replaced, 3, 20, 20, 1'200'100));
  CHECK(g.gw.replace(3, PX(1'200'100), 101, 7).risk == RiskResult::MaxOrderQty);
  CHECK(g.gw.replace(3, PX(3'000'000), 5, 7).risk == RiskResult::PriceBand);
  CHECK(g.cap.bytes.size() == after_ok);
  Decoded d2 = decode_all(g.cap.bytes);
  CHECK(d2.msgs.size() == 5);
  CHECK(d2.msgs[3].kind == kMsgCancel && d2.msgs[3].id == 1 && d2.msgs[3].seq == 4);
  CHECK(d2.msgs[4].kind == kMsgReplace && d2.msgs[4].id == 3 && d2.msgs[4].qty == 20 && d2.msgs[4].price.raw() == 1'200'100);
  // The five messages we sent carry 1..5 in order; a gap on our side would be a bug.
  for (std::size_t k = 0; k < d2.msgs.size(); ++k) CHECK(d2.msgs[k].seq == k + 1);
  CHECK(g.gw.stats().reports == 3 && g.gw.oms().stats().anomalies == 0 && g.gw.oms().check_invariants());
}

void test_gateway_close_order() {
  GwRig g;
  // nothing to close
  CHECK(g.gw.close_order(Side::Sell, PX(1'199'000), 5, 1).risk == RiskResult::NotReducing);
  CHECK(g.cap.bytes.empty() && g.gw.stats().sent_new == 0 && g.gw.stats().risk_rejects == 1);
  // a long position of 10 lots
  CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 10, 1).ok());
  g.deliver(mkrep(ReportType::Accepted, 1, 10, 10));
  g.deliver(mkrep(ReportType::Fill, 1, 10, 0));
  CHECK(g.gw.oms().position().pos == 10 && g.gw.oms().open_orders() == 0);
  const std::uint64_t sent = g.gw.stats().sent_new;
  // the wrong way, too much: refused, nothing sent
  CHECK(g.gw.close_order(Side::Buy, PX(1'201'000), 5, 2).risk == RiskResult::NotReducing);
  CHECK(g.gw.close_order(Side::Sell, PX(1'199'000), 11, 2).risk == RiskResult::NotReducing);
  CHECK(g.gw.stats().sent_new == sent);
  // the kill switch does not stop it; a new order is stopped
  g.gw.kill(risk::KillReason::Manual, 3);
  CHECK(g.gw.new_order(Side::Sell, OrderType::Limit, PX(1'199'000), 5, 3).risk == RiskResult::KillSwitch);
  const std::size_t before = g.cap.bytes.size();
  const SubmitResult c = g.gw.close_order(Side::Sell, PX(1'199'000), 10, 3);
  CHECK(c.ok() && c.id == 2);
  CHECK(g.gw.stats().sent_new == sent + 1);
  const oms::OrderRec* o = g.gw.oms().find(c.id);
  CHECK(o != nullptr && o->type == OrderType::IOC && o->side == Side::Sell && o->qty == 10 && o->state == State::PendingNew);
  Decoded d = decode_all(g.cap.bytes);
  const ClientMsg& m = d.msgs.back();
  CHECK(g.cap.bytes.size() > before && m.kind == kMsgNew && m.id == c.id && m.otype == OrderType::IOC && m.side == Side::Sell);
  CHECK(m.qty == 10 && m.price.raw() == 1'199'000);
  CHECK(g.gw.oms().check_invariants());
  // while that order is out, another closing order is refused (a working order could flip the position)
  CHECK(g.gw.close_order(Side::Sell, PX(1'199'000), 1, 4).risk == RiskResult::NotReducing);
}

// A fresh rig with orders 1 and 2 sent and acknowledged.
void working_pair(GwRig& g) {
  CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 10, 1).ok());
  CHECK(g.gw.new_order(Side::Sell, OrderType::Limit, PX(1'200'500), 10, 1).ok());
  g.deliver(mkrep(ReportType::Accepted, 1, 10, 10));
  g.deliver(mkrep(ReportType::Accepted, 2, 10, 10, 1'200'500, Reason::None, Side::Sell));
}

std::uint64_t count_kind(const Bytes& b, std::uint8_t kind) {
  std::uint64_t n = 0;
  for (const ClientMsg& m : decode_all(b).msgs) n += m.kind == kind;
  return n;
}

void test_gateway_session_faults() {
  {  // a gap in the exchange's report sequence
    GwRig g;
    working_pair(g);
    Bytes f = frame_report(g.in_seq + 2, mkrep(ReportType::Fill, 1, 4, 6));  // skips one
    g.gw.on_bytes(f.data(), f.size(), 20);
    CHECK(g.gw.stats().seq_gaps == 1 && g.gw.risk().killed() && g.gw.risk().kill_reason() == KillReason::SessionGap);
    CHECK(g.gw.oms().position().pos == 4);  // the report itself is still applied
    CHECK(count_kind(g.cap.bytes, kMsgCancel) == 2 && g.gw.stats().kill_cancels == 2);
    CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 1, 21).risk == RiskResult::KillSwitch);
    // replaying a sequence number we already applied is dropped, not applied twice
    g.gw.on_bytes(f.data(), f.size(), 22);
    CHECK(g.gw.stats().seq_dups == 1 && g.gw.oms().position().pos == 4);
  }
  {  // a well-framed but undecodable report
    GwRig g;
    working_pair(g);
    Bytes f = frame_report(g.in_seq + 1, mkrep(ReportType::Fill, 1, 4, 6));
    f[2 + 9] = 99;  // report type
    g.gw.on_bytes(f.data(), f.size(), 20);
    CHECK(g.gw.stats().bad_messages == 1 && g.gw.risk().killed() && g.gw.oms().position().pos == 0);
    CHECK(g.gw.stats().kill_cancels == 2);
  }
  {  // a corrupt byte stream
    GwRig g;
    working_pair(g);
    const std::uint8_t junk[] = {0, 0, 1, 2, 3};
    g.gw.on_bytes(junk, sizeof junk, 20);
    CHECK(g.gw.stats().broken_streams == 1 && g.gw.risk().kill_reason() == KillReason::Disconnect);
    g.gw.on_bytes(junk, sizeof junk, 21);  // ignored, and not counted again
    CHECK(g.gw.stats().broken_streams == 1 && g.gw.stats().kill_cancels == 2);
  }
  {  // kill while an order is still unacknowledged: cancel it the moment it is acknowledged
    GwRig g;
    CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 10, 1).ok());
    g.gw.kill(KillReason::Manual, 2);
    CHECK(g.gw.stats().kill_cancels == 0 && g.gw.oms().find(1)->state == State::PendingNew);
    g.deliver(mkrep(ReportType::Accepted, 1, 10, 10));
    CHECK(g.gw.stats().kill_cancels == 1 && g.gw.oms().find(1)->state == State::PendingCancel);
    g.gw.kill(KillReason::Manual, 3);  // idempotent: no second cancel for an order already being cancelled
    CHECK(g.gw.stats().kill_cancels == 1 && count_kind(g.cap.bytes, kMsgCancel) == 1);
    g.deliver(mkrep(ReportType::Canceled, 1, 10, 0, 1'200'000, Reason::UserRequested));
    CHECK(g.gw.oms().open_orders() == 0 && g.gw.oms().find(1)->state == State::Canceled);
    // cancels are still allowed after the kill; new risk is not
    CHECK(g.gw.new_order(Side::Buy, OrderType::Limit, PX(1'200'000), 1, 4).risk == RiskResult::KillSwitch);
    CHECK(g.gw.risk().kill_reason() == KillReason::Manual);
  }
  {  // a stream cut mid-frame resumes cleanly when the rest arrives
    GwRig g;
    working_pair(g);
    Bytes f = frame_report(g.in_seq + 1, mkrep(ReportType::Fill, 1, 4, 6));
    for (std::size_t k = 0; k < f.size(); ++k) g.gw.on_bytes(&f[k], 1, 30);
    CHECK(g.gw.oms().position().pos == 4 && g.gw.stats().seq_gaps == 0 && !g.gw.risk().killed());
  }
}

// ---- end to end: gateway <-> bytes <-> exchange simulator -----------------------------------

struct World;

struct ClientSend {
  World* w;
  void operator()(const std::uint8_t* d, std::size_t n);
};
struct ToClient {
  World* w;
  void operator()(Owner p, const std::uint8_t* d, std::size_t n, std::uint64_t t);
};

struct WorldCfg {
  std::uint64_t ingress = 0, egress = 0, md = 0, jitter = 0;
  bool chunk = false;          // deliver reports to the client in random pieces
  risk::RiskConfig risk = gw_risk();
  std::uint64_t step_ns = 100;
  std::uint32_t client_pct = 90;  // chance per step that the client acts
};

struct World {
  core::Region region = make_region(96);
  core::Arena arena{region};
  ExchangeSim sim;
  gw::ExchangeEdge<ToClient> edge;
  Gateway<ClientSend> gw;
  WorldCfg cfg;
  std::uint64_t now = 0;
  Rng rng{2468};
  testsupport::FlowGen bg;
  std::int64_t ref_raw = kBase + 2048 * kTick;
  // independent tally of our fills, straight from the exchange's reports
  std::int64_t t_pos = 0;
  core::i128 t_cash = 0;
  std::uint64_t t_fills = 0, reports_to_client = 0;
  std::uint64_t exposure_violations = 0;

  explicit World(const WorldCfg& c, std::uint64_t seed = 1)
      : edge(sim, ToClient{this}),
        gw(ClientSend{this}),
        cfg(c),
        rng(seed * 7919 + 1),
        bg(testsupport::FlowConfig{PX(kBase), kTick, kLevels, 2, true}, seed) {
    SimConfig sc;
    sc.engine.base = PX(kBase);
    sc.engine.tick_raw = kTick;
    sc.engine.num_levels = kLevels;
    sc.engine.max_orders = 1u << 15;
    sc.latency.ingress_ns = c.ingress;
    sc.latency.egress_ns = c.egress;
    sc.latency.md_ns = c.md;
    sc.latency.jitter_ns = c.jitter;
    sc.latency.seed = seed;
    sc.queue_capacity = 1u << 16;
    sc.max_participants = 8;
    CHECK(sim.init(arena, sc));
    CHECK(edge.init(arena, 8));
    CHECK(gw.init(arena, oms::OmsConfig{1u << 16}, c.risk));
    gw.risk().set_reference(PX(ref_raw));
  }

  struct Sink {
    World* w;
    void on_report(Owner p, const Report& r, std::uint64_t t) {
      if (p == 1 && r.type == ReportType::Fill) {
        const std::int64_t sg = r.side == Side::Buy ? 1 : -1;
        w->t_pos += sg * r.qty;
        w->t_cash -= static_cast<core::i128>(sg) * r.price.raw() * r.qty;
        ++w->t_fills;
      }
      w->edge.on_report(p, r, t);
    }
    void on_public(const sim::PublicEvent&, std::uint64_t) {}
  };

  void update_reference() {
    const Price b = sim.engine().best_bid(), a = sim.engine().best_ask();
    if (b != Price::invalid() && a != Price::invalid()) {
      const std::int64_t mid = b.raw() + (a.raw() - b.raw()) / 2;
      ref_raw = kBase + (mid - kBase) / kTick * kTick;
    }
    gw.risk().set_reference(PX(ref_raw));
  }

  std::int64_t near_price(Side s) {
    const std::int64_t off = static_cast<std::int64_t>(rng.below(9)) - 4;
    (void)s;
    return ref_raw + off * kTick;
  }

  void check_exposure() {
    const oms::Position& p = gw.oms().position();
    if (p.pos + p.open_buy > cfg.risk.max_position || p.pos - p.open_sell < -cfg.risk.max_position)
    {
      if (exposure_violations == 0 && std::getenv("GW_DEBUG"))
        std::printf("VIOLATION pos %lld ob %lld os %lld max %lld t=%llu\n", (long long)p.pos, (long long)p.open_buy,
                    (long long)p.open_sell, (long long)cfg.risk.max_position, (unsigned long long)now);
      ++exposure_violations;
    }
  }

  void client_act() {
    const std::uint32_t r = rng.below(100);
    if (r < 55) {
      const Side s = rng.below(2) ? Side::Sell : Side::Buy;
      const std::uint32_t k = rng.below(100);
      OrderType t = OrderType::Limit;
      if (k >= 55) t = k < 60 ? OrderType::Market : k < 70 ? OrderType::IOC : k < 75 ? OrderType::FOK : OrderType::PostOnly;
      gw.new_order(s, t, PX(near_price(s)), 1 + static_cast<Qty>(rng.below(30)), now);
      check_exposure();
      return;
    }
    // pick a random open order
    OrderId ids[64];
    std::size_t n = 0;
    gw.oms().for_each_open([&](const oms::OrderRec& o) {
      if (n < 64) ids[n++] = o.id;
    });
    if (n == 0) return;
    const OrderId id = ids[rng.below(static_cast<std::uint32_t>(n))];
    if (r < 80) {
      gw.cancel(id, now);
    } else {
      gw.replace(id, PX(near_price(Side::Buy)), 1 + static_cast<Qty>(rng.below(60)), now);
      check_exposure();
    }
  }

  void background() {
    const testsupport::Op op = bg.next();
    const std::uint64_t kOff = 1'000'000'000ULL;
    const Owner p = static_cast<Owner>(op.kind == testsupport::Op::New ? op.o.owner + 1 : op.owner + 1);
    switch (op.kind) {
      case testsupport::Op::New: {
        sim::NewOrder o = op.o;
        o.id += kOff;
        sim.submit_new(p, o, now);
        break;
      }
      case testsupport::Op::Cancel: sim.submit_cancel(p, op.id + kOff, now); break;
      case testsupport::Op::Replace: sim.submit_replace(p, op.id + kOff, op.price, op.qty, now); break;
    }
  }

  void step() {
    now += cfg.step_ns;
    update_reference();
    if (rng.below(100) < cfg.client_pct) client_act();
    background();
    Sink sink{this};
    sim.advance_to(now, sink);
    check_exposure();
  }

  void drain() {
    Sink sink{this};
    sim.advance_to(~std::uint64_t{0} >> 1, sink);
  }

  // Everything the client believes must match what the exchange actually holds.
  void verify(bool expect_races) {
    const oms::Oms& o = gw.oms();
    const oms::OmsStats& st = o.stats();
    CHECK(st.anomalies == 0 && st.overfills == 0 && st.mismatches == 0 && st.orphan_fills == 0 &&
          st.unknown_reports == 0);
    CHECK(o.check_invariants());
    CHECK(exposure_violations == 0);
    CHECK(gw.stats().seq_gaps == 0 && gw.stats().seq_dups == 0 && gw.stats().bad_messages == 0 &&
          gw.stats().broken_streams == 0);
    CHECK(edge.stats().malformed == 0 && edge.stats().seq_gaps == 0 && edge.stats().refused == 0 &&
          edge.stats().broken == 0);
    CHECK(sim.dropped_out() == 0);
    CHECK(o.position().pos == t_pos && o.position().cash == t_cash && o.position().fills == t_fills);
    if (expect_races) CHECK(st.ignored > 0);  // cancels really did cross fills on the wire
    // Open orders at the client are exactly the orders resting at the exchange.
    std::uint64_t open = 0;
    o.for_each_open([&](const oms::OrderRec& r) {
      ++open;
      CHECK(r.state == State::New || r.state == State::PartiallyFilled);
      CHECK(sim.engine().contains(r.id) && sim.engine().order_qty(r.id) == r.leaves);
    });
    CHECK(open == o.open_orders());
    for (OrderId id = 1; id <= gw.stats().sent_new + gw.stats().risk_rejects + 1 && id < 100000; ++id) {
      const oms::OrderRec* r = o.find(id);
      if (!r) continue;
      if (oms::is_terminal(r->state)) CHECK(!sim.engine().contains(id));
    }
  }
};

void ClientSend::operator()(const std::uint8_t* d, std::size_t n) { w->edge.on_client_bytes(1, d, n, w->now); }
void ToClient::operator()(Owner p, const std::uint8_t* d, std::size_t n, std::uint64_t t) {
  if (p != 1) return;
  ++w->reports_to_client;
  if (w->cfg.chunk && n > 1) {
    const std::size_t cut = 1 + w->rng.below(static_cast<std::uint32_t>(n - 1));
    w->gw.on_bytes(d, cut, t);
    w->gw.on_bytes(d + cut, n - cut, t);
  } else {
    w->gw.on_bytes(d, n, t);
  }
}

void run_world(const char* name, const WorldCfg& c, std::uint64_t steps, std::uint64_t seed, bool races) {
  World w(c, seed);
  for (std::uint64_t i = 0; i < steps; ++i) w.step();
  w.drain();
  w.verify(races);
  const auto& s = w.gw.stats();
  std::printf("  %-22s sent new/cancel/replace %llu/%llu/%llu  risk rejects %llu  fills %llu  late rejects ignored %llu  pos %lld\n",
              name, static_cast<unsigned long long>(s.sent_new), static_cast<unsigned long long>(s.sent_cancel),
              static_cast<unsigned long long>(s.sent_replace), static_cast<unsigned long long>(s.risk_rejects),
              static_cast<unsigned long long>(w.t_fills),
              static_cast<unsigned long long>(w.gw.oms().stats().ignored), static_cast<long long>(w.t_pos));
  CHECK(s.sent_new > steps / 8 && w.t_fills > 0 && s.sent_cancel > 0 && s.sent_replace > 0);
}

void test_end_to_end(std::uint64_t steps) {
  WorldCfg zero;
  run_world("zero latency", zero, steps, 1, false);
  WorldCfg lat;
  lat.ingress = 300;
  lat.egress = 200;
  lat.md = 100;
  lat.jitter = 250;
  run_world("latency + jitter", lat, steps, 2, true);
  lat.chunk = true;
  run_world("latency, chunked bytes", lat, steps, 3, true);
  WorldCfg slow = lat;
  slow.ingress = 5000;  // many orders are in flight at once
  slow.egress = 4000;
  slow.jitter = 3000;
  run_world("slow network", slow, steps, 4, true);
  WorldCfg tight = lat;
  tight.risk.rate_per_sec = 2'000'000;  // binds: the client tries ~9M orders/s
  tight.risk.burst = 10;
  tight.risk.max_open_orders = 12;
  tight.risk.max_position = 60;
  World w(tight, 5);
  for (std::uint64_t i = 0; i < steps; ++i) w.step();
  w.drain();
  w.verify(true);
  CHECK(w.gw.stats().risk_rejects > 0);
  std::printf("  %-22s risk rejects %llu of %llu attempts\n", "tight limits",
              static_cast<unsigned long long>(w.gw.stats().risk_rejects),
              static_cast<unsigned long long>(w.gw.stats().risk_rejects + w.gw.stats().sent_new));
}

void test_kill_switch_end_to_end(std::uint64_t steps) {
  WorldCfg c;
  c.ingress = 400;
  c.egress = 300;
  c.jitter = 300;
  World w(c, 11);
  for (std::uint64_t i = 0; i < steps; ++i) w.step();
  CHECK(w.gw.oms().open_orders() > 0);  // there is something to cancel, some of it still in flight
  w.gw.kill(KillReason::Manual, w.now);
  const std::uint64_t new_before = w.gw.stats().sent_new;
  for (std::uint64_t i = 0; i < steps / 4; ++i) w.step();  // the strategy keeps trying
  w.drain();
  CHECK(w.gw.stats().sent_new == new_before);  // nothing new left after the switch
  CHECK(w.gw.oms().open_orders() == 0);
  CHECK(w.gw.stats().kill_cancels > 0);
  for (OrderId id = 1; id < new_before + 100; ++id) CHECK(!w.sim.engine().contains(id));
  w.verify(false);
  CHECK(w.gw.risk().kill_reason() == KillReason::Manual);
}

void test_loss_limit_end_to_end(std::uint64_t steps) {
  WorldCfg c;
  c.ingress = 200;
  c.egress = 200;
  c.risk.max_loss_raw = 400'000;  // a few hundred units of spread crossing
  World w(c, 13);
  for (std::uint64_t i = 0; i < steps && !w.gw.risk().killed(); ++i) w.step();
  CHECK(w.gw.risk().killed() && w.gw.risk().kill_reason() == KillReason::MaxLoss);
  for (int i = 0; i < 200; ++i) w.step();
  w.drain();
  CHECK(w.gw.oms().open_orders() == 0);
  for (OrderId id = 1; id < w.gw.stats().sent_new + 100; ++id) CHECK(!w.sim.engine().contains(id));
  w.verify(false);
}

// The client path allocates nothing once running.
void test_no_allocation(std::uint64_t steps) {
  WorldCfg c;
  c.ingress = 300;
  c.egress = 200;
  c.jitter = 200;
  World w(c, 17);
  std::vector<testsupport::Op> ops;
  ops.reserve(steps);
  testsupport::FlowGen gen(testsupport::FlowConfig{PX(kBase), kTick, kLevels, 2, true}, 99);
  for (std::uint64_t i = 0; i < steps; ++i) ops.push_back(gen.next());
  alloc_trap::arm();
  for (std::uint64_t i = 0; i < steps; ++i) {
    w.now += 100;
    w.update_reference();
    w.client_act();
    const testsupport::Op& op = ops[i];
    const Owner p = static_cast<Owner>(op.kind == testsupport::Op::New ? op.o.owner + 1 : op.owner + 1);
    if (op.kind == testsupport::Op::New) {
      sim::NewOrder o = op.o;
      o.id += 1'000'000'000ULL;
      w.sim.submit_new(p, o, w.now);
    } else if (op.kind == testsupport::Op::Cancel) {
      w.sim.submit_cancel(p, op.id + 1'000'000'000ULL, w.now);
    } else {
      w.sim.submit_replace(p, op.id + 1'000'000'000ULL, op.price, op.qty, w.now);
    }
    World::Sink sink{&w};
    w.sim.advance_to(w.now, sink);
  }
  w.drain();
  const std::uint64_t allocs = alloc_trap::disarm();
  CHECK(allocs == 0);
  std::printf("  alloc test: sent_new %llu fills %llu allocs %llu\n", (unsigned long long)w.gw.stats().sent_new, (unsigned long long)w.t_fills, (unsigned long long)allocs);
  CHECK(w.gw.stats().sent_new > steps / 20 && w.t_fills > 0 && w.gw.oms().check_invariants());
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000ULL;
  test_codec_golden();
  test_codec_roundtrip(n);
  test_codec_malformed();
  test_stream_decoder(n);
  test_gateway_basic();
  test_gateway_close_order();
  test_gateway_session_faults();
  std::puts("codec and gateway unit tests done");
  test_end_to_end(n);
  test_kill_switch_end_to_end(n / 4);
  test_loss_limit_end_to_end(n);
  test_no_allocation(n);
  if (g_failures == 0) std::puts("test_gw: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
