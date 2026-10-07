#pragma once
// ops: the supervisor's heartbeat, disk space and the preflight rows (plan section 2).
//
// The heartbeat file is written only while every watched owner advanced its progress counter in
// the last 5 s, so a desk whose web threads still answer but whose gateway or feeds are stuck
// looks dead to the supervisor and is restarted. The old desk wrote its heartbeat after every
// engine pass for the same reason (2026-09: a desk with every request thread busy looked dead to
// /api/state while it was cycling normally, and the opposite case must not look alive).
//
// Watched (runtime.cpp): gw, md-k, priv-k, send-cx (KILL's own path) and pub (every page's hb),
// plus "web", which has no loop of its own: ops sends GET /api/health over loopback each second
// with a 200 ms timeout, and the "web" counter moves only on a 200. A wedged request pool would
// otherwise leave no KILL path but the venue's website (deskctl kill uses the same route) while
// the heartbeat said all was well (review, 2026-10-06). A real feed thread must therefore come
// back to beat() well inside 5 s: week 1's socket reads take timeouts of 250 ms or less, so a
// quiet socket never gets a healthy desk restarted.
//
// Every time here is on the steady clock (app/log.h) except the heartbeat file's content, which
// the supervisor compares with its own `date +%s`.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "deskd/bus/msg.h"

namespace deskd {

class Worker;
struct Feeds;

class HeartbeatRule {
 public:
  explicit HeartbeatRule(std::int64_t window_ms) : window_ms_(window_ms) {}
  // a counter seen for the first time counts as just advanced (the start-up grace)
  void observe(const std::string& name, std::uint64_t counter, std::int64_t now_ms);
  // "" when every observed counter advanced within the window; else the silent ones,
  // "gw silent 6.2 s, md-k silent 5.1 s"
  std::string withheld(std::int64_t now_ms) const;
  // the silent ones' names alone, "gw,md-k": what the log compares, so a stuck worker is one
  // line when it stops and one when it moves again, not a line a second
  std::string silent_names(std::int64_t now_ms) const;

 private:
  struct Seen {
    std::string name;
    std::uint64_t counter;
    std::int64_t changed_ms;
  };
  std::vector<Seen> seen_;
  std::int64_t window_ms_;
};

// "<epoch seconds>\n" through a temporary file and rename(2), so a reader never sees half a line
bool write_heartbeat(const std::string& path, std::int64_t now_s, std::string* err);
std::optional<std::uint64_t> disk_free_bytes(const std::string& dir);

class Ops {
 public:
  // 10 GB: the free-space floor the old desk's data has run against since 2026-09-24
  static constexpr std::uint64_t kDiskFloorBytes = 10ull * 1000 * 1000 * 1000;
  static constexpr const char* kHeartbeatFile = "deskd.heartbeat";

  using ProbeFn = std::function<bool()>;   // true when web answered GET /api/health
  using AlertFn = std::function<void(const std::string& lvl, const std::string& msg)>;

  Ops(std::string data_dir, std::int64_t window_ms, std::vector<const Worker*> watched, const Feeds& feeds,
      ProbeFn web_probe = nullptr, AlertFn alert = nullptr);

  void run(Worker& self);

  std::vector<PreflightRow> preflight() const;   // the latest rows, an immutable copy
  std::string heartbeat_path() const { return data_dir_ + "/" + kHeartbeatFile; }
  std::int64_t heartbeat_written_ms() const { return written_ms_.load(); }   // wall clock
  std::string heartbeat_withheld() const;        // "" while it is being written
  std::uint64_t web_probes_ok() const { return web_ok_.load(); }
  std::uint64_t web_probes_failed() const { return web_failed_.load(); }

 private:
  void refresh_preflight(std::int64_t now_ms, bool disk);

  const std::string data_dir_;
  const std::vector<const Worker*> watched_;
  const Feeds& feeds_;
  const ProbeFn web_probe_;
  const AlertFn alert_;
  HeartbeatRule rule_;
  std::optional<std::uint64_t> disk_free_;      // ops thread only
  std::shared_ptr<const std::vector<PreflightRow>> rows_;   // std::atomic_load / store
  std::shared_ptr<const std::string> withheld_;
  std::atomic<std::int64_t> written_ms_{0};
  std::atomic<std::uint64_t> web_ok_{0}, web_failed_{0};
};

}  // namespace deskd
