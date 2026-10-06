#pragma once
// KILL's own path (plan section 2): the route sets this flag and wakes send-cx, which cancels
// without gw, so KILL works while gw is frozen. gw reads it to refuse new orders.
//
// One atomic holds the kill time (0 = not killed), so tripping, reading and re-arming can never
// see a half-written state. The lockout is fixed at 60 s (contract: rearm_ms).
//
// KILLED also survives a restart: the kill time is written to <data dir>/deskd.killed and read
// back at start, so a supervisor restarting a hung desk, or an operator restarting it, never
// clears a KILL that nobody re-armed (found 2026-10-06: a restart came back STOP and trading).
#include <atomic>
#include <cstdint>
#include <string>

namespace deskd {

class KillSwitch {
 public:
  static constexpr std::int64_t kLockoutMs = 60000;

  // the file that keeps KILLED across restarts; "" (the default) keeps it in memory only
  void persist_to(std::string path) { path_ = std::move(path); }
  // at start: KILLED again, with the recorded kill time, when the file says so; returns it (0 = no)
  std::int64_t restore();

  // true when this call killed; false when already KILLED (the first kill's times stand, so a
  // second click cannot extend or shorten the lockout)
  bool trip(std::int64_t now_ms);
  bool active() const { return since_.load() != 0; }
  std::int64_t since_ms() const { return since_.load(); }
  std::int64_t rearm_ms() const {
    const std::int64_t s = since_.load();
    return s ? s + kLockoutMs : 0;
  }
  // Leaves KILLED once the lockout is over: "" on success, else the refusal (BAD_STATE).
  std::string rearm(std::int64_t now_ms);

 private:
  std::atomic<std::int64_t> since_{0};
  std::string path_;
};

}  // namespace deskd
