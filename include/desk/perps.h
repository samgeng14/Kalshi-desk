#pragma once
// Kalshi's margined instance: the perpetual futures (KXBTCPERP, KXETHPERP, ...). A separate
// exchange instance with its own cash, its own REST surface under /margin/*, the same API key.
//
// The desk reads it (cash, equity, margin, positions, marks, funding), trades it by hand from a
// ticket behind its own rails, moves cash between the two instances, and runs one small
// systematic rule on it, the basis reversion. Nothing here touches the event-contract machinery:
// perps are priced per contract in dollars, fees are basis points of notional, P&L is continuous,
// and the exchange can liquidate a position. None of the binary-contract arithmetic applies.
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/store.h"

namespace desk {

struct PerpMarket {
  std::string ticker, title, status, asset_class;
  double tick = 0.0001, contract_size = 0, bid = 0, ask = 0, last = 0, mark = 0, liquidation_mark = 0, reference = 0;
  double leverage = 0, volume_24h = 0, open_interest = 0;
  long long mark_ts_ms = 0, reference_ts_ms = 0;
  bool is_open = true;
  bool fractional = false;
  double mid() const { return bid > 0 && ask > 0 ? 0.5 * (bid + ask) : (last > 0 ? last : mark); }
  // Basis of the traded price over the index, in basis points. Positive: the perp is rich.
  double basis_bps() const { return reference > 0 && mid() > 0 ? (mid() - reference) / reference * 1e4 : 0; }
};
struct PerpBook {
  std::string ticker;
  std::vector<BookLevel> bids, asks;   // best last, like the event-contract book
  double best_bid() const { return bids.empty() ? 0 : bids.back().price; }
  double best_ask() const { return asks.empty() ? 0 : asks.back().price; }
  double bid_size() const { return bids.empty() ? 0 : bids.back().qty; }
  double ask_size() const { return asks.empty() ? 0 : asks.back().qty; }
};
struct PerpPosition {
  int subaccount = 0;
  std::string ticker;
  double position = 0, entry_price = 0, unrealized_pnl = 0, margin_used = 0, fees = 0;
  // From /margin/risk: the exchange's own estimate of where it liquidates this position.
  double liquidation_price = 0, position_leverage = 0;
  bool is_portfolio = false;
};
struct PerpBalance {
  double settled_funds = 0, position_value = 0, equity = 0, maintenance_margin = 0, initial_margin = 0,
         resting_margin = 0, available = 0;
  long long updated_ts = 0;
};
struct PerpOrder {
  std::string order_id, client_order_id, ticker, side, status, created_time, last_update_time, last_update_reason,
      order_source, order_reason;
  double price = 0, fill_count = 0, remaining_count = 0;
};
struct PerpFill {
  std::string fill_id, order_id, ticker, side, created_time, order_source;
  double count = 0, price = 0, entry_price = 0, fees = 0, realized_pnl = 0;
  bool is_taker = false;
  long long ts = 0;
};
struct PerpFunding {
  std::string ticker, computed_time, next_funding_time;
  double rate = 0, mark = 0;
};
struct PerpOrderRequest {
  std::string ticker, side = "bid";   // bid = buy / go long, ask = sell / go short
  double count = 0, price = 0;
  bool post_only = false, reduce_only = false;
  std::string tif = "good_till_canceled";
  std::string client_order_id;
};

PerpMarket parse_perp_market(const json& j);
PerpBook parse_perp_book(const json& j, const std::string& ticker);
PerpPosition parse_perp_position(const json& j);
PerpBalance parse_perp_balance(const json& j);
PerpOrder parse_perp_order(const json& j);
PerpFill parse_perp_fill(const json& j);
json perp_order_body(const PerpOrderRequest& r);

// Rails for the margined instance, read from config["perps"]. Every hand ticket and every
// algorithm order passes through them; the exchange's own margin checks come after.
struct PerpsConfig {
  bool enabled = true;
  double max_order_notional = 30.0;    // dollars of notional per order
  double max_total_notional = 60.0;    // dollars of notional across all perp positions and resting orders
  double max_leverage = 2.0;           // total notional / perps equity
  double price_band_pct = 2.0;         // a limit price further than this from the mark is a fat finger
  int refresh_seconds = 10;            // public market data
  int account_refresh_seconds = 30;    // balance, positions, orders, fills
  std::vector<std::string> watch;      // empty = every active perp
  // The basis-reversion rule (docs/perps.md). Off unless started from the tab.
  double algo_entry_bps = 25.0;        // |mid - index| over the index at which a maker order is posted against the move
  double algo_exit_bps = 5.0;          // basis back inside this: take the position off
  double algo_stop_bps = 60.0;         // basis through this against the position: get out, taker
  double algo_max_hold_minutes = 120.0;
  double algo_contracts = 1.0;         // per market, used when algo_notional_dollars is 0
  // Dollars of notional per entry. A contract count is not one setting across this book: a WLD
  // contract is $0.40 and a ZEC contract is $11. 0 keeps the old contract-count behaviour.
  double algo_notional_dollars = 0.0;
  double algo_daily_loss_stop = 3.0;   // dollars
  std::vector<std::string> algo_markets = {"KXBTCPERP", "KXETHPERP"};
};
PerpsConfig perps_config(const nlohmann::json& params);

class PerpsDesk {
 public:
  PerpsDesk(const Config& cfg, Store& store, KalshiClient& client);

  // Cycle hooks. Public data is cheap (one request for every market); the account is read on
  // its own cadence and whenever something was sent. Exceptions are logged, never thrown.
  void refresh_public(long long now);
  void refresh_account(long long now, bool force = false);
  // The rule, when running. Under the engine lock.
  void step(long long now);

  json view();                                   // everything the tab shows
  json book(const std::string& ticker);          // live orderbook, top 10
  json ticket(const PerpOrderRequest& req, const std::string& note);   // rails, then the exchange
  json cancel(const std::string& order_id);
  // Every resting order the desk knows about on the margined instance (the kill switch).
  int cancel_all(const std::string& why);
  json close_position(const std::string& ticker);   // reduce-only IOC into the touch
  json transfer(const std::string& source, const std::string& destination, double dollars);
  json configure_algo(const json& body);         // {running:bool, paper:bool, settings:{...}}
  json summary();                                // for the header: equity, unrealized, positions, algo state
  // P&L of the margined instance as money: today = equity now against the first snapshot of the
  // local day, net of cash moved in or out since; total = equity against everything ever moved
  // in. Both are zero until the account has been read.
  json pnl();

  bool armed() const { return client_.authenticated(); }
  const PerpsConfig& config() const { return pc_; }

 private:
  void rails(const PerpOrderRequest& req, const PerpMarket& m, double* notional_out);
  std::optional<PerpMarket> market(const std::string& ticker);
  json algo_state_;      // running, paper, settings, log, positions (paper), cooldowns
  // Markets whose one-contract price is above the per-entry notional. Logged once per run rather
  // than once per cycle; cleared for a market as soon as it can be sized again.
  std::set<std::string> unsized_;
  void algo_persist();
  void algo_log(const std::string& level, const std::string& msg);
  double algo_pnl_today(long long now);
  std::vector<PerpPosition> positions_cache_;
  PerpBalance balance_cache_;
  std::map<std::string, PerpMarket> markets_cache_;
  std::map<std::string, PerpFunding> funding_cache_;
  std::map<std::string, long long> funding_ts_;
  json fee_tiers_;
  long long fee_ts_ = 0, public_ts_ = 0, account_ts_ = 0, last_write_ts_ = 0;
  std::string last_error_;
  const Config& cfg_;
  PerpsConfig pc_;
  Store& store_;
  KalshiClient& client_;
  // Recursive: step() sends and cancels through ticket()/cancel(), and configure_algo() returns
  // view(), all of which take the lock themselves. Network calls in the refreshes happen
  // outside it so the tab never waits on the exchange.
  std::recursive_mutex mu_;
  using Lock = std::lock_guard<std::recursive_mutex>;
};

}  // namespace desk
