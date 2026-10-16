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

struct Observer {
  std::uint64_t segments = 0, stale = 0, resnaps = 0, sums = 0, mirrored = 0, bad_lines = 0, regress = 0;
  bool halted = false, lost_logged = false;
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
                    num("flatten_sim_ns", sm.flatten_sim_ns) + "," + num("open_before_flatten", sm.open_before_flatten);
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
  std::string from, resnap_file, log_path, metrics_path;
  double metrics_s = 5.0;
  double pace = 0.0, report_s = 10.0, max_seconds = 0.0;
  bool spin = false;
  int orders_fd = -1;
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
    else if (a == "--spin") spin = true;
    else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
    else if (a == "--metrics" && i + 1 < argc) metrics_path = argv[++i];
    else if (a == "--metrics-s" && num(v) && v > 0) metrics_s = v;
    else if (a == "--orders-fd" && num(v) && v >= 3 && v < 1024) orders_fd = static_cast<int>(v);
    else {
      std::fprintf(stderr,
                   "usage: hft_live [--from capture.l2 [--pace X]] [--stale-ms N] [--rtt-us N] [--lots N] [--levels N]\n"
                   "                [--max-orders N] [--check-every N] [--report-s S] [--max-seconds S] [--resnap-file PATH] [--spin]\n                [--log FILE|-] [--metrics FILE] [--metrics-s S] [--orders-fd N]\n"
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
  feed::LiveSession session(lc);
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
  };
  auto periodic = [&] {
    if (secs() >= next_report) {
      status(session, secs());
      next_report += report_s;
    }
    if (!metrics_path.empty() && secs() >= next_metrics) {
      write_metrics(metrics_path, session, secs());
      next_metrics = secs() + metrics_s;
    }
  };

  if (!from.empty()) {
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
      pollfd p{0, POLLIN, 0};
      const int r = ::poll(&p, 1, spin ? 0 : 1);  // --spin: never sleep, the way a real feed handler polls
      if (r < 0) continue;  // EINTR: the loop condition looks at g_stop
      if (r > 0) {
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n == 0) {
          eof = true;
        } else if (n > 0) {
          carry.append(buf, static_cast<std::size_t>(n));
          std::size_t pos = 0, nl;
          while ((nl = carry.find('\n', pos)) != std::string::npos) {
            session.on_line(std::string_view(carry).substr(pos, nl - pos), now_ns());
            pos = nl + 1;
            after_line();
          }
          carry.erase(0, pos);
          if (carry.size() > (1u << 22)) carry.clear();  // a "line" this long is not from our capture script
        }
      } else {
        session.poll(now_ns());
        after_line();
      }
      periodic();
      if (max_seconds > 0 && secs() >= max_seconds) break;
    }
    last_now = now_ns();
    if (g_stop) session.kill(last_now);
    else session.shutdown(last_now);
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

  if (st.check_failures || st.final_mismatches || st.flatten_failures || st.desyncs || st.book_errors) return 4;
  if (st.segments == 0) return 5;
  return 0;
}
