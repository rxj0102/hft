#pragma once
// ITCH-style wire format. Not the real Nasdaq spec: it keeps the parts that
// matter for the engineering (big-endian fixed-size binary messages, a
// MoldUDP64-like sequenced packet frame, order-by-order book events) and drops
// the exchange-specific breadth.
//
// Packet frame (all integers big-endian):
//   session[10]  seq u64 (sequence number of the FIRST message)  count u16
//   then `count` times:  length u16  |  body[length]   (body[0] is the type)
// A packet with count == 0 is a heartbeat; its seq field is the next sequence
// number the sender will use, which lets a receiver notice trailing loss.
//
// Parsing is zero-copy: fields are read straight out of the receive buffer
// with memcpy+bswap (one load, one byte swap; no aliasing violations, no
// intermediate struct copy).
//
// Validation is strict and happens for the whole packet BEFORE any message is
// applied, so a malformed packet can never half-apply to the book.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/core/fixed_point.hpp"
#include "hft/core/types.hpp"

namespace hft::md {

inline constexpr std::size_t kSessionBytes = 10;
inline constexpr std::size_t kHeaderBytes = 20;
// UDP payload that fits a 1500-byte Ethernet MTU without IP fragmentation.
inline constexpr std::size_t kMaxPacketBytes = 1472;

// Message types. 'B'/'N' only appear on the snapshot channel.
inline constexpr std::uint8_t kAdd = 'A';
inline constexpr std::uint8_t kExec = 'E';
inline constexpr std::uint8_t kPartialCancel = 'X';
inline constexpr std::uint8_t kDelete = 'D';
inline constexpr std::uint8_t kReplace = 'U';
inline constexpr std::uint8_t kSnapBegin = 'B';
inline constexpr std::uint8_t kSnapEnd = 'N';

// Body sizes, including the type byte. 0 means unknown type.
constexpr std::size_t msg_size(std::uint8_t t) noexcept {
  switch (t) {
    case kAdd: return 36;
    case kExec: return 31;
    case kPartialCancel: return 23;
    case kDelete: return 19;
    case kReplace: return 35;
    case kSnapBegin: return 13;
    case kSnapEnd: return 9;
    default: return 0;
  }
}

// ---- big-endian primitives --------------------------------------------------

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
// 6-byte timestamp (nanoseconds since midnight, as in ITCH).
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2);
}

inline void put_be16(std::uint8_t* p, std::uint16_t v) noexcept {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void put_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void put_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline void put_be48(std::uint8_t* p, std::uint64_t v) noexcept {
  put_be16(p, static_cast<std::uint16_t>(v >> 32));
  put_be32(p + 2, static_cast<std::uint32_t>(v));
}

// ---- packet parsing ---------------------------------------------------------

struct PacketView {
  const std::uint8_t* data{nullptr};
  std::size_t len{0};
  const std::uint8_t* session{nullptr};  // points into data
  std::uint64_t seq{0};                  // sequence number of the first message
  std::uint16_t count{0};
  // seq + count, guaranteed not to overflow after a successful parse.
  std::uint64_t end() const noexcept { return seq + count; }
};

// Validates the whole packet. Returns false for anything malformed: short or
// oversized frame, sequence overflow, a message that overruns the packet, a
// known type with the wrong length, a bad side byte, or trailing bytes.
// Unknown message types are tolerated (skipped) for forward compatibility.
inline bool parse_packet(const std::uint8_t* data, std::size_t len, PacketView& out) noexcept {
  if (len < kHeaderBytes || len > kMaxPacketBytes) return false;
  const std::uint64_t seq = be64(data + kSessionBytes);
  const std::uint16_t count = be16(data + kSessionBytes + 8);
  if (seq + count < seq) return false;  // wraps the 64-bit sequence space

  std::size_t off = kHeaderBytes;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (len - off < 2) return false;
    const std::size_t mlen = be16(data + off);
    off += 2;
    if (mlen == 0 || mlen > len - off) return false;
    const std::uint8_t type = data[off];
    const std::size_t want = msg_size(type);
    if (want != 0 && mlen != want) return false;
    if (type == kAdd && data[off + 19] != 'B' && data[off + 19] != 'S') return false;
    off += mlen;
  }
  if (off != len) return false;

  out.data = data;
  out.len = len;
  out.session = data;
  out.seq = seq;
  out.count = count;
  return true;
}

// Walks the messages of a packet that parse_packet() already accepted, so no
// bounds checks are repeated. Hands back body pointer and length.
class MsgCursor {
 public:
  explicit MsgCursor(const PacketView& pv) noexcept : p_(pv.data + kHeaderBytes), left_(pv.count) {}
  bool next(const std::uint8_t*& body, std::size_t& blen) noexcept {
    if (left_ == 0) return false;
    blen = be16(p_);
    body = p_ + 2;
    p_ += 2 + blen;
    --left_;
    return true;
  }

 private:
  const std::uint8_t* p_;
  std::uint16_t left_;
};

// ---- decoded view of one message -------------------------------------------

struct Decoded {
  std::uint8_t type{0};
  core::Side side{core::Side::Buy};
  std::uint16_t locate{0};
  std::uint64_t ts{0};
  std::uint64_t id{0};      // order ref (original ref for 'U'); snapshot seq for 'B'/'N'
  std::uint64_t id2{0};     // new order ref for 'U'
  core::Qty qty{0};         // shares / executed / cancelled; total orders for 'B'
  core::Price price{};      // ITCH price has 4 implied decimals, same scale as Price
};

// Body must already be validated for length (parse_packet). Layouts:
//  A: type1 locate2 tracking2 ts6 ref8 side1 shares4 stock8 price4   (36)
//  E: type1 locate2 tracking2 ts6 ref8 executed4 match8              (31)
//  X: type1 locate2 tracking2 ts6 ref8 cancelled4                    (23)
//  D: type1 locate2 tracking2 ts6 ref8                               (19)
//  U: type1 locate2 tracking2 ts6 orig8 new8 shares4 price4          (35)
//  B: type1 snapshot_seq8 total_orders4                              (13)
//  N: type1 snapshot_seq8                                            (9)
inline bool decode(const std::uint8_t* p, Decoded& d) noexcept {
  d.type = p[0];
  switch (p[0]) {
    case kAdd:
      d.locate = be16(p + 1);
      d.ts = be48(p + 5);
      d.id = be64(p + 11);
      d.side = (p[19] == 'B') ? core::Side::Buy : core::Side::Sell;
      d.qty = static_cast<core::Qty>(be32(p + 20));
      d.price = core::Price::from_raw(static_cast<std::int64_t>(be32(p + 32)));
      return true;
    case kExec:
      d.locate = be16(p + 1);
      d.ts = be48(p + 5);
      d.id = be64(p + 11);
      d.qty = static_cast<core::Qty>(be32(p + 19));
      return true;
    case kPartialCancel:
      d.locate = be16(p + 1);
      d.ts = be48(p + 5);
      d.id = be64(p + 11);
      d.qty = static_cast<core::Qty>(be32(p + 19));
      return true;
    case kDelete:
      d.locate = be16(p + 1);
      d.ts = be48(p + 5);
      d.id = be64(p + 11);
      return true;
    case kReplace:
      d.locate = be16(p + 1);
      d.ts = be48(p + 5);
      d.id = be64(p + 11);
      d.id2 = be64(p + 19);
      d.qty = static_cast<core::Qty>(be32(p + 27));
      d.price = core::Price::from_raw(static_cast<std::int64_t>(be32(p + 31)));
      return true;
    case kSnapBegin:
      d.id = be64(p + 1);
      d.qty = static_cast<core::Qty>(be32(p + 9));
      return true;
    case kSnapEnd:
      d.id = be64(p + 1);
      return true;
    default:
      return false;
  }
}

}  // namespace hft::md
