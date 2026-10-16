// libFuzzer target for the real-data path: arbitrary text as a capture file.
//
// The input is split into lines and fed to the capture loader (JSON parser,
// decimal parser, converter, ITCH encoder). Every segment it produces is then
// replayed through the production Pipeline.
//
// Oracles, beyond "no crash and no sanitizer report":
//  * a message parsed as Bad carries no changes;
//  * the pipeline's book equals the converter's independent model at every
//    checkpoint and at the end of every segment;
//  * the feed handler saw no desync and no book error (a converter must never
//    emit something its own book rejects);
//  * replaying the segment twice puts identical bytes on the wire.
#include <cstddef>
#include <cstdint>
#include <string>

#include "hft/feed/replay.hpp"

using namespace hft::feed;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 64 * 1024) return 0;
  L2Config cfg;
  cfg.num_levels = 1024;  // a small window so edges and exits are reachable
  CaptureLoader loader(cfg, 4);
  std::size_t pos = 0;
  const char* text = reinterpret_cast<const char*>(data);
  while (pos < size) {
    std::size_t end = pos;
    while (end < size && text[end] != '\n') ++end;
    const std::string line(text + pos, end - pos);
    loader.feed_line(line);
    // the parser alone, with its own oracle
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos) {
      const L2Message m = parse_l2(std::string_view(line).substr(tab + 1));
      if (m.kind == L2Message::Kind::Bad && !m.changes.empty()) __builtin_trap();
    }
    pos = end + 1;
  }
  loader.feed_line("#999999999999\tstop");

  ReplayOptions ro;
  ro.timed = false;
  ro.arena_bytes = 8u << 20;
  for (const Segment& seg : loader.segments()) {
    if (seg.pkts.empty()) continue;
    const ReplayResult a = replay_segment(seg, ro, nullptr);
    if (!a.ok) continue;  // book capacity beyond the small arena: not a finding
    if (a.check_failures != 0 || !a.final_book_matches) __builtin_trap();
    if (a.desyncs != 0 || a.book_errors != 0) __builtin_trap();
    const ReplayResult b = replay_segment(seg, ro, nullptr);
    if (a.wire_hash != b.wire_hash || a.frames != b.frames) __builtin_trap();
  }
  return 0;
}
