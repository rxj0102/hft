// libFuzzer target for the live session with an external venue and position closing: the feed as in fuzz_live, plus
// an "exchange" the input controls.
//
//   feed lines, "#" markers, "@p<n>" poll, "@k" kill, "@s" shutdown     as in fuzz_live
//   "@a<n>"        answer every order frame sent so far like an exchange: new -> accepted, cancel -> canceled,
//                  replace -> replaced; a closing IOC is filled for n % (qty+1) lots and the rest cancelled
//   "@f<n>"        a fill report for an order picked by n, for (n / 7) % (qty+2) lots (may exceed the order: a bad report)
//   "@r<a>,<b>,<c>" any report: type a % 9, order b % (orders seen + 2), quantity c % 40, segment off by one when a >= 9
//
// Oracles, beyond "no crash and no sanitizer report":
//  * a closing (IOC) order always goes against the position and never exceeds it, and a segment sends at most
//    close_max_attempts of them;
//  * when the input is over and time has passed every deadline, the session is not busy;
//  * the same input twice gives the same frames in the same order (determinism).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/feed/live.hpp"
#include "hft/gw/order_entry.hpp"

using namespace hft;
using namespace hft::feed;

namespace {

struct Known {
  std::uint64_t seg, id;
  core::Side side;
  sim::OrderType otype;
  std::int64_t qty, price_raw;
  std::uint8_t kind;
};

struct Ctx {
  LiveSession* s = nullptr;
  std::vector<Known> all;          // every frame sent, in order
  std::size_t answered = 0;        // all[0..answered) have been answered by "@a"
  std::uint64_t hash = 1469598103934665603ULL;
  std::uint64_t ioc_in_seg[64] = {};
  std::uint64_t max_attempts = 3;
  bool bad = false;
};

void tap(void* c, std::uint64_t seg, const std::uint8_t* d, std::size_t n) noexcept {
  Ctx* x = static_cast<Ctx*>(c);
  gw::ClientMsg m;
  if (n < 2 || !gw::decode_client(d + 2, n - 2, m)) return;
  x->all.push_back({seg, m.id, m.side, m.otype, m.qty, m.price.raw(), m.kind});
  for (std::size_t i = 0; i < n; ++i) x->hash = (x->hash ^ d[i]) * 1099511628211ULL;
  if (m.kind == gw::kMsgNew && m.otype == sim::OrderType::IOC) {
    const std::int64_t pos = x->s->position_lots();
    if (m.side == core::Side::Sell ? !(pos > 0 && m.qty <= pos) : !(pos < 0 && m.qty <= -pos)) x->bad = true;
    if (seg < 64 && ++x->ioc_in_seg[seg] > x->max_attempts) x->bad = true;
  }
}

sim::Report rep(sim::ReportType t, const Known& k, std::int64_t qty, std::int64_t leaves) {
  sim::Report r{};
  r.type = t;
  r.id = k.id;
  r.side = k.side;
  r.owner = 1;
  r.qty = qty;
  r.leaves = leaves;
  r.price = core::Price::from_raw(k.price_raw > 0 ? k.price_raw : 1'000'000);
  return r;
}

std::uint64_t num(const std::string& line, std::size_t from, std::size_t* end = nullptr) {
  std::uint64_t v = 0;
  std::size_t i = from;
  for (; i < line.size() && i < from + 12 && line[i] >= '0' && line[i] <= '9'; ++i) v = v * 10 + static_cast<std::uint64_t>(line[i] - '0');
  if (end) *end = i;
  return v;
}

void answer_all(LiveSession& s, Ctx& x, std::uint64_t n, std::uint64_t now) {
  const std::size_t to = x.all.size();
  for (std::size_t i = x.answered; i < to; ++i) {
    const Known k = x.all[i];  // a copy: answering can make the session send more
    if (k.kind == gw::kMsgNew && k.otype == sim::OrderType::IOC) {
      const std::int64_t f = static_cast<std::int64_t>(n % static_cast<std::uint64_t>(k.qty + 1));
      s.on_exchange_report(k.seg, rep(sim::ReportType::Accepted, k, k.qty, k.qty), now);
      if (f > 0) s.on_exchange_report(k.seg, rep(sim::ReportType::Fill, k, f, k.qty - f), now);
      if (f < k.qty) s.on_exchange_report(k.seg, rep(sim::ReportType::Canceled, k, k.qty - f, 0), now);
    } else if (k.kind == gw::kMsgNew) {
      s.on_exchange_report(k.seg, rep(sim::ReportType::Accepted, k, k.qty, k.qty), now);
    } else if (k.kind == gw::kMsgCancel) {
      s.on_exchange_report(k.seg, rep(sim::ReportType::Canceled, k, 1, 0), now);
    } else {
      s.on_exchange_report(k.seg, rep(sim::ReportType::Replaced, k, k.qty, k.qty), now);
    }
  }
  x.answered = to;
}

struct Out {
  std::uint64_t hash = 0;
  LiveStats st;
  bool bad = false;
};

Out run(const std::uint8_t* data, std::size_t size) {
  Ctx x;
  LiveConfig c;
  c.l2.num_levels = 1024;
  c.check_every = 3;
  c.arena_bytes = 16u << 20;
  c.max_orders = 8192;
  c.stale_ns = 1000;
  c.external_venue = true;
  c.flatten_timeout_ns = 500;
  c.close_position = true;
  c.close_max_attempts = static_cast<std::uint32_t>(x.max_attempts);
  c.order_tap = tap;
  c.order_tap_ctx = &x;
  LiveSession s(c);
  x.s = &s;
  const char* text = reinterpret_cast<const char*>(data);
  std::uint64_t last = 0;
  std::size_t pos = 0;
  while (pos < size) {
    std::size_t end = pos;
    while (end < size && text[end] != '\n') ++end;
    const std::string line(text + pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line[0] == '@') {
      const char cmd = line.size() > 1 ? line[1] : 0;
      if (cmd == 'k') s.kill(last);
      else if (cmd == 's') s.shutdown(last);
      else if (cmd == 'p') {
        const std::uint64_t d = num(line, 2);
        s.poll(last + d);
        last += d;
      } else if (cmd == 'a') {
        answer_all(s, x, num(line, 2), last);
      } else if (cmd == 'f' && !x.all.empty()) {
        const std::uint64_t n = num(line, 2);
        const Known k = x.all[n % x.all.size()];
        const std::int64_t q = static_cast<std::int64_t>((n / 7) % static_cast<std::uint64_t>(k.qty + 2));
        s.on_exchange_report(k.seg, rep(sim::ReportType::Fill, k, q, k.qty - q), last);
      } else if (cmd == 'r') {
        std::size_t e = 2;
        const std::uint64_t a = num(line, e, &e);
        const std::uint64_t b = num(line, e + 1, &e);
        const std::uint64_t cc = num(line, e + 1, &e);
        Known k{1, b % (x.all.size() + 2), core::Side::Buy, sim::OrderType::PostOnly, 5, 1'000'000, gw::kMsgNew};
        if (!x.all.empty() && b % (x.all.size() + 2) < x.all.size()) k = x.all[b % (x.all.size() + 2)];
        const std::uint64_t seg = a >= 9 ? k.seg + 1 : k.seg;
        s.on_exchange_report(seg, rep(static_cast<sim::ReportType>(a % 7), k, static_cast<std::int64_t>(cc % 40), static_cast<std::int64_t>(cc % 13)), last);
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
  }
  // the end: stop for good, let the exchange answer (twice, because answers cause more frames), then let every deadline pass
  s.shutdown(last + 1);
  for (int round = 0; round < 8; ++round) {
    answer_all(s, x, static_cast<std::uint64_t>(round), last + 2);
    last += 1000;
  }
  s.poll(last + 1'000'000'000ULL);
  s.poll(last + 2'000'000'000ULL);
  if (s.busy()) x.bad = true;
  Out o;
  o.hash = x.hash;
  o.st = s.stats();
  o.bad = x.bad;
  return o;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 64 * 1024) return 0;
  const Out a = run(data, size);
  if (a.bad) __builtin_trap();
  const Out b = run(data, size);
  if (a.hash != b.hash || a.st.sent_new != b.st.sent_new || a.st.close_orders != b.st.close_orders || a.st.sent_cancel != b.st.sent_cancel) __builtin_trap();
  return 0;
}
