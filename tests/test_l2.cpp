// Tests for the real-data path: decimal and JSON parsing, conversion of
// aggregated levels to ITCH events, capture loading, replay against an
// independent model, and the paper venue. No network access.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.hpp"
#include "l2_gen.hpp"
#include "hft/feed/replay.hpp"

using namespace hft;
using namespace hft::feed;
using l2gen::make_capture;

namespace {

// ---- decimals -------------------------------------------------------------------------------------

void test_decimal() {
  std::int64_t v = 0;
  bool tr = false;
  CHECK(parse_decimal("82049.00", 4, v, tr) && v == 820490000 && !tr);
  CHECK(parse_decimal("0.17576076", 5, v, tr) && v == 17576 && tr);       // 0.00000076 floored away
  CHECK(parse_decimal("0.17576000", 5, v, tr) && v == 17576 && !tr);      // dropped digits were zero
  CHECK(parse_decimal("5", 5, v, tr) && v == 500000);
  CHECK(parse_decimal("5.", 5, v, tr) && v == 500000);
  CHECK(parse_decimal(".5", 1, v, tr) && v == 5);
  CHECK(parse_decimal("0.00000000", 5, v, tr) && v == 0 && !tr);
  CHECK(parse_decimal("0.00000001", 5, v, tr) && v == 0 && tr);
  CHECK(parse_decimal("123456789012.5", 4, v, tr) && v == 1234567890125000);
  CHECK(!parse_decimal("1234567890123", 4, v, tr));                        // 13 integer digits
  CHECK(!parse_decimal("", 4, v, tr));
  CHECK(!parse_decimal(".", 4, v, tr));
  CHECK(!parse_decimal("-1.0", 4, v, tr));
  CHECK(!parse_decimal("+1.0", 4, v, tr));
  CHECK(!parse_decimal("1e5", 4, v, tr));
  CHECK(!parse_decimal("1.2.3", 4, v, tr));
  CHECK(!parse_decimal("12a", 4, v, tr));
  CHECK(!parse_decimal(" 1", 4, v, tr));
  // every value up to 1000.000 with 3 decimals round-trips
  for (int i = 0; i <= 1000000; i += 37) {
    char b[32];
    std::snprintf(b, sizeof b, "%d.%03d", i / 1000, i % 1000);
    CHECK(parse_decimal(b, 3, v, tr) && v == i && !tr);
  }
}

// ---- messages ------------------------------------------------------------------------------------------

void test_parse_messages() {
  const char* snap =
      R"({"type":"snapshot","product_id":"BTC-USD","asks":[["82049.00","0.9"],["82059.00","2.00000000"]],"bids":[["82048.99","1.5"]]})";
  L2Message m = parse_l2(snap);
  CHECK(m.kind == L2Message::Kind::Snapshot && m.changes.size() == 3);
  CHECK(m.changes[0].side == core::Side::Sell && m.changes[0].price_raw == 820490000 && m.changes[0].lots == 90000);
  CHECK(m.changes[2].side == core::Side::Buy && m.changes[2].price_raw == 820489900 && m.changes[2].lots == 150000);

  const char* upd =
      R"({"type":"l2update","product_id":"BTC-USD","changes":[["buy","82048.99","0.00000000"],["sell","82049.00","0.80000000"]],"time":"2026-10-16T02:46:47.861146Z"})";
  m = parse_l2(upd);
  CHECK(m.kind == L2Message::Kind::Update && m.changes.size() == 2);
  CHECK(m.changes[0].side == core::Side::Buy && m.changes[0].lots == 0);
  CHECK(m.changes[1].side == core::Side::Sell && m.changes[1].lots == 80000);

  // key order, whitespace, extra fields and extra row elements are all fine
  m = parse_l2(R"( { "time" : "t" , "extra": {"a":[1,2,{"b":null}]}, "changes" : [ [ "sell" , "1.0" , "2.0" , "x" ] ] , "type" : "l2update" } )");
  CHECK(m.kind == L2Message::Kind::Update && m.changes.size() == 1 && m.changes[0].lots == 200000);

  // messages we do not care about
  CHECK(parse_l2(R"({"type":"heartbeat","sequence":5,"product_id":"BTC-USD","time":"t"})").kind == L2Message::Kind::Other);
  CHECK(parse_l2(R"({"type":"subscriptions","channels":[{"name":"level2_50","product_ids":["BTC-USD"]}]})").kind ==
        L2Message::Kind::Other);
  CHECK(parse_l2(R"({"type":"error","message":"boom"})").kind == L2Message::Kind::Other);
  CHECK(parse_l2(R"({"type":"snapshot","product_id":"BTC-USD","bids":[],"asks":[]})").kind == L2Message::Kind::Snapshot);

  // damaged messages
  const char* bad[] = {
      "", "{", "}", "[]", "null", "{}", R"({"type":)", R"({"type":"l2update"})",         // update without changes
      R"({"type":"l2update","changes":[["hold","1.0","2.0"]]})",                        // unknown side
      R"({"type":"l2update","changes":[["buy","1.0"]]})",                               // short row
      R"({"type":"l2update","changes":[["buy","x","2.0"]]})",                           // bad number
      R"({"type":"l2update","changes":[["buy","1.0","2.0"])",                            // truncated
      R"({"type":"l2update","changes":[[buy,1.0,2.0]]})",                                // not strings
      R"({"type":"snapshot","product_id":"BTC-USD"})",                                  // no sides
      R"({"type":"snapshot","bids":[["1.0"]],"asks":[]})",                              // short level
      R"({"changes":[["buy","1.0","2.0"]]})",                                           // no type
      R"({"type":"l2update","changes":[["buy","1.0","2.0"]],})",                         // trailing comma
  };
  for (const char* b : bad) {
    const L2Message x = parse_l2(b);
    CHECK(x.kind == L2Message::Kind::Bad && x.changes.empty());
  }
  // nesting deeper than any real message is refused, even when it is well formed
  CHECK(parse_l2(R"({"type":"l2update","changes":[],"x":[[[[[[[[[[[[1]]]]]]]]]]]]})").kind == L2Message::Kind::Bad);
  CHECK(parse_l2(R"({"type":"l2update","changes":[],"x":[[[1]]]})").kind == L2Message::Kind::Update);
  // hostile nesting must not recurse without limit
  std::string deep = R"({"type":"x","a":)";
  for (int i = 0; i < 5000; ++i) deep += '[';
  CHECK(parse_l2(deep).kind == L2Message::Kind::Bad);
  // random byte damage never crashes and never yields changes from a Bad parse
  std::uint64_t r = 12345;
  const std::string good = upd;
  for (int i = 0; i < 20000; ++i) {
    std::string d = good;
    for (int k = 0; k < 3; ++k) {
      r = r * 6364136223846793005ULL + 1442695040888963407ULL;
      d[(r >> 33) % d.size()] = static_cast<char>(r >> 20);
    }
    const L2Message x = parse_l2(d);
    CHECK(x.kind != L2Message::Kind::Bad || x.changes.empty());
  }
}

// ---- conversion semantics ----------------------------------------------------------------------------

struct Ev {
  sim::PublicType t;
  core::Side side;
  core::OrderId id;
  std::int64_t px;
  core::Qty qty;
};

L2Message M(const char* json) { return parse_l2(json); }

void test_converter() {
  L2Config cfg;
  cfg.num_levels = 1000;
  cfg.margin_pct = 10;
  L2Converter c(cfg);
  std::vector<Ev> ev;
  auto emit = [&](const sim::PublicEvent& e) { ev.push_back({e.type, e.side, e.id, e.price.raw(), e.qty}); };

  CHECK(!c.on_snapshot(M(R"({"type":"snapshot","bids":[["100.00","1"]],"asks":[]})"), 0, emit));  // no mid
  CHECK(!c.started());

  CHECK(c.on_snapshot(M(R"({"type":"snapshot","bids":[["100.00","1.0"],["99.99","2.5"]],"asks":[["100.02","3"]]})"), 5, emit));
  CHECK(ev.size() == 3 && c.stats().adds == 3);
  // grid centred on mid = 100.01 (index 10001) -> base index 9501
  CHECK(c.base_raw() == 9501 * 100);
  for (const Ev& e : ev) CHECK(e.t == sim::PublicType::Add);
  const core::OrderId id_100 = ev[0].id;

  ev.clear();
  // bigger -> delete + new add; smaller -> reduce; zero -> delete; same -> nothing
  c.on_update(M(R"({"type":"l2update","changes":[["buy","100.00","3.0"]]})"), 10, emit);
  CHECK(ev.size() == 2 && ev[0].t == sim::PublicType::Delete && ev[0].id == id_100 && ev[1].t == sim::PublicType::Add &&
        ev[1].id != id_100 && ev[1].qty == 300000);
  ev.clear();
  c.on_update(M(R"({"type":"l2update","changes":[["buy","100.00","2.0"]]})"), 11, emit);
  CHECK(ev.size() == 1 && ev[0].t == sim::PublicType::Reduce && ev[0].qty == 100000);
  ev.clear();
  c.on_update(M(R"({"type":"l2update","changes":[["buy","100.00","2.0"]]})"), 12, emit);
  CHECK(ev.empty() && c.stats().unchanged == 1);
  c.on_update(M(R"({"type":"l2update","changes":[["buy","100.00","0.0"]]})"), 13, emit);
  CHECK(ev.size() == 1 && ev[0].t == sim::PublicType::Delete && ev[0].qty == 200000);
  CHECK(c.levels(core::Side::Buy).size() == 1);
  ev.clear();
  // zero for a level we never had; off tick; outside the window; sub-lot
  c.on_update(M(R"({"type":"l2update","changes":[["buy","99.50","0.0"],["buy","99.994","1.0"],["sell","900.00","1.0"],["sell","100.05","0.000001"]]})"), 14, emit);
  CHECK(ev.empty());
  // 99.50 and 100.05 name levels we never had (zero lots); 99.994 is off the tick grid;
  // 900.00 is outside the window; 100.05's size is finer than a lot
  CHECK(c.stats().unknown_delete == 2 && c.stats().off_tick == 1 && c.stats().out_of_window == 1 && c.stats().sub_lot == 1);
  // the window is [base, base + num_levels): 105.00 is its last price, 105.01 is outside
  {
    L2Converter w(cfg);
    std::vector<Ev> we;
    auto wemit = [&](const sim::PublicEvent& e) { we.push_back({e.type, e.side, e.id, e.price.raw(), e.qty}); };
    CHECK(w.on_snapshot(M(R"({"type":"snapshot","bids":[["100.00","1"]],"asks":[["100.02","1"]]})"), 0, wemit));
    we.clear();
    w.on_update(M(R"({"type":"l2update","changes":[["sell","105.00","1"],["sell","105.01","1"],["buy","95.01","1"],["buy","95.00","1"]]})"), 1, wemit);
    CHECK(we.size() == 2 && w.stats().out_of_window == 2);
    // crossed means bid >= ask: locked and crossed both count
    const std::uint64_t before = w.stats().crossed;
    w.on_update(M(R"({"type":"l2update","changes":[["buy","100.02","1"]]})"), 2, wemit);
    CHECK(w.stats().crossed == before + 1);
    w.on_update(M(R"({"type":"l2update","changes":[["buy","100.03","1"]]})"), 3, wemit);
    CHECK(w.stats().crossed == before + 2);
    w.on_update(M(R"({"type":"l2update","changes":[["buy","100.03","0"],["buy","100.02","0"]]})"), 4, wemit);
    CHECK(w.stats().crossed == before + 2);
  }
  // wire-size clamp
  c.on_update(M(R"({"type":"l2update","changes":[["sell","100.03","99999999.0"]]})"), 15, emit);
  CHECK(ev.size() == 1 && ev[0].qty == 0xFFFFFFFFLL && c.stats().clamped == 1);
  ev.clear();
  // the best price drifting to the edge of the window ends the segment
  CHECK(!c.window_exit());
  // (window is 95.01 .. 105.00; with a 10% margin the touch must stay below 103.99)
  c.on_update(M(R"({"type":"l2update","changes":[["sell","100.02","0.0"],["sell","100.03","0.0"],["sell","104.50","1.0"]]})"), 16, emit);
  CHECK(c.window_exit() && c.stats().window_exits == 1);

  // the same on the bid side: the bids walk down to 98.xx (exit below 96.01 + margin)
  {
    L2Converter d(cfg);
    std::vector<Ev> de;
    auto demit = [&](const sim::PublicEvent& e) { de.push_back({e.type, e.side, e.id, e.price.raw(), e.qty}); };
    CHECK(d.on_snapshot(M(R"({"type":"snapshot","bids":[["100.00","1"]],"asks":[["100.02","1"]]})"), 0, demit));
    d.on_update(M(R"({"type":"l2update","changes":[["buy","96.50","1"],["buy","100.00","0"]]})"), 1, demit);
    CHECK(!d.window_exit());  // 96.50 is inside the margin (lowest allowed touch is 96.01)
    d.on_update(M(R"({"type":"l2update","changes":[["buy","96.00","1"],["buy","96.50","0"]]})"), 2, demit);
    CHECK(d.window_exit() && d.stats().window_exits == 1);
  }

  // a second snapshot in a fresh converter forgets nothing it should not
  L2Converter c2(cfg);
  ev.clear();
  c2.on_update(M(R"({"type":"l2update","changes":[["buy","1.0","1.0"]]})"), 1, emit);  // ignored before a snapshot
  CHECK(c2.stats().updates == 0 && ev.empty());
}

// ---- random captures through the whole chain ------------------------------------------------------------


void run_capture_text(const std::string& text, int expect_segments_min, bool expect_window_end) {
  L2Config cfg;
  cfg.num_levels = 4096;  // $40.96 window so drift can leave it
  CaptureLoader loader(cfg, 25);
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    loader.feed_line(text.substr(pos, nl - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  loader.feed_line("#999999999\tstop");
  CHECK(static_cast<int>(loader.segments().size()) >= expect_segments_min);
  CHECK(loader.stats().bad > 0);  // the damaged lines were seen and survived
  bool saw_window_end = false;
  ReplayOptions ro;
  ro.timed = false;
  for (const Segment& seg : loader.segments()) {
    if (seg.ended_by_window) saw_window_end = true;
    if (seg.pkts.empty()) continue;
    const ReplayResult a = replay_segment(seg, ro, nullptr);
    const ReplayResult b = replay_segment(seg, ro, nullptr);
    CHECK(a.ok);
    CHECK(a.checks > 3);
    CHECK(a.check_failures == 0);
    CHECK(a.final_book_matches);
    CHECK(a.desyncs == 0 && a.book_errors == 0 && a.feed_losses == 0);
    CHECK(a.wire_hash == b.wire_hash && a.frames == b.frames && a.wire_bytes == b.wire_bytes);
    CHECK(a.messages_applied > 0 && a.decisions > 0);
    // every request the strategy sent was answered
    CHECK(a.venue.frames == a.frames);
    CHECK(a.gw.reports == a.reports_delivered);  // the gateway actually received what the venue sent
    CHECK(a.reports_delivered == a.venue.accepted + a.venue.replaced + a.venue.canceled + a.venue.rejected_cross +
                                      a.venue.rejected_other + a.venue.unknown_cancel + a.venue.unknown_replace);
    CHECK(a.gw.seq_gaps == 0 && a.gw.bad_messages == 0 && a.gw.broken_streams == 0);
  }
  CHECK(saw_window_end == expect_window_end);
}

void test_random_captures() {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) run_capture_text(make_capture(seed, 3, 600), 3, false);
}

// The price walks to the edge of the grid window: the segment must end there, the
// replay of what came before must still check out, and the rest is not trusted.
void test_window_exit_through_pipeline() {
  std::string t = "#1\tstart\tu\tp\n";
  t += "10\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"],[\"99.99\",\"2\"]],\"asks\":[[\"100.02\",\"1\"],[\"100.03\",\"2\"]]}\n";
  t += "20\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"2\"]]}\n";
  // the asks walk up to 118.02 (window is 79.53 .. 120.48, margin 4.10 -> exit at 116.39)
  t += "30\t{\"type\":\"l2update\",\"changes\":[[\"sell\",\"100.02\",\"0\"],[\"sell\",\"100.03\",\"0\"],[\"sell\",\"118.02\",\"1\"]]}\n";
  t += "40\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"5\"]]}\n";  // after the exit: not part of any segment
  t += "#50\tstop\n";
  L2Config cfg;
  cfg.num_levels = 4096;
  CaptureLoader l(cfg, 1);
  std::size_t pos = 0;
  while (pos < t.size()) {
    const std::size_t nl = t.find('\n', pos);
    l.feed_line(t.substr(pos, nl - pos));
    pos = nl + 1;
  }
  CHECK(l.segments().size() == 1 && l.segments()[0].ended_by_window);
  CHECK(l.stats().dropped_before_snapshot == 1);
  ReplayOptions ro;
  ro.timed = false;
  const ReplayResult a = replay_segment(l.segments()[0], ro, nullptr);
  CHECK(a.ok && a.check_failures == 0 && a.final_book_matches && a.checks >= 3);
}

void test_loader_edges() {
  L2Config cfg;
  // updates before any snapshot are dropped and counted
  {
    CaptureLoader l(cfg);
    l.feed_line("#1\tstart\tu\tp");
    l.feed_line("5\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"1.0\",\"1.0\"]]}");
    CHECK(l.stats().dropped_before_snapshot == 1 && l.segments().empty());
  }
  // a snapshot with one side only cannot be centred
  {
    CaptureLoader l(cfg);
    l.feed_line("5\t{\"type\":\"snapshot\",\"bids\":[[\"1.0\",\"1.0\"]],\"asks\":[]}");
    CHECK(l.stats().snapshots_unusable == 1 && l.segments().empty());
  }
  // a second snapshot without a marker restarts the segment; a marker also ends one
  {
    CaptureLoader l(cfg);
    const char* s = "{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"]],\"asks\":[[\"100.02\",\"1\"]]}";
    l.feed_line(std::string("5\t") + s);
    l.feed_line("6\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"2\"]]}");
    l.feed_line(std::string("9\t") + s);
    l.feed_line("#10\tidle_timeout");
    l.feed_line(std::string("11\t") + s);
    l.feed_line("12\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"2\"]]}");
    l.feed_line("#13\tstop");
    CHECK(l.segments().size() == 3);
    CHECK(l.segments()[0].pkts.size() == 2 && l.segments()[1].pkts.size() == 1 && l.segments()[2].pkts.size() == 2);
  }
  // max_t: later messages are ignored
  {
    CaptureLoader l(cfg, 500, 7);
    l.feed_line("5\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"]],\"asks\":[[\"100.02\",\"1\"]]}");
    l.feed_line("6\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"2\"]]}");
    l.feed_line("8\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"3\"]]}");
    l.feed_line("#9\tstop");
    CHECK(l.segments().size() == 1 && l.segments()[0].final_levels[0].begin()->second == 200000);
  }
  // a message stamped exactly at the limit is still included
  {
    CaptureLoader l(cfg, 500, 8);
    l.feed_line("5\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"]],\"asks\":[[\"100.02\",\"1\"]]}");
    l.feed_line("8\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"3\"]]}");
    l.feed_line("9\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\"100.00\",\"4\"]]}");
    l.feed_line("#10\tstop");
    CHECK(l.segments().size() == 1 && l.segments()[0].final_levels[0].begin()->second == 300000);
  }
  // junk
  {
    CaptureLoader l(cfg);
    l.feed_line("");
    l.feed_line("no tab here");
    l.feed_line("abc\t{}");
    l.feed_line("\t{}");
    l.feed_line("99999999999999999999999\t{}");
    CHECK(l.stats().bad_lines == 4 && l.segments().empty());
  }
}

// ---- the exchange-snapshot comparison -----------------------------------------------------------------------

void test_verify_against_snapshot() {
  L2Config cfg;
  CaptureLoader l(cfg);
  l.feed_line("5\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"],[\"99.99\",\"2\"],[\"99.98\",\"3\"]],\"asks\":[[\"100.02\",\"1\"],[\"100.03\",\"2\"]]}");
  l.feed_line("#6\tstop");
  const Segment& seg = l.segments()[0];
  const L2Message same = parse_l2("{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"],[\"99.99\",\"2\"],[\"99.98\",\"3\"]],\"asks\":[[\"100.02\",\"1\"],[\"100.03\",\"2\"]]}");
  VerifyResult v = verify_against_snapshot(seg, same, 10);
  CHECK(v.usable && v.compared == 5 && v.equal == 5);
  const L2Message diff = parse_l2("{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"9\"],[\"99.97\",\"2\"]],\"asks\":[[\"100.02\",\"1\"]]}");
  v = verify_against_snapshot(seg, diff, 10);
  CHECK(v.compared == 3 && v.equal == 1 && v.size_differs == 1 && v.missing_in_model == 1);
  v = verify_against_snapshot(seg, same, 2);  // only the two nearest per side
  CHECK(v.compared == 4);
  CHECK(!verify_against_snapshot(seg, parse_l2("{\"type\":\"heartbeat\"}"), 10).usable);
  CHECK(v.outside_window == 0);
}

// Levels beyond the segment's price window are dropped by the converter, so the comparison counts them apart.
void test_verify_outside_window() {
  L2Config cfg;
  cfg.num_levels = 64;  // a narrow window, so that its edges can be reached with ordinary prices
  CaptureLoader l(cfg);
  l.feed_line("5\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"]],\"asks\":[[\"100.02\",\"1\"]]}");
  l.feed_line("#6\tstop");
  CHECK(l.segments().size() == 1);
  const Segment& seg = l.segments()[0];
  const std::int64_t lo = seg.base_raw / cfg.tick_raw;  // the first index inside, in ticks of 0.01
  const std::int64_t hi = lo + 64;                       // the first index outside
  auto px = [](std::int64_t idx) {
    char b[48];
    std::snprintf(b, sizeof b, "%lld.%02lld", static_cast<long long>(idx / 100), static_cast<long long>(idx % 100));
    return std::string(b);
  };
  CHECK(lo > 0 && lo < 10000 && hi > 10002);
  // bids at the first index inside and just below it; asks at the last index inside and the first outside
  const std::string snap = "{\"type\":\"snapshot\",\"bids\":[[\"" + px(lo) + "\",\"1\"],[\"" + px(lo - 1) + "\",\"1\"]],\"asks\":[[\"" +
                           px(hi - 1) + "\",\"1\"],[\"" + px(hi) + "\",\"1\"]]}";
  VerifyResult v = verify_against_snapshot(seg, parse_l2(snap), 10);
  CHECK(v.usable && v.compared == 2 && v.missing_in_model == 2 && v.equal == 0 && v.size_differs == 0 && v.outside_window == 2);
  // the nearest-N rule counts a level outside the window as one of the N
  v = verify_against_snapshot(seg, parse_l2(snap), 1);
  CHECK(v.compared == 2 && v.outside_window == 0);  // the nearest bid and the nearest ask are the inside ones
  const std::string far = "{\"type\":\"snapshot\",\"bids\":[[\"" + px(lo - 5) + "\",\"1\"],[\"100.00\",\"1\"]],\"asks\":[[\"100.02\",\"1\"],[\"" + px(hi + 7) + "\",\"1\"]]}";
  v = verify_against_snapshot(seg, parse_l2(far), 10);
  CHECK(v.compared == 2 && v.equal == 2 && v.outside_window == 2 && v.missing_in_model == 0);
  v = verify_against_snapshot(seg, parse_l2(far), 2);  // two nearest per side: all four are taken, two are outside
  CHECK(v.compared == 2 && v.outside_window == 2);
  // when the nearest level is itself outside the window it uses up one of the N
  const std::string odd = "{\"type\":\"snapshot\",\"bids\":[[\"" + px(hi + 3) + "\",\"1\"],[\"" + px(lo) + "\",\"1\"]],\"asks\":[[\"100.02\",\"1\"]]}";
  v = verify_against_snapshot(seg, parse_l2(odd), 1);
  CHECK(v.outside_window == 1 && v.compared == 1);  // the bid beyond the window, and the ask
}

// ---- the checker must be able to fail ---------------------------------------------------------------------------

// The comparison of the pipeline's book with the model is only worth something
// if it notices when they differ. Tamper with the model one field at a time and
// require the replay to report it.
void test_checker_detects_differences() {
  const std::string text = make_capture(77, 1, 400);
  L2Config cfg;
  CaptureLoader loader(cfg, 20);
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    loader.feed_line(text.substr(pos, nl - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  loader.feed_line("#99999999\tstop");
  CHECK(loader.segments().size() == 1);
  const Segment good = loader.segments()[0];
  ReplayOptions ro;
  ro.timed = false;
  const ReplayResult base = replay_segment(good, ro, nullptr);
  CHECK(base.check_failures == 0 && base.final_book_matches && base.checks > 5);

  auto mid_cp = [](Segment& s) -> Checkpoint& { return s.checkpoints[s.checkpoints.size() / 2]; };
  {
    Segment t = good;
    mid_cp(t).bid_px += 100;
    const ReplayResult r = replay_segment(t, ro, nullptr);
    CHECK(r.check_failures == 1 && r.final_book_matches);
  }
  {
    Segment t = good;
    mid_cp(t).ask_px += 100;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;
    mid_cp(t).bid_qty += 1;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;
    mid_cp(t).ask_qty += 1;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;
    mid_cp(t).bid_depth += 1;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;
    mid_cp(t).ask_depth += 1;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;
    mid_cp(t).live_levels += 1;
    CHECK(replay_segment(t, ro, nullptr).check_failures == 1);
  }
  {
    Segment t = good;  // a level the book does not have
    t.final_levels[0][t.final_levels[0].begin()->first - 1] = 5;
    const ReplayResult r = replay_segment(t, ro, nullptr);
    CHECK(!r.final_book_matches && r.check_failures == 0);
  }
  {
    Segment t = good;  // a level with a different size
    t.final_levels[1].begin()->second += 1;
    CHECK(!replay_segment(t, ro, nullptr).final_book_matches);
  }
  {
    Segment t = good;  // a level the model lacks
    t.final_levels[0].erase(t.final_levels[0].begin());
    CHECK(!replay_segment(t, ro, nullptr).final_book_matches);
  }
  // and a damaged packet stream (one packet dropped) is caught by the checkpoints
  {
    Segment t = good;
    const Packet gone = t.pkts[t.pkts.size() / 2];
    t.pkts.erase(t.pkts.begin() + static_cast<std::ptrdiff_t>(t.pkts.size() / 2));
    (void)gone;
    const ReplayResult r = replay_segment(t, ro, nullptr);
    CHECK(r.check_failures > 0 || !r.final_book_matches || r.desyncs > 0 || r.feed_losses > 0);
  }
}

// ---- the paper venue ----------------------------------------------------------------------------------------------

void test_ack_venue() {
  core::Region region = core::Region::create(8u << 20, core::RegionOptions{false, false, true, -1});
  core::Arena arena(region);
  book::OrderBook ob;
  book::BookConfig bc;
  bc.base = core::Price::from_raw(0);
  bc.tick_raw = 100;
  bc.num_levels = 4096;
  bc.max_orders = 1024;
  CHECK(ob.init(arena, bc));
  CHECK(ob.add(1, core::Side::Buy, core::Price::from_raw(100 * 100), 10) == book::BookResult::Ok);
  CHECK(ob.add(2, core::Side::Sell, core::Price::from_raw(102 * 100), 10) == book::BookResult::Ok);

  AckVenue v(1000, &ob);
  std::uint8_t f[gw::kMaxFrame], rep[gw::kMaxFrame];
  auto send_new = [&](std::uint64_t id, core::Side s, std::int64_t px, sim::OrderType t) {
    const std::size_t n = gw::encode_new(f, id, id, s, t, 5, core::Price::from_raw(px));
    v.on_frame(f, n);
  };
  auto pop = [&](sim::Report& r) {
    std::uint64_t seq = 0;
    const std::size_t n = v.pop(rep);
    return gw::decode_report(rep + 2, n - 2, r, seq);
  };

  v.set_now(100);
  send_new(10, core::Side::Buy, 101 * 100, sim::OrderType::PostOnly);   // inside the spread: accepted
  send_new(11, core::Side::Buy, 102 * 100, sim::OrderType::PostOnly);   // at the ask: would take, rejected
  send_new(12, core::Side::Sell, 100 * 100, sim::OrderType::PostOnly);  // at the bid: rejected
  send_new(13, core::Side::Buy, 102 * 100, sim::OrderType::Limit);      // not post-only: accepted (venue never fills)
  send_new(10, core::Side::Buy, 101 * 100, sim::OrderType::PostOnly);   // duplicate id
  CHECK(!v.has_due(1099) && v.has_due(1100));
  sim::Report r{};
  CHECK(pop(r) && r.type == sim::ReportType::Accepted && r.id == 10 && r.qty == 5 && r.leaves == 5);
  CHECK(pop(r) && r.type == sim::ReportType::Rejected && r.reason == sim::Reason::WouldCross && r.id == 11);
  CHECK(pop(r) && r.type == sim::ReportType::Rejected && r.reason == sim::Reason::WouldCross && r.id == 12);
  CHECK(pop(r) && r.type == sim::ReportType::Accepted && r.id == 13);
  CHECK(pop(r) && r.type == sim::ReportType::Rejected && r.reason == sim::Reason::Duplicate);
  CHECK(v.empty());

  // replace and cancel of known and unknown orders
  std::size_t n = gw::encode_replace(f, 50, 10, 7, core::Price::from_raw(100 * 100 + 50));
  v.on_frame(f, n);
  n = gw::encode_replace(f, 51, 99, 7, core::Price::from_raw(5));
  v.on_frame(f, n);
  n = gw::encode_cancel(f, 52, 10);
  v.on_frame(f, n);
  n = gw::encode_cancel(f, 53, 10);
  v.on_frame(f, n);
  CHECK(pop(r) && r.type == sim::ReportType::Replaced && r.id == 10 && r.qty == 7 && r.leaves == 7);
  CHECK(pop(r) && r.type == sim::ReportType::ReplaceRejected && r.reason == sim::Reason::UnknownOrder);
  CHECK(pop(r) && r.type == sim::ReportType::Canceled && r.id == 10 && r.qty == 7 && r.leaves == 0);
  CHECK(pop(r) && r.type == sim::ReportType::CancelRejected && r.reason == sim::Reason::UnknownOrder);
  CHECK(v.stats().accepted == 2 && v.stats().rejected_cross == 2 && v.stats().rejected_other == 1 && v.live() == 1);

  // garbage is counted, not acted on
  const std::uint8_t junk[] = {0, 3, 'Z', 1, 2};
  v.on_frame(junk, sizeof junk);
  CHECK(v.stats().malformed == 1 && v.empty());
}

}  // namespace

int main() {
  test_decimal();
  test_parse_messages();
  test_converter();
  test_loader_edges();
  test_verify_against_snapshot();
  test_verify_outside_window();
  test_ack_venue();
  test_checker_detects_differences();
  test_random_captures();
  test_window_exit_through_pipeline();
  if (g_failures) {
    std::fprintf(stderr, "test_l2: %d FAILURES\n", g_failures);
    return 1;
  }
  std::printf("test_l2: all checks passed\n");
  return 0;
}
