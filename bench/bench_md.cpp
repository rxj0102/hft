// Market data handler throughput. Phase 3 gate: > 10M msgs/sec.
//
// Three measurements over the same pre-built feed (2M messages in packets of
// 1 to 8 messages, steady-state book of ~20k resting orders):
//   parse only      validate every packet, walk and decode every message; no book
//   handler, 1 line parse + sequencing + book update, line A only
//   handler, A + B  every packet arrives on both lines; B copies are dropped as
//                   duplicates. Reported in UNIQUE messages/sec.
// Each is repeated 7 times (fresh book each time) and reported as min / median /
// max, because a single run on a shared VM says little.
//
// The feed is replayed from one contiguous 70 MB buffer, so reads stream from
// memory instead of sitting in cache, which is closer to a NIC ring than a
// small hot loop would be. Network I/O is not measured here.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "feed_gen.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/core/memory.hpp"
#include "hft/md/itch_encoder.hpp"
#include "hft/md/md_handler.hpp"

using namespace hft;

namespace {

struct Feed {
  std::vector<std::uint8_t> blob;
  std::vector<std::uint32_t> off;  // packet i is blob[off[i], off[i+1])
  std::uint64_t messages = 0;
  std::size_t packets() const { return off.size() - 1; }
};

Feed build_feed(std::uint64_t n_ops, const book::BookConfig& bc) {
  Feed f;
  testsupport::FeedGen gen({bc.base, bc.tick_raw, bc.num_levels, 20000, 1}, 2026);
  testsupport::Rng rng(77);
  md::PacketBuilder pb("SESSION001");
  std::uint64_t seq = 1, ops = 0;
  std::uint8_t body[64];
  f.off.push_back(0);
  while (ops < n_ops) {
    pb.reset(seq);
    for (std::uint32_t j = 0, k = 1 + rng.below(8); j < k && ops < n_ops; ++j) {
      const testsupport::Op op = gen.next();
      pb.add_raw(body, testsupport::FeedGen::encode(op, 1, ops, body));
      ++ops;
    }
    const std::uint8_t* d = pb.finish();
    f.blob.insert(f.blob.end(), d, d + pb.size());
    f.off.push_back(static_cast<std::uint32_t>(f.blob.size()));
    seq += pb.count();
  }
  f.messages = ops;
  return f;
}

struct Stat {
  double min, med, max;
};

Stat summarize(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  return {v.front(), v[v.size() / 2], v.back()};
}

void report(const char* name, Stat s) {
  std::printf("%-22s min %6.1fM  median %6.1fM  max %6.1fM msgs/sec\n", name, s.min / 1e6, s.med / 1e6, s.max / 1e6);
}

}  // namespace

int main() {
  const core::TscCalibration cal;
  std::printf("TSC %.4f ticks/ns, invariant=%s\n", cal.ticks_per_ns(), core::has_invariant_tsc() ? "yes" : "NO");

  book::BookConfig bc;
  bc.base = core::Price::from_units(100);
  bc.tick_raw = 100;
  bc.num_levels = 4096;
  bc.max_orders = 1u << 16;

  constexpr std::uint64_t kOps = 2'000'000;
  const Feed feed = build_feed(kOps, bc);
  std::printf("feed: %llu messages in %zu packets, %.1f MB\n\n", static_cast<unsigned long long>(feed.messages),
              feed.packets(), static_cast<double>(feed.blob.size()) / 1e6);

  core::Region region = core::Region::create(64u << 20, core::RegionOptions{true, true, true, -1});
  if (!region) return 1;
  constexpr int kTrials = 7;

  // 1. Parse only.
  {
    std::vector<double> r;
    std::uint64_t sink = 0;
    for (int t = 0; t < kTrials; ++t) {
      const std::uint64_t c0 = core::rdtsc_ordered();
      std::uint64_t msgs = 0;
      for (std::size_t i = 0; i < feed.packets(); ++i) {
        md::PacketView pv;
        if (!md::parse_packet(feed.blob.data() + feed.off[i], feed.off[i + 1] - feed.off[i], pv)) return 2;
        md::MsgCursor c(pv);
        const std::uint8_t* b;
        std::size_t bl;
        md::Decoded d;
        while (c.next(b, bl)) {
          md::decode(b, d);
          sink += d.id + static_cast<std::uint64_t>(d.qty);
          ++msgs;
        }
      }
      const std::uint64_t c1 = core::rdtsc_ordered();
      r.push_back(static_cast<double>(msgs) / (cal.to_ns(c1 - c0) * 1e-9));
    }
    report("parse only", summarize(r));
    if (sink == 42) std::puts("");  // keep the loop observable
  }

  // 2 and 3. Full handler.
  for (int mode = 0; mode < 2; ++mode) {
    std::vector<double> r;
    std::uint64_t rejects = 0;
    core::Histogram lat;
    for (int t = 0; t < kTrials; ++t) {
      core::Arena arena(region);
      book::OrderBook book;
      md::MdHandler<> h;
      md::MdConfig mc;
      mc.locate = 1;
      mc.pending_slots = 64;
      if (!book.init(arena, bc) || !h.init(arena, book, mc)) return 3;
      h.begin_live(1);

      const bool record_latency = (mode == 0 && t == kTrials - 1);
      const std::uint64_t c0 = core::rdtsc_ordered();
      for (std::size_t i = 0; i < feed.packets(); ++i) {
        const std::uint8_t* p = feed.blob.data() + feed.off[i];
        const std::size_t n = feed.off[i + 1] - feed.off[i];
        if (record_latency) {
          const std::uint64_t a = core::rdtsc_ordered();
          h.on_packet(md::Line::A, p, n, 0);
          lat.record(core::rdtsc_ordered() - a);
        } else {
          h.on_packet(md::Line::A, p, n, 0);
        }
        if (mode == 1) h.on_packet(md::Line::B, p, n, 0);
      }
      const std::uint64_t c1 = core::rdtsc_ordered();
      r.push_back(static_cast<double>(h.stats().messages) / (cal.to_ns(c1 - c0) * 1e-9));
      rejects += h.stats().malformed + h.stats().desyncs + h.stats().gaps_declared;
      if (h.stats().messages != feed.messages || !book.check_invariants()) {
        std::fprintf(stderr, "handler did not apply the whole feed correctly\n");
        return 4;
      }
    }
    report(mode == 0 ? "handler, line A" : "handler, A + B (unique)", summarize(r));
    if (rejects) std::printf("  note: %llu malformed/desync/gap events (expected 0)\n", static_cast<unsigned long long>(rejects));
    if (mode == 0) {
      // Per-packet latency includes the clock reads (about 14 ns, not subtracted here).
      std::printf("  per-packet latency (avg 4.5 msgs/packet, timer overhead included): "
                  "p50 %.0f  p99 %.0f  p99.9 %.0f  max %.0f ns\n",
                  cal.to_ns(lat.quantile(0.5)), cal.to_ns(lat.quantile(0.99)), cal.to_ns(lat.quantile(0.999)),
                  cal.to_ns(lat.max()));
    }
  }
  return 0;
}
