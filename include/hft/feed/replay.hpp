#pragma once
// Replays a recorded level-2 capture through the production Pipeline.
//
//   capture text --> L2Converter --> ITCH packets (one per feed message)
//                                         |
//                                         v        +-------------------+
//   timestamps (receive time) ---> Pipeline<AckSend> <--- reports ---- AckVenue
//
// Two things make this more than a demo:
//  * An independent model. The converter keeps its own picture of the
//    aggregated book. At checkpoints (and at the end) the book the Pipeline
//    built from the ITCH bytes is compared with it: best prices, top-of-book
//    sizes, depth, and the number of live levels. A mismatch is a bug in the
//    converter, the wire codec, the feed handler or the book.
//  * Determinism. Replaying twice must put identical bytes on the wire.
//
// The AckVenue is deliberately dumb: it accepts, replaces and cancels, and it
// rejects a post-only order that would cross the replayed book. It never fills.
// Real data carries no information about our queue position, so any fill model
// would be invented; the queue-aware backtester (Phase 6) is where fills belong.
//
// Tooling code: allocates freely outside the timed loop.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/feed/l2.hpp"
#include "hft/gw/order_entry.hpp"
#include "hft/sim/feed_publisher.hpp"
#include "hft/t2t/pipeline.hpp"

namespace hft::feed {

// ---- capture -> segments ------------------------------------------------------------------------

struct Packet {
  std::uint64_t t;  // receive time in ns since the capture started
  std::uint32_t off, len;
};

// What the converter's model says the book looks like after `packets` packets.
struct Checkpoint {
  std::uint64_t packets;
  std::int64_t bid_px, ask_px;    // raw price, -1 when that side is empty
  core::Qty bid_qty, ask_qty;     // size at the touch
  core::Qty bid_depth, ask_depth; // total over the 5 price levels nearest the touch
  std::uint64_t live_levels;
};

struct Segment {
  std::vector<std::uint8_t> blob;
  std::vector<Packet> pkts;
  std::vector<Checkpoint> checkpoints;
  L2Config cfg;
  L2Stats stats;
  std::int64_t base_raw = 0;
  bool ended_by_window = false;
  std::map<std::int64_t, std::int64_t> final_levels[2];
};

struct LoadStats {
  std::uint64_t lines = 0, messages = 0, other = 0, bad = 0, markers = 0, bad_lines = 0;
  std::uint64_t dropped_before_snapshot = 0, snapshots_unusable = 0, segments = 0;
};

constexpr int kDepthCheck = 5;

inline Checkpoint model_checkpoint(const L2Converter& c, std::uint64_t packets) {
  Checkpoint k{};
  k.packets = packets;
  k.bid_px = k.ask_px = -1;
  const auto& b = c.levels(core::Side::Buy);
  const auto& a = c.levels(core::Side::Sell);
  if (!b.empty()) {
    const std::int64_t i = b.rbegin()->first;
    k.bid_px = c.price_of(i);
    k.bid_qty = b.rbegin()->second;
    for (std::int64_t d = 0; d < kDepthCheck; ++d) {
      const auto it = b.find(i - d);
      if (it != b.end()) k.bid_depth += it->second;
    }
  }
  if (!a.empty()) {
    const std::int64_t i = a.begin()->first;
    k.ask_px = c.price_of(i);
    k.ask_qty = a.begin()->second;
    for (std::int64_t d = 0; d < kDepthCheck; ++d) {
      const auto it = a.find(i + d);
      if (it != a.end()) k.ask_depth += it->second;
    }
  }
  k.live_levels = c.live_levels();
  return k;
}

namespace detail {
struct SegEmit {
  Segment* s;
  std::uint64_t* t;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    s->pkts.push_back({*t, static_cast<std::uint32_t>(s->blob.size()), static_cast<std::uint32_t>(n)});
    s->blob.insert(s->blob.end(), d, d + n);
  }
};
}  // namespace detail

// Reads a capture file (see scripts/capture_l2.py) into segments. A segment
// starts at a snapshot and ends at a reconnect marker, the next snapshot, the
// end of the file, or when the price leaves the grid window.
class CaptureLoader {
 public:
  // Messages stamped after `max_t_ns` are ignored (used to rebuild the book as of a moment in the capture).
  explicit CaptureLoader(const L2Config& cfg, std::uint32_t checkpoint_every = 500, std::uint64_t max_t_ns = ~0ULL)
      : cfg_(cfg), every_(checkpoint_every), max_t_(max_t_ns) {}

  bool load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) feed_line(line);
    close_segment();
    return true;
  }

  void feed_line(const std::string& line) {
    ++ls_.lines;
    if (line.empty()) return;
    if (line[0] == '#') {
      ++ls_.markers;
      const std::size_t tab = line.find('\t');
      const std::string_view ev = tab == std::string::npos ? std::string_view{} : std::string_view(line).substr(tab + 1);
      if (ev.rfind("start", 0) != 0) close_segment();  // reconnect, idle_timeout, error, stop, give_up
      return;
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab > 20) {
      ++ls_.bad_lines;
      return;
    }
    std::uint64_t t = 0;
    for (std::size_t i = 0; i < tab; ++i) {
      if (line[i] < '0' || line[i] > '9') {
        ++ls_.bad_lines;
        return;
      }
      t = t * 10 + static_cast<std::uint64_t>(line[i] - '0');
    }
    if (t > max_t_) return;
    ++ls_.messages;
    const L2Message m = parse_l2(std::string_view(line).substr(tab + 1));
    switch (m.kind) {
      case L2Message::Kind::Bad:
        ++ls_.bad;
        return;
      case L2Message::Kind::Other:
        ++ls_.other;
        return;
      case L2Message::Kind::Snapshot:
        close_segment();
        open_segment(m, t);
        return;
      case L2Message::Kind::Update:
        if (!conv_) {
          ++ls_.dropped_before_snapshot;
          return;
        }
        convert(m, t, false);
        if (conv_->window_exit()) {
          cur_.ended_by_window = true;
          close_segment();
        }
        return;
    }
  }

  std::vector<Segment>& segments() noexcept { return segs_; }
  const LoadStats& stats() const noexcept { return ls_; }

 private:
  void open_segment(const L2Message& m, std::uint64_t t) {
    conv_ = std::make_unique<L2Converter>(cfg_);
    cur_ = Segment{};
    cur_.cfg = cfg_;
    cur_t_ = t;
    pub_ = std::make_unique<sim::ItchPublisher<detail::SegEmit>>("L2REPLAY01", 1, detail::SegEmit{&cur_, &cur_t_}, 1);
    if (!convert(m, t, true)) {
      ++ls_.snapshots_unusable;
      conv_.reset();
      pub_.reset();
      return;
    }
    cur_.base_raw = conv_->base_raw();
    cur_.checkpoints.push_back(model_checkpoint(*conv_, cur_.pkts.size()));
    last_cp_ = cur_.pkts.size();
  }

  bool convert(const L2Message& m, std::uint64_t t, bool snapshot) {
    cur_t_ = t;
    auto emit = [&](const sim::PublicEvent& e) { pub_->on_public(e); };
    bool ok = true;
    if (snapshot) {
      ok = conv_->on_snapshot(m, t, emit);
    } else {
      conv_->on_update(m, t, emit);
    }
    pub_->flush();  // each feed message becomes its own packet (or several, if it is big)
    if (!snapshot && cur_.pkts.size() - last_cp_ >= every_) {
      cur_.checkpoints.push_back(model_checkpoint(*conv_, cur_.pkts.size()));
      last_cp_ = cur_.pkts.size();
    }
    return ok;
  }

  void close_segment() {
    if (!conv_) return;
    cur_.stats = conv_->stats();
    cur_.final_levels[0] = conv_->levels(core::Side::Buy);
    cur_.final_levels[1] = conv_->levels(core::Side::Sell);
    cur_.checkpoints.push_back(model_checkpoint(*conv_, cur_.pkts.size()));
    segs_.push_back(std::move(cur_));
    cur_ = Segment{};
    conv_.reset();
    pub_.reset();
    ++ls_.segments;
  }

  L2Config cfg_;
  std::uint32_t every_;
  std::uint64_t max_t_;
  LoadStats ls_;
  std::vector<Segment> segs_;
  Segment cur_;
  std::uint64_t cur_t_ = 0;
  std::unique_ptr<L2Converter> conv_;
  std::unique_ptr<sim::ItchPublisher<detail::SegEmit>> pub_;
  std::size_t last_cp_ = 0;
};

// ---- the paper venue --------------------------------------------------------------------------------

// Receives the gateway's frames, answers with reports after `rtt_ns`. Accepts,
// replaces, cancels; rejects a post-only order that would cross the replayed
// book. Never fills (see the header comment).
class AckVenue {
 public:
  struct Stats {
    std::uint64_t frames = 0, accepted = 0, replaced = 0, canceled = 0, rejected_cross = 0, rejected_other = 0;
    std::uint64_t unknown_cancel = 0, unknown_replace = 0, malformed = 0;
  };
  AckVenue(std::uint64_t rtt_ns, const book::OrderBook* book) : rtt_(rtt_ns), book_(book) {}

  void set_now(std::uint64_t now) noexcept { now_ = now; }
  void set_book(const book::OrderBook* b) noexcept { book_ = b; }

  void on_frame(const std::uint8_t* d, std::size_t n) {
    ++st_.frames;
    gw::ClientMsg m;
    if (n < 2 || !gw::decode_client(d + 2, n - 2, m)) {
      ++st_.malformed;
      return;
    }
    sim::Report r{};
    r.owner = 0;
    r.ts = now_ + rtt_;
    if (m.kind == gw::kMsgNew) {
      r.side = m.side;
      r.id = m.id;
      r.price = m.price;
      if (crosses(m.side, m.price, m.otype)) {
        ++st_.rejected_cross;
        r.type = sim::ReportType::Rejected;
        r.reason = sim::Reason::WouldCross;
      } else if (live_.count(m.id)) {
        ++st_.rejected_other;
        r.type = sim::ReportType::Rejected;
        r.reason = sim::Reason::Duplicate;
      } else {
        live_[m.id] = Live{m.side, m.price, m.qty};
        ++st_.accepted;
        r.type = sim::ReportType::Accepted;
        r.qty = r.leaves = m.qty;
      }
    } else if (m.kind == gw::kMsgReplace) {
      auto it = live_.find(m.id);
      r.id = m.id;
      if (it == live_.end()) {
        ++st_.unknown_replace;
        r.type = sim::ReportType::ReplaceRejected;
        r.reason = sim::Reason::UnknownOrder;
      } else {
        it->second.price = m.price;
        it->second.leaves = m.qty;
        ++st_.replaced;
        r.type = sim::ReportType::Replaced;
        r.side = it->second.side;
        r.price = m.price;
        r.qty = r.leaves = m.qty;
      }
    } else {  // cancel
      auto it = live_.find(m.id);
      r.id = m.id;
      if (it == live_.end()) {
        ++st_.unknown_cancel;
        r.type = sim::ReportType::CancelRejected;
        r.reason = sim::Reason::UnknownOrder;
      } else {
        ++st_.canceled;
        r.type = sim::ReportType::Canceled;
        r.reason = sim::Reason::UserRequested;
        r.side = it->second.side;
        r.price = it->second.price;
        r.qty = it->second.leaves;
        r.leaves = 0;
        live_.erase(it);
      }
    }
    r.seq = ++report_seq_;
    Due du;
    du.at = r.ts;
    du.len = static_cast<std::uint8_t>(gw::encode_report(du.bytes, ++out_seq_, r));
    q_.push_back(du);
  }

  bool has_due(std::uint64_t upto) const noexcept { return !q_.empty() && q_.front().at <= upto; }
  std::uint64_t front_time() const noexcept { return q_.front().at; }
  // Copies the next report out and removes it.
  std::size_t pop(std::uint8_t* out) {
    const Due d = q_.front();
    q_.pop_front();
    std::memcpy(out, d.bytes, d.len);
    return d.len;
  }
  bool empty() const noexcept { return q_.empty(); }
  const Stats& stats() const noexcept { return st_; }
  std::size_t live() const noexcept { return live_.size(); }

 private:
  struct Live {
    core::Side side;
    core::Price price;
    core::Qty leaves;
  };
  struct Due {
    std::uint64_t at;
    std::uint8_t len;
    std::uint8_t bytes[gw::kMaxFrame];
  };
  bool crosses(core::Side s, core::Price p, sim::OrderType t) const noexcept {
    if (t != sim::OrderType::PostOnly || !book_) return false;
    if (s == core::Side::Buy) {
      const core::Price a = book_->best_ask();
      return a != core::Price::invalid() && p.raw() >= a.raw();
    }
    const core::Price b = book_->best_bid();
    return b != core::Price::invalid() && p.raw() <= b.raw();
  }

  std::uint64_t rtt_, now_ = 0, report_seq_ = 0, out_seq_ = 0;
  const book::OrderBook* book_;
  std::unordered_map<core::OrderId, Live> live_;
  std::deque<Due> q_;
  Stats st_;
};

// ---- running a segment -----------------------------------------------------------------------------------

struct ReplayOptions {
  std::uint64_t venue_rtt_ns = 50'000;  // order out to report back
  bool timed = true;                    // measure each packet's processing time
  std::uint32_t book_slack = 3;         // book capacity = pow2 >= slack * peak live levels
  std::int64_t mm_qty_lots = 5;
  std::size_t arena_bytes = 256u << 20;  // working memory for one replay (fuzzers use less)
};

struct ReplayResult {
  bool ok = false;               // pipeline initialised
  std::uint64_t packets = 0;
  std::uint64_t wire_hash = 0, frames = 0, wire_bytes = 0;
  // the order flow up to the last packet, before the trailing reports are delivered (what a live
  // session compares with: it flattens instead of delivering the tail to the strategy)
  std::uint64_t hash_before_tail = 0, frames_before_tail = 0;
  std::uint64_t checks = 0, check_failures = 0;   // checkpoint comparisons
  bool final_book_matches = false;
  std::uint64_t decisions = 0, feed_losses = 0, pull_sweeps = 0, messages_applied = 0, desyncs = 0, book_errors = 0;
  gw::GatewayStats gw{};
  AckVenue::Stats venue{};
  std::uint64_t reports_delivered = 0;
  core::Histogram on_packet_ns;       // every packet
  core::Histogram on_packet_order_ns; // packets that made the strategy send something
  std::uint64_t order_packets = 0, total_ns = 0;
  std::uint32_t book_capacity = 0;
};

namespace detail {
struct HashSend {
  std::uint64_t* hash;
  std::uint64_t* frames;
  std::uint64_t* bytes;
  AckVenue* venue;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    constexpr std::uint64_t kPrime = 0x100000001B3ULL;
    std::uint64_t h = *hash ^ n;
    h *= kPrime;
    for (std::size_t i = 0; i < n; ++i) {
      h ^= d[i];
      h *= kPrime;
    }
    *hash = h;
    ++*frames;
    *bytes += n;
    if (venue) venue->on_frame(d, n);
  }
};

inline std::uint32_t pow2_at_least(std::uint64_t n) {
  std::uint32_t c = 1024;
  while (c < n) c <<= 1;
  return c;
}

// Compares the pipeline's book with a checkpoint of the model.
inline bool matches(const book::OrderBook& b, const Checkpoint& k) {
  const core::Price bb = b.best_bid(), ba = b.best_ask();
  const std::int64_t bpx = bb == core::Price::invalid() ? -1 : bb.raw();
  const std::int64_t apx = ba == core::Price::invalid() ? -1 : ba.raw();
  if (bpx != k.bid_px || apx != k.ask_px) return false;
  if (b.best_bid_qty() != k.bid_qty || b.best_ask_qty() != k.ask_qty) return false;
  if (b.depth(core::Side::Buy, kDepthCheck) != k.bid_depth || b.depth(core::Side::Sell, kDepthCheck) != k.ask_depth)
    return false;
  return b.order_count() == k.live_levels;
}
}  // namespace detail

inline ReplayResult replay_segment(const Segment& seg, const ReplayOptions& opt, const core::TscCalibration* cal) {
  ReplayResult R;
  R.book_capacity = detail::pow2_at_least(static_cast<std::uint64_t>(opt.book_slack) * seg.stats.peak_levels);
  t2t::PipelineConfig cfg;
  cfg.md.locate = 1;
  cfg.book.base = core::Price::from_raw(seg.base_raw);
  cfg.book.tick_raw = seg.cfg.tick_raw;
  cfg.book.num_levels = seg.cfg.num_levels;
  cfg.book.max_orders = R.book_capacity;
  cfg.mm.qty = opt.mm_qty_lots;
  cfg.first_seq = 1;
  cfg.live_at_start = true;

  core::Region region = core::Region::create(opt.arena_bytes, core::RegionOptions{false, false, true, -1});
  core::Arena arena(region);

  std::uint64_t hash = 0xCBF29CE484222325ULL, frames = 0, bytes = 0;
  AckVenue venue2(opt.venue_rtt_ns, nullptr);
  using Pipe = t2t::Pipeline<detail::HashSend>;
  auto* p = new Pipe(detail::HashSend{&hash, &frames, &bytes, &venue2}, cfg.mm);
  if (!p->init(arena, cfg)) {
    delete p;
    return R;
  }
  venue2.set_book(&p->book());
  R.ok = true;

  std::size_t next_cp = 0;
  std::uint8_t rep[gw::kMaxFrame];
  // overhead of one timer pair, subtracted from every measurement
  std::uint64_t overhead = 0;
  if (opt.timed && cal) {
    std::uint64_t best = ~0ULL;
    for (int i = 0; i < 1000; ++i) {
      const std::uint64_t a = core::rdtsc_ordered();
      const std::uint64_t b = core::rdtsc_ordered();
      if (b - a < best) best = b - a;
    }
    overhead = best;
  }

  auto deliver_due = [&](std::uint64_t upto) {
    while (venue2.has_due(upto)) {
      const std::uint64_t at = venue2.front_time();
      const std::size_t n = venue2.pop(rep);
      venue2.set_now(at);
      p->on_report(rep, n, at);
      ++R.reports_delivered;
    }
  };

  for (std::size_t i = 0; i < seg.pkts.size(); ++i) {
    const Packet& pk = seg.pkts[i];
    deliver_due(pk.t);
    venue2.set_now(pk.t);
    const std::uint64_t f0 = frames;
    if (opt.timed && cal) {
      const std::uint64_t t0 = core::rdtsc_ordered();
      p->on_packet(md::Line::A, seg.blob.data() + pk.off, pk.len, pk.t);
      const std::uint64_t t1 = core::rdtsc_ordered();
      const std::uint64_t d = t1 - t0 > overhead ? t1 - t0 - overhead : 0;
      const auto ns = static_cast<std::uint64_t>(cal->to_ns(d));
      R.on_packet_ns.record(ns);
      R.total_ns += ns;
      if (frames != f0) {
        R.on_packet_order_ns.record(ns);
        ++R.order_packets;
      }
    } else {
      p->on_packet(md::Line::A, seg.blob.data() + pk.off, pk.len, pk.t);
      if (frames != f0) ++R.order_packets;
    }
    ++R.packets;
    while (next_cp < seg.checkpoints.size() && seg.checkpoints[next_cp].packets <= R.packets) {
      if (seg.checkpoints[next_cp].packets == R.packets) {
        ++R.checks;
        if (!detail::matches(p->book(), seg.checkpoints[next_cp])) ++R.check_failures;
      }
      ++next_cp;
    }
  }
  R.hash_before_tail = hash;
  R.frames_before_tail = frames;
  if (!seg.pkts.empty()) deliver_due(seg.pkts.back().t + 10 * opt.venue_rtt_ns);

  // final comparison against the model, level by level
  bool ok = p->book().order_count() == seg.final_levels[0].size() + seg.final_levels[1].size();
  for (int s = 0; s < 2 && ok; ++s) {
    const core::Side side = s == 0 ? core::Side::Buy : core::Side::Sell;
    for (const auto& kv : seg.final_levels[s]) {
      if (p->book().level_qty(side, core::Price::from_raw(kv.first * seg.cfg.tick_raw)) != kv.second) {
        ok = false;
        break;
      }
    }
  }
  R.final_book_matches = ok;
  R.wire_hash = hash;
  R.frames = frames;
  R.wire_bytes = bytes;
  R.decisions = p->stats().decisions;
  R.feed_losses = p->stats().feed_losses;
  R.pull_sweeps = p->stats().pull_sweeps;
  R.messages_applied = p->feed().stats().messages;
  R.desyncs = p->feed().stats().desyncs;
  R.book_errors = p->feed().stats().book_errors;
  R.gw = p->gateway().stats();
  R.venue = venue2.stats();
  delete p;
  return R;
}

// ---- check against the exchange's own snapshot -------------------------------------------------------

struct VerifyResult {
  bool usable = false;
  std::uint64_t compared = 0, equal = 0, size_differs = 0, missing_in_model = 0;
  std::uint64_t outside_window = 0;  // among the nearest levels, but beyond the grid: the replay drops them, so they are not compared
  // The segment ended (price left the window) well before the exchange snapshot was taken: the replayed book is the book of
  // that earlier moment, so comparing the two would say nothing. Nothing is compared; `usable` stays false.
  bool stale = false;
  std::uint64_t stale_gap_ns = 0;  // snapshot time minus the segment's last packet
};

// How long after the end of its segment an exchange snapshot may be and still count as the same moment (the two connections
// already differ by a second or two).
constexpr std::uint64_t kVerifyMaxGapNs = 2'000'000'000ULL;

// Compares the n levels nearest the touch on each side of a fresh exchange
// snapshot with the reconstructed book at the end of the segment. The snapshot
// is taken a moment after the last replayed message, so a few levels can have
// legitimately moved; a wrong reading of the update messages would show as a
// large mismatch instead.
//
// A level beyond the segment's price window is dropped by the converter by
// design, so it can never be in the reconstructed book; it is counted in
// `outside_window` and left out of `compared`. (A sparse book such as the
// Coinbase sandbox's has most of its nearest levels far from the touch, and
// counting them as missing hid the result.)
inline VerifyResult verify_against_snapshot(const Segment& seg, const L2Message& snap, int top_n, std::uint64_t snap_t_ns = 0) {
  VerifyResult v;
  if (snap.kind != L2Message::Kind::Snapshot) return v;
  if (seg.ended_by_window && !seg.pkts.empty() && snap_t_ns > seg.pkts.back().t &&
      snap_t_ns - seg.pkts.back().t > kVerifyMaxGapNs) {
    v.stale = true;
    v.stale_gap_ns = snap_t_ns - seg.pkts.back().t;
    return v;
  }
  v.usable = true;
  for (int s = 0; s < 2; ++s) {
    const core::Side side = s == 0 ? core::Side::Buy : core::Side::Sell;
    std::map<std::int64_t, std::int64_t> theirs;
    for (const L2Change& c : snap.changes) {
      if (c.side != side || c.lots <= 0 || c.price_raw % seg.cfg.tick_raw != 0) continue;
      theirs[c.price_raw / seg.cfg.tick_raw] = c.lots;
    }
    int taken = 0;
    const std::int64_t base_idx = seg.base_raw / seg.cfg.tick_raw;
    auto visit = [&](std::int64_t idx, std::int64_t lots) {
      ++taken;
      if (idx < base_idx || idx >= base_idx + static_cast<std::int64_t>(seg.cfg.num_levels)) {
        ++v.outside_window;
        return;
      }
      ++v.compared;
      const auto it = seg.final_levels[s].find(idx);
      if (it == seg.final_levels[s].end()) ++v.missing_in_model;
      else if (it->second == lots) ++v.equal;
      else ++v.size_differs;
    };
    if (side == core::Side::Buy) {
      for (auto it = theirs.rbegin(); it != theirs.rend() && taken < top_n; ++it) visit(it->first, it->second);
    } else {
      for (auto it = theirs.begin(); it != theirs.end() && taken < top_n; ++it) visit(it->first, it->second);
    }
  }
  return v;
}

}  // namespace hft::feed
