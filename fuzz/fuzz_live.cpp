// libFuzzer target for the live session: arbitrary text as the input stream, plus
// control lines that move time and stop the session.
//
//   "<digits>\t<json>"   a feed line; the digits are the clock
//   "#<digits>\t<event>"  a marker
//   "@p<digits>"          poll at (last time + digits)
//   "@k"                  kill      "@s"   shutdown
//
// Oracles, beyond "no crash and no sanitizer report":
//  * the session never fails to get flat, never loses the independent model, and
//    the feed handler never sees a desync or a book error;
//  * after any segment ends nothing is left working (open orders, venue orders);
//  * every cancel the session sent was for an order the venue accepted;
//  * the same input twice gives identical order flow (determinism).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/feed/live.hpp"

using namespace hft::feed;

namespace {

struct Out {
  std::vector<std::uint64_t> hashes;
  LiveStats st;
  std::size_t segs = 0;
};

Out run(const std::uint8_t* data, std::size_t size) {
  LiveConfig c;
  c.l2.num_levels = 1024;  // small window: edges and exits are reachable
  c.check_every = 3;
  c.arena_bytes = 16u << 20;
  c.max_orders = 8192;
  c.stale_ns = 1000;       // short, so silences between lines are common
  c.venue_rtt_ns = 40;
  LiveSession s(c);
  const char* text = reinterpret_cast<const char*>(data);
  std::uint64_t last = 0;
  std::size_t pos = 0;
  while (pos < size) {
    std::size_t end = pos;
    while (end < size && text[end] != '\n') ++end;
    const std::string line(text + pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line[0] == '@') {
      if (line.size() > 1 && line[1] == 'k') s.kill(last);
      else if (line.size() > 1 && line[1] == 's') s.shutdown(last);
      else if (line.size() > 1 && line[1] == 'p') {
        std::uint64_t d = 0;
        for (std::size_t i = 2; i < line.size() && i < 12 && line[i] >= '0' && line[i] <= '9'; ++i) d = d * 10 + static_cast<std::uint64_t>(line[i] - '0');
        s.poll(last + d);
        last += d;
      }
      continue;
    }
    std::size_t i = (!line.empty() && line[0] == '#') ? 1 : 0;
    std::uint64_t t = 0;
    bool any = false;
    for (; i < line.size() && i < 12 && line[i] >= '0' && line[i] <= '9'; ++i) {
      t = t * 10 + static_cast<std::uint64_t>(line[i] - '0');
      any = true;
    }
    if (any) last = t > last ? t : last;
    s.on_line(line, any ? t : last);
    if (!s.active() && (s.venue_live() != 0 || s.open_orders() != 0)) __builtin_trap();
  }
  s.shutdown(last + 1);
  Out o;
  o.st = s.stats();
  o.segs = s.summaries().size();
  for (const SegmentSummary& sm : s.summaries()) {
    if (!sm.flat) __builtin_trap();
    if (!sm.book_matched_model) __builtin_trap();
    o.hashes.push_back(sm.hash_before_flatten ^ (sm.frames_total << 1));
  }
  return o;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 64 * 1024) return 0;
  const Out a = run(data, size);
  if (a.st.flatten_failures || a.st.check_failures || a.st.final_mismatches) __builtin_trap();
  if (a.st.desyncs || a.st.book_errors) __builtin_trap();
  if (a.st.canc > a.st.acc) __builtin_trap();
  const Out b = run(data, size);
  if (a.hashes != b.hashes || a.segs != b.segs || a.st.sent_new != b.st.sent_new || a.st.sent_cancel != b.st.sent_cancel ||
      a.st.sent_replace != b.st.sent_replace)
    __builtin_trap();
  return 0;
}
