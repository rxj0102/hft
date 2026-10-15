#pragma once
// Market data handler for one instrument: turns sequenced packets from two
// redundant lines (A and B) into an in-order stream of book updates, and
// recovers from loss using a snapshot channel.
//
// Arbitration. The same packet normally arrives on both lines. The first copy
// of each sequence number wins; later copies are dropped as duplicates. A
// packet that arrives ahead of the next expected sequence number is parked in
// a small fixed store. If the other line delivers the missing packet shortly
// after, the parked packets drain in order and nothing was lost. If it does
// not arrive within gap_timeout_ns, the gap is declared real.
//
// Recovery. A declared gap (or any inconsistency the book detects, such as an
// execute for an unknown order) puts the handler in NeedSnapshot: the book is
// no longer trusted and updates are parked instead of applied. When a complete
// snapshot "as of sequence S" arrives, the book is rebuilt from it, the next
// expected sequence becomes S+1, and the parked packets replay on top. Packets
// at or below S are discarded as already included in the snapshot.
//
// Time is passed in by the caller (now_ns), never read from a clock here, so a
// recorded session replays bit-identically.
//
// Parked-packet policy when the store is full: evict the lowest sequence
// number. During a long outage the packets right after the (recent) snapshot
// point are the ones worth keeping, not the oldest.

#include <cstdint>
#include <cstring>

#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "hft/md/itch_wire.hpp"

namespace hft::md {

enum class Line : std::uint8_t { A = 0, B = 1 };
enum class FeedState : std::uint8_t { NeedSnapshot, Live };

struct MdConfig {
  std::uint16_t locate = 0;                // instrument to track; others are skipped
  std::uint32_t pending_slots = 64;        // out-of-order packets that can be parked
  std::uint64_t gap_timeout_ns = 100'000;  // wait for the other line this long
  bool check_session = false;              // otherwise the first packet fixes the session
  std::uint8_t session[kSessionBytes] = {};
};

struct MdStats {
  std::uint64_t packets = 0;             // well-formed incremental packets
  std::uint64_t heartbeats = 0;
  std::uint64_t messages = 0;            // applied to the book
  std::uint64_t malformed = 0;
  std::uint64_t bad_session = 0;
  std::uint64_t dups[2] = {0, 0};        // duplicate packets dropped, per line
  std::uint64_t delivered[2] = {0, 0};   // packets that advanced the stream, per line
  std::uint64_t parked = 0;              // packets stored out of order
  std::uint64_t pending_evicted = 0;
  std::uint64_t pending_dropped = 0;
  std::uint64_t gaps_filled = 0;         // closed by the other line, no snapshot needed
  std::uint64_t gaps_declared = 0;       // timed out, snapshot required
  std::uint64_t desyncs = 0;             // book rejected a message
  std::uint64_t book_errors = 0;
  std::uint64_t filtered = 0;            // other instruments
  std::uint64_t unknown_types = 0;
  std::uint64_t snapshots_applied = 0;
  std::uint64_t snapshots_rejected = 0;
  std::uint64_t snapshots_ignored = 0;   // arrived while Live
};

struct NullListener {
  void on_event(std::uint64_t /*seq*/, const Decoded& /*d*/) noexcept {}
  void on_state(FeedState /*s*/) noexcept {}
};

template <class Listener = NullListener>
class MdHandler {
 public:
  // Startup only. Parked-packet storage comes from `arena`.
  bool init(core::Arena& arena, book::OrderBook& book, const MdConfig& cfg, Listener l = Listener{}) noexcept {
    book_ = &book;
    cfg_ = cfg;
    listener_ = l;
    if (cfg.pending_slots == 0) return false;
    pending_ = arena.allocate_array<Pending>(cfg.pending_slots);
    if (!pending_) return false;
    for (std::uint32_t i = 0; i < cfg.pending_slots; ++i) pending_[i].used = false;
    state_ = FeedState::NeedSnapshot;
    return true;
  }

  // Start tracking without a snapshot (e.g. replaying a full session from
  // sequence 1). The book must be empty or already consistent with next_seq-1.
  void begin_live(std::uint64_t next_seq) noexcept {
    state_ = FeedState::Live;
    next_seq_ = next_seq;
    gap_active_ = false;
    for (std::uint32_t i = 0; i < cfg_.pending_slots; ++i) pending_[i].used = false;
  }

  void on_packet(Line line, const std::uint8_t* data, std::size_t len, std::uint64_t now_ns) noexcept {
    PacketView pv;
    if (!parse_packet(data, len, pv)) {
      ++stats_.malformed;
      return;
    }
    if (!session_ok(pv)) {
      ++stats_.bad_session;
      return;
    }
    const int li = static_cast<int>(line);
    if (pv.count == 0) {
      ++stats_.heartbeats;
      // A heartbeat announces the next sequence the sender will use. Ahead of
      // us means we lost the tail of the stream and no later packet will reveal it.
      if (state_ == FeedState::Live && pv.seq > next_seq_) note_gap(now_ns);
      return;
    }
    ++stats_.packets;

    if (state_ == FeedState::NeedSnapshot) {
      park(pv, li);
      return;
    }
    if (pv.end() <= next_seq_) {
      ++stats_.dups[li];
      return;
    }
    if (pv.seq <= next_seq_) {
      ++stats_.delivered[li];
      if (!deliver(pv, next_seq_ - pv.seq)) {
        park(pv, li);  // desynced mid-packet: keep it for replay after the snapshot
        return;
      }
      drain_pending(now_ns);
      return;
    }
    // Ahead of the expected sequence: wait for the other line.
    park(pv, li);
    note_gap(now_ns);
  }

  void on_snapshot_packet(const std::uint8_t* data, std::size_t len, std::uint64_t now_ns) noexcept {
    PacketView pv;
    if (!parse_packet(data, len, pv)) {
      ++stats_.malformed;
      return;
    }
    if (state_ == FeedState::Live) {
      ++stats_.snapshots_ignored;
      return;
    }
    MsgCursor cur(pv);
    const std::uint8_t* body;
    std::size_t blen;
    std::uint64_t seq = pv.seq;
    for (; cur.next(body, blen); ++seq) {
      const std::uint8_t type = body[0];
      if (type == kSnapBegin) {
        Decoded d;
        decode(body, d);
        book_->clear();
        snap_active_ = true;
        snap_seq_ = d.id;
        snap_total_ = static_cast<std::uint32_t>(d.qty);
        snap_applied_ = 0;
        snap_expect_ = seq + 1;
        continue;
      }
      if (!snap_active_) continue;  // joined mid-cycle: wait for the next Begin
      if (seq != snap_expect_) {    // lost part of the snapshot
        abort_snapshot();
        continue;
      }
      snap_expect_ = seq + 1;
      if (type == kAdd) {
        Decoded d;
        decode(body, d);
        if (d.locate != cfg_.locate) continue;
        if (book_->add(d.id, d.side, d.price, d.qty) != book::BookResult::Ok) {
          abort_snapshot();
          continue;
        }
        ++snap_applied_;
      } else if (type == kSnapEnd) {
        Decoded d;
        decode(body, d);
        const bool good = d.id == snap_seq_ && snap_applied_ == snap_total_ && snap_seq_ != ~std::uint64_t{0};
        snap_active_ = false;
        if (good) finalize_snapshot(now_ns);
        else ++stats_.snapshots_rejected;
      }
    }
  }

  // Call regularly (and at least once per gap_timeout_ns) so a stalled gap is
  // eventually declared.
  void poll(std::uint64_t now_ns) noexcept {
    // now_ns >= gap_since_ guards against a caller clock that steps backwards,
    // which would otherwise underflow and declare a gap instantly.
    if (state_ == FeedState::Live && gap_active_ && now_ns >= gap_since_ &&
        now_ns - gap_since_ >= cfg_.gap_timeout_ns) {
      ++stats_.gaps_declared;
      enter_need_snapshot();
    }
  }

  FeedState state() const noexcept { return state_; }
  bool book_valid() const noexcept { return state_ == FeedState::Live; }
  bool gap_pending() const noexcept { return gap_active_; }
  std::uint64_t next_seq() const noexcept { return next_seq_; }
  const MdStats& stats() const noexcept { return stats_; }
  Listener& listener() noexcept { return listener_; }

 private:
  struct Pending {
    std::uint64_t first_seq;
    std::uint16_t count;
    std::uint16_t len;
    bool used;
    std::uint8_t data[kMaxPacketBytes];
  };

  bool session_ok(const PacketView& pv) noexcept {
    if (cfg_.check_session) return std::memcmp(pv.session, cfg_.session, kSessionBytes) == 0;
    if (!session_set_) {
      std::memcpy(session_, pv.session, kSessionBytes);
      session_set_ = true;
      return true;
    }
    return std::memcmp(pv.session, session_, kSessionBytes) == 0;
  }

  void note_gap(std::uint64_t now_ns) noexcept {
    if (!gap_active_) {
      gap_active_ = true;
      gap_since_ = now_ns;
    }
  }

  void enter_need_snapshot() noexcept {
    state_ = FeedState::NeedSnapshot;
    gap_active_ = false;
    listener_.on_state(state_);
  }

  // Applies the messages of an in-order packet, skipping the first `skip`
  // that were already applied. Returns false if the book rejected a message
  // (the handler is then in NeedSnapshot and the packet is incomplete).
  bool deliver(const PacketView& pv, std::uint64_t skip) noexcept {
    MsgCursor cur(pv);
    const std::uint8_t* body;
    std::size_t blen;
    std::uint64_t idx = 0;
    while (cur.next(body, blen)) {
      if (idx++ < skip) continue;
      const std::uint64_t seq = pv.seq + (idx - 1);
      if (!apply(seq, body)) return false;
      next_seq_ = seq + 1;
    }
    return true;
  }

  bool apply(std::uint64_t seq, const std::uint8_t* body) noexcept {
    Decoded d;
    if (!decode(body, d)) {
      ++stats_.unknown_types;
      return true;  // forward compatible: skip, but the sequence number is consumed
    }
    if (d.type == kSnapBegin || d.type == kSnapEnd) {
      ++stats_.unknown_types;  // snapshot-channel messages are meaningless here
      return true;
    }
    if (d.locate != cfg_.locate) {
      ++stats_.filtered;
      return true;
    }
    book::BookResult r = book::BookResult::Ok;
    switch (d.type) {
      case kAdd:
        r = book_->add(d.id, d.side, d.price, d.qty);
        break;
      case kExec:
      case kPartialCancel:
        r = book_->execute(d.id, d.qty);
        break;
      case kDelete:
        r = book_->cancel(d.id);
        break;
      case kReplace: {
        // The feed replace gives the order a new id and sends it to the back of
        // the queue: exactly cancel + add on the same side.
        core::Side s = core::Side::Buy;
        if (!book_->side_of(d.id, s)) {
          r = book::BookResult::NotFound;
        } else if (d.id2 != d.id && book_->contains(d.id2)) {
          r = book::BookResult::Duplicate;  // checked first so the original is not lost
        } else {
          book_->cancel(d.id);
          r = book_->add(d.id2, s, d.price, d.qty);
        }
        break;
      }
      default:
        break;
    }
    if (r != book::BookResult::Ok) {
      ++stats_.book_errors;
      ++stats_.desyncs;
      enter_need_snapshot();
      return false;
    }
    ++stats_.messages;
    listener_.on_event(seq, d);
    return true;
  }

  void park(const PacketView& pv, int li) noexcept {
    Pending* free_slot = nullptr;
    Pending* lowest = nullptr;
    for (std::uint32_t i = 0; i < cfg_.pending_slots; ++i) {
      Pending& p = pending_[i];
      if (!p.used) {
        if (!free_slot) free_slot = &p;
        continue;
      }
      if (p.first_seq == pv.seq && p.count == pv.count) {  // same packet from the other line
        ++stats_.dups[li];
        return;
      }
      if (!lowest || p.first_seq < lowest->first_seq) lowest = &p;
    }
    Pending* dst = free_slot;
    if (!dst) {
      if (lowest && pv.seq > lowest->first_seq) {
        ++stats_.pending_evicted;
        dst = lowest;
      } else {
        ++stats_.pending_dropped;
        return;
      }
    }
    dst->first_seq = pv.seq;
    dst->count = pv.count;
    dst->len = static_cast<std::uint16_t>(pv.len);
    dst->used = true;
    std::memcpy(dst->data, pv.data, pv.len);
    ++stats_.parked;
  }

  // Delivers parked packets that have become next in line, frees stale ones.
  void drain_pending(std::uint64_t now_ns) noexcept {
    bool progressed = false;
    for (;;) {
      bool moved = false;
      for (std::uint32_t i = 0; i < cfg_.pending_slots; ++i) {
        Pending& p = pending_[i];
        if (!p.used) continue;
        const std::uint64_t end = p.first_seq + p.count;
        if (end <= next_seq_) {
          p.used = false;  // already covered
          continue;
        }
        if (p.first_seq > next_seq_) continue;  // still ahead of us
        PacketView pv;
        pv.data = p.data;
        pv.len = p.len;
        pv.session = p.data;
        pv.seq = p.first_seq;
        pv.count = p.count;
        if (!deliver(pv, next_seq_ - p.first_seq)) return;  // desynced; slot kept for replay
        p.used = false;
        moved = progressed = true;
      }
      if (!moved) break;
    }
    bool any = false;
    for (std::uint32_t i = 0; i < cfg_.pending_slots; ++i) any = any || pending_[i].used;
    if (any) {
      // Start the clock when a gap newly opens, and restart it when progress
      // shrank the gap (the remaining hole is a different one now).
      if (!gap_active_ || progressed) gap_since_ = now_ns;
      gap_active_ = true;
    } else if (gap_active_) {
      gap_active_ = false;
      ++stats_.gaps_filled;
    }
  }

  void abort_snapshot() noexcept {
    snap_active_ = false;
    ++stats_.snapshots_rejected;
  }

  void finalize_snapshot(std::uint64_t now_ns) noexcept {
    next_seq_ = snap_seq_ + 1;
    state_ = FeedState::Live;
    gap_active_ = false;
    ++stats_.snapshots_applied;
    listener_.on_state(state_);
    drain_pending(now_ns);
  }

  book::OrderBook* book_{nullptr};
  MdConfig cfg_{};
  Listener listener_{};
  Pending* pending_{nullptr};
  FeedState state_{FeedState::NeedSnapshot};
  std::uint64_t next_seq_{0};
  bool gap_active_{false};
  std::uint64_t gap_since_{0};
  bool session_set_{false};
  std::uint8_t session_[kSessionBytes]{};
  bool snap_active_{false};
  std::uint64_t snap_seq_{0}, snap_expect_{0};
  std::uint32_t snap_total_{0}, snap_applied_{0};
  MdStats stats_{};
};

}  // namespace hft::md
