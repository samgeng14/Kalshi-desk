#include "deskd/gw/kill.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "deskd/app/log.h"

namespace deskd {

std::int64_t KillSwitch::restore() {
  if (path_.empty()) return 0;
  FILE* f = std::fopen(path_.c_str(), "r");
  if (!f) return 0;
  long long t = 0;
  const int n = std::fscanf(f, "%lld", &t);
  std::fclose(f);
  // An unreadable file still means a KILL happened: fail closed with the file's own time
  // unknown, so the lockout counts as over and only the re-arm is needed.
  if (n != 1 || t <= 0) t = 1;
  std::int64_t expected = 0;
  since_.compare_exchange_strong(expected, t);
  return since_.load();
}

bool KillSwitch::trip(std::int64_t now_ms) {
  std::int64_t expected = 0;
  if (!since_.compare_exchange_strong(expected, now_ms > 0 ? now_ms : 1)) return false;
  // the flag is set first: the file only makes it outlive this process
  if (!path_.empty()) {
    const std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    const bool ok = f && std::fprintf(f, "%lld\n", static_cast<long long>(since_.load())) > 0 && std::fclose(f) == 0 &&
                    std::rename(tmp.c_str(), path_.c_str()) == 0;
    if (!ok) DLOG_ERR("KILL not persisted to %s: a restart would forget it", path_.c_str());
  }
  return true;
}

std::string KillSwitch::rearm(std::int64_t now_ms) {
  std::int64_t s = since_.load();
  if (s == 0) return "not KILLED";
  if (now_ms < s + kLockoutMs) return "rearm in " + std::to_string((s + kLockoutMs - now_ms + 999) / 1000) + " s";
  if (!since_.compare_exchange_strong(s, 0)) return "KILL changed while re-arming; try again";
  if (!path_.empty() && ::unlink(path_.c_str()) != 0) DLOG_WARN("re-armed, but %s could not be removed", path_.c_str());
  return "";
}

}  // namespace deskd
