#pragma once
// Deliberately simple reference order book: std::map of price -> std::list of
// orders. It is slow and obviously correct, and shares no code with the real
// book except the result enum and the digest mixer, so agreement between the
// two is meaningful evidence.

#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>

#include "hft/book/digest.hpp"
#include "hft/book/order_book.hpp"

namespace testsupport {

using hft::book::BookConfig;
using hft::book::BookResult;
using hft::book::digest_combine;
using hft::book::kDigestSeed;
using hft::core::Price;
using hft::core::Qty;
using hft::core::Side;

class RefBook {
 public:
  explicit RefBook(const BookConfig& c) : cfg_(c) {}

  BookResult add(std::uint64_t id, Side side, Price p, Qty qty) {
    if (id == 0) return BookResult::BadId;
    if (qty <= 0) return BookResult::BadQty;
    if (!valid(p)) return BookResult::BadPrice;
    if (where_.count(id)) return BookResult::Duplicate;
    if (where_.size() >= cfg_.max_orders) return BookResult::Full;
    insert(id, side, p.raw(), qty);
    return BookResult::Ok;
  }

  BookResult cancel(std::uint64_t id) {
    auto it = where_.find(id);
    if (it == where_.end()) return BookResult::NotFound;
    erase(it);
    return BookResult::Ok;
  }

  BookResult execute(std::uint64_t id, Qty qty) {
    auto it = where_.find(id);
    if (it == where_.end()) return BookResult::NotFound;
    if (qty <= 0 || qty > it->second.it->qty) return BookResult::BadQty;
    if (qty == it->second.it->qty) erase(it);
    else it->second.it->qty -= qty;
    return BookResult::Ok;
  }

  BookResult replace(std::uint64_t id, Price p, Qty qty) {
    auto it = where_.find(id);
    if (it == where_.end()) return BookResult::NotFound;
    if (qty <= 0) return BookResult::BadQty;
    if (!valid(p)) return BookResult::BadPrice;
    if (p.raw() == it->second.price && qty <= it->second.it->qty) {
      it->second.it->qty = qty;  // same price, not larger: keeps priority
      return BookResult::Ok;
    }
    const Side side = it->second.side;
    erase(it);          // loses priority: goes to the back of its (new) level
    insert(id, side, p.raw(), qty);
    return BookResult::Ok;
  }

  Price best_bid() const { return bids_.empty() ? Price::invalid() : Price::from_raw(bids_.rbegin()->first); }
  Price best_ask() const { return asks_.empty() ? Price::invalid() : Price::from_raw(asks_.begin()->first); }
  std::uint32_t order_count() const { return static_cast<std::uint32_t>(where_.size()); }

  std::uint64_t digest() const {
    std::uint64_t h = kDigestSeed;
    h = digest_combine(h, 0);
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) h = level_digest(h, *it);
    h = digest_combine(h, 1);
    for (auto it = asks_.begin(); it != asks_.end(); ++it) h = level_digest(h, *it);
    return h;
  }

 private:
  struct Ord {
    std::uint64_t id;
    Qty qty;
  };
  using Queue = std::list<Ord>;
  struct Loc {
    Side side;
    std::int64_t price;
    Queue::iterator it;
  };

  bool valid(Price p) const {
    const std::int64_t off = p.raw() - cfg_.base.raw();
    return off >= 0 && off % cfg_.tick_raw == 0 &&
           off / cfg_.tick_raw < static_cast<std::int64_t>(cfg_.num_levels);
  }

  std::map<std::int64_t, Queue>& side_map(Side s) { return s == Side::Buy ? bids_ : asks_; }

  void insert(std::uint64_t id, Side side, std::int64_t price, Qty qty) {
    Queue& q = side_map(side)[price];
    q.push_back(Ord{id, qty});
    where_[id] = Loc{side, price, std::prev(q.end())};
  }

  void erase(std::unordered_map<std::uint64_t, Loc>::iterator it) {
    auto& m = side_map(it->second.side);
    auto lv = m.find(it->second.price);
    lv->second.erase(it->second.it);
    if (lv->second.empty()) m.erase(lv);
    where_.erase(it);
  }

  static std::uint64_t level_digest(std::uint64_t h, const std::pair<const std::int64_t, Queue>& lv) {
    Qty total = 0;
    for (const Ord& o : lv.second) total += o.qty;
    h = digest_combine(h, static_cast<std::uint64_t>(lv.first));
    h = digest_combine(h, static_cast<std::uint64_t>(total));
    for (const Ord& o : lv.second) {
      h = digest_combine(h, o.id);
      h = digest_combine(h, static_cast<std::uint64_t>(o.qty));
    }
    return h;
  }

  BookConfig cfg_;
  std::map<std::int64_t, Queue> bids_, asks_;
  std::unordered_map<std::uint64_t, Loc> where_;
};

}  // namespace testsupport
