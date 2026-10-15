#pragma once
// Event-driven backtester. The strategy runs through the same Gateway, OMS and
// RiskEngine as in production, speaking the real order entry wire format, to a
// VirtualVenue that replays a public event tape.
//
//   tape --(md_ns)--> strategy view of the book --> Strategy --> Gateway
//     |                                                             |
//     |                                                        (ingress_ns)
//     v                                                             v
//   venue book ---------------------------------------------> VirtualVenue
//                                                                   |
//   Gateway <------------------------(egress_ns)------------- reports
//
// Four time-ordered sources are merged; ties go to the earlier in this list:
//   1 tape event at the venue, 2 order message reaches the venue,
//   3 tape event reaches the strategy, 4 report reaches the strategy.
// Time only comes from the tape and the latencies, never from a clock, and
// jitter comes from a seeded generator, so a run is a pure function of its
// inputs: same tape, same config, same seed, same result bit for bit.
//
// After init() the loop allocates nothing: books, queues and statistics are
// all carved out of the arena up front.

#include <cstdint>
#include <cstdio>

#include "hft/bt/metrics.hpp"
#include "hft/bt/rng.hpp"
#include "hft/bt/tape_book.hpp"
#include "hft/bt/virtual_venue.hpp"
#include "hft/gw/gateway.hpp"
#include "hft/gw/order_entry.hpp"

namespace hft::bt {

struct BtConfig {
  std::uint64_t md_ns = 0;       // tape event -> strategy sees it
  std::uint64_t ingress_ns = 0;  // strategy sends -> venue receives
  std::uint64_t egress_ns = 0;   // venue acts -> strategy receives the report
  std::uint64_t jitter_ns = 0;   // added uniformly in [0, jitter] to each hop, FIFO preserved
  std::uint64_t seed = 1;
  std::uint32_t queue_penalty_pct = 0;  // see VirtualVenue
  std::int64_t fee_raw_per_unit = 0;
  std::uint64_t bin_ns = 1'000'000;
  risk::RiskConfig risk{};
  oms::OmsConfig oms{1u << 14};
  std::uint32_t max_resting = 1u << 16;  // tape book capacity
  std::uint32_t queue_capacity = 1u << 12;
  std::int64_t tick_raw = 100;
};

struct BtReport {
  // run
  std::uint64_t tape_events = 0, duration_ns = 0, digest = 0;
  // PnL, currency units
  double pnl = 0, edge = 0, inventory = 0, fees = 0, max_drawdown = 0, volume = 0;
  // activity
  std::uint64_t fills = 0, orders_accepted = 0, quotes = 0, quotes_filled = 0, sent_new = 0, sent_cancel = 0, sent_replace = 0,
                venue_rejects = 0, post_only_rejects = 0, risk_rejects = 0;
  std::int64_t filled_qty = 0, quoted_qty = 0, final_pos = 0, max_abs_pos = 0;
  double fill_rate_quotes = 0, fill_rate_qty = 0;
  // risk adjusted
  std::uint64_t bins = 0;
  double sharpe_per_bin = 0, sharpe_annualized = 0;
  // execution quality, ticks per share
  double effective_edge = 0;
  double realized[kHorizons] = {}, adverse[kHorizons] = {};
  std::uint64_t horizons_ns[kHorizons] = {};
  // health
  std::uint64_t queue_inconsistent = 0, oms_anomalies = 0, oms_mismatches = 0, queue_overflows = 0, tape_errors = 0, dropped_samples = 0,
                kill_reason = 0;
};

inline void print_report(const BtReport& r, std::FILE* f = stdout) {
  const double secs = static_cast<double>(r.duration_ns) * 1e-9;
  std::fprintf(f, "  tape            %llu events over %.3f s of market time\n", (unsigned long long)r.tape_events, secs);
  std::fprintf(f, "  PnL             %+.2f  (spread capture %+.2f, inventory %+.2f, fees %+.2f)\n", r.pnl, r.edge,
               r.inventory, r.fees);
  std::fprintf(f, "  volume          %llu fills, %lld shares, notional %.0f, max drawdown %.2f\n",
               (unsigned long long)r.fills, (long long)r.filled_qty, r.volume, r.max_drawdown);
  std::fprintf(f, "  Sharpe          %.2f per bin (%llu bins), annualised %.1f (6.5h x 252d, synthetic data, illustrative)\n",
               r.sharpe_per_bin, (unsigned long long)r.bins, r.sharpe_annualized);
  std::fprintf(f, "  fill rate       %.1f%% of quotes traded (%llu of %llu), %.1f%% of quoted shares\n",
               100 * r.fill_rate_quotes, (unsigned long long)r.quotes_filled, (unsigned long long)r.quotes,
               100 * r.fill_rate_qty);
  std::fprintf(f, "  position        final %+lld, max |pos| %lld\n", (long long)r.final_pos, (long long)r.max_abs_pos);
  std::fprintf(f, "  effective edge  %+.3f ticks/share at the fill\n", r.effective_edge);
  for (int h = 0; h < kHorizons; ++h)
    std::fprintf(f, "  after %6.0f us   realized spread %+.3f, post-fill move %+.3f ticks/share\n",
                 static_cast<double>(r.horizons_ns[h]) * 1e-3, r.realized[h], r.adverse[h]);
  std::fprintf(f, "  requests        %llu new, %llu replace, %llu cancel; venue rejects %llu (post-only crossed %llu), risk rejects %llu\n",
               (unsigned long long)r.sent_new, (unsigned long long)r.sent_replace, (unsigned long long)r.sent_cancel,
               (unsigned long long)r.venue_rejects, (unsigned long long)r.post_only_rejects,
               (unsigned long long)r.risk_rejects);
  std::fprintf(f, "  health          oms anomalies %llu, mismatches %llu, queue inconsistencies %llu, overflows %llu, tape errors %llu\n",
               (unsigned long long)r.oms_anomalies, (unsigned long long)r.oms_mismatches,
               (unsigned long long)r.queue_inconsistent, (unsigned long long)r.queue_overflows,
               (unsigned long long)r.tape_errors);
  std::fprintf(f, "  digest          %016llx\n", (unsigned long long)r.digest);
}

namespace detail {

struct Msg {
  std::uint64_t eff;
  std::uint32_t len;
  std::uint8_t data[gw::kMaxFrame];
};

// A one-way link with fixed latency plus seeded jitter. FIFO: a message never
// overtakes an earlier one.
struct Link {
  Msg* q = nullptr;
  std::uint32_t cap = 0;
  std::uint64_t head = 0, tail = 0;
  std::uint64_t last_eff = 0, base = 0, jitter = 0, overflow = 0;
  const std::uint64_t* now = nullptr;
  Rng rng{1};

  bool empty() const noexcept { return head == tail; }
  std::uint64_t front_eff() const noexcept { return q[head % cap].eff; }
  const Msg& front() const noexcept { return q[head % cap]; }
  void pop() noexcept { ++head; }

  void push(const std::uint8_t* d, std::size_t n) noexcept {
    if (tail - head >= cap || n > gw::kMaxFrame) {
      ++overflow;
      return;
    }
    std::uint64_t eff = *now + base + (jitter ? rng.below(static_cast<std::uint32_t>(jitter) + 1) : 0);
    if (eff < last_eff) eff = last_eff;
    last_eff = eff;
    Msg& m = q[tail % cap];
    m.eff = eff;
    m.len = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0; i < n; ++i) m.data[i] = d[i];
    ++tail;
  }
};

struct ClientSend {
  Link* link;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { link->push(d, n); }
};

}  // namespace detail

template <class Strategy>
class Backtester {
 public:
  using Gw = gw::Gateway<detail::ClientSend>;

  Backtester() : gw_(detail::ClientSend{&to_venue_}) {}

  bool init(core::Arena& arena, const sim::PublicEvent* tape, std::size_t n, const book::BookConfig& book_cfg,
            const BtConfig& cfg, Strategy* strat) noexcept {
    tape_ = tape;
    n_ = n;
    cfg_ = cfg;
    strat_ = strat;
    book::BookConfig bc = book_cfg;
    bc.max_orders = cfg.max_resting;
    if (!vbook_.init(arena, bc) || !sbook_.init(arena, bc)) return false;
    venue_.init(&vbook_, cfg.queue_penalty_pct);
    MetricsConfig mc;
    mc.tick_raw = book_cfg.tick_raw;
    mc.fee_raw_per_unit = cfg.fee_raw_per_unit;
    mc.bin_ns = cfg.bin_ns;
    mc.bins_per_year = 252.0 * 6.5 * 3600.0 * 1e9 / static_cast<double>(cfg.bin_ns);
    if (!metrics_.init(arena, mc)) return false;
    to_venue_.q = arena.allocate_array<detail::Msg>(cfg.queue_capacity);
    to_client_.q = arena.allocate_array<detail::Msg>(cfg.queue_capacity);
    if (!to_venue_.q || !to_client_.q) return false;
    to_venue_.cap = to_client_.cap = cfg.queue_capacity;
    to_venue_.now = to_client_.now = &now_;
    to_venue_.base = cfg.ingress_ns;
    to_client_.base = cfg.egress_ns;
    to_venue_.jitter = to_client_.jitter = cfg.jitter_ns;
    to_venue_.rng = Rng(cfg.seed * 2 + 1);
    to_client_.rng = Rng(cfg.seed * 2 + 2);
    return gw_.init(arena, cfg.oms, cfg.risk);
  }

  void run() noexcept {
    constexpr std::uint64_t kInf = ~std::uint64_t{0};
    VenueOut out{this};
    std::size_t iv = 0, is = 0;
    for (;;) {
      const std::uint64_t t1 = iv < n_ ? tape_[iv].ts : kInf;
      const std::uint64_t t2 = to_venue_.empty() ? kInf : to_venue_.front_eff();
      const std::uint64_t t3 = is < n_ ? tape_[is].ts + cfg_.md_ns : kInf;
      const std::uint64_t t4 = to_client_.empty() ? kInf : to_client_.front_eff();
      std::uint64_t t = t1;
      int which = 1;
      if (t2 < t) t = t2, which = 2;
      if (t3 < t) t = t3, which = 3;
      if (t4 < t) t = t4, which = 4;
      if (t == kInf) break;
      now_ = t;
      switch (which) {
        case 1: {
          const sim::PublicEvent& e = tape_[iv++];
          metrics_.before_event(t);
          venue_.on_tape(e, out);
          vbook_.apply(e);
          metrics_.on_mid(vbook_.two_sided(), vbook_.two_sided() ? vbook_.mid2() : 0);
          last_tape_ts_ = t;
          break;
        }
        case 2: {
          const detail::Msg& m = to_venue_.front();
          venue_message(m, out);
          to_venue_.pop();
          break;
        }
        case 3: {
          sbook_.apply(tape_[is++]);
          strat_->on_update(t, sbook_, gw_);
          break;
        }
        default: {
          const detail::Msg& m = to_client_.front();
          gw_.on_bytes(m.data, m.len, t);
          to_client_.pop();
          strat_->on_update(t, sbook_, gw_);
          break;
        }
      }
    }
    metrics_.before_event(last_tape_ts_);
    metrics_.finish(last_tape_ts_);
  }

  BtReport report() const noexcept {
    BtReport r;
    const Metrics& m = metrics_;
    r.tape_events = n_;
    r.duration_ns = n_ ? tape_[n_ - 1].ts - tape_[0].ts : 0;
    r.pnl = m.to_units(m.value2());
    r.edge = m.to_units(m.edge2());
    r.inventory = m.to_units(m.inventory2());
    r.fees = m.to_units(m.fees2());
    r.max_drawdown = m.max_drawdown_units();
    r.volume = m.notional_units();
    const VenueStats& v = venue_.stats();
    const gw::GatewayStats& g = gw_.stats();
    r.fills = m.fills();
    r.filled_qty = m.qty();
    r.orders_accepted = v.accepted;
    r.quotes = v.quotes;
    r.quotes_filled = v.quotes_filled;
    r.quoted_qty = static_cast<std::int64_t>(v.quoted_qty);
    r.fill_rate_quotes = v.quotes ? static_cast<double>(v.quotes_filled) / static_cast<double>(v.quotes) : 0;
    r.fill_rate_qty = v.quoted_qty ? static_cast<double>(v.filled_qty) / static_cast<double>(v.quoted_qty) : 0;
    r.sent_new = g.sent_new;
    r.sent_cancel = g.sent_cancel;
    r.sent_replace = g.sent_replace;
    r.venue_rejects = v.rejected + v.replace_rejected;
    r.post_only_rejects = v.wouldcross;
    r.risk_rejects = g.risk_rejects;
    r.final_pos = m.pos();
    r.max_abs_pos = m.max_abs_pos();
    r.bins = m.bins();
    r.sharpe_per_bin = m.sharpe_per_bin();
    r.sharpe_annualized = m.sharpe_annualized();
    r.effective_edge = m.effective_edge_ticks();
    for (int h = 0; h < kHorizons; ++h) {
      r.realized[h] = m.realized_ticks(h);
      r.adverse[h] = m.adverse_ticks(h);
      r.horizons_ns[h] = m.horizon_ns(h);
    }
    r.queue_inconsistent = v.queue_inconsistent;
    r.oms_anomalies = gw_.oms().stats().anomalies;
    r.oms_mismatches = gw_.oms().stats().mismatches;
    r.queue_overflows = to_venue_.overflow + to_client_.overflow;
    r.tape_errors = vbook_.errors() + sbook_.errors();
    r.dropped_samples = m.dropped_samples();
    r.kill_reason = static_cast<std::uint64_t>(gw_.risk().kill_reason());
    r.digest = digest(r);
    return r;
  }

  Gw& gateway() noexcept { return gw_; }
  const Gw& gateway() const noexcept { return gw_; }
  const Metrics& metrics() const noexcept { return metrics_; }
  const VirtualVenue& venue() const noexcept { return venue_; }
  const TapeBook& venue_book() const noexcept { return vbook_; }
  const TapeBook& strategy_book() const noexcept { return sbook_; }

 private:
  struct VenueOut {
    Backtester* b;
    void report(const sim::Report& r) noexcept { b->venue_report(r); }
  };

  void venue_report(const sim::Report& r) noexcept {
    if (r.type == sim::ReportType::Fill)
      metrics_.on_fill(r.ts, r.side == core::Side::Buy ? std::int8_t{1} : std::int8_t{-1}, r.price.raw(), r.qty);
    std::uint8_t buf[gw::kMaxFrame];
    const std::size_t n = gw::encode_report(buf, ++wire_seq_, r);
    to_client_.push(buf, n);
  }

  void venue_message(const detail::Msg& m, VenueOut& out) noexcept {
    gw::ClientMsg c;
    if (m.len < 2 || !gw::decode_client(m.data + 2, m.len - 2, c)) {
      ++bad_frames_;
      return;
    }
    switch (c.kind) {
      case gw::kMsgNew:
        venue_.on_new(sim::NewOrder{c.id, 1, c.side, c.otype, c.price, c.qty}, now_, out);
        break;
      case gw::kMsgCancel: venue_.on_cancel(c.id, now_, out); break;
      case gw::kMsgReplace: venue_.on_replace(c.id, c.price, c.qty, now_, out); break;
      default: ++bad_frames_; break;
    }
  }

  static std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h * 0x100000001B3ULL;
  }

  // Covers the whole result: the fill sequence (Metrics keeps its own digest of
  // time, price and signed quantity of every fill) and the integer outcomes.
  std::uint64_t digest(const BtReport& r) const noexcept {
    std::uint64_t h = metrics_.digest();
    const core::i128 v = metrics_.value2();
    h = mix(h, static_cast<std::uint64_t>(v));
    h = mix(h, static_cast<std::uint64_t>(v >> 64));
    h = mix(h, static_cast<std::uint64_t>(metrics_.edge2()));
    h = mix(h, static_cast<std::uint64_t>(metrics_.inventory2()));
    h = mix(h, r.fills);
    h = mix(h, r.sent_new);
    h = mix(h, r.sent_cancel);
    h = mix(h, r.sent_replace);
    h = mix(h, r.venue_rejects);
    h = mix(h, r.risk_rejects);
    h = mix(h, r.orders_accepted);
    h = mix(h, r.quotes_filled);
    return h;
  }

  const sim::PublicEvent* tape_ = nullptr;
  std::size_t n_ = 0;
  BtConfig cfg_{};
  Strategy* strat_ = nullptr;
  TapeBook vbook_, sbook_;
  VirtualVenue venue_;
  Metrics metrics_;
  detail::Link to_venue_, to_client_;
  Gw gw_;
  std::uint64_t now_ = 0, last_tape_ts_ = 0, wire_seq_ = 0, bad_frames_ = 0;
};

}  // namespace hft::bt
