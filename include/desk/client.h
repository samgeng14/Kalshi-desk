#pragma once
// Kalshi Trade API v2 client (REST). Public endpoints work without a Signer;
// portfolio and order endpoints need one.
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desk/auth.h"
#include "desk/http.h"
#include "desk/types.h"

namespace desk {

class ApiError : public std::runtime_error {
 public:
  ApiError(long status, std::string body, const std::string& what)
      : std::runtime_error(what), status(status), body(std::move(body)) {}
  long status;
  std::string body;
};

class KalshiClient {
 public:
  // base: e.g. https://external-api.demo.kalshi.co/trade-api/v2
  KalshiClient(std::string base, std::shared_ptr<Signer> signer, std::string user_agent);

  bool authenticated() const { return signer_ != nullptr; }
  const std::string& base() const { return base_; }
  // The same key the REST calls are signed with. The WebSocket feeds sign their own path with it
  // (include/desk/ws_client.h), so they need the signer rather than another copy of the key.
  const std::shared_ptr<Signer>& signer() const { return signer_; }

  // ---- public market data --------------------------------------------------
  std::vector<Series> get_series(const std::string& category = "");
  Series get_series_one(const std::string& ticker);
  std::vector<Market> get_markets(const std::string& series_ticker, const std::string& status = "open",
                                  int max_pages = 5);
  std::vector<Market> get_markets_by_event(const std::string& event_ticker);
  // Event metadata. `mutually_exclusive` is Kalshi's own flag: at most one market in the event
  // settles YES. Without it the desk must assume the brackets can settle independently.
  struct EventMeta { std::string event_ticker, title, series_ticker; bool mutually_exclusive = false; bool found = false; };
  EventMeta get_event(const std::string& event_ticker);
  std::vector<Market> get_markets_by_tickers(const std::vector<std::string>& tickers);
  Market get_market(const std::string& ticker);
  Orderbook get_orderbook(const std::string& ticker, int depth = 0);
  std::vector<Orderbook> get_orderbooks(const std::vector<std::string>& tickers);
  std::vector<Trade> get_trades(const std::string& ticker, long long min_ts = 0, long long max_ts = 0,
                                int max_pages = 10);

  // ---- portfolio (auth) -----------------------------------------------------
  Balance get_balance();
  // include_flat: also return markets with position 0 (they carry realized P&L from closes).
  std::vector<Position> get_positions(bool include_flat = false);
  // External cash flows (deposits, withdrawals, transfers to/from the margined instance): the
  // daily P&L subtracts them so moving money is never counted as a loss or a gain.
  json get_cashflows();  // -> [{id, ts, kind, amount}] kind: deposit | withdrawal | to_margined | from_margined
  // Move cash between exchange shards (Kalshi intra-account transfer; asynchronous). Dollars.
  std::string transfer_between_shards(int from_shard, int to_shard, double dollars);
  std::vector<Order> get_orders(const std::string& status = "resting", const std::string& ticker = "");
  std::vector<Fill> get_fills(long long min_ts = 0, int max_pages = 10);
  std::vector<Settlement> get_settlements(long long min_ts = 0, int max_pages = 10);

  // ---- orders (auth) ---------------------------------------------------------
  OrderResponse create_order(const OrderRequest& req);
  json cancel_order(const std::string& order_id, const std::string& ticker);

  // Translates an outcome-side request into the V2 bid/ask body. Exposed for tests.
  static json order_body(const OrderRequest& req);

  // Minimum spacing between exchange calls from this process, all threads together. Kalshi's
  // Basic tier allows 200 read tokens a second and most requests cost 10, so 20 reads a second
  // is the ceiling; the default sits under it. The old fixed 150 ms (~6/s) was the largest
  // single cost in a cycle: 32 watched series alone took 4.8 s of pure waiting.
  //
  // The spacing actually used adapts: a 429 doubles it (up to a second) and every successful
  // call shrinks it 5% back toward this floor, so a limit lower than expected (the public, keyless
  // limit is stricter than the tier's) costs a few slow calls instead of a storm of retries.
  void set_min_interval_ms(int ms) {
    std::lock_guard<std::mutex> lk(mu_);
    min_interval_ms_ = ms < 0 ? 0 : ms;
    interval_ms_ = min_interval_ms_;
  }
  int min_interval_ms() const { return min_interval_ms_; }
  int current_interval_ms() const { return interval_ms_; }

  // Generic call; path is relative to the base, e.g. "/markets".
  json call(const std::string& method, const std::string& path,
            const std::map<std::string, std::string>& query = {}, const json* body = nullptr);

 private:
  std::string base_;    // https://host/trade-api/v2
  std::string prefix_;  // /trade-api/v2
  std::shared_ptr<Signer> signer_;
  Http http_;
  void pace();                     // wait for this process's turn at the exchange
  void note_status(long status);   // adapt the spacing to what the exchange said
  std::mutex mu_;
  long long last_call_ms_ = 0;
  int min_interval_ms_ = 60;
  int interval_ms_ = 60;   // adaptive, >= min_interval_ms_; guarded by mu_
};

}  // namespace desk
