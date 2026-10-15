// Phase 7 tests: the assembled tick-to-trade pipeline.
//   - replaying a recorded session through the production pipeline puts the same
//     bytes on the wire as the backtest did (the oracle behind every latency number);
//   - zero allocations; the measurement probe changes nothing;
//   - the second feed line is harmless; a gap the other line closes is not a loss;
//   - a lost book pulls the quotes, nothing new is sent until a snapshot arrives,
//     and quoting resumes afterwards.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "alloc_trap.hpp"
#include "check.hpp"
#include "hft/t2t/probe.hpp"
#include "hft/t2t/session.hpp"
#include "snapshot_builder.hpp"

using namespace hft;
using namespace hft::t2t;

namespace {

core::Region make_region(std::size_t mb) {
  return core::Region::create(mb << 20, core::RegionOptions{false, false, true, -1});
}

risk::RiskConfig test_risk() {
  risk::RiskConfig r;
  r.max_order_qty = 100;
  r.max_order_notional_raw = 100'000'000'000LL;
  r.price_band_bps = 2000;
  r.max_position = 80;
  r.max_open_orders = 10;
  r.rate_per_sec = 1'000'000;
  r.burst = 50;
  r.max_loss_raw = 0;
  return r;
}

bt::MarketConfig market_cfg(std::uint64_t requests, std::uint64_t seed) {
  bt::MarketConfig c;
  c.requests = requests;
  c.seed = seed;
  return c;
}

bt::BtConfig bt_cfg(std::uint64_t md, std::uint64_t in, std::uint64_t eg, std::uint64_t jit) {
  bt::BtConfig c;
  c.md_ns = md;
  c.ingress_ns = in;
  c.egress_ns = eg;
  c.jitter_ns = jit;
  c.seed = 7;
  return c;
}

// The sink the pipeline sends to: hashes and counts, remembers frame types.
struct Sink {
  std::uint64_t hash = kHashSeed, frames = 0, bytes = 0;
  std::vector<std::uint8_t>* types = nullptr;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    hash = frame_hash(hash, d, n);
    ++frames;
    bytes += n;
    if (types) types->push_back(d[2]);
  }
};
struct SinkRef {
  Sink* s;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { (*s)(d, n); }
};
using Pipe = Pipeline<SinkRef>;

void replay_one(Pipe& p, const Session& s, const Input& in, md::Line line = md::Line::A) {
  std::size_t n = 0;
  if (in.kind == Input::Md) {
    const std::uint8_t* d = s.packet(in.idx, n);
    p.on_packet(line, d, n, in.now);
  } else {
    const std::uint8_t* d = s.report(in.idx, n);
    p.on_report(d, n, in.now);
  }
}

// ---- replay equals the backtest -------------------------------------------------------------

void check_matches_backtest(const Session& s, const Pipe& p, const Sink& k) {
  const gw::GatewayStats& g = p.gateway().stats();
  CHECK(k.hash == s.wire_hash);
  CHECK(k.frames == s.frames && k.bytes == s.wire_bytes);
  CHECK(g.sent_new == s.sent_new && g.sent_replace == s.sent_replace && g.sent_cancel == s.sent_cancel);
  CHECK(g.risk_rejects == s.risk_rejects);
  CHECK(p.gateway().oms().position().pos == s.final_pos);
  CHECK(p.book().digest() == s.book_digest);
  CHECK(p.book().check_invariants());
  const md::MdStats& m = p.feed().stats();
  CHECK(m.malformed == 0 && m.desyncs == 0 && m.book_errors == 0 && m.gaps_declared == 0 && m.parked == 0);
  CHECK(m.dups[0] == 0 && m.dups[1] == 0 && m.messages == s.md_inputs());
  CHECK(p.stats().feed_losses == 0 && p.stats().pull_sweeps == 0);
  CHECK(g.seq_gaps == 0 && g.bad_messages == 0 && g.broken_streams == 0);
  CHECK(p.gateway().oms().stats().anomalies == 0 && p.gateway().oms().stats().mismatches == 0);
  CHECK(!p.gateway().risk().killed());
}

void test_replay_equals_backtest(std::uint64_t requests) {
  struct Case {
    std::uint64_t seed, md, in, eg, jit;
  } cases[] = {{11, 5000, 10000, 10000, 2000}, {12, 0, 0, 0, 0}, {13, 30000, 60000, 60000, 5000}};
  for (const Case& c : cases) {
    const bt::Market m = bt::MarketGen::generate(market_cfg(requests, c.seed));
    Session s;
    CHECK(record_session(m, bt_cfg(c.md, c.in, c.eg, c.jit), strat::MmConfig{}, test_risk(), s));
    CHECK(s.md_inputs() == m.tape.size());
    if (!(s.frames > 200 && s.sent_replace > 100 && s.sent_new > 4))
      std::fprintf(stderr, "case seed %llu: frames %llu new %llu replace %llu cancel %llu\n", (unsigned long long)c.seed,
                   (unsigned long long)s.frames, (unsigned long long)s.sent_new, (unsigned long long)s.sent_replace,
                   (unsigned long long)s.sent_cancel);
    CHECK(s.frames > 200 && s.sent_replace > 100 && s.sent_new > 4 && s.sent_cancel + 1 > 0);  // it really trades
    core::Region region = make_region(96);
    core::Arena arena(region);
    Sink k;
    auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
    CHECK(p->init(arena, s.cfg));
    for (const Input& in : s.inputs) replay_one(*p, s, in);
    check_matches_backtest(s, *p, k);
    delete p;
  }
}

// The probe is a pure forwarder: same bytes, and it stamps at most once per tick.
void test_probe_is_transparent(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 21));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  core::Region region = make_region(96);
  core::Arena arena(region);
  Sink k;
  auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
  CHECK(p->init(arena, s.cfg));
  std::uint64_t stamped = 0, acted = 0;
  for (const Input& in : s.inputs) {
    if (in.kind == Input::Report) {
      replay_one(*p, s, in);
      continue;
    }
    std::size_t n = 0;
    const std::uint8_t* d = s.packet(in.idx, n);
    const std::uint64_t f0 = k.frames;
    StageStamp st;
    if (p->ingest(md::Line::A, d, n, in.now)) {
      ProbedGateway<gw::Gateway<SinkRef>> pg(p->gateway(), st);
      p->decide(in.now, pg);
    }
    if (k.frames != f0) ++acted;
    stamped += st.t_enter != 0;
    CHECK((st.t_enter != 0) == (k.frames != f0));  // stamped exactly when something was sent
  }
  check_matches_backtest(s, *p, k);
  CHECK(stamped == acted && acted > 50);
  delete p;
}

void test_no_allocation(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 31));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  core::Region region = make_region(96);
  core::Arena arena(region);
  Sink k;
  auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
  CHECK(p->init(arena, s.cfg));
  alloc_trap::arm();
  for (const Input& in : s.inputs) replay_one(*p, s, in);
  CHECK(alloc_trap::disarm() == 0);
  check_matches_backtest(s, *p, k);
  delete p;
}

// ---- the second line -------------------------------------------------------------------------

void test_line_b_duplicates(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 41));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  core::Region region = make_region(96);
  core::Arena arena(region);
  Sink k;
  auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
  CHECK(p->init(arena, s.cfg));
  std::uint64_t md_in = 0;
  for (const Input& in : s.inputs) {
    replay_one(*p, s, in);
    if (in.kind == Input::Md) {
      replay_one(*p, s, in, md::Line::B);  // the same packet again, from the other line
      ++md_in;
    }
  }
  CHECK(k.hash == s.wire_hash && k.frames == s.frames);
  CHECK(p->feed().stats().dups[1] == md_in && p->feed().stats().dups[0] == 0);
  CHECK(p->book().digest() == s.book_digest);
  delete p;
}

// A packet lost on line A but delivered by line B is not a loss: nothing is declared,
// nothing is pulled, and the book ends up exactly right.
void test_gap_closed_by_other_line(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 51));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  core::Region region = make_region(96);
  core::Arena arena(region);
  Sink k;
  auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
  CHECK(p->init(arena, s.cfg));
  std::vector<Input> md_in;
  for (const Input& in : s.inputs)
    if (in.kind == Input::Md) md_in.push_back(in);
  // Feed only the market data (no reports): the book is what is under test here.
  std::size_t lost = 0;
  for (std::size_t i = 0; i < md_in.size(); ++i) {
    std::size_t n = 0;
    const std::uint8_t* d = s.packet(md_in[i].idx, n);
    if (i % 97 == 50 && i + 1 < md_in.size()) {
      // line A drops packet i; packet i+1 arrives first on A and is parked; B then delivers i.
      std::size_t n2 = 0;
      const std::uint8_t* d2 = s.packet(md_in[i + 1].idx, n2);
      p->on_packet(md::Line::A, d2, n2, md_in[i + 1].now);
      p->on_packet(md::Line::B, d, n, md_in[i].now);
      ++i;
      ++lost;
    } else {
      p->on_packet(md::Line::A, d, n, md_in[i].now);
    }
  }
  CHECK(lost > 5);
  CHECK(p->feed().stats().gaps_filled >= lost && p->feed().stats().gaps_declared == 0);
  CHECK(p->stats().feed_losses == 0 && p->stats().pull_sweeps == 0);
  CHECK(p->book().digest() == s.book_digest && p->feed().book_valid());
  delete p;
}

// ---- losing the book ----------------------------------------------------------------------------

bool all_acked(const Pipe& p, std::uint32_t& workable) {
  workable = 0;
  bool pending = false;
  const_cast<oms::Oms&>(p.gateway().oms()).for_each_open([&](const oms::OrderRec& o) {
    if (oms::is_workable(o.state)) ++workable;
    else pending = true;
  });
  return !pending;
}

void test_feed_loss_pulls_quotes_and_recovers(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 61));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  core::Region region = make_region(96);
  core::Arena arena(region);
  std::vector<std::uint8_t> types;
  Sink k;
  k.types = &types;
  auto* p = new Pipe(SinkRef{&k}, s.cfg.mm);
  CHECK(p->init(arena, s.cfg));

  // Replay until two quotes are working and acknowledged (so they can be cancelled).
  std::size_t at = 0;
  std::uint32_t workable = 0;
  std::uint64_t reports_fed = 0, last_now = 0;
  for (; at < s.inputs.size(); ++at) {
    const Input& in = s.inputs[at];
    replay_one(*p, s, in);
    last_now = in.now;
    if (in.kind == Input::Report) ++reports_fed;
    if (at > 2000 && in.kind == Input::Report && all_acked(*p, workable) && workable >= 2) {
      ++at;
      break;
    }
  }
  CHECK(workable >= 2 && at < s.inputs.size());
  const std::uint32_t working_at_loss = workable;

  // Skip the next market data packet (the loss), deliver the one after it.
  std::size_t lost_at = at;
  while (lost_at < s.inputs.size() && s.inputs[lost_at].kind != Input::Md) ++lost_at;
  std::size_t next_md = lost_at + 1;
  while (next_md < s.inputs.size() && s.inputs[next_md].kind != Input::Md) ++next_md;
  CHECK(next_md + 60 < s.inputs.size());
  // Reports between `at` and the loss still arrive (they belong to the exchange link).
  for (std::size_t i = at; i < lost_at; ++i) {
    replay_one(*p, s, s.inputs[i]);
    ++reports_fed;
  }
  std::size_t n = 0;
  const std::uint8_t* d = s.packet(s.inputs[next_md].idx, n);
  const std::uint64_t frames_before = k.frames;
  p->on_packet(md::Line::A, d, n, s.inputs[next_md].now);  // ahead of the expected number: parked
  CHECK(p->feed().stats().parked == 1 && p->feed().gap_pending() && p->feed().book_valid());
  CHECK(p->stats().feed_losses == 0 && k.frames == frames_before);

  // The other line never delivers it. After the timeout the gap is declared.
  std::uint32_t working_now = 0;
  all_acked(*p, working_now);
  const std::uint64_t t_loss = s.inputs[next_md].now + s.cfg.md.gap_timeout_ns + 1;
  p->poll(t_loss);
  CHECK(!p->feed().book_valid() && p->stats().feed_losses == 1 && p->stats().pull_sweeps >= 1);
  // Every working order got a cancel, and nothing else was sent.
  CHECK(k.frames - frames_before == working_now && working_now >= 1 && working_now <= working_at_loss + 2);
  for (std::size_t i = frames_before; i < types.size(); ++i) CHECK(types[i] == gw::kMsgCancel);
  CHECK(p->gateway().stats().kill_cancels == 0 && p->gateway().stats().pulled_cancels == working_now &&
        !p->gateway().risk().killed());  // pulled, not killed

  // The sweep is idempotent: asking again while the cancels are in flight sends nothing.
  const std::uint64_t frames_after_pull = k.frames;
  for (int i = 0; i < 3; ++i) p->decide(t_loss + 1 + static_cast<std::uint64_t>(i));
  CHECK(k.frames == frames_after_pull);

  // More market data keeps arriving; the book is untrusted, so nothing new may go out.
  std::size_t last_md = next_md;
  for (std::size_t i = next_md + 1; i < next_md + 40; ++i) {
    if (s.inputs[i].kind != Input::Md) continue;
    replay_one(*p, s, s.inputs[i]);
    last_md = i;
  }
  CHECK(k.frames == frames_after_pull);
  CHECK(p->feed().stats().parked >= 2);

  // The exchange confirms the cancels.
  std::uint64_t rseq = reports_fed + 1;
  std::vector<std::pair<core::OrderId, std::pair<core::Side, std::pair<core::Price, core::Qty>>>> pend;
  const_cast<oms::Oms&>(p->gateway().oms()).for_each_open([&](const oms::OrderRec& o) {
    if (o.state == oms::State::PendingCancel) pend.push_back({o.id, {o.side, {o.price, o.leaves}}});
  });
  CHECK(pend.size() == working_now);
  for (const auto& e : pend) {
    sim::Report r{};
    r.type = sim::ReportType::Canceled;
    r.owner = 1;
    r.id = e.first;
    r.side = e.second.first;
    r.price = e.second.second.first;
    r.qty = e.second.second.second;
    r.leaves = 0;
    r.seq = 1;
    r.ts = t_loss + 100;
    std::uint8_t buf[gw::kMaxFrame];
    const std::size_t len = gw::encode_report(buf, rseq++, r);
    p->on_report(buf, len, t_loss + 100);
  }
  CHECK(p->gateway().oms().position().open_orders == 0);
  CHECK(k.frames == frames_after_pull);  // still waiting for a trustworthy book
  CHECK(!p->gateway().risk().killed() && p->gateway().oms().stats().anomalies == 0);

  // A snapshot of the true book as of the last packet delivered brings the feed back.
  testsupport::RefBook ref(s.cfg.book);
  const std::uint32_t last_pkt = s.inputs[last_md].idx;
  for (std::uint32_t i = 0; i <= last_pkt; ++i) {
    const sim::PublicEvent& e = m.tape[i];
    switch (e.type) {
      case sim::PublicType::Add: ref.add(e.id, e.side, e.price, e.qty); break;
      case sim::PublicType::Exec: ref.execute(e.id, e.qty); break;
      case sim::PublicType::Reduce: {
        core::Side sd = core::Side::Buy;
        std::int64_t px = 0;
        core::Qty q = 0;
        CHECK(ref.lookup(e.id, sd, px, q));
        ref.replace(e.id, core::Price::from_raw(px), q - e.qty);
        break;
      }
      case sim::PublicType::Delete: ref.cancel(e.id); break;
    }
  }
  std::uint64_t stream_seq = 1;
  const auto snap = testsupport::build_snapshot(ref, last_pkt + 1ULL, stream_seq, 1);
  for (std::size_t i = 0; i < snap.size(); ++i) p->on_snapshot(snap[i].data(), snap[i].size(), t_loss + 200);
  CHECK(p->feed().book_valid() && p->feed().stats().snapshots_applied == 1);
  CHECK(p->book().digest() == ref.digest());
  // Quoting resumed by itself: fresh orders, no kill, no human.
  CHECK(k.frames > frames_after_pull);
  for (std::size_t i = frames_after_pull; i < types.size(); ++i) CHECK(types[i] == gw::kMsgNew);
  CHECK(!p->gateway().risk().killed());
  (void)last_now;
  delete p;
}

// A book that is too small for the instrument must fail safe: the first order that does not
// fit is a desync, the quotes are pulled, and nothing is sent on the untrusted book after that.
struct GuardedSink {
  Sink* inner;
  const md::MdHandler<hft::t2t::detail::FeedWatch>* feed;
  std::uint64_t bad = 0, frames_while_lost = 0;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    (*inner)(d, n);
    if (!feed->book_valid()) {
      ++frames_while_lost;
      if (d[2] == gw::kMsgNew || d[2] == gw::kMsgReplace) ++bad;
    }
  }
};
struct GuardedRef {
  GuardedSink* g;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { (*g)(d, n); }
};

void test_book_capacity_exceeded(std::uint64_t requests) {
  const bt::Market m = bt::MarketGen::generate(market_cfg(requests, 71));
  Session s;
  CHECK(record_session(m, bt_cfg(5000, 10000, 10000, 2000), strat::MmConfig{}, test_risk(), s));
  s.cfg.book.max_orders = 48;  // the session peaks at several hundred resting orders
  core::Region region = make_region(96);
  core::Arena arena(region);
  Sink k;
  GuardedSink g{&k, nullptr};
  auto* p = new Pipeline<GuardedRef>(GuardedRef{&g}, s.cfg.mm);
  g.feed = &p->feed();
  CHECK(p->init(arena, s.cfg));
  for (const Input& in : s.inputs) {
    std::size_t n = 0;
    if (in.kind == Input::Md) {
      const std::uint8_t* d = s.packet(in.idx, n);
      p->on_packet(md::Line::A, d, n, in.now);
    } else {
      const std::uint8_t* d = s.report(in.idx, n);
      p->on_report(d, n, in.now);
    }
  }
  CHECK(p->stats().feed_losses == 1);                       // lost once, and no snapshot ever came
  CHECK(!p->feed().book_valid() && p->feed().stats().book_errors >= 1);
  CHECK(g.bad == 0);                                         // never a new order or replace on the untrusted book
  CHECK(p->book().check_invariants() && p->book().order_count() <= 48);
  CHECK(p->gateway().stats().pulled_cancels >= 1);          // the quotes were pulled
  CHECK(!p->gateway().risk().killed());
  delete p;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000ULL;
  test_replay_equals_backtest(std::max<std::uint64_t>(n / 2, 20000));
  test_probe_is_transparent(std::max<std::uint64_t>(n / 2, 20000));
  test_no_allocation(std::max<std::uint64_t>(n / 2, 20000));
  test_line_b_duplicates(std::max<std::uint64_t>(n / 4, 20000));
  test_gap_closed_by_other_line(std::max<std::uint64_t>(n / 4, 20000));
  test_feed_loss_pulls_quotes_and_recovers(std::max<std::uint64_t>(n / 2, 20000));
  test_book_capacity_exceeded(std::max<std::uint64_t>(n / 4, 20000));
  if (g_failures == 0) std::puts("test_t2t: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
