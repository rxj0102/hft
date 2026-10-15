#pragma once
// The production tick-to-trade path, assembled:
//
//   feed packet -> MdHandler (A/B arbitration, decode) -> OrderBook
//               -> MarketMaker (signal, quote logic)
//               -> Gateway (risk -> OMS -> wire encode) -> Send
//   exchange bytes -> Gateway::on_bytes (frame, sequence, OMS) -> MarketMaker
//
// This is the code the latency numbers are about. Everything the tick touches
// is in this header and the headers it includes; none of it allocates, locks,
// throws or makes a system call after init().
//
// Time is passed in, never read here, so a recorded session replays to the
// same bytes on the wire.
//
// Feed trust. The strategy only quotes from a book the feed handler vouches
// for. The moment the handler loses the book (declared gap, desync) every
// working quote is pulled, and the sweep repeats after each report until the
// exchange has confirmed the cancels, because an order still PendingNew cannot
// be cancelled yet. Quoting resumes by itself once a snapshot is applied. This
// is deliberately not the kill switch: losing a feed is routine and recovers on
// its own, the kill switch needs a human.

#include <cstddef>
#include <cstdint>

#include "hft/book/book_view.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/md/md_handler.hpp"
#include "hft/strat/market_maker.hpp"

namespace hft::t2t {

struct PipelineConfig {
  md::MdConfig md;
  book::BookConfig book;
  oms::OmsConfig oms{1u << 14};
  risk::RiskConfig risk;
  strat::MmConfig mm;
  std::uint64_t first_seq = 1;
  bool live_at_start = true;  // false: wait for a snapshot before trusting the book
};

struct PipelineStats {
  std::uint64_t decisions = 0;     // strategy runs
  std::uint64_t feed_losses = 0;   // times the book was lost
  std::uint64_t pull_sweeps = 0;   // cancel sweeps while the book was untrusted
};

namespace detail {
struct FeedWatch {
  std::uint64_t* losses = nullptr;
  void on_event(std::uint64_t, const md::Decoded&) noexcept {}
  void on_state(md::FeedState s) noexcept {
    if (s == md::FeedState::NeedSnapshot && losses) ++*losses;
  }
};
}  // namespace detail

template <class Send>
class Pipeline {
 public:
  explicit Pipeline(Send send, const strat::MmConfig& mm = strat::MmConfig{}) noexcept : gw_(send), mm_(mm) {}
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  bool init(core::Arena& arena, const PipelineConfig& cfg) noexcept {
    if (!book_.init(arena, cfg.book)) return false;
    view_.init(&book_, cfg.book);
    if (!md_.init(arena, book_, cfg.md, detail::FeedWatch{&stats_.feed_losses})) return false;
    if (cfg.live_at_start) md_.begin_live(cfg.first_seq);
    return gw_.init(arena, cfg.oms, cfg.risk);
  }

  // ---- the two halves of a tick, for callers that time them separately ------

  // Feed side. True if the strategy should run: the book changed, or it was
  // just lost (so quotes get pulled).
  bool ingest(md::Line line, const std::uint8_t* d, std::size_t n, std::uint64_t now) noexcept {
    const std::uint64_t before = md_.stats().messages;
    const bool was_live = md_.book_valid();
    md_.on_packet(line, d, n, now);
    return md_.stats().messages != before || (was_live && !md_.book_valid());
  }

  // Snapshot channel. True if the snapshot was applied and the book is live again.
  bool ingest_snapshot(const std::uint8_t* d, std::size_t n, std::uint64_t now) noexcept {
    const std::uint64_t before = md_.stats().snapshots_applied;
    md_.on_snapshot_packet(d, n, now);
    return md_.stats().snapshots_applied != before;
  }

  // Strategy side. `G` is the gateway or a thin forwarder around it.
  template <class G>
  void decide(std::uint64_t now, G& gw) noexcept {
    if (!md_.book_valid()) {
      pull(now);
      return;
    }
    ++stats_.decisions;
    mm_.on_update(now, view_, gw);
  }
  void decide(std::uint64_t now) noexcept { decide(now, gw_); }

  // ---- whole-event entry points -------------------------------------------

  void on_packet(md::Line line, const std::uint8_t* d, std::size_t n, std::uint64_t now) noexcept {
    if (ingest(line, d, n, now)) decide(now);
  }

  void on_snapshot(const std::uint8_t* d, std::size_t n, std::uint64_t now) noexcept {
    if (ingest_snapshot(d, n, now)) decide(now);
  }

  // Bytes from the exchange. The strategy always gets a look afterwards: an
  // ack or a fill changes what it should do.
  void on_report(const std::uint8_t* d, std::size_t n, std::uint64_t now) noexcept {
    gw_.on_bytes(d, n, now);
    decide(now);
  }

  // Housekeeping tick: lets a stalled gap be declared even when no packet arrives.
  void poll(std::uint64_t now) noexcept {
    const bool was_live = md_.book_valid();
    md_.poll(now);
    if (was_live && !md_.book_valid()) decide(now);
  }

  gw::Gateway<Send>& gateway() noexcept { return gw_; }
  const gw::Gateway<Send>& gateway() const noexcept { return gw_; }
  md::MdHandler<detail::FeedWatch>& feed() noexcept { return md_; }
  const md::MdHandler<detail::FeedWatch>& feed() const noexcept { return md_; }
  const book::OrderBook& book() const noexcept { return book_; }
  const strat::MarketMaker& strategy() const noexcept { return mm_; }
  const PipelineStats& stats() const noexcept { return stats_; }

 private:
  void pull(std::uint64_t now) noexcept {
    ++stats_.pull_sweeps;
    gw_.cancel_all(now);
  }

  PipelineStats stats_{};
  book::OrderBook book_;
  book::BookView view_;
  md::MdHandler<detail::FeedWatch> md_;
  gw::Gateway<Send> gw_;
  strat::MarketMaker mm_;
};

}  // namespace hft::t2t
