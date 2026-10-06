#pragma once
// One feed's liveness as the stream's hb carries it (contract hb.feeds: up, gap, lag_ms). The
// feed's owner thread writes it; pub and ops read it. Staleness is socket liveness plus
// sequence continuity, never time since the last trade: thin college books go 57-90 s between
// trades and are not stale.
#include <atomic>
#include <optional>

namespace deskd {

struct FeedView {
  bool up = false;
  bool gap = false;
  std::optional<int> lag_ms;   // p50 of now - the venue's send time; null while down
};

class FeedState {
 public:
  void set(bool up, bool gap, int lag_ms = -1) {
    lag_ms_.store(lag_ms, std::memory_order_relaxed);
    gap_.store(gap, std::memory_order_relaxed);
    up_.store(up, std::memory_order_release);
  }
  FeedView view() const {
    FeedView v;
    v.up = up_.load(std::memory_order_acquire);
    v.gap = gap_.load(std::memory_order_relaxed);
    const int lag = lag_ms_.load(std::memory_order_relaxed);
    if (v.up && lag >= 0) v.lag_ms = lag;
    return v;
  }

 private:
  std::atomic<bool> up_{false};
  std::atomic<bool> gap_{false};
  std::atomic<int> lag_ms_{-1};
};

// KX: Kalshi market data (md-k); KXP: Kalshi private, orders and fills (priv-k); PM: Polymarket.
struct Feeds {
  FeedState KX, KXP, PM;
};

}  // namespace deskd

namespace deskd {

class Worker;

// md-k and priv-k in week 0: the owner thread exists and beats, its feed reads down. The
// socket, books and sequence numbers arrive with the gateway (week 1) and the private socket
// (week 2); the threads come first so the heartbeat rule watches the real ones from day one.
void run_feed_placeholder(Worker& self, FeedState& feed);

}  // namespace deskd
