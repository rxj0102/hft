#pragma once
// Turns the matching engine's public events into ITCH-style packets (the wire
// format of Phase 3), so the simulator can drive the real market data handler.
//
// One public event becomes one message and consumes one feed sequence number:
//   Add -> Add, Exec -> Execute, Reduce -> Partial Cancel, Delete -> Delete.
// Messages are batched into packets of up to kMaxPacketBytes. Packets leave
// through `emit(const uint8_t*, size_t)` when full or on flush(). Nothing is
// sent automatically on a timer: the caller decides when to flush, which keeps
// replay deterministic.

#include <cstddef>
#include <cstdint>

#include "hft/md/itch_encoder.hpp"
#include "hft/md/itch_wire.hpp"
#include "hft/sim/types.hpp"

namespace hft::sim {

template <class Emit>
class ItchPublisher {
 public:
  ItchPublisher(const char (&session)[md::kSessionBytes + 1], std::uint16_t locate, Emit emit,
                std::uint64_t first_seq = 1) noexcept
      : pb_(session), emit_(emit), locate_(locate), next_seq_(first_seq) {
    pb_.reset(next_seq_);
  }

  void on_public(const PublicEvent& e) noexcept {
    if (!append(e)) {
      flush();
      append(e);  // an empty packet always has room for one message
    }
    ++next_seq_;
  }

  // Sends the packet being built, if it has any messages.
  void flush() noexcept {
    if (pb_.count() == 0) return;
    const std::uint8_t* d = pb_.finish();
    emit_(d, pb_.size());
    pb_.reset(next_seq_);
  }

  // Sequence number the next message will carry (what a heartbeat advertises).
  std::uint64_t next_seq() const noexcept { return next_seq_; }

 private:
  bool append(const PublicEvent& e) noexcept {
    using namespace md;
    const auto px = static_cast<std::uint32_t>(e.price.raw());
    const auto q = static_cast<std::uint32_t>(e.qty);
    switch (e.type) {
      case PublicType::Add:
        return pb_.add_message(36, [&](std::uint8_t* d) { encode_add(d, locate_, e.ts, e.id, e.side, q, px); });
      case PublicType::Exec:
        return pb_.add_message(31, [&](std::uint8_t* d) { encode_exec(d, locate_, e.ts, e.id, q); });
      case PublicType::Reduce:
        return pb_.add_message(23, [&](std::uint8_t* d) { encode_partial_cancel(d, locate_, e.ts, e.id, q); });
      case PublicType::Delete:
        return pb_.add_message(19, [&](std::uint8_t* d) { encode_delete(d, locate_, e.ts, e.id); });
    }
    return true;
  }

  md::PacketBuilder pb_;
  Emit emit_;
  std::uint16_t locate_;
  std::uint64_t next_seq_;
};

}  // namespace hft::sim
