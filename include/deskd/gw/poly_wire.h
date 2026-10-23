#pragma once
// Polymarket US on the wire: request builders, strict reply parsers, the ledger's view of the
// venue's rows, what a write's reply means, the taker fee allowance and the weekly maintenance
// window (plan section 3). Pure: no sockets, no clock, no locks; any thread.
//
// The shapes are the old desk's reading of docs.polymarket.us (src/poly.cpp's parsers and
// poly_order_body, tests/test_poly.cpp's mock) as the fake Polymarket serves them
// (tests/fake_exchange/fake_poly.h). None is verified against the venue: tools/poly_checks reads
// the real ones (decisions 3), and a parser here is changed only from what it records.
//
//   api.polymarket.us (signed, gw/poly_sign.h)      gateway.polymarket.us (public)
//   POST /v1/orders                 create           GET /v2/leagues/{league}/events
//   POST /v1/order/{id}/cancel      {marketSlug}     GET /v1/markets?slug=a&slug=b
//   POST /v1/orders/open/cancel     {}               GET /v1/markets/{slug}/book
//   POST /v1/order/preview          {request}
//   GET  /v1/order/{id}, /v1/orders/open, /v1/portfolio/{positions,activities}, /v1/account/balances
//
// Create order, asynchronous (plan section 3: "asynchronous submission, GTD, post-only via
// participateDontInitiate, and manualOrderIndicator from the owner"):
//   {marketSlug, type: ORDER_TYPE_LIMIT, price: {value: "0.385", currency: USD}, quantity: 5 (whole
//    contracts), tif: TIME_IN_FORCE_IMMEDIATE_OR_CANCEL | _FILL_OR_KILL | _GOOD_TILL_DATE,
//    outcomeSide: OUTCOME_SIDE_YES, action: ORDER_ACTION_BUY | _SELL, manualOrderIndicator:
//    MANUAL_ORDER_INDICATOR_MANUAL (Manual's clicks) | _AUTOMATIC, synchronousExecution: false,
//    participateDontInitiate: true (post-only, GTD only), expireTime (GTD only: RFC 3339 UTC, a
//    whole second)}
// -> {id, executions: []}; the order's fills come later, by its order read and the activities.
// There is no client order id: an uncertain create is found again by its shape (gw/poly_book).
// The old desk's synchronousExecution: true with maxBlockTime is kept only as the fallback the
// open question names, should the async reply prove unusable. GTC is never sent: every order
// lives at most 60 s (plan section 3).
//
// Units are deskd's (bus/units.h): prices in 1/10,000 dollar, sizes in 1/100 contract, and money
// in Micros (1/1,000,000 dollar) where the venue writes more than four places. The venue writes
// money as {value: "0.3850", currency: "USD"}, as a decimal string or as a JSON number (balances);
// sizes as numbers or decimal strings ("5.00"). A field the venue left out is nullopt, never 0
// (plan section 8), and a row that breaks its shape fails the whole reply, so a shape change is
// loud.
//
// What a write's reply means (classify_write) is Kalshi's rule (gw/kalshi_wire.h): a timeout, a
// reset, a reply cut mid-way, a 5xx, a 408, or a 4xx without the venue's own error body {code,
// message} (code the gRPC status) is UNKNOWN; nothing complete written is NOT_SENT; a 2xx whose
// body does not parse is UNKNOWN; a 429 is RETRYABLE. classify_refusal is reject.h's with no
// self-trade or duplicate classes (the venue has neither a self-trade rule nor a client id).
//
// Fees: gw/fees.h, per fill to the cent, half to even, until the venue confirms it.
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"
#include "deskd/gw/fees.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/gw/reject.h"
#include "deskd/net/http.h"

namespace deskd::poly {

// Per IP, every host and thread together: docs/polymarket.md's 20 until tools/poly_checks
// records the venue's (decisions 3; the plan says 25).
constexpr int kRequestsPerSecond = 20;
constexpr int kLedgerShard = 0;              // the P ledger's one shard (Polymarket has none)
constexpr std::size_t kMarketsPerRequest = 40;   // slugs in one GET /v1/markets

// ---- values on the wire ----------------------------------------------------------------------
std::string price_wire(Px p);   // 3850 -> "0.385", 3800 -> "0.380", 3825 -> "0.3825": three places, four when needed
// Money: {value, currency USD} with the value a decimal string (up to 6 places) or a number, a
// bare decimal string, or a number. nullopt for anything else: another currency, finer than
// $0.000001, a boolean.
std::optional<Micros> parse_amount(const nlohmann::json& v);
// A price: an amount on the 1/10,000 grid, 0..1 dollar ({"value":"0.385"} -> 3850, 0.005 -> 50)
std::optional<Px> parse_px(const nlohmann::json& v);
// A size: 5, 2.5, "5", "5.00" -> 500, 250; at most 2 places; negative only with allow_negative
std::optional<Qty> parse_qty(const nlohmann::json& v, bool allow_negative = false);
std::string time_wire(std::int64_t ms);   // RFC 3339 UTC, the whole second below: "2026-10-06T15:31:00Z"

// ---- requests --------------------------------------------------------------------------------
enum class Host : std::uint8_t { API, GATEWAY };   // api.polymarket.us (signed) | gateway.polymarket.us (public)
struct WireRequest {
  std::string method;
  std::string path;     // what is signed: the route, no query ("/v1/portfolio/positions")
  std::string query;    // "" or "a=1&b=2", encoded
  std::string body;     // JSON or ""
  Host host = Host::API;
  bool auth = true;     // needs the X-PM-* headers
  bool write = false;   // changes state: one per connection, never retried, UNKNOWN on doubt
  WireOp op = WireOp::OTHER;
  std::string target() const { return query.empty() ? path : path + "?" + query; }
};

enum class Action : std::uint8_t { BUY, SELL };     // of the YES (long) side; a SELL when flat opens a short
enum class PmTif : std::uint8_t { IOC, FOK, GTC, GTD };   // deskd sends IOC, FOK and GTD
const char* action_wire(Action a);   // "ORDER_ACTION_BUY", "ORDER_ACTION_SELL"
const char* tif_wire(PmTif t);       // "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL", ...

struct CreateOrder {
  std::string slug;
  Action action = Action::BUY;
  Px price = 0;                    // the YES price
  Qty count = 0;                   // whole contracts
  PmTif tif = PmTif::IOC;
  bool post_only = false;          // participateDontInitiate; GTD only
  std::int64_t good_till_ms = 0;   // GTD only: the effective expiry (ord.exp_ms), a whole second
  bool manual = true;              // manualOrderIndicator MANUAL: Manual's own clicks only
  bool asynchronous = true;        // false: synchronousExecution true, the fallback
};

// A deskd intent in venue terms: TAKE -> IOC or FOK; POST -> GTD + post-only, expiring at exp_ms or
// floor(now_ms / 1000) + ttl_s seconds (Kalshi's rule), and never sooner than the first whole
// second after now_ms + gtd_min_lead_ms (the venue's minimum lead, so ord.exp_ms is what the
// venue holds); manual for Owner::MAN only. reduce_only has no venue flag: admission cuts the size.
CreateOrder from_intent(const PlaceIntent& in, std::int64_t now_ms, std::int64_t gtd_min_lead_ms);

// "" when the order may be sent, else why not: a desk bug, refused before it costs a write (the
// venue's 400 for one is a hard refusal, which trips KILL). Against the market's tick (the grid
// tick .. 1 - tick), its minimumTradeQty (0: none listed) and the clock: a GTD expiry must be ahead.
std::string check_create(const CreateOrder& o, Px tick, Qty min_qty, std::int64_t now_ms);

// Each builder returns false with *why (and leaves *out alone) on a request it will not build.
// build_create refuses what is wrong in the order itself; check_create also knows the market.
bool build_create(const CreateOrder& o, WireRequest* out, std::string* why);   // POST /v1/orders
bool build_cancel(const std::string& order_id, const std::string& slug, WireRequest* out, std::string* why);
WireRequest build_cancel_all();                                               // POST /v1/orders/open/cancel {}
bool build_preview(const CreateOrder& o, WireRequest* out, std::string* why); // POST /v1/order/preview: no write
WireRequest build_get_order(const std::string& order_id);
WireRequest build_get_open_orders();
// limit 1..1000 (0: the venue's default); cursor "" for the newest page
WireRequest build_get_activities(const std::string& cursor, int limit);
WireRequest build_get_positions(const std::string& cursor, int limit);
WireRequest build_get_balances();
// Public (gateway). 1 to kMarketsPerRequest slugs, in the order asked.
bool build_get_markets(const std::vector<std::string>& slugs, WireRequest* out, std::string* why);
WireRequest build_get_book(const std::string& slug);
WireRequest build_get_league_events(const std::string& league, int offset, int limit);

// The bytes to write: request line, Host, the auth headers when given, Accept and Content-Type,
// and Connection (close for a write: the connection is spent anyway).
std::string request_bytes(const WireRequest& r, const std::string& host_header, const net::HeaderList& auth_headers,
                          const std::string& user_agent = "deskd/1");

// ---- replies ---------------------------------------------------------------------------------
struct SideInfo {   // one of a market's two sides; the team as the venue names it
  std::string description, abbr /* lower case */, name, safe_name, alias;
  bool is_long = false;            // the market's YES
};
struct Market {
  std::string slug, event_slug, event_title, question, league, sports_type;
  std::optional<Px> tick;          // orderPriceMinTickSize
  std::optional<Qty> min_qty;      // minimumTradeQty
  std::optional<double> fee_coeff; // feeCoefficient: the taker's
  std::optional<Px> bid, ask;      // bestBidQuote, bestAskQuote: left out when that side is empty
  std::optional<std::int64_t> start_ms, end_ms, updated_ms;   // gameStartTime (else startDate), endDate, updatedAt
  bool active = false, closed = false;   // absent: not active, not closed
  std::vector<SideInfo> sides;     // the long side first
};
struct LeaguePage {
  std::vector<Market> markets;     // every market of every event, its event and league filled in
  std::size_t events = 0;          // events on the page: a short page is the last
};
struct Book {
  std::vector<std::pair<Px, Qty>> bids, asks;   // best first
};
enum class OrderState : std::uint8_t { PENDING_NEW, NEW, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED, EXPIRED, PENDING_CANCEL };
const char* order_state_name(OrderState s);   // "pending_new", "new", ...
bool order_open(OrderState s);                // PENDING_NEW, NEW, PARTIALLY_FILLED, PENDING_CANCEL
struct Order {
  std::string id, slug;
  Action action = Action::BUY;
  PmTif tif = PmTif::IOC;
  OrderState state = OrderState::NEW;
  Px price = 0;
  Qty quantity = 0, cum = 0, leaves = 0;
  // avgPx: what a YES contract cost on average, to the micro-dollar (fills at several prices
  // average off the price grid); present once cum is above 0
  std::optional<Micros> avg_px;
  Micros commission = 0;           // commissionNotionalTotalCollected
  std::optional<std::int64_t> created_ms, updated_ms;
};
struct Trade {   // one of our fills, as the activities list it: no order id, no side, no fee
  std::string id, slug;
  Px price = 0;
  Qty qty = 0;
  bool aggressor = false;
  std::optional<Micros> cost_basis, realized;   // costBasis, realizedPnl
  std::int64_t ts_ms = 0;
};
struct Position {
  std::string slug;
  Qty net = 0;                     // signed: long YES > 0
  Micros cost = 0;
  std::optional<Micros> realized, cash_value;
  std::optional<Qty> available;    // qtyAvailableDecimal
  std::optional<bool> expired;
  std::optional<std::int64_t> updated_ms;
};
struct Balance {   // the USD row
  Micros current = 0, buying_power = 0;
  std::optional<Micros> asset_notional, asset_available;
};
struct Activity {
  enum class Kind : std::uint8_t { TRADE, DEPOSIT, WITHDRAWAL, OTHER };
  Kind kind = Kind::OTHER;
  std::string type;                // the venue's, "ACTIVITY_TYPE_TRADE": OTHER keeps it for whoever learns it
  Trade trade;                     // TRADE
  std::string id;                  // DEPOSIT, WITHDRAWAL: the transaction id
  Micros amount = 0;               // DEPOSIT > 0, WITHDRAWAL < 0
  std::int64_t ts_ms = 0;          // TRADE, DEPOSIT, WITHDRAWAL
};
struct Execution {   // a synchronous create's fill, as the reply lists it
  std::string trade_id;
  Qty qty = 0;
  Px price = 0;
  std::optional<Micros> fee;       // commissionNotionalCollected
  std::optional<OrderState> state; // the order's, after this fill
};
struct CreateAck {
  std::string order_id;
  std::vector<Execution> executions;   // empty when asynchronous
};

// Each returns nullopt with *why when the body does not have the shape above.
std::optional<LeaguePage> parse_league_events(const std::string& body, const std::string& league, std::string* why);
std::optional<std::vector<Market>> parse_markets(const std::string& body, std::string* why);
std::optional<Book> parse_book(const std::string& body, std::string* why);
std::optional<Order> parse_order(const std::string& body, std::string* why);   // {"order":{...}}
std::optional<std::vector<Order>> parse_open_orders(const std::string& body, std::string* why);
// newest first; cursor: nextCursor, "" on the last page (eof)
std::optional<kalshi::Page<Activity>> parse_activities(const std::string& body, std::string* why);
std::optional<kalshi::Page<Position>> parse_positions(const std::string& body, std::string* why);
std::optional<Balance> parse_balance(const std::string& body, std::string* why);
std::optional<CreateAck> parse_create_ack(const std::string& body, std::string* why);
std::optional<std::vector<std::string>> parse_cancel_all(const std::string& body, std::string* why);   // canceledOrderIds

// ---- the ledger's view (gw/order_ledger.h books Polymarket's rows as Kalshi's) -------------------
// No client order id; the slug as the ticker; BUY bids, SELL asks at the same YES price; open
// states RESTING, FILLED EXECUTED, the rest CANCELED (end_why says which); the cost of what the
// side bought, as Kalshi's taker_cost (a SELL bought NO at 1 - avg); the commission as taker_fees;
// shard kLedgerShard.
kalshi::OrderRow to_ledger(const Order& o);
// A trade tied to its order (by the caller: the venue's trade names neither the order nor its
// side). The fee is the model's (gw/fees.h): the taker's at coeff for an aggressor, else the
// maker's rebate, since the trade carries none.
kalshi::FillRow to_ledger(const Trade& t, const std::string& order_id, Action action, double coeff);
std::string end_why(const Order& o);   // "EXCHANGE" rejected, "EXPIRY" expired, else ""

// ---- fees -------------------------------------------------------------------------------------
// The most an order's taker fills can be charged, for the buying-power check: the fee rounded
// once at the worst price the order can fill at (p(1 - p) nearest 0.50: a BUY at or under its
// limit, a SELL at or over it), plus 1c for the first fill and half a cent for each further fill,
// since each fill rounds on its own and can gain up to half a cent (a fill is a whole contract or
// more). Ten one-contract fills at 50c and 0.06 are charged 20c against an exact 15c; this
// allows 20c. 0 when coeff is not positive.
Micros taker_fee_allowance(Side s, Px limit, Qty q, double coeff);

// ---- what a write's reply means ----------------------------------------------------------------
// {code: the gRPC status, message}: venue_body when both are there; code as its decimal text
kalshi::ApiError parse_error(int status, const std::string& body);
// For a write: CREATE (a 2xx with a create ack), CANCEL (a 2xx with a JSON object), CANCEL_ALL (a
// 2xx with canceledOrderIds), OTHER (any 2xx). A 2xx whose body fails its parser is UNKNOWN.
kalshi::WriteOutcome classify_write(WireOp op, const net::Exchange& x);
// `text` is the venue's code and message (any case). HTTP status first.
RejectClass classify_refusal(WireOp op, int status, const std::string& text);

// ---- the weekly maintenance window: Thursday 02:00-06:00 America/New_York -----------------------
struct Window {
  std::int64_t from_ms = 0, to_ms = 0;   // [from, to)
};
bool in_maintenance(std::int64_t now_ms);
Window maintenance_window(std::int64_t now_ms);   // the window now_ms is in, else the next one

}  // namespace deskd::poly
