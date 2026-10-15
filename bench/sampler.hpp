#pragma once
// A small statistical sampler for flame graphs, for machines where the perf
// tool and hardware counters are not available (this sandbox has neither).
//
// A real-time interval timer fires SIGALRM at a fixed rate. The handler records
// the interrupted instruction pointer and walks the frame-pointer chain (build
// with -fno-omit-frame-pointer) into a preallocated buffer. Nothing is done in
// the handler beyond that. After the run, dump() writes raw addresses and the
// load bias; scripts/flamegraph.py turns them into stacks with addr2line (which
// expands inlined calls from the debug info) and draws the SVG.
//
// Limits: a sample taken in a function prologue or epilogue misses that one
// frame; the signal itself perturbs timing, so never take latency numbers from
// a profiled run.

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <link.h>
#include <ucontext.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace benchprof {

inline constexpr unsigned kMaxDepth = 24;

struct Sample {
  std::uint32_t depth;
  std::uint64_t ip[kMaxDepth];
};

inline Sample* g_samples = nullptr;
inline volatile std::uint64_t g_count = 0;
inline std::uint64_t g_capacity = 0, g_dropped = 0;
inline std::uintptr_t g_stack_lo = 0, g_stack_hi = 0;
inline volatile sig_atomic_t g_on = 0;

inline void handler(int, siginfo_t*, void* uctx) {
  if (!g_on) return;
  if (g_count >= g_capacity) {
    ++g_dropped;
    return;
  }
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  Sample& s = g_samples[g_count];
  std::uint32_t d = 0;
  s.ip[d++] = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  auto bp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
  const auto sp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
  while (d < kMaxDepth && bp >= sp && bp + 16 <= g_stack_hi && bp >= g_stack_lo && (bp & 7) == 0) {
    const auto* fr = reinterpret_cast<const std::uintptr_t*>(bp);
    const std::uintptr_t next = fr[0], ret = fr[1];
    if (ret == 0) break;
    s.ip[d++] = ret;
    if (next <= bp) break;
    bp = next;
  }
  s.depth = d;
  g_count = g_count + 1;
}

// Call once from the thread that will be profiled.
inline bool start(unsigned hz, std::uint64_t capacity) {
  g_samples = static_cast<Sample*>(std::calloc(capacity, sizeof(Sample)));
  if (!g_samples) return false;
  g_capacity = capacity;
  pthread_attr_t at;
  if (pthread_getattr_np(pthread_self(), &at) != 0) return false;
  void* addr = nullptr;
  std::size_t size = 0;
  pthread_attr_getstack(&at, &addr, &size);
  pthread_attr_destroy(&at);
  g_stack_lo = reinterpret_cast<std::uintptr_t>(addr);
  g_stack_hi = g_stack_lo + size;
  struct sigaction sa {};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGALRM, &sa, nullptr) != 0) return false;
  itimerval it{};
  const long us = static_cast<long>(1'000'000u / hz);
  it.it_interval.tv_sec = it.it_value.tv_sec = us / 1'000'000;
  it.it_interval.tv_usec = it.it_value.tv_usec = us % 1'000'000;
  g_on = 0;
  return setitimer(ITIMER_REAL, &it, nullptr) == 0;
}

// Samples are only taken between resume() and pause(), so setup (page
// faults, initialisation) does not pollute the picture.
inline void resume() { g_on = 1; }
inline void pause() { g_on = 0; }

inline void stop() {
  g_on = 0;
  itimerval it{};
  setitimer(ITIMER_REAL, &it, nullptr);
}

inline int bias_cb(dl_phdr_info* info, std::size_t, void* data) {
  *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
  return 1;  // the first object is the executable
}

inline bool dump(const char* path, const char* exe) {
  std::FILE* f = std::fopen(path, "w");
  if (!f) return false;
  std::uintptr_t bias = 0;
  dl_iterate_phdr(bias_cb, &bias);
  std::fprintf(f, "# exe %s\n# bias %llx\n# samples %llu dropped %llu\n", exe, static_cast<unsigned long long>(bias),
               static_cast<unsigned long long>(g_count), static_cast<unsigned long long>(g_dropped));
  for (std::uint64_t i = 0; i < g_count; ++i) {
    const Sample& s = g_samples[i];
    for (std::uint32_t k = 0; k < s.depth; ++k)
      std::fprintf(f, "%s%llx", k ? " " : "", static_cast<unsigned long long>(s.ip[k]));
    std::fputc('\n', f);
  }
  std::fclose(f);
  return true;
}

}  // namespace benchprof
