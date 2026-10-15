#pragma once
// Read-only view of an OrderBook with the price grid arithmetic a strategy
// needs: the touch, depth over the top levels, and tick index <-> price.
// It holds no state of its own beyond the grid, so it costs nothing to keep
// next to the book the market data handler updates.

#include <cstdint>

#include "hft/book/order_book.hpp"
#include "hft/core/fast_div.hpp"

namespace hft::book {

class BookView {
 public:
  void init(const OrderBook* b, const BookConfig& cfg) noexcept {
    book_ = b;
    cfg_ = cfg;
    if (cfg.tick_raw > 0) tick_div_.set(static_cast<std::uint64_t>(cfg.tick_raw));
  }

  Price best_bid() const noexcept { return book_->best_bid(); }
  Price best_ask() const noexcept { return book_->best_ask(); }
  bool two_sided() const noexcept { return best_bid() != Price::invalid() && best_ask() != Price::invalid(); }

  // Total quantity on the n price levels nearest the touch on one side, counted
  // by price distance (an empty level inside the window contributes zero).
  Qty depth(Side s, int n) const noexcept { return book_->depth(s, n); }

  // Tick index of a price on the grid. Prices at or above the base (every price
  // the strategy sees) take the multiply path; anything else falls back to the
  // plain divide, which keeps the old behaviour for out of range input.
  std::int64_t tick_index(Price p) const noexcept {
    const std::int64_t off = p.raw() - cfg_.base.raw();
    if (off < 0) return off / cfg_.tick_raw;
    return static_cast<std::int64_t>(tick_div_.div(static_cast<std::uint64_t>(off)));
  }
  Price price_at(std::int64_t idx) const noexcept { return Price::from_raw(cfg_.base.raw() + idx * cfg_.tick_raw); }

 private:
  const OrderBook* book_{nullptr};
  BookConfig cfg_{};
  core::InvariantDivisor tick_div_{};
};

}  // namespace hft::book
