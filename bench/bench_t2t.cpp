// Phase 7: end-to-end tick-to-trade latency.
//
// What is timed: a feed packet arrives (already in memory, as if the NIC had
// written it) and the clock starts; the clock stops when the order frame that
// the packet caused has been copied into the transmit staging buffer. Between
// the two sit the real production components, nothing stubbed:
//   MdHandler (A/B arbitration, decode) -> OrderBook -> MarketMaker
//   -> Gateway (pre-trade risk -> OMS -> wire encode) -> staging copy.
// The network card, the kernel and the exchange are outside the number, and
// the report says so.
//
// Input is a recorded backtest session, so the work per tick is the real mix
// of market data events and exchange reports a strategy sees, and every pass
// is checked against the backtest byte for byte before its numbers are used.
//
// Passes (each on a fresh pipeline):
//   verify  untimed, hashes every frame, must equal the backtest's wire hash
//   total   only two probes in the path (packet in, frame out): the headline
//   staged  adds probes between stages to split the time (the probes are not free; the run measures their cost)
//   cold    like total, but L1 and L2 are flushed before every tick
//   speed   no probes at all: sustained ticks per second
// Also measured: the timer overhead (subtracted) and the platform's own jitter
// (a spin loop that does nothing), so a tail is read against the noise floor.
//
//   bench_t2t [--requests N] [--cpu C] [--cold-ticks N] [--json file]
//             [--max-orders N]          book capacity (resting orders) for the pipeline
//             [--workload N]            replay N inputs once, for callgrind
//             [--profile file --seconds S]   sample the replay for a flame graph
#include <sched.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hft/core/clock.hpp"
#include "hft/core/histogram.hpp"
#include "hft/t2t/probe.hpp"
#include "hft/t2t/session.hpp"
#include "sampler.hpp"

using namespace hft;
using hft::core::Histogram;
using hft::core::rdtsc_ordered;
using namespace hft::t2t;

namespace {

template <class T>
inline void do_not_optimize(T const& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

risk::RiskConfig bench_risk() {
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

// ---- the wire: where the order frame lands ----------------------------------------------------

// Stands in for the NIC transmit ring: the frame is copied into the next 128 byte
// slot, which is what a kernel-bypass send does before it rings the doorbell.
// With Stamp, the first frame of a tick also reads the clock, right after the copy.
template <bool Stamp>
struct Tx {
  alignas(64) std::uint8_t ring[1u << 16];
  std::uint32_t pos = 0, frames = 0;
  std::uint64_t t_first = 0;
  std::uint8_t kind = 0;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    std::memcpy(ring + pos, d, n);
    pos = (pos + 128) & ((1u << 16) - 1);
    if constexpr (Stamp) {
      if (t_first == 0) {
        t_first = rdtsc_ordered();
        kind = d[2];
      }
    }
    ++frames;
  }
};
template <bool Stamp>
struct TxRef {
  Tx<Stamp>* tx;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { (*tx)(d, n); }
};

// Frame hash for the verify pass.
struct HashTx {
  std::uint64_t hash = kHashSeed, frames = 0, bytes = 0;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept {
    hash = frame_hash(hash, d, n);
    ++frames;
    bytes += n;
  }
};
struct HashRef {
  HashTx* tx;
  void operator()(const std::uint8_t* d, std::size_t n) noexcept { (*tx)(d, n); }
};

// ---- setup ------------------------------------------------------------------------------------------

struct Rig {
  core::Region region;
  std::unique_ptr<core::Arena> arena;
};

core::Region make_region() {
  return core::Region::create(48u << 20, core::RegionOptions{true, true, true, -1});
}

template <class Pipe>
bool init_pipe(Pipe& p, core::Region& region, const Session& s) {
  core::Arena arena(region);
  return p.init(arena, s.cfg);
}

bool matches_backtest(const Session& s, const gw::GatewayStats& g, std::int64_t pos, std::uint64_t digest, const md::MdStats& m,
                      bool killed) {
  return g.sent_new == s.sent_new && g.sent_replace == s.sent_replace && g.sent_cancel == s.sent_cancel &&
         g.risk_rejects == s.risk_rejects && pos == s.final_pos && digest == s.book_digest && m.messages == s.md_inputs() &&
         m.desyncs == 0 && m.gaps_declared == 0 && !killed;
}

// ---- the platform ------------------------------------------------------------------------------------

struct Platform {
  double ticks_per_ns = 1;
  std::uint64_t floor_ticks = 0;  // back-to-back rdtsc_ordered pair, p50
  Histogram hiccups;              // gaps in a do-nothing spin loop
  std::uint64_t spin_ns = 0;
  std::uint64_t gt1us = 0, gt5us = 0, gt20us = 0, gt100us = 0;
  std::uint64_t max_gap_ticks = 0;
  int cpu = -1;
  bool pinned = false;
};

Platform measure_platform(const core::TscCalibration& cal, int cpu) {
  Platform pf;
  pf.ticks_per_ns = cal.ticks_per_ns();
  pf.cpu = cpu;
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pf.pinned = sched_setaffinity(0, sizeof(set), &set) == 0;
  }
  Histogram floor;
  for (int i = 0; i < 20'000; ++i) {
    const std::uint64_t a = rdtsc_ordered();
    const std::uint64_t b = rdtsc_ordered();
    do_not_optimize(a);
    (void)b;
  }
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t a = rdtsc_ordered();
    const std::uint64_t b = rdtsc_ordered();
    floor.record(b - a);
  }
  pf.floor_ticks = floor.quantile(0.50);
  // Spin for 1 s doing nothing but reading the clock. Any gap this loop sees is
  // time the CPU was taken away (interrupt, hypervisor, another tenant).
  const auto t1us = static_cast<std::uint64_t>(1'000 * pf.ticks_per_ns);
  const auto t5us = 5 * t1us, t20us = 20 * t1us, t100us = 100 * t1us;
  const std::uint64_t begin = core::rdtsc_fast();
  const std::uint64_t end = begin + static_cast<std::uint64_t>(1e9 * pf.ticks_per_ns);
  std::uint64_t prev = begin;
  for (;;) {
    const std::uint64_t now = core::rdtsc_fast();
    const std::uint64_t d = now - prev;
    if (d > t1us) {
      pf.hiccups.record(d);
      ++pf.gt1us;
      if (d > t5us) ++pf.gt5us;
      if (d > t20us) ++pf.gt20us;
      if (d > t100us) ++pf.gt100us;
      if (d > pf.max_gap_ticks) pf.max_gap_ticks = d;
    }
    prev = now;
    if (now >= end) break;
  }
  pf.spin_ns = 1'000'000'000ULL;
  return pf;
}

// ---- results --------------------------------------------------------------------------------------------

double g_tpn = 2.8;  // TSC ticks per ns, set once in main after calibration (used for the shadow lead time)

struct Hist {
  Histogram h;
  void rec(std::uint64_t ticks, std::uint64_t floor) { h.record(ticks > floor ? ticks - floor : 0); }
};

struct Pass {
  Hist act, act_new, act_repl, act_canc;  // ticks that put at least one order frame on the wire
  Hist idle;                              // market data ticks where the strategy sent nothing (tick to decision)
  Hist rep_all, rep_act;                  // exchange report -> decision / -> order
  std::uint64_t md_ticks = 0, rep_ticks = 0, acted = 0, minflt = 0, nvcsw = 0, nivcsw = 0;
  std::uint64_t seconds_x1000 = 0;
};

struct Stages {
  Hist md, strat, gate, after;   // acting ticks
  Hist idle_md, idle_strat;      // non-acting ticks
  Hist sum;                      // md + strat + gate for acting ticks, same run
  Hist e2e;                      // packet -> frame in the same staged run
};

rusage usage() {
  rusage u{};
  getrusage(RUSAGE_THREAD, &u);
  return u;
}

void pollute(volatile std::uint8_t* buf, std::size_t n) {
  for (std::size_t i = 0; i < n; i += 64) buf[i] = static_cast<std::uint8_t>(buf[i] + 1);
}

template <bool Stamp>
using PipeT = Pipeline<TxRef<Stamp>>;

// One timed pass over the first `limit` inputs. `cold` flushes L1/L2 before each tick.
bool run_total(const Session& s, std::size_t limit, bool cold, volatile std::uint8_t* junk, std::size_t junk_n, std::uint64_t floor,
               Pass& out, std::uint64_t gap_ticks = 0, std::size_t shadow_k = 0) {
  core::Region region = make_region();
  Tx<true>* tx = new Tx<true>();
  auto* p = new PipeT<true>(TxRef<true>{tx}, s.cfg.mm);
  if (!init_pipe(*p, region, s)) return false;
  // Optional shadow pipeline: a second, independent instance fed its own copy of the session from input 0. During the idle gap, just before
  // each real tick, it processes shadow_k inputs so the same code paths run (instruction cache, branch predictors) without touching the
  // live book's data. It never sends anything; its own wire is a private Tx.
  core::Region shadow_region = make_region();
  Tx<true>* stx = nullptr;
  PipeT<true>* sp = nullptr;
  std::size_t shadow_pos = 0;
  if (shadow_k) {
    stx = new Tx<true>();
    sp = new PipeT<true>(TxRef<true>{stx}, s.cfg.mm);
    if (!init_pipe(*sp, shadow_region, s)) return false;
  }
  const rusage u0 = usage();
  const std::size_t n_in = std::min(limit, s.inputs.size());
  for (std::size_t i = 0; i < n_in; ++i) {
    const Input& in = s.inputs[i];
    std::size_t n = 0;
    if (cold) pollute(junk, junk_n);
    if (gap_ticks) {  // idle spin before the tick, outside the timed interval: inter-arrival spacing without cache pollution
      const std::uint64_t g0 = rdtsc_ordered();
      if (shadow_k && sp) {
        const std::uint64_t lead = static_cast<std::uint64_t>(3000.0 * g_tpn);  // run the shadow ticks in the last ~3 us of the gap
        while (rdtsc_ordered() - g0 + lead < gap_ticks) {
        }
        for (std::size_t k = 0; k < shadow_k && shadow_pos < s.inputs.size(); ++k, ++shadow_pos) {
          const Input& si = s.inputs[shadow_pos];
          std::size_t sn = 0;
          stx->t_first = 0;
          stx->frames = 0;
          if (si.kind == Input::Md) {
            const std::uint8_t* sd = s.packet(si.idx, sn);
            if (sp->ingest(md::Line::A, sd, sn, si.now)) sp->decide(si.now);
          } else {
            const std::uint8_t* sd = s.report(si.idx, sn);
            sp->on_report(sd, sn, si.now);
          }
        }
      }
      while (rdtsc_ordered() - g0 < gap_ticks) {
      }
    }
    tx->t_first = 0;
    tx->frames = 0;
    if (in.kind == Input::Md) {
      const std::uint8_t* d = s.packet(in.idx, n);
      const std::uint64_t t0 = rdtsc_ordered();
      if (p->ingest(md::Line::A, d, n, in.now)) p->decide(in.now);
      const std::uint64_t t1 = rdtsc_ordered();
      ++out.md_ticks;
      if (tx->frames) {
        ++out.acted;
        const std::uint64_t dt = tx->t_first - t0;
        out.act.rec(dt, floor);
        (tx->kind == gw::kMsgReplace ? out.act_repl : tx->kind == gw::kMsgNew ? out.act_new : out.act_canc).rec(dt, floor);
      } else {
        out.idle.rec(t1 - t0, floor);
      }
    } else {
      const std::uint8_t* d = s.report(in.idx, n);
      const std::uint64_t t0 = rdtsc_ordered();
      p->on_report(d, n, in.now);
      const std::uint64_t t1 = rdtsc_ordered();
      ++out.rep_ticks;
      out.rep_all.rec(t1 - t0, floor);
      if (tx->frames) out.rep_act.rec(tx->t_first - t0, floor);
    }
  }
  const rusage u1 = usage();
  out.minflt = static_cast<std::uint64_t>(u1.ru_minflt - u0.ru_minflt);
  out.nvcsw = static_cast<std::uint64_t>(u1.ru_nvcsw - u0.ru_nvcsw);
  out.nivcsw = static_cast<std::uint64_t>(u1.ru_nivcsw - u0.ru_nivcsw);
  const bool ok = n_in < s.inputs.size() ||
                  matches_backtest(s, p->gateway().stats(), p->gateway().oms().position().pos, p->book().digest(),
                                   p->feed().stats(), p->gateway().risk().killed());
  delete sp;
  delete stx;
  delete p;
  delete tx;
  return ok;
}

bool run_staged(const Session& s, std::uint64_t floor, Stages& st) {
  core::Region region = make_region();
  Tx<true>* tx = new Tx<true>();
  auto* p = new PipeT<true>(TxRef<true>{tx}, s.cfg.mm);
  if (!init_pipe(*p, region, s)) return false;
  for (const Input& in : s.inputs) {
    std::size_t n = 0;
    tx->t_first = 0;
    tx->frames = 0;
    if (in.kind == Input::Report) {
      const std::uint8_t* d = s.report(in.idx, n);
      p->on_report(d, n, in.now);
      continue;
    }
    const std::uint8_t* d = s.packet(in.idx, n);
    StageStamp stamp;
    const std::uint64_t t0 = rdtsc_ordered();
    const bool changed = p->ingest(md::Line::A, d, n, in.now);
    const std::uint64_t t1 = rdtsc_ordered();
    if (changed) {
      ProbedGateway<gw::Gateway<TxRef<true>>> pg(p->gateway(), stamp);
      p->decide(in.now, pg);
    }
    const std::uint64_t t2 = rdtsc_ordered();
    if (tx->frames && stamp.t_enter) {
      st.md.rec(t1 - t0, floor);
      st.strat.rec(stamp.t_enter - t1, floor);
      st.gate.rec(tx->t_first - stamp.t_enter, floor);
      st.after.rec(t2 - tx->t_first, floor);
      st.e2e.rec(tx->t_first - t0, floor);
      const std::uint64_t sum = (t1 - t0 > floor ? t1 - t0 - floor : 0) + (stamp.t_enter - t1 > floor ? stamp.t_enter - t1 - floor : 0) +
                                (tx->t_first - stamp.t_enter > floor ? tx->t_first - stamp.t_enter - floor : 0);
      st.sum.h.record(sum);
    } else {
      st.idle_md.rec(t1 - t0, floor);
      st.idle_strat.rec(t2 - t1, floor);
    }
  }
  const bool ok = matches_backtest(s, p->gateway().stats(), p->gateway().oms().position().pos, p->book().digest(),
                                   p->feed().stats(), p->gateway().risk().killed());
  delete p;
  delete tx;
  return ok;
}

bool run_verify(const Session& s, std::uint32_t& peak_orders) {
  core::Region region = make_region();
  HashTx h;
  auto* p = new Pipeline<HashRef>(HashRef{&h}, s.cfg.mm);
  if (!init_pipe(*p, region, s)) return false;
  peak_orders = 0;
  for (const Input& in : s.inputs) {
    std::size_t n = 0;
    if (in.kind == Input::Md) {
      const std::uint8_t* d = s.packet(in.idx, n);
      p->on_packet(md::Line::A, d, n, in.now);
      peak_orders = std::max(peak_orders, p->book().order_count());
    } else {
      const std::uint8_t* d = s.report(in.idx, n);
      p->on_report(d, n, in.now);
    }
  }
  const bool ok = h.hash == s.wire_hash && h.frames == s.frames && h.bytes == s.wire_bytes &&
                  matches_backtest(s, p->gateway().stats(), p->gateway().oms().position().pos, p->book().digest(),
                                   p->feed().stats(), p->gateway().risk().killed());
  delete p;
  return ok;
}

// Sustained throughput: no probes in the loop, only the ends are timed.
double run_speed(const Session& s, double& ns_per_input) {
  core::Region region = make_region();
  Tx<false>* tx = new Tx<false>();
  auto* p = new PipeT<false>(TxRef<false>{tx}, s.cfg.mm);
  if (!init_pipe(*p, region, s)) return 0;
  const std::uint64_t t0 = rdtsc_ordered();
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
  const std::uint64_t t1 = rdtsc_ordered();
  do_not_optimize(tx->frames);
  delete p;
  delete tx;
  ns_per_input = static_cast<double>(t1 - t0);  // ticks; converted by the caller
  return static_cast<double>(s.inputs.size());
}

// The duplicate-line path: a packet the other line already delivered.
void run_dups(const Session& s, std::uint64_t floor, Hist& out) {
  core::Region region = make_region();
  Tx<false>* tx = new Tx<false>();
  auto* p = new PipeT<false>(TxRef<false>{tx}, s.cfg.mm);
  if (!init_pipe(*p, region, s)) return;
  for (const Input& in : s.inputs) {
    std::size_t n = 0;
    if (in.kind == Input::Md) {
      const std::uint8_t* d = s.packet(in.idx, n);
      p->on_packet(md::Line::A, d, n, in.now);
      const std::uint64_t t0 = rdtsc_ordered();
      p->on_packet(md::Line::B, d, n, in.now);
      const std::uint64_t t1 = rdtsc_ordered();
      out.rec(t1 - t0, floor);
    } else {
      const std::uint8_t* d = s.report(in.idx, n);
      p->on_report(d, n, in.now);
    }
  }
  delete p;
  delete tx;
}

// ---- callgrind entry: the only code the instruction-count gate looks at ----------------------------

struct Workload {
  core::Region region;
  Tx<false>* tx = nullptr;
  PipeT<false>* p = nullptr;
};

[[gnu::noinline]] std::uint64_t collect_replay(Workload& w, const Session& s, std::size_t limit) {
  PipeT<false>& p = *w.p;
  const std::size_t n_in = std::min(limit, s.inputs.size());
  for (std::size_t i = 0; i < n_in; ++i) {
    const Input& in = s.inputs[i];
    std::size_t n = 0;
    if (in.kind == Input::Md) {
      const std::uint8_t* d = s.packet(in.idx, n);
      p.on_packet(md::Line::A, d, n, in.now);
    } else {
      const std::uint8_t* d = s.report(in.idx, n);
      p.on_report(d, n, in.now);
    }
  }
  return w.tx->frames;
}

// ---- reporting -------------------------------------------------------------------------------------------

struct Row {
  const char* name;
  const Hist* h;
};

void print_row(const char* name, const Hist& x, double tpn) {
  const Histogram& h = x.h;
  if (h.count() == 0) {
    std::printf("  %-34s %9s\n", name, "-");
    return;
  }
  std::printf("  %-34s %9llu  p50 %7.1f  p90 %7.1f  p99 %7.1f  p99.9 %8.1f  max %9.1f ns\n", name,
              static_cast<unsigned long long>(h.count()), static_cast<double>(h.quantile(0.50)) / tpn,
              static_cast<double>(h.quantile(0.90)) / tpn, static_cast<double>(h.quantile(0.99)) / tpn,
              static_cast<double>(h.quantile(0.999)) / tpn, static_cast<double>(h.max()) / tpn);
}

void json_hist(std::FILE* f, const char* key, const Hist& x, double tpn, bool last = false) {
  const Histogram& h = x.h;
  std::fprintf(f, "  \"%s\": {\"n\": %llu, \"p50\": %.1f, \"p99\": %.1f, \"p999\": %.1f, \"max\": %.1f, \"ticks_per_ns\": %.8f, \"bins\": [", key,
               static_cast<unsigned long long>(h.count()), static_cast<double>(h.quantile(0.50)) / tpn,
               static_cast<double>(h.quantile(0.99)) / tpn, static_cast<double>(h.quantile(0.999)) / tpn,
               static_cast<double>(h.max()) / tpn, tpn);
  bool first = true;
  for (unsigned i = 0; i < Histogram::kBuckets; ++i) {
    if (h.bucket_count(i) == 0) continue;
    std::fprintf(f, "%s{\"upper_ticks\": %llu, \"count\": %llu}", first ? "" : ",",
                 static_cast<unsigned long long>(Histogram::bucket_upper_edge(i)),
                 static_cast<unsigned long long>(h.bucket_count(i)));
    first = false;
  }
  std::fprintf(f, "]}%s\n", last ? "" : ",");
}

std::string cpu_model() {
  std::FILE* f = std::fopen("/proc/cpuinfo", "r");
  char line[256];
  std::string m = "unknown";
  while (f && std::fgets(line, sizeof line, f)) {
    if (std::strncmp(line, "model name", 10) == 0) {
      const char* c = std::strchr(line, ':');
      if (c) m = c + 2;
      while (!m.empty() && (m.back() == '\n' || m.back() == ' ')) m.pop_back();
      break;
    }
  }
  if (f) std::fclose(f);
  return m;
}

bool read_int_file(const char* path, int& v) {
  std::FILE* f = std::fopen(path, "r");
  if (!f) return false;
  const bool ok = std::fscanf(f, "%d", &v) == 1;
  std::fclose(f);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t requests = 300'000;
  int cpu = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)) - 1;
  std::size_t cold_ticks = 40'000, workload = 0;
  std::uint32_t max_orders = 0;
  const char* json = nullptr;
  const char* prof = nullptr;
  double prof_seconds = 20;
  bool gap_sweep = false;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (!std::strcmp(argv[i], "--requests")) requests = std::strtoull(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--cpu")) cpu = std::atoi(next());
    else if (!std::strcmp(argv[i], "--cold-ticks")) cold_ticks = std::strtoull(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--json")) json = next();
    else if (!std::strcmp(argv[i], "--max-orders")) max_orders = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (!std::strcmp(argv[i], "--workload")) workload = std::strtoull(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--profile")) prof = next();
    else if (!std::strcmp(argv[i], "--seconds")) prof_seconds = std::atof(next());
    else if (!std::strcmp(argv[i], "--gap-sweep")) gap_sweep = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 64;
    }
  }

  // ---- the session -------------------------------------------------------------------------------------
  bt::MarketConfig mc;
  mc.requests = requests;
  mc.seed = 1000;
  const bt::Market m = bt::MarketGen::generate(mc);
  bt::BtConfig bc;
  bc.md_ns = 5'000;
  bc.ingress_ns = 10'000;
  bc.egress_ns = 10'000;
  bc.jitter_ns = 2'000;
  bc.seed = 1;
  Session s;
  if (!record_session(m, bc, strat::MmConfig{}, bench_risk(), s)) {
    std::fprintf(stderr, "could not record the session\n");
    return 1;
  }

  // Verify the replay against the backtest at the recorded capacity (which also
  // yields the peak occupancy), then size the book for that peak.
  auto id_slots = [](std::uint32_t cap) {
    std::uint32_t c = 2;
    while (c < cap * 2u) c <<= 1;
    return c;
  };
  std::uint32_t peak_orders = 0;
  if (!run_verify(s, peak_orders)) {
    std::fprintf(stderr, "VERIFY FAILED: the pipeline replay does not match the backtest; numbers would be meaningless\n");
    return 2;
  }
  const std::uint32_t recorded_capacity = s.cfg.book.max_orders;
  if (max_orders) {
    s.cfg.book.max_orders = max_orders;
  } else {
    // Rule used throughout: capacity = 3 x the peak seen, rounded up to a power of two (at least 1024).
    std::uint32_t c = 1024;
    while (c < 3 * peak_orders) c <<= 1;
    s.cfg.book.max_orders = c;
  }
  std::uint32_t peak_again = 0;
  if (!run_verify(s, peak_again)) {
    std::fprintf(stderr, "VERIFY FAILED at capacity %u\n", s.cfg.book.max_orders);
    return 2;
  }

  // ---- callgrind mode: replay once, nothing else in the collected region --------------------------------------
  if (workload) {
    Workload w;
    w.region = make_region();
    w.tx = new Tx<false>();
    w.p = new PipeT<false>(TxRef<false>{w.tx}, s.cfg.mm);
    if (!init_pipe(*w.p, w.region, s)) return 1;
    const std::uint64_t frames = collect_replay(w, s, workload);
    std::printf("workload: %zu inputs, %llu frames, capacity %u\n", std::min(workload, s.inputs.size()),
                static_cast<unsigned long long>(frames), s.cfg.book.max_orders);
    std::printf("compiler: %s\n", __VERSION__);
    return 0;
  }

  // ---- sampling mode ----------------------------------------------------------------------------------------------
  if (prof) {
    if (!benchprof::start(4000, 400'000)) {
      std::fprintf(stderr, "sampler failed to start\n");
      return 1;
    }
    const std::uint64_t t_end = core::monotonic_raw_ns() + static_cast<std::uint64_t>(prof_seconds * 1e9);
    std::uint64_t runs = 0;
    while (core::monotonic_raw_ns() < t_end) {
      core::Region region = make_region();
      Tx<false>* tx = new Tx<false>();
      auto* p = new PipeT<false>(TxRef<false>{tx}, s.cfg.mm);
      init_pipe(*p, region, s);
      Workload w;
      w.p = p;
      w.tx = tx;
      benchprof::resume();
      collect_replay(w, s, s.inputs.size());
      benchprof::pause();
      delete p;
      delete tx;
      ++runs;
    }
    benchprof::stop();
    benchprof::dump(prof, argv[0]);
    std::printf("profiled %llu replays of %zu inputs, %llu samples (%llu dropped) -> %s\n", static_cast<unsigned long long>(runs),
                s.inputs.size(), static_cast<unsigned long long>(benchprof::g_count),
                static_cast<unsigned long long>(benchprof::g_dropped), prof);
    return 0;
  }

  // ---- the measurement ----------------------------------------------------------------------------------------------
  const core::TscCalibration cal;
  const double tpn = cal.ticks_per_ns();
  g_tpn = tpn;
  utsname un{};
  uname(&un);
  int paranoid = -1;
  read_int_file("/proc/sys/kernel/perf_event_paranoid", paranoid);
  std::printf("== tick-to-trade, production pipeline, recorded session ==\n");
  std::printf("  cpu            %s\n", cpu_model().c_str());
  std::printf("  kernel         %s %s, %ld cpus online\n", un.sysname, un.release, sysconf(_SC_NPROCESSORS_ONLN));
  std::printf("  compiler       %s\n", __VERSION__);
#ifdef NDEBUG
  std::printf("  build          NDEBUG\n");
#else
  std::printf("  build          assertions ON (not a release build, do not trust the numbers)\n");
#endif
  std::printf("  TSC            %.4f ticks/ns, invariant %s\n", tpn, core::has_invariant_tsc() ? "yes" : "NO");
  {
    core::Region probe = make_region();
    std::printf("  memory         hugepages %s, mlock %s, prefault yes\n",
                probe.report().huge_explicit ? "explicit" : (probe.report().huge_thp_hint ? "THP hint" : "no"),
                probe.report().locked ? "yes" : "NO");
  }
  const Platform pf = measure_platform(cal, cpu);
  std::printf("  pinned         cpu %d %s\n", pf.cpu, pf.pinned ? "(ok)" : "(FAILED)");
  std::printf("  timer overhead %.1f ns (rdtsc pair with lfence, p50, subtracted from every interval)\n",
              static_cast<double>(pf.floor_ticks) / tpn);
  std::printf("  platform noise a do-nothing spin for 1 s lost the CPU: >1us %llu times, >5us %llu, >20us %llu, >100us %llu, longest %.1f us\n",
              static_cast<unsigned long long>(pf.gt1us), static_cast<unsigned long long>(pf.gt5us),
              static_cast<unsigned long long>(pf.gt20us), static_cast<unsigned long long>(pf.gt100us),
              static_cast<double>(pf.max_gap_ticks) / tpn / 1000.0);
  std::printf("  session        %zu inputs (%zu feed packets, %zu exchange reports), %llu order frames on the wire\n", s.inputs.size(),
              s.md_inputs(), s.inputs.size() - s.md_inputs(), static_cast<unsigned long long>(s.frames));
  std::printf("                 new %llu, replace %llu, cancel %llu\n\n", static_cast<unsigned long long>(s.sent_new),
              static_cast<unsigned long long>(s.sent_replace), static_cast<unsigned long long>(s.sent_cancel));

  std::printf("  verify         replay puts the backtest's exact bytes on the wire (hash %016llx): OK\n",
              static_cast<unsigned long long>(s.wire_hash));
  std::printf("  book sizing    peak %u resting orders; recorded with capacity %u (id table %u slots, %u KiB)\n", peak_orders,
              recorded_capacity, id_slots(recorded_capacity), id_slots(recorded_capacity) * 16u / 1024u);
  std::printf("  measured at    capacity %u (id table %u slots, %u KiB)%s\n\n", s.cfg.book.max_orders, id_slots(s.cfg.book.max_orders),
              id_slots(s.cfg.book.max_orders) * 16u / 1024u, max_orders ? " (forced by --max-orders)" : " (3x peak, power of two)");

  // Warm-up pass (page cache, branch predictors, frequency ramp), discarded.
  {
    Pass warm;
    run_total(s, s.inputs.size(), false, nullptr, 0, pf.floor_ticks, warm);
  }

  Pass hot;
  if (!run_total(s, s.inputs.size(), false, nullptr, 0, pf.floor_ticks, hot)) {
    std::fprintf(stderr, "TIMED PASS DIVERGED FROM THE BACKTEST\n");
    return 2;
  }
  std::printf("== hot caches: packets replayed back to back (best case for caches and predictors) ==\n");
  print_row("tick-to-trade, any order", hot.act, tpn);
  print_row("  first frame was a replace", hot.act_repl, tpn);
  print_row("  first frame was a new order", hot.act_new, tpn);
  print_row("  first frame was a cancel", hot.act_canc, tpn);
  print_row("tick-to-decision, no order", hot.idle, tpn);
  print_row("report-to-decision (all reports)", hot.rep_all, tpn);
  print_row("report-to-order (report caused one)", hot.rep_act, tpn);
  std::printf("  ticks: %llu feed packets, %llu caused an order (%.1f%%), %llu reports\n",
              static_cast<unsigned long long>(hot.md_ticks), static_cast<unsigned long long>(hot.acted),
              100.0 * static_cast<double>(hot.acted) / static_cast<double>(hot.md_ticks), static_cast<unsigned long long>(hot.rep_ticks));
  std::printf("  during the pass: page faults %llu, voluntary context switches %llu, involuntary %llu\n\n",
              static_cast<unsigned long long>(hot.minflt), static_cast<unsigned long long>(hot.nvcsw),
              static_cast<unsigned long long>(hot.nivcsw));
  if (hot.minflt != 0) {
    // Everything is prefaulted at startup; a fault in the loop means something touched memory it should not have.
    std::fprintf(stderr, "HARD CHECK FAILED: %llu page faults inside the timed loop\n", static_cast<unsigned long long>(hot.minflt));
    return 3;
  }

  Stages st;
  if (!run_staged(s, pf.floor_ticks, st)) {
    std::fprintf(stderr, "STAGED PASS DIVERGED FROM THE BACKTEST\n");
    return 2;
  }
  std::printf("== where the time goes (ticks that sent an order; the probes between stages are not free, see the measured probe cost below) ==\n");
  print_row("feed handler: parse, arbitrate, book", st.md, tpn);
  print_row("strategy: signal and quote logic", st.strat, tpn);
  print_row("gateway: risk, OMS, encode, stage", st.gate, tpn);
  print_row("  sum of the three, same ticks", st.sum, tpn);
  print_row("  end to end, same run", st.e2e, tpn);
  print_row("after the wire (second side etc.)", st.after, tpn);
  std::printf("  probe cost: the staged run's end to end p50 is %.1f ns above the unprobed headline p50 (%.1f ns); stage times\n"
              "  below include their share of it, so read them as proportions, not as exact nanoseconds\n",
              static_cast<double>(st.e2e.h.quantile(0.50)) / tpn - static_cast<double>(hot.act.h.quantile(0.50)) / tpn,
              static_cast<double>(hot.act.h.quantile(0.50)) / tpn);
  print_row("no-order ticks: feed handler", st.idle_md, tpn);
  print_row("no-order ticks: strategy", st.idle_strat, tpn);
  std::printf("\n");

  Hist dups;
  run_dups(s, pf.floor_ticks, dups);
  std::printf("== the other feed line ==\n");
  print_row("duplicate packet dropped (line B)", dups, tpn);
  std::printf("\n");

  Pass cold;
  std::vector<std::uint8_t> junk_store(4u << 20);
  volatile std::uint8_t* junk = junk_store.data();
  run_total(s, cold_ticks, true, junk, junk_store.size(), pf.floor_ticks, cold);
  std::printf("== cold L1 and L2: a 4 MiB walk before every tick (first %zu inputs; data still in L3) ==\n", cold_ticks);
  print_row("tick-to-trade, any order", cold.act, tpn);
  print_row("tick-to-decision, no order", cold.idle, tpn);
  print_row("report-to-decision", cold.rep_all, tpn);
  std::printf("\n");

  // Inter-arrival spacing: an idle spin before every tick (outside the timed interval), no cache pollution. Each gap is compared with a
  // gap-0 pass over the same first N inputs, so the only difference is the idle time between ticks.
  struct GapRow { double gap_us; std::size_t n; double idle0_p50, idle0_p99, idle_p50, idle_p99, act0_p50, act_p50, idle_w_p50, act_w_p50; std::uint64_t n_idle, n_act; };
  std::vector<GapRow> gap_rows;
  if (gap_sweep) {
    std::printf("== inter-arrival spacing: idle spin before every tick, gap 0 over the same first N inputs as control ==\n");
    std::printf("  %-9s %-8s %-10s %-24s %-24s %-22s\n", "gap us", "N", "n idle/act", "no-order p50 / p99 (ctl)", "no-order p50 / p99", "order p50 (ctl) / p50");
    const double gaps_us[] = {0.0, 1.0, 5.0, 20.0, 100.0, 1000.0, 10000.0};
    for (const double g : gaps_us) {
      std::size_t n = g == 0.0 ? 40000 : std::min<std::size_t>(40000, std::max<std::size_t>(300, static_cast<std::size_t>(2.0e6 / g)));
      Pass c, x, w;
      run_total(s, n, false, nullptr, 0, pf.floor_ticks, c, 0);
      run_total(s, n, false, nullptr, 0, pf.floor_ticks, x, static_cast<std::uint64_t>(g * 1000.0 * tpn));
      if (g >= 5.0) run_total(s, n, false, nullptr, 0, pf.floor_ticks, w, static_cast<std::uint64_t>(g * 1000.0 * tpn), 5);
      auto q = [&](const Hist& h, double qq) { return static_cast<double>(h.h.quantile(qq)) / tpn; };
      GapRow r{g, n, q(c.idle, 0.50), q(c.idle, 0.99), q(x.idle, 0.50), q(x.idle, 0.99), q(c.act, 0.50), q(x.act, 0.50), q(w.idle, 0.50), q(w.act, 0.50), x.idle.h.count(), x.act.h.count()};
      gap_rows.push_back(r);
      std::printf("  %-9.0f %-8zu %5llu/%-4llu %9.1f / %-10.1f %9.1f / %-10.1f %9.1f / %-9.1f  warmed(5 shadow ticks): no-order %.1f order %.1f\n", g, n, static_cast<unsigned long long>(r.n_idle),
                  static_cast<unsigned long long>(r.n_act), r.idle0_p50, r.idle0_p99, r.idle_p50, r.idle_p99, r.act0_p50, r.act_p50, r.idle_w_p50, r.act_w_p50);
    }
    std::printf("\n");
  }

  // Book capacity changes the cache footprint of the order id table, so it moves the tail.
  std::printf("== book capacity sensitivity (hot caches; same session, same wire bytes) ==\n");
  std::printf("  %-12s %-12s %12s %12s %12s %12s\n", "capacity", "id table", "t2t p50", "t2t p99", "no-order p50", "no-order p99");
  {
    const std::uint32_t keep = s.cfg.book.max_orders;
    for (const std::uint32_t cap : {keep, 4096u, 16384u, 65536u, 262144u}) {
      if (cap == keep && cap != s.cfg.book.max_orders) continue;
      s.cfg.book.max_orders = cap;
      Pass x;
      if (!run_total(s, s.inputs.size(), false, nullptr, 0, pf.floor_ticks, x)) {
        std::fprintf(stderr, "DIVERGED at capacity %u\n", cap);
        return 2;
      }
      std::printf("  %-12u %5u KiB    %9.1f ns %9.1f ns %9.1f ns %9.1f ns%s\n", cap, id_slots(cap) * 16u / 1024u,
                  static_cast<double>(x.act.h.quantile(0.50)) / tpn, static_cast<double>(x.act.h.quantile(0.99)) / tpn,
                  static_cast<double>(x.idle.h.quantile(0.50)) / tpn, static_cast<double>(x.idle.h.quantile(0.99)) / tpn,
                  cap == keep ? "   <- headline" : "");
    }
    s.cfg.book.max_orders = keep;
  }
  std::printf("\n");

  double ticks_total = 0;
  const double inputs = run_speed(s, ticks_total);
  const double ns_per_input = ticks_total / inputs / tpn;
  std::printf("== sustained ==\n  %.1f ns per input on average (%.2f M inputs/s), no probes in the loop\n\n", ns_per_input,
              1000.0 / ns_per_input);

  if (json) {
    std::FILE* f = std::fopen(json, "w");
    if (f) {
      std::fprintf(f, "{\n  \"cpu\": \"%s\",\n  \"tsc_ghz\": %.4f,\n  \"timer_overhead_ns\": %.1f,\n", cpu_model().c_str(), tpn,
                   static_cast<double>(pf.floor_ticks) / tpn);
      json_hist(f, "t2t_hot", hot.act, tpn);
      json_hist(f, "t2t_hot_replace", hot.act_repl, tpn);
      json_hist(f, "t2d_hot_idle", hot.idle, tpn);
      json_hist(f, "report_hot", hot.rep_all, tpn);
      json_hist(f, "t2t_cold", cold.act, tpn);
      json_hist(f, "stage_md", st.md, tpn);
      json_hist(f, "stage_strategy", st.strat, tpn);
      json_hist(f, "stage_gateway", st.gate, tpn);
      std::fprintf(f, "  \"gap_sweep\": [");
      for (std::size_t k = 0; k < gap_rows.size(); ++k) {
        const GapRow& r = gap_rows[k];
        std::fprintf(f, "%s{\"gap_us\": %.1f, \"n_inputs\": %zu, \"n_idle\": %llu, \"n_act\": %llu, \"idle_ctl_p50\": %.1f, \"idle_ctl_p99\": %.1f, "
                        "\"idle_p50\": %.1f, \"idle_p99\": %.1f, \"act_ctl_p50\": %.1f, \"act_p50\": %.1f, \"idle_warm_p50\": %.1f, \"act_warm_p50\": %.1f}", k ? ", " : "", r.gap_us, r.n,
                     static_cast<unsigned long long>(r.n_idle), static_cast<unsigned long long>(r.n_act), r.idle0_p50, r.idle0_p99, r.idle_p50,
                     r.idle_p99, r.act0_p50, r.act_p50, r.idle_w_p50, r.act_w_p50);
      }
      std::fprintf(f, "],\n");
      std::fprintf(f, "  \"ns_per_input\": %.1f,\n  \"noise_gt5us_per_s\": %llu\n}\n", ns_per_input,
                   static_cast<unsigned long long>(pf.gt5us));
      std::fclose(f);
    }
  }
  return 0;
}
