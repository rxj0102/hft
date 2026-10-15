// Compile-time enforcement of the hot-path rules for the market data path.
// Built with -fno-exceptions -fno-rtti (hft_hotpath); a regression in any md or
// book header that needs exceptions or RTTI breaks the build here.
#include <cstddef>
#include <cstdint>

#include "hft/md/itch_encoder.hpp"
#include "hft/md/itch_wire.hpp"
#include "hft/md/md_handler.hpp"

namespace hft::md {

template class MdHandler<NullListener>;

// Touches the public API so every inline member is compiled in this unit.
std::uint64_t md_hotpath_touch(MdHandler<>& h, const std::uint8_t* data, std::size_t len) noexcept {
  h.on_packet(Line::A, data, len, 1);
  h.on_packet(Line::B, data, len, 2);
  h.on_snapshot_packet(data, len, 3);
  h.poll(4);
  PacketView pv;
  std::uint64_t acc = h.next_seq() + h.stats().messages;
  if (parse_packet(data, len, pv)) {
    MsgCursor c(pv);
    const std::uint8_t* body;
    std::size_t bl;
    Decoded d;
    while (c.next(body, bl)) {
      if (decode(body, d)) acc += d.id;
    }
  }
  return acc + (h.book_valid() ? 1u : 0u) + (h.gap_pending() ? 1u : 0u);
}

}  // namespace hft::md
