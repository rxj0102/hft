#pragma once
// Generates a valid exchange feed: a stream of order-level operations (always
// legal, as a real exchange's would be) plus the encoders to put them on the
// wire. It keeps its own small model of live orders to choose valid targets,
// and can replay the same operations onto the RefBook, which is the ground
// truth the handler's book is compared against.

#include <cstdint>
#include <vector>

#include "event_gen.hpp"
#include "hft/md/itch_encoder.hpp"
#include "reference_book.hpp"

namespace testsupport {

struct Op {
  enum Type : std::uint8_t { Add, Exec, PartialCancel, Delete, Replace } type;
  Side side;
  std::uint64_t id;
  std::uint64_t id2;        // new id for Replace
  std::int64_t price_raw;   // Add / Replace
  std::int64_t qty;         // shares (Add, Replace), quantity (Exec, PartialCancel)
};

struct FeedConfig {
  Price base;
  std::int64_t tick_raw;
  std::uint32_t num_levels;
  std::uint32_t target_live;
  std::uint16_t locate;
};

class FeedGen {
 public:
  FeedGen(const FeedConfig& c, std::uint64_t seed) : c_(c), rng_(seed), mid_(c.num_levels / 2) {}

  // Picks the next operation and commits it to the internal live-order model.
  Op next() {
    drift_mid();
    // The mix must be net-removing above the target and net-adding below it, or
    // the live count drifts without bound on long runs (an earlier version
    // added 35% vs removed ~31% above target and overran the book capacity).
    //   below target: add 55, delete 15, exec 12, partial 6, replace 12
    //   at/above:     add 28, delete 30, exec 12, partial 8, replace 22
    // Removals come from delete (always) and exec (about half remove the order).
    const bool low = live_.size() < c_.target_live;
    const std::uint32_t r = rng_.below(100);
    if (live_.empty() || r < (low ? 55u : 28u)) return do_add();
    if (r < (low ? 70u : 58u)) return do_delete();
    if (r < (low ? 82u : 70u)) return do_exec();
    if (r < (low ? 88u : 78u)) return do_partial_cancel();
    return do_replace();
  }

  std::size_t live() const { return live_.size(); }

  // Encodes the op's message body into `out` and returns its length.
  static std::size_t encode(const Op& op, std::uint16_t locate, std::uint64_t ts, std::uint8_t* out) {
    using namespace hft::md;
    switch (op.type) {
      case Op::Add:
        return encode_add(out, locate, ts, op.id, op.side, static_cast<std::uint32_t>(op.qty),
                          static_cast<std::uint32_t>(op.price_raw));
      case Op::Exec:
        return encode_exec(out, locate, ts, op.id, static_cast<std::uint32_t>(op.qty));
      case Op::PartialCancel:
        return encode_partial_cancel(out, locate, ts, op.id, static_cast<std::uint32_t>(op.qty));
      case Op::Delete:
        return encode_delete(out, locate, ts, op.id);
      case Op::Replace:
        return encode_replace(out, locate, ts, op.id, op.id2, static_cast<std::uint32_t>(op.qty),
                              static_cast<std::uint32_t>(op.price_raw));
    }
    return 0;
  }

  // Applies the op to the ground-truth book with the same semantics a feed
  // consumer must implement. Returns false if the reference rejected it, which
  // would mean the generator produced an illegal op.
  static bool apply_to_ref(RefBook& ref, const Op& op) {
    switch (op.type) {
      case Op::Add:
        return ref.add(op.id, op.side, Price::from_raw(op.price_raw), op.qty) == BookResult::Ok;
      case Op::Exec:
      case Op::PartialCancel:
        return ref.execute(op.id, op.qty) == BookResult::Ok;
      case Op::Delete:
        return ref.cancel(op.id) == BookResult::Ok;
      case Op::Replace:
        if (ref.cancel(op.id) != BookResult::Ok) return false;
        return ref.add(op.id2, op.side, Price::from_raw(op.price_raw), op.qty) == BookResult::Ok;
    }
    return false;
  }

 private:
  struct Live {
    std::uint64_t id;
    std::int64_t qty;
    Side side;
  };

  void drift_mid() {
    const std::uint32_t r = rng_.below(100);
    const std::uint32_t lo = c_.num_levels / 4, hi = c_.num_levels * 3 / 4;
    if (r < 4 && mid_ > lo) --mid_;
    else if (r < 8 && mid_ < hi) ++mid_;
  }

  std::int64_t pick_price(Side side) {
    std::uint32_t lvl;
    if (rng_.below(100) < 3) {
      lvl = rng_.below(c_.num_levels);
    } else {
      std::uint32_t depth = static_cast<std::uint32_t>(std::countr_zero(rng_.next() | (1ULL << 40)));
      if (depth > 40) depth = 40;
      lvl = side == Side::Buy ? mid_ - 1 - depth : mid_ + 1 + depth;
    }
    return c_.base.raw() + static_cast<std::int64_t>(lvl) * c_.tick_raw;
  }

  Op do_add() {
    const Side side = rng_.below(2) ? Side::Buy : Side::Sell;
    Op op{Op::Add, side, next_id_++, 0, pick_price(side), static_cast<std::int64_t>(1 + rng_.below(1000))};
    live_.push_back(Live{op.id, op.qty, side});
    return op;
  }

  std::size_t pick() { return rng_.below(static_cast<std::uint32_t>(live_.size())); }

  Op do_delete() {
    const std::size_t i = pick();
    Op op{Op::Delete, live_[i].side, live_[i].id, 0, 0, 0};
    remove(i);
    return op;
  }

  Op do_exec() {
    const std::size_t i = pick();
    Live& l = live_[i];
    std::int64_t q = l.qty;
    if (l.qty > 1 && rng_.below(2)) q = 1 + static_cast<std::int64_t>(rng_.below(static_cast<std::uint32_t>(l.qty - 1)));
    Op op{Op::Exec, l.side, l.id, 0, 0, q};
    if (q == l.qty) remove(i);
    else l.qty -= q;
    return op;
  }

  Op do_partial_cancel() {
    const std::size_t i = pick();
    Live& l = live_[i];
    if (l.qty <= 1) return do_delete();
    const std::int64_t q = 1 + static_cast<std::int64_t>(rng_.below(static_cast<std::uint32_t>(l.qty - 1)));
    Op op{Op::PartialCancel, l.side, l.id, 0, 0, q};
    l.qty -= q;
    return op;
  }

  Op do_replace() {
    const std::size_t i = pick();
    Live& l = live_[i];
    Op op{Op::Replace, l.side, l.id, next_id_++, pick_price(l.side), static_cast<std::int64_t>(1 + rng_.below(1000))};
    l.id = op.id2;
    l.qty = op.qty;
    return op;
  }

  void remove(std::size_t i) {
    live_[i] = live_.back();
    live_.pop_back();
  }

  FeedConfig c_;
  Rng rng_;
  std::uint32_t mid_;
  std::uint64_t next_id_{1};
  std::vector<Live> live_;
};

}  // namespace testsupport
