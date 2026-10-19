// hft_replay: run a recorded level-2 capture through the production pipeline.
//
//   hft_replay [--levels N] [--tick-raw N] [--rtt-us N] [--lots N] [--no-timing]
//              [--check-every PACKETS] [--verify capture.l2.verify] capture.l2 ...
//
// Prints what was loaded, what the converter did, how the book compares with an
// independent model of the feed, the strategy's order flow against a paper venue
// that never fills, and per-packet processing time on this machine.
//
// Exit codes: 0 ok, 1 usage or I/O, 2 replay not deterministic, 4 book differs
// from the model, 5 nothing usable in the capture.
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hft/feed/replay.hpp"

using namespace hft;

static void print_hist(const char* name, const core::Histogram& h) {
  if (h.count() == 0) {
    std::printf("  %-34s -\n", name);
    return;
  }
  std::printf("  %-34s %8llu  p50 %7llu  p90 %7llu  p99 %7llu  p99.9 %8llu  max %9llu ns\n", name,
              static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(h.quantile(0.50)),
              static_cast<unsigned long long>(h.quantile(0.90)), static_cast<unsigned long long>(h.quantile(0.99)),
              static_cast<unsigned long long>(h.quantile(0.999)), static_cast<unsigned long long>(h.max()));
}

int main(int argc, char** argv) {
  feed::L2Config lc;
  feed::ReplayOptions ro;
  std::vector<std::string> files;
  std::string verify_path;
  std::uint32_t check_every = 100;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](long long& v) {
      if (i + 1 >= argc) return false;
      v = std::atoll(argv[++i]);
      return true;
    };
    long long v = 0;
    if (a == "--levels" && next(v) && v > 0) lc.num_levels = static_cast<std::uint32_t>(v);
    else if (a == "--tick-raw" && next(v) && v > 0) lc.tick_raw = v;
    else if (a == "--rtt-us" && next(v) && v >= 0) ro.venue_rtt_ns = static_cast<std::uint64_t>(v) * 1000;
    else if (a == "--lots" && next(v) && v > 0) ro.mm_qty_lots = v;
    else if (a == "--no-timing") ro.timed = false;
    else if (a == "--check-every" && next(v) && v > 0) check_every = static_cast<std::uint32_t>(v);
    else if (a == "--verify" && i + 1 < argc) verify_path = argv[++i];
    else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "usage: hft_replay [--levels N] [--tick-raw N] [--rtt-us N] [--lots N] [--no-timing] capture.l2 ...\n");
      return 1;
    } else files.push_back(a);
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: hft_replay [options] capture.l2 ...\n");
    return 1;
  }

  core::TscCalibration cal;
  int exit_code = 0;
  std::size_t used = 0;
  for (const std::string& path : files) {
    feed::CaptureLoader loader(lc, check_every);
    if (!loader.load(path)) {
      std::fprintf(stderr, "cannot read %s\n", path.c_str());
      return 1;
    }
    const feed::LoadStats& ls = loader.stats();
    std::printf("== %s\n", path.c_str());
    std::printf("  lines %llu, messages %llu (other %llu, bad %llu, unparsable lines %llu), markers %llu, segments %llu\n",
                static_cast<unsigned long long>(ls.lines), static_cast<unsigned long long>(ls.messages),
                static_cast<unsigned long long>(ls.other), static_cast<unsigned long long>(ls.bad),
                static_cast<unsigned long long>(ls.bad_lines), static_cast<unsigned long long>(ls.markers),
                static_cast<unsigned long long>(ls.segments));
    if (ls.dropped_before_snapshot || ls.snapshots_unusable)
      std::printf("  dropped before a snapshot %llu, unusable snapshots %llu\n",
                  static_cast<unsigned long long>(ls.dropped_before_snapshot),
                  static_cast<unsigned long long>(ls.snapshots_unusable));
    int seg_no = 0;
    for (const feed::Segment& seg : loader.segments()) {
      ++seg_no;
      if (seg.pkts.empty()) continue;
      ++used;
      const feed::L2Stats& s = seg.stats;
      const double secs = static_cast<double>(seg.pkts.back().t - seg.pkts.front().t) / 1e9;
      std::printf("-- segment %d: %zu packets over %.1f s%s, grid base %.4f, window %.2f wide (%u levels)\n", seg_no,
                  seg.pkts.size(), secs, seg.ended_by_window ? " (ended: price left the window)" : "",
                  static_cast<double>(seg.base_raw) / 1e4,
                  static_cast<double>(seg.cfg.num_levels) * static_cast<double>(seg.cfg.tick_raw) / 1e4, seg.cfg.num_levels);
      std::printf("  converter: %llu updates, %llu changes -> %llu add, %llu reduce, %llu delete; peak %llu live levels\n",
                  static_cast<unsigned long long>(s.updates), static_cast<unsigned long long>(s.changes),
                  static_cast<unsigned long long>(s.adds), static_cast<unsigned long long>(s.reduces),
                  static_cast<unsigned long long>(s.deletes), static_cast<unsigned long long>(s.peak_levels));
      std::printf("  dropped: off-tick %llu, outside window %llu, unknown delete %llu; floored sub-lot sizes %llu; clamped %llu; crossed updates %llu\n",
                  static_cast<unsigned long long>(s.off_tick), static_cast<unsigned long long>(s.out_of_window),
                  static_cast<unsigned long long>(s.unknown_delete), static_cast<unsigned long long>(s.sub_lot),
                  static_cast<unsigned long long>(s.clamped), static_cast<unsigned long long>(s.crossed));

      const feed::ReplayResult r = feed::replay_segment(seg, ro, &cal);
      if (!r.ok) {
        std::fprintf(stderr, "pipeline init failed\n");
        return 1;
      }
      feed::ReplayOptions again = ro;
      again.timed = false;
      const feed::ReplayResult r2 = feed::replay_segment(seg, again, nullptr);

      std::printf("  book capacity %u orders (3x peak levels, power of two)\n", r.book_capacity);
      std::printf("  feed handler: %llu messages applied, desyncs %llu, book errors %llu, feed losses %llu\n",
                  static_cast<unsigned long long>(r.messages_applied), static_cast<unsigned long long>(r.desyncs),
                  static_cast<unsigned long long>(r.book_errors), static_cast<unsigned long long>(r.feed_losses));
      std::printf("  model check: %llu checkpoints, %llu failures; final book %s the model\n",
                  static_cast<unsigned long long>(r.checks), static_cast<unsigned long long>(r.check_failures),
                  r.final_book_matches ? "MATCHES" : "DIFFERS FROM");
      std::printf("  strategy: %llu decisions, %llu orders sent (%llu new, %llu replace, %llu cancel), %llu risk rejects\n",
                  static_cast<unsigned long long>(r.decisions), static_cast<unsigned long long>(r.frames),
                  static_cast<unsigned long long>(r.gw.sent_new), static_cast<unsigned long long>(r.gw.sent_replace),
                  static_cast<unsigned long long>(r.gw.sent_cancel), static_cast<unsigned long long>(r.gw.risk_rejects));
      std::printf("  paper venue (rtt %llu us, never fills): %llu accepted, %llu replaced, %llu canceled, %llu rejected as crossing, %llu reports delivered\n",
                  static_cast<unsigned long long>(ro.venue_rtt_ns / 1000), static_cast<unsigned long long>(r.venue.accepted),
                  static_cast<unsigned long long>(r.venue.replaced), static_cast<unsigned long long>(r.venue.canceled),
                  static_cast<unsigned long long>(r.venue.rejected_cross), static_cast<unsigned long long>(r.reports_delivered));
      std::printf("  determinism: wire hash %016llx, second replay %016llx -> %s\n",
                  static_cast<unsigned long long>(r.wire_hash), static_cast<unsigned long long>(r2.wire_hash),
                  r.wire_hash == r2.wire_hash && r.frames == r2.frames ? "identical" : "DIFFERENT");
      if (ro.timed) {
        std::printf("  processing time per packet on this machine (feed handler + book + strategy + gateway, hot loop,\n"
                    "  excludes network, TLS, JSON parsing and the capture process):\n");
        print_hist("every packet", r.on_packet_ns);
        print_hist("packets that sent an order", r.on_packet_order_ns);
        if (r.messages_applied)
          std::printf("  mean %.1f ns per feed message applied (%llu messages in %llu packets)\n",
                      static_cast<double>(r.total_ns) / static_cast<double>(r.messages_applied),
                      static_cast<unsigned long long>(r.messages_applied), static_cast<unsigned long long>(r.packets));
      }
      if (!verify_path.empty() && seg_no == static_cast<int>(loader.segments().size())) {
        // the exchange's own snapshot, fetched right after the capture: did we read the updates correctly?
        std::ifstream vf(verify_path);
        std::string vl;
        const std::size_t vtab = (vf && std::getline(vf, vl)) ? vl.find('\t') : std::string::npos;
        if (vtab == std::string::npos) {
          std::printf("  exchange snapshot check: cannot read %s\n", verify_path.c_str());
        } else {
          const std::uint64_t vt = std::strtoull(vl.c_str(), nullptr, 10);
          feed::CaptureLoader upto(lc, check_every, vt);  // the book as of the moment the snapshot arrived
          upto.load(path);
          const feed::VerifyResult vr = upto.segments().empty()
              ? feed::VerifyResult{}
              : feed::verify_against_snapshot(upto.segments().back(), feed::parse_l2(std::string_view(vl).substr(vtab + 1)), 10);
          if (!vr.usable) {
            std::printf("  exchange snapshot check: no usable snapshot or no book at that time\n");
          } else {
            std::printf("  exchange snapshot check (10 nearest levels per side; exchange snapshot from a second\n"
                        "  connection vs. this replay's book at the same moment, which differ by network skew):\n"
                        "    %llu of %llu levels identical, %llu differ in size, %llu absent from the replayed book\n",
                        static_cast<unsigned long long>(vr.equal), static_cast<unsigned long long>(vr.compared),
                        static_cast<unsigned long long>(vr.size_differs), static_cast<unsigned long long>(vr.missing_in_model));
            if (vr.outside_window)
              std::printf("    (%llu more of the nearest levels are outside the price window, which drops them by design: not compared)\n",
                          static_cast<unsigned long long>(vr.outside_window));
          }
        }
      }
      if (r.wire_hash != r2.wire_hash || r.frames != r2.frames) exit_code = exit_code ? exit_code : 2;
      if (!r.final_book_matches || r.check_failures != 0) exit_code = 4;
    }
  }
  if (used == 0) {
    std::fprintf(stderr, "nothing usable in the capture(s)\n");
    return 5;
  }
  return exit_code;
}
