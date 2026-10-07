#pragma once
// The spare pool (plan section 2): each sender keeps connections already open and verified, so
// a click never pays a TCP and TLS handshake (35-95 ms) on its own path.
//
//   - A connection carries one request and is then closed: take() hands it over for good, and a
//     replacement opens at once on the pool's own thread ("pool-<name>"). A write never rides a
//     connection that carried anything before, so "did the venue get it?" never depends on
//     whether an old keep-alive had died under us (http.cpp:98-105's rule in the old desk).
//   - Kalshi's load balancer closed a connection left idle 65 s and kept one idle 30 s (v3
//     research F10, 2026-10-06). Ready connections are retired at idle_replace_ms (20 s), and the
//     replacement starts lead_ms (2 s) before that, so the pool never stands empty while it
//     turns over; take() hands out the newest one.
//   - Every check_ms (1 s) the ready ones are looked at without waiting (Conn::usable): one the
//     venue closed (close_notify, a FIN, a reset) is dropped and replaced before a write can
//     land on it. That look is a non-blocking poll and read of what is already in the socket,
//     done under the pool's lock: a few microseconds, never a network wait.
//   - take() looks once more before handing one out. With none ready it opens one inline and
//     counts a miss: the caller then pays the handshake, and the count shows it.
//   - Open failures back off from 100 ms to 5 s; a guard refusal backs off at once to the most,
//     since it will not change on its own.
//
// Counters: handshakes (every connection the pool opened), hits and misses on take(), stale ones
// dropped at take(), idle retirements, closes by the venue, open failures and refusals, and the
// last 256 handshake times (TCP connect plus TLS).
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deskd/net/conn.h"

namespace deskd::net {

struct PoolConfig {
  std::string name = "pool";         // the thread is "pool-<name>"
  Endpoint endpoint;
  int spares = 1;                    // connections kept ready
  int idle_replace_ms = 20000;
  int lead_ms = 2000;                // a replacement opens this long before the old one retires
  int check_ms = 1000;
  int connect_timeout_ms = 5000;     // the pool's own opens
  int backoff_min_ms = 100, backoff_max_ms = 5000;
};

struct PoolStats {
  std::uint64_t handshakes = 0;      // connections the pool opened, in the background or inline
  std::uint64_t opened_background = 0, opened_inline = 0;
  std::uint64_t open_failures = 0, refused = 0;
  std::uint64_t takes = 0, hits = 0, misses = 0, miss_failed = 0;
  std::uint64_t stale_on_take = 0;   // a ready one found closed or unfit when taken
  std::uint64_t idle_retired = 0;    // closed at idle_replace_ms
  std::uint64_t closed_by_venue = 0; // found closed by the periodic look
  std::size_t ready = 0;
  std::vector<double> handshake_ms; // the last 256, oldest first (connect + TLS)
  std::string last_error;
};

class SparePool {
 public:
  // factory must outlive the pool
  SparePool(ConnFactory& factory, PoolConfig cfg);
  ~SparePool();
  SparePool(const SparePool&) = delete;
  SparePool& operator=(const SparePool&) = delete;

  void start();
  void stop();   // closes every ready connection (close_notify) and joins the thread

  // A connection for one request, never used before; nullptr with *why when none could be
  // opened within timeout_ms. *miss says whether the caller paid for the open.
  std::unique_ptr<Conn> take(int timeout_ms, bool* miss = nullptr, std::string* why = nullptr);

  std::size_t ready() const;
  bool wait_ready(std::size_t n, int timeout_ms) const;   // tests and startup
  PoolStats stats() const;
  const PoolConfig& config() const { return cfg_; }

 private:
  void loop();
  void record_open(const Conn& c, bool inline_open);

  ConnFactory& factory_;
  PoolConfig cfg_;
  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::deque<std::unique_ptr<Conn>> ready_;   // oldest first; under mu_
  bool stop_ = false;
  bool running_ = false;
  std::uint64_t wakeups_ = 0;                 // take() asks for a refill
  PoolStats st_;                              // under mu_ (ready and handshake_ms filled on read)
  std::deque<double> hs_ms_;
  std::thread th_;
};

}  // namespace deskd::net
