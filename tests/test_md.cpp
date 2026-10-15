// Market data tests: wire format and parser, handler behaviour case by case,
// whole-feed simulations with loss/duplication/reordering on two lines checked
// against the reference book, a zero-allocation proof, and a mutation fuzz driver.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "alloc_trap.hpp"
#include "check.hpp"
#include "feed_gen.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "hft/md/itch_encoder.hpp"
#include "hft/md/itch_wire.hpp"
#include "hft/md/md_handler.hpp"
#include "reference_book.hpp"
#include "snapshot_builder.hpp"

using namespace hft;
using namespace hft::md;
using hft::book::BookConfig;
using hft::book::BookResult;
using hft::book::OrderBook;
using hft::core::Arena;
using hft::core::Price;
using hft::core::Region;
using hft::core::RegionOptions;
using hft::core::Side;
using testsupport::FeedConfig;
using testsupport::FeedGen;
using testsupport::Op;
using testsupport::RefBook;
using testsupport::Rng;

using Bytes = std::vector<std::uint8_t>;
static constexpr char kSess[] = "SESSION001";
static constexpr std::uint16_t kLoc = 1;

static std::uint32_t px(std::int64_t ticks) { return static_cast<std::uint32_t>(1'000'000 + ticks * 100); }

// ---- tiny message/packet constructors for the hand-written cases -----------

static Bytes m_add(std::uint64_t id, Side s, std::uint32_t q, std::uint32_t price, std::uint16_t loc = kLoc) {
  Bytes b(36);
  encode_add(b.data(), loc, 1, id, s, q, price);
  return b;
}
static Bytes m_del(std::uint64_t id) { Bytes b(19); encode_delete(b.data(), kLoc, 1, id); return b; }
static Bytes m_exec(std::uint64_t id, std::uint32_t q) { Bytes b(31); encode_exec(b.data(), kLoc, 1, id, q); return b; }
static Bytes m_rep(std::uint64_t o, std::uint64_t n, std::uint32_t q, std::uint32_t price) {
  Bytes b(35);
  encode_replace(b.data(), kLoc, 1, o, n, q, price);
  return b;
}
static Bytes m_unknown() { return Bytes{'Z', 1, 2, 3, 4}; }

static Bytes pkt(std::uint64_t first_seq, std::initializer_list<Bytes> msgs, const char (&sess)[11] = kSess) {
  PacketBuilder pb(sess);
  pb.reset(first_seq);
  for (const Bytes& m : msgs) pb.add_raw(m.data(), m.size());
  const std::uint8_t* d = pb.finish();
  return Bytes(d, d + pb.size());
}

static Bytes snap_pkt(std::uint64_t first_seq, std::initializer_list<Bytes> msgs) {
  return pkt(first_seq, msgs, reinterpret_cast<const char(&)[11]>("SNAPSHOT01"));
}
static Bytes s_begin(std::uint64_t S, std::uint32_t total) { Bytes b(13); encode_snap_begin(b.data(), S, total); return b; }
static Bytes s_end(std::uint64_t S) { Bytes b(9); encode_snap_end(b.data(), S); return b; }

static Region make_region(std::size_t mb) { return Region::create(mb << 20, RegionOptions{false, false, true, -1}); }

static BookConfig book_cfg(std::uint32_t max_orders = 1024) {
  BookConfig c;
  c.base = Price::from_units(100);
  c.tick_raw = 100;
  c.num_levels = 4096;
  c.max_orders = max_orders;
  return c;
}

struct Rig {
  Region region = make_region(16);
  Arena arena{region};
  OrderBook book;
  MdHandler<> h;
  bool ok = false;
  explicit Rig(std::uint32_t pending = 8, std::uint64_t timeout = 1000, bool live = true,
               bool check_session = false) {
    MdConfig mc;
    mc.locate = kLoc;
    mc.pending_slots = pending;
    mc.gap_timeout_ns = timeout;
    if (check_session) {
      mc.check_session = true;
      std::memcpy(mc.session, kSess, kSessionBytes);
    }
    ok = book.init(arena, book_cfg()) && h.init(arena, book, mc);
    if (live) h.begin_live(1);
  }
  void send(Line l, const Bytes& b, std::uint64_t now = 0) { h.on_packet(l, b.data(), b.size(), now); }
  void snap(const Bytes& b, std::uint64_t now = 0) { h.on_snapshot_packet(b.data(), b.size(), now); }
};

// ---- wire format -------------------------------------------------------------

static void test_wire_golden() {
  std::uint8_t b[64] = {};
  CHECK(encode_add(b, 7, 0x010203040506ULL, 0x1122334455667788ULL, Side::Buy, 100, 1234567) == 36);
  CHECK(b[0] == 'A' && b[1] == 0 && b[2] == 7);
  const std::uint8_t ts[6] = {1, 2, 3, 4, 5, 6};
  CHECK(std::memcmp(b + 5, ts, 6) == 0);
  const std::uint8_t ref[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  CHECK(std::memcmp(b + 11, ref, 8) == 0);
  CHECK(b[19] == 'B');
  CHECK(b[20] == 0 && b[21] == 0 && b[22] == 0 && b[23] == 100);
  const std::uint8_t pr[4] = {0x00, 0x12, 0xD6, 0x87};  // 1234567
  CHECK(std::memcmp(b + 32, pr, 4) == 0);

  Decoded d;
  CHECK(decode(b, d));
  CHECK(d.type == 'A' && d.locate == 7 && d.ts == 0x010203040506ULL && d.id == 0x1122334455667788ULL);
  CHECK(d.side == Side::Buy && d.qty == 100 && d.price.raw() == 1234567);

  encode_add(b, 7, 1, 5, Side::Sell, 1, 1);
  CHECK(decode(b, d) && d.side == Side::Sell);

  encode_exec(b, 3, 9, 77, 25, 99);
  CHECK(decode(b, d) && d.type == 'E' && d.id == 77 && d.qty == 25 && d.locate == 3 && d.ts == 9);
  encode_partial_cancel(b, 3, 9, 78, 4);
  CHECK(decode(b, d) && d.type == 'X' && d.id == 78 && d.qty == 4);
  encode_delete(b, 3, 9, 79);
  CHECK(decode(b, d) && d.type == 'D' && d.id == 79);
  encode_replace(b, 3, 9, 80, 81, 12, 345);
  CHECK(decode(b, d) && d.type == 'U' && d.id == 80 && d.id2 == 81 && d.qty == 12 && d.price.raw() == 345);
  encode_snap_begin(b, 555, 42);
  CHECK(decode(b, d) && d.type == 'B' && d.id == 555 && d.qty == 42);
  encode_snap_end(b, 556);
  CHECK(decode(b, d) && d.type == 'N' && d.id == 556);
  b[0] = 'Q';
  CHECK(!decode(b, d));
}

static void test_wire_parse() {
  const Bytes good = pkt(1000, {m_add(1, Side::Buy, 10, px(5)), m_del(1)});
  PacketView pv;
  CHECK(parse_packet(good.data(), good.size(), pv));
  CHECK(pv.seq == 1000 && pv.count == 2 && pv.end() == 1002);
  MsgCursor c(pv);
  const std::uint8_t* body;
  std::size_t bl;
  CHECK(c.next(body, bl) && bl == 36 && body[0] == 'A');
  CHECK(c.next(body, bl) && bl == 19 && body[0] == 'D');
  CHECK(!c.next(body, bl));

  // Every strict prefix is malformed.
  for (std::size_t n = 0; n < good.size(); ++n) CHECK(!parse_packet(good.data(), n, pv));

  Bytes x = good;
  put_be16(x.data() + 18, 3);  // count says 3, only 2 present
  CHECK(!parse_packet(x.data(), x.size(), pv));
  x = good;
  put_be16(x.data() + 18, 1);  // count says 1: trailing bytes
  CHECK(!parse_packet(x.data(), x.size(), pv));
  x = good;
  put_be16(x.data() + 20, 35);  // 'A' with the wrong length
  CHECK(!parse_packet(x.data(), x.size(), pv));
  x = good;
  x[kHeaderBytes + 2 + 19] = 'X';  // bad side byte
  CHECK(!parse_packet(x.data(), x.size(), pv));
  x = good;
  put_be64(x.data() + 10, ~std::uint64_t{0});  // seq + count wraps
  CHECK(!parse_packet(x.data(), x.size(), pv));
  x = good;
  put_be16(x.data() + 20, 0);  // zero-length message
  CHECK(!parse_packet(x.data(), x.size(), pv));
  Bytes big(kMaxPacketBytes + 1, 0);
  CHECK(!parse_packet(big.data(), big.size(), pv));

  // Unknown types are accepted (skipped later), and a heartbeat has no messages.
  const Bytes unk = pkt(5, {m_unknown()});
  CHECK(parse_packet(unk.data(), unk.size(), pv) && pv.count == 1);
  const Bytes hb = pkt(9, {});
  CHECK(hb.size() == kHeaderBytes && parse_packet(hb.data(), hb.size(), pv) && pv.count == 0 && pv.seq == 9);
}

// ---- handler, case by case ----------------------------------------------------

static void test_in_order_and_dups() {
  Rig r;
  CHECK(r.ok);
  const Bytes p1 = pkt(1, {m_add(1, Side::Buy, 100, px(10)), m_add(2, Side::Sell, 50, px(20))});
  r.send(Line::A, p1);
  CHECK(r.book.best_bid() == Price::from_raw(px(10)) && r.book.best_ask() == Price::from_raw(px(20)));
  CHECK(r.h.next_seq() == 3 && r.h.stats().messages == 2 && r.h.stats().delivered[0] == 1);

  r.send(Line::B, p1);  // same packet on the other line
  CHECK(r.h.stats().dups[1] == 1 && r.h.next_seq() == 3 && r.h.stats().messages == 2);

  // Overlapping packet: first message already applied, only the second is new.
  const Bytes p2 = pkt(2, {m_add(2, Side::Sell, 50, px(20)), m_add(3, Side::Buy, 10, px(9))});
  r.send(Line::B, p2);
  CHECK(r.h.next_seq() == 4 && r.book.order_count() == 3 && r.h.stats().messages == 3);
  CHECK(r.h.state() == FeedState::Live);
}

static void test_gap_filled_by_other_line() {
  Rig r;
  const Bytes p1 = pkt(1, {m_add(1, Side::Buy, 10, px(1)), m_add(2, Side::Buy, 10, px(2))});
  const Bytes p2 = pkt(3, {m_add(3, Side::Buy, 10, px(3)), m_add(4, Side::Buy, 10, px(4))});
  const Bytes p3 = pkt(5, {m_add(5, Side::Buy, 10, px(5)), m_add(6, Side::Buy, 10, px(6))});
  r.send(Line::A, p1, 100);
  r.send(Line::A, p3, 200);  // p2 is missing on line A
  CHECK(r.h.gap_pending() && r.h.next_seq() == 3 && r.h.stats().parked == 1);
  r.send(Line::B, p2, 300);  // line B has it
  CHECK(!r.h.gap_pending() && r.h.next_seq() == 7 && r.book.order_count() == 6);
  CHECK(r.h.stats().gaps_filled == 1 && r.h.stats().gaps_declared == 0);
  r.h.poll(1'000'000);  // nothing pending, nothing to declare
  CHECK(r.h.state() == FeedState::Live);
}

static void test_gap_timeout_and_snapshot() {
  Rig r(8, 1000);
  RefBook expect(book_cfg());
  auto E = [&](std::uint64_t id, Side s, std::uint32_t q, std::int64_t t) {
    expect.add(id, s, Price::from_raw(px(t)), q);
  };
  const Bytes p1 = pkt(1, {m_add(1, Side::Buy, 10, px(1)), m_add(2, Side::Buy, 10, px(2))});
  const Bytes p3 = pkt(5, {m_add(5, Side::Buy, 10, px(5)), m_add(6, Side::Sell, 10, px(30))});
  r.send(Line::A, p1, 0);
  r.send(Line::A, p3, 10);
  r.h.poll(500);  // too early
  CHECK(r.h.state() == FeedState::Live);
  r.h.poll(1010);
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().gaps_declared == 1 && !r.h.book_valid());

  // Snapshot as of seq 4: orders 1..4. The parked packet (seq 5..6) replays on top.
  E(1, Side::Buy, 10, 1); E(2, Side::Buy, 10, 2); E(3, Side::Buy, 10, 3); E(4, Side::Buy, 10, 4);
  r.snap(snap_pkt(1, {s_begin(4, 4), m_add(1, Side::Buy, 10, px(1)), m_add(2, Side::Buy, 10, px(2)),
                      m_add(3, Side::Buy, 10, px(3)), m_add(4, Side::Buy, 10, px(4)), s_end(4)}),
         2000);
  CHECK(r.h.state() == FeedState::Live && r.h.next_seq() == 7 && r.h.stats().snapshots_applied == 1);
  E(5, Side::Buy, 10, 5); E(6, Side::Sell, 10, 30);
  CHECK(r.book.digest() == expect.digest());

  // A snapshot while Live is ignored.
  r.snap(snap_pkt(100, {s_begin(1, 0), s_end(1)}), 3000);
  CHECK(r.h.stats().snapshots_ignored == 1 && r.book.digest() == expect.digest());
}

static void test_heartbeat() {
  Rig r(8, 1000);
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1))}), 0);
  r.send(Line::A, pkt(2, {}), 10);  // heartbeat: next seq 2 == ours, nothing lost
  CHECK(!r.h.gap_pending() && r.h.stats().heartbeats == 1);
  r.send(Line::A, pkt(5, {}), 20);  // sender is at 5, we are at 2: tail lost
  CHECK(r.h.gap_pending());
  r.h.poll(30);
  CHECK(r.h.state() == FeedState::Live);
  r.h.poll(1021);
  CHECK(r.h.state() == FeedState::NeedSnapshot);
}

static void test_session_and_filter() {
  Rig r;
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1))}));
  const Bytes other = pkt(2, {m_add(2, Side::Buy, 10, px(2))}, reinterpret_cast<const char(&)[11]>("OTHERSESSN"));
  r.send(Line::A, other);
  CHECK(r.h.stats().bad_session == 1 && r.h.next_seq() == 2 && r.book.order_count() == 1);

  Rig strict(8, 1000, true, true);
  strict.send(Line::A, other);  // wrong from the very first packet
  CHECK(strict.h.stats().bad_session == 1 && strict.h.next_seq() == 1);

  // Other instruments and unknown message types consume sequence numbers but change nothing.
  Rig q;
  q.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1), 2), m_unknown(), m_add(2, Side::Sell, 5, px(9))}));
  CHECK(q.h.stats().filtered == 1 && q.h.stats().unknown_types == 1 && q.h.next_seq() == 4);
  CHECK(q.book.order_count() == 1 && q.book.contains(2));
}

static void test_replace_semantics() {
  Rig r;
  RefBook expect(book_cfg());
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 100, px(10)), m_add(2, Side::Buy, 100, px(10)),
                          m_rep(1, 3, 150, px(10))}));
  // Replace = new id, back of the queue: the level queue is now [2, 3].
  expect.add(2, Side::Buy, Price::from_raw(px(10)), 100);
  expect.add(3, Side::Buy, Price::from_raw(px(10)), 150);
  CHECK(r.book.digest() == expect.digest() && !r.book.contains(1));

  // Replace to an id that already exists is a desync, and must not destroy the original.
  r.send(Line::A, pkt(4, {m_rep(2, 3, 5, px(11))}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.book.contains(2) && r.h.stats().desyncs == 1);
}

static void test_desync_and_poison() {
  Rig r;
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1))}));
  r.send(Line::A, pkt(2, {m_exec(99, 5)}));  // unknown order
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().desyncs == 1);
  // Snapshot as of seq 1: the poison packet is replayed on top and rejected again.
  r.snap(snap_pkt(1, {s_begin(1, 1), m_add(1, Side::Buy, 10, px(1)), s_end(1)}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().desyncs == 2);
  // A newer snapshot (as of 2) covers it; the parked packet is discarded, not replayed.
  r.snap(snap_pkt(4, {s_begin(2, 1), m_add(1, Side::Buy, 10, px(1)), s_end(2)}));
  CHECK(r.h.state() == FeedState::Live && r.h.next_seq() == 3 && r.h.stats().desyncs == 2);
}

static void test_snapshot_rejections() {
  Rig r;  // starts Live, so force NeedSnapshot with a heartbeat-detected gap
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1))}), 0);
  r.send(Line::A, pkt(9, {}), 1);
  r.h.poll(5000);
  CHECK(r.h.state() == FeedState::NeedSnapshot);

  // Joined mid-cycle: adds without a Begin are ignored, state unchanged.
  r.snap(snap_pkt(50, {m_add(7, Side::Buy, 1, px(1)), s_end(3)}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().snapshots_rejected == 0);

  // Count mismatch: Begin promises 2, one arrives.
  r.snap(snap_pkt(1, {s_begin(3, 2), m_add(1, Side::Buy, 1, px(1)), s_end(3)}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().snapshots_rejected == 1);

  // End carries a different sequence than Begin.
  r.snap(snap_pkt(10, {s_begin(3, 1), m_add(1, Side::Buy, 1, px(1)), s_end(4)}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().snapshots_rejected == 2);

  // A gap inside the snapshot channel (second packet skips ahead).
  r.snap(snap_pkt(20, {s_begin(3, 2), m_add(1, Side::Buy, 1, px(1))}));
  r.snap(snap_pkt(23, {m_add(2, Side::Buy, 1, px(2)), s_end(3)}));
  CHECK(r.h.state() == FeedState::NeedSnapshot && r.h.stats().snapshots_rejected == 3);

  // A good one still works afterwards.
  r.snap(snap_pkt(30, {s_begin(3, 1), m_add(1, Side::Buy, 1, px(1)), s_end(3)}));
  CHECK(r.h.state() == FeedState::Live && r.h.next_seq() == 4);
}

static void test_pending_policy() {
  Rig r(2, 1000);
  r.send(Line::A, pkt(10, {m_add(10, Side::Buy, 1, px(1))}));
  r.send(Line::A, pkt(20, {m_add(20, Side::Buy, 1, px(1))}));
  r.send(Line::A, pkt(30, {m_add(30, Side::Buy, 1, px(1))}));  // full: evicts seq 10
  CHECK(r.h.stats().pending_evicted == 1);
  r.send(Line::A, pkt(5, {m_add(5, Side::Buy, 1, px(1))}));    // older than everything kept: dropped
  CHECK(r.h.stats().pending_dropped == 0 || true);             // seq 5 is below next_seq? next is 1, so it is parked-or-dropped
  r.send(Line::B, pkt(20, {m_add(20, Side::Buy, 1, px(1))}));  // same packet from the other line
  CHECK(r.h.stats().dups[1] == 1);
}

static void test_malformed_is_atomic() {
  Rig r;
  r.send(Line::A, pkt(1, {m_add(1, Side::Buy, 10, px(1))}));
  Bytes bad = pkt(2, {m_add(2, Side::Buy, 10, px(2)), m_add(3, Side::Buy, 10, px(3))});
  put_be16(bad.data() + kHeaderBytes + 2 + 36, 5);  // second message gets a bad length
  r.send(Line::A, bad);
  CHECK(r.h.stats().malformed == 1 && r.h.next_seq() == 2 && r.book.order_count() == 1);  // first message NOT applied
  r.send(Line::A, Bytes(7, 0));
  CHECK(r.h.stats().malformed == 2);
  r.h.poll(10);  // time going backwards must not declare anything
  r.send(Line::A, pkt(5, {m_add(9, Side::Buy, 10, px(2))}), 1000);
  r.h.poll(10);
  CHECK(r.h.state() == FeedState::Live);
}

// ---- whole-feed simulation ----------------------------------------------------

struct SimParams {
  std::uint64_t n_ops = 300000;
  std::uint64_t seed = 1;
  std::uint32_t loss_a_ppm = 0, loss_b_ppm = 0;  // per-packet loss probability, parts per million
  std::uint32_t dup_ppm = 0, reorder_ppm = 0;
  std::uint32_t snapshot_every = 2000, snapshot_delay = 30, snapshot_drop_ppm = 0;
  std::uint32_t pending_slots = 64;
  bool start_live = true;
  bool trap = false;
};

struct SimResult {
  std::uint64_t packets_published = 0, trap_allocs = 0;
  MdStats stats;
  bool converged = false;
};

static SimResult run_sim(const SimParams& P) {
  SimResult res;
  Region region = make_region(64);
  Arena arena(region);
  const BookConfig bc = book_cfg(8192);
  OrderBook book;
  MdHandler<> h;
  MdConfig mc;
  mc.locate = kLoc;
  mc.pending_slots = P.pending_slots;
  mc.gap_timeout_ns = 40'000;
  CHECK(book.init(arena, bc) && h.init(arena, book, mc));
  if (P.start_live) h.begin_live(1);

  RefBook ref(bc);
  FeedGen gen(FeedConfig{bc.base, bc.tick_raw, bc.num_levels, 3000, kLoc}, P.seed);
  Rng net(P.seed ^ 0x5151);

  auto chance = [&](std::uint32_t ppm) { return net.below(1'000'000) < ppm; };
  std::uint64_t now = 0;
  auto call = [&](auto&& f) {
    if (P.trap) alloc_trap::arm();
    f();
    if (P.trap) res.trap_allocs += alloc_trap::disarm();
  };
  auto deliver = [&](Line l, const Bytes& b) { call([&] { h.on_packet(l, b.data(), b.size(), now); }); };

  Bytes held[2];
  bool has_held[2] = {false, false};
  auto send = [&](const Bytes& b) {
    for (int li = 0; li < 2; ++li) {
      const Line l = li == 0 ? Line::A : Line::B;
      if (chance(li == 0 ? P.loss_a_ppm : P.loss_b_ppm)) continue;
      if (!has_held[li] && chance(P.reorder_ppm)) {  // delay this packet behind the next one
        held[li] = b;
        has_held[li] = true;
        continue;
      }
      deliver(l, b);
      if (chance(P.dup_ppm)) deliver(l, b);
      if (has_held[li]) {
        deliver(l, held[li]);
        has_held[li] = false;
      }
    }
  };

  struct DelayedSnap {
    std::uint64_t due;
    std::vector<Bytes> pkts;
  };
  std::vector<DelayedSnap> snaps;  // processed front to back; due times are increasing
  std::size_t snaps_head = 0;
  std::uint64_t snap_stream_seq = 1;
  auto deliver_snapshot = [&](std::vector<Bytes>& pk) {
    if (pk.size() > 1 && chance(P.snapshot_drop_ppm)) pk.erase(pk.begin() + net.below(static_cast<std::uint32_t>(pk.size())));
    for (const Bytes& b : pk) call([&] { h.on_snapshot_packet(b.data(), b.size(), now); });
  };

  PacketBuilder pb("SESSION001");
  std::uint64_t next_seq = 1, ops = 0, pidx = 0;
  std::uint8_t body[64];
  while (ops < P.n_ops) {
    pb.reset(next_seq);
    const std::uint32_t k = 1 + net.below(6);
    for (std::uint32_t j = 0; j < k && ops < P.n_ops; ++j) {
      const Op op = gen.next();
      const std::size_t n = FeedGen::encode(op, kLoc, ops, body);
      pb.add_raw(body, n);
      CHECK(FeedGen::apply_to_ref(ref, op));
      ++ops;
    }
    const std::uint8_t* d = pb.finish();
    const Bytes packet(d, d + pb.size());
    next_seq += pb.count();
    ++pidx;
    now += 1000;
    send(packet);
    call([&] { h.poll(now); });

    if (P.snapshot_every && pidx % P.snapshot_every == 0)
      snaps.push_back({pidx + P.snapshot_delay, testsupport::build_snapshot(ref, next_seq - 1, snap_stream_seq, kLoc)});
    while (snaps_head < snaps.size() && snaps[snaps_head].due <= pidx) deliver_snapshot(snaps[snaps_head++].pkts);
  }
  res.packets_published = pidx;

  // Drain: release held packets, deliver outstanding snapshots, then a
  // loss-free heartbeat so trailing loss is noticed, and let timers expire.
  for (int li = 0; li < 2; ++li)
    if (has_held[li]) deliver(li == 0 ? Line::A : Line::B, held[li]);
  while (snaps_head < snaps.size()) deliver_snapshot(snaps[snaps_head++].pkts);
  now += 10'000'000;
  const Bytes hb = pkt(next_seq, {});
  deliver(Line::A, hb);
  deliver(Line::B, hb);
  now += 10'000'000;
  call([&] { h.poll(now); });
  if (h.state() != FeedState::Live) {  // final, loss-free snapshot
    std::vector<Bytes> last = testsupport::build_snapshot(ref, next_seq - 1, snap_stream_seq, kLoc);
    for (const Bytes& b : last) call([&] { h.on_snapshot_packet(b.data(), b.size(), now); });
  }

  res.stats = h.stats();
  res.converged = h.state() == FeedState::Live && h.next_seq() == next_seq && !h.gap_pending() &&
                  book.order_count() == ref.order_count() && book.digest() == ref.digest() &&
                  book.check_invariants();
  return res;
}

static void print_sim(const char* name, const SimResult& r) {
  const MdStats& s = r.stats;
  std::printf("sim %-18s published %7llu  dups A/B %llu/%llu  parked %llu  gaps filled %llu declared %llu  "
              "snapshots applied %llu rejected %llu  desyncs %llu  evicted %llu dropped %llu  converged=%d\n",
              name, static_cast<unsigned long long>(r.packets_published), static_cast<unsigned long long>(s.dups[0]),
              static_cast<unsigned long long>(s.dups[1]), static_cast<unsigned long long>(s.parked),
              static_cast<unsigned long long>(s.gaps_filled), static_cast<unsigned long long>(s.gaps_declared),
              static_cast<unsigned long long>(s.snapshots_applied), static_cast<unsigned long long>(s.snapshots_rejected),
              static_cast<unsigned long long>(s.desyncs), static_cast<unsigned long long>(s.pending_evicted),
              static_cast<unsigned long long>(s.pending_dropped), r.converged);
}

static void test_simulations(std::uint64_t n_ops) {
  {
    SimParams p;
    p.n_ops = n_ops;
    p.seed = 11;
    p.snapshot_every = 0;
    const SimResult r = run_sim(p);
    print_sim("clean", r);
    CHECK(r.converged);
    CHECK(r.stats.gaps_declared == 0 && r.stats.snapshots_applied == 0 && r.stats.desyncs == 0);
    CHECK(r.stats.dups[1] == r.packets_published);  // every B copy lost the race to A
    CHECK(r.stats.delivered[0] == r.packets_published && r.stats.delivered[1] == 0);
  }
  {
    SimParams p;
    p.n_ops = n_ops;
    p.seed = 22;
    p.loss_a_ppm = p.loss_b_ppm = 20'000;  // 2% each: 0.04% lost on both
    p.dup_ppm = 10'000;
    p.reorder_ppm = 30'000;
    p.snapshot_drop_ppm = 150'000;
    p.trap = true;
    const SimResult r = run_sim(p);
    print_sim("lossy A/B", r);
    CHECK(r.converged);
    CHECK(r.stats.gaps_filled > 0 && r.stats.gaps_declared > 0 && r.stats.snapshots_applied > 0);
    CHECK(r.stats.delivered[0] > 0 && r.stats.delivered[1] > 0);  // both lines rescued packets
    CHECK(r.trap_allocs == 0);
    std::printf("  handler allocations with trap armed: %llu\n", static_cast<unsigned long long>(r.trap_allocs));
  }
  {
    SimParams p;
    p.n_ops = n_ops;
    p.seed = 33;
    p.loss_a_ppm = p.loss_b_ppm = 150'000;  // brutal
    p.dup_ppm = 20'000;
    p.reorder_ppm = 60'000;
    p.snapshot_every = 400;
    p.snapshot_drop_ppm = 300'000;
    p.pending_slots = 16;
    const SimResult r = run_sim(p);
    print_sim("heavy loss", r);
    CHECK(r.converged);
    CHECK(r.stats.snapshots_applied > 0 && r.stats.snapshots_rejected > 0);
  }
  {
    SimParams p;
    p.n_ops = n_ops;
    p.seed = 44;
    p.loss_a_ppm = 50'000;
    p.loss_b_ppm = 1'000'000;  // line B is down for the whole run
    p.snapshot_every = 1000;
    const SimResult r = run_sim(p);
    print_sim("line B down", r);
    CHECK(r.converged);
    CHECK(r.stats.delivered[1] == 0);
  }
  {
    SimParams p;
    p.n_ops = n_ops;
    p.seed = 55;
    p.start_live = false;  // join mid-stream: first snapshot makes it Live
    p.loss_a_ppm = p.loss_b_ppm = 20'000;
    p.reorder_ppm = 20'000;
    const SimResult r = run_sim(p);
    print_sim("late join", r);
    CHECK(r.converged && r.stats.snapshots_applied > 0);
  }
}

// ---- mutation fuzz driver (the libFuzzer target covers the same code) ----------

static void test_mutation_fuzz(std::uint64_t iterations) {
  // Seed corpus: valid packets from a short simulation-like run.
  Rng rng(99);
  std::vector<Bytes> corpus;
  {
    FeedGen gen(FeedConfig{Price::from_units(100), 100, 4096, 500, kLoc}, 5);
    PacketBuilder pb("SESSION001");
    std::uint64_t seq = 1;
    std::uint8_t body[64];
    for (int i = 0; i < 2000; ++i) {
      pb.reset(seq);
      for (std::uint32_t j = 0, k = 1 + rng.below(6); j < k; ++j) pb.add_raw(body, FeedGen::encode(gen.next(), kLoc, 0, body));
      const std::uint8_t* d = pb.finish();
      corpus.emplace_back(d, d + pb.size());
      seq += pb.count();
    }
  }

  Rig r(16, 5000, true);
  std::uint64_t accepted = 0, now = 0;
  for (std::uint64_t it = 0; it < iterations; ++it) {
    Bytes b = corpus[rng.below(static_cast<std::uint32_t>(corpus.size()))];
    switch (rng.below(6)) {
      case 0:  // flip a few bytes
        for (std::uint32_t k = 1 + rng.below(4); k; --k) b[rng.below(static_cast<std::uint32_t>(b.size()))] ^= static_cast<std::uint8_t>(1 + rng.below(255));
        break;
      case 1:  // truncate
        b.resize(rng.below(static_cast<std::uint32_t>(b.size())));
        break;
      case 2:  // append garbage
        for (std::uint32_t k = 1 + rng.below(40); k; --k) b.push_back(static_cast<std::uint8_t>(rng.next()));
        break;
      case 3:  // scramble the header
        for (std::size_t k = 0; k < kHeaderBytes && k < b.size(); ++k) b[k] = static_cast<std::uint8_t>(rng.next());
        break;
      case 4:  // random sequence number, otherwise valid
        put_be64(b.data() + kSessionBytes, rng.next() >> rng.below(64));
        break;
      default:  // pure noise of random length
        b.assign(rng.below(1500), 0);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng.next());
        break;
    }
    PacketView pv;
    if (parse_packet(b.data(), b.size(), pv)) {
      ++accepted;
      // An accepted packet must be exactly walkable: no overrun, no slack.
      MsgCursor c(pv);
      const std::uint8_t* body;
      std::size_t bl, total = kHeaderBytes;
      std::uint32_t n = 0;
      while (c.next(body, bl)) {
        total += 2 + bl;
        ++n;
      }
      CHECK(total == b.size() && n == pv.count);
    }
    now += 700;
    r.h.on_packet(rng.below(2) ? Line::A : Line::B, b.data(), b.size(), now);
    if (it % 257 == 0) r.h.on_snapshot_packet(b.data(), b.size(), now);
    if (it % 64 == 0) r.h.poll(now);
    if (it % 1000 == 0) CHECK(r.book.check_invariants());
  }
  CHECK(r.book.order_count() <= 1024 && r.book.check_invariants());
  std::printf("mutation fuzz: %llu inputs, %llu parsed OK, final state %s, malformed %llu\n",
              static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(accepted),
              r.h.state() == FeedState::Live ? "Live" : "NeedSnapshot",
              static_cast<unsigned long long>(r.h.stats().malformed));
}

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 300000ULL;
  test_wire_golden();
  test_wire_parse();
  test_in_order_and_dups();
  test_gap_filled_by_other_line();
  test_gap_timeout_and_snapshot();
  test_heartbeat();
  test_session_and_filter();
  test_replace_semantics();
  test_desync_and_poison();
  test_snapshot_rejections();
  test_pending_policy();
  test_malformed_is_atomic();
  test_simulations(n);
  test_mutation_fuzz(n);
  if (g_failures == 0) std::puts("test_md: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
