// Tests for the live paper session. No network, no wall clock: time is whatever
// the test passes in, so every drill is exactly reproducible.
//
// The anchor is equivalence: fed the same lines with the same stamps, the live
// session must put the same bytes on the wire as the offline replay (up to the
// last packet), build the same book, and make the same decisions. Everything the
// live session adds (stale feed, reconnect, kill, window exit, integrity check)
// is then tested as a drill with a stated expected outcome.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "hft/feed/live.hpp"
#include "l2_gen.hpp"

using namespace hft;
using namespace hft::feed;

namespace {

// "<digits>\t..." or "#<digits>\t..." -> the stamp; 0 if there is none
std::uint64_t stamp_of(const std::string& line) {
  std::size_t i = (!line.empty() && line[0] == '#') ? 1 : 0;
  std::uint64_t t = 0;
  bool any = false;
  for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
    t = t * 10 + static_cast<std::uint64_t>(line[i] - '0');
    any = true;
  }
  return any ? t : 0;
}

template <class F>
void each_line(const std::string& text, F&& f) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    f(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

// feeds a capture to a session using the stamps in the lines as the clock
void drive(LiveSession& s, const std::string& text) {
  std::uint64_t last = 0;
  each_line(text, [&](const std::string& line) {
    const std::uint64_t t = stamp_of(line);
    if (t) last = t;
    s.on_line(line, last);
  });
}

LiveConfig small_cfg() {
  LiveConfig c;
  c.l2.num_levels = 4096;
  c.check_every = 25;
  c.arena_bytes = 64u << 20;
  c.max_orders = 1u << 14;
  return c;
}

// ---- equivalence with the offline replay --------------------------------------------------------------

void equivalence(std::uint64_t seed, std::uint64_t rtt_ns) {
  const std::string text = l2gen::make_capture(seed, 3, 500);

  L2Config l2;
  l2.num_levels = 4096;
  CaptureLoader loader(l2, 25);
  each_line(text, [&](const std::string& line) { loader.feed_line(line); });
  loader.feed_line("#999999999\tstop");

  ReplayOptions ro;
  ro.timed = false;
  ro.venue_rtt_ns = rtt_ns;
  ro.arena_bytes = 64u << 20;
  ro.book_slack = 3;

  LiveConfig lc = small_cfg();
  lc.venue_rtt_ns = rtt_ns;
  lc.stale_ns = ~0ULL >> 1;  // the recorded gaps are irrelevant here
  LiveSession live(lc);
  drive(live, text);
  live.on_line("#999999999\tstop", 999999999);

  std::size_t used = 0;
  std::uint64_t decisions = 0, applied = 0, sent = 0;
  for (const Segment& seg : loader.segments()) {
    if (seg.pkts.empty()) continue;
    CHECK(used < live.summaries().size());
    if (used >= live.summaries().size()) break;
    const ReplayResult r = replay_segment(seg, ro, nullptr);
    const SegmentSummary& sm = live.summaries()[used++];
    CHECK(r.ok && r.check_failures == 0 && r.final_book_matches);
    CHECK(sm.packets == r.packets);
    CHECK(sm.frames_before_flatten == r.frames_before_tail);
    CHECK(sm.hash_before_flatten == r.hash_before_tail);  // the same bytes on the wire
    CHECK(sm.flat && sm.book_matched_model && !sm.kill_latched);
    CHECK(sm.reason == EndReason::Marker);
    decisions += r.decisions;
    applied += r.messages_applied;
    sent += r.gw.sent_new;
  }
  CHECK(used == live.summaries().size() && used >= 3);
  const LiveStats& st = live.stats();
  // the replay also lets the strategy react to the last reports; the live session flattens instead
  CHECK(st.decisions <= decisions && st.decisions + 40 >= decisions && st.messages_applied == applied);
  CHECK(st.check_failures == 0 && st.final_mismatches == 0 && st.flatten_failures == 0);
  CHECK(st.desyncs == 0 && st.book_errors == 0 && st.feed_losses == 0);
  CHECK(st.dropped_no_segment == loader.stats().dropped_before_snapshot);
  CHECK(st.bad == loader.stats().bad && st.bad_lines == loader.stats().bad_lines);
  CHECK(st.sent_new >= sent);   // the live session also sends what flatten needs (cancels), but no extra new orders
  CHECK(st.sent_new == sent);
  CHECK(st.checks > 10);
  // everything the strategy and flatten sent was answered; nothing is left over
  CHECK(st.sent_new + st.sent_replace + st.sent_cancel == st.acc + st.rep + st.canc + st.rej_cross + 0);
  CHECK(!live.active() && live.venue_live() == 0 && live.open_orders() == 0);
}

void test_equivalence() {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) equivalence(seed, 50'000);
  equivalence(9, 150);  // acks arrive within a few packets: the strategy reacts to them constantly
  equivalence(10, 0);
}

std::string upd(std::uint64_t t, const char* side, const char* price, const char* size);

// A snapshot of thousands of levels and an update touching hundreds are several packets each: the
// comparison with the model must only happen between feed messages, never in the middle of one.
void test_multi_packet_messages() {
  LiveConfig c = small_cfg();
  c.check_every = 3;  // far fewer packets than the snapshot has: any mid-message comparison would fire
  c.l2.num_levels = 8192;
  LiveSession s(c);
  std::string snap = "1000\t{\"type\":\"snapshot\",\"bids\":[";
  char b[64];
  for (int i = 0; i < 3000; ++i) {
    std::snprintf(b, sizeof b, "%s[\"%d.%02d\",\"%d\"]", i ? "," : "", (10000 - i) / 100, (10000 - i) % 100, 1 + i % 7);
    snap += b;
  }
  snap += "],\"asks\":[";
  for (int i = 0; i < 3000; ++i) {
    std::snprintf(b, sizeof b, "%s[\"%d.%02d\",\"%d\"]", i ? "," : "", (10010 + i) / 100, (10010 + i) % 100, 1 + i % 5);
    snap += b;
  }
  snap += "]}";
  s.on_line(snap, 1000);
  CHECK(s.active());
  CHECK(s.stats().packets > 20);  // several packets for one message
  std::string up = "2000\t{\"type\":\"l2update\",\"changes\":[";
  for (int i = 0; i < 400; ++i) {
    std::snprintf(b, sizeof b, "%s[\"buy\",\"%d.%02d\",\"%d\"]", i ? "," : "", (9990 - i) / 100, (9990 - i) % 100, 2 + i % 9);
    up += b;
  }
  up += "]}";
  s.on_line(up, 2000);
  s.on_line(upd(3000, "sell", "100.20", "0"), 3000);
  CHECK(s.active() && s.stats().check_failures == 0 && s.stats().checks >= 2);
  s.shutdown(4000);
  CHECK(s.summaries().back().book_matched_model && s.stats().desyncs == 0 && s.stats().book_errors == 0);
}

void test_deterministic() {
  const std::string text = l2gen::make_capture(5, 3, 400);
  LiveSession a(small_cfg()), b(small_cfg());
  drive(a, text);
  drive(b, text);
  a.shutdown(1u << 30);
  b.shutdown(1u << 30);
  CHECK(a.summaries().size() == b.summaries().size() && a.summaries().size() >= 3);
  for (std::size_t i = 0; i < a.summaries().size() && i < b.summaries().size(); ++i) {
    CHECK(a.summaries()[i].hash_before_flatten == b.summaries()[i].hash_before_flatten);
    CHECK(a.summaries()[i].frames_total == b.summaries()[i].frames_total);
  }
  CHECK(a.stats().sent_new == b.stats().sent_new && a.stats().sent_cancel == b.stats().sent_cancel);
}

// ---- hand-written drills --------------------------------------------------------------------------

// a book with some depth around 100.00; invented numbers
std::string snapshot_line(std::uint64_t t, int shift_cents = 0) {
  auto px = [&](int cents) {
    char b[32];
    std::snprintf(b, sizeof b, "%d.%02d", (cents + shift_cents) / 100, (cents + shift_cents) % 100);
    return std::string(b);
  };
  std::string s = std::to_string(t) + "\t{\"type\":\"snapshot\",\"product_id\":\"X\",\"bids\":[";
  for (int i = 0; i < 8; ++i) s += std::string(i ? "," : "") + "[\"" + px(10000 - i) + "\",\"" + std::to_string(2 + i) + "\"]";
  s += "],\"asks\":[";
  for (int i = 0; i < 8; ++i) s += std::string(i ? "," : "") + "[\"" + px(10010 + i) + "\",\"" + std::to_string(2 + i) + "\"]";
  s += "]}";
  return s;
}
std::string upd(std::uint64_t t, const char* side, const char* price, const char* size) {
  return std::to_string(t) + "\t{\"type\":\"l2update\",\"product_id\":\"X\",\"changes\":[[\"" + side + "\",\"" + price +
         "\",\"" + size + "\"]]}";
}
std::string beat(std::uint64_t t) { return std::to_string(t) + "\t{\"type\":\"heartbeat\",\"sequence\":1}"; }

// Feeds a snapshot and a few updates, a microsecond apart, so that when this returns
// the strategy has orders out whose acks (50 us away) have not come back yet.
void warm_up(LiveSession& s, std::uint64_t t0) {
  s.on_line(snapshot_line(t0), t0);
  const char* sizes[] = {"3", "4", "5", "6"};
  for (std::uint64_t i = 0; i < 4; ++i) s.on_line(upd(t0 + 1000 * (i + 1), "buy", "100.00", sizes[i]), t0 + 1000 * (i + 1));
}

void test_flatten_with_orders_in_flight() {
  LiveSession s(small_cfg());
  warm_up(s, 1'000'000);
  CHECK(s.active());
  CHECK(s.open_orders() > 0);                 // the strategy is quoting
  const std::uint32_t stage_open_orders = s.open_orders();
  CHECK(s.gateway_stats()->reports == 0);     // no ack has come back yet: the orders are still PendingNew at the gateway
  s.on_line("#1006000\treconnect", 1'006'000);  // 2 us later the connection drops
  CHECK(!s.active());
  CHECK(s.summaries().size() == 1);
  const SegmentSummary& sm = s.summaries()[0];
  CHECK(sm.reason == EndReason::Marker);
  CHECK(sm.flat);                              // nothing left working anywhere
  CHECK(sm.open_before_flatten == stage_open_orders);  // and the flatten really had orders to take down
  CHECK(sm.flatten_rounds >= 2);               // PendingNew orders had to wait for their ack before they could be cancelled
  CHECK(!sm.kill_latched);                     // a lost feed is not a kill
  CHECK(s.stats().pulled_cancels > 0 && s.stats().kill_cancels == 0);
  CHECK(s.stats().canc == s.stats().acc);      // every accepted order was cancelled
  CHECK(s.stats().flatten_failures == 0 && !s.halted());
  CHECK(s.stats().max_flatten_sim_ns >= 90'000);  // about an ack and a cancel round trip of paper-venue time
  // quoting resumes by itself at the next snapshot
  warm_up(s, 2'000'000);
  CHECK(s.active() && s.open_orders() > 0);
}

void test_stale_feed() {
  LiveConfig c = small_cfg();
  c.stale_ns = 3'000'000'000ULL;
  LiveSession s(c);
  warm_up(s, 1'000'000);
  const std::uint64_t last = 1'004'000;
  CHECK(s.next_deadline() == last + c.stale_ns);
  s.poll(last + c.stale_ns - 1);
  CHECK(s.active() && s.stats().stale_events == 0);
  s.poll(last + c.stale_ns);
  CHECK(!s.active() && s.stats().stale_events == 1);
  CHECK(s.summaries().back().reason == EndReason::Stale && s.summaries().back().flat);
  CHECK(!s.summaries().back().kill_latched);
  CHECK(s.summaries().back().flatten_sim_ns >= 50'000);  // timed from the moment the silence was noticed, not from the last packet
  CHECK(s.take_resnap() && !s.take_resnap());
  // the connection comes back to life with a stale update: it belongs to no segment
  s.on_line(upd(last + c.stale_ns + 10, "buy", "100.00", "9"), last + c.stale_ns + 10);
  CHECK(!s.active() && s.stats().dropped_no_segment == 1);
  // a fresh snapshot restores quoting
  warm_up(s, last + c.stale_ns + 1000);
  CHECK(s.active() && s.open_orders() > 0);
  CHECK(s.stats().stale_events == 1);
}

void test_stale_found_by_next_line() {
  LiveConfig c = small_cfg();
  c.stale_ns = 1'000'000;
  LiveSession s(c);
  warm_up(s, 100'000);
  // no poll; the next line arrives after the silence: the silence is acted on first, the line is not applied
  s.on_line(upd(5'000'000, "buy", "100.00", "7"), 5'000'000);
  CHECK(!s.active() && s.stats().stale_events == 1 && s.stats().dropped_no_segment == 1);
  CHECK(s.summaries().back().reason == EndReason::Stale && s.summaries().back().flat);
}

void test_heartbeats_keep_the_feed_alive() {
  LiveConfig c = small_cfg();
  c.stale_ns = 3'000'000'000ULL;
  LiveSession s(c);
  warm_up(s, 1'000'000);
  std::uint64_t t = 1'004'000;
  for (int i = 0; i < 20; ++i) {  // 20 s of nothing but one heartbeat a second
    t += 1'000'000'000ULL;
    s.on_line(beat(t), t);
    s.poll(t + 1);
  }
  CHECK(s.active() && s.stats().stale_events == 0 && s.stats().other == 20);
  // then they stop
  s.poll(t + c.stale_ns + 5);
  CHECK(!s.active() && s.stats().stale_events == 1);
}

// A second snapshot on the same connection (nothing said about the first) replaces the book: the old
// segment's quotes are pulled before the new one starts.
void test_snapshot_replaces_segment() {
  LiveSession s(small_cfg());
  warm_up(s, 1'000'000);
  CHECK(s.open_orders() > 0);
  s.on_line(snapshot_line(1'010'000, 500), 1'010'000);  // the market has moved to 105.00
  CHECK(s.active() && s.stats().segments == 2 && s.summaries().size() == 1);
  CHECK(s.summaries()[0].reason == EndReason::NewSnapshot && s.summaries()[0].flat);
  CHECK(!s.summaries()[0].kill_latched && !s.halted());
  CHECK(s.stats().canc == s.stats().acc);  // the first segment's orders are all gone
  CHECK(s.book()->best_bid().raw() == 10500LL * 100);
}

void test_kill() {
  LiveSession s(small_cfg());
  warm_up(s, 1'000'000);
  CHECK(s.open_orders() > 0 && !s.kill_switch_latched());
  s.kill(1'006'000);
  CHECK(!s.active() && s.halted() && s.killed());
  const SegmentSummary& sm = s.summaries().back();
  CHECK(sm.reason == EndReason::Kill && sm.flat && sm.kill_latched);
  CHECK(s.stats().kill_cancels > 0);
  CHECK(s.stats().canc == s.stats().acc && s.stats().flatten_failures == 0);
  CHECK(s.stats().max_flatten_sim_ns >= 90'000);  // timed from the moment of the kill, not from the last packet
  // nothing after a kill is used
  const std::uint64_t lines = s.stats().lines;
  s.on_line(snapshot_line(2'000'000), 2'000'000);
  s.on_line(upd(2'001'000, "buy", "100.00", "9"), 2'001'000);
  CHECK(!s.active() && s.stats().ignored_halted == 2 && s.stats().lines == lines + 2);
  CHECK(s.stats().sent_new == s.stats().acc + s.stats().rej_cross);
  // kill with nothing running is harmless
  LiveSession e(small_cfg());
  e.kill(5);
  CHECK(e.halted() && e.killed() && e.summaries().empty());
}

// Quiet market, acks long back, then the kill: the cancels must be timed from the kill, not from the last packet.
void test_kill_after_quiet_period() {
  LiveSession s(small_cfg());
  warm_up(s, 1'000'000);
  s.poll(1'200'000);  // acks come back, the strategy reacts
  CHECK(s.gateway_stats()->reports > 0 && s.open_orders() > 0);
  s.kill(5'000'000);
  const SegmentSummary& sm = s.summaries().back();
  CHECK(sm.reason == EndReason::Kill && sm.flat && sm.kill_latched);
  CHECK(s.stats().kill_cancels > 0);
  CHECK(sm.flatten_sim_ns >= 50'000 && sm.flatten_sim_ns < 1'000'000);  // one cancel round trip after the kill, not zero
}

void test_shutdown_is_not_a_kill() {
  LiveSession s(small_cfg());
  warm_up(s, 1'000'000);
  s.shutdown(1'006'000);
  const SegmentSummary& sm = s.summaries().back();
  CHECK(sm.reason == EndReason::Shutdown && sm.flat && !sm.kill_latched);
  CHECK(s.halted() && !s.killed() && s.stats().kill_cancels == 0);
}

void test_window_exit() {
  LiveSession s(small_cfg());  // 4096 ticks of $0.01: 40.96 wide, exit when the touch is within 10% of an edge
  warm_up(s, 1'000'000);
  CHECK(!s.take_resnap());
  s.on_line(upd(1'010'000, "sell", "139.00", "1"), 1'010'000);  // far above the window's top: outside the grid, dropped
  CHECK(s.active());
  // the ask side walks away: the touch itself reaches the margin
  std::uint64_t t = 1'020'000;
  const char* lvls[] = {"100.10", "100.11", "100.12", "100.13", "100.14", "100.15", "100.16", "100.17"};
  for (const char* p : lvls) {
    t += 1000;
    s.on_line(upd(t, "sell", p, "0"), t);
  }
  t += 1000;
  s.on_line(upd(t, "sell", "118.00", "1"), t);
  CHECK(!s.active());
  CHECK(s.summaries().back().reason == EndReason::Window && s.summaries().back().flat);
  CHECK(s.take_resnap());
  CHECK(s.stats().stale_events == 0);
  // the rest of that connection is not used
  t += 1000;
  s.on_line(upd(t, "buy", "100.00", "9"), t);
  CHECK(s.stats().dropped_no_segment == 1);
  // a snapshot centred on the new market starts a new segment on a new grid
  t += 1000;
  s.on_line(snapshot_line(t, 1800), t);
  CHECK(s.active() && s.stats().segments == 2);
}

void test_integrity_failure_halts_and_flattens() {
  LiveConfig c = small_cfg();
  c.check_every = 1;
  LiveSession s(c);
  warm_up(s, 1'000'000);
  CHECK(s.active() && s.stats().check_failures == 0 && s.stats().checks > 0);
  CHECK(s.open_orders() > 0);
  // corrupt the model: from now on the book and the model disagree
  CHECK(s.converter_for_test()->debug_forget_best_bid());
  s.on_line(upd(1'006'000, "sell", "100.50", "4"), 1'006'000);
  CHECK(!s.active() && s.halted() && !s.killed());
  CHECK(s.stats().check_failures == 1);
  const SegmentSummary& sm = s.summaries().back();
  CHECK(sm.reason == EndReason::Integrity && sm.flat && sm.kill_latched);
  CHECK(!sm.book_matched_model);  // the final comparison says the same
  CHECK(s.stats().final_mismatches == 1);
  CHECK(s.stats().canc == s.stats().acc);
  s.on_line(snapshot_line(2'000'000), 2'000'000);
  CHECK(!s.active() && s.stats().ignored_halted == 1);
}

// With the periodic comparison off, a book that has drifted from the model is still found when the segment
// ends, and the session refuses to start another one on a book it cannot vouch for.
void test_final_mismatch_halts() {
  LiveConfig c = small_cfg();
  c.check_every = 0;
  LiveSession s(c);
  warm_up(s, 1'000'000);
  CHECK(s.converter_for_test()->debug_forget_best_bid());
  s.on_line("#1006000\treconnect", 1'006'000);
  CHECK(!s.active() && s.stats().check_failures == 0 && s.stats().final_mismatches == 1);
  CHECK(!s.summaries().back().book_matched_model && s.summaries().back().flat);
  CHECK(s.halted() && !s.killed());
  s.on_line(snapshot_line(2'000'000), 2'000'000);
  CHECK(!s.active() && s.stats().segments == 1);
}

// The level count and the touch can agree while a level further down has the wrong size: only the
// level-by-level comparison at the end of the segment sees that.
void test_final_comparison_is_level_by_level() {
  LiveConfig c = small_cfg();
  c.check_every = 0;
  LiveSession s(c);
  warm_up(s, 1'000'000);
  CHECK(s.converter_for_test()->debug_bump_a_bid_size());
  s.on_line("#1006000\treconnect", 1'006'000);
  CHECK(s.stats().final_mismatches == 1 && !s.summaries().back().book_matched_model && s.halted());
}

void test_flatten_failure_kills() {
  LiveConfig c = small_cfg();
  c.max_flatten_rounds = 1;  // not enough rounds for an ack to come back and a cancel to go out
  LiveSession s(c);
  warm_up(s, 1'000'000);
  s.on_line("#1006000\treconnect", 1'006'000);
  CHECK(!s.active() && s.stats().flatten_failures == 1 && s.halted());
  const SegmentSummary& sm = s.summaries().back();
  CHECK(!sm.flat && sm.kill_latched);  // could not get flat: the kill switch is on, trading stops
}

void test_unusable_snapshots_and_noise() {
  LiveSession s(small_cfg());
  s.on_line("", 1);
  s.on_line("#10\tstart\turl\tX", 10);
  s.on_line("no tab here", 11);
  s.on_line("\t{}", 12);
  s.on_line("12x\t{}", 13);
  s.on_line("1000\t{\"type\":\"snapshot\",\"bids\":[[\"100.00\",\"1\"]],\"asks\":[]}", 1000);  // no ask: no mid to centre on
  CHECK(!s.active() && s.stats().snapshots_unusable == 1);
  s.on_line(upd(1100, "buy", "100.00", "2"), 1100);
  CHECK(s.stats().dropped_no_segment == 1);
  s.on_line("1200\t{\"type\":\"snapshot\",\"bids\":", 1200);  // damaged
  CHECK(s.stats().bad == 1 && !s.active());
  CHECK(s.stats().bad_lines == 3 && s.stats().markers == 1);
  // a start marker does not end a running segment; other markers do
  s.on_line(snapshot_line(2000), 2000);
  CHECK(s.active());
  s.on_line("#2100\tstart\tagain", 2100);
  CHECK(s.active());
  s.on_line("#2200\tidle_timeout", 2200);
  CHECK(!s.active() && s.summaries().back().reason == EndReason::Marker);
}

void test_time_going_backwards() {
  LiveSession a(small_cfg()), b(small_cfg());
  // b sees the same lines but with a clock that steps back; the session must behave as if it had stood still
  a.on_line(snapshot_line(5000), 5000);
  a.on_line(upd(5100, "buy", "100.00", "9"), 5100);
  a.on_line(upd(5200, "sell", "100.10", "9"), 5100);
  b.on_line(snapshot_line(5000), 5000);
  b.on_line(upd(5100, "buy", "100.00", "9"), 5100);
  b.on_line(upd(5200, "sell", "100.10", "9"), 100);  // earlier than before
  CHECK(b.stats().time_regressions == 1 && a.stats().time_regressions == 0);
  CHECK(b.next_deadline() == a.next_deadline());  // the stale deadline is built on the clamped time, not the regressed one
  a.shutdown(6000);
  b.shutdown(6000);
  CHECK(a.summaries()[0].hash_before_flatten == b.summaries()[0].hash_before_flatten);
}

void test_timer_records_only_work_lines() {
  LiveConfig c = small_cfg();
  c.timed = true;
  LiveSession s(c);
  warm_up(s, 1'000'000);          // 1 snapshot + 4 updates
  s.on_line(beat(1'010'000), 1'010'000);   // a heartbeat is not work
  s.on_line("garbage", 1'011'000);
  CHECK(s.line_ns().count() == 4 && s.start_ns().count() == 1);  // the snapshot is timed apart
  CHECK(s.line_ns().max() > 0 && s.start_ns().max() > 0);
}

}  // namespace

int main() {
  test_equivalence();
  test_multi_packet_messages();
  test_deterministic();
  test_flatten_with_orders_in_flight();
  test_stale_feed();
  test_stale_found_by_next_line();
  test_heartbeats_keep_the_feed_alive();
  test_snapshot_replaces_segment();
  test_kill();
  test_kill_after_quiet_period();
  test_shutdown_is_not_a_kill();
  test_window_exit();
  test_integrity_failure_halts_and_flattens();
  test_final_mismatch_halts();
  test_final_comparison_is_level_by_level();
  test_flatten_failure_kills();
  test_unusable_snapshots_and_noise();
  test_time_going_backwards();
  test_timer_records_only_work_lines();
  if (g_failures) {
    std::fprintf(stderr, "test_live: %d FAILURES\n", g_failures);
    return 1;
  }
  std::printf("test_live: all checks passed\n");
  return 0;
}
