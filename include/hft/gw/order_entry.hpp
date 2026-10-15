#pragma once
// Binary order-entry protocol between a client and the exchange simulator.
// Modelled on OUCH/SoupBinTCP but simpler: fixed-size big-endian messages on
// a byte stream, each prefixed by a 2-byte length, each carrying the sender's
// per-direction sequence number so a gap (lost or reordered data on what is
// supposed to be a reliable stream) is detectable.
//
//   frame   := length:u16  body[length]
//   body    := type:u8  seq:u64  payload
//
//   'O' new order   id:u64 side:u8 otype:u8 qty:i64 price:i64            (35)
//   'X' cancel      id:u64                                                (17)
//   'U' replace     id:u64 qty:i64 price:i64                              (33)
//   'R' report      rtype:u8 reason:u8 liq:u8 side:u8 owner:u16 id:u64
//                   counter:u64 price:i64 qty:i64 leaves:i64 xseq:u64 ts:u64  (71)
//
// The client never sends its identity: the exchange edge stamps the owner from
// the session. The decoders check lengths exactly and every enum range, and
// nothing else: whether a quantity makes sense is the engine's business.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/md/itch_wire.hpp"  // be16/be32/be64 helpers
#include "hft/sim/types.hpp"

namespace hft::gw {

using sim::NewOrder;
using sim::OrderId;
using sim::OrderType;
using sim::Price;
using sim::Qty;
using sim::Report;
using sim::Side;

inline constexpr std::uint8_t kMsgNew = 'O', kMsgCancel = 'X', kMsgReplace = 'U', kMsgReport = 'R';
inline constexpr std::size_t kNewBody = 35, kCancelBody = 17, kReplaceBody = 33, kReportBody = 71;
inline constexpr std::size_t kMaxBody = 71;
inline constexpr std::size_t kMaxFrame = 2 + kMaxBody;

namespace detail {
inline void put_i64(std::uint8_t* p, std::int64_t v) noexcept { md::put_be64(p, static_cast<std::uint64_t>(v)); }
inline std::int64_t get_i64(const std::uint8_t* p) noexcept { return static_cast<std::int64_t>(md::be64(p)); }
inline std::uint8_t* start(std::uint8_t* out, std::size_t body, std::uint8_t type, std::uint64_t seq) noexcept {
  md::put_be16(out, static_cast<std::uint16_t>(body));
  out[2] = type;
  md::put_be64(out + 3, seq);
  return out + 11;
}
}  // namespace detail

// Each encoder writes a complete frame and returns its size. `out` needs kMaxFrame bytes.
inline std::size_t encode_new(std::uint8_t* out, std::uint64_t seq, OrderId id, Side side, OrderType t, Qty qty,
                              Price price) noexcept {
  std::uint8_t* p = detail::start(out, kNewBody, kMsgNew, seq);
  md::put_be64(p, id);
  p[8] = static_cast<std::uint8_t>(side);
  p[9] = static_cast<std::uint8_t>(t);
  detail::put_i64(p + 10, qty);
  detail::put_i64(p + 18, price.raw());
  return 2 + kNewBody;
}
inline std::size_t encode_cancel(std::uint8_t* out, std::uint64_t seq, OrderId id) noexcept {
  std::uint8_t* p = detail::start(out, kCancelBody, kMsgCancel, seq);
  md::put_be64(p, id);
  return 2 + kCancelBody;
}
inline std::size_t encode_replace(std::uint8_t* out, std::uint64_t seq, OrderId id, Qty qty, Price price) noexcept {
  std::uint8_t* p = detail::start(out, kReplaceBody, kMsgReplace, seq);
  md::put_be64(p, id);
  detail::put_i64(p + 8, qty);
  detail::put_i64(p + 16, price.raw());
  return 2 + kReplaceBody;
}
inline std::size_t encode_report(std::uint8_t* out, std::uint64_t seq, const Report& r) noexcept {
  std::uint8_t* p = detail::start(out, kReportBody, kMsgReport, seq);
  p[0] = static_cast<std::uint8_t>(r.type);
  p[1] = static_cast<std::uint8_t>(r.reason);
  p[2] = static_cast<std::uint8_t>(r.liq);
  p[3] = static_cast<std::uint8_t>(r.side);
  md::put_be16(p + 4, r.owner);
  md::put_be64(p + 6, r.id);
  md::put_be64(p + 14, r.counter_id);
  detail::put_i64(p + 22, r.price.raw());
  detail::put_i64(p + 30, r.qty);
  detail::put_i64(p + 38, r.leaves);
  md::put_be64(p + 46, r.seq);
  md::put_be64(p + 54, r.ts);
  return 2 + kReportBody;
}

// What a client may send. `kind` is the message type byte.
struct ClientMsg {
  std::uint8_t kind = 0;
  std::uint64_t seq = 0;
  OrderId id = 0;
  Side side = Side::Buy;
  OrderType otype = OrderType::Limit;
  Qty qty = 0;
  Price price{};
};

inline std::uint8_t body_type(const std::uint8_t* body, std::size_t len) noexcept { return len ? body[0] : 0; }

// `body` points past the length prefix. False if the type is unknown, the
// length is not exactly right, or an enum is out of range.
inline bool decode_client(const std::uint8_t* body, std::size_t len, ClientMsg& m) noexcept {
  if (len < 9) return false;
  const std::uint8_t t = body[0];
  const std::uint8_t* p = body + 9;
  m.kind = t;
  m.seq = md::be64(body + 1);
  switch (t) {
    case kMsgNew:
      if (len != kNewBody || p[8] > 1 || p[9] > static_cast<std::uint8_t>(OrderType::PostOnly)) return false;
      m.id = md::be64(p);
      m.side = static_cast<Side>(p[8]);
      m.otype = static_cast<OrderType>(p[9]);
      m.qty = detail::get_i64(p + 10);
      m.price = Price::from_raw(detail::get_i64(p + 18));
      return true;
    case kMsgCancel:
      if (len != kCancelBody) return false;
      m.id = md::be64(p);
      return true;
    case kMsgReplace:
      if (len != kReplaceBody) return false;
      m.id = md::be64(p);
      m.qty = detail::get_i64(p + 8);
      m.price = Price::from_raw(detail::get_i64(p + 16));
      return true;
    default:
      return false;
  }
}

inline bool decode_report(const std::uint8_t* body, std::size_t len, Report& r, std::uint64_t& seq) noexcept {
  if (len != kReportBody || body[0] != kMsgReport) return false;
  seq = md::be64(body + 1);
  const std::uint8_t* p = body + 9;
  if (p[0] > static_cast<std::uint8_t>(sim::ReportType::ReplaceRejected) ||
      p[1] > static_cast<std::uint8_t>(sim::Reason::BookFull) ||
      p[2] > static_cast<std::uint8_t>(sim::Liquidity::Taker) || p[3] > 1)
    return false;
  r.type = static_cast<sim::ReportType>(p[0]);
  r.reason = static_cast<sim::Reason>(p[1]);
  r.liq = static_cast<sim::Liquidity>(p[2]);
  r.side = static_cast<Side>(p[3]);
  r.owner = md::be16(p + 4);
  r.id = md::be64(p + 6);
  r.counter_id = md::be64(p + 14);
  r.price = Price::from_raw(detail::get_i64(p + 22));
  r.qty = detail::get_i64(p + 30);
  r.leaves = detail::get_i64(p + 38);
  r.seq = md::be64(p + 46);
  r.ts = md::be64(p + 54);
  return true;
}

// Reassembles frames from an arbitrary byte stream (any split, including one
// byte at a time). Fixed buffer, no allocation. A frame whose declared length
// is 0 or larger than kMaxBody means the stream is corrupt: the decoder goes
// to the Broken state and ignores further input until reset().
class StreamDecoder {
 public:
  enum class Status : std::uint8_t { Ok, Broken };

  // Handler needs: void on_frame(const uint8_t* body, size_t len).
  template <class Handler>
  Status feed(const std::uint8_t* p, std::size_t n, Handler& h) noexcept {
    if (broken_) return Status::Broken;
    while (n > 0) {
      if (have_ == 0 && n >= 2) {  // fast path: parse in place, no copy
        const std::size_t len = md::be16(p);
        if (len == 0 || len > kMaxBody) return broken_ = true, Status::Broken;
        if (n >= 2 + len) {
          h.on_frame(p + 2, len);
          p += 2 + len;
          n -= 2 + len;
          continue;
        }
      }
      // Slow path: a frame straddles calls. Take only what completes it.
      std::size_t want = have_ < 2 ? 2 - have_ : 2 + len_ - have_;
      const std::size_t take = n < want ? n : want;
      std::memcpy(buf_ + have_, p, take);
      have_ += take;
      p += take;
      n -= take;
      if (have_ == 2 && len_ == 0) {
        len_ = md::be16(buf_);
        if (len_ == 0 || len_ > kMaxBody) return broken_ = true, Status::Broken;
      }
      if (have_ >= 2 && have_ == 2 + len_) {
        h.on_frame(buf_ + 2, len_);
        have_ = 0;
        len_ = 0;
      }
    }
    return Status::Ok;
  }

  bool broken() const noexcept { return broken_; }
  std::size_t buffered() const noexcept { return have_; }
  void reset() noexcept { have_ = 0, len_ = 0, broken_ = false; }

 private:
  std::uint8_t buf_[kMaxFrame]{};
  std::size_t have_{0}, len_{0};
  bool broken_{false};
};

}  // namespace hft::gw
