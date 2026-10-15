#pragma once
// A recorded session: exactly what the strategy side of a backtest saw (feed
// packets and exchange reports, each with the time it arrived) and exactly
// what it sent. Replaying the inputs through the production Pipeline must put
// the same bytes on the wire; that equality is the correctness oracle for every
// latency measurement taken on the replay.
//
// Setup code, not hot path: it allocates freely.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hft/bt/backtest.hpp"
#include "hft/bt/market_gen.hpp"
#include "hft/sim/feed_publisher.hpp"
#include "hft/t2t/pipeline.hpp"

namespace hft::t2t {

struct Input {
  enum Kind : std::uint8_t { Md = 0, Report = 1 };
  Kind kind;
  std::uint64_t now;  // logical time the backtest delivered it
  std::uint32_t idx;  // packet index or report index
};

// FNV-1a over a frame, with the length mixed in so frame boundaries count.
inline std::uint64_t frame_hash(std::uint64_t h, const std::uint8_t* d, std::size_t n) noexcept {
  constexpr std::uint64_t kPrime = 0x100000001B3ULL;
  h ^= n;
  h *= kPrime;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= d[i];
    h *= kPrime;
  }
  return h;
}
inline constexpr std::uint64_t kHashSeed = 0xCBF29CE484222325ULL;

struct Session {
  PipelineConfig cfg;
  std::vector<Input> inputs;
  std::vector<std::uint8_t> pkt_blob;
  std::vector<std::uint32_t> pkt_off{0};  // packet i = [pkt_off[i], pkt_off[i+1])
  std::vector<std::uint8_t> rep_blob;
  std::vector<std::uint32_t> rep_off{0};

  // what the backtest sent
  std::uint64_t frames = 0, wire_bytes = 0, wire_hash = kHashSeed;
  std::uint64_t sent_new = 0, sent_replace = 0, sent_cancel = 0, risk_rejects = 0;
  std::int64_t final_pos = 0;
  std::uint64_t book_digest = 0;
  std::uint64_t backtest_digest = 0;

  const std::uint8_t* packet(std::uint32_t i, std::size_t& n) const noexcept {
    n = pkt_off[i + 1] - pkt_off[i];
    return pkt_blob.data() + pkt_off[i];
  }
  const std::uint8_t* report(std::uint32_t i, std::size_t& n) const noexcept {
    n = rep_off[i + 1] - rep_off[i];
    return rep_blob.data() + rep_off[i];
  }
  std::size_t md_inputs() const noexcept { return pkt_off.size() - 1; }
};

namespace detail {

struct Recorder {
  Session* s = nullptr;
  void md(std::uint64_t now, std::size_t i) noexcept { s->inputs.push_back({Input::Md, now, static_cast<std::uint32_t>(i)}); }
  void report(std::uint64_t now, const std::uint8_t* d, std::size_t n) noexcept {
    s->rep_blob.insert(s->rep_blob.end(), d, d + n);
    s->rep_off.push_back(static_cast<std::uint32_t>(s->rep_blob.size()));
    s->inputs.push_back({Input::Report, now, static_cast<std::uint32_t>(s->rep_off.size() - 2)});
  }
  void sent(std::uint64_t, const std::uint8_t* d, std::size_t n) noexcept {
    ++s->frames;
    s->wire_bytes += n;
    s->wire_hash = frame_hash(s->wire_hash, d, n);
  }
};

struct PacketEmit {
  Session* s;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    s->pkt_blob.insert(s->pkt_blob.end(), d, d + n);
    s->pkt_off.push_back(static_cast<std::uint32_t>(s->pkt_blob.size()));
  }
};

}  // namespace detail

// Runs the backtester over `m`, records everything the strategy side saw and
// sent, and encodes the tape as one ITCH packet per event (the lowest latency
// way to publish, and the case worth measuring). Returns false if init failed.
inline bool record_session(const bt::Market& m, const bt::BtConfig& bc, const strat::MmConfig& mm,
                           const risk::RiskConfig& rc, Session& out) {
  out = Session{};
  out.cfg.book.base = m.cfg.base;
  out.cfg.book.tick_raw = m.cfg.tick_raw;
  out.cfg.book.num_levels = m.cfg.num_levels;
  out.cfg.book.max_orders = bc.max_resting;
  out.cfg.md.locate = 1;
  out.cfg.oms = bc.oms;
  out.cfg.risk = rc;
  out.cfg.mm = mm;
  out.cfg.first_seq = 1;

  {
    detail::PacketEmit emit{&out};
    sim::ItchPublisher<detail::PacketEmit> pub("T2TSESS001", 1, emit, 1);
    for (const sim::PublicEvent& e : m.tape) {
      pub.on_public(e);
      pub.flush();
    }
  }

  bt::BtConfig c = bc;
  c.risk = rc;
  c.tick_raw = m.cfg.tick_raw;
  core::Region region = core::Region::create(128u << 20, core::RegionOptions{false, false, true, -1});
  core::Arena arena(region);
  strat::MarketMaker strat(mm);
  auto* b = new bt::Backtester<strat::MarketMaker, detail::Recorder>();
  b->observer().s = &out;
  if (!b->init(arena, m.tape.data(), m.tape.size(), out.cfg.book, c, &strat)) {
    delete b;
    return false;
  }
  // init() applied bc.max_resting to the tape book; the pipeline book matches it.
  b->run();
  const bt::BtReport r = b->report();
  out.sent_new = b->gateway().stats().sent_new;
  out.sent_replace = b->gateway().stats().sent_replace;
  out.sent_cancel = b->gateway().stats().sent_cancel;
  out.risk_rejects = b->gateway().stats().risk_rejects;
  out.final_pos = b->gateway().oms().position().pos;
  out.book_digest = b->strategy_book().digest();
  out.backtest_digest = r.digest;
  delete b;
  return true;
}

}  // namespace hft::t2t
