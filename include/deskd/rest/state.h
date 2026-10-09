#pragma once
// The REST owner publishes complete, immutable account/listing revisions. A failed page never
// becomes a partial account: readers keep the last values with ready=false. Nothing here writes
// to the venue. Every connection is opened by the guarded ConnFactory, including reconnects.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/conn.h"

namespace deskd::rest {

struct ClockEstimate {
  // local clock minus venue. HTTP Date has a whole-second quantization interval; we retain
  // that interval plus network uncertainty instead of pretending Date is a millisecond clock.
  std::optional<std::int64_t> offset_ms, uncertainty_ms;
  std::int64_t measured_ms = 0;
  std::size_t samples = 0;
  bool post_trusted = false;  // the entire offset interval, not its midpoint, is inside +/-500ms
};

class ClockCheck {
 public:
  void observe(const std::string& http_date, std::int64_t sent_ms, std::int64_t received_ms,
               std::int64_t elapsed_mono_ms);
  ClockEstimate estimate(std::int64_t now_ms) const;
 private:
  struct Sample { std::int64_t lo, hi, at; };
  std::vector<Sample> samples_;
};

struct Snapshot {
  std::map<std::string, kalshi::MarketRow> markets;
  kalshi::Balance balance;
  std::map<int, Money> balances;
  std::vector<kalshi::PositionRow> positions;
  std::vector<kalshi::OrderRow> orders;
  std::vector<kalshi::FillRow> fills;
  kalshi::ExchangeStatus exchange;
  ClockEstimate clock;
  std::set<std::string> pins, live;
  bool ready = false, account_ready = false, listing_ready = false;
  std::int64_t account_ms = 0, listing_ms = 0, ts_ms = 0;
  std::uint64_t revision = 0;
  std::string error;
};

struct Options {
  int poll_ms = 12000, listing_poll_ms = 60000;
  int timeout_ms = 3000, page_size = 1000, max_pages = 300;
  // Public listing horizon. UI still admits only outcome dates T/T1; a longer close horizon
  // includes games whose published trading close is after their expected outcome date.
  int listing_horizon_days = 4;
};

class State {
 public:
  using Callback = std::function<void(std::shared_ptr<const Snapshot>)>;
  State(net::ConnFactory& factory, net::Endpoint endpoint, const KalshiSigner* signer,
        Callback callback = {}, Options options = {});
  void run(Worker& worker);
  bool refresh_once();  // owner-thread only; synchronous entry for tests/probes
  std::shared_ptr<const Snapshot> snapshot() const;
  void set_pins(std::set<std::string> pins);  // immediate immutable publication; no network
  void set_live(std::set<std::string> live);  // ladders/lanes; positions/orders are also included
  void request_refresh();
  // Set before run(). A private-event generation fences the whole paged read: an event or
  // reconnect during it means those pages cannot be accepted as one reconciled account.
  void set_reconcile_guard(std::function<std::uint64_t()> begin,
                           std::function<bool(std::uint64_t)> commit) {
    begin_ = std::move(begin); commit_ = std::move(commit);
  }

 private:
  bool refresh(Worker* worker);
  bool get(kalshi::WireRequest request, std::string* body, std::string* why, Worker* worker);
  bool account(Snapshot& next, std::string* why, Worker* worker);
  bool listing(Snapshot& next, std::string* why, Worker* worker);
  void publish(Snapshot next);
  void controls(bool pins, std::set<std::string> values);
  net::ConnFactory& factory_;
  net::Endpoint endpoint_;
  const KalshiSigner* signer_;
  Callback callback_;
  Options options_;
  std::function<std::uint64_t()> begin_;
  std::function<bool(std::uint64_t)> commit_;
  std::shared_ptr<const Snapshot> published_;
  std::mutex controls_mu_;  // controls/publication only, never held over I/O or callbacks
  std::set<std::string> pins_, live_;
  std::atomic<bool> refresh_requested_{false};
  ClockCheck clock_;
  std::unique_ptr<net::Conn> conn_;  // REST owner only; read connections can be reused
  std::int64_t started_ms_ = 0, last_listing_mono_ = 0;
};

// Contract 1.5 projections. Missing exchange_index remains null; the gateway must refuse a
// write without it. No model is invented: universe fair and edge are null. LIP remains false
// until incentive metadata is integrated. The fee display is conservatively QUAD x1.
std::string category(const kalshi::MarketRow& market);
std::string outcome_date(const kalshi::MarketRow& market);
nlohmann::json market_json(const kalshi::MarketRow& market, bool pin, bool live, std::int64_t now_ms);
nlohmann::json universe(const Snapshot& state, const nlohmann::json& query, std::int64_t now_ms);

}  // namespace deskd::rest
