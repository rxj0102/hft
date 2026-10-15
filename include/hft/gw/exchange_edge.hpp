#pragma once
// Exchange-side adapter: speaks the order-entry protocol to clients and drives
// the ExchangeSim. It is the mirror image of Gateway and exists so the whole
// client path (risk -> OMS -> wire -> exchange -> wire -> OMS) can be tested
// in one process with real bytes in both directions.
//
// Bytes in:  on_client_bytes(participant, data, len, now) decodes frames and
//            submits to the simulator (which applies ingress latency).
// Bytes out: the simulator delivers private reports to on_report(); the edge
//            stamps a per-participant sequence number, encodes, and hands the
//            frame to `to_client(participant, data, len, delivered_at)`.
// Public market data events are ignored here; feed them to an ItchPublisher
// from the same sink if wanted.

#include <cstdint>
#include <new>

#include "hft/gw/order_entry.hpp"
#include "hft/sim/exchange_sim.hpp"

namespace hft::gw {

struct EdgeStats {
  std::uint64_t messages = 0, malformed = 0, seq_gaps = 0, seq_dups = 0, refused = 0, broken = 0, reports_out = 0;
};

// ToClient needs: void operator()(Owner, const uint8_t*, size_t, uint64_t delivered_at).
template <class ToClient>
class ExchangeEdge {
 public:
  ExchangeEdge(sim::ExchangeSim& sim, ToClient to_client) noexcept : sim_(sim), out_(to_client) {}

  bool init(core::Arena& arena, std::uint16_t max_participants) noexcept {
    n_ = max_participants;
    dec_ = arena.allocate_array<StreamDecoder>(n_);
    in_seq_ = arena.allocate_array<std::uint64_t>(n_);
    out_seq_ = arena.allocate_array<std::uint64_t>(n_);
    if (!dec_ || !in_seq_ || !out_seq_) return false;
    for (std::uint16_t i = 0; i < n_; ++i) {
      ::new (static_cast<void*>(&dec_[i])) StreamDecoder();
      in_seq_[i] = 1;
      out_seq_[i] = 0;
    }
    return true;
  }

  void on_client_bytes(sim::Owner p, const std::uint8_t* data, std::size_t len, std::uint64_t now) noexcept {
    if (p >= n_) return;
    Handler h{this, p, now};
    if (dec_[p].feed(data, len, h) == StreamDecoder::Status::Broken) ++stats_.broken;
  }

  // ExchangeSim sink interface.
  void on_report(sim::Owner p, const Report& r, std::uint64_t delivered_at) noexcept {
    if (p >= n_) return;
    std::uint8_t buf[kMaxFrame];
    const std::size_t n = encode_report(buf, ++out_seq_[p], r);
    ++stats_.reports_out;
    out_(p, buf, n, delivered_at);
  }
  void on_public(const sim::PublicEvent&, std::uint64_t) noexcept {}

  const EdgeStats& stats() const noexcept { return stats_; }

 private:
  struct Handler {
    ExchangeEdge* e;
    sim::Owner p;
    std::uint64_t now;
    void on_frame(const std::uint8_t* body, std::size_t len) noexcept { e->on_frame(p, body, len, now); }
  };

  void on_frame(sim::Owner p, const std::uint8_t* body, std::size_t len, std::uint64_t now) noexcept {
    ClientMsg m;
    if (!decode_client(body, len, m)) {
      ++stats_.malformed;
      return;
    }
    if (m.seq < in_seq_[p]) {
      ++stats_.seq_dups;
      return;
    }
    if (m.seq > in_seq_[p]) ++stats_.seq_gaps;
    in_seq_[p] = m.seq + 1;
    ++stats_.messages;
    bool ok = false;
    switch (m.kind) {
      case kMsgNew:
        ok = sim_.submit_new(p, NewOrder{m.id, p, m.side, m.otype, m.price, m.qty}, now);
        break;
      case kMsgCancel:
        ok = sim_.submit_cancel(p, m.id, now);
        break;
      case kMsgReplace:
        ok = sim_.submit_replace(p, m.id, m.price, m.qty, now);
        break;
      default:
        break;
    }
    if (!ok) ++stats_.refused;
  }

  sim::ExchangeSim& sim_;
  ToClient out_;
  std::uint16_t n_{0};
  StreamDecoder* dec_{nullptr};
  std::uint64_t* in_seq_{nullptr};
  std::uint64_t* out_seq_{nullptr};
  EdgeStats stats_;
};

}  // namespace hft::gw
