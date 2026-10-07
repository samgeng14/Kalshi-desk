#include "deskd/net/send.h"

#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kill.h"

namespace deskd {

namespace {
void refuse_write(const SendJob& job, SenderStats& stats) {
  stats.writes_refused.fetch_add(1);
  DLOG_WARN("write for %s refused: no venue connection in this stage", job.cid.c_str());
}
}  // namespace

void run_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, const AlertFn& alert) {
  while (!self.stopping()) {
    auto job = inbox.pop_for(std::chrono::milliseconds(250));
    self.beat();
    if (job) refuse_write(*job, stats);
  }
}

void watch_kill(KillSwitch& kill, BoundedQueue<SendJob>& inbox) {
  kill.on_trip([&inbox] { inbox.kick(); });
}

void run_cancel_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, KillSwitch& kill,
                       const AlertFn& alert) {
  std::uint64_t seen = 0;   // 0: a KILL restored at start (generation 1) is handled on the first turn
  auto take_kill = [&] {
    const std::uint64_t gen = kill.generation();
    if (gen == seen) return;
    seen = gen;
    // Later: trigger every order group, cancel-all, batch-cancel what is still listed, cancel
    // Polymarket's open orders; all on this thread's own connections, never behind gw. Then
    // kill.cancel_all_done(now_ms(), mono_ms()) once the venue answers the cancel-all, so the
    // re-arm waits a full minute after it (the venue cancels orders placed in that minute).
    stats.writes_before_kill.store(stats.writes_refused.load());
    stats.kills.fetch_add(1);
    stats.last_kill_ms.store(now_ms());
    stats.kill_gen_seen.store(gen);
    DLOG_WARN("KILL received %lld ms after the click: no venue connection in this stage, nothing to cancel",
              static_cast<long long>(now_ms() - kill.since_ms()));
    if (alert) alert("CRIT", "KILL", "KILL: no venue connection in this stage, nothing to cancel");
  };
  while (!self.stopping()) {
    take_kill();
    auto job = inbox.pop_for(std::chrono::milliseconds(250));
    self.beat();
    take_kill();   // a KILL that tripped while this job was popped still goes first
    if (job) refuse_write(*job, stats);
  }
}

}  // namespace deskd
