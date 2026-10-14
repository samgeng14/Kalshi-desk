#include "deskd/gw/kill.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "deskd/app/log.h"

namespace deskd {

std::string KillSwitch::why() const {
  std::lock_guard<std::mutex> lk(m_);
  return why_;
}

void KillSwitch::persist_locked() {
  if (path_.empty()) return;
  const std::string tmp = path_ + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  const bool ok = f && std::fprintf(f, "%lld\n", static_cast<long long>(since_.load())) > 0 && std::fclose(f) == 0 &&
                  std::rename(tmp.c_str(), path_.c_str()) == 0;
  if (!ok) DLOG_ERR("KILL not persisted to %s: a restart would forget it", path_.c_str());
}

std::int64_t KillSwitch::restore(std::int64_t wall_ms, std::int64_t mono_ms) {
  std::lock_guard<std::mutex> lk(m_);
  if (path_.empty() || since_.load() != 0) return since_.load();
  FILE* f = std::fopen(path_.c_str(), "r");
  if (!f) return 0;
  long long t = 0;
  const int n = std::fscanf(f, "%lld", &t);
  std::fclose(f);
  // An unreadable file still means a KILL happened: fail closed with the file's own time
  // unknown, so the lockout counts as over and only the re-arm is needed.
  if (n != 1 || t <= 0) t = 1;
  // what is left of the lockout, by the wall clock (the only clock that outlives a process),
  // never more than a whole lockout however far the clock has gone back since
  const std::int64_t left = std::clamp<std::int64_t>(t + kLockoutMs - wall_ms, 0, kLockoutMs);
  since_.store(t);
  rearm_wall_.store(wall_ms + left);
  lockout_end_mono_ = mono_ms + left;
  why_ = "restart";
  gen_.fetch_add(1);   // send-cx cancels again at start: orders may have been placed by hand meanwhile
  state_gen_.fetch_add(1);
  return t;
}

bool KillSwitch::trip(std::int64_t wall_ms, std::int64_t mono_ms, const std::string& why) {
  bool first = false;
  std::int64_t mine = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (since_.load() == 0) {
      first = true;
      mine = wall_ms > 0 ? wall_ms : 1;
      since_.store(mine);
      rearm_wall_.store(mine + kLockoutMs);
      lockout_end_mono_ = mono_ms + kLockoutMs;
      why_ = why;
      state_gen_.fetch_add(1);
    }
    gen_.fetch_add(1);
  }
  // the watchers first: send-cx is cancelling before the file is even opened
  for (const auto& w : wakers_) w();
  if (first) {
    std::lock_guard<std::mutex> lk(m_);
    if (since_.load() == mine) persist_locked();   // a re-arm in between (never inside 60 s) wins
  }
  return first;
}

void KillSwitch::cancel_all_done(std::int64_t wall_ms, std::int64_t mono_ms, std::uint64_t generation) {
  std::lock_guard<std::mutex> lk(m_);
  if (generation > confirmed_gen_.load()) confirmed_gen_.store(generation);
  if (since_.load() == 0) return;
  lockout_end_mono_ = std::max(lockout_end_mono_, mono_ms + kLockoutMs);
  rearm_wall_.store(std::max(rearm_wall_.load(), wall_ms + kLockoutMs));
}

std::string KillSwitch::rearm(std::int64_t wall_ms, std::int64_t mono_ms) {
  std::lock_guard<std::mutex> lk(m_);
  if (since_.load() == 0) return "not KILLED";
  if (mono_ms < lockout_end_mono_) return "rearm in " + std::to_string((lockout_end_mono_ - mono_ms + 999) / 1000) + " s";
  since_.store(0);
  rearm_wall_.store(0);
  rearmed_wall_.store(wall_ms);
  why_.clear();
  state_gen_.fetch_add(1);
  if (!path_.empty() && ::unlink(path_.c_str()) != 0 && errno != ENOENT)
    DLOG_WARN("re-armed, but %s could not be removed: a restart would come back KILLED", path_.c_str());
  return "";
}

}  // namespace deskd
