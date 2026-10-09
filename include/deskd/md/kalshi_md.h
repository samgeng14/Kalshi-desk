#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/units.h"
#include "deskd/md/feed.h"
#include "deskd/net/conn.h"

namespace deskd {
class Worker;
class KalshiSigner;

struct MdLevel {
  Px px = 0;
  Qty qty = 0;
  bool operator==(const MdLevel& o) const { return px == o.px && qty == o.qty; }
};
struct MdBook {
  std::string ticker;
  std::vector<MdLevel> bids;  // YES bids, best first (descending)
  std::vector<MdLevel> asks;  // YES asks, best first (ascending); 1 - the NO bid
  std::int64_t sid = 0, seq = 0, recv_ms = 0, last_change_ms = 0;
  std::optional<std::int64_t> venue_ms;
  bool stale = true;
  std::string reason = "awaiting snapshot";
};

// Deterministic owner-only reducer: sequence is per sid across ALL messages and markets, not
// per ticker. ok/unsubscribed replies spend sequence numbers too. A snapshot replaces a book.
// A gap invalidates every book on its sid; no delta can repair one. The caller resubscribes.
class MdBooks {
 public:
  using Callback = std::function<void(const MdBook&)>;
  explicit MdBooks(Callback callback) : callback_(std::move(callback)) {}
  void watch(const std::vector<std::string>& tickers, std::int64_t now_ms);
  void disconnected(const std::string& reason, std::int64_t now_ms);
  bool subscribed(std::int64_t sid, const std::vector<std::string>& tickers, std::string* why);
  bool message(const nlohmann::json& frame, std::int64_t recv_ms, std::string* why);
  bool ready() const;
  const std::map<std::string, MdBook>& books() const { return books_; }
 private:
  struct Sub { std::int64_t last_seq = 0; std::set<std::string> tickers; bool broken = false; };
  bool fail(std::int64_t sid, const std::string& reason, std::int64_t now_ms, std::string* why);
  Callback callback_;
  std::map<std::string, MdBook> books_;
  std::map<std::int64_t, Sub> subs_;
};

class KalshiMd {
 public:
  struct Options {
    int connect_timeout_ms = 2000;
    int ping_ms = 10000;
    int pong_timeout_ms = 5000;
    int snapshot_timeout_ms = 10000;
    int backoff_min_ms = 250;
    int backoff_max_ms = 15000;
    std::size_t markets_per_subscription = 250;
    std::size_t max_watch = 10000;
  };
  using Callback = MdBooks::Callback;
  // Endpoint uses https/http (TLS/loopback), with /trade-api/ws/v2 as base_path.
  KalshiMd(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
           FeedState& feed, Callback callback);
  KalshiMd(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
           FeedState& feed, Callback callback, Options options);
  // Thread-safe, copied and sorted. Empty lists are refused (Kalshi interprets them as ALL).
  // clear_watch is separate and closes the old subscription without issuing an empty one.
  bool set_watch(std::vector<std::string> tickers, std::string* why = nullptr);
  void clear_watch();
  void run(Worker& self);
 private:
  net::Endpoint endpoint_;
  net::ConnFactory& factory_;
  const KalshiSigner& signer_;
  FeedState& feed_;
  Callback callback_;
  Options options_;
  std::mutex mutex_;  // desired list only; never held over I/O or callbacks
  std::vector<std::string> desired_;
  std::uint64_t version_ = 1;
};

}  // namespace deskd
