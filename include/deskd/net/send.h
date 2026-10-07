#pragma once
// The send pool (plan section 2): send-k1 (Manual only), send-k2..3 (Systematic) and send-cx
// (cancels and KILL on its own connections). Each will hold one pre-opened TLS connection and
// make one HTTP/1.1 write per connection, never silently retried.
//
// Week 0 has no venue connection: a sender takes jobs and refuses writes, and send-cx answers a
// KILL with an alert saying there was nothing to cancel. The threads exist now so the heartbeat,
// status and KILL paths are the real ones from the first day.
//
// send-cx takes KILL from the kill switch, never from its inbox: it compares the switch's
// generation every turn of its loop (the trip kicks its inbox awake, so that is at once), and it
// handles a new generation before any queued job. Its inbox will carry week 2's cancels; a full
// one drops cancels and marks a gap, but it can never drop a KILL.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class KillSwitch;
class Worker;

struct SendJob {
  enum class Kind : std::uint8_t { WRITE };
  Kind kind = Kind::WRITE;
  std::int64_t t_ms = 0;
  std::string cid;
};

struct SenderStats {
  std::atomic<std::uint64_t> kills{0};             // KILL generations handled (send-cx)
  std::atomic<std::uint64_t> kill_gen_seen{0};     // the newest kill switch generation handled
  std::atomic<std::int64_t> last_kill_ms{0};
  std::atomic<std::uint64_t> writes_refused{0};
  std::atomic<std::uint64_t> writes_before_kill{0};  // jobs this sender had handled when it took its last KILL
};

using AlertFn = std::function<void(const std::string& lvl, const std::string& src, const std::string& msg)>;

// send-k1..k3: writes only
void run_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, const AlertFn& alert);
// send-cx: KILL from the switch first, then its inbox (cancels). Registers its inbox's kick with
// kill.on_trip: call it, or kill.on_trip yourself, before any thread can trip kill.
void watch_kill(KillSwitch& kill, BoundedQueue<SendJob>& inbox);
void run_cancel_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, KillSwitch& kill,
                       const AlertFn& alert);

}  // namespace deskd
