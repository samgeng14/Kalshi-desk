#include "deskd/net/send.h"

#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"

namespace deskd {

void run_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, const AlertFn& alert) {
  while (!self.stopping()) {
    auto job = inbox.pop_for(std::chrono::milliseconds(250));
    self.beat();
    if (!job) continue;
    if (job->kind == SendJob::Kind::KILL) {
      // Later: trigger every order group, cancel-all, batch-cancel what is still listed, cancel
      // Polymarket's open orders; all on this thread's own connections, never behind gw.
      stats.kills.fetch_add(1);
      stats.last_kill_ms.store(job->t_ms);
      DLOG_WARN("KILL received %lld ms after the click: no venue connection in this stage, nothing to cancel",
                static_cast<long long>(now_ms() - job->t_ms));
      if (alert) alert("CRIT", "KILL", "KILL: no venue connection in this stage, nothing to cancel");
    } else {
      stats.writes_refused.fetch_add(1);
      DLOG_WARN("write for %s refused: no venue connection in this stage", job->cid.c_str());
    }
  }
}

}  // namespace deskd
