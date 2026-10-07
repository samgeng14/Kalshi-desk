#include "deskd/app/ops.h"

#include <sys/statvfs.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/md/feed.h"

namespace deskd {

void HeartbeatRule::observe(const std::string& name, std::uint64_t counter, std::int64_t now_ms) {
  for (auto& s : seen_) {
    if (s.name != name) continue;
    if (s.counter != counter) {
      s.counter = counter;
      s.changed_ms = now_ms;
    }
    return;
  }
  seen_.push_back(Seen{name, counter, now_ms});
}

std::string HeartbeatRule::withheld(std::int64_t now_ms) const {
  std::string out;
  for (const auto& s : seen_) {
    const std::int64_t quiet = now_ms - s.changed_ms;
    if (quiet <= window_ms_) continue;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s%s silent %.1f s", out.empty() ? "" : ", ", s.name.c_str(), quiet / 1000.0);
    out += buf;
  }
  return out;
}

std::string HeartbeatRule::silent_names(std::int64_t now_ms) const {
  std::string out;
  for (const auto& s : seen_)
    if (now_ms - s.changed_ms > window_ms_) out += (out.empty() ? "" : ",") + s.name;
  return out;
}

bool write_heartbeat(const std::string& path, std::int64_t now_s, std::string* err) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) {
    if (err) *err = tmp + ": " + std::strerror(errno);
    return false;
  }
  std::fprintf(f, "%lld\n", static_cast<long long>(now_s));
  const bool ok = std::fclose(f) == 0 && std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok && err) *err = path + ": " + std::strerror(errno);
  return ok;
}

std::optional<std::uint64_t> disk_free_bytes(const std::string& dir) {
  struct statvfs v {};
  if (::statvfs(dir.c_str(), &v) != 0) return std::nullopt;
  return static_cast<std::uint64_t>(v.f_bavail) * static_cast<std::uint64_t>(v.f_frsize);
}

Ops::Ops(std::string data_dir, std::int64_t window_ms, std::vector<const Worker*> watched, const Feeds& feeds,
         ProbeFn web_probe, AlertFn alert)
    : data_dir_(std::move(data_dir)),
      watched_(std::move(watched)),
      feeds_(feeds),
      web_probe_(std::move(web_probe)),
      alert_(std::move(alert)),
      rule_(window_ms) {
  std::atomic_store(&withheld_, std::make_shared<const std::string>("starting"));
  refresh_preflight(mono_ms(), true);   // valid rows before the ops thread first runs
}

std::vector<PreflightRow> Ops::preflight() const {
  auto r = std::atomic_load(&rows_);
  return r ? *r : std::vector<PreflightRow>{};
}

std::string Ops::heartbeat_withheld() const {
  auto w = std::atomic_load(&withheld_);
  return w ? *w : std::string();
}

void Ops::refresh_preflight(std::int64_t now_ms, bool disk) {
  (void)now_ms;
  if (disk) disk_free_ = disk_free_bytes(data_dir_);
  const FeedView kx = feeds_.KX.view(), kxp = feeds_.KXP.view();
  std::vector<PreflightRow> rows;
  // Week 0 loads no key and reads no balance; each row says what it waits for.
  rows.push_back({"KEY", false, "not loaded"});
  rows.push_back({"CASH", false, "no balance"});
  rows.push_back({"SHARD", false, "no balance"});
  rows.push_back({"CLK", false, "no venue clock"});
  if (!disk_free_) {
    rows.push_back({"DISK", false, "statvfs failed"});
  } else {
    const double gb = *disk_free_ / 1e9;
    char why[64] = "";
    if (*disk_free_ < kDiskFloorBytes) std::snprintf(why, sizeof why, "%.1f GB free < 10", gb);
    rows.push_back({"DISK", *disk_free_ >= kDiskFloorBytes, why});
  }
  rows.push_back({"FEED", kx.up && !kx.gap, kx.up ? (kx.gap ? "KX gap" : "") : "KX down"});
  rows.push_back({"PRIV", kxp.up && !kxp.gap, kxp.up ? (kxp.gap ? "KXP gap" : "") : "KXP down"});
  rows.push_back({"GROUPS", false, "none"});
  std::atomic_store(&rows_, std::make_shared<const std::vector<PreflightRow>>(std::move(rows)));
}

void Ops::run(Worker& self) {
  const std::string path = heartbeat_path();
  std::int64_t next_beat = 0, next_disk = 0;
  std::string last_silent = "starting", err;
  bool web_was_ok = true;
  while (!self.stopping()) {
    std::int64_t now = mono_ms();
    if (now >= next_beat) {
      next_beat = now + 1000;
      if (web_probe_) {
        // at most 200 ms on this thread; the probe's own timeouts bound it
        const bool ok = web_probe_();
        (ok ? web_ok_ : web_failed_).fetch_add(1);
        if (ok != web_was_ok) {
          if (ok) DLOG_INFO("web answers /api/health again");
          else DLOG_ERR("web did not answer /api/health within 200 ms");
          web_was_ok = ok;
        }
        now = mono_ms();
        rule_.observe("web", web_ok_.load(), now);
      }
      for (const Worker* w : watched_) rule_.observe(w->name(), w->progress(), now);
      const std::string why = rule_.withheld(now), silent = rule_.silent_names(now);
      if (why.empty()) {
        const std::int64_t wall = now_ms();
        if (write_heartbeat(path, wall / 1000, &err)) written_ms_.store(wall);
        else DLOG_ERR("heartbeat: %s", err.c_str());
      }
      if (silent != last_silent) {
        if (!silent.empty()) {
          DLOG_ERR("heartbeat withheld: %s", why.c_str());
          if (alert_) alert_("CRIT", "heartbeat withheld (" + why + "): the supervisor will restart deskd");
        } else {
          DLOG_INFO("heartbeat %s", last_silent == "starting" ? "started" : "resumed");
        }
        last_silent = silent;
      }
      std::atomic_store(&withheld_, std::make_shared<const std::string>(why));
      const bool disk = now >= next_disk;
      if (disk) next_disk = now + 10000;
      refresh_preflight(now, disk);
    }
    self.beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace deskd
