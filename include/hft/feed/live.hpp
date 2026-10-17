#pragma once
// A live paper session: the real-time sibling of the offline replay.
//
//   capture-format lines --> LiveSession --> L2Converter --> ITCH packets
//   (from a pipe or a file)       |                               |
//                                 |                               v
//                                 |            Pipeline<HashSend> <-- reports -- AckVenue (paper, never fills)
//                                 v
//                        guards: stale feed, reconnect, window exit,
//                                model check, kill, shutdown
//
// What it adds over replay (include/hft/feed/replay.hpp), which runs a finished
// file:
//  * Time comes from the caller, one `now` per call, so the same code runs on a
//    real clock (the hft_live tool reads a monotonic clock) or on recorded
//    stamps (tests, drills). Nothing in here reads a clock except the optional
//    per-line timer.
//  * Every way a live feed can stop being trustworthy ends the segment with the
//    same procedure, flatten(): cancel everything, answer the cancels, repeat
//    until nothing is working at the venue or in the OMS. Reasons:
//      - a reconnect, idle timeout or error marker from the capture process;
//      - a new snapshot (the previous book is replaced);
//      - the price leaving the grid window (also asks the caller to resubscribe);
//      - silence: no feed line at all for `stale_ns` (a dead connection that the
//        socket has not noticed yet);
//      - the periodic comparison with the independent model failing (halts);
//      - kill() and shutdown().
//    Quoting resumes only at the next snapshot, never by guessing.
//  * kill() is not the same as a flatten: it latches the risk engine's kill
//    switch, so no new order can be sent for the rest of the session.
//
// The paper venue is simulated: its 50 us round trip is added to the caller's
// clock; no real exchange is involved and nothing is ever filled.
//
// Tooling code: allocates freely (per segment, never inside the pipeline).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/feed/l2.hpp"
#include "hft/feed/replay.hpp"
#include "hft/risk/risk.hpp"

namespace hft::feed {

// Called for every frame the gateway sends (new, replace, cancel), after the paper venue has it.
// Must not block or allocate; it runs inside the tick-to-trade path. `seg` is the segment number (1-based).
using OrderTap = void (*)(void* ctx, std::uint64_t seg, const std::uint8_t* frame, std::size_t n) noexcept;

struct LiveConfig {
  L2Config l2;
  std::uint64_t venue_rtt_ns = 50'000;
  std::uint64_t stale_ns = 3'000'000'000ULL;  // no feed line at all for this long: pull the quotes
  std::int64_t mm_qty_lots = 5;
  std::uint32_t max_orders = 1u << 17;        // book capacity (rounded up to a power of two); must exceed live levels
  std::uint32_t check_every = 100;            // packets between comparisons with the model; 0 = never
  std::size_t arena_bytes = 256u << 20;       // working memory per segment (lazily committed)
  bool timed = false;                         // time each line with the steady clock (parse + convert + pipeline)
  std::uint32_t max_flatten_rounds = 64;      // cancel/answer rounds before giving up and killing
  // External venue: orders go only to order_tap (the paper venue is not used) and the exchange's reports come back through
  // on_exchange_report(). Flatten then waits for real cancel confirmations, at most flatten_timeout_ns of the caller's clock.
  bool external_venue = false;
  std::uint64_t flatten_timeout_ns = 5'000'000'000ULL;
  // External venue only. When the session ends for good (shutdown or kill), after the cancels are confirmed, bring the net position
  // to zero with reduce-only IOC orders priced close_collar_bps beyond the touch (each attempt waits at most flatten_timeout_ns).
  // The feed must be fresh and the book two-sided; if not, or after close_max_attempts, the position stays and the summary says so.
  bool close_position = false;
  std::int64_t close_collar_bps = 30;
  std::uint32_t close_max_attempts = 5;
  OrderTap order_tap = nullptr;               // mirror of the order flow for an order-entry bridge (nullptr: none)
  void* order_tap_ctx = nullptr;
};

enum class EndReason : std::uint8_t { Marker, NewSnapshot, Window, Stale, Integrity, Kill, Shutdown, kCount };

inline const char* end_reason_name(EndReason r) noexcept {
  switch (r) {
    case EndReason::Marker: return "reconnect/marker";
    case EndReason::NewSnapshot: return "new snapshot";
    case EndReason::Window: return "left price window";
    case EndReason::Stale: return "stale feed";
    case EndReason::Integrity: return "integrity failure";
    case EndReason::Kill: return "kill";
    case EndReason::Shutdown: return "shutdown";
    default: return "?";
  }
}

// What one segment looked like when it ended.
struct SegmentSummary {
  EndReason reason = EndReason::Marker;
  std::uint64_t packets = 0;
  std::uint64_t frames_before_flatten = 0, hash_before_flatten = 0;  // the order flow the feed itself caused
  std::uint64_t frames_total = 0;
  std::uint64_t flatten_rounds = 0, flatten_sim_ns = 0;
  std::uint32_t open_before_flatten = 0;
  std::int64_t net_position_lots = 0;  // signed filled quantity at the end of the segment (a flatten only cancels orders; --close-position closes it, ADR 29)
  std::uint64_t fills = 0;  // orders the flatten had to take down
  std::uint32_t close_attempts = 0;    // closing orders sent (close_position)
  std::int64_t closed_lots = 0;        // |position| before closing minus |position| after
  bool close_failed = false;           // closing was wanted and the position is not zero
  const char* close_why_not = "";      // when close_failed: why it stopped
  std::uint8_t close_risk_code = 0;    // when the risk engine refused: its RiskResult
  bool flat = false;            // nothing left working at the venue or in the OMS
  bool kill_latched = false;    // the risk engine's kill switch was on when the segment ended
  bool book_matched_model = false;
};

struct LiveStats {
  // input
  std::uint64_t lines = 0, messages = 0, other = 0, bad = 0, bad_lines = 0, markers = 0, time_regressions = 0;
  std::uint64_t dropped_no_segment = 0, ignored_halted = 0, snapshots_unusable = 0, init_failures = 0;
  // segments
  std::uint64_t segments = 0;
  std::uint64_t ended[static_cast<int>(EndReason::kCount)] = {};
  std::uint64_t packets = 0, checks = 0, check_failures = 0, final_mismatches = 0;
  std::uint64_t stale_events = 0, resnap_requests = 0, flatten_failures = 0, max_flatten_rounds = 0;
  std::uint64_t max_flatten_sim_ns = 0;
  // summed over finished segments
  std::uint64_t messages_applied = 0, decisions = 0, feed_losses = 0, pull_sweeps = 0, desyncs = 0, book_errors = 0;
  std::uint64_t sent_new = 0, sent_replace = 0, sent_cancel = 0, risk_rejects = 0, pulled_cancels = 0, kill_cancels = 0;
  std::uint64_t reports = 0, acc = 0, rep = 0, canc = 0, rej_cross = 0;
  std::uint64_t fills = 0;
  std::uint64_t close_orders = 0, close_failures = 0;
  // external venue
  std::uint64_t exchange_reports = 0, reports_stale = 0, snapshots_dropped_ending = 0, ignored_ending = 0;
  std::uint64_t conv_adds = 0, conv_reduces = 0, conv_deletes = 0, out_of_window = 0, crossed = 0;
};

class LiveSession {
 public:
  explicit LiveSession(const LiveConfig& cfg) : cfg_(cfg) {
    cfg_.max_orders = detail::pow2_at_least(cfg_.max_orders);
  }
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  // One capture-format line: "<t>\t<json>" or "#<t>\t<event>...". The stamp in
  // the line is ignored; `now` (ns, monotonic from the caller) is the time.
  void on_line(std::string_view line, std::uint64_t now) {
    now = clamp(now);
    ++st_.lines;
    if (halted_) {
      ++st_.ignored_halted;
      return;
    }
    check_stale(now);
    if (line.empty()) return;
    if (line[0] == '#') {
      ++st_.markers;
      const std::size_t tab = line.find('\t');
      const std::string_view ev = tab == std::string_view::npos ? std::string_view{} : line.substr(tab + 1);
      if (ev.substr(0, 5) != "start") end_segment(now, EndReason::Marker);  // reconnect, idle_timeout, error, stop...
      return;
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0 || tab > 20) {
      ++st_.bad_lines;
      return;
    }
    for (std::size_t i = 0; i < tab; ++i) {
      if (line[i] < '0' || line[i] > '9') {
        ++st_.bad_lines;
        return;
      }
    }
    using clk = std::chrono::steady_clock;
    clk::time_point t0{};
    if (cfg_.timed) t0 = clk::now();
    ++st_.messages;
    const L2Message m = parse_l2(line.substr(tab + 1));
    bool work = false, snapshot = false;
    switch (m.kind) {
      case L2Message::Kind::Bad:
        ++st_.bad;
        return;
      case L2Message::Kind::Other:
        ++st_.other;
        last_feed_ = now;  // a heartbeat or a subscription ack proves the connection is alive
        return;
      case L2Message::Kind::Snapshot:
        last_feed_ = now;
        end_segment(now, EndReason::NewSnapshot);
        if (seg_) {  // external venue: the old segment is still waiting for its cancels. Ask for another snapshot instead.
          ++st_.snapshots_dropped_ending;
          if (!resnap_) ++st_.resnap_requests;
          resnap_ = true;
          return;
        }
        start_segment(m, now);
        work = true;
        snapshot = true;
        break;
      case L2Message::Kind::Update:
        last_feed_ = now;
        if (!seg_) {
          ++st_.dropped_no_segment;
          return;
        }
        if (seg_->ending) {
          ++st_.ignored_ending;
          return;
        }
        apply_update(m, now);
        work = true;
        break;
    }
    if (cfg_.timed && work) {
      const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
      (snapshot ? start_ns_ : line_ns_).record(ns);  // building a segment (memory, book, thousands of levels) is not a tick
    }
  }

  // Call whenever time passes without input (the tool does it every millisecond
  // on a pipe). Delivers due venue reports and pulls the quotes if the feed has
  // gone silent.
  void poll(std::uint64_t now) {
    now = clamp(now);
    if (seg_ && seg_->ending) {  // waiting for the exchange to confirm the cancels
      step_flatten(now);
      return;
    }
    if (halted_) return;
    check_stale(now);
    if (!seg_) return;
    // Reports that came due while the feed was quiet reach the gateway and the strategy. (There is no
    // feed-handler timer to run: a stream the converter makes itself has no sequence gaps to wait out.)
    deliver_due(*seg_, now);
  }

  // The next time poll() has something to do on its own (for driving recorded
  // time); ~0 when nothing is pending.
  std::uint64_t next_deadline() const noexcept { return seg_ && !halted_ ? last_feed_ + cfg_.stale_ns : ~0ULL; }

  // Latch the kill switch, cancel everything, stop. No further input is used.
  void kill(std::uint64_t now) {
    now = clamp(now);
    if (seg_) {
      seg_->venue.set_now(now);  // the kill sweep goes out now, not at the time of the last packet
      seg_->pipe->gateway().kill(risk::KillReason::Manual, now);
    }
    end_segment(now, EndReason::Kill);
    halted_ = true;
    killed_ = true;
  }
  // Flatten and stop without latching the kill switch.
  void shutdown(std::uint64_t now) {
    now = clamp(now);
    end_segment(now, EndReason::Shutdown);
    halted_ = true;
  }

  // True once after the price left the window (or the feed stalled): the
  // capture process should reconnect to get a snapshot centred on the market.
  bool take_resnap() noexcept {
    const bool r = resnap_;
    resnap_ = false;
    return r;
  }

  // External venue only. A report from the exchange for segment `seg` (the number the order tap was given). Reports for a
  // segment that is gone are counted and dropped. During a flatten the strategy does not see them.
  void on_exchange_report(std::uint64_t seg, const sim::Report& r, std::uint64_t now) {
    now = clamp(now);
    if (!seg_ || seg != seg_->number) {
      ++st_.reports_stale;
      return;
    }
    Seg& s = *seg_;
    std::uint8_t buf[gw::kMaxFrame];
    sim::Report rr = r;
    rr.seq = ++s.xseq;
    rr.ts = now;
    const std::size_t n = gw::encode_report(buf, s.xseq, rr);
    ++st_.exchange_reports;
    if (s.ending) {
      s.pipe->gateway().on_bytes(buf, n, now);
      step_flatten(now);
    } else {
      s.pipe->on_report(buf, n, now);
    }
  }
  // True while a segment is waiting for the exchange to confirm its cancels (external venue). The caller must keep
  // feeding reports and calling poll() until this is false.
  bool busy() const noexcept { return seg_ && seg_->ending; }
  std::uint64_t segment_number() const noexcept { return seg_ ? seg_->number : 0; }

  bool halted() const noexcept { return halted_; }
  bool killed() const noexcept { return killed_; }
  bool active() const noexcept { return seg_ != nullptr; }
  const LiveStats& stats() const noexcept { return st_; }
  const std::vector<SegmentSummary>& summaries() const noexcept { return sums_; }
  const core::Histogram& line_ns() const noexcept { return line_ns_; }
  const core::Histogram& start_ns() const noexcept { return start_ns_; }  // snapshot line to ready, one per segment
  // The running segment's pipeline, for inspection by tests and status lines.
  const book::OrderBook* book() const noexcept { return seg_ ? &seg_->pipe->book() : nullptr; }
  std::int64_t position_lots() const noexcept { return seg_ ? seg_->pipe->gateway().oms().position().pos : 0; }
  std::uint32_t open_orders() const noexcept { return seg_ ? seg_->pipe->gateway().oms().open_orders() : 0; }
  std::size_t venue_live() const noexcept { return seg_ ? seg_->venue.live() : 0; }
  const gw::GatewayStats* gateway_stats() const noexcept { return seg_ ? &seg_->pipe->gateway().stats() : nullptr; }
  bool kill_switch_latched() const noexcept { return seg_ && seg_->pipe->gateway().risk().killed(); }
  L2Converter* converter_for_test() noexcept { return seg_ ? &seg_->conv : nullptr; }

 private:
  struct LiveSend {
    detail::HashSend h;
    OrderTap tap;
    void* ctx;
    std::uint64_t seg;
    void operator()(const std::uint8_t* d, std::size_t n) noexcept {
      h(d, n);  // hashes and counts; hands the frame to the paper venue unless there is none
      if (tap) tap(ctx, seg, d, n);
    }
  };
  using Pipe = t2t::Pipeline<LiveSend>;
  struct Seg;
  struct Sink {
    Seg* s;
    void operator()(const std::uint8_t* d, std::size_t n) noexcept;
  };
  struct Seg {
    explicit Seg(const L2Config& c, std::uint64_t rtt) : conv(c), venue(rtt, nullptr) {}
    L2Converter conv;
    std::vector<std::uint8_t> blob;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pk;
    std::unique_ptr<sim::ItchPublisher<Sink>> pub;
    core::Region region;
    core::Arena arena;
    AckVenue venue;
    std::unique_ptr<Pipe> pipe;
    std::uint64_t hash = 0xCBF29CE484222325ULL, frames = 0, bytes = 0;
    std::uint64_t pkts = 0, last_cp = 0;
    // external venue
    std::uint64_t number = 0, xseq = 0;
    bool ending = false;
    bool final_stop = false;     // shutdown or kill has been asked for (possibly while a routine end was already waiting)
    bool closing = false;        // a closing order has been sent: the IOC cancels itself, so the flatten must not cancel it
    std::uint32_t close_tries = 0;
    std::int64_t pos_before_close = 0;
    EndReason end_why = EndReason::Marker;
    std::uint64_t end_started = 0, end_deadline = 0;
    SegmentSummary pending;
  };

  std::uint64_t clamp(std::uint64_t now) noexcept {
    if (now < last_now_) {
      ++st_.time_regressions;
      return last_now_;
    }
    return last_now_ = now;
  }

  void check_stale(std::uint64_t now) {
    if (seg_ && !seg_->ending && now >= last_feed_ && now - last_feed_ >= cfg_.stale_ns) {
      ++st_.stale_events;
      resnap_ = true;
      ++st_.resnap_requests;
      end_segment(now, EndReason::Stale);
    }
  }

  void start_segment(const L2Message& m, std::uint64_t now) {
    seg_ = std::make_unique<Seg>(cfg_.l2, cfg_.venue_rtt_ns);
    Seg& s = *seg_;
    s.pub = std::make_unique<sim::ItchPublisher<Sink>>("L2REPLAY01", 1, Sink{&s}, 1);
    auto emit = [&](const sim::PublicEvent& e) { s.pub->on_public(e); };
    if (!s.conv.on_snapshot(m, now, emit)) {
      ++st_.snapshots_unusable;
      seg_.reset();
      return;
    }
    s.pub->flush();
    t2t::PipelineConfig pc;
    pc.md.locate = 1;
    pc.book.base = core::Price::from_raw(s.conv.base_raw());
    pc.book.tick_raw = cfg_.l2.tick_raw;
    pc.book.num_levels = cfg_.l2.num_levels;
    pc.book.max_orders = cfg_.max_orders;
    pc.mm.qty = cfg_.mm_qty_lots;
    pc.first_seq = 1;
    pc.live_at_start = true;
    s.region = core::Region::create(cfg_.arena_bytes, core::RegionOptions{false, false, true, -1});
    s.arena = core::Arena(s.region);
    s.number = st_.segments + 1;
    s.pipe.reset(new Pipe(LiveSend{detail::HashSend{&s.hash, &s.frames, &s.bytes, cfg_.external_venue ? nullptr : &s.venue},
                                   cfg_.order_tap, cfg_.order_tap_ctx, s.number},
                          pc.mm));
    if (s.region.data() == nullptr || !s.pipe->init(s.arena, pc)) {
      ++st_.init_failures;
      seg_.reset();
      return;
    }
    s.venue.set_book(&s.pipe->book());
    ++st_.segments;
    last_feed_ = now;
    run_packets(s, now);
    after_work(now);
  }

  void apply_update(const L2Message& m, std::uint64_t now) {
    Seg& s = *seg_;
    auto emit = [&](const sim::PublicEvent& e) { s.pub->on_public(e); };
    s.conv.on_update(m, now, emit);
    s.pub->flush();
    run_packets(s, now);
    after_work(now);
  }

  // Integrity failure and window exit are handled after the packets are in, and
  // never inside the loop that owns the segment.
  void after_work(std::uint64_t now) {
    if (!seg_) return;
    if (integrity_failed_) {
      integrity_failed_ = false;
      seg_->venue.set_now(now);
      seg_->pipe->gateway().kill(risk::KillReason::Manual, now);
      end_segment(now, EndReason::Integrity);
      halted_ = true;
      return;
    }
    if (seg_->conv.window_exit()) {
      resnap_ = true;
      ++st_.resnap_requests;
      end_segment(now, EndReason::Window);
    }
  }

  void run_packets(Seg& s, std::uint64_t now) {
    for (const auto& p : s.pk) {
      deliver_due(s, now);
      s.venue.set_now(now);
      s.pipe->on_packet(md::Line::A, s.blob.data() + p.first, p.second, now);
      ++s.pkts;
      ++st_.packets;
    }
    s.pk.clear();
    s.blob.clear();
    // Compare only here, between feed messages: the model has already absorbed the whole message, so
    // inside a message that spans several packets the book is legitimately behind it.
    if (cfg_.check_every && s.pkts - s.last_cp >= cfg_.check_every) {
      s.last_cp = s.pkts;
      ++st_.checks;
      if (!detail::matches(s.pipe->book(), model_checkpoint(s.conv, s.pkts))) {
        ++st_.check_failures;
        integrity_failed_ = true;
      }
    }
  }

  void deliver_due(Seg& s, std::uint64_t upto) {
    std::uint8_t rep[gw::kMaxFrame];
    while (s.venue.has_due(upto)) {
      const std::uint64_t at = s.venue.front_time();
      const std::size_t n = s.venue.pop(rep);
      s.venue.set_now(at);
      s.pipe->on_report(rep, n, at);
    }
  }

  // Cancels everything and runs the paper venue forward until nothing is
  // working. Reports go to the gateway only: the strategy must not get a look,
  // or it would quote again. An order still PendingNew cannot be cancelled until
  // its ack arrives, hence the rounds. The venue's time is simulated, so the
  // loop does not wait. With an external venue the same job is a state: the cancels go out,
  // and the segment ends when the exchange has confirmed them (step_flatten) or the timeout passes.
  void end_segment(std::uint64_t now, EndReason why) {
    if (!seg_) return;
    Seg& s = *seg_;
    if (s.ending) {  // already on its way out; the first reason stands, but a stop for good is remembered
      if (why == EndReason::Kill || why == EndReason::Shutdown) s.final_stop = true;
      return;
    }
    SegmentSummary sm;
    sm.reason = why;
    sm.packets = s.pkts;
    sm.frames_before_flatten = s.frames;
    sm.hash_before_flatten = s.hash;
    sm.open_before_flatten = s.pipe->gateway().oms().open_orders();
    auto& g = s.pipe->gateway();
    if (cfg_.external_venue) {
      s.ending = true;
      s.final_stop = why == EndReason::Kill || why == EndReason::Shutdown;
      s.end_why = why;
      s.end_started = now;
      s.end_deadline = now + cfg_.flatten_timeout_ns;
      s.pending = sm;
      g.cancel_all(now);
      step_flatten(now);
      return;
    }

    std::uint64_t t = now;
    std::uint8_t rep[gw::kMaxFrame];
    std::uint64_t rounds = 0;
    bool flat = false;
    while (rounds < cfg_.max_flatten_rounds) {
      ++rounds;
      s.venue.set_now(t);  // the cancels go out now, not at the time of the last packet
      g.cancel_all(t);
      while (!s.venue.empty()) {
        const std::uint64_t at = s.venue.front_time() > t ? s.venue.front_time() : t;
        const std::size_t n = s.venue.pop(rep);
        s.venue.set_now(at);
        g.on_bytes(rep, n, at);
        t = at;
      }
      if (g.oms().open_orders() == 0 && s.venue.live() == 0) {
        flat = true;
        break;
      }
    }
    finish_segment(sm, flat, rounds, t, now, why);
  }

  // External venue: called after every report and every poll while a segment is ending.
  void step_flatten(std::uint64_t now) {
    Seg& s = *seg_;
    auto& g = s.pipe->gateway();
    if (g.oms().open_orders() == 0) {
      if (cfg_.close_position && s.final_stop && !s.pending.close_failed && step_close(now)) return;  // an order is out: wait for its reports
      finish_segment(s.pending, true, 0, now, s.end_started, s.end_why);
    } else if (now >= s.end_deadline) {
      finish_segment(s.pending, false, 0, now, s.end_started, s.end_why);
    } else if (!s.closing) {
      g.cancel_all(now);  // an order whose ack just arrived can be cancelled now
    }
  }

  // The cancels are confirmed and the session is stopping for good. If the position is not zero, send the next reduce-only IOC.
  // Returns true while an order is out. On any reason not to, records it in the summary and returns false (the segment then ends).
  bool step_close(std::uint64_t now) {
    Seg& s = *seg_;
    auto& g = s.pipe->gateway();
    const std::int64_t pos = g.oms().position().pos;
    if (pos == 0) return false;
    auto stop = [&](const char* why) {
      s.pending.close_failed = true;
      s.pending.close_why_not = why;
      ++st_.close_failures;
      return false;
    };
    if (s.close_tries >= cfg_.close_max_attempts) return stop("attempts used up");
    if (!(now >= last_feed_ && now - last_feed_ < cfg_.stale_ns)) return stop("feed not fresh");
    const auto& bk = s.pipe->book();
    const core::Price bid = bk.best_bid(), ask = bk.best_ask();
    if (bid == core::Price::invalid() || ask == core::Price::invalid()) return stop("no two-sided book");
    const std::int64_t tick = cfg_.l2.tick_raw > 0 ? cfg_.l2.tick_raw : 1;
    const bool sell = pos > 0;
    std::int64_t px;
    if (sell) {  // as low as the collar allows, on the grid, rounded away from the market
      px = bid.raw() - bid.raw() * cfg_.close_collar_bps / 10'000;
      px = px / tick * tick;
    } else {
      px = ask.raw() + ask.raw() * cfg_.close_collar_bps / 10'000;
      px = (px + tick - 1) / tick * tick;
    }
    const std::int64_t want = sell ? pos : -pos;
    // the whole position in one order; the strategy's own cap keeps it far below max_order_qty, and if it were not, the risk engine refuses
    const auto r = g.close_order(sell ? core::Side::Sell : core::Side::Buy, core::Price::from_raw(px), want, now);
    if (!r.ok()) {
      s.pending.close_risk_code = static_cast<std::uint8_t>(r.risk);
      return stop("refused by risk or OMS");
    }
    if (s.close_tries == 0) s.pos_before_close = pos;
    s.closing = true;
    ++s.close_tries;
    ++s.pending.close_attempts;
    ++st_.close_orders;
    s.end_deadline = now + cfg_.flatten_timeout_ns;  // each attempt gets the full time
    return true;
  }

  void finish_segment(SegmentSummary sm, bool flat, std::uint64_t rounds, std::uint64_t t, std::uint64_t now, EndReason why) {
    Seg& s = *seg_;
    auto& g = s.pipe->gateway();
    sm.net_position_lots = g.oms().position().pos;
    if (s.close_tries > 0) {
      const std::int64_t before = s.pos_before_close < 0 ? -s.pos_before_close : s.pos_before_close;
      const std::int64_t after = sm.net_position_lots < 0 ? -sm.net_position_lots : sm.net_position_lots;
      sm.closed_lots = before - after;
    }
    sm.fills = g.oms().position().fills;
    st_.fills += sm.fills;
    sm.flat = flat;
    sm.flatten_rounds = rounds;
    sm.flatten_sim_ns = t - now;
    sm.frames_total = s.frames;
    if (!flat) {
      ++st_.flatten_failures;
      g.kill(risk::KillReason::Manual, t);  // could not get flat: stop trading altogether
      halted_ = true;
    }
    sm.kill_latched = g.risk().killed();

    // the segment's last word on the book: level by level against the model
    bool ok = s.pipe->book().order_count() == s.conv.levels(core::Side::Buy).size() + s.conv.levels(core::Side::Sell).size();
    for (int side = 0; side < 2 && ok; ++side) {
      const core::Side sd = side == 0 ? core::Side::Buy : core::Side::Sell;
      for (const auto& kv : s.conv.levels(sd)) {
        if (s.pipe->book().level_qty(sd, core::Price::from_raw(kv.first * cfg_.l2.tick_raw)) != kv.second) {
          ok = false;
          break;
        }
      }
    }
    sm.book_matched_model = ok;
    if (!ok) {
      ++st_.final_mismatches;
      halted_ = true;  // a book we cannot vouch for: do not start another segment
    }

    // totals
    const auto& ps = s.pipe->stats();
    const auto& fs = s.pipe->feed().stats();
    const auto& gs = g.stats();
    const auto& vs = s.venue.stats();
    const auto& cs = s.conv.stats();
    st_.messages_applied += fs.messages;
    st_.desyncs += fs.desyncs;
    st_.book_errors += fs.book_errors;
    st_.decisions += ps.decisions;
    st_.feed_losses += ps.feed_losses;
    st_.pull_sweeps += ps.pull_sweeps;
    st_.sent_new += gs.sent_new;
    st_.sent_replace += gs.sent_replace;
    st_.sent_cancel += gs.sent_cancel;
    st_.risk_rejects += gs.risk_rejects;
    st_.pulled_cancels += gs.pulled_cancels;
    st_.kill_cancels += gs.kill_cancels;
    st_.reports += gs.reports;
    st_.acc += vs.accepted;
    st_.rep += vs.replaced;
    st_.canc += vs.canceled;
    st_.rej_cross += vs.rejected_cross;
    st_.conv_adds += cs.adds;
    st_.conv_reduces += cs.reduces;
    st_.conv_deletes += cs.deletes;
    st_.out_of_window += cs.out_of_window;
    st_.crossed += cs.crossed;
    ++st_.ended[static_cast<int>(why)];
    if (rounds > st_.max_flatten_rounds) st_.max_flatten_rounds = rounds;
    if (sm.flatten_sim_ns > st_.max_flatten_sim_ns) st_.max_flatten_sim_ns = sm.flatten_sim_ns;
    sums_.push_back(sm);
    seg_.reset();
  }

  LiveConfig cfg_;
  LiveStats st_;
  core::Histogram line_ns_, start_ns_;
  std::vector<SegmentSummary> sums_;
  std::unique_ptr<Seg> seg_;
  std::uint64_t last_now_ = 0, last_feed_ = 0;
  bool halted_ = false, killed_ = false, resnap_ = false, integrity_failed_ = false;
};

inline void LiveSession::Sink::operator()(const std::uint8_t* d, std::size_t n) noexcept {
  s->pk.emplace_back(static_cast<std::uint32_t>(s->blob.size()), static_cast<std::uint32_t>(n));
  s->blob.insert(s->blob.end(), d, d + n);
}

}  // namespace hft::feed
