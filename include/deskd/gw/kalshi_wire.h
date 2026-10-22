#pragma once
// Kalshi's Trade API v2 on the wire: request builders, reply parsers, and what a write's reply
// means (plan section 3). Shapes from docs.kalshi.com/api-reference (fetched 2026-10-06 and
// 2026-10-07; the fake exchange's comments list the same paths, tests/fake_exchange/fake_kalshi.h).
//
// Create order is V2, POST /portfolio/events/orders, in YES-book terms:
//   {ticker, side: "bid" (buys YES) | "ask" (sells YES, i.e. buys NO at 1 - price),
//    count: "10.00" (0-2 decimals), price: "0.5600" (the YES price, 2-4 decimals),
//    time_in_force: fill_or_kill | good_till_canceled | immediate_or_cancel,
//    self_trade_prevention_type: taker_at_cross | maker,                      (all required)
//    post_only, reduce_only (IOC only), cancel_order_on_pause, expiration_time (whole Unix
//    seconds, good_till_canceled only), client_order_id, order_group_id, subaccount, exchange_index}
// -> 201 {order_id, client_order_id, fill_count, remaining_count, average_fill_price and
//    average_fee_paid (only when fill_count > 0), ts_ms}.
// The V1 body (POST /portfolio/orders: count_fp, yes_price / no_price in cents or *_dollars,
// action buy|sell) is not used: V2 is the venue's current path, and the old desk sent V2 from
// 2026-09 (src/client.cpp order_body).
//
// Units are deskd's (bus/units.h): prices and money in 1/10,000 dollar, sizes in 1/100 contract.
// The venue writes fees, costs and averages with up to 6 decimals ("0.010000"), so those are kept
// in Micros (1/1,000,000 dollar) and never rounded on the way in. A field the venue left out is
// nullopt, never 0 (plan section 8: "a missing field is NULL, never 0"); a page with a row that
// breaks its shape fails whole, so a shape change is loud.
//
// Every write names its shard (exchange_index): the venue bills an auto-routed single order write
// to the unscoped write bucket AND every nonzero shard's (exchange_sharding.md), so the builders
// refuse a write without one.
//
// What a write's reply means (classify_write), the old desk's rules (executor.cpp:225-251) and the
// plan's: a timeout, a reset, a 5xx, a reply cut mid-way, a 408, or a 4xx without the venue's own
// error body (something in between answered) is UNKNOWN, never "failed"; nothing complete written
// is NOT_SENT; a 2xx whose body does not parse is UNKNOWN too (the venue took it, deskd cannot
// tell what it holds); a 429 is RETRYABLE (refused before processing).
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"
#include "deskd/gw/reject.h"
#include "deskd/net/http.h"

namespace deskd::kalshi {

using Micros = std::int64_t;   // 1/1,000,000 dollar: fees, costs and averages as the venue writes them

constexpr const char* kBasePath = "/trade-api/v2";

// ---- values on the wire ----------------------------------------------------------------------
std::string price_wire(Px p);    // 5600 -> "0.5600"
std::string count_wire(Qty q);   // 1000 -> "10.00", 1 -> "0.01"
// "0.5600", "0.56", "1", "-3.2100", "0.010000": up to 6 decimals, optional minus
std::optional<Micros> parse_dollars(const std::string& s);
// a price: 0..1 dollar on the 1/10,000 grid ("0.563333" is no price)
std::optional<Px> parse_px(const std::string& s);
// money in deskd's unit when it is a whole 1/10,000 ("-3.2100"); nullopt for finer values
std::optional<Money> parse_money_wire(const std::string& s);
// "10.00", "10", "-3.50": up to 2 decimals; negative only with allow_negative (a position)
std::optional<Qty> parse_count(const std::string& s, bool allow_negative = false);
// RFC 3339 with Z or an offset and any fraction -> ms since the epoch
std::optional<std::int64_t> parse_time_ms(const std::string& s);
std::string url_encode(const std::string& s);   // RFC 3986: unreserved kept, the rest %XX

// ---- requests --------------------------------------------------------------------------------
struct WireRequest {
  std::string method;
  std::string path;     // what is signed: base + route, no query ("/trade-api/v2/portfolio/balance")
  std::string query;    // "" or "a=1&b=2", encoded
  std::string body;     // JSON or ""
  bool auth = true;     // needs the KALSHI-ACCESS-* headers
  bool write = false;   // changes state: one per connection, never retried, UNKNOWN on doubt
  WireOp op = WireOp::OTHER;
  std::string target() const { return query.empty() ? path : path + "?" + query; }
};

enum class BookSide : std::uint8_t { BID, ASK };   // bid buys YES; ask sells YES (buys NO)
enum class TimeInForce : std::uint8_t { FOK, GTC, IOC };
enum class Stp : std::uint8_t { TAKER_AT_CROSS, MAKER };
const char* book_side_name(BookSide s);   // "bid", "ask"
const char* tif_name(TimeInForce t);      // "fill_or_kill", "good_till_canceled", "immediate_or_cancel"
const char* stp_name(Stp s);              // "taker_at_cross", "maker"

struct CreateOrder {
  std::string ticker;
  BookSide side = BookSide::BID;
  Qty count = 0;
  Px price = 0;                    // the YES price
  TimeInForce tif = TimeInForce::IOC;
  bool post_only = false;
  bool reduce_only = false;        // IOC only
  bool cancel_on_pause = false;
  std::int64_t expiration_s = 0;   // whole Unix seconds, GTC only; 0 = none
  Stp stp = Stp::TAKER_AT_CROSS;
  std::string client_order_id;     // required: every write carries one (plan section 2)
  std::string order_group_id;      // "" = none
  int exchange_index = -1;         // required, >= 0
  int subaccount = -1;             // -1 = left out (the primary)
};

// "" when the order may be sent, else why not: a desk bug, refused before it costs a write.
// The price grid is the market's (gw checks it against price_ranges); this checks 0 < p < $1.
std::string check_create(const CreateOrder& o);

// A deskd intent in venue terms: BUY -> bid, SELL -> ask at the same YES price; TAKE IOC/FOK;
// POST -> good_till_canceled + post_only + cancel_order_on_pause + expiration_time =
// floor(now_ms / 1000) + ttl_s (so it dies ttl_s - 1 to ttl_s after sending, plan section 3) or
// exp_ms / 1000; Manual's orders self-trade "maker" (your click cancels our quote), Systematic's
// "taker_at_cross" (plan section 3).
CreateOrder from_intent(const PlaceIntent& in, int exchange_index, std::int64_t now_ms);

// Each builder returns false with *why (and leaves *out alone) on a request it will not build.
bool build_create(const CreateOrder& o, WireRequest* out, std::string* why);
// DELETE /portfolio/events/orders/{id}?exchange_index=N[&market_ticker=T][&subaccount=S]
bool build_cancel(const std::string& order_id, int exchange_index, const std::string& market_ticker, int subaccount,
                  WireRequest* out, std::string* why);
struct BatchCancelItem {
  std::string order_id;
  int exchange_index = -1;        // required, >= 0
  std::string market_ticker;      // optional
  int subaccount = -1;
};
// DELETE /portfolio/events/orders/batched {orders:[...]}; 1 to kBatchCancelMax orders
constexpr std::size_t kBatchCancelMax = 20;
bool build_batch_cancel(const std::vector<BatchCancelItem>& items, WireRequest* out, std::string* why);
// DELETE /portfolio/events/orders[?subaccount=S]: every resting order (204); KILL's backstop
WireRequest build_cancel_all(int subaccount = -1);

// Order groups (plan section 3: one per Systematic lane and shard; groups do not span shards).
// POST /portfolio/order_groups/create {contracts_limit, contracts_limit_fp, exchange_index}: the
// limit is whole contracts over the venue's rolling 15 s window of fills; going over it cancels
// every order in the group and refuses new ones until a reset.
bool build_create_order_group(Qty contracts_limit, int exchange_index, int subaccount, WireRequest* out, std::string* why);
// PUT /portfolio/order_groups/{id}/{trigger|reset}?exchange_index=N[&subaccount=S]
bool build_order_group_action(const std::string& id, const std::string& action, int exchange_index, int subaccount,
                              WireRequest* out, std::string* why);
// POST /portfolio/intra_exchange_instance_transfer: free cash from one shard to another (the
// ticket's MOVE). The body is the old desk's, used live (src/client.cpp transfer_between_shards):
// {source, destination: "event_contract", amount in 1/10,000 $, source_exchange_shard,
// destination_exchange_shard}; the reply {transfer_id}.
bool build_transfer(int from_shard, int to_shard, Money amount, WireRequest* out, std::string* why);
// DELETE /portfolio/order_groups/{id}?exchange_index=N[&subaccount=S]: its orders cancelled, the group gone
bool build_delete_order_group(const std::string& id, int exchange_index, int subaccount, WireRequest* out,
                              std::string* why);
std::optional<std::string> parse_create_order_group(const std::string& body, std::string* why);

struct ListQuery {   // the read routes' shared filters; empty / -1 / 0 = left out
  std::string ticker;             // fills: comma-separated, up to 100
  std::string event_ticker;
  std::string status;             // orders: resting | canceled | executed
  std::string order_id;           // fills
  std::int64_t min_ts = 0, max_ts = 0;   // Unix seconds
  int limit = 0;                  // 1..1000 (the venue's default 100)
  std::string cursor;
  int exchange_index = -1;
  int subaccount = -1;
  std::string count_filter;       // positions: position | total_traded
  std::string settlement_status;  // positions: unsettled | settled | all
};
WireRequest build_get_order(const std::string& order_id);
WireRequest build_get_orders(const ListQuery& q);
// GET /portfolio/order_groups: the account's order groups (deskctl's direct KILL triggers each)
WireRequest build_get_order_groups(int subaccount = -1);
struct OrderGroupRow {
  std::string id;
  std::optional<int> exchange_index;   // not always in the row: the caller assumes shard 0
};
std::optional<std::vector<OrderGroupRow>> parse_order_groups(const std::string& body, std::string* why);
WireRequest build_get_fills(const ListQuery& q);
WireRequest build_get_positions(const ListQuery& q);
WireRequest build_get_balance(int exchange_index = -1, int subaccount = -1);
WireRequest build_get_market(const std::string& ticker);
WireRequest build_get_series(const std::string& ticker);
struct MarketsQuery {
  int limit = 0;                  // 1..1000
  std::string cursor, event_ticker, series_ticker, status, tickers;
  std::int64_t min_close_ts = 0, max_close_ts = 0;
  std::string mve_filter;         // only | exclude
};
WireRequest build_get_markets(const MarketsQuery& q);
WireRequest build_get_orderbook(const std::string& ticker, int depth = 0);
// GET /markets/trades (public): a market's prints, newest first, paged by cursor; the fill model's
// backfill (strat/fill_model.h). min_ts / max_ts in Unix seconds.
struct TradesQuery {
  std::string ticker, cursor;
  std::int64_t min_ts = 0, max_ts = 0;
  int limit = 0;                  // 1..1000
};
WireRequest build_get_trades(const TradesQuery& q);
// GET /incentive_programs (public): Kalshi's incentive programs, `limit` a page. The venue ignores
// its documented `type` filter (scripts/incentive_audit.py), so callers read incentive_type.
WireRequest build_get_incentive_programs(const std::string& status, const std::string& cursor, int limit);
WireRequest build_get_exchange_status();

// The bytes to write: request line, Host, the auth headers when given, Accept and Content-Type,
// and Connection (close for a write: the connection is spent anyway).
std::string request_bytes(const WireRequest& r, const std::string& host_header,
                          const std::vector<std::pair<std::string, std::string>>& auth_headers,
                          const std::string& user_agent = "deskd/1");

// ---- replies ---------------------------------------------------------------------------------
struct ApiError {
  int http = 0;
  bool venue_body = false;   // the venue's own shape: {"error":{"code","message","details"?}}, or a
                             // 429's {"error":"too many requests"}; false: something in between
  std::string code, message, details;
  std::string text() const;  // code + " " + message + " " + details, for classify_refusal
};
ApiError parse_error(int status, const std::string& body);

struct CreateAck {
  std::string order_id, client_order_id;
  Qty fill_count = 0, remaining = 0;
  std::optional<Micros> avg_fill_price, avg_fee;   // only when fill_count > 0
  std::int64_t ts_ms = 0;
};
struct CancelAck {
  std::string order_id, client_order_id;
  Qty reduced_by = 0;              // what was resting when it went: raises filled, never the position
  std::int64_t ts_ms = 0;
};
struct BatchCancelRow {
  std::string order_id, client_order_id;
  Qty reduced_by = 0;
  std::optional<std::int64_t> ts_ms;
  std::optional<ApiError> error;
};

enum class OrderStatus : std::uint8_t { RESTING, CANCELED, EXECUTED };
const char* order_status_name(OrderStatus s);
struct OrderRow {
  std::string order_id, client_order_id, ticker;
  BookSide side = BookSide::BID;
  OrderStatus status = OrderStatus::RESTING;
  Px yes_price = 0;
  Qty filled = 0, remaining = 0, initial = 0;
  Micros taker_fees = 0, maker_fees = 0, taker_cost = 0, maker_cost = 0;
  std::optional<std::int64_t> expiration_ms, created_ms, updated_ms;
  std::optional<Stp> stp;
  std::string order_group_id;
  bool cancel_on_pause = false;        // demo never echoed it (2026-10-07): false says nothing
  std::optional<int> subaccount;
  std::optional<int> exchange_index;   // not in the schema's required list (get-order.md)
};
struct FillRow {
  std::string fill_id, trade_id, order_id, ticker;
  BookSide side = BookSide::BID;
  Qty count = 0;
  Px yes_price = 0;
  bool taker = false;
  Micros fee = 0;
  std::int64_t ts_ms = 0;          // created_time; ts (seconds) when it is missing
  int exchange_index = 0;
  std::optional<int> subaccount;
};
struct PositionRow {
  std::string ticker;
  int exchange_index = 0;
  Qty position = 0;                // signed: YES long > 0, NO long < 0
  Micros exposure = 0, realized = 0, fees = 0, traded = 0;
  std::optional<std::int64_t> updated_ms;
};
struct Balance {
  Money available = 0;             // balance_dollars (cents x 100 when the dollars field is absent)
  std::optional<Money> portfolio_value;
  std::int64_t updated_ts = 0;
  std::vector<std::pair<int, Money>> by_shard;   // balance_breakdown
};
struct PriceRange {
  Px start = 0, end = 0, step = 0;
};
struct MarketRow {
  std::string ticker, event_ticker, status, market_type, price_level_structure;
  std::string title, subtitle, yes_sub_title, category, result;
  std::optional<int> exchange_index;
  std::optional<Px> yes_bid, yes_ask, no_bid, no_ask, last;
  std::optional<Qty> yes_bid_size, yes_ask_size, volume, volume_24h, open_interest;
  std::optional<std::int64_t> open_ms, close_ms, expected_expiration_ms, expiration_ms;
  std::vector<PriceRange> price_ranges;
  // fractional_trading_enabled; absent means whole contracts only (Kalshi enables fractional
  // order sizes market by market; docs.kalshi.com/getting_started/fixed_point_migration).
  std::optional<bool> fractional;
  // What the contract pays on (greater, greater_or_equal, less, less_or_equal, between, ...),
  // for a terminal-price model; a strike that is absent or not a number is left out, never 0.
  std::string strike_type;
  std::optional<double> floor_strike, cap_strike;
  // Enriched only by the public series endpoint, never a guessed market-prefix fee.
  std::string fee_type;
  std::optional<double> fee_multiplier;
  std::int64_t fee_ms = 0;
};
// An event (GET /events/{event_ticker}): the markets that settle on one outcome (a city's high on
// one day, one game). gw/event_risk.h reads how they settle together from the flag and their strikes.
struct EventRow {
  std::string event_ticker, series_ticker, title, sub_title, category;
  std::optional<bool> mutually_exclusive;   // absent stays unknown: never read as known
  std::string collateral_return_type;       // shown only; never relied on for cash
  std::vector<MarketRow> markets;           // every market of the event, any status
  std::int64_t fetched_ms = 0;
};
struct SeriesRow {
  std::string ticker, category, fee_type;
  double fee_multiplier = 0;
};
struct Orderbook {
  std::vector<std::pair<Px, Qty>> yes, no;   // as sent: ascending, the best last
};
struct ShardStatus {
  int exchange_index = 0;
  bool exchange_active = false, trading_active = false;
  std::optional<bool> transfers_active;   // intra_exchange_transfers_active
};
struct ExchangeStatus {
  bool exchange_active = false, trading_active = false;
  // exchange_index_statuses: one row per shard (demo listed shards 0-3 on 2026-10-07); empty when
  // the venue leaves it out
  std::vector<ShardStatus> shards;
};
template <class T>
struct Page {
  std::vector<T> rows;
  std::string cursor;   // "" = the last page
};

// Each returns nullopt with *why when the body does not have the documented shape.
std::optional<CreateAck> parse_create_ack(const std::string& body, std::string* why);
std::optional<CancelAck> parse_cancel_ack(const std::string& body, std::string* why);
std::optional<std::vector<BatchCancelRow>> parse_batch_cancel(const std::string& body, std::string* why);
std::optional<OrderRow> parse_order(const std::string& body, std::string* why);   // {"order":{...}}
std::optional<Page<OrderRow>> parse_orders(const std::string& body, std::string* why);
std::optional<Page<FillRow>> parse_fills(const std::string& body, std::string* why);
struct TradeRow {               // a public print
  std::string trade_id, ticker;
  Px yes_price = 0;
  Qty count = 0;
  bool taker_bought_yes = true;   // taker_side "yes": the aggressor bought YES (a print on the ask)
  std::int64_t ts_ms = 0;
};
std::optional<Page<TradeRow>> parse_trades(const std::string& body, std::string* why);
// A liquidity incentive program (LIP) on one market: period_reward (in 1/10,000 $, the venue's
// centi-cents) is paid over [start, end) on per-second book snapshots; target_size and the
// discount per cent from the best price decide what scores (strat/lip.h)
struct IncentiveRow {
  std::string market_ticker, type;   // type: the row's incentive_type ("liquidity", "volume", ...)
  std::int64_t period_reward = 0;    // 1/10,000 $
  Qty target_size = 0;
  int discount_bps = 0;
  std::int64_t start_ms = 0, end_ms = 0;
};
// Every row's type; only liquidity rows are read whole (one that breaks its shape fails the page:
// a shape change is loud); the others carry their ticker and type. next_cursor, else cursor.
std::optional<Page<IncentiveRow>> parse_incentive_programs(const std::string& body, std::string* why);
std::optional<Page<PositionRow>> parse_positions(const std::string& body, std::string* why);
std::optional<Balance> parse_balance(const std::string& body, std::string* why);
std::optional<MarketRow> parse_market(const std::string& body, std::string* why);   // {"market":{...}}
std::optional<SeriesRow> parse_series(const std::string& body, std::string* why);   // {"series":{...}}
std::optional<Page<MarketRow>> parse_markets(const std::string& body, std::string* why);
std::optional<Orderbook> parse_orderbook(const std::string& body, std::string* why);
std::optional<ExchangeStatus> parse_exchange_status(const std::string& body, std::string* why);

// ---- what a write's reply means ----------------------------------------------------------------
struct WriteOutcome {
  enum class Kind : std::uint8_t { ACK, REFUSED, UNKNOWN, NOT_SENT };
  Kind kind = Kind::UNKNOWN;
  int http = 0;
  RejectClass cls = RejectClass::UNKNOWN;   // REFUSED only
  ApiError error;                           // REFUSED only
  std::string why;                          // UNKNOWN and NOT_SENT; REFUSED: the error text
};
const char* write_kind_name(WriteOutcome::Kind k);
// For a write: CREATE (201 with a create ack), CANCEL (200 with a cancel ack), BATCH_CANCEL (200
// with its rows), CANCEL_ALL (204). A 2xx whose body fails its parser is UNKNOWN.
WriteOutcome classify_write(WireOp op, const net::Exchange& x);

}  // namespace deskd::kalshi
