#pragma once
// The send pool (plan section 2): send-k1 (Manual only), send-k2..3 (Systematic) and send-cx
// (cancels and KILL on its own connections). Each will hold one pre-opened TLS connection and
// make one HTTP/1.1 write per connection, never silently retried.
//
// Week 0 has no venue connection: a sender takes jobs, refuses writes, and send-cx answers a
// KILL with an alert saying there was nothing to cancel. The threads exist now so the
// heartbeat, status and KILL paths are the real ones from the first day.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class Worker;

struct SendJob {
  enum class Kind : std::uint8_t { WRITE, KILL };
  Kind kind = Kind::WRITE;
  std::int64_t t_ms = 0;
  std::string cid;   // WRITE
};

struct SenderStats {
  std::atomic<std::uint64_t> kills{0};            // KILL jobs handled
  std::atomic<std::int64_t> last_kill_ms{0};
  std::atomic<std::uint64_t> writes_refused{0};
};

using AlertFn = std::function<void(const std::string& lvl, const std::string& src, const std::string& msg)>;

void run_sender(Worker& self, BoundedQueue<SendJob>& inbox, SenderStats& stats, const AlertFn& alert);

}  // namespace deskd
