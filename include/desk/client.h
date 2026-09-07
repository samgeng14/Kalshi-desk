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

  // Generic call; path is relative to the base, e.g. "/markets".
  json call(const std::string& method, const std::string& path,
            const std::map<std::string, std::string>& query = {}, const json* body = nullptr);

 private:
  std::string base_;    // https://host/trade-api/v2
  std::string prefix_;  // /trade-api/v2
  std::shared_ptr<Signer> signer_;
  Http http_;
  std::mutex mu_;
  long long last_call_ms_ = 0;
};

}  // namespace desk
