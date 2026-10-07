#pragma once
// KILL's own path (plan section 2). The route trips this switch; send-cx (which cancels without
// gw) and gw (which refuses new orders and deletes the queued ones) each watch its generation
// and are woken by the trip itself. A KILL therefore takes no queue slot: it cannot be dropped by
// a full send-cx inbox, and it reaches send-cx while gw is frozen (review, 2026-10-06: the route
// pushed into send-cx's drop-on-full queue and ignored the result).
//
//   generation()   +1 on every trip, a repeated KILL included (cancel-all again: idempotent),
//                  and on a restore of a persisted KILL
//   state_gen()    +1 on every change of state: killed, re-armed, restored
//
// Reads (active, since_ms, rearm_ms) are lock-free. trip, rearm, restore and cancel_all_done
// change the state and its file under one small mutex, so a trip landing in the middle of a
// re-arm can no longer have its new file deleted by the re-arm's unlink (a restart would then
// have come back un-killed). The file is local, never network I/O, and the mutex is never on an
// order's path: gw reads active() lock-free.
//
// Two clocks (app/log.h): since_ms and rearm_ms are wall-clock times for the screen and the file;
// the 60 s lockout itself runs on the steady clock, so a clock step forward cannot shorten it.
// The lockout runs 60 s from the trip and at least 60 s from the moment send-cx's cancel-all
// returns (cancel_all_done): Kalshi cancels "newly placed orders ... during the minute after the
// request" (cancel-all-orders.md), so an order placed sooner after a re-arm could be cancelled.
//
// KILLED survives a restart: the kill time is written to the account's <lock dir>/kalshi-<env>.killed
// and read back at start, so a supervisor restarting a hung desk, or an operator restarting it from
// another directory, never clears a KILL that nobody re-armed (found 2026-10-06: a restart came
// back STOP and trading).
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace deskd {

class KillSwitch {
 public:
  static constexpr std::int64_t kLockoutMs = 60000;

  // the file that keeps KILLED across restarts; "" (the default) keeps it in memory only
  void persist_to(std::string path) { path_ = std::move(path); }
  // Runs on every trip, before the file is written: send-cx's and gw's inbox kick. Register
  // before any thread trips the switch; the list is never changed after that.
  void on_trip(std::function<void()> wake) { wakers_.push_back(std::move(wake)); }

  // at start: KILLED again, with the recorded kill time, when the file says so; returns it (0 = no)
  std::int64_t restore(std::int64_t wall_ms, std::int64_t mono_ms);

  // true when this call killed; false when already KILLED (the first kill's times stand, so a
  // second click cannot extend or shorten the lockout). Either way the generation moves and the
  // watchers wake: a second KILL is a second cancel-all.
  bool trip(std::int64_t wall_ms, std::int64_t mono_ms, const std::string& why);
  // send-cx's cancel-all returned at this time: the lockout lasts at least kLockoutMs from now
  void cancel_all_done(std::int64_t wall_ms, std::int64_t mono_ms);
  // Leaves KILLED once the lockout is over: "" on success, else the refusal (BAD_STATE).
  std::string rearm(std::int64_t wall_ms, std::int64_t mono_ms);

  bool active() const { return since_.load() != 0; }
  std::int64_t since_ms() const { return since_.load(); }
  std::int64_t rearm_ms() const { return active() ? rearm_wall_.load() : 0; }   // wall; 0 = not killed
  std::int64_t rearmed_ms() const { return rearmed_wall_.load(); }               // the last re-arm, wall
  std::string why() const;
  std::uint64_t generation() const { return gen_.load(); }
  std::uint64_t state_gen() const { return state_gen_.load(); }

 private:
  void persist_locked();

  mutable std::mutex m_;
  std::atomic<std::int64_t> since_{0}, rearm_wall_{0}, rearmed_wall_{0};
  std::int64_t lockout_end_mono_ = 0;   // under m_
  std::string why_;                     // under m_
  std::atomic<std::uint64_t> gen_{0}, state_gen_{0};
  std::vector<std::function<void()>> wakers_;
  std::string path_;
};

}  // namespace deskd
