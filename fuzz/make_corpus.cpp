// Writes a seed corpus for fuzz_md: valid-looking sessions (in-order, duplicated
// across lines, with loss, reordering, heartbeats and snapshots) in the
// harness's chunk framing, so the fuzzer starts deep inside the handler instead
// of spending its first hours rediscovering the packet header.
//
// usage: make_corpus <output-dir> [count]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "feed_gen.hpp"
#include "hft/md/itch_encoder.hpp"
#include "snapshot_builder.hpp"

using namespace hft;
using namespace hft::md;
using testsupport::FeedConfig;
using testsupport::FeedGen;
using testsupport::RefBook;
using testsupport::Rng;
using Bytes = std::vector<std::uint8_t>;

namespace {

void chunk(Bytes& out, std::uint8_t kind, const std::uint8_t* d, std::size_t n) {
  out.push_back(kind);
  out.push_back(static_cast<std::uint8_t>(n >> 8));
  out.push_back(static_cast<std::uint8_t>(n));
  out.insert(out.end(), d, d + n);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output-dir> [count]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];
  const int count = argc > 2 ? std::atoi(argv[2]) : 200;

  book::BookConfig bc;
  bc.base = core::Price::from_units(100);
  bc.tick_raw = 100;
  bc.num_levels = 256;
  bc.max_orders = 128;

  for (int f = 0; f < count; ++f) {
    Rng rng(1000 + static_cast<std::uint64_t>(f));
    FeedGen gen(FeedConfig{bc.base, bc.tick_raw, bc.num_levels, 40, 1}, 7 + static_cast<std::uint64_t>(f));
    RefBook ref(bc);
    const bool start_live = (f % 3) != 0;  // a third of the seeds begin by waiting for a snapshot
    Bytes out;
    out.push_back(start_live ? 1 : 0);

    PacketBuilder pb("SESSION001");
    std::uint64_t seq = 1, snap_seq = 1;
    std::uint8_t body[64];
    const int packets = 20 + static_cast<int>(rng.below(40));
    for (int p = 0; p < packets; ++p) {
      pb.reset(seq);
      for (std::uint32_t j = 0, k = 1 + rng.below(5); j < k; ++j) {
        const testsupport::Op op = gen.next();
        pb.add_raw(body, FeedGen::encode(op, 1, static_cast<std::uint64_t>(p), body));
        testsupport::FeedGen::apply_to_ref(ref, op);
      }
      const std::uint8_t* d = pb.finish();
      const std::size_t n = pb.size();
      seq += pb.count();

      const std::uint32_t r = rng.below(100);
      if (r < 10) {
        // Dropped on both lines: a true gap.
      } else if (r < 25) {
        chunk(out, 0, d, n);  // only line A
      } else if (r < 40) {
        chunk(out, 1, d, n);  // only line B
      } else {
        chunk(out, 0, d, n);
        chunk(out, 1, d, n);  // both lines: the second copy is a duplicate
      }
      if (rng.below(100) < 8) {  // heartbeat announcing the next sequence
        PacketBuilder hb("SESSION001");
        hb.reset(seq);
        chunk(out, 0, hb.finish(), hb.size());
      }
      if (rng.below(100) < 12) {  // let time pass so gap timers can fire
        const std::uint8_t none = 0;
        chunk(out, static_cast<std::uint8_t>(3 | (rng.below(60) << 2)), &none, 0);
      }
      if (rng.below(100) < 10) {  // a snapshot as of the current sequence
        for (const Bytes& s : testsupport::build_snapshot(ref, seq - 1, snap_seq, 1)) chunk(out, 2, s.data(), s.size());
      }
    }

    char name[64];
    std::snprintf(name, sizeof name, "/seed_%04d", f);
    std::FILE* fp = std::fopen((dir + name).c_str(), "wb");
    if (!fp) {
      std::fprintf(stderr, "cannot write %s\n", (dir + name).c_str());
      return 1;
    }
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
  }
  std::printf("wrote %d seeds to %s\n", count, dir.c_str());
  return 0;
}
