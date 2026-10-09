#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/feed.h"
#include "deskd/net/conn.h"

namespace deskd {
class Worker;
class KalshiSigner;
struct PrivateEvent {
  enum class Kind { ORDER, FILL } kind = Kind::ORDER;
  kalshi::OrderRow order;
  kalshi::FillRow fill;
  std::string cid;
  std::optional<Qty> post_position;
  std::int64_t received_ms = 0;
};
// Strict adapters for the private WS shapes (which differ from REST's fills shape).
std::optional<PrivateEvent> parse_private_event(const nlohmann::json& frame,
                                               std::int64_t received_ms, std::string* why);

class KalshiPrivate {
 public:
  struct Options {
    int connect_ms = 2000, ping_ms = 10000, pong_ms = 5000, ack_ms = 5000;
    int backoff_min_ms = 250, backoff_max_ms = 15000;
  };
  // true means the event reached the gateway's priority inbox. false requires REST
  // reconciliation before KXP can be healthy again. Neither callback may do network I/O.
  using EventFn = std::function<bool(const PrivateEvent&)>;
  using ReconcileFn = std::function<void()>;
  KalshiPrivate(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
                FeedState& feed, EventFn event, ReconcileFn reconcile);
  KalshiPrivate(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
                FeedState& feed, EventFn event, ReconcileFn reconcile, Options options);
  void run(Worker& worker);
  // Capture before a paginated account read. Confirm only after every page succeeded.
  // Any private update/reconnect during that read invalidates the fence. Zero is never valid.
  std::uint64_t fence() const;
  bool reconcile(std::uint64_t fence);
 private:
  net::Endpoint endpoint_;
  net::ConnFactory& factory_;
  const KalshiSigner& signer_;
  FeedState& feed_;
  EventFn event_;
  ReconcileFn reconcile_;
  Options options_;
  std::atomic<std::uint64_t> generation_{0}, confirmed_{0};
  std::atomic<bool> subscribed_{false};
};
}
