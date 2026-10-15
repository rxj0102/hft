#pragma once
// Seeded random order flow for the matching engine: new orders of every type,
// cancels and replaces aimed at previously issued ids (live or dead), several
// owners so self-trade prevention fires, and a few percent of deliberately
// invalid requests. Prices hover around a drifting mid so orders cross often.

#include <cstdint>
#include <vector>

#include "event_gen.hpp"
#include "hft/sim/types.hpp"

namespace testsupport {

struct Op {
  enum Kind : std::uint8_t { New, Cancel, Replace } kind;
  hft::sim::NewOrder o;  // New
  hft::sim::OrderId id;  // Cancel / Replace
  hft::sim::Owner owner;
  hft::sim::Price price;  // Replace
  hft::sim::Qty qty;      // Replace
};

struct FlowConfig {
  hft::sim::Price base;
  std::int64_t tick_raw;
  std::uint32_t num_levels;
  std::uint16_t owners;      // owners are 1..owners
  bool inject_invalid;
  std::uint32_t big_qty_pct = 10;
};

class FlowGen {
 public:
  FlowGen(const FlowConfig& c, std::uint64_t seed) : c_(c), rng_(seed), mid_(c.num_levels / 2) {
    hist_.resize(kHist);
  }

  Op next() {
    drift();
    const std::uint32_t r = rng_.below(100);
    if (r < 72 || issued_ == 0) return make_new();
    if (r < 88) return make_cancel();
    return make_replace();
  }

 private:
  using Price = hft::sim::Price;
  using Side = hft::sim::Side;
  using OrderType = hft::sim::OrderType;
  using Owner = hft::sim::Owner;
  using OrderId = hft::sim::OrderId;
  using Qty = hft::sim::Qty;
  static constexpr std::size_t kHist = 2048;

  struct H {
    OrderId id;
    Owner owner;
  };

  void drift() {
    const std::uint32_t r = rng_.below(100);
    const std::uint32_t lo = c_.num_levels / 4, hi = c_.num_levels * 3 / 4;
    if (r < 5 && mid_ > lo) --mid_;
    else if (r < 10 && mid_ < hi) ++mid_;
  }

  Price price_near() {
    std::int64_t l = static_cast<std::int64_t>(mid_) + static_cast<std::int64_t>(rng_.below(13)) - 6;
    if (l < 0) l = 0;
    if (l >= static_cast<std::int64_t>(c_.num_levels)) l = static_cast<std::int64_t>(c_.num_levels) - 1;
    return Price::from_raw(c_.base.raw() + l * c_.tick_raw);
  }

  Owner pick_owner() { return static_cast<Owner>(1 + rng_.below(c_.owners)); }

  Qty pick_qty() {
    return rng_.below(100) < c_.big_qty_pct ? 1 + static_cast<Qty>(rng_.below(200)) : 1 + static_cast<Qty>(rng_.below(20));
  }

  OrderType pick_type() {
    const std::uint32_t r = rng_.below(100);
    if (r < 8) return OrderType::Market;
    if (r < 20) return OrderType::IOC;
    if (r < 30) return OrderType::FOK;
    if (r < 45) return OrderType::PostOnly;
    return OrderType::Limit;
  }

  void remember(OrderId id, Owner o) {
    hist_[issued_ % kHist] = H{id, o};
    ++issued_;
  }

  H pick_hist() {
    const std::uint64_t n = issued_ < kHist ? issued_ : kHist;
    return hist_[rng_.below(static_cast<std::uint32_t>(n))];
  }

  Op make_new() {
    Op op{};
    op.kind = Op::New;
    op.o.id = next_id_++;
    op.o.owner = pick_owner();
    op.o.side = rng_.below(2) ? Side::Buy : Side::Sell;
    op.o.type = pick_type();
    op.o.price = price_near();
    op.o.qty = pick_qty();
    if (c_.inject_invalid && rng_.below(100) < 3) {
      switch (rng_.below(6)) {
        case 0: op.o.id = 0; break;
        case 1: op.o.qty = 0; break;
        case 2: op.o.qty = -static_cast<Qty>(1 + rng_.below(5)); break;
        case 3: op.o.price = Price::from_raw(op.o.price.raw() + 1); break;  // off tick
        case 4: op.o.price = Price::from_raw(c_.base.raw() + static_cast<std::int64_t>(c_.num_levels) * c_.tick_raw); break;
        default:
          if (issued_ > 0) op.o.id = pick_hist().id;  // maybe a live id: duplicate
          break;
      }
    }
    remember(op.o.id, op.o.owner);
    return op;
  }

  Op make_cancel() {
    Op op{};
    op.kind = Op::Cancel;
    const H h = pick_hist();
    op.id = h.id;
    op.owner = h.owner;
    const std::uint32_t r = rng_.below(100);
    if (c_.inject_invalid && r < 10) op.owner = pick_owner();               // maybe the wrong owner
    else if (c_.inject_invalid && r < 13) op.id = 1'000'000'000ULL + rng_.below(1000);  // never issued
    return op;
  }

  Op make_replace() {
    Op op{};
    op.kind = Op::Replace;
    const H h = pick_hist();
    op.id = h.id;
    op.owner = h.owner;
    op.price = price_near();
    op.qty = pick_qty();
    const std::uint32_t r = rng_.below(100);
    if (c_.inject_invalid) {
      if (r < 5) op.qty = 0;
      else if (r < 8) op.price = Price::from_raw(op.price.raw() + 1);
      else if (r < 14) op.owner = pick_owner();
    }
    return op;
  }

  FlowConfig c_;
  Rng rng_;
  std::uint32_t mid_;
  OrderId next_id_{1};
  std::uint64_t issued_{0};
  std::vector<H> hist_;
};

}  // namespace testsupport
