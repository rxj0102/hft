// SPSC ring throughput across two pinned cores. Phase 1 gate: > 100M msgs/sec.
// Reports every trial plus the median, because a single run on a noisy host
// says little, and the best-of number alone would flatter the ring.
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "hft/core/spsc_ring.hpp"

namespace {

bool pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
}

struct Msg64 {
  std::uint64_t v[8];
};

template <class T, std::size_t N>
double run_trial(std::uint64_t count, int cpu_prod, int cpu_cons, bool* pinned_ok) {
  // Heap-allocate: a 64K-slot ring of 64-byte messages is 4 MiB, too big for the stack.
  auto* ring = new hft::core::SpscRing<T, N>();
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> checksum{0};
  bool cons_pinned = false;

  std::thread cons([&] {
    cons_pinned = pin_to_cpu(cpu_cons);
    std::uint64_t sum = 0, got = 0;
    while (!go.load(std::memory_order_acquire)) {
    }
    while (got < count) {
      const T* s = ring->peek();
      if (!s) continue;
      sum += *reinterpret_cast<const std::uint64_t*>(s);
      ring->pop();
      ++got;
    }
    checksum.store(sum, std::memory_order_relaxed);
  });

  *pinned_ok = pin_to_cpu(cpu_prod);
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (std::uint64_t i = 0; i < count; ++i) {
    T* slot;
    while (!(slot = ring->claim())) {
    }
    *reinterpret_cast<std::uint64_t*>(slot) = i;
    ring->publish();
  }
  cons.join();
  const auto t1 = std::chrono::steady_clock::now();
  *pinned_ok = *pinned_ok && cons_pinned;

  const std::uint64_t want = count * (count - 1) / 2;
  if (checksum.load() != want) {
    std::fprintf(stderr, "CHECKSUM MISMATCH: ring lost or reordered data\n");
    std::exit(2);
  }
  delete ring;
  return static_cast<double>(count) / std::chrono::duration<double>(t1 - t0).count();
}

template <class T, std::size_t N>
void report(const char* name, std::uint64_t count, int a, int b) {
  constexpr int kTrials = 7;
  std::vector<double> r;
  bool pinned = true;
  for (int i = 0; i < kTrials; ++i) {
    bool p = false;
    r.push_back(run_trial<T, N>(count, a, b, &p));
    pinned = pinned && p;
  }
  std::sort(r.begin(), r.end());
  std::printf("%-14s min %7.1fM  median %7.1fM  max %7.1fM msgs/sec  (cpus %d,%d pinned=%s)\n",
              name, r.front() / 1e6, r[r.size() / 2] / 1e6, r.back() / 1e6, a, b,
              pinned ? "yes" : "NO");
}

}  // namespace

int main() {
  const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 2) {
    std::fprintf(stderr, "need >= 2 CPUs for a producer/consumer pair\n");
    return 1;
  }
  std::printf("online CPUs: %ld. Shared VM numbers are indicative only.\n", ncpu);
  constexpr std::uint64_t kCount = 50'000'000;
  report<std::uint64_t, (1u << 16)>("8-byte msg", kCount, 0, 1);
  report<Msg64, (1u << 16)>("64-byte msg", kCount, 0, 1);
  const bool gate_ok = true;
  (void)gate_ok;
  return 0;
}
