// Compile-time enforcement of the hot-path rules for the assembled tick-to-trade
// pipeline: built with -fno-exceptions -fno-rtti.
#include <cstddef>
#include <cstdint>

#include "hft/t2t/pipeline.hpp"

namespace hft::t2t {

namespace {
struct CountSend {
  std::uint64_t* n;
  void operator()(const std::uint8_t*, std::size_t len) noexcept { *n += len; }
};
}  // namespace

std::uint64_t t2t_hotpath_touch(core::Arena& arena, const PipelineConfig& cfg, const std::uint8_t* pkt, std::size_t n,
                                const std::uint8_t* rep, std::size_t rn) noexcept {
  std::uint64_t bytes = 0;
  Pipeline<CountSend> p(CountSend{&bytes}, cfg.mm);
  if (!p.init(arena, cfg)) return 0;
  p.on_packet(md::Line::A, pkt, n, 1);
  p.on_report(rep, rn, 2);
  p.poll(3);
  return bytes + p.stats().decisions;
}

}  // namespace hft::t2t
