#pragma once
// Vocabulary shared by the matching engine, the exchange simulator and the
// feed publisher. Everything is trivially copyable so events can sit in rings,
// heaps and logs without constructors.

#include <cstdint>
#include <type_traits>

#include "hft/core/fixed_point.hpp"
#include "hft/core/types.hpp"

namespace hft::sim {

using core::OrderId;
using core::Price;
using core::Qty;
using core::Side;

using Owner = std::uint16_t;  // participant id; self-trade prevention keys on it

enum class OrderType : std::uint8_t {
  Limit,     // rests at its limit price after matching what crosses
  Market,    // takes liquidity at any price; the remainder is cancelled
  IOC,       // limit price, immediate-or-cancel: the remainder is cancelled
  FOK,       // limit price, fill-or-kill: all of it now, or nothing happens
  PostOnly,  // rests or is rejected; never takes liquidity
};

enum class StpMode : std::uint8_t {
  None,                // self-trades execute like any other
  CancelNewest,        // the incoming order's remainder is cancelled
  CancelOldest,        // the resting order is cancelled; matching continues
  CancelBoth,          // resting order and incoming remainder are both cancelled
  DecrementAndCancel,  // both shrink by min(qty) with no trade; zeroed one is removed
};

enum class Reason : std::uint8_t {
  None = 0,
  // rejects
  BadId,
  BadQty,
  BadPrice,
  Duplicate,
  WouldCross,    // post-only order would have taken liquidity
  UnknownOrder,  // cancel/replace of an id that is not resting, or not yours
  // cancels
  UserRequested,
  IocRemainder,
  MarketRemainder,  // also: a market order that found no liquidity
  FokUnfillable,
  StpCancel,
  StpDecrement,
  BookFull,  // order matched what it could but no slot was free to rest the rest
};

enum class ReportType : std::uint8_t {
  Accepted,        // passed validation; qty = order quantity
  Rejected,        // new order refused; reason says why. Nothing else follows.
  Fill,            // one side of a trade (a trade produces two reports)
  Canceled,        // order, or part of it, removed; qty = amount removed
  Replaced,        // replace accepted; qty/leaves = new open quantity
  CancelRejected,  // cancel refused
  ReplaceRejected, // replace refused; the original order is untouched
};

enum class Liquidity : std::uint8_t { None, Maker, Taker };

struct NewOrder {
  OrderId id;
  Owner owner;
  Side side;
  OrderType type;
  Price price;  // ignored for Market
  Qty qty;
};

// Private (per-participant) execution report.
struct Report {
  ReportType type;
  Reason reason;
  Liquidity liq;
  Side side;
  Owner owner;
  OrderId id;
  OrderId counter_id;  // Fill: the other side's order id
  Price price;         // Fill: trade price. Accepted/Replaced: the order's limit price
  Qty qty;
  Qty leaves;          // open quantity after this report
  std::uint64_t seq;   // engine report sequence, strictly increasing
  std::uint64_t ts;
};

// Public book-change event: exactly what a market data feed needs.
enum class PublicType : std::uint8_t {
  Add,     // order starts resting; qty = resting quantity
  Exec,    // resting order traded; qty = traded quantity (trade price = price)
  Reduce,  // resting order shrank without trading; qty = amount removed
  Delete,  // resting order removed; qty = quantity it still had
};

struct PublicEvent {
  PublicType type;
  Side side;
  OrderId id;
  Price price;  // level price of the order
  Qty qty;
  std::uint64_t seq;
  std::uint64_t ts;
};

struct NullListener {
  void on_report(const Report&) noexcept {}
  void on_public(const PublicEvent&) noexcept {}
};

static_assert(std::is_trivially_copyable_v<Report> && std::is_trivially_copyable_v<PublicEvent> &&
              std::is_trivially_copyable_v<NewOrder>);

}  // namespace hft::sim
