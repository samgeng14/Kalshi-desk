#pragma once
// A fake Polymarket US for deskd's tests (plan section 8): the venue's REST API on one loopback
// port, over the fake Kalshi's matching engine (engine.h, in its Polymarket mode: FeeType::POLY's
// fee per fill and Stp::NONE), with its own JSON shapes and its own Ed25519 check. It shares no
// code with deskd (gw/poly_wire, gw/poly_sign): a fake that parsed with deskd's parsers would agree
// with deskd's mistakes.
//
//   http(s)://127.0.0.1:<port>/v1/...   (the venue: https://api.polymarket.us, signed, and
//                                        https://gateway.polymarket.us, public)
// The venue's two hosts are one port here, as in the old desk's mock (tests/test_poly.cpp
// MockPoly): a deskd pointed at the fake names the same base for both.
//
// Every shape is the old desk's reading of docs.polymarket.us (src/poly.cpp's parsers,
// tests/test_poly.cpp's mock replies, docs/polymarket.md), and none is verified against the venue:
// tools/poly_checks reads the real ones (decisions 3). The fake lists no field the old desk did not
// read, so a deskd parser cannot come to rely on one the venue may not send; the few the fake adds
// are listed below as its own.
//
// Read routes (public: the first three; the rest signed):
//   GET /v2/leagues/{league}/events?limit&offset   {events:[{slug, title, markets:[market]}],
//                                                  league:{slug}}; an unknown league lists none
//   GET /v1/markets?slug=a&slug=b                  {markets:[market]}, in the order asked; an
//                                                  unknown slug is left out. Without a slug: every
//                                                  market in listing order, ?limit&offset
//   GET /v1/markets/{slug}/book                    {marketData:{bids, asks}}, each [{px:{value,
//                                                  currency}, qty}], best first; 404 unknown
//   GET /v1/account/balances                       {balances:[{currentBalance, currency USD,
//                                                  buyingPower, assetNotional, assetAvailable}]}
//   GET /v1/portfolio/positions?limit&cursor       {positions:{<slug>:{netPositionDecimal, cost,
//                                                  realized, cashValue, qtyAvailableDecimal,
//                                                  expired, updateTime}}, nextCursor, eof}: open
//                                                  positions by slug
//   GET /v1/portfolio/activities?limit&cursor      {activities:[...], nextCursor, eof}, newest
//                                                  first: {type ACTIVITY_TYPE_TRADE, trade:{id,
//                                                  marketSlug, price, qtyDecimal, isAggressor,
//                                                  createTime}} for each fill of ours, and
//                                                  {type ACTIVITY_TYPE_ACCOUNT_DEPOSIT | _WITHDRAWAL,
//                                                  accountBalanceChange:{transactionId, amount,
//                                                  updateTime}} for each deposit()
//   GET /v1/orders/open                            {orders:[order]}, oldest first, the creates the
//                                                  engine has not taken yet among them
//   GET /v1/order/{id}                             {order}; 404 unknown
// Write routes (signed):
//   POST /v1/orders                                create: 200 {id, executions}; see Writes
//   POST /v1/order/{id}/cancel {marketSlug}        200 {}; 404 an order that is not open, or not
//                                                  in that market; 400 without marketSlug
//   POST /v1/orders/open/cancel {}                 cancel-all: 200 {canceledOrderIds:[id]}, every
//                                                  open order of ours, the ones not taken yet too
//   POST /v1/order/preview {request:<create>}      200 {order:{id "", commissionsBasisPoints}}: the
//                                                  create checked as POST /v1/orders checks it,
//                                                  nothing placed
// market: {slug, question, sportsMarketType, orderPriceMinTickSize, minimumTradeQty,
//   feeCoefficient, active, closed, bestBidQuote, bestAskQuote, gameStartTime, endDate, updatedAt,
//   marketSides:[{description, long, team:{abbreviation, name, safeName, alias, league}, price}]};
//   a quote with no level on its side is left out, as is a side's price.
// order: {id, marketSlug, type ORDER_TYPE_LIMIT, side ORDER_SIDE_BUY | _SELL, tif
//   TIME_IN_FORCE_GOOD_TILL_CANCEL | _GOOD_TILL_DATE | _IMMEDIATE_OR_CANCEL | _FILL_OR_KILL, state
//   ORDER_STATE_PENDING_NEW | _NEW | _PARTIALLY_FILLED | _FILLED | _CANCELED | _EXPIRED | _REJECTED,
//   price, quantity, cumQuantity, leavesQuantity, avgPx (once filled),
//   commissionNotionalTotalCollected, createTime, updateTime}.
// Prices and money are {value, currency USD} with the value a decimal string (four places, six
// when a money amount needs them); quantities are numbers, or decimal strings where the field's
// name says Decimal. Errors are {code, message}: the gRPC status code beside the HTTP one, as the
// old mock's 401 {"code":16,"message":"unauthenticated"} had it (400 3, 401 16, 404 5, 405 12,
// 429 8, 500 13, 503 14).
//
// The engine's terms. Polymarket US quotes the long side (YES): a BUY at p is the engine's bid at
// p, a SELL its ask. A SELL with nothing held opens a short, which the engine holds as NO at 1 - p,
// so it posts $1 - p a contract (docs/polymarket.md: "SELL YES; margin = $1 - price"). The
// engine's ticker is the slug, its event ticker the event slug. One account: the engine's
// subaccount 0 on shard 0. An order made over the wire has an id of the fake's own, which the
// engine's id stands behind once the engine has the order (engine_id()); one a test makes with
// with_engine() goes by the engine's id.
//
// Writes. A create's body is the old desk's (src/poly.cpp poly_order_body) without its synchronous
// fields, plus GTD: {marketSlug, type ORDER_TYPE_LIMIT, price:{value, currency USD}, quantity
// (whole contracts, at least minimumTradeQty: a number, or a decimal string), tif, outcomeSide
// OUTCOME_SIDE_YES, action ORDER_ACTION_BUY | _SELL, manualOrderIndicator
// MANUAL_ORDER_INDICATOR_MANUAL | _AUTOMATIC; participateDontInitiate (post-only, GTC or GTD
// only), synchronousExecution and maxBlockTime (read, never waited on) when wanted; expireTime
// with GTD and only with it: RFC 3339 in UTC, a whole second}. A field the fake does not know, a
// wrong type or enum, a price outside (0, 1) or off the market's grid, is a 400 and places
// nothing; an unlisted slug is a 404. Every order carries Stp::NONE: ours trade with ours.
//   Asynchronous (synchronousExecution false or left out, as deskd sends it): the venue answers
//   {id, executions: []} and the engine takes the order async_delay_ms later. Until then it is
//   ORDER_STATE_PENDING_NEW, listed as open and in no book, and its executions show only after: in
//   GET /v1/order/{id}, /v1/orders/open and the trade activities. What the engine refuses when it
//   takes the order (buying power, a market closed or paused, its expiry passed) ends it
//   ORDER_STATE_REJECTED. With async_delay_ms 0 the engine takes it before the reply is written.
//   Synchronous (synchronousExecution true): the engine takes it at once, a refusal is a 400, and
//   the reply lists the order's fills as the old desk read them: executions:[{tradeId (its trade
//   activity's id), lastShares, lastPx, commissionNotionalCollected, order:{state}}].
//   Post-only that would cross, by EngineConfig::post_only_cross: REFUSE_400 refuses it (a 400
//   when synchronous, ORDER_STATE_REJECTED when not); CANCEL takes it and cancels it at once,
//   ORDER_STATE_CANCELED.
//   GTD rests until expireTime, then ORDER_STATE_EXPIRED. An expireTime no more than
//   EngineConfig::min_expiry_lead_s ahead is a 400: the venue's minimum lead is not known
//   (tools/poly_checks records it; Q1 waits on it, decisions 3).
// The rate limit: requests_per_s over every route and both hosts (decisions 3: 20 a second until
// tools/poly_checks says otherwise), holding one second's worth; past it a 429 with Retry-After,
// and nothing processed. A maintenance window (set_maintenance; the venue's is Thursday 02:00-06:00
// ET) answers every request 503, before auth; the engine runs on, so resting orders stay and
// expire on time.
// Faults: fake_kalshi.h's HttpFault, matched by method, a prefix of the path ("/v1/orders",
// "/v1/order/") and the body's marketSlug where it names a ticker (poly_fault()). Each kind does
// what it does there, in the venue's shapes. FILL_BEFORE_REPLY and HIT_AFTER_REST have the engine
// take the create before the reply, whatever async_delay_ms says: its fills are in the account
// before the client hears of the order. FILL_DURING_CANCEL is a cancel's. private_order means
// nothing here: the fake has no socket.
//
// Auth (docs/polymarket.md "Wire"): X-PM-Access-Key, X-PM-Timestamp (ms) and X-PM-Signature on
// /v1/orders, /v1/order/*, /v1/portfolio and /v1/account. AuthMode::VERIFY checks the Ed25519
// signature (base64) of timestamp + METHOD + path, the path without its query, against the key's
// raw public half; a key id the venue does not know is refused first, "API key not found", before
// any signature is checked, and a signature that does not verify "invalid signature" (the two
// 401s scripts/check_polymarket.py tells apart). Every mode but OFF refuses a timestamp more than
// auth_max_skew_ms from the fake's clock: "Polymarket requires the timestamp within 30 seconds of
// ITS clock" (check_polymarket.py).
//
// The fake's own choices, unverified: positions' and activities' paging (limit 1-1000, default
// 100; cursor, nextCursor and eof); a trade's id, one for each fill of ours, so a self-trade's two
// fills differ; cashValue, a position marked at the held side's best bid (a short at the NO bid,
// 1 - the YES ask), with assetNotional and assetAvailable their sum; qtyAvailableDecimal, the
// whole position; the gRPC codes and messages; the 404s; and both hosts on one port. On the write
// side (tools/poly_checks reads the venue's): the async reply {id, executions:[]}; GTD's field
// expireTime and its format; refusing a field it does not know; what is refused at once and what
// is REJECTED later; a pending create listed as open; the cancel's {} and the cancel-all's ids; a
// cancel of an order no longer open answered 404; preview's reply; one rate pool for both hosts,
// the 429's body and Retry-After; and the 503 during maintenance.
//
// The wire log: "http <status> <METHOD> <path>", the path without its query, then " (signed)" when
// the request carried any X-PM-* header, valid or not (md-pm never signs a public read). Faults
// log as on the fake Kalshi: "http 429 POST /v1/orders (signed) (fault)", "http drop-before-reply
// ...", "http drop-after-accept 200 ...", "http cut 200 ...", "http 200 ... (chunked)", "http
// pull-touch <slug> <qty>".
//
// Threads: one per connection (two with TLS, listener.h), a timer. The engine sits behind one
// mutex. Nothing here talks to anything but 127.0.0.1.
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine.h"
#include "fake_kalshi.h"
#include "listener.h"
#include "net.h"

namespace fakex {

// The engine as Polymarket US runs it: no cancel-all echo, which is Kalshi's documented minute.
EngineConfig poly_engine_config();

// A fault for the fake Polymarket US, on its create unless told otherwise (HttpFault's own path is
// the fake Kalshi's).
HttpFault poly_fault(HttpFault::Kind kind, const std::string& method = "POST", const std::string& path = "/v1/orders");

// One team of a market's side, as /v2/leagues/{league}/events lists it.
struct PolyTeam {
  std::string abbreviation, name, safe_name, alias;
};
struct PolySide {
  std::string description;  // "Patriots"
  bool is_long = false;     // the market's long side is its YES
  PolyTeam team;
};

// A market as the fake lists it. The engine holds its book, orders and positions by slug.
struct PolyMarketSpec {
  std::string slug;                    // "aec-nfl-ne-sea-2026-09-09"
  std::string league;                  // "nfl": the league whose events list it
  std::string event_slug, event_title; // "nfl-ne-sea-2026-09-09", "New England vs. Seattle"
  std::string question;
  std::string sports_type = "football_team_full_game_winner";  // sportsMarketType
  Px tick = 50;                        // orderPriceMinTickSize: 0.005 on game markets
  Qty min_qty = kContract;             // minimumTradeQty: 1
  int coeff_micro = 60000;             // feeCoefficient in millionths (engine.h poly_coeff_micro)
  std::int64_t start_ms = 0;           // gameStartTime; 0: left out
  std::int64_t end_ms = 0;             // endDate; 0: left out
  std::vector<PolySide> sides;         // marketSides, in the order listed
};

struct FakePolyConfig {
  EngineConfig engine = poly_engine_config();
  int port = 0;                        // 0: any free port
  AuthMode auth = AuthMode::PRESENCE;
  std::string key_id;                  // VERIFY: the one key id the venue knows; "": any
  std::string public_key_b64;          // VERIFY: the key's raw Ed25519 public half (32 bytes), base64
  std::int64_t auth_max_skew_ms = 30000;  // 0: any timestamp (a test on a manual clock)
  int idle_close_ms = 60000;           // a kept-open connection idle this long is closed
  // TLS: both set, the listener speaks TLS with this chain (leaf first) and key, PEM files.
  std::string tls_cert_file, tls_key_file;
  bool tls_close_notify = true;
  int tls_handshake_ms = 5000;
  bool manual_clock = false;           // time moves only by advance_ms
  std::int64_t start_ms = 0;           // the manual clock's first reading; 0: the wall clock now
  int tick_ms = 5;                     // the venue's timers (expiry, close) on the wall clock
  // An asynchronous create waits this long for the engine (ORDER_STATE_PENDING_NEW meanwhile).
  int async_delay_ms = 0;
  // requests_per_s on every route, holding one second's worth; past it a 429 with Retry-After
  // retry_after_s (0: no header).
  bool rate_limits = true;
  double requests_per_s = 20;
  int retry_after_s = 1;
};

class FakePoly {
 public:
  explicit FakePoly(FakePolyConfig cfg = {});
  ~FakePoly();
  FakePoly(const FakePoly&) = delete;
  FakePoly& operator=(const FakePoly&) = delete;

  bool start(std::string* err);
  void stop();
  int port() const { return listener_.port(); }
  std::string base() const;  // http(s)://127.0.0.1:<port>: both hosts' base
  bool tls() const { return listener_.tls(); }

  // ---- time
  std::int64_t now_ms() const;
  void advance_ms(std::int64_t d);  // manual clock: time moves by d, expiries and closes run

  // ---- the venue, under its lock
  // Lists a market under its league's events (a listed slug's spec is replaced; its book, orders
  // and positions stay) and opens it in the engine on its tick grid with Polymarket's fees.
  void list_market(PolyMarketSpec spec);
  // f(engine, now): books, orders and cash, as on the fake Kalshi. Orders a test makes here
  // should carry Stp::NONE, as the venue's do. Return values, not references into the engine.
  template <class F>
  auto with_engine(F&& f) -> decltype(f(std::declval<Engine&>(), std::int64_t{})) {
    std::unique_lock<std::mutex> lk(mu_);
    const std::int64_t now = now_locked();
    if constexpr (std::is_void_v<decltype(f(engine_, now))>) {
      f(engine_, now);
      drain_locked();
    } else {
      auto r = f(engine_, now);
      drain_locked();
      return r;
    }
  }
  // Cash into the account (a deposit) or, negative, out of it (a withdrawal): an activity of its own.
  void deposit(Micros amount);
  // The engine's id for order `id`: "" while the engine has not taken it, or never did. An order
  // made with with_engine() is its own.
  std::string engine_id(const std::string& id) const;
  // The body of the create that made order `id`, as it arrived (null for one the wire never saw):
  // what deskd sent, manualOrderIndicator and participateDontInitiate among it.
  json sent(const std::string& id) const;

  // ---- faults and settings
  void add_fault(HttpFault f);
  void clear_faults();
  std::size_t faults_pending() const;
  void set_rate_limits(bool on, double requests_per_s = 20);
  void set_async_delay_ms(int ms);
  // Every request from from_ms until to_ms on the fake's clock is answered 503; (0, 0) ends it.
  void set_maintenance(std::int64_t from_ms, std::int64_t to_ms);
  // Every kept-open connection closed under its client: the next request on it gets nothing back.
  void drop_http_connections();

  // ---- what happened on the wire, in the order it was written: "http 200 GET /v1/markets",
  // "http 401 GET /v1/account/balances (signed)", "http idle-close", "tls handshake-failed"
  std::vector<std::string> wire_log() const;
  void clear_wire_log();
  std::uint64_t http_requests() const { return requests_.load(); }
  std::uint64_t connections_accepted() const { return listener_.accepted(); }
  std::uint64_t idle_closes() const { return idle_closes_.load(); }

 private:
  struct Reply {
    int status = 200;
    std::string body;
  };
  // One entry of /v1/portfolio/activities: its body, and its place in the account's history.
  struct Activity {
    std::uint64_t seq = 0;
    json body;
  };
  // An order made by POST /v1/orders, by the fake's id. The engine has it from TAKEN on.
  struct Submitted {
    enum class State { PENDING, TAKEN, REJECTED, CANCELED };  // CANCELED: before it was taken
    std::string id, engine_id;
    NewOrder order;
    json sent;
    bool sync = false;
    State state = State::PENDING;
    std::int64_t created_ms = 0, due_ms = 0, updated_ms = 0;
  };

  std::int64_t now_locked() const;
  void tick_loop();
  void serve(int fd);
  void log_wire(const std::string& line);
  // What every engine call ends with: its socket messages dropped (the fake has no socket yet)
  // and its new fills taken into the activity history, so a deposit after them lists after them.
  void drain_locked();
  // A position marked at the held side's best bid (cashValue): a short at the NO bid.
  Micros held_value_locked(const std::string& slug, Qty pos) const;

  std::string auth_refusal(const net::Request& req, std::int64_t now) const;
  bool take_fault_locked(const net::Request& req, const json& body, HttpFault* f);
  bool rate_ok_locked(std::int64_t now);
  bool in_maintenance_locked(std::int64_t now) const;
  void pull_touch_locked(const json& create, Qty qty, std::int64_t now);   // PULL_TOUCH
  // Every accepted create whose time has come goes to the engine, in the order accepted.
  void take_due_locked(std::int64_t now);
  // One accepted create goes to the engine now: it trades, rests, or is refused (REJECTED).
  CreateResult take_locked(Submitted& s, std::int64_t now);
  // `created` gets the id of an order this request made (the faults that act on it).
  Reply route_locked(const net::Request& req, const std::string& path, const json& body, std::int64_t now,
                     std::string* created);
  Reply league_events_locked(const net::Request& req, const std::string& league, std::int64_t now);
  Reply markets_locked(const net::Request& req, std::int64_t now);
  Reply book_locked(const std::string& slug);
  Reply balances_locked();
  Reply positions_locked(const net::Request& req);
  Reply activities_locked(const net::Request& req);
  Reply open_orders_locked();
  Reply order_locked(const std::string& id);
  // A create's body read: "" and *out, or why not (*status 400, or 404 for a slug not listed).
  std::string read_create_locked(const json& body, std::int64_t now, NewOrder* out, bool* sync, int* status) const;
  Reply create_locked(const json& body, std::int64_t now, std::string* created);
  Reply preview_locked(const json& body, std::int64_t now);
  Reply cancel_locked(const std::string& id, const json& body, std::int64_t now);
  Reply cancel_all_locked(const json& body, std::int64_t now);
  json market_json(const PolyMarketSpec& s, std::int64_t now) const;
  json order_json(const Order& o) const;
  json submitted_json(const Submitted& s) const;   // one the engine does not have
  // An order by the fake's id: the engine's, one it has not got, or null when there is none.
  json order_view_locked(const std::string& id) const;
  std::vector<std::string> open_ids_locked() const;  // our open orders, oldest first
  std::string engine_id_locked(const std::string& id) const;

  FakePolyConfig cfg_;
  mutable std::mutex mu_;
  Engine engine_;
  std::map<std::string, PolyMarketSpec> listing_;
  std::vector<std::string> listed_;    // slugs in the order first listed
  std::vector<Activity> activities_;   // oldest first
  std::size_t fills_seen_ = 0;         // engine fills already in activities_
  std::uint64_t next_activity_ = 1;
  std::mt19937_64 rng_;               // deposit and order ids
  std::map<std::string, Submitted> submitted_;   // by our id
  std::map<std::string, std::string> by_engine_; // the engine's id -> ours
  std::deque<std::string> pending_;    // accepted creates the engine has not taken, in that order
  std::deque<HttpFault> faults_;
  double tokens_ = 0;                  // the rate limit's bucket, at tokens_ms_
  std::int64_t tokens_ms_ = 0;
  std::int64_t maintenance_from_ = 0, maintenance_to_ = 0;
  std::int64_t manual_now_ = 0;
  mutable std::int64_t clock_ms_ = 0;  // never goes back (fake_kalshi.h)
  void* pubkey_ = nullptr;             // EVP_PKEY*, VERIFY

  Listener listener_;
  std::atomic<bool> running_{false}, stopping_{false};
  std::thread ticker_;

  mutable std::mutex log_mu_;
  std::deque<std::string> wire_;
  std::atomic<std::uint64_t> requests_{0}, idle_closes_{0};
};

}  // namespace fakex
