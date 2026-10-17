#pragma once
// Client-side gateway: the one door between a strategy and the exchange.
//
//   strategy -> Gateway::new_order() -> risk -> OMS (PendingNew) -> wire encode -> Send
//   exchange bytes -> Gateway::on_bytes() -> frame reassembly -> sequence check -> OMS
//
// It owns the OMS and the risk engine so no order can reach the wire without
// passing risk and being recorded first. Cancels skip risk (always allowed).
//
// Kill switch: kill() latches the risk engine, then asks the exchange to
// cancel every order we have working. Orders still PendingNew cannot be
// cancelled until the exchange acknowledges them, so the sweep runs again
// after every inbound report while killed; nothing is left behind by latency.
// The gateway also trips the switch itself when it can no longer trust its
// picture of the world: a gap in the exchange's report sequence, a malformed
// report, or a corrupt byte stream. Resetting the switch is a human action.

#include <cstdint>

#include "hft/gw/order_entry.hpp"
#include "hft/oms/oms.hpp"
#include "hft/risk/risk.hpp"

namespace hft::gw {

using oms::OmsConfig;
using risk::KillReason;
using risk::RiskConfig;
using risk::RiskResult;

struct GatewayStats {
  std::uint64_t sent_new = 0, sent_cancel = 0, sent_replace = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t reports = 0;
  std::uint64_t seq_gaps = 0, seq_dups = 0, bad_messages = 0, broken_streams = 0;
  std::uint64_t kill_cancels = 0;  // cancels sent by the kill sweep
  std::uint64_t pulled_cancels = 0;  // cancels sent by cancel_all (quotes pulled, no kill)
};

struct SubmitResult {
  RiskResult risk = RiskResult::Ok;
  oms::Submit oms = oms::Submit::Ok;
  OrderId id = 0;
  bool ok() const noexcept { return risk == RiskResult::Ok && oms == oms::Submit::Ok; }
};

struct ReplaceResult {
  RiskResult risk = RiskResult::Ok;
  oms::Request req = oms::Request::Ok;
  bool ok() const noexcept { return risk == RiskResult::Ok && req == oms::Request::Ok; }
};

// Send needs: void operator()(const std::uint8_t* data, std::size_t len).
template <class Send>
class Gateway {
 public:
  explicit Gateway(Send send) noexcept : send_(send) {}

  bool init(core::Arena& arena, const OmsConfig& oc, const RiskConfig& rc, OrderId first_id = 1) noexcept {
    risk_.init(rc);
    next_id_ = first_id ? first_id : 1;
    return oms_.init(arena, oc);
  }

  SubmitResult new_order(Side side, OrderType type, Price price, Qty qty, std::uint64_t now) noexcept {
    SubmitResult r;
    r.risk = risk_.check_new(side, type, price, qty, oms_.position(), now);
    if (risk_.killed()) sweep(now, stats_.kill_cancels);  // the check itself can trip the switch (loss limit)
    if (r.risk != RiskResult::Ok) {
      ++stats_.risk_rejects;
      return r;
    }
    r.id = next_id_;
    r.oms = oms_.on_send_new(r.id, side, type, price, qty, now);
    if (r.oms != oms::Submit::Ok) return r;
    ++next_id_;
    ++stats_.sent_new;
    std::uint8_t buf[kMaxFrame];
    send_(buf, encode_new(buf, ++out_seq_, r.id, side, type, qty, type == OrderType::Market ? Price::from_raw(0) : price));
    return r;
  }

  // Closes (part of) the position: an IOC that the risk engine accepts only if it shrinks the position and nothing else is
  // working (RiskEngine::check_close). Works with the kill switch latched.
  SubmitResult close_order(Side side, Price price, Qty qty, std::uint64_t now) noexcept {
    SubmitResult r;
    r.risk = risk_.check_close(side, price, qty, oms_.position(), now);
    if (r.risk != RiskResult::Ok) {
      ++stats_.risk_rejects;
      return r;
    }
    r.id = next_id_;
    r.oms = oms_.on_send_new(r.id, side, OrderType::IOC, price, qty, now);
    if (r.oms != oms::Submit::Ok) return r;
    ++next_id_;
    ++stats_.sent_new;
    std::uint8_t buf[kMaxFrame];
    send_(buf, encode_new(buf, ++out_seq_, r.id, side, OrderType::IOC, qty, price));
    return r;
  }

  oms::Request cancel(OrderId id, std::uint64_t now) noexcept {
    const oms::Request q = oms_.on_request_cancel(id, now);
    if (q == oms::Request::Ok) send_cancel(id);
    return q;
  }

  ReplaceResult replace(OrderId id, Price price, Qty qty, std::uint64_t now) noexcept {
    ReplaceResult r;
    const oms::OrderRec* o = oms_.find(id);
    if (!o) {
      r.req = oms::Request::Unknown;
      return r;
    }
    // Ask risk first, but do not spend a token on a request the OMS will refuse anyway.
    if (!oms::is_workable(o->state)) {
      r.req = oms_.on_request_replace(id, price, qty, now);  // records the refusal
      return r;
    }
    r.risk = risk_.check_replace(o->side, price, qty, o->leaves, oms_.position(), now);
    if (risk_.killed()) sweep(now, stats_.kill_cancels);
    if (r.risk != RiskResult::Ok) {
      ++stats_.risk_rejects;
      return r;
    }
    r.req = oms_.on_request_replace(id, price, qty, now);
    if (r.req == oms::Request::Ok) {
      std::uint8_t buf[kMaxFrame];
      send_(buf, encode_replace(buf, ++out_seq_, id, qty, price));
      ++stats_.sent_replace;
    }
    return r;
  }

  void kill(KillReason why, std::uint64_t now) noexcept {
    risk_.kill(why);
    sweep(now, stats_.kill_cancels);
  }

  // Cancels everything cancellable, without latching the kill switch. For
  // pulling quotes when the market data can no longer be trusted. Orders still
  // PendingNew cannot be cancelled yet, so call again after later reports.
  // Safe to call repeatedly; does nothing when nothing is working.
  void cancel_all(std::uint64_t now) noexcept { sweep(now, stats_.pulled_cancels); }

  // Bytes from the exchange, in any chunking.
  void on_bytes(const std::uint8_t* data, std::size_t len, std::uint64_t now) noexcept {
    Handler h{this, now};
    if (decoder_.feed(data, len, h) == StreamDecoder::Status::Broken && !broken_noted_) {
      broken_noted_ = true;
      ++stats_.broken_streams;
      kill(KillReason::Disconnect, now);
    }
  }

  oms::Oms& oms() noexcept { return oms_; }
  const oms::Oms& oms() const noexcept { return oms_; }
  risk::RiskEngine& risk() noexcept { return risk_; }
  const risk::RiskEngine& risk() const noexcept { return risk_; }
  const GatewayStats& stats() const noexcept { return stats_; }

 private:
  struct Handler {
    Gateway* g;
    std::uint64_t now;
    void on_frame(const std::uint8_t* body, std::size_t len) noexcept { g->on_frame(body, len, now); }
  };

  void on_frame(const std::uint8_t* body, std::size_t len, std::uint64_t now) noexcept {
    Report rep;
    std::uint64_t seq = 0;
    if (!decode_report(body, len, rep, seq)) {
      ++stats_.bad_messages;
      kill(KillReason::SessionGap, now);  // we cannot tell what we just missed
      return;
    }
    if (seq < expect_in_) {  // a replay of something already applied: drop it
      ++stats_.seq_dups;
      return;
    }
    const bool gap = seq > expect_in_;
    expect_in_ = seq + 1;
    ++stats_.reports;
    oms_.on_report(rep);
    if (gap) {
      ++stats_.seq_gaps;
      risk_.kill(KillReason::SessionGap);
    }
    if (risk_.killed()) sweep(now, stats_.kill_cancels);
  }

  void send_cancel(OrderId id) noexcept {
    std::uint8_t buf[kMaxFrame];
    send_(buf, encode_cancel(buf, ++out_seq_, id));
    ++stats_.sent_cancel;
  }

  // Cancel everything cancellable. Safe to call repeatedly.
  void sweep(std::uint64_t now, std::uint64_t& counter) noexcept {
    // Collect first: requesting a cancel changes state but never the list, so
    // iterating while requesting is fine, and no allocation is needed.
    oms_.for_each_open([&](const oms::OrderRec& o) {
      if (oms::is_workable(o.state) && oms_.on_request_cancel(o.id, now) == oms::Request::Ok) {
        send_cancel(o.id);
        ++counter;
      }
    });
  }

  Send send_;
  oms::Oms oms_;
  risk::RiskEngine risk_;
  StreamDecoder decoder_;
  GatewayStats stats_;
  OrderId next_id_{1};
  std::uint64_t out_seq_{0}, expect_in_{1};
  bool broken_noted_{false};
};

}  // namespace hft::gw
