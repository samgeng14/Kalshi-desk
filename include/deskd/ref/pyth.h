#pragma once
// The settlement reference for Kalshi's 15-minute financial markets (plan section 5, LIP-15):
// Kalshi relays the Pyth index each one settles on over its own socket, channel `pyth_value`.
// Ported from the old desk's ReferenceFeed (src/reference_feed.cpp), without its store.
//
//   frame  {type: pyth_value, sid, msg: {underlying_ticker, value_usd, source_ts_ms}}
//
// value_usd may arrive as a number or a decimal string; a frame without a symbol or a positive
// finite value is skipped and counted, never guessed. Per underlying, a source timestamp equal to
// the last is a duplicate and an older one is out of order: both are dropped (Kalshi drops them
// too). Unknown fields are allowed.
//
// Fresh (plan section 5's pull rule): a value is fresh while our last message for it is younger
// than max(2 s, 8 x the median gap between its messages) and younger than 20 s. This is a claim
// about our observation only: a silence may mean the source stopped, or that the venue sent
// nothing because nothing changed, and the desk cannot tell which.
//
// Threading: ref-k's thread owns the socket and is the only writer. Readers (strat, ops) copy
// what they need under a short lock; nothing here is on an order's path.
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/md/feed.h"
#include "deskd/net/conn.h"

namespace deskd {
class Worker;
class KalshiSigner;

struct RefTick {
  std::int64_t source_ms = 0;   // the source's clock, as Kalshi relays it (0 = not given)
  std::int64_t recv_ms = 0;     // ours
  double value = 0;
};

struct RefSeries {
  std::string symbol;
  std::deque<RefTick> ticks;    // oldest first, the last history_ms by our clock
  std::deque<std::int64_t> gaps;   // the last 64 gaps between messages, ms
  std::int64_t messages = 0, duplicates = 0, out_of_order = 0, unchanged = 0;
  const RefTick* last() const { return ticks.empty() ? nullptr : &ticks.back(); }
  std::int64_t median_gap_ms() const;
};

class PythValues {
 public:
  struct Options {
    std::int64_t history_ms = 5400000;   // 90 minutes: an hour of volatility plus a window
    double gap_multiple = 8.0;
    std::int64_t gap_floor_ms = 2000;
    std::int64_t stale_ms = 20000;
  };
  PythValues() = default;
  explicit PythValues(Options o) : o_(o) {}
  // A pyth_value frame's msg. false (with why) when it carries nothing usable; it is counted.
  bool value(const nlohmann::json& msg, std::int64_t recv_ms, std::string* why);
  bool fresh(const RefSeries& s, std::int64_t now_ms) const;
  std::int64_t gap_limit_ms(const RefSeries& s) const;
  const std::map<std::string, RefSeries>& series() const { return series_; }
  std::int64_t malformed() const { return malformed_; }
 private:
  Options o_;
  std::map<std::string, RefSeries> series_;
  std::int64_t malformed_ = 0;
};

// The first symbol in `values` that contains any pattern (case-insensitive), exact names first;
// "" when none has arrived. KalshiPyth::symbol_for and the demo probe's feeds check share it.
std::string pyth_symbol_for(const PythValues& values, const std::vector<std::string>& patterns);

// The value of one underlying at one moment, for a model.
struct RefView {
  std::string symbol;
  double value = 0;
  std::int64_t source_ms = 0, recv_ms = 0;
  bool fresh = false;
  std::int64_t gap_limit_ms = 0;
  std::vector<std::pair<std::int64_t, double>> ticks;   // (source or recv ms, value), oldest first
};

class KalshiPyth {
 public:
  struct Options {
    int connect_ms = 2000, ping_ms = 10000, pong_ms = 5000, ack_ms = 5000;
    int backoff_min_ms = 250, backoff_max_ms = 15000;
    PythValues::Options values;
  };
  KalshiPyth(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer, FeedState& feed);
  KalshiPyth(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer, FeedState& feed,
             Options options);
  void run(Worker& self);
  // Any thread. The first symbol seen that contains any pattern (case-insensitive), exact names
  // first; "" when none has arrived.
  std::string symbol_for(const std::vector<std::string>& patterns) const;
  // Any thread: the underlying's value now, with its ticks since since_ms (none when since_ms < 0).
  std::optional<RefView> view(const std::string& symbol, std::int64_t since_ms, std::int64_t now_ms) const;
  nlohmann::json status(std::int64_t now_ms) const;
 private:
  net::Endpoint endpoint_;
  net::ConnFactory& factory_;
  const KalshiSigner& signer_;
  FeedState& feed_;
  Options options_;
  mutable std::mutex m_;   // values_ and counters; never held over I/O
  PythValues values_;
  std::int64_t connects_ = 0;
  std::string last_error_;
};

}  // namespace deskd
