#pragma once
// Exchange simulator: a matching engine behind configurable network latency.
//
//   participant --ingress--> [engine] --egress--> participant   (private reports)
//                                     --md------> everyone      (public events)
//
// Everything is driven by caller-supplied timestamps, never a clock, so a run
// is bit-for-bit repeatable for a given seed. Latency per leg is
//   base + uniform[0, jitter]
// and each (participant, direction) link is FIFO, like a TCP session: jitter
// can delay a message but never lets it overtake an earlier one on the same
// link. Messages from DIFFERENT participants can and do race, which is the
// point of simulating latency. Ties on the same timestamp resolve by the
// order they were queued.
//
// Fixed capacity: the event queue is allocated at init(). A full queue makes
// submit_*() return false (back-pressure the caller can see). Output events
// cannot be refused, so overflow there is counted in dropped_out(); size the
// queue so the counter stays zero (the tests assert it).

#include <cstdint>

#include "hft/core/memory.hpp"
#include "hft/sim/matching_engine.hpp"

namespace hft::sim {

struct LatencyConfig {
  std::uint64_t ingress_ns = 0;     // participant -> exchange
  std::uint64_t egress_ns = 0;      // exchange -> participant (reports)
  std::uint64_t md_ns = 0;          // exchange -> market data subscribers
  std::uint64_t processing_ns = 0;  // added to the engine timestamp of every request
  std::uint64_t jitter_ns = 0;      // uniform [0, jitter] added to each leg
  std::uint64_t seed = 1;
};

struct SimConfig {
  EngineConfig engine;
  LatencyConfig latency;
  std::uint32_t queue_capacity = 1u << 14;
  std::uint16_t max_participants = 64;
};

struct Request {
  enum Kind : std::uint8_t { New, Cancel, Replace } kind;
  Owner participant;
  NewOrder order;  // New
  OrderId id;      // Cancel / Replace
  Price price;     // Replace
  Qty qty;         // Replace
};

namespace detail {

struct Event {
  enum Kind : std::uint8_t { Ingress, ReportOut, PublicOut } kind;
  Owner participant;
  std::uint64_t time;
  std::uint64_t seq;
  union Payload {
    Request req;
    Report rep;
    PublicEvent pub;
    Payload() noexcept : req{} {}
  } u;
};

// Binary min-heap on (time, seq). seq makes ordering total and deterministic.
class EventQueue {
 public:
  bool init(core::Arena& a, std::uint32_t cap) noexcept {
    heap_ = a.allocate_array<Event>(cap);
    cap_ = cap;
    n_ = 0;
    return heap_ != nullptr;
  }
  bool push(const Event& e) noexcept {
    if (n_ == cap_) return false;
    std::uint32_t i = n_++;
    while (i > 0) {
      const std::uint32_t p = (i - 1) / 2;
      if (!less(e, heap_[p])) break;
      heap_[i] = heap_[p];
      i = p;
    }
    heap_[i] = e;
    return true;
  }
  bool empty() const noexcept { return n_ == 0; }
  const Event& top() const noexcept { return heap_[0]; }
  Event pop() noexcept {
    const Event out = heap_[0];
    const Event last = heap_[--n_];
    if (n_ > 0) {
      std::uint32_t i = 0;
      for (;;) {
        std::uint32_t c = 2 * i + 1;
        if (c >= n_) break;
        if (c + 1 < n_ && less(heap_[c + 1], heap_[c])) ++c;
        if (!less(heap_[c], last)) break;
        heap_[i] = heap_[c];
        i = c;
      }
      heap_[i] = last;
    }
    return out;
  }
  std::uint32_t size() const noexcept { return n_; }

 private:
  static bool less(const Event& a, const Event& b) noexcept {
    return a.time != b.time ? a.time < b.time : a.seq < b.seq;
  }
  Event* heap_{nullptr};
  std::uint32_t cap_{0}, n_{0};
};

struct SplitMix {
  std::uint64_t s;
  std::uint64_t next() noexcept {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
};

// Owns the queue and the latency model; the engine's listener forwards here.
struct Router {
  EventQueue q;
  LatencyConfig lat{};
  SplitMix rng{1};
  std::uint16_t nparts{0};
  std::uint64_t* last_in{nullptr};
  std::uint64_t* last_out{nullptr};
  std::uint64_t last_md{0};
  std::uint64_t seq{0};
  std::uint64_t now{0};  // engine time of the request being processed
  std::uint64_t dropped_out{0};

  std::uint64_t jitter() noexcept { return lat.jitter_ns ? rng.next() % (lat.jitter_ns + 1) : 0; }

  void on_report(const Report& r) noexcept {
    std::uint64_t t = now + lat.egress_ns + jitter();
    if (r.owner < nparts) {
      if (t < last_out[r.owner]) t = last_out[r.owner];
      last_out[r.owner] = t;
    }
    Event e;
    e.kind = Event::ReportOut;
    e.participant = r.owner;
    e.time = t;
    e.seq = ++seq;
    e.u.rep = r;
    if (!q.push(e)) ++dropped_out;
  }

  void on_public(const PublicEvent& p) noexcept {
    std::uint64_t t = now + lat.md_ns + jitter();
    if (t < last_md) t = last_md;
    last_md = t;
    Event e;
    e.kind = Event::PublicOut;
    e.participant = 0;
    e.time = t;
    e.seq = ++seq;
    e.u.pub = p;
    if (!q.push(e)) ++dropped_out;
  }
};

struct Tap {
  Router* r{nullptr};
  void on_report(const Report& x) noexcept { r->on_report(x); }
  void on_public(const PublicEvent& x) noexcept { r->on_public(x); }
};

}  // namespace detail

class ExchangeSim {
 public:
  bool init(core::Arena& arena, const SimConfig& cfg) noexcept {
    r_.lat = cfg.latency;
    r_.rng = detail::SplitMix{cfg.latency.seed};
    r_.nparts = cfg.max_participants;
    r_.last_in = arena.allocate_array<std::uint64_t>(cfg.max_participants);
    r_.last_out = arena.allocate_array<std::uint64_t>(cfg.max_participants);
    if (!r_.last_in || !r_.last_out) return false;
    for (std::uint16_t i = 0; i < cfg.max_participants; ++i) r_.last_in[i] = r_.last_out[i] = 0;
    if (!r_.q.init(arena, cfg.queue_capacity)) return false;
    tap_.r = &r_;
    return engine_.init(arena, cfg.engine, tap_);
  }

  // Submitters. `now` is the participant's send time and must not be earlier
  // than the last advance_to() time. Return false if refused (unknown
  // participant, stale time, or queue full); nothing is queued in that case.
  bool submit_new(Owner p, NewOrder o, std::uint64_t now) noexcept {
    o.owner = p;
    Request rq{};
    rq.kind = Request::New;
    rq.participant = p;
    rq.order = o;
    return enqueue(rq, now);
  }
  bool submit_cancel(Owner p, OrderId id, std::uint64_t now) noexcept {
    Request rq{};
    rq.kind = Request::Cancel;
    rq.participant = p;
    rq.id = id;
    return enqueue(rq, now);
  }
  bool submit_replace(Owner p, OrderId id, Price price, Qty qty, std::uint64_t now) noexcept {
    Request rq{};
    rq.kind = Request::Replace;
    rq.participant = p;
    rq.id = id;
    rq.price = price;
    rq.qty = qty;
    return enqueue(rq, now);
  }

  // Runs the simulation up to and including time `t`: requests arrive and are
  // matched, and outputs due by `t` are delivered, all in time order.
  // Sink needs: on_report(Owner, const Report&, u64 delivered_at) and
  // on_public(const PublicEvent&, u64 delivered_at). Returns events delivered.
  template <class Sink>
  std::uint64_t advance_to(std::uint64_t t, Sink& sink) noexcept {
    if (t > clock_) clock_ = t;
    std::uint64_t delivered = 0;
    while (!r_.q.empty() && r_.q.top().time <= t) {
      const detail::Event e = r_.q.pop();
      switch (e.kind) {
        case detail::Event::Ingress:
          process(e.u.req, e.time);
          break;
        case detail::Event::ReportOut:
          sink.on_report(e.participant, e.u.rep, e.time);
          ++delivered;
          break;
        case detail::Event::PublicOut:
          sink.on_public(e.u.pub, e.time);
          ++delivered;
          break;
      }
    }
    return delivered;
  }

  std::uint64_t pending() const noexcept { return r_.q.size(); }
  std::uint64_t dropped_out() const noexcept { return r_.dropped_out; }
  const MatchingEngine<detail::Tap>& engine() const noexcept { return engine_; }

 private:
  bool enqueue(const Request& rq, std::uint64_t now) noexcept {
    if (rq.participant >= r_.nparts || now < clock_) return false;
    std::uint64_t t = now + r_.lat.ingress_ns + r_.jitter();
    if (t < r_.last_in[rq.participant]) t = r_.last_in[rq.participant];
    detail::Event e;
    e.kind = detail::Event::Ingress;
    e.participant = rq.participant;
    e.time = t;
    e.seq = r_.seq + 1;
    e.u.req = rq;
    if (!r_.q.push(e)) return false;
    ++r_.seq;
    r_.last_in[rq.participant] = t;
    return true;
  }

  void process(const Request& rq, std::uint64_t arrival) noexcept {
    const std::uint64_t ts = arrival + r_.lat.processing_ns;
    r_.now = ts;
    switch (rq.kind) {
      case Request::New:
        engine_.new_order(rq.order, ts);
        break;
      case Request::Cancel:
        engine_.cancel(rq.id, rq.participant, ts);
        break;
      case Request::Replace:
        engine_.replace(rq.id, rq.participant, rq.price, rq.qty, ts);
        break;
    }
  }

  detail::Router r_;
  detail::Tap tap_;
  MatchingEngine<detail::Tap> engine_;
  std::uint64_t clock_{0};
};

}  // namespace hft::sim
