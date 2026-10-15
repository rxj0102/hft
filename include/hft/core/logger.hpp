#pragma once
// Lock-free binary logger. The hot thread (single producer) pushes fixed 64-byte
// records into an SpscRing and returns immediately; a background thread drains
// the ring and does the write(2) calls. The hot path therefore never does I/O,
// never takes a lock, and never allocates.
//
// Fixed-size records keep the producer branch-free and make the file trivially
// seekable. Payloads over kPayload bytes are not supported: log an id and keep
// the big thing elsewhere.
//
// Policy on a full ring: drop and count. Blocking would stall the trading
// thread behind disk, which is worse than losing a diagnostic line. Anything
// that must not be lost (the replay event log, Phase 6) sizes its ring for the
// worst burst and treats dropped() != 0 as a failed run.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "hft/core/clock.hpp"
#include "hft/core/spsc_ring.hpp"

namespace hft::core {

inline constexpr char kLogMagic[8] = {'H', 'F', 'T', 'L', 'O', 'G', '0', '1'};

struct LogRecord {
  static constexpr std::size_t kPayload = 44;
  std::uint64_t seq;       // producer-assigned, lets a reader detect drops
  std::uint64_t tsc;       // raw TSC; convert offline with the calibration
  std::uint16_t type;      // caller-defined event id
  std::uint16_t len;       // valid payload bytes
  std::uint8_t payload[kPayload];
};
static_assert(sizeof(LogRecord) == 64, "log record must be exactly one cache line");

template <std::size_t RingCapacity = (1u << 16)>
class BinaryLogger {
 public:
  BinaryLogger() = default;
  BinaryLogger(const BinaryLogger&) = delete;
  BinaryLogger& operator=(const BinaryLogger&) = delete;
  ~BinaryLogger() { stop(); }

  // Startup only (opens the file, starts the thread). Returns false on failure.
  bool start(const char* path) noexcept {
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) return false;
    if (::write(fd_, kLogMagic, sizeof kLogMagic) != static_cast<ssize_t>(sizeof kLogMagic)) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { drain_loop(); });
    return true;
  }

  // Hot path. Single producer thread only.
  bool log(std::uint16_t type, const void* data, std::size_t len) noexcept {
    LogRecord* r = ring_.claim();
    if (!r) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    r->seq = next_seq_++;
    r->tsc = rdtsc_fast();
    r->type = type;
    const std::size_t n = len < LogRecord::kPayload ? len : LogRecord::kPayload;
    r->len = static_cast<std::uint16_t>(n);
    if (n) std::memcpy(r->payload, data, n);
    ring_.publish();
    return true;
  }

  // Flushes everything already published, then joins. Idempotent.
  void stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }

 private:
  void drain_loop() noexcept {
    constexpr std::size_t kBatch = 1024;  // 64 KiB per write(2)
    static thread_local LogRecord buf[kBatch];
    for (;;) {
      std::size_t n = 0;
      while (n < kBatch) {
        const LogRecord* r = ring_.peek();
        if (!r) break;
        buf[n++] = *r;
        ring_.pop();
      }
      if (n) {
        write_all(buf, n * sizeof(LogRecord));
        written_.fetch_add(n, std::memory_order_release);
        continue;
      }
      // Empty: exit only after the stop flag AND a final empty check, so records
      // published just before stop() are never lost.
      if (!running_.load(std::memory_order_acquire)) {
        if (!ring_.peek()) return;
        continue;
      }
      // Not the hot path; a short sleep keeps the flusher from burning a core.
      ::usleep(50);
    }
  }

  void write_all(const void* p, std::size_t bytes) noexcept {
    const auto* c = static_cast<const char*>(p);
    while (bytes) {
      const ssize_t w = ::write(fd_, c, bytes);
      if (w <= 0) return;  // disk error: stop writing, producer keeps going
      c += w;
      bytes -= static_cast<std::size_t>(w);
    }
  }

  SpscRing<LogRecord, RingCapacity> ring_;
  std::uint64_t next_seq_{0};            // producer-only
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
  int fd_{-1};
};

// Offline reader for tests and the decoder tool. Not hot path (uses stdio).
// Returns the number of records read, or -1 if the file is missing/corrupt.
template <class Fn>
long read_log(const char* path, Fn&& fn) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return -1;
  char magic[8];
  if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kLogMagic, 8) != 0) {
    std::fclose(f);
    return -1;
  }
  long n = 0;
  LogRecord r;
  while (std::fread(&r, sizeof r, 1, f) == 1) {
    fn(r);
    ++n;
  }
  std::fclose(f);
  return n;
}

}  // namespace hft::core
