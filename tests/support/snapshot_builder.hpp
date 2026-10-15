#pragma once
// Builds the snapshot-channel packets that describe a RefBook "as of" sequence S:
// Begin(S, total), one Add per resting order (queue order preserved), End(S).
// Shared by the simulation tests and the fuzz seed-corpus generator.

#include <cstdint>
#include <vector>

#include "hft/md/itch_encoder.hpp"
#include "reference_book.hpp"

namespace testsupport {

inline std::vector<std::vector<std::uint8_t>> build_snapshot(const RefBook& ref, std::uint64_t S,
                                                             std::uint64_t& stream_seq,
                                                             std::uint16_t locate) {
  using namespace hft::md;
  struct O {
    Side s;
    std::int64_t px;
    std::uint64_t id;
    hft::core::Qty q;
  };
  std::vector<O> orders;
  ref.for_each_order([&](Side s, std::int64_t p, std::uint64_t id, hft::core::Qty q) {
    orders.push_back({s, p, id, q});
  });
  std::vector<std::vector<std::uint8_t>> out;
  PacketBuilder pb("SNAPSHOT01");
  std::uint64_t seq = stream_seq;
  pb.reset(seq);
  auto flush = [&] {
    if (!pb.count()) return;
    const std::uint8_t* d = pb.finish();
    out.emplace_back(d, d + pb.size());
    seq += pb.count();
    pb.reset(seq);
  };
  auto put = [&](std::size_t len, auto fill) {
    if (!pb.add_message(len, fill)) {
      flush();
      pb.add_message(len, fill);
    }
  };
  put(13, [&](std::uint8_t* p) { encode_snap_begin(p, S, static_cast<std::uint32_t>(orders.size())); });
  for (const O& o : orders)
    put(36, [&](std::uint8_t* p) {
      encode_add(p, locate, 0, o.id, o.s, static_cast<std::uint32_t>(o.q), static_cast<std::uint32_t>(o.px));
    });
  put(9, [&](std::uint8_t* p) { encode_snap_end(p, S); });
  flush();
  stream_seq = seq;
  return out;
}

}  // namespace testsupport
