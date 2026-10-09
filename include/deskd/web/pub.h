#pragma once
// pub (plan section 2): the owner of what the screen is sent. It keeps the published view
// (hdr, alerts, subscriptions), pushes every stream's messages into that stream's bounded
// queue, and takes snapshots on its own thread, so a snapshot's seq is exactly the last message
// of that stream it includes: nothing can be pushed between reading the state and the counter.
//
// Rates (contract): hb exactly 1/s to every stream; hdr only on change, at most 4/s.
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#include "deskd/bus/queue.h"
#include "deskd/md/feed.h"
#include "deskd/web/contract.h"
#include "deskd/web/streams.h"

namespace deskd {

class Worker;

struct PubReply {
  int status = 200;
  nlohmann::json body;
};
using PubPromise = std::shared_ptr<std::promise<PubReply>>;

struct PubSnapshot {
  std::string sid;
  PubPromise reply;
};
struct PubSubscribe {
  std::string sid;
  Subs subs;
  PubPromise reply;
};
struct PubAlert {
  std::string lvl;   // INFO WARN CRIT
  std::string src;   // GW KX PM SYS KILL OPS ...
  std::string msg;
  std::int64_t ts_ms = 0;
};
// Producers send contract-shaped full replacements, never deltas. A book is FULL depth;
// pub derives each tab's TOP view. mark_only permits 1 Hz coalescing of valuation changes;
// fills and reconciled positions must leave it false so their accounting is immediate.
struct PubData {
  std::string type;
  nlohmann::json data;
  bool mark_only = false;
};
// A complete, reconciled cache replacement (not an unpaged REST response). Empty venue
// replaces every venue. Missing active orders must be resolved by gw before sending this:
// pub never infers a cancellation from an order absent in a REST response.
struct PubReplace {
  std::string type;
  std::vector<nlohmann::json> rows;
  std::string venue;
};
using PubMsg = std::variant<PubSnapshot, PubSubscribe, PubAlert, PubData, PubReplace>;

class Pub {
 public:
  using HdrFn = std::function<HdrInputs()>;
  using SubsFn = std::function<void(const std::string&, const Subs&)>;
  static constexpr std::size_t kAlertsKept = 200;           // the snapshot's "last 200"
  static constexpr std::int64_t kForgetSidMs = 60000;        // a closed sid keeps its subs 60 s

  Pub(std::string epoch, const Feeds& feeds, HdrFn hdr, int max_streams, std::size_t stream_cap = 4096);

  // snapshot and subscribe requests go first: a web thread is waiting on them
  bool post(PubMsg m);
  bool healthy() const { return !overflow_.load(std::memory_order_acquire); }
  void alert(const std::string& lvl, const std::string& src, const std::string& msg);
  // Set before run(). Called on pub's owner thread: enqueue live-set work, never do I/O here.
  void set_subs_callback(SubsFn fn) { subs_fn_ = std::move(fn); }

  StreamRegistry& streams() { return streams_; }
  const std::string& epoch() const { return epoch_; }
  std::size_t stream_cap() const { return stream_cap_; }

  // hb.data for sid now: the first message of a new stream is built on the web thread, before
  // the stream is visible to pub, so it is always seq 1
  std::string hb_data(const std::string& sid, std::int64_t now_ms) const;

  void run(Worker& self);

  std::uint64_t hb_sent() const { return hb_sent_.load(); }
  std::uint64_t hdr_sent() const { return hdr_sent_.load(); }

 private:
  void handle(PubMsg& m);
  void broadcast(const std::string& type, const std::string& data_json, std::int64_t now);
  void broadcast_hb(std::int64_t now);
  void maybe_hdr(bool force);
  void data(PubData update);
  void replace(const PubReplace& update);
  void flush_due(std::int64_t mono);
  void prune(std::int64_t now);
  nlohmann::json snapshot(const std::string& sid, std::uint64_t seq, std::int64_t now) const;
  void book_to(const std::shared_ptr<Stream>& stream, const Subs& subs,
               const nlohmann::json& book, std::int64_t now, bool no_prints = false);
  void publish_book(const std::string& key, nlohmann::json book, std::int64_t mono);
  using Rows = std::map<std::string, nlohmann::json>;
  Rows* rows(const std::string& type);
  const Rows* rows(const std::string& type) const;
  nlohmann::json alerts_json() const;

  const std::string epoch_;
  const Feeds& feeds_;
  HdrFn hdr_fn_;
  SubsFn subs_fn_;
  const std::size_t stream_cap_;
  StreamRegistry streams_;
  BoundedQueue<PubMsg> inbox_;
  std::atomic<bool> overflow_{false};
  bool overflow_reported_ = false; // owner thread; restart required after a lost upstream update
  // pub thread only
  std::deque<nlohmann::json> alerts_;
  std::uint64_t alert_n_ = 0;
  nlohmann::json hdr_;
  std::string hdr_dump_;
  std::int64_t hdr_at_ = 0;   // steady clock
  Rows markets_, books_, orders_, positions_, fills_, lanes_, mos_;
  Rows pending_books_, pending_positions_;
  nlohmann::json risk_ = {{"groups", nlohmann::json::array()}, {"shards", nlohmann::json::array()}};
  nlohmann::json pending_risk_;
  std::map<std::string, std::int64_t> book_at_, position_at_, market_closed_at_;
  std::int64_t risk_at_ = 0;
  std::string day_;
  std::atomic<std::uint64_t> hb_sent_{0}, hdr_sent_{0};
};

}  // namespace deskd
