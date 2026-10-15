// Compile-time enforcement of the hot-path rules. This translation unit is built
// with -fno-exceptions -fno-rtti (see hft_hotpath in the root CMakeLists). If a
// hot-path header starts using exceptions, RTTI, dynamic_cast or typeid, this
// file stops compiling and CI fails, which is cheaper than finding it in a profile.
#include "hft/book/order_book.hpp"

namespace hft::book {

// Touches every public member so all inline code is actually compiled here.
BookResult book_hotpath_touch(OrderBook& b) noexcept {
  const Price p = Price::from_units(100, 100);
  BookResult r = b.add(1, Side::Buy, p, 10);
  r = b.replace(1, p, 5);
  r = b.execute(1, 1);
  (void)b.best_bid();
  (void)b.best_ask();
  (void)b.best_bid_qty();
  (void)b.best_ask_qty();
  (void)b.level_qty(Side::Buy, p);
  (void)b.contains(1);
  (void)b.digest();
  r = b.cancel(1);
  return r;
}

}  // namespace hft::book
