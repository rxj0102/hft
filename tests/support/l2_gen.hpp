#pragma once
// Synthetic level-2 captures for tests and drills. Prices and sizes are invented
// (a random walk around 82,000.00); nothing here is exchange data.
#include <cstdint>
#include <cstdio>
#include <string>

namespace l2gen {

struct Rng {
  std::uint64_t s;
  std::uint64_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 17;
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
};

std::string fmt_px(std::int64_t cents) {
  char b[40];
  std::snprintf(b, sizeof b, "%lld.%02lld", static_cast<long long>(cents / 100), static_cast<long long>(cents % 100));
  return b;
}
std::string fmt_sz(std::int64_t n8) {  // size in 1e-8 units, to exercise sub-lot digits
  char b[40];
  std::snprintf(b, sizeof b, "%lld.%08lld", static_cast<long long>(n8 / 100000000), static_cast<long long>(n8 % 100000000));
  return b;
}

// A capture with `segments` segments: a snapshot, many updates (growth, shrink,
// removal, new levels, junk), heartbeats and bad lines mixed in.
std::string make_capture(std::uint64_t seed, int segments, int updates_per_segment) {
  Rng r{seed};
  std::string out = "#100\tstart\turl\tBTC-USD\n";
  std::uint64_t t = 1000;
  for (int s = 0; s < segments; ++s) {
    std::int64_t mid = 8200000 + static_cast<std::int64_t>(r.below(100000));  // cents
    std::string snap = R"({"type":"snapshot","product_id":"BTC-USD","bids":[)";
    for (int i = 0; i < 40; ++i) {
      if (i) snap += ",";
      snap += "[\"" + fmt_px(mid - 1 - i * (1 + static_cast<std::int64_t>(r.below(3)))) + "\",\"" + fmt_sz(static_cast<std::int64_t>(r.below(900000000)) + 1) + "\"]";
    }
    snap += R"(],"asks":[)";
    for (int i = 0; i < 40; ++i) {
      if (i) snap += ",";
      snap += "[\"" + fmt_px(mid + 1 + i * (1 + static_cast<std::int64_t>(r.below(3)))) + "\",\"" + fmt_sz(static_cast<std::int64_t>(r.below(900000000)) + 1) + "\"]";
    }
    snap += "]}";
    out += std::to_string(t += 1000) + "\t" + snap + "\n";
    for (int u = 0; u < updates_per_segment; ++u) {
      if (r.below(50) == 0) out += std::to_string(t += 100) + "\t{\"type\":\"heartbeat\",\"sequence\":1}\n";
      if (r.below(200) == 0) out += std::to_string(t += 100) + "\t{\"type\":\"l2update\",\"changes\":[[\"buy\",\n";  // damaged
      if (r.below(300) == 0) out += "garbage without a tab\n";
      std::string m = R"({"type":"l2update","product_id":"BTC-USD","changes":[)";
      const int n = 1 + static_cast<int>(r.below(6));
      for (int k = 0; k < n; ++k) {
        if (k) m += ",";
        const bool buy = r.below(2) == 0;
        const std::int64_t off = static_cast<std::int64_t>(r.below(60));
        const std::int64_t px = buy ? mid - 1 - off : mid + 1 + off;
        std::int64_t sz = 0;
        switch (r.below(4)) {
          case 0: sz = 0; break;
          case 1: sz = static_cast<std::int64_t>(r.below(900000000)) + 1; break;
          case 2: sz = static_cast<std::int64_t>(r.below(1000)); break;  // sub-lot or zero lots
          default: sz = static_cast<std::int64_t>(r.below(50000000)) + 1; break;
        }
        m += std::string("[\"") + (buy ? "buy" : "sell") + "\",\"" + fmt_px(px) + "\",\"" + fmt_sz(sz) + "\"]";
      }
      m += "],\"time\":\"t\"}";
      out += std::to_string(t += 50 + r.below(500)) + "\t" + m + "\n";
    }
    out += "#" + std::to_string(t += 10) + "\treconnect\n";
  }
  out += "#" + std::to_string(t) + "\tstop\n";
  return out;
}

}  // namespace l2gen
