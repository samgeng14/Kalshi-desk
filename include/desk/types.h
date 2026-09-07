#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace desk {
using json = nlohmann::json;

struct Series {
  std::string ticker, category, title, fee_type = "quadratic", frequency;
  double fee_multiplier = 1.0;
};

struct Market {
  std::string ticker, event_ticker, series_ticker, title, subtitle, yes_sub_title,
      no_sub_title, status, market_type, strike_type, result, rules_primary, open_time,
      close_time, expected_expiration_time, expiration_time, updated_time;
  std::optional<double> floor_strike, cap_strike;
  double yes_bid = 0, yes_ask = 0, no_bid = 0, no_ask = 0, last = 0;
  double yes_bid_size = 0, yes_ask_size = 0;
  double volume = 0, volume_24h = 0, open_interest = 0, liquidity = 0;
  double tick = 0.01;
  json custom_strike;
  int exchange_index = 0;
  long long quote_ts = 0;    // when this quote snapshot was written to the store (0 = not from the store)

  bool tradable() const { return status == "active" || status == "open"; }
  // Mid of a two-sided book; a one-sided real quote otherwise; the last trade when the book is
  // empty. A bid of 0 or an ask of 1.00 is an empty side, not a price (closed markets report
  // 0 / 1.00 and used to mark a dead YES at 1.00).
  double mid() const {
    bool b = yes_bid > 0 && yes_bid < 1, a = yes_ask > 0 && yes_ask < 1;
    if (b && a) return 0.5 * (yes_bid + yes_ask);
    if (a) return yes_ask;
    if (b) return yes_bid;
    if (last > 0 && last < 1) return last;
    return last >= 1 ? 1.0 : (last > 0 ? last : 0.0);
  }
  // Value of one YES contract for P&L: the settled result when known, else the mid.
  double mark() const {
    if (result == "yes") return 1.0;
    if (result == "no") return 0.0;
    return mid();
  }
};

struct BookLevel {
  double price = 0, qty = 0;
};
struct Orderbook {
  std::string ticker;
  std::vector<BookLevel> yes_bids;  // bids for YES (== asks for NO at 1-p)
  std::vector<BookLevel> no_bids;   // bids for NO  (== asks for YES at 1-p)
  double best_yes_bid() const { return yes_bids.empty() ? 0 : yes_bids.back().price; }
  double best_no_bid() const { return no_bids.empty() ? 0 : no_bids.back().price; }
  double best_yes_ask() const { return no_bids.empty() ? 0 : 1.0 - no_bids.back().price; }
  double best_no_ask() const { return yes_bids.empty() ? 0 : 1.0 - yes_bids.back().price; }
};

struct Trade {
  std::string trade_id, ticker, taker_side, created_time;
  double count = 0, yes_price = 0, no_price = 0;
};

struct Balance {
  double balance = 0, portfolio_value = 0;
  long long updated_ts = 0;
};

struct Position {
  std::string ticker, updated;
  double position = 0;  // +YES / -NO contracts
  double exposure = 0, realized_pnl = 0, fees_paid = 0, total_traded = 0;
};

struct Order {
  std::string order_id, client_order_id, ticker, outcome_side, book_side, type, status,
      created_time, expiration_time, last_update_time;
  double yes_price = 0, no_price = 0, initial_count = 0, fill_count = 0,
         remaining_count = 0, taker_fees = 0, maker_fees = 0;
};

struct Fill {
  std::string fill_id, trade_id, order_id, ticker, outcome_side, book_side, created_time;
  double count = 0, yes_price = 0, no_price = 0, fee = 0;
  bool is_taker = false;
  long long ts = 0;
};

struct Settlement {
  std::string ticker, event_ticker, market_result, settled_time;
  double yes_count = 0, no_count = 0, yes_cost = 0, no_cost = 0, revenue = 0, fee = 0;
};

// What the desk wants to do, expressed in outcome terms.
struct OrderRequest {
  std::string ticker;
  std::string outcome_side = "yes";  // yes | no
  double price = 0;                  // price of that outcome, dollars
  double count = 0;                  // contracts
  bool post_only = false;
  bool reduce_only = false;
  std::string tif = "good_till_canceled";  // good_till_canceled | immediate_or_cancel | fill_or_kill
  std::string client_order_id;
  int exchange_index = -1;  // require ticker-based auto-routing unless explicitly set
};

struct OrderResponse {
  std::string order_id, client_order_id;
  double fill_count = 0, remaining_count = 0;
  std::optional<double> avg_fill_price, avg_fee;
  long long ts_ms = 0;
  json raw;
};

// ---- JSON parsing -----------------------------------------------------------
Series parse_series(const json& j);
Market parse_market(const json& j);
Orderbook parse_orderbook(const json& j, const std::string& ticker);
Trade parse_trade(const json& j);
Balance parse_balance(const json& j);
Position parse_position(const json& j);
Order parse_order(const json& j);
Fill parse_fill(const json& j);
Settlement parse_settlement(const json& j);
OrderResponse parse_order_response(const json& j);

json series_to_json(const Series& s);
json market_to_json(const Market& m);
json orderbook_to_json(const Orderbook& b);

std::string jstr(const json& j, const char* key, const std::string& dflt = "");
double jnum(const json& j, const char* key, double dflt = 0.0);
std::string series_from_event(const std::string& event_ticker);

}  // namespace desk
