#pragma once
// Seeded random event generator shared by the property tests and the book
// benchmark. Deterministic for a given seed, so any failure reproduces.
//
// The mix is meant to resemble a feed: most adds land near a drifting mid, a
// few land anywhere in the window (so best-price recompute crosses many empty
// levels), and a small share of events are deliberately invalid to exercise
// every error path.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hft/book/order_book.hpp"

namespace testsupport {

using hft::book::BookResult;
using hft::core::Price;
using hft::core::Qty;
using hft::core::Side;

struct Event {
  enum Type : std::uint8_t { Add, Cancel, Exec, Replace } type;
  Side side;
  std::uint64_t id;
  Price price;
  Qty qty;
};

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1DULL;
  }
  // Unbiased enough for testing; avoids a modulo on the hot generator path.
  std::uint32_t below(std::uint32_t n) {
    return static_cast<std::uint32_t>(((next() >> 32) * static_cast<std::uint64_t>(n)) >> 32);
  }

 private:
  std::uint64_t s_;
};

struct GenConfig {
  Price base;
  std::int64_t tick_raw;
  std::uint32_t num_levels;
  std::uint32_t target_live;  // steady-state resting orders the mix aims for
  bool inject_invalid;
};

class EventGen {
 public:
  EventGen(const GenConfig& c, std::uint64_t seed) : c_(c), rng_(seed), mid_(c.num_levels / 2) {}

  Event next() {
    drift_mid();
    was_invalid_ = false;
    if (c_.inject_invalid && rng_.below(100) == 0) return invalid_event();
    const bool low = live_.size() < c_.target_live;
    const std::uint32_t r = rng_.below(100);
    if (live_.empty() || r < (low ? 50u : 30u)) return next_add();
    if (r < (low ? 70u : 65u)) return pick_cancel();
    if (r < (low ? 85u : 80u)) return pick_exec();
    return pick_replace();
  }

  Event next_add() {
    was_invalid_ = false;
    const Side side = rng_.below(2) ? Side::Buy : Side::Sell;
    return Event{Event::Add, side, next_id_++, price_at(pick_level(side)),
                 static_cast<Qty>(1 + rng_.below(1000))};
  }

  // Feed the book's actual result back so the generator's view of live orders
  // stays exact (a Full add, for instance, must not become a live order).
  void commit(const Event& e, BookResult r) {
    if (r != BookResult::Ok || was_invalid_) return;
    switch (e.type) {
      case Event::Add:
        live_.push_back(Live{e.id, e.qty, e.side, e.price});
        break;
      case Event::Cancel:
        remove_live(last_idx_);
        break;
      case Event::Exec:
        if (e.qty == live_[last_idx_].qty) remove_live(last_idx_);
        else live_[last_idx_].qty -= e.qty;
        break;
      case Event::Replace:
        live_[last_idx_].qty = e.qty;
        live_[last_idx_].price = e.price;
        break;
    }
  }

  std::size_t live() const { return live_.size(); }

  // Call after a successful book rebase. Live order prices are absolute so they
  // stay valid; only the level-relative mid has to follow the shifted window.
  void on_rebase(Price new_base, std::int64_t delta_ticks) {
    c_.base = new_base;
    std::int64_t m = static_cast<std::int64_t>(mid_) - delta_ticks;
    const std::int64_t lo = c_.num_levels / 4, hi = c_.num_levels * 3 / 4;
    if (m < lo) m = lo;
    if (m > hi) m = hi;
    mid_ = static_cast<std::uint32_t>(m);
  }

 private:
  struct Live {
    std::uint64_t id;
    Qty qty;
    Side side;
    Price price;
  };

  Price price_at(std::uint32_t lvl) const {
    return Price::from_raw(c_.base.raw() + static_cast<std::int64_t>(lvl) * c_.tick_raw);
  }

  void drift_mid() {
    const std::uint32_t r = rng_.below(100);
    const std::uint32_t lo = c_.num_levels / 4, hi = c_.num_levels * 3 / 4;
    if (r < 4 && mid_ > lo) --mid_;
    else if (r < 8 && mid_ < hi) ++mid_;
  }

  std::uint32_t pick_level(Side side) {
    const std::uint32_t r = rng_.below(100);
    if (r < 5) return rng_.below(c_.num_levels);  // anywhere in the window
    if (r < 10) return mid_;                       // at mid: equal/crossing prices
    // Geometric depth from mid: most orders sit close, a tail sits deep.
    std::uint32_t depth = static_cast<std::uint32_t>(std::countr_zero(rng_.next() | (1ULL << 40)));
    if (depth > 40) depth = 40;
    return side == Side::Buy ? mid_ - 1 - depth : mid_ + 1 + depth;
  }

  Event pick_cancel() {
    last_idx_ = rng_.below(static_cast<std::uint32_t>(live_.size()));
    return Event{Event::Cancel, live_[last_idx_].side, live_[last_idx_].id, Price{}, 0};
  }

  Event pick_exec() {
    last_idx_ = rng_.below(static_cast<std::uint32_t>(live_.size()));
    const Live& l = live_[last_idx_];
    Qty q = l.qty;
    if (l.qty > 1 && rng_.below(2)) q = 1 + static_cast<Qty>(rng_.below(static_cast<std::uint32_t>(l.qty - 1)));
    return Event{Event::Exec, l.side, l.id, Price{}, q};
  }

  Event pick_replace() {
    last_idx_ = rng_.below(static_cast<std::uint32_t>(live_.size()));
    const Live& l = live_[last_idx_];
    if (l.qty > 1 && rng_.below(100) < 35) {
      // Same price, smaller size: the path that keeps queue priority.
      return Event{Event::Replace, l.side, l.id, l.price,
                   1 + static_cast<Qty>(rng_.below(static_cast<std::uint32_t>(l.qty - 1)))};
    }
    return Event{Event::Replace, l.side, l.id, price_at(pick_level(l.side)),
                 static_cast<Qty>(1 + rng_.below(1000))};
  }

  Event invalid_event() {
    was_invalid_ = true;
    const bool any = !live_.empty();
    const std::uint32_t k = rng_.below(11);
    const Live l = any ? live_[rng_.below(static_cast<std::uint32_t>(live_.size()))] : Live{0, 1, Side::Buy, Price{}};
    const Price in_win = price_at(mid_);
    switch (k) {
      case 0: return Event{Event::Cancel, Side::Buy, 0x7000000000000000ULL + rng_.below(1000), Price{}, 0};
      case 1: return Event{Event::Add, Side::Buy, 0, in_win, 10};                      // id 0
      case 2: return Event{Event::Add, Side::Sell, next_id_++, in_win, 0};             // qty 0
      case 3: return Event{Event::Add, Side::Buy, next_id_++, price_at(c_.num_levels), 10};  // above window
      case 4: return Event{Event::Add, Side::Buy, next_id_++, Price::from_raw(c_.base.raw() - c_.tick_raw), 10};  // below
      case 5: return Event{Event::Add, Side::Sell, next_id_++, Price::from_raw(in_win.raw() + 1), 10};  // off tick (if tick > 1)
      // Duplicate id; with no live order to duplicate this degrades to id 0, which is also invalid.
      case 6: return Event{Event::Add, l.side, l.id, in_win, 10};
      case 7: return Event{Event::Exec, l.side, l.id ? l.id : 0x7000000000000001ULL, Price{}, l.qty + 1};
      case 8: return Event{Event::Replace, Side::Buy, 0x7000000000000002ULL, in_win, 5};  // unknown
      case 9: return Event{Event::Replace, l.side, l.id ? l.id : 0x7000000000000003ULL, in_win, 0};  // qty 0
      default: return Event{Event::Replace, l.side, l.id ? l.id : 0x7000000000000004ULL, price_at(c_.num_levels + 7), 5};
    }
  }

  void remove_live(std::size_t i) {
    live_[i] = live_.back();
    live_.pop_back();
  }

  GenConfig c_;
  Rng rng_;
  std::uint32_t mid_;
  std::uint64_t next_id_{1};
  std::vector<Live> live_;
  std::size_t last_idx_{0};
  bool was_invalid_{false};
};

template <class Book>
BookResult apply(Book& b, const Event& e) {
  switch (e.type) {
    case Event::Add: return b.add(e.id, e.side, e.price, e.qty);
    case Event::Cancel: return b.cancel(e.id);
    case Event::Exec: return b.execute(e.id, e.qty);
    case Event::Replace: return b.replace(e.id, e.price, e.qty);
  }
  return BookResult::Ok;
}

}  // namespace testsupport
