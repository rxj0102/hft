#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "alloc_trap.hpp"
#include "check.hpp"
#include "hft/core/fixed_point.hpp"
#include "hft/core/logger.hpp"
#include "hft/core/memory.hpp"
#include "hft/core/spsc_ring.hpp"
#include "hft/core/types.hpp"

using namespace hft::core;

static void test_fixed_point() {
  const Price p = Price::from_units(101, 2500);  // 101.2500
  CHECK(p.raw() == 1'012'500);
  CHECK(p.to_double() > 101.24 && p.to_double() < 101.26);
  CHECK(Price::from_units(1) < Price::from_units(2));
  CHECK(Price::from_units(5) + Price::from_units(3) == Price::from_units(8));
  CHECK(!Price::invalid().valid());

  // 1e12 raw * 1e12 qty = 1e24: wraps in int64, exact in i128.
  const i128 n = notional_raw(Price::from_raw(1'000'000'000'000LL), 1'000'000'000'000LL);
  CHECK(n == static_cast<i128>(1'000'000'000'000LL) * 1'000'000'000'000LL);
  CHECK(saturate_i64(n) == INT64_MAX);
  CHECK(saturate_i64(-n) == INT64_MIN);
  CHECK(saturate_i64(12345) == 12345);

  const Price ref = Price::from_units(100);
  CHECK(within_bps(Price::from_units(101), ref, 100));   // exactly +100 bps
  CHECK(!within_bps(Price::from_units(102), ref, 100));  // +200 bps
  CHECK(within_bps(Price::from_units(99), ref, 100));    // exactly -100 bps
  CHECK(opposite(Side::Buy) == Side::Sell);
  CHECK(sign(Side::Sell) == -1);
}

static void test_arena() {
  alignas(64) static std::byte buf[1024];
  Arena a(buf, sizeof buf);
  void* p1 = a.allocate(1, 1);
  void* p2 = a.allocate(8, 64);
  CHECK(p1 && p2);
  CHECK(reinterpret_cast<std::uintptr_t>(p2) % 64 == 0);
  CHECK(a.allocate(2000) == nullptr);  // exhaustion returns null, no throw
  const auto used = a.used();
  CHECK(used > 0);
  a.reset();
  CHECK(a.used() == 0);
  CHECK(a.allocate(1024, 1) != nullptr);
}

struct Obj {
  std::uint64_t a, b;
  Obj(std::uint64_t x, std::uint64_t y) : a(x), b(y) {}
};

static void test_pool() {
  alignas(64) static std::byte buf[64 * 1024];
  Arena a(buf, sizeof buf);
  Pool<Obj> pool;
  CHECK(pool.init(a, 8));
  std::vector<Obj*> v;
  for (std::uint64_t i = 0; i < 8; ++i) {
    Obj* o = pool.emplace(i, i * 2);
    CHECK(o != nullptr);
    CHECK(o->b == i * 2);
    v.push_back(o);
  }
  CHECK(pool.in_use() == 8);
  CHECK(pool.emplace(0u, 0u) == nullptr);  // full
  Obj* freed = v[3];
  const auto idx = pool.index_of(freed);
  CHECK(pool.at(idx) == freed);
  pool.release(freed);
  CHECK(pool.in_use() == 7);
  Obj* again = pool.emplace(9u, 9u);
  CHECK(again == freed);  // LIFO reuse of the most recently freed slot
  Pool<Obj> tiny;
  CHECK(!tiny.init(a, 1u << 30));  // arena too small
}

static void test_region() {
  Region r = Region::create(4u << 20, RegionOptions{});
  CHECK(static_cast<bool>(r));
  CHECK(r.size() >= (4u << 20));
  CHECK(r.data()[0] == std::byte{0} && r.data()[r.size() - 1] == std::byte{0});
  const auto& rep = r.report();
  std::printf("region: huge_explicit=%d huge_thp_hint=%d locked=%d numa_bound=%d\n",
              rep.huge_explicit, rep.huge_thp_hint, rep.locked, rep.numa_bound);
  Region moved = std::move(r);
  CHECK(!r && static_cast<bool>(moved));
}

static void test_ring_single_thread() {
  static SpscRing<std::uint64_t, 8> ring;
  std::uint64_t out = 0;
  CHECK(!ring.try_pop(out));  // empty
  for (std::uint64_t i = 0; i < 8; ++i) CHECK(ring.try_push(i));
  CHECK(!ring.try_push(99));  // full
  CHECK(ring.size_approx() == 8);
  for (std::uint64_t i = 0; i < 8; ++i) {
    CHECK(ring.try_pop(out));
    CHECK(out == i);  // FIFO
  }
  // Wrap many times to cross the mask boundary.
  for (std::uint64_t i = 0; i < 1000; ++i) {
    CHECK(ring.try_push(i));
    CHECK(ring.try_pop(out) && out == i);
  }
  // Zero-copy API.
  std::uint64_t* slot = ring.claim();
  CHECK(slot != nullptr);
  *slot = 777;
  ring.publish();
  const std::uint64_t* s = ring.peek();
  CHECK(s && *s == 777);
  ring.pop();
  CHECK(ring.peek() == nullptr);
}

static void test_ring_two_threads() {
  // A deliberately small ring forces constant full/empty transitions, which is
  // where ordering bugs show up (and what TSan is best at catching).
  static SpscRing<std::uint64_t, 64> ring;
  constexpr std::uint64_t kN = 2'000'000;
  std::atomic<bool> ok{true};
  std::uint64_t sum = 0;
  std::thread cons([&] {
    std::uint64_t expect = 0, v = 0;
    while (expect < kN) {
      if (ring.try_pop(v)) {
        if (v != expect) ok.store(false, std::memory_order_relaxed);
        sum += v;
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kN; ++i)
    while (!ring.try_push(i)) {
    }
  cons.join();
  CHECK(ok.load());
  CHECK(sum == kN * (kN - 1) / 2);
}

static void test_logger_roundtrip() {
  const char* path = "/tmp/hft_test_log.bin";
  static BinaryLogger<(1u << 16)> lg;
  CHECK(lg.start(path));
  constexpr std::uint64_t kN = 20000;
  for (std::uint64_t i = 0; i < kN; ++i) {
    struct P { std::uint64_t a; std::uint32_t b; } p{i, static_cast<std::uint32_t>(i * 3)};
    // Pace the producer so the 64K ring cannot overflow on a slow flusher.
    while (!lg.log(static_cast<std::uint16_t>(i % 7), &p, sizeof p)) std::this_thread::yield();
  }
  lg.stop();
  // dropped() counts failed attempts, which the producer retried above, so it
  // is not asserted here; every record must still arrive exactly once.
  std::uint64_t n = 0, prev_seq = 0;
  bool contiguous = true, payload_ok = true;
  const long got = read_log(path, [&](const LogRecord& r) {
    if (n > 0 && r.seq <= prev_seq) contiguous = false;
    prev_seq = r.seq;
    std::uint64_t a = 0;
    std::memcpy(&a, r.payload, 8);
    // The logged struct is {u64, u32} padded to 16 bytes by the compiler.
    if (r.len != 16 || r.type != a % 7) payload_ok = false;
    ++n;
  });
  CHECK(got == static_cast<long>(kN));
  CHECK(lg.written() == kN);
  CHECK(contiguous && payload_ok);
  CHECK(read_log("/nonexistent/path", [](const LogRecord&) {}) == -1);
  std::remove(path);
}

static void test_zero_alloc_hot_path() {
  // Setup (allowed to allocate): everything the hot loop will touch.
  static SpscRing<std::uint64_t, 1024> ring;
  alignas(64) static std::byte buf[1 << 16];
  Arena arena(buf, sizeof buf);
  Pool<Obj> pool;
  CHECK(pool.init(arena, 64));
  static BinaryLogger<(1u << 12)> lg;
  CHECK(lg.start("/tmp/hft_alloc_trap_log.bin"));

  // Prove the trap itself works: a deliberate allocation must be counted.
  alloc_trap::arm();
  void* probe = ::operator new(8);
  asm volatile("" : : "r"(probe) : "memory");
  ::operator delete(probe);
  CHECK(alloc_trap::disarm() == 1);

  // Hot path: ring, pool, arena, logger. Nothing here may allocate.
  alloc_trap::arm();
  std::uint64_t out = 0, acc = 0;
  for (std::uint64_t i = 0; i < 100000; ++i) {
    ring.try_push(i);
    ring.try_pop(out);
    acc += out;
    Obj* o = pool.emplace(i, i);
    acc += o->a;
    pool.release(o);
    if ((i & 1023) == 0) lg.log(1, &i, sizeof i);
    (void)arena.allocate(8, 8);
    if ((i & 255) == 255) arena.reset();
  }
  const std::uint64_t allocs = alloc_trap::disarm();
  CHECK(allocs == 0);
  std::printf("hot-path allocations: %llu (acc=%llu)\n", static_cast<unsigned long long>(allocs),
              static_cast<unsigned long long>(acc));
  lg.stop();
  std::remove("/tmp/hft_alloc_trap_log.bin");
}

int main() {
  test_fixed_point();
  test_arena();
  test_pool();
  test_region();
  test_ring_single_thread();
  test_ring_two_threads();
  test_logger_roundtrip();
  test_zero_alloc_hot_path();
  if (g_failures == 0) std::puts("test_phase1: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
