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
//   GET /v1/orders/open                            {orders:[order]}, oldest first
//   GET /v1/order/{id}                             {order}; 404 unknown
// market: {slug, question, sportsMarketType, orderPriceMinTickSize, minimumTradeQty,
//   feeCoefficient, active, closed, bestBidQuote, bestAskQuote, gameStartTime, endDate, updatedAt,
//   marketSides:[{description, long, team:{abbreviation, name, safeName, alias, league}, price}]};
//   a quote with no level on its side is left out, as is a side's price.
// order: {id, marketSlug, type ORDER_TYPE_LIMIT, side ORDER_SIDE_BUY | _SELL, tif
//   TIME_IN_FORCE_GOOD_TILL_CANCEL | _GOOD_TILL_DATE | _IMMEDIATE_OR_CANCEL | _FILL_OR_KILL, state
//   ORDER_STATE_NEW | _PARTIALLY_FILLED | _FILLED | _CANCELED | _EXPIRED, price, quantity,
//   cumQuantity, leavesQuantity, avgPx (once filled), commissionNotionalTotalCollected, createTime,
//   updateTime}.
// Prices and money are {value, currency USD} with the value a decimal string (four places, six
// when a money amount needs them); quantities are numbers, or decimal strings where the field's
// name says Decimal. Errors are {code, message}: the gRPC status code beside the HTTP one, as the
// old mock's 401 {"code":16,"message":"unauthenticated"} had it (400 3, 401 16, 404 5, 405 12).
//
// The engine's terms. Polymarket US quotes the long side (YES): a BUY at p is the engine's bid at
// p, a SELL its ask. A SELL with nothing held opens a short, which the engine holds as NO at 1 - p,
// so it posts $1 - p a contract (docs/polymarket.md: "SELL YES; margin = $1 - price"). The
// engine's ticker is the slug, its event ticker the event slug. One account: the engine's
// subaccount 0 on shard 0. The write routes (create, cancel, cancel-all) and their async fills are
// not served yet: a test makes its orders with with_engine().
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
// whole position; the gRPC codes and messages; the 404s; and both hosts on one port.
//
// The wire log: "http <status> <METHOD> <path>", the path without its query, then " (signed)" when
// the request carried any X-PM-* header, valid or not (md-pm never signs a public read).
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
  Reply route_locked(const net::Request& req, const std::string& path, std::int64_t now);
  Reply league_events_locked(const net::Request& req, const std::string& league, std::int64_t now);
  Reply markets_locked(const net::Request& req, std::int64_t now);
  Reply book_locked(const std::string& slug);
  Reply balances_locked();
  Reply positions_locked(const net::Request& req);
  Reply activities_locked(const net::Request& req);
  Reply open_orders_locked();
  Reply order_locked(const std::string& id);
  json market_json(const PolyMarketSpec& s, std::int64_t now) const;
  json order_json(const Order& o) const;

  FakePolyConfig cfg_;
  mutable std::mutex mu_;
  Engine engine_;
  std::map<std::string, PolyMarketSpec> listing_;
  std::vector<std::string> listed_;    // slugs in the order first listed
  std::vector<Activity> activities_;   // oldest first
  std::size_t fills_seen_ = 0;         // engine fills already in activities_
  std::uint64_t next_activity_ = 1;
  std::mt19937_64 rng_;               // deposit ids
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
