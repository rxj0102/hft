// hft_live: run the production pipeline on a live level-2 feed, against a paper venue.
//
//   python3 scripts/capture_l2.py --out - --seconds 600 | hft_live --resnap-file /tmp/resnap
//   hft_live --from data/btc.l2 [--pace 1]          # a recorded capture, through the same live code
//
// Reads capture-format lines (see scripts/capture_l2.py) from stdin or a file.
// With stdin the clock is this process's monotonic clock, read when each line
// arrives; the stamps written by the capture process are ignored. With --from
// the stamps in the file are the clock, so a drill is exactly reproducible.
//
// What happens on the way is described in include/hft/feed/live.hpp. In short:
// every reason the feed can stop being trustworthy ends the segment by cancelling
// every working quote; quoting restarts at the next snapshot. SIGINT or SIGTERM
// latches the kill switch, cancels everything, prints a summary and exits.
//
// The venue is a paper venue (acks, never fills) with a simulated round trip;
// there is no order entry to any real exchange here, and no API key is read.
//
// Exit codes: 0 clean, 1 usage or I/O, 4 integrity failure (book differs from the
// model, or the session could not get flat), 5 no segment ever started.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "hft/feed/live.hpp"

using namespace hft;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::uint64_t mono_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t stamp_of(const std::string& line) {
  std::size_t i = (!line.empty() && line[0] == '#') ? 1 : 0;
  std::uint64_t t = 0;
  for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) t = t * 10 + static_cast<std::uint64_t>(line[i] - '0');
  return t;
}

void print_hist(const char* name, const core::Histogram& h) {
  if (h.count() == 0) {
    std::printf("  %-34s -\n", name);
    return;
  }
  std::printf("  %-34s %8llu  p50 %7llu  p90 %7llu  p99 %7llu  p99.9 %8llu  max %9llu ns\n", name,
              static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(h.quantile(0.50)),
              static_cast<unsigned long long>(h.quantile(0.90)), static_cast<unsigned long long>(h.quantile(0.99)),
              static_cast<unsigned long long>(h.quantile(0.999)), static_cast<unsigned long long>(h.max()));
}

void touch(const std::string& path) {
  if (path.empty()) return;
  std::ofstream f(path, std::ios::trunc);
  f << "resnap\n";
}

void status(const feed::LiveSession& s, double secs) {
  const feed::LiveStats& st = s.stats();
  const gw::GatewayStats* g = s.gateway_stats();  // the running segment's counters are added when it ends
  const unsigned long long sn = st.sent_new + (g ? g->sent_new : 0), sr = st.sent_replace + (g ? g->sent_replace : 0),
                           sc = st.sent_cancel + (g ? g->sent_cancel : 0);
  std::fprintf(stderr,
               "[%7.1fs] %s | lines %llu applied %llu | segments %llu (stale %llu, window %llu, reconnect %llu) | "
               "orders new %llu repl %llu canc %llu | open now %u | checks %llu fail %llu\n",
               secs, s.active() ? "QUOTING" : (s.halted() ? "HALTED " : "PULLED "),
               static_cast<unsigned long long>(st.lines), static_cast<unsigned long long>(st.messages),
               static_cast<unsigned long long>(st.segments), static_cast<unsigned long long>(st.stale_events),
               static_cast<unsigned long long>(st.ended[static_cast<int>(feed::EndReason::Window)]),
               static_cast<unsigned long long>(st.ended[static_cast<int>(feed::EndReason::Marker)]), sn, sr, sc,
               s.open_orders(), static_cast<unsigned long long>(st.checks),
               static_cast<unsigned long long>(st.check_failures));
}

// ---- operations: event log and metrics file ----------------------------------------------------------
// One JSON object per line, appended and flushed at once, so a crash loses nothing already written.
// Events are derived from changes in the session's counters and summaries after each call; the session
// itself knows nothing about logging.
struct EventLog {
  std::FILE* f = nullptr;
  bool ok() const { return f != nullptr; }
  void open(const std::string& path) {
    if (path == "-") f = stderr;
    else f = std::fopen(path.c_str(), "a");
  }
  // `body` is the inside of the object after "event":"name"; callers pass only numbers and fixed words.
  void emit(const char* event, double mono_s, const std::string& body = std::string()) {
    if (!f) return;
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(f, "{\"ts_ms\":%lld,\"mono_s\":%.3f,\"event\":\"%s\"%s%s}\n", static_cast<long long>(wall), mono_s, event,
                 body.empty() ? "" : ",", body.c_str());
    std::fflush(f);
  }
};

// Mirror of the order flow for the order-entry bridge, one JSON line per frame, to an inherited pipe (--orders-fd).
// The tap runs inside the tick-to-trade path: it only formats into a fixed buffer. The write happens afterwards, in
// flush(), and never blocks. If the reader is gone or falls a megabyte behind, the mirror is declared lost and the pipe
// closed; the bridge sees end of input and cancels everything it has working, and the supervisor restarts the trio.
struct OrderOut {
  static constexpr std::size_t kCap = 1u << 20;
  int fd = -1;
  bool lost = false;
  const char* lost_why = "";
  std::size_t len = 0;
  std::uint64_t lines = 0;
  std::unique_ptr<char[]> buf{new char[kCap]};

  void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (fd < 0) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf.get() + len, kCap - len, fmt, ap);
    va_end(ap);
    if (n < 0 || static_cast<std::size_t>(n) >= kCap - len) {
      lose("overflow");
      return;
    }
    len += static_cast<std::size_t>(n);
    ++lines;
  }
  void lose(const char* why) {
    if (fd >= 0) ::close(fd);
    fd = -1;
    lost = true;
    lost_why = why;
    len = 0;
  }
  // Writes what the pipe takes now. wait_ms > 0: keep trying that long (used once, at exit).
  void flush(int wait_ms = 0) {
    const auto t0 = std::chrono::steady_clock::now();
    while (fd >= 0 && len > 0) {
      const ssize_t w = ::write(fd, buf.get(), len);
      if (w > 0) {
        std::memmove(buf.get(), buf.get() + w, len - static_cast<std::size_t>(w));
        len -= static_cast<std::size_t>(w);
        continue;
      }
      if (w < 0 && errno == EINTR) continue;
      if (w < 0 && errno == EAGAIN) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (wait_ms <= 0 || waited >= wait_ms) return;
        pollfd p{fd, POLLOUT, 0};
        ::poll(&p, 1, 20);
        continue;
      }
      lose("write_failed");
      return;
    }
  }
};

void order_tap(void* ctx, std::uint64_t seg, const std::uint8_t* d, std::size_t n) noexcept {
  auto* o = static_cast<OrderOut*>(ctx);
  gw::ClientMsg m;
  if (n < 2 || !gw::decode_client(d + 2, n - 2, m)) return;
  const unsigned long long sg = seg, id = m.id;
  if (m.kind == gw::kMsgNew)
    o->line("{\"t\":\"new\",\"seg\":%llu,\"id\":%llu,\"side\":\"%s\",\"px\":%lld,\"qty\":%lld,\"type\":%u}\n", sg, id,
            m.side == core::Side::Buy ? "buy" : "sell", static_cast<long long>(m.price.raw()), static_cast<long long>(m.qty),
            static_cast<unsigned>(m.otype));
  else if (m.kind == gw::kMsgReplace)
    o->line("{\"t\":\"rep\",\"seg\":%llu,\"id\":%llu,\"px\":%lld,\"qty\":%lld}\n", sg, id, static_cast<long long>(m.price.raw()),
            static_cast<long long>(m.qty));
  else if (m.kind == gw::kMsgCancel)
    o->line("{\"t\":\"can\",\"seg\":%llu,\"id\":%llu}\n", sg, id);
}

// ---- reports from the exchange (--exchange-fd) -------------------------------------------------------------------
// One JSON line per report, written by scripts/order_bridge.py --loop:
//   {"t":"rep","seg":S,"rt":"accepted|rejected|fill|canceled|replaced|cancel_rejected|replace_rejected","id":I,
//    "px":P,"qty":Q,"leaves":L, ...}   (other keys, such as side and reason, are for people and are ignored)
// px is scaled by 1e4, qty and leaves are lots. Anything that does not parse is counted and dropped.
bool field_i64(std::string_view s, const char* key, std::int64_t& out) {
  const std::string k = std::string("\"") + key + "\":";
  const std::size_t p = s.find(k);
  if (p == std::string_view::npos) return false;
  std::size_t i = p + k.size();
  while (i < s.size() && s[i] == ' ') ++i;
  bool neg = false;
  if (i < s.size() && s[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
  std::int64_t v = 0;
  int digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    if (++digits > 18) return false;
    v = v * 10 + (s[i++] - '0');
  }
  out = neg ? -v : v;
  return true;
}

bool field_str(std::string_view s, const char* key, std::string_view& out) {
  const std::string k = std::string("\"") + key + "\":";
  const std::size_t p = s.find(k);
  if (p == std::string_view::npos) return false;
  std::size_t b = p + k.size();
  while (b < s.size() && s[b] == ' ') ++b;
  if (b >= s.size() || s[b] != '"') return false;
  ++b;
  const std::size_t e = s.find('"', b);
  if (e == std::string_view::npos) return false;
  out = s.substr(b, e - b);
  return true;
}

bool parse_report(std::string_view line, std::uint64_t& seg, sim::Report& r) {
  std::string_view t;
  if (!field_str(line, "t", t) || t != "rep") return false;
  std::int64_t sg = 0, id = 0, px = 0, qty = 0, leaves = 0;
  if (!field_i64(line, "seg", sg) || !field_i64(line, "id", id) || sg <= 0 || id <= 0) return false;
  std::string_view rt;
  if (!field_str(line, "rt", rt)) return false;
  r = sim::Report{};
  if (rt == "accepted") r.type = sim::ReportType::Accepted;
  else if (rt == "rejected") r.type = sim::ReportType::Rejected;
  else if (rt == "fill") r.type = sim::ReportType::Fill;
  else if (rt == "canceled") r.type = sim::ReportType::Canceled;
  else if (rt == "replaced") r.type = sim::ReportType::Replaced;
  else if (rt == "cancel_rejected") r.type = sim::ReportType::CancelRejected;
  else if (rt == "replace_rejected") r.type = sim::ReportType::ReplaceRejected;
  else return false;
  field_i64(line, "px", px);
  field_i64(line, "qty", qty);
  field_i64(line, "leaves", leaves);
  if (px < 0 || qty < 0 || leaves < 0) return false;
  r.id = static_cast<core::OrderId>(id);
  r.price = core::Price::from_raw(px);
  r.qty = qty;
  r.leaves = leaves;
  // The OMS keeps the side, the reason and the liquidity flag of its own record; what the report says about them is
  // informational (the bridge sends them for people reading its output) and is not read here.
  seg = static_cast<std::uint64_t>(sg);
  return true;
}

struct ReportIn {
  int fd = -1;
  bool lost = false;
  std::string carry;
  std::uint64_t lines = 0, bad = 0;
};

struct Observer {
  std::uint64_t segments = 0, stale = 0, resnaps = 0, sums = 0, mirrored = 0, bad_lines = 0, regress = 0;
  bool halted = false, lost_logged = false, exchange_lost_logged = false;
};

std::string num(const char* k, unsigned long long v) { return std::string("\"") + k + "\":" + std::to_string(v); }

void observe(EventLog& log, Observer& o, const feed::LiveSession& s, double mono_s, OrderOut* oo = nullptr) {
  if (oo) {
    while (o.mirrored < s.summaries().size()) {
      const feed::SegmentSummary& sm = s.summaries()[o.mirrored++];
      oo->line("{\"t\":\"end\",\"seg\":%llu,\"reason\":\"%s\"}\n", static_cast<unsigned long long>(o.mirrored),
               feed::end_reason_name(sm.reason));
    }
    oo->flush();
    if (oo->lost && !o.lost_logged) {
      o.lost_logged = true;
      log.emit("orders_out_lost", mono_s, std::string("\"why\":\"") + oo->lost_why + "\"");
    }
  }
  if (!log.ok()) return;
  const feed::LiveStats& st = s.stats();
  if (st.segments != o.segments) {
    o.segments = st.segments;
    log.emit("segment_start", mono_s, num("segment", st.segments));
  }
  if (st.stale_events != o.stale) {
    o.stale = st.stale_events;
    log.emit("stale_feed", mono_s, num("count", st.stale_events));
  }
  if (st.resnap_requests != o.resnaps) {
    o.resnaps = st.resnap_requests;
    log.emit("resubscribe_requested", mono_s, num("count", st.resnap_requests));
  }
  while (o.sums < s.summaries().size()) {
    const feed::SegmentSummary& sm = s.summaries()[o.sums++];
    std::string b = std::string("\"reason\":\"") + feed::end_reason_name(sm.reason) + "\"," + num("segment", o.sums) + "," +
                    num("packets", sm.packets) + "," + num("flat", sm.flat) + "," + num("kill_latched", sm.kill_latched) + "," +
                    num("book_matched_model", sm.book_matched_model) + "," + num("flatten_rounds", sm.flatten_rounds) + "," +
                    num("flatten_sim_ns", sm.flatten_sim_ns) + "," + num("open_before_flatten", sm.open_before_flatten) + ",\"position_lots\":" + std::to_string(sm.net_position_lots) + "," + num("fills", sm.fills) +
                    "," + num("close_attempts", sm.close_attempts) + ",\"closed_lots\":" + std::to_string(sm.closed_lots) + "," + num("close_failed", sm.close_failed) +
                    ",\"close_why_not\":\"" + sm.close_why_not + "\"";
    log.emit("segment_end", mono_s, b);
  }
  if (st.time_regressions != o.regress) {
    o.regress = st.time_regressions;
    log.emit("clock_regression", mono_s, num("count", st.time_regressions));
  }
  // a halt the operator did not ask for: shutdown and kill are already visible as segment_end reasons
  if (s.halted() && !o.halted && (st.check_failures || st.final_mismatches || st.flatten_failures)) {
    o.halted = true;
    log.emit("halted", mono_s,
             num("check_failures", st.check_failures) + "," + num("final_mismatches", st.final_mismatches) + "," +
                 num("flatten_failures", st.flatten_failures));
  }
}

// Prometheus text format, written to a temporary file and renamed, so a reader never sees half a file.
void write_metrics(const std::string& path, const feed::LiveSession& s, double uptime_s) {
  if (path.empty()) return;
  const feed::LiveStats& st = s.stats();
  const gw::GatewayStats* g = s.gateway_stats();
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  auto c = [&](const char* name, const char* help, unsigned long long v, const char* type = "counter") {
    std::fprintf(f, "# HELP %s %s\n# TYPE %s %s\n%s %llu\n", name, help, name, type, name, v);
  };
  std::fprintf(f, "# HELP hft_live_uptime_seconds Seconds since start.\n# TYPE hft_live_uptime_seconds gauge\nhft_live_uptime_seconds %.3f\n", uptime_s);
  c("hft_live_quoting", "1 while a segment is live and quotes may be out.", s.active() ? 1 : 0, "gauge");
  c("hft_live_halted", "1 once the session has halted for good.", s.halted() ? 1 : 0, "gauge");
  c("hft_live_open_orders", "Orders working now (OMS view).", s.open_orders(), "gauge");
  std::fprintf(f, "# HELP hft_live_position_lots Net filled quantity of the running segment, in lots.\n# TYPE hft_live_position_lots gauge\nhft_live_position_lots %lld\n", static_cast<long long>(s.position_lots()));
  c("hft_live_close_orders_total", "Closing (reduce-only IOC) orders sent.", st.close_orders);
  c("hft_live_close_failures_total", "Times closing the position was wanted and stopped with the position not zero.", st.close_failures);
  c("hft_live_lines_total", "Input lines.", st.lines);
  c("hft_live_feed_messages_total", "Feed messages parsed.", st.messages);
  c("hft_live_feed_bad_total", "Damaged feed messages.", st.bad);
  c("hft_live_segments_started_total", "Segments started.", st.segments);
  c("hft_live_stale_feed_total", "Times the feed went silent and quotes were pulled.", st.stale_events);
  c("hft_live_resubscribe_requests_total", "Resubscribe requests to the capture process.", st.resnap_requests);
  c("hft_live_model_checks_total", "Comparisons of the book with the independent model.", st.checks);
  c("hft_live_model_check_failures_total", "Failed comparisons.", st.check_failures);
  c("hft_live_final_mismatches_total", "Segments whose final book differed from the model.", st.final_mismatches);
  c("hft_live_flatten_failures_total", "Flattens that did not get flat.", st.flatten_failures);
  c("hft_live_orders_new_total", "New orders sent.", st.sent_new + (g ? g->sent_new : 0));
  c("hft_live_orders_replace_total", "Replaces sent.", st.sent_replace + (g ? g->sent_replace : 0));
  c("hft_live_orders_cancel_total", "Cancels sent.", st.sent_cancel + (g ? g->sent_cancel : 0));
  c("hft_live_fills_total", "Fills in finished segments.", st.fills);
  c("hft_live_exchange_reports_total", "Reports from the exchange applied.", st.exchange_reports);
  c("hft_live_exchange_reports_stale_total", "Reports for a segment that no longer exists.", st.reports_stale);
  c("hft_live_snapshots_dropped_ending_total", "Snapshots that arrived while a segment was still being flattened.", st.snapshots_dropped_ending);
  c("hft_live_risk_rejects_total", "Orders refused by risk.", st.risk_rejects + (g ? g->risk_rejects : 0));
  std::fprintf(f, "# HELP hft_live_segments_ended_total Segments ended, by reason.\n# TYPE hft_live_segments_ended_total counter\n");
  for (int r = 0; r < static_cast<int>(feed::EndReason::kCount); ++r)
    std::fprintf(f, "hft_live_segments_ended_total{reason=\"%s\"} %llu\n", feed::end_reason_name(static_cast<feed::EndReason>(r)),
                 static_cast<unsigned long long>(st.ended[r]));
  const core::Histogram& h = s.line_ns();
  std::fprintf(f, "# HELP hft_live_message_ns Wall-clock ns per update message (parse + convert + pipeline).\n# TYPE hft_live_message_ns gauge\n");
  if (h.count()) {
    std::fprintf(f, "hft_live_message_ns{q=\"0.5\"} %llu\nhft_live_message_ns{q=\"0.99\"} %llu\nhft_live_message_ns{q=\"1\"} %llu\n",
                 static_cast<unsigned long long>(h.quantile(0.5)), static_cast<unsigned long long>(h.quantile(0.99)),
                 static_cast<unsigned long long>(h.max()));
  }
  std::fclose(f);
  std::rename(tmp.c_str(), path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  feed::LiveConfig lc;
  lc.timed = true;
  std::string from, resnap_file, log_path, metrics_path, position_file, clock_out, clock_in;
  double metrics_s = 5.0, mid_sample_ms = 0.0;
  double pace = 0.0, report_s = 10.0, max_seconds = 0.0;
  bool spin = false;
  int orders_fd = -1, exchange_fd = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto num = [&](double& v) {
      if (i + 1 >= argc) return false;
      v = std::atof(argv[++i]);
      return true;
    };
    double v = 0;
    if (a == "--from" && i + 1 < argc) from = argv[++i];
    else if (a == "--pace" && num(v) && v > 0) pace = v;
    else if (a == "--stale-ms" && num(v) && v > 0) lc.stale_ns = static_cast<std::uint64_t>(v * 1e6);
    else if (a == "--rtt-us" && num(v) && v >= 0) lc.venue_rtt_ns = static_cast<std::uint64_t>(v * 1e3);
    else if (a == "--lots" && num(v) && v > 0) lc.mm_qty_lots = static_cast<std::int64_t>(v);
    else if (a == "--levels" && num(v) && v > 0) lc.l2.num_levels = static_cast<std::uint32_t>(v);
    else if (a == "--max-orders" && num(v) && v > 0) lc.max_orders = static_cast<std::uint32_t>(v);
    else if (a == "--check-every" && num(v) && v >= 0) lc.check_every = static_cast<std::uint32_t>(v);
    else if (a == "--report-s" && num(v) && v > 0) report_s = v;
    else if (a == "--max-seconds" && num(v) && v > 0) max_seconds = v;
    else if (a == "--resnap-file" && i + 1 < argc) resnap_file = argv[++i];
    else if (a == "--clock-out" && i + 1 < argc) clock_out = argv[++i];
    else if (a == "--clock-in" && i + 1 < argc) clock_in = argv[++i];
    else if (a == "--spin") spin = true;
    else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
    else if (a == "--metrics" && i + 1 < argc) metrics_path = argv[++i];
    else if (a == "--metrics-s" && num(v) && v > 0) metrics_s = v;
    else if (a == "--orders-fd" && num(v) && v >= 3 && v < 1024) orders_fd = static_cast<int>(v);
    else if (a == "--exchange-fd" && num(v) && v >= 3 && v < 1024) exchange_fd = static_cast<int>(v);
    else if (a == "--flatten-timeout-s" && num(v) && v > 0) lc.flatten_timeout_ns = static_cast<std::uint64_t>(v * 1e9);
    else if (a == "--mid-sample-ms" && num(v) && v >= 0) mid_sample_ms = v;
    else if (a == "--position-file" && i + 1 < argc) position_file = argv[++i];
    else if (a == "--close-position") lc.close_position = true;
    else if (a == "--close-collar-bps" && num(v) && v >= 1 && v <= 1000) lc.close_collar_bps = static_cast<std::int64_t>(v);
    else {
      std::fprintf(stderr,
                   "usage: hft_live [--from capture.l2 [--pace X]] [--stale-ms N] [--rtt-us N] [--lots N] [--levels N]\n"
                   "                [--max-orders N] [--check-every N] [--report-s S] [--max-seconds S] [--resnap-file PATH] [--spin] [--clock-out FILE | --from F --clock-in FILE]\n                [--log FILE|-] [--metrics FILE] [--metrics-s S] [--orders-fd N [--exchange-fd N] [--flatten-timeout-s S] [--close-position [--close-collar-bps N]] [--position-file PATH]] [--mid-sample-ms N]\n"
                   "reads capture-format lines from stdin unless --from is given\n");
      return 1;
    }
  }

  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;  // no SA_RESTART: a blocked poll() returns at once
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  OrderOut oout;
  if (orders_fd >= 0) {
    const int fl = fcntl(orders_fd, F_GETFL);
    if (fl < 0 || fcntl(orders_fd, F_SETFL, fl | O_NONBLOCK) < 0) {
      std::fprintf(stderr, "--orders-fd %d is not an open descriptor\n", orders_fd);
      return 1;
    }
    oout.fd = orders_fd;
    lc.order_tap = order_tap;
    lc.order_tap_ctx = &oout;
    oout.line("{\"t\":\"hello\",\"v\":1,\"px_scale\":10000,\"qty_scale\":100000}\n");
  }
  OrderOut* oo = orders_fd >= 0 ? &oout : nullptr;
  ReportIn rin;
  if ((!clock_out.empty() && (exchange_fd >= 0 || !from.empty())) || (!clock_in.empty() && from.empty())) {
    std::fprintf(stderr, "--clock-out records a live run without --exchange-fd (the exchange's reports are not recorded); --clock-in needs --from\n");
    return 1;
  }
  if (lc.close_position && exchange_fd < 0) {
    std::fprintf(stderr, "--close-position needs --exchange-fd (closing needs the exchange's reports)\n");
    return 1;
  }
  if (exchange_fd >= 0) {
    if (orders_fd < 0 || !from.empty()) {
      std::fprintf(stderr, "--exchange-fd needs --orders-fd and a live pipe (not --from)\n");
      return 1;
    }
    const int fl = fcntl(exchange_fd, F_GETFL);
    if (fl < 0 || fcntl(exchange_fd, F_SETFL, fl | O_NONBLOCK) < 0) {
      std::fprintf(stderr, "--exchange-fd %d is not an open descriptor\n", exchange_fd);
      return 1;
    }
    rin.fd = exchange_fd;
    lc.external_venue = true;
  }
  if (!position_file.empty() && exchange_fd < 0) {
    std::fprintf(stderr, "--position-file needs --exchange-fd (the position is only known from the exchange's reports)\n");
    return 1;
  }
  feed::LiveSession session(lc);
  long long saved_pos = 0, saved_cash = 0, saved_base = 0;
  // ADR 31: the position file also holds the account's base balance minus the position (the "baseline"), written once the
  // bridge has reported the balance; at the next start the balance must equal baseline + position, or we do not trade.
  bool baseline_known = false;
  long long baseline = 0;
  bool acct_pending = !position_file.empty();
  bool acct_failed = false;
  std::string acct_why;
  if (!position_file.empty()) {  // what the previous process left at the exchange (ADR 30)
    if (FILE* f = std::fopen(position_file.c_str(), "r")) {
      long long p = 0, c = 0, b = 0;
      const int n = std::fscanf(f, "%lld %lld %lld", &p, &c, &b);
      std::fclose(f);
      if (n != 2 && n != 3) {
        std::fprintf(stderr, "--position-file %s exists but is not \"<lots> <cash> [<baseline>]\"; refusing to start with an unknown position\n", position_file.c_str());
        return 1;
      }
      session.seed_position(p, c);
      saved_pos = p;
      saved_cash = c;
      if (n == 3) {
        baseline_known = true;
        baseline = saved_base = b;
      }
      std::fprintf(stderr, "hft_live: starting with a position of %lld lots (cash %lld) read from %s; make sure it matches the account\n", p, c, position_file.c_str());
    }
  }
  auto save_position = [&] {
    if (position_file.empty()) return;
    const long long p = session.net_position(), c = static_cast<long long>(session.net_cash());
    if (p == saved_pos && c == saved_cash && baseline == saved_base) return;
    const std::string tmp = position_file + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
      const bool ok = std::fprintf(f, "%lld %lld %lld\n", p, c, baseline) > 0;
      const bool cl = std::fclose(f) == 0;
      if (ok && cl && std::rename(tmp.c_str(), position_file.c_str()) == 0) {
        saved_pos = p;
        saved_cash = c;
        saved_base = baseline;
        return;
      }
    }
    static bool warned = false;
    if (!warned) std::fprintf(stderr, "hft_live: cannot write %s; the next start will not know the position\n", position_file.c_str());
    warned = true;
  };
  const std::uint64_t t_start = mono_ns();
  auto now_ns = [&] { return mono_ns() - t_start; };
  auto secs = [&] { return static_cast<double>(now_ns()) / 1e9; };
  double next_report = report_s, next_metrics = 0.0;
  std::uint64_t last_now = 0;
  EventLog elog;
  if (!log_path.empty()) {
    elog.open(log_path);
    if (!elog.ok()) {
      std::fprintf(stderr, "cannot open log %s\n", log_path.c_str());
      return 1;
    }
  }
  Observer obs;
  elog.emit("start", 0.0, std::string("\"mode\":\"") + (from.empty() ? "pipe" : "file") + "\"," + num("stale_ms", lc.stale_ns / 1000000));
  auto after_line = [&] {
    if (session.take_resnap()) touch(resnap_file);
    observe(elog, obs, session, secs(), oo);
    save_position();
  };
  bool exchange_lost = false, lost_handled = false;
  // The bridge's one report about the account (ADR 31). Without a position file it is not needed and is ignored.
  auto on_account = [&](bool ok, long long base_raw) {
    if (!acct_pending) return;
    acct_pending = false;
    const long long have = session.net_position();
    if (!ok) {
      acct_failed = true;
      acct_why = "the account's balance could not be read, so the position file cannot be checked";
    } else if (baseline_known) {
      const long long expect = baseline + have;
      const long long diff = base_raw - expect;
      if (diff > 1 || diff < -1) {
        acct_failed = true;
        acct_why = "the account holds " + std::to_string(base_raw) + " lots of the base currency, the position file implies " +
                   std::to_string(expect) + " (baseline " + std::to_string(baseline) + " + position " + std::to_string(have) + ")";
      }
    } else {
      baseline = base_raw - have;
      baseline_known = true;
      std::fprintf(stderr, "hft_live: account balance %lld lots recorded as the baseline in %s (position %lld)\n", base_raw, position_file.c_str(), have);
    }
    if (acct_failed) elog.emit("account_mismatch", secs(), std::string("\"why\":\"") + acct_why + "\"");
    else elog.emit("account_checked", secs(), std::string("\"base_lots\":") + std::to_string(base_raw));
  };
  // Twice the mid price in raw units (no division, so no rounding), or -1 when the book has no two sides (or no segment runs).
  auto mid_x2 = [&]() -> long long {
    const book::OrderBook* bk = session.book();
    if (!bk) return -1;
    const core::Price b = bk->best_bid(), a = bk->best_ask();
    if (b == core::Price::invalid() || a == core::Price::invalid()) return -1;
    return static_cast<long long>(b.raw()) + static_cast<long long>(a.raw());
  };
  // One event per fill the exchange reported (ADR 32): what traded, at what price, where the market was, what the position is now.
  // `applied` is 0 when the session ignored the report (a segment that no longer exists). Not on the hot path: this runs in
  // the report reader, after the report has been handed to the session.
  auto log_fill = [&](std::uint64_t seg, const sim::Report& r, std::string_view line, long long m2, bool applied) {
    std::string_view sd;
    const char* side = field_str(line, "side", sd) && sd == "sell" ? "sell" : "buy";
    // Liquidity as the bridge worked it out from the order type (ADR 35); left out when the report does not say.
    std::string_view lq;
    const std::string liq = field_str(line, "liq", lq) && (lq == "maker" || lq == "taker") ? ",\"liq\":\"" + std::string(lq) + "\"" : std::string();
    elog.emit("fill", secs(),
              num("seg", seg) + ",\"side\":\"" + side + "\"," + num("px", static_cast<unsigned long long>(r.price.raw())) + "," +
                  num("qty", static_cast<unsigned long long>(r.qty)) + ",\"mid_x2\":" + std::to_string(m2) +
                  ",\"pos\":" + std::to_string(session.net_position()) + ",\"applied\":" + (applied ? "1" : "0") + liq);
  };
  // Reads whatever reports are waiting and hands them to the session. A closed pipe means the bridge is gone.
  auto pump_reports = [&] {
    if (rin.fd < 0) return;
    char b[1 << 14];
    for (;;) {
      const ssize_t n = ::read(rin.fd, b, sizeof b);
      if (n > 0) {
        rin.carry.append(b, static_cast<std::size_t>(n));
        std::size_t pos = 0, nl;
        while ((nl = rin.carry.find('\n', pos)) != std::string::npos) {
          ++rin.lines;
          std::uint64_t seg = 0;
          sim::Report r;
          const std::string_view ln = std::string_view(rin.carry).substr(pos, nl - pos);
          std::string_view lt;
          if (field_str(ln, "t", lt) && lt == "acct") {
            std::int64_t ok = 0, br = 0;
            field_i64(ln, "ok", ok);
            field_i64(ln, "base_raw", br);
            on_account(ok == 1, br);
          } else if (parse_report(std::string_view(rin.carry).substr(pos, nl - pos), seg, r)) {
            const std::uint64_t before = session.stats().exchange_reports;
            const long long m2 = r.type == sim::ReportType::Fill && elog.ok() ? mid_x2() : -1;   // before: the fill can end the segment (a close)
            session.on_exchange_report(seg, r, now_ns());
            if (r.type == sim::ReportType::Fill && elog.ok()) log_fill(seg, r, ln, m2, session.stats().exchange_reports > before);
          } else {
            ++rin.bad;
          }
          pos = nl + 1;
        }
        rin.carry.erase(0, pos);
        if (rin.carry.size() > (1u << 16)) rin.carry.clear();
        continue;
      }
      if (n == 0) {
        ::close(rin.fd);
        rin.fd = -1;
        rin.lost = true;
      } else if (errno == EINTR) {
        continue;
      }
      break;
    }
  };
  double next_mid = 0.0;
  long long last_mid = -2;
  auto periodic = [&] {
    if (mid_sample_ms > 0 && elog.ok() && secs() >= next_mid) {
      next_mid = secs() + mid_sample_ms / 1e3;
      const long long m = mid_x2();
      if (m != last_mid && m > 0) {   // only changes, and only with a two-sided book
        last_mid = m;
        elog.emit("mid", secs(), "\"mid_x2\":" + std::to_string(m));
      }
    }
    if (secs() >= next_report) {
      status(session, secs());
      next_report += report_s;
    }
    if (!metrics_path.empty() && secs() >= next_metrics) {
      write_metrics(metrics_path, session, secs());
      next_metrics = secs() + metrics_s;
    }
  };

  // --clock-out: every time value handed to the session, in order (L line, P poll, K or S the end). With --from and
  // --clock-in the same values are replayed, so a live run can be reproduced exactly (ADR 48, 49).
  std::FILE* clock_f = nullptr;
  if (!clock_out.empty()) {
    clock_f = std::fopen(clock_out.c_str(), "w");
    if (!clock_f) {
      std::fprintf(stderr, "cannot write %s\n", clock_out.c_str());
      return 1;
    }
  }
  auto stamp = [&](char kind) {
    const std::uint64_t t = now_ns();
    if (clock_f) std::fprintf(clock_f, "%c %llu\n", kind, static_cast<unsigned long long>(t));
    return t;
  };

  if (!from.empty() && !clock_in.empty()) {
    std::ifstream in(from, std::ios::binary), ck(clock_in);
    if (!in || !ck) {
      std::fprintf(stderr, "cannot read %s\n", in ? clock_in.c_str() : from.c_str());
      return 1;
    }
    std::string line;
    char kind = 0, end_kind = 0;
    unsigned long long t = 0;
    while (!g_stop && ck >> kind >> t) {
      if (kind == 'L') {
        if (!std::getline(in, line)) {
          std::fprintf(stderr, "clock file has more lines than %s\n", from.c_str());
          return 1;
        }
        session.on_line(line, t);
      } else if (kind == 'P') {
        session.poll(t);
      } else {
        end_kind = kind;
        last_now = t;
        break;
      }
      last_now = t;
    }
    if (g_stop || end_kind == 'K') session.kill(last_now);
    else session.shutdown(last_now);
  } else if (!from.empty()) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "cannot read %s\n", from.c_str());
      return 1;
    }
    std::string line;
    std::uint64_t first = 0;
    bool have_first = false;
    while (!g_stop && std::getline(in, line)) {
      std::uint64_t t = stamp_of(line);
      if (t == 0) t = last_now;
      if (!have_first) {
        first = t;
        have_first = true;
      }
      if (pace > 0) {
        const double due = static_cast<double>(t - first) / 1e9 / pace;
        const double nowsec = secs();
        if (spin) {
          while (secs() < due) {}  // keep the core and its caches busy instead of sleeping
        } else if (due > nowsec) {
          std::this_thread::sleep_for(std::chrono::duration<double>(due - nowsec));
        }
      }
      session.on_line(line, t);
      last_now = t;
      after_line();
      periodic();
      if (max_seconds > 0 && secs() >= max_seconds) break;
    }
    // end of the file: whatever the caller did, the session ends flat
    if (g_stop) session.kill(last_now);
    else session.shutdown(last_now);
  } else {
    std::string carry;
    char buf[1 << 16];
    bool eof = false;
    while (!g_stop && !eof) {
      if (acct_pending && secs() > 10.0) on_account(false, 0);   // fail closed: no report about the account
      if (acct_failed) break;
      // until the account has been looked at, stdin is not read: no snapshot, no segment, no order
      pollfd p[2] = {{acct_pending ? -1 : 0, POLLIN, 0}, {rin.fd, POLLIN, 0}};  // a negative descriptor is ignored by poll
      const int r = ::poll(p, 2, spin ? 0 : 1);  // --spin: never sleep, the way a real feed handler polls
      if (r < 0) continue;  // EINTR: the loop condition looks at g_stop
      if (p[1].revents || rin.fd >= 0) pump_reports();
      if (p[0].revents) {
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n == 0) {
          eof = true;
        } else if (n > 0) {
          carry.append(buf, static_cast<std::size_t>(n));
          std::size_t pos = 0, nl;
          while ((nl = carry.find('\n', pos)) != std::string::npos) {
            session.on_line(std::string_view(carry).substr(pos, nl - pos), stamp('L'));
            pos = nl + 1;
            after_line();
          }
          carry.erase(0, pos);
          if (carry.size() > (1u << 22)) carry.clear();  // a "line" this long is not from our capture script
        }
      } else if (r == 0 || session.busy()) {
        session.poll(stamp('P'));
        after_line();
      }
      if (rin.lost && !lost_handled) {  // the bridge died: nobody will confirm anything; stop and say so
        lost_handled = exchange_lost = true;
        elog.emit("exchange_lost", secs());
        break;
      }
      after_line();
      periodic();
      if (max_seconds > 0 && secs() >= max_seconds) break;
    }
    last_now = stamp(g_stop || exchange_lost ? 'K' : 'S');
    if (g_stop || exchange_lost) session.kill(last_now);
    else session.shutdown(last_now);
    // External venue: the segment is over only when the exchange has confirmed the cancels (or the timeout passed).
    const double give_up = secs() + static_cast<double>(lc.flatten_timeout_ns) / 1e9 * (1 + (lc.close_position ? lc.close_max_attempts : 0)) + 2.0;
    while (session.busy() && secs() < give_up) {
      pollfd q{rin.fd, POLLIN, 0};
      ::poll(&q, 1, 2);
      pump_reports();
      session.poll(stamp('P'));
      after_line();
      oout.flush();
    }
  }

  observe(elog, obs, session, secs(), oo);
  oout.flush(2000);  // the final cancels must reach the bridge before the pipe closes
  if (oout.fd >= 0 && oout.len > 0) {  // the reader did not take them in two seconds: say so, the bridge's picture is incomplete
    oout.lose("unflushed_at_exit");
    if (!obs.lost_logged) elog.emit("orders_out_lost", secs(), std::string("\"why\":\"") + oout.lost_why + "\"");
  }
  write_metrics(metrics_path, session, secs());
  elog.emit("exit", secs(), std::string("\"cause\":\"") + (g_stop ? "signal" : "input_ended") + "\"");
  const feed::LiveStats& st = session.stats();
  std::printf("== hft_live summary (%s)\n", g_stop ? "stopped by signal: kill switch latched, all quotes cancelled" : "input ended");
  std::printf("  input: %llu lines, %llu feed messages (%llu other, %llu damaged), %llu unparsable lines, %llu markers, %llu before a snapshot\n",
              static_cast<unsigned long long>(st.lines), static_cast<unsigned long long>(st.messages),
              static_cast<unsigned long long>(st.other), static_cast<unsigned long long>(st.bad),
              static_cast<unsigned long long>(st.bad_lines), static_cast<unsigned long long>(st.markers),
              static_cast<unsigned long long>(st.dropped_no_segment));
  std::printf("  segments: %llu started", static_cast<unsigned long long>(st.segments));
  for (int r = 0; r < static_cast<int>(feed::EndReason::kCount); ++r)
    if (st.ended[r]) std::printf(", %llu ended: %s", static_cast<unsigned long long>(st.ended[r]), feed::end_reason_name(static_cast<feed::EndReason>(r)));
  std::printf("\n");
  std::printf("  guards: %llu stale-feed pulls, %llu resubscribe requests, %llu snapshots unusable, %llu clock regressions\n",
              static_cast<unsigned long long>(st.stale_events), static_cast<unsigned long long>(st.resnap_requests),
              static_cast<unsigned long long>(st.snapshots_unusable), static_cast<unsigned long long>(st.time_regressions));
  std::printf("  converter: %llu add, %llu reduce, %llu delete; %llu changes outside the window dropped; %llu crossed updates\n",
              static_cast<unsigned long long>(st.conv_adds), static_cast<unsigned long long>(st.conv_reduces),
              static_cast<unsigned long long>(st.conv_deletes), static_cast<unsigned long long>(st.out_of_window),
              static_cast<unsigned long long>(st.crossed));
  std::printf("  feed handler: %llu messages applied, desyncs %llu, book errors %llu\n",
              static_cast<unsigned long long>(st.messages_applied), static_cast<unsigned long long>(st.desyncs),
              static_cast<unsigned long long>(st.book_errors));
  std::printf("  model check: %llu comparisons, %llu failures; final book vs model: %llu mismatches\n",
              static_cast<unsigned long long>(st.checks), static_cast<unsigned long long>(st.check_failures),
              static_cast<unsigned long long>(st.final_mismatches));
  std::printf("  orders: %llu new, %llu replace, %llu cancel (%llu by pulls, %llu by kill), %llu risk rejects\n",
              static_cast<unsigned long long>(st.sent_new), static_cast<unsigned long long>(st.sent_replace),
              static_cast<unsigned long long>(st.sent_cancel), static_cast<unsigned long long>(st.pulled_cancels),
              static_cast<unsigned long long>(st.kill_cancels), static_cast<unsigned long long>(st.risk_rejects));
  std::printf("  paper venue (rtt %llu us, never fills): %llu accepted, %llu replaced, %llu canceled, %llu rejected as crossing\n",
              static_cast<unsigned long long>(lc.venue_rtt_ns / 1000), static_cast<unsigned long long>(st.acc),
              static_cast<unsigned long long>(st.rep), static_cast<unsigned long long>(st.canc),
              static_cast<unsigned long long>(st.rej_cross));
  if (lc.external_venue)
    std::printf("  exchange: %llu reports applied, %llu for a finished segment, %llu unparsable; %llu snapshots dropped while flattening, %llu feed messages ignored while flattening\n",
                static_cast<unsigned long long>(st.exchange_reports), static_cast<unsigned long long>(st.reports_stale),
                static_cast<unsigned long long>(rin.bad), static_cast<unsigned long long>(st.snapshots_dropped_ending),
                static_cast<unsigned long long>(st.ignored_ending));
  std::printf("  flatten: %llu failures; worst case %llu rounds, %llu us of simulated venue time\n",
              static_cast<unsigned long long>(st.flatten_failures), static_cast<unsigned long long>(st.max_flatten_rounds),
              static_cast<unsigned long long>(st.max_flatten_sim_ns / 1000));
  std::printf("  wall-clock cost per feed message that carried data (JSON parse + conversion + feed handler + book +\n"
              "  strategy + gateway, one process, steady clock incl. ~20-30 ns timer overhead; excludes network, TLS, capture):\n");
  print_hist("per update message", session.line_ns());
  print_hist("segment start (snapshot to ready)", session.start_ns());
  {
    // one number for the order flow the feed caused, to compare two runs or two compilers
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const feed::SegmentSummary& sm : session.summaries()) {
      h = (h ^ sm.hash_before_flatten) * 0x100000001B3ULL;
      h = (h ^ sm.frames_before_flatten) * 0x100000001B3ULL;
    }
    std::printf("  wire hash of the order flow the feed caused (flatten excluded): %016llx\n", static_cast<unsigned long long>(h));
  }
  for (std::size_t i = 0; i < session.summaries().size() && i < 20; ++i) {
    const feed::SegmentSummary& sm = session.summaries()[i];
    std::printf("  segment %zu: %llu packets, ended by %s, %s%s, book %s the model\n", i + 1,
                static_cast<unsigned long long>(sm.packets), feed::end_reason_name(sm.reason),
                sm.flat ? "flat" : "NOT FLAT", sm.kill_latched ? " (kill switch on)" : "",
                sm.book_matched_model ? "matched" : "DIFFERED FROM");
  }
  if (session.summaries().size() > 20) std::printf("  ... %zu more segments\n", session.summaries().size() - 20);

  if (acct_failed) {
    std::printf("  ACCOUNT CHECK FAILED: %s; not trading (delete %s to start from a new baseline once you have checked the account)\n", acct_why.c_str(), position_file.c_str());
    return 4;
  }
  if (lc.close_position && session.net_position() != 0) {
    // the session total (ADR 30): includes what earlier segments left, also when no segment is running now
    const feed::SegmentSummary* last = session.summaries().empty() ? nullptr : &session.summaries().back();
    const char* why = !last ? "no live segment to price a close"
                      : last->close_failed ? last->close_why_not
                                           : "closing did not run";
    std::printf("  POSITION NOT CLOSED: %lld lots left (%s)\n", static_cast<long long>(session.net_position()), why);
    elog.emit("position_not_closed", secs(), std::string("\"position_lots\":") + std::to_string(session.net_position()) + ",\"why\":\"" + why + "\"");
    return 4;  // risk is still open at the exchange: not restartable, an operator has to look
  }
  if (exchange_lost) return 6;  // restartable: a new bridge cancels whatever is left on the exchange
  if (st.check_failures || st.final_mismatches || st.flatten_failures || st.desyncs || st.book_errors) return 4;
  if (st.segments == 0) return 5;
  return 0;
}
