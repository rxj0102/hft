#pragma once
// Real market data, offline side: parse a recorded Coinbase-style level-2 feed
// (JSON text) and convert it into the engine's own ITCH-style events.
//
// What the feed is: aggregated price levels ("this price now has this much"),
// not individual orders, and the update messages carry no sequence number. The
// engine's book is order-by-order, so each live price level is represented by
// ONE synthetic order whose id is private to the converter:
//   level appears              -> Add
//   level size goes down       -> Reduce (partial cancel by the difference)
//   level size goes up         -> Delete + Add (a new synthetic order; the
//                                 ITCH-style wire has no replace)
//   level size goes to zero    -> Delete
// There is no queue position in this data, so nothing here can say where an
// order of ours would stand; that is a property of the source, not a bug.
//
// Units: prices are scaled by 1e4 (raw 820490000 = 82049.0000), sizes by 1e5
// (one lot = 0.00001 of the base asset; sub-lot size is dropped, counted).
//
// This is setup / tooling code (allocates freely). The hot path never sees JSON.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "hft/core/types.hpp"
#include "hft/sim/types.hpp"

namespace hft::feed {

inline constexpr int kPriceDecimals = 4;
inline constexpr int kSizeDecimals = 5;

// ---- decimal text -> scaled integer -----------------------------------------------------------

// Parses "123.4500" into value * 10^decimals. Digits past `decimals` are
// dropped (floor); `truncated` says whether any dropped digit was non-zero.
// False for empty text, signs, exponents, a second dot, non-digits, or a value
// that would not fit comfortably in an int64.
inline bool parse_decimal(std::string_view s, int decimals, std::int64_t& out, bool& truncated) noexcept {
  truncated = false;
  if (s.empty()) return false;
  std::int64_t whole = 0, frac = 0;
  int frac_digits = 0, int_digits = 0;
  bool dot = false, any = false;
  for (const char c : s) {
    if (c == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    any = true;
    const int d = c - '0';
    if (!dot) {
      if (++int_digits > 12) return false;
      whole = whole * 10 + d;
    } else if (frac_digits < decimals) {
      frac = frac * 10 + d;
      ++frac_digits;
    } else if (d != 0) {
      truncated = true;
    }
  }
  if (!any) return false;
  for (; frac_digits < decimals; ++frac_digits) frac *= 10;
  std::int64_t scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  out = whole * scale + frac;
  return true;
}

// ---- a small JSON walker, just enough for the two message shapes -------------------------------

namespace detail {

struct Cursor {
  const char* p;
  const char* e;
  void ws() noexcept {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool eat(char c) noexcept {
    ws();
    if (p < e && *p == c) {
      ++p;
      return true;
    }
    return false;
  }
  bool peek(char c) noexcept {
    ws();
    return p < e && *p == c;
  }
  // A string without escapes handled beyond skipping them (the fields we read are plain).
  bool str(std::string_view& out) noexcept {
    ws();
    if (p >= e || *p != '"') return false;
    const char* b = ++p;
    while (p < e && *p != '"') {
      if (*p == '\\') ++p;
      ++p;
    }
    if (p >= e) return false;
    out = std::string_view(b, static_cast<std::size_t>(p - b));
    ++p;
    return true;
  }
  // Skips any JSON value, depth-limited so hostile input cannot recurse deeply.
  bool skip(int depth = 0) noexcept {
    ws();
    if (p >= e || depth > 8) return false;
    const char c = *p;
    if (c == '"') {
      std::string_view t;
      return str(t);
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++p;
      if (eat(close)) return true;
      for (;;) {
        if (c == '{') {
          std::string_view k;
          if (!str(k) || !eat(':')) return false;
        }
        if (!skip(depth + 1)) return false;
        if (eat(',')) continue;
        return eat(close);
      }
    }
    // number or literal
    const char* b = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') ++p;
    return p > b;
  }
};

}  // namespace detail

// ---- one parsed message -------------------------------------------------------------------------

struct L2Change {
  core::Side side;
  std::int64_t price_raw;  // price * 1e4
  std::int64_t lots;       // size * 1e5, floored; 0 means the level is gone
  bool sub_lot;            // size had digits finer than one lot
  bool price_inexact;      // price had digits finer than 1e-4
};

struct L2Message {
  enum class Kind : std::uint8_t { Other, Snapshot, Update, Bad };
  Kind kind = Kind::Other;
  std::vector<L2Change> changes;
};

namespace detail {
// [["price","size"], ...] -> changes on `side`. Rows may carry extra elements.
inline bool read_levels(Cursor& c, core::Side side, L2Message& m) {
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  for (;;) {
    if (!c.eat('[')) return false;
    std::string_view ps, ss;
    if (!c.str(ps) || !c.eat(',') || !c.str(ss)) return false;
    while (c.eat(',')) {
      if (!c.skip(2)) return false;
    }
    if (!c.eat(']')) return false;
    L2Change ch{};
    ch.side = side;
    bool t1 = false, t2 = false;
    if (!parse_decimal(ps, kPriceDecimals, ch.price_raw, t1) || !parse_decimal(ss, kSizeDecimals, ch.lots, t2))
      return false;
    ch.price_inexact = t1;
    ch.sub_lot = t2;
    m.changes.push_back(ch);
    if (c.eat(',')) continue;
    return c.eat(']');
  }
}
// [["buy","price","size"], ...]
inline bool read_changes(Cursor& c, L2Message& m) {
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  for (;;) {
    if (!c.eat('[')) return false;
    std::string_view sd, ps, ss;
    if (!c.str(sd) || !c.eat(',') || !c.str(ps) || !c.eat(',') || !c.str(ss)) return false;
    while (c.eat(',')) {
      if (!c.skip(2)) return false;
    }
    if (!c.eat(']')) return false;
    L2Change ch{};
    if (sd == "buy") {
      ch.side = core::Side::Buy;
    } else if (sd == "sell") {
      ch.side = core::Side::Sell;
    } else {
      return false;
    }
    bool t1 = false, t2 = false;
    if (!parse_decimal(ps, kPriceDecimals, ch.price_raw, t1) || !parse_decimal(ss, kSizeDecimals, ch.lots, t2))
      return false;
    ch.price_inexact = t1;
    ch.sub_lot = t2;
    m.changes.push_back(ch);
    if (c.eat(',')) continue;
    return c.eat(']');
  }
}
}  // namespace detail

// Parses one raw JSON message. Messages of other types (subscriptions,
// heartbeats, errors) come back as Kind::Other with no changes; text that is
// not a well-formed message of a known type comes back as Kind::Bad.
inline L2Message parse_l2(std::string_view text) {
  L2Message m;
  detail::Cursor c{text.data(), text.data() + text.size()};
  if (!c.eat('{')) {
    m.kind = L2Message::Kind::Bad;
    return m;
  }
  std::string_view type;
  bool have_type = false, ok = true;
  bool saw_changes = false, saw_bids = false, saw_asks = false;
  if (!c.eat('}')) {
    for (;;) {
      std::string_view key;
      if (!c.str(key) || !c.eat(':')) {
        ok = false;
        break;
      }
      if (key == "type") {
        have_type = c.str(type);
        if (!have_type) {
          ok = false;
          break;
        }
      } else if (key == "changes") {
        saw_changes = true;
        if (!detail::read_changes(c, m)) {
          ok = false;
          break;
        }
      } else if (key == "bids") {
        saw_bids = true;
        if (!detail::read_levels(c, core::Side::Buy, m)) {
          ok = false;
          break;
        }
      } else if (key == "asks") {
        saw_asks = true;
        if (!detail::read_levels(c, core::Side::Sell, m)) {
          ok = false;
          break;
        }
      } else if (!c.skip()) {
        ok = false;
        break;
      }
      if (c.eat(',')) continue;
      ok = c.eat('}');
      break;
    }
  }
  if (!ok || !have_type) {
    m.kind = L2Message::Kind::Bad;
    m.changes.clear();
    return m;
  }
  if (type == "snapshot") {
    m.kind = (saw_bids || saw_asks) ? L2Message::Kind::Snapshot : L2Message::Kind::Bad;
  } else if (type == "l2update") {
    m.kind = saw_changes ? L2Message::Kind::Update : L2Message::Kind::Bad;
  } else {
    m.kind = L2Message::Kind::Other;
    m.changes.clear();
  }
  if (m.kind == L2Message::Kind::Bad) m.changes.clear();
  return m;
}

// ---- conversion to the engine's events ---------------------------------------------------------------

struct L2Config {
  std::int64_t tick_raw = 100;         // 100 raw = 0.01
  std::uint32_t num_levels = 65536;    // the grid: a window of num_levels * tick around the first mid
  std::uint32_t margin_pct = 10;       // best price this close to a window edge ends the segment
};

struct L2Stats {
  std::uint64_t snapshots = 0, updates = 0, changes = 0;
  std::uint64_t adds = 0, reduces = 0, deletes = 0;
  std::uint64_t unchanged = 0;       // same size as before
  std::uint64_t off_tick = 0;        // price not on the tick grid: dropped
  std::uint64_t out_of_window = 0;   // price outside the grid: dropped
  std::uint64_t unknown_delete = 0;  // size 0 for a level we never had
  std::uint64_t sub_lot = 0;         // size finer than one lot: floored
  std::uint64_t clamped = 0;         // size larger than the wire's 32 bits: clamped
  std::uint64_t price_inexact = 0;   // price finer than 1e-4: floored (and then probably off tick)
  std::uint64_t crossed = 0;         // updates that left best bid >= best ask
  std::uint64_t window_exits = 0;
  std::uint64_t peak_levels = 0;
};

// Turns a snapshot and the updates after it into PublicEvents. One converter =
// one segment (one price grid). Not copyable in spirit: construct a new one
// after a reconnect.
class L2Converter {
 public:
  explicit L2Converter(const L2Config& cfg = L2Config{}) : cfg_(cfg) {}

  const L2Config& config() const noexcept { return cfg_; }
  const L2Stats& stats() const noexcept { return stats_; }
  bool started() const noexcept { return started_; }
  bool window_exit() const noexcept { return exit_; }
  std::int64_t base_raw() const noexcept { return base_idx_ * cfg_.tick_raw; }

  // The converter's own picture of the aggregated book: price index -> lots.
  // This is the independent model the replay is checked against.
  const std::map<std::int64_t, std::int64_t>& levels(core::Side s) const noexcept { return lv_[idx(s)]; }
  std::int64_t price_of(std::int64_t level_idx) const noexcept { return level_idx * cfg_.tick_raw; }
  std::size_t live_levels() const noexcept { return lv_[0].size() + lv_[1].size(); }

  // Starts the segment. The grid is centred on the snapshot's mid. False if the
  // snapshot lacks one side (no mid to centre on).
  template <class Emit>
  bool on_snapshot(const L2Message& m, std::uint64_t ts, Emit&& emit) {
    ++stats_.snapshots;
    std::int64_t best_bid = -1, best_ask = -1;
    for (const L2Change& c : m.changes) {
      if (c.lots <= 0 || c.price_raw % cfg_.tick_raw != 0) continue;
      const std::int64_t i = c.price_raw / cfg_.tick_raw;
      if (c.side == core::Side::Buy) {
        if (best_bid < 0 || i > best_bid) best_bid = i;
      } else if (best_ask < 0 || i < best_ask) {
        best_ask = i;
      }
    }
    if (best_bid < 0 || best_ask < 0) return false;
    const std::int64_t mid = (best_bid + best_ask) / 2;
    base_idx_ = mid - static_cast<std::int64_t>(cfg_.num_levels / 2);
    if (base_idx_ < 0) base_idx_ = 0;
    lv_[0].clear();
    lv_[1].clear();
    ids_[0].clear();
    ids_[1].clear();
    started_ = true;
    exit_ = false;
    apply(m, ts, emit, false);
    return true;
  }

  template <class Emit>
  void on_update(const L2Message& m, std::uint64_t ts, Emit&& emit) {
    if (!started_) return;
    ++stats_.updates;
    apply(m, ts, emit, true);
  }

 private:
  static int idx(core::Side s) noexcept { return s == core::Side::Buy ? 0 : 1; }

  template <class Emit>
  void emit_event(Emit& emit, sim::PublicType t, core::Side s, core::OrderId id, std::int64_t px_idx, std::int64_t qty,
                  std::uint64_t ts) {
    sim::PublicEvent e{};
    e.type = t;
    e.side = s;
    e.id = id;
    e.price = core::Price::from_raw(px_idx * cfg_.tick_raw);
    e.qty = qty;
    e.ts = ts;
    emit(e);
  }

  template <class Emit>
  void apply(const L2Message& m, std::uint64_t ts, Emit& emit, bool incremental) {
    constexpr std::int64_t kWireMax = 0xFFFFFFFFLL;
    for (const L2Change& c : m.changes) {
      ++stats_.changes;
      if (c.price_inexact) ++stats_.price_inexact;
      if (c.sub_lot) ++stats_.sub_lot;
      if (c.price_raw % cfg_.tick_raw != 0) {
        ++stats_.off_tick;
        continue;
      }
      const std::int64_t i = c.price_raw / cfg_.tick_raw;
      if (i < base_idx_ || i >= base_idx_ + static_cast<std::int64_t>(cfg_.num_levels)) {
        ++stats_.out_of_window;
        continue;
      }
      std::int64_t lots = c.lots;
      if (lots > kWireMax) {
        lots = kWireMax;
        ++stats_.clamped;
      }
      auto& book = lv_[idx(c.side)];
      auto& ids = ids_[idx(c.side)];
      auto it = book.find(i);
      const std::int64_t have = it == book.end() ? 0 : it->second;
      if (lots == have) {
        if (lots == 0 && incremental) ++stats_.unknown_delete;
        else if (lots != 0) ++stats_.unchanged;
        continue;
      }
      if (lots == 0) {
        emit_event(emit, sim::PublicType::Delete, c.side, ids[i], i, have, ts);
        ids.erase(i);
        book.erase(i);
        ++stats_.deletes;
      } else if (have == 0) {
        const core::OrderId id = ++next_id_;
        emit_event(emit, sim::PublicType::Add, c.side, id, i, lots, ts);
        ids[i] = id;
        book[i] = lots;
        ++stats_.adds;
      } else if (lots < have) {
        emit_event(emit, sim::PublicType::Reduce, c.side, ids[i], i, have - lots, ts);
        it->second = lots;
        ++stats_.reduces;
      } else {
        emit_event(emit, sim::PublicType::Delete, c.side, ids[i], i, have, ts);
        const core::OrderId id = ++next_id_;
        emit_event(emit, sim::PublicType::Add, c.side, id, i, lots, ts);
        ids[i] = id;
        it->second = lots;
        ++stats_.deletes;
        ++stats_.adds;
      }
    }
    const std::uint64_t n = live_levels();
    if (n > stats_.peak_levels) stats_.peak_levels = n;
    check_window();
  }

  void check_window() {
    if (lv_[0].empty() || lv_[1].empty()) return;
    const std::int64_t bid = lv_[0].rbegin()->first;
    const std::int64_t ask = lv_[1].begin()->first;
    if (bid >= ask) ++stats_.crossed;
    const std::int64_t margin = static_cast<std::int64_t>(cfg_.num_levels) * cfg_.margin_pct / 100;
    const std::int64_t lo = base_idx_ + margin, hi = base_idx_ + static_cast<std::int64_t>(cfg_.num_levels) - margin;
    if (!exit_ && (bid < lo || ask >= hi)) {
      exit_ = true;
      ++stats_.window_exits;
    }
  }

  L2Config cfg_;
  L2Stats stats_;
  bool started_ = false, exit_ = false;
  std::int64_t base_idx_ = 0;
  core::OrderId next_id_ = 0;
  std::map<std::int64_t, std::int64_t> lv_[2];
  std::map<std::int64_t, core::OrderId> ids_[2];
};

}  // namespace hft::feed
