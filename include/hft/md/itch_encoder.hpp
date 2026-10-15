#pragma once
// Encoders for the wire format. Used by tests, the benchmark and (later) the
// exchange simulator. Not on the receive hot path.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/md/itch_wire.hpp"

namespace hft::md {

// Each writer fills `p` (which must have msg_size(type) bytes) and returns that size.
inline std::size_t encode_add(std::uint8_t* p, std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                              core::Side side, std::uint32_t shares, std::uint32_t price) noexcept {
  std::memset(p, 0, 36);
  p[0] = kAdd;
  put_be16(p + 1, locate);
  put_be48(p + 5, ts);
  put_be64(p + 11, ref);
  p[19] = side == core::Side::Buy ? 'B' : 'S';
  put_be32(p + 20, shares);
  std::memcpy(p + 24, "SYNTH   ", 8);
  put_be32(p + 32, price);
  return 36;
}

inline std::size_t encode_exec(std::uint8_t* p, std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                               std::uint32_t shares, std::uint64_t match = 0) noexcept {
  std::memset(p, 0, 31);
  p[0] = kExec;
  put_be16(p + 1, locate);
  put_be48(p + 5, ts);
  put_be64(p + 11, ref);
  put_be32(p + 19, shares);
  put_be64(p + 23, match);
  return 31;
}

inline std::size_t encode_partial_cancel(std::uint8_t* p, std::uint16_t locate, std::uint64_t ts,
                                         std::uint64_t ref, std::uint32_t shares) noexcept {
  std::memset(p, 0, 23);
  p[0] = kPartialCancel;
  put_be16(p + 1, locate);
  put_be48(p + 5, ts);
  put_be64(p + 11, ref);
  put_be32(p + 19, shares);
  return 23;
}

inline std::size_t encode_delete(std::uint8_t* p, std::uint16_t locate, std::uint64_t ts,
                                 std::uint64_t ref) noexcept {
  std::memset(p, 0, 19);
  p[0] = kDelete;
  put_be16(p + 1, locate);
  put_be48(p + 5, ts);
  put_be64(p + 11, ref);
  return 19;
}

inline std::size_t encode_replace(std::uint8_t* p, std::uint16_t locate, std::uint64_t ts, std::uint64_t orig,
                                  std::uint64_t neu, std::uint32_t shares, std::uint32_t price) noexcept {
  std::memset(p, 0, 35);
  p[0] = kReplace;
  put_be16(p + 1, locate);
  put_be48(p + 5, ts);
  put_be64(p + 11, orig);
  put_be64(p + 19, neu);
  put_be32(p + 27, shares);
  put_be32(p + 31, price);
  return 35;
}

inline std::size_t encode_snap_begin(std::uint8_t* p, std::uint64_t snapshot_seq, std::uint32_t total) noexcept {
  p[0] = kSnapBegin;
  put_be64(p + 1, snapshot_seq);
  put_be32(p + 9, total);
  return 13;
}

inline std::size_t encode_snap_end(std::uint8_t* p, std::uint64_t snapshot_seq) noexcept {
  p[0] = kSnapEnd;
  put_be64(p + 1, snapshot_seq);
  return 9;
}

// Accumulates messages into one packet. add() returns false (and writes
// nothing) if the next message would not fit in kMaxPacketBytes.
class PacketBuilder {
 public:
  explicit PacketBuilder(const char (&session)[kSessionBytes + 1]) noexcept {
    std::memcpy(session_, session, kSessionBytes);
  }

  void reset(std::uint64_t first_seq) noexcept {
    std::memcpy(buf_, session_, kSessionBytes);
    put_be64(buf_ + kSessionBytes, first_seq);
    count_ = 0;
    size_ = kHeaderBytes;
    first_seq_ = first_seq;
  }

  // Writes `body_len` bytes via `fill(dest)`; the writers above fit that shape.
  template <class Fill>
  bool add_message(std::size_t body_len, Fill&& fill) noexcept {
    if (size_ + 2 + body_len > kMaxPacketBytes || count_ == 0xFFFF) return false;
    put_be16(buf_ + size_, static_cast<std::uint16_t>(body_len));
    fill(buf_ + size_ + 2);
    size_ += 2 + body_len;
    ++count_;
    return true;
  }

  // Raw message bytes (for tests that need to craft unusual bodies).
  bool add_raw(const std::uint8_t* body, std::size_t len) noexcept {
    return add_message(len, [&](std::uint8_t* d) { std::memcpy(d, body, len); });
  }

  const std::uint8_t* finish() noexcept {
    put_be16(buf_ + kSessionBytes + 8, count_);
    return buf_;
  }
  std::size_t size() const noexcept { return size_; }
  std::uint16_t count() const noexcept { return count_; }
  std::uint64_t first_seq() const noexcept { return first_seq_; }

 private:
  std::uint8_t session_[kSessionBytes]{};
  std::uint8_t buf_[kMaxPacketBytes]{};
  std::size_t size_{kHeaderBytes};
  std::uint16_t count_{0};
  std::uint64_t first_seq_{0};
};

}  // namespace hft::md
