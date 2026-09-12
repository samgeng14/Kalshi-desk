// Polymarket US: the Ed25519 wire, the parsers against the documented shapes, the matcher between
// Kalshi game markets and Polymarket sides, the lock arithmetic, the rails and orders, and the
// pair rule in paper and live mode through mock exchanges on both sides.
// Built and run with `ninja -C build-ci && ./build-ci/poly-tests`.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <httplib.h>

#include "desk/auth.h"
#include "desk/client.h"
#include "desk/executor.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/poly.h"
#include "desk/risk.h"
#include "desk/store.h"
#include "desk/timeutil.h"

using namespace desk;

static int fails = 0, checks = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++checks;                                                                       \
    if (!(cond)) {                                                                  \
      ++fails;                                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                               \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))
#define CHECK_THROWS(expr, needle)                                                   \
  do {                                                                              \
    ++checks;                                                                       \
    bool threw = false;                                                             \
    std::string what;                                                               \
    try { expr; } catch (const std::exception& e) { threw = true; what = e.what(); } \
    if (!threw || what.find(needle) == std::string::npos) {                         \
      ++fails;                                                                      \
      std::fprintf(stderr, "FAIL %s:%d: expected throw containing '%s', got '%s'\n", __FILE__, __LINE__, needle, what.c_str()); \
    }                                                                               \
  } while (0)

static std::string random_seed_b64() {
  std::mt19937_64 rng{std::random_device{}()};
  unsigned char seed[32];
  for (auto& b : seed) b = static_cast<unsigned char>(rng() & 0xff);
  return Signer::base64(seed, 32);
}

static json team(const std::string& abbr, const std::string& name, const std::string& safe, const std::string& alias) {
  return json{{"abbreviation", abbr}, {"name", name}, {"safeName", safe}, {"alias", alias}, {"league", "nfl"}};
}
// A Polymarket game-winner market as /v2/leagues/{slug}/events lists it.
static json pm_winner(const std::string& slug, const std::string& type, double bid, double ask, const json& long_team, const json& short_team,
                      const std::string& long_desc, const std::string& short_desc, const std::string& start) {
  json sides = json::array();
  sides.push_back({{"description", long_desc}, {"long", true}, {"team", long_team}, {"price", fmt_dollars(ask)}});
  sides.push_back({{"description", short_desc}, {"long", false}, {"team", short_team}, {"price", fmt_dollars(1 - bid)}});
  return json{{"slug", slug}, {"question", "Who will win " + long_desc + " vs " + short_desc + "?"}, {"sportsMarketType", type},
              {"orderPriceMinTickSize", 0.005}, {"minimumTradeQty", 1}, {"feeCoefficient", 0.06}, {"active", true}, {"closed", false},
              {"bestBidQuote", {{"value", fmt_dollars(bid)}, {"currency", "USD"}}}, {"bestAskQuote", {{"value", fmt_dollars(ask)}, {"currency", "USD"}}},
              {"gameStartTime", start}, {"endDate", "2026-09-24T00:20:00Z"}, {"updatedAt", now_iso()}, {"marketSides", sides}};
}

static void test_auth() {
  std::string seed = random_seed_b64();
  PolyAuth a("a422f5a3-0000-4000-8000-000000000000", seed);
  std::string pub = a.public_key_b64();
  CHECK(pub.size() == 44);
  std::string sig = a.sign("1788900000000GET/v1/portfolio/positions");
  CHECK(Signer::unbase64(sig).size() == 64);
  CHECK(PolyAuth::verify(pub, "1788900000000GET/v1/portfolio/positions", sig));
  CHECK(!PolyAuth::verify(pub, "1788900000001GET/v1/portfolio/positions", sig));
  auto h = a.headers("POST", "/v1/orders", 1788900000123LL);
  CHECK(h["X-PM-Access-Key"] == "a422f5a3-0000-4000-8000-000000000000" && h["X-PM-Timestamp"] == "1788900000123");
  CHECK(PolyAuth::verify(pub, "1788900000123POST/v1/orders", h["X-PM-Signature"]));
  // A 64-byte secret (seed + public half) works the same: the first 32 bytes are the seed.
  std::string raw = Signer::unbase64(seed) + Signer::unbase64(pub);
  PolyAuth b("k", Signer::base64(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
  CHECK(b.public_key_b64() == pub);
  CHECK_THROWS(PolyAuth("k", "not-a-key"), "32 or 64 bytes");
}

static void test_parsers_and_math() {
  json mj = pm_winner("aec-nfl-ne-sea-2026-09-09", "football_team_full_game_winner", 0.38, 0.385, team("ne", "New England Patriots", "New England", "Patriots"),
                      team("sea", "Seattle Seahawks", "Seattle", "Seahawks"), "Patriots", "Seahawks", "2026-09-10T00:20:00Z");
  PolyMarket m = parse_poly_market(mj, "nfl", "nfl-ne-sea-2026-09-09", "New England vs. Seattle");
  CHECK(m.slug == "aec-nfl-ne-sea-2026-09-09" && m.league == "nfl" && m.sides.size() == 2 && m.sides[0].is_long && m.sides[0].abbr == "ne");
  CHECK_NEAR(m.bid, 0.38, 1e-9);
  CHECK_NEAR(m.ask, 0.385, 1e-9);
  CHECK_NEAR(m.tick, 0.005, 1e-12);
  CHECK(m.start_ts == parse_iso("2026-09-10T00:20:00Z") && m.two_sided());
  PolyBalance b = parse_poly_balance(json{{"balances", json::array({{{"currentBalance", 100.5}, {"currency", "USD"}, {"buyingPower", 80.25},
                                                                       {"assetNotional", 20.0}, {"assetAvailable", 20.0}}})}});
  CHECK_NEAR(b.current_balance, 100.5, 1e-9);
  CHECK_NEAR(b.buying_power, 80.25, 1e-9);
  PolyPosition p = parse_poly_position("aec-nfl-ne-sea-2026-09-09", json{{"netPositionDecimal", "-5"}, {"cost", {{"value", "3.10"}, {"currency", "USD"}}},
                                                                          {"realized", {{"value", "0"}, {"currency", "USD"}}},
                                                                          {"cashValue", {{"value", "0.15"}, {"currency", "USD"}}}, {"qtyAvailableDecimal", "5"}});
  CHECK(p.net == -5 && std::fabs(p.cost - 3.10) < 1e-9 && std::fabs(p.unrealized - 0.15) < 1e-9);
  PolyOrder o = parse_poly_order(json{{"id", "o-1"}, {"marketSlug", "s"}, {"side", "ORDER_SIDE_SELL"}, {"state", "ORDER_STATE_PARTIALLY_FILLED"},
                                      {"price", {{"value", "0.385"}, {"currency", "USD"}}}, {"quantity", 5.0}, {"cumQuantity", 2.0}, {"leavesQuantity", 3.0}});
  CHECK(o.side == "SELL" && o.state == "partially_filled" && o.leaves == 3 && std::fabs(o.price - 0.385) < 1e-9);
  PolyTrade t = parse_poly_trade(json{{"id", "t-1"}, {"marketSlug", "s"}, {"price", {{"value", "0.38"}, {"currency", "USD"}}}, {"qtyDecimal", "5"},
                                      {"isAggressor", true}, {"createTime", "2026-09-08T20:00:00Z"}});
  CHECK(t.id == "t-1" && t.qty == 5 && t.aggressor && t.ts == parse_iso("2026-09-08T20:00:00Z"));
  PolyOrderRequest r;
  r.slug = "aec-nfl-ne-sea-2026-09-09"; r.action = "SELL"; r.price = 0.385; r.quantity = 5; r.tif = "IOC"; r.manual = false;
  json body = poly_order_body(r);
  CHECK(body["marketSlug"] == r.slug && body["outcomeSide"] == "OUTCOME_SIDE_YES" && body["action"] == "ORDER_ACTION_SELL");
  CHECK(body["price"]["value"] == "0.385" && body["quantity"] == 5.0 && body["tif"] == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL");
  CHECK(body["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_AUTOMATIC" && !body.contains("participateDontInitiate"));
  r.post_only = true; r.tif = "GTC";
  body = poly_order_body(r);
  CHECK(body["participateDontInitiate"] == true && body["tif"] == "TIME_IN_FORCE_GOOD_TILL_CANCEL");
  // Fees: the documented schedule. Taker at 0.50 pays 1.5c; a maker receives 0.3125c.
  CHECK_NEAR(poly_fee(0.5, true), 0.015, 1e-9);
  CHECK_NEAR(poly_fee(0.5, false), -0.003125, 1e-9);
  CHECK(poly_league_for_series("KXNFLGAME") == "nfl" && poly_league_for_series("KXEPLGAME") == "epl" && poly_league_for_series("KXHIGHNY").empty());
  std::string date, codes, dummy;
  CHECK(parse_kalshi_game_event("KXNFLGAME-26SEP09NESEA", &date, &codes, &dummy) && date == "2026-09-09" && codes == "NESEA");
  CHECK(parse_kalshi_game_event("KXMLBGAME-26SEP112010CLEMIN", &date, &codes, &dummy) && date == "2026-09-11" && codes == "CLEMIN");
  std::string hhmm;
  CHECK(parse_kalshi_game_event("KXMLBGAME-26SEP112010CLEMIN", &date, &codes, &dummy, &hhmm) && hhmm == "2010");
  CHECK(parse_kalshi_game_event("KXNFLGAME-26SEP09NESEA", &date, &codes, &dummy, &hhmm) && hhmm.empty());
  CHECK(!parse_kalshi_game_event("KXHIGHNY-26SEP08", &date, &codes, &dummy) || codes.empty() || date == "2026-09-08");
  PolySide s;
  s.abbr = "ne"; s.name = "New England Patriots"; s.safe_name = "New England"; s.alias = "Patriots";
  CHECK(poly_team_score("NE", "New England", s) == 3);
  CHECK(poly_team_score("NWE", "New England", s) == 2);
  CHECK(poly_team_score("XX", "Patriots", s) == 2);
  CHECK(poly_team_score("XX", "England", s) == 1);
  CHECK(poly_team_score("XX", "Dallas", s) == 0);
  PolySide sj; sj.abbr = "sjsu"; sj.name = "San Jose State Spartans"; sj.safe_name = "San Jose State"; sj.alias = "Spartans";
  CHECK(poly_team_score("SJSU", "San Jose St.", sj) == 3);
  CHECK(poly_team_score("SJS", "San Jose St.", sj) == 2);   // "St." reads as State
}

// ---- mocks -------------------------------------------------------------------------------------------------

// Polymarket US, both hosts on one server: the gateway paths are public, /v1/orders, /v1/portfolio,
// /v1/account and /v1/order/* verify the Ed25519 signature against the desk's public key.
struct MockPoly {
  httplib::Server server;
  std::thread thread;
  int port = 0;
  std::string public_key;
  std::mutex mu;
  std::atomic<int> orders{0}, cancels{0}, cancel_alls{0}, bad_auth{0};
  json last_order, last_cancel;
  json events = json::array();                 // /v2/leagues/{events_league}/events
  std::string events_league = "nfl";
  std::map<std::string, json> quotes;          // slug -> market row for /v1/markets?slug=
  json positions = json::object(), open_orders = json::array(), activities = json::array();
  std::map<std::string, json> order_details;
  double fill_on_cancel = 0;
  bool fail_order_lookup = false;
  bool reject_cancel = false, uncertain_submit = false;
  double balance = 100.0, buying_power = 100.0, asset_notional = 0.0;
  double fill_fraction = 1.0;
  bool authed(const httplib::Request& q) {
    std::string key = q.get_header_value("X-PM-Access-Key"), ts = q.get_header_value("X-PM-Timestamp"), sig = q.get_header_value("X-PM-Signature");
    bool ok = !key.empty() && PolyAuth::verify(public_key, ts + q.method + q.path, sig);
    if (!ok) ++bad_auth;
    return ok;
  }
  MockPoly() {
    server.Get(R"(/v2/leagues/([^/]+)/events)", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      // NFL events by default; a test may set `events_league` to serve them under another league.
      json evs = q.matches[1].str() == events_league ? events : json::array();
      r.set_content(json{{"events", evs}, {"league", {{"slug", q.matches[1].str()}}}}.dump(), "application/json");
    });
    server.Get("/v1/markets", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      json ms = json::array();
      auto range = q.params.equal_range("slug");
      for (auto it = range.first; it != range.second; ++it)
        if (quotes.count(it->second)) ms.push_back(quotes[it->second]);
      r.set_content(json{{"markets", ms}}.dump(), "application/json");
    });
    server.Get(R"(/v1/markets/([^/]+)/book)", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"marketData":{"bids":[{"px":{"value":"0.3800","currency":"USD"},"qty":"1000.00"}],"asks":[{"px":{"value":"0.3850","currency":"USD"},"qty":"900.00"}]}})", "application/json");
    });
    server.Get("/v1/account/balances", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; r.set_content(R"({"code":16,"message":"unauthenticated"})", "application/json"); return; }
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"balances", json::array({{{"currentBalance", balance}, {"currency", "USD"}, {"buyingPower", buying_power},
                                                     {"assetNotional", asset_notional}, {"assetAvailable", asset_notional}}})}}.dump(), "application/json");
    });
    server.Get("/v1/portfolio/positions", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"positions", positions}}.dump(), "application/json");
    });
    server.Get("/v1/orders/open", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"orders", open_orders}}.dump(), "application/json");
    });
    server.Get(R"(/v1/order/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      if (fail_order_lookup || !order_details.count(q.matches[1].str())) { r.status = 503; r.set_content("unavailable", "text/plain"); return; }
      r.set_content(json{{"order", order_details.at(q.matches[1].str())}}.dump(), "application/json");
    });
    server.Get("/v1/portfolio/activities", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"activities", activities}}.dump(), "application/json");
    });
    server.Post("/v1/orders", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      ++orders;
      last_order = json::parse(q.body);
      if (uncertain_submit) { r.status = 503; r.set_content("submission outcome unavailable", "text/plain"); return; }
      double qty = last_order.value("quantity", 0.0), filled = std::floor(qty * fill_fraction + 1e-9);
      json execs = json::array();
      if (filled > 0)
        execs.push_back({{"id", "x-1"}, {"tradeId", "t-" + std::to_string(orders.load())}, {"lastShares", fmt_fp(filled)},
                         {"lastPx", last_order["price"]}, {"commissionNotionalCollected", {{"value", "0.0100"}, {"currency", "USD"}}},
                         {"order", {{"state", filled >= qty ? "ORDER_STATE_FILLED" : "ORDER_STATE_PARTIALLY_FILLED"}}}});
      const std::string id = "po-" + std::to_string(orders.load());
      bool gtc = last_order.value("tif", "") == "TIME_IN_FORCE_GOOD_TILL_CANCEL";
      order_details[id] = {{"id", id}, {"marketSlug", last_order["marketSlug"]},
                          {"side", last_order.value("action", "") == "ORDER_ACTION_SELL" ? "ORDER_SIDE_SELL" : "ORDER_SIDE_BUY"},
                          {"quantity", qty}, {"cumQuantity", filled}, {"leavesQuantity", gtc ? qty - filled : 0.0},
                          {"price", last_order["price"]}, {"avgPx", filled > 0 ? last_order["price"] : json{{"value", "0"}}},
                          {"commissionNotionalTotalCollected", {{"value", filled > 0 ? "0.0100" : "0.0000"}}},
                          {"state", filled >= qty ? "ORDER_STATE_FILLED" : gtc ? "ORDER_STATE_NEW" : "ORDER_STATE_CANCELED"}};
      r.set_content(json{{"id", "po-" + std::to_string(orders.load())}, {"executions", execs}}.dump(), "application/json");
    });
    server.Post(R"(/v1/order/([^/]+)/cancel)", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      std::lock_guard<std::mutex> lk(mu);
      ++cancels;
      if (reject_cancel) { r.status = 503; r.set_content("cancel unavailable", "text/plain"); return; }
      last_cancel = json{{"id", q.matches[1].str()}, {"body", json::parse(q.body)}};
      if (order_details.count(q.matches[1].str())) {
        auto& o = order_details[q.matches[1].str()];
        double cumulative = std::min(o.value("quantity", 0.0), o.value("cumQuantity", 0.0) + fill_on_cancel);
        o["cumQuantity"] = cumulative; o["leavesQuantity"] = 0.0; o["state"] = "ORDER_STATE_CANCELED";
        if (cumulative > 0) { o["avgPx"] = o["price"]; o["commissionNotionalTotalCollected"] = {{"value", "0.0100"}}; }
      }
      r.set_content("{}", "application/json");
    });
    server.Post("/v1/orders/open/cancel", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      ++cancel_alls;
      r.set_content(R"({"canceledOrderIds":[]})", "application/json");
    });
    server.Post("/v1/order/preview", [&](const httplib::Request& q, httplib::Response& r) {
      if (!authed(q)) { r.status = 401; return; }
      r.set_content(R"({"order":{"id":"","commissionsBasisPoints":"600"}})", "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([&] { server.listen_after_bind(); });
  }
  ~MockPoly() { server.stop(); thread.join(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
  void quote(const std::string& slug, double bid, double ask) {
    std::lock_guard<std::mutex> lk(mu);
    json& m = quotes[slug];
    if (m.is_null()) m = json{{"slug", slug}, {"active", true}, {"closed", false}};
    m["bestBidQuote"] = {{"value", fmt_dollars(bid)}, {"currency", "USD"}};
    m["bestAskQuote"] = {{"value", fmt_dollars(ask)}, {"currency", "USD"}};
    m["updatedAt"] = now_iso();
  }
};

// Kalshi, the minimum the executor needs for a maker/taker order and a reconcile.
struct MockKalshi {
  httplib::Server server;
  std::thread thread;
  int port = 0;
  std::atomic<int> writes{0}, cancels{0};
  json last_order, market_positions = json::array(), resting_orders = json::array();
  std::map<std::string, json> markets;   // ticker -> {event_ticker, yes_bid, yes_ask}
  double fill_all = 0.0;
  std::mutex mu;
  MockKalshi() {
    server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      std::string tk = q.matches[1].str();
      json ov = markets.count(tk) ? markets[tk] : json::object();
      json m = {{"ticker", tk}, {"event_ticker", ov.value("event_ticker", "KXNFLGAME-26SEP09NESEA")}, {"status", "active"},
                {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", iso_of(now_s() + 5 * 3600)},
                {"yes_bid_dollars", fmt_dollars(ov.value("yes_bid", 0.38))}, {"yes_ask_dollars", fmt_dollars(ov.value("yes_ask", 0.40))},
                {"yes_bid_size_fp", "20.00"}, {"yes_ask_size_fp", "30.00"}, {"result", ""},
                {"price_level_structure", "linear"}, {"price_ranges", json::array({{{"start", "0.01"}, {"end", "0.99"}, {"step", "0.01"}}})}};
      r.set_content(json{{"market", m}}.dump(), "application/json");
    });
    server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      r.set_content(json{{"series", {{"ticker", q.matches[1].str()}, {"category", "Sports"}, {"fee_type", "quadratic_with_maker_fees"}, {"fee_multiplier", 1}}}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}, {"updated_ts", now_s() - 3600}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"market_positions", market_positions}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"orders", resting_orders}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
    });
    server.Post("/trade-api/v2/portfolio/events/orders", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++writes;
      last_order = json::parse(q.body);
      double count = parse_num(last_order.value("count", "0")), filled = std::floor(count * fill_all * 100) / 100;
      r.status = 201;
      r.set_content(json{{"order_id", "kord-" + std::to_string(writes.load())}, {"fill_count", fmt_fp(filled)}, {"remaining_count", fmt_fp(count - filled)}}.dump(), "application/json");
    });
    server.Delete(R"(/trade-api/v2/portfolio/events/orders/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++cancels;
      // This fixture cancels its latest unfilled order. Zero would report every contract
      // filling concurrently with the cancel, not an unfilled cancellation.
      r.set_content(json{{"order_id", q.matches[1].str()}, {"reduced_by", last_order.value("count","0.00")}}.dump(), "application/json");
    });
    server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})", "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([&] { server.listen_after_bind(); });
  }
  ~MockKalshi() { server.stop(); thread.join(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2"; }
  void hold(const std::string& ticker, double position, double exposure) {
    std::lock_guard<std::mutex> lk(mu);
    market_positions = json::array();
    if (position != 0) market_positions.push_back({{"ticker", ticker}, {"position_fp", fmt_fp(position)}, {"market_exposure_dollars", fmt_dollars(exposure)}});
  }
};

static std::shared_ptr<Signer> test_signer(const std::string& pem) {
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str()) != 0) return nullptr;
  return std::make_shared<Signer>("test-key", pem);
}

static Config poly_cfg(const json& extra = json::object()) {
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  json p = extra;
  p["enabled"] = true;
  if (!p.contains("refresh_seconds")) p["refresh_seconds"] = 0;
  if (!p.contains("universe_refresh_seconds")) p["universe_refresh_seconds"] = 0;
  if (!p.contains("account_refresh_seconds")) p["account_refresh_seconds"] = 0;
  cfg.strategy_params = json{{"polymarket", p}};
  return cfg;
}

static void seed_kalshi(Store& st) {
  auto mk = [&](const std::string& t, const std::string& ev, const std::string& series, const std::string& sub, double bid, double ask) {
    Market m; m.ticker = t; m.event_ticker = ev; m.series_ticker = series; m.title = sub + " wins"; m.yes_sub_title = sub; m.status = "active";
    m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now_s() + 30 * 3600); m.yes_bid = bid; m.yes_ask = ask; m.tick = 0.01;
    m.yes_bid_size = 20; m.yes_ask_size = 30; m.quote_ts = now_s();
    st.upsert_market(m);
  };
  Series s; s.ticker = "KXNFLGAME"; s.category = "Sports"; s.fee_type = "quadratic_with_maker_fees"; s.fee_multiplier = 1;
  st.upsert_series(s);
  s.ticker = "KXEPLGAME"; st.upsert_series(s);
  mk("KXNFLGAME-26SEP09NESEA-NE", "KXNFLGAME-26SEP09NESEA", "KXNFLGAME", "New England", 0.36, 0.38);
  mk("KXNFLGAME-26SEP09NESEA-SEA", "KXNFLGAME-26SEP09NESEA", "KXNFLGAME", "Seattle", 0.62, 0.64);
  mk("KXNFLGAME-26SEP13DALNYG-DAL", "KXNFLGAME-26SEP13DALNYG", "KXNFLGAME", "Dallas", 0.60, 0.61);
  mk("KXNFLGAME-26SEP13DALNYG-NYG", "KXNFLGAME-26SEP13DALNYG", "KXNFLGAME", "New York G", 0.39, 0.40);
  // A game Polymarket does not list, and an EPL game with a draw market.
  mk("KXNFLGAME-26SEP20WASDAL-WAS", "KXNFLGAME-26SEP20WASDAL", "KXNFLGAME", "Washington", 0.34, 0.35);
  mk("KXNFLGAME-26SEP20WASDAL-DAL", "KXNFLGAME-26SEP20WASDAL", "KXNFLGAME", "Dallas", 0.65, 0.67);
  mk("KXEPLGAME-26SEP12BORBRE-BOR", "KXEPLGAME-26SEP12BORBRE", "KXEPLGAME", "Bournemouth", 0.37, 0.39);
  mk("KXEPLGAME-26SEP12BORBRE-BRE", "KXEPLGAME-26SEP12BORBRE", "KXEPLGAME", "Brentford", 0.33, 0.35);
  mk("KXEPLGAME-26SEP12BORBRE-TIE", "KXEPLGAME-26SEP12BORBRE", "KXEPLGAME", "Tie", 0.26, 0.28);
}

static void seed_poly(MockPoly& x) {
  std::lock_guard<std::mutex> lk(x.mu);
  json m1 = pm_winner("aec-nfl-ne-sea-2026-09-09", "football_team_full_game_winner", 0.38, 0.385, team("ne", "New England Patriots", "New England", "Patriots"),
                      team("sea", "Seattle Seahawks", "Seattle", "Seahawks"), "Patriots", "Seahawks", "2026-09-10T00:20:00Z");
  json m2 = pm_winner("aec-nfl-dal-nyg-2026-09-13", "football_team_full_game_winner", 0.60, 0.605, team("dal", "Dallas Cowboys", "Dallas", "Cowboys"),
                      team("nyg", "New York Giants", "New York", "Giants"), "Cowboys", "Giants", "2026-09-13T20:25:00Z");
  x.events = json::array({{{"slug", "nfl-ne-sea-2026-09-09"}, {"title", "New England vs. Seattle"}, {"markets", json::array({m1})}},
                          {{"slug", "nfl-dal-nyg-2026-09-13"}, {"title", "Dallas vs. New York G"}, {"markets", json::array({m2})}}});
  x.quotes["aec-nfl-ne-sea-2026-09-09"] = m1;
  x.quotes["aec-nfl-dal-nyg-2026-09-13"] = m2;
}

static void test_matching_quotes_and_locks() {
  MockPoly x;
  seed_poly(x);
  Store st(":memory:");
  seed_kalshi(st);
  Config cfg = poly_cfg();
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), nullptr, "tests");
  PolyDesk desk(cfg, st, client, nullptr);
  CHECK(desk.enabled() && !desk.armed());
  long long now = now_s();
  desk.refresh_universe(now);
  desk.match(now);
  auto pairs = desk.pairs_by_ticker();
  CHECK(pairs.size() == 4);
  CHECK(pairs.count("KXNFLGAME-26SEP09NESEA-NE") && pairs["KXNFLGAME-26SEP09NESEA-NE"].slug == "aec-nfl-ne-sea-2026-09-09" && pairs["KXNFLGAME-26SEP09NESEA-NE"].pm_yes_is_kalshi_yes);
  CHECK(pairs.count("KXNFLGAME-26SEP09NESEA-SEA") && !pairs["KXNFLGAME-26SEP09NESEA-SEA"].pm_yes_is_kalshi_yes && pairs["KXNFLGAME-26SEP09NESEA-SEA"].matched_by == "code");
  CHECK(pairs.count("KXNFLGAME-26SEP13DALNYG-NYG") && pairs["KXNFLGAME-26SEP13DALNYG-NYG"].slug == "aec-nfl-dal-nyg-2026-09-13");
  CHECK(!pairs.count("KXNFLGAME-26SEP20WASDAL-WAS") && !pairs.count("KXEPLGAME-26SEP12BORBRE-TIE") && !pairs.count("KXEPLGAME-26SEP12BORBRE-BOR"));
  CHECK(st.query("SELECT * FROM poly_pairs").size() == 4 && st.query("SELECT * FROM poly_markets").size() == 2);
  // Quotes: the batch fetch by slug, flipped for the NO side.
  x.quote("aec-nfl-ne-sea-2026-09-09", 0.40, 0.405);
  desk.refresh_quotes(now);
  auto q = desk.quote_for("KXNFLGAME-26SEP09NESEA-NE", now);
  CHECK(q && std::fabs(q->bid - 0.40) < 1e-9 && std::fabs(q->ask - 0.405) < 1e-9 && !q->flipped);
  auto q2 = desk.quote_for("KXNFLGAME-26SEP09NESEA-SEA", now);
  CHECK(q2 && q2->flipped && std::fabs(q2->bid - 0.595) < 1e-9 && std::fabs(q2->ask - 0.60) < 1e-9);
  CHECK(!desk.quote_for("KXNFLGAME-26SEP20WASDAL-WAS", now));
  // Lock arithmetic for NE: Kalshi 0.36/0.38, Polymarket 0.40/0.405 (in NE-YES terms).
  json rows = desk.pairs(now);
  CHECK(rows.size() == 4);
  json ne = nullptr;
  for (const auto& r : rows) if (r["kalshi_ticker"] == "KXNFLGAME-26SEP09NESEA-NE") ne = r;
  CHECK(!ne.is_null() && ne["fresh"] == true && ne["two_sided"] == true);
  FeeModel kf{"quadratic_with_maker_fees", 1};
  json pick = nullptr;
  for (const auto& r : ne["routes"]) if (r["route"] == "buy_kalshi_sell_pm" && r["kalshi_style"] == "maker" && r["pm_style"] == "taker") pick = r;
  CHECK(!pick.is_null());
  if (!pick.is_null()) {
    // Buy NE on Kalshi at 0.37 (bid + tick), sell it on Polymarket at the 0.40 bid: gross 0.03,
    // less Kalshi's maker fee at 0.37 and Polymarket's taker fee at 0.40.
    double expect = 0.40 - 0.37 - kf.fee(0.37, 1, false) - poly_fee(0.40, true);
    CHECK_NEAR(pick["profit"].get<double>(), expect, 1e-4);
    CHECK_NEAR(pick["capital"].get<double>(), 0.37 + 0.60, 1e-9);
  }
  json pick2 = nullptr;
  for (const auto& r : ne["routes"]) if (r["route"] == "sell_kalshi_buy_pm" && r["kalshi_style"] == "taker" && r["pm_style"] == "taker") pick2 = r;
  CHECK(!pick2.is_null());
  if (!pick2.is_null()) {
    // Sell NE on Kalshi = buy NO at 1 - 0.36 = 0.64; buy NE on Polymarket at 0.405: gross 1 - 0.64 - 0.405 = -0.045.
    double expect = 1 - 0.64 - 0.405 - kf.fee(0.64, 1, true) - poly_fee(0.405, true);
    CHECK_NEAR(pick2["profit"].get<double>(), expect, 1e-4);
  }
  // The SEA side sees the same books flipped: Polymarket 0.595/0.60 for SEA-YES.
  json sea = nullptr;
  for (const auto& r : rows) if (r["kalshi_ticker"] == "KXNFLGAME-26SEP09NESEA-SEA") sea = r;
  CHECK(!sea.is_null() && std::fabs(sea["pm_bid"].get<double>() - 0.595) < 1e-9 && std::fabs(sea["pm_ask"].get<double>() - 0.60) < 1e-9);
  // The best route sorts first.
  CHECK(rows[0]["kalshi_ticker"] == "KXNFLGAME-26SEP09NESEA-NE");
}

// A daily league: the same two teams play on consecutive days, so the game date has to match
// exactly (Eastern), the ticker time keeps a doubleheader's games apart, and one Polymarket market
// belongs to one Kalshi event.
static void test_matching_daily_league() {
  MockPoly x;
  {
    std::lock_guard<std::mutex> lk(x.mu);
    auto mlb = [](const std::string& abbr, const std::string& name, const std::string& safe, const std::string& alias) {
      json t = team(abbr, name, safe, alias); t["league"] = "mlb"; return t;
    };
    json g8 = pm_winner("aec-mlb-hou-phi-2026-09-08", "baseball_team_full_game_winner", 0.79, 0.795, mlb("hou", "Houston Astros", "Houston", "Astros"),
                        mlb("phi", "Philadelphia Phillies", "Philadelphia", "Phillies"), "Astros", "Phillies", "2026-09-08T22:40:00Z");
    json g9 = pm_winner("aec-mlb-hou-phi-2026-09-09", "baseball_team_full_game_winner", 0.55, 0.56, mlb("hou", "Houston Astros", "Houston", "Astros"),
                        mlb("phi", "Philadelphia Phillies", "Philadelphia", "Phillies"), "Astros", "Phillies", "2026-09-09T22:40:00Z");
    // A doubleheader on the 10th: two games four hours apart.
    json g10a = pm_winner("aec-mlb-hou-phi-2026-09-10-1", "baseball_team_full_game_winner", 0.50, 0.51, mlb("hou", "Houston Astros", "Houston", "Astros"),
                          mlb("phi", "Philadelphia Phillies", "Philadelphia", "Phillies"), "Astros", "Phillies", "2026-09-10T17:10:00Z");
    json g10b = pm_winner("aec-mlb-hou-phi-2026-09-10-2", "baseball_team_full_game_winner", 0.45, 0.46, mlb("hou", "Houston Astros", "Houston", "Astros"),
                          mlb("phi", "Philadelphia Phillies", "Philadelphia", "Phillies"), "Astros", "Phillies", "2026-09-10T22:40:00Z");
    x.events = json::array({{{"slug", "mlb-hou-phi-2026-09-08"}, {"title", "Houston vs. Philadelphia"}, {"markets", json::array({g8})}},
                            {{"slug", "mlb-hou-phi-2026-09-09"}, {"title", "Houston vs. Philadelphia"}, {"markets", json::array({g9})}},
                            {{"slug", "mlb-hou-phi-2026-09-10-1"}, {"title", "Houston vs. Philadelphia"}, {"markets", json::array({g10a})}},
                            {{"slug", "mlb-hou-phi-2026-09-10-2"}, {"title", "Houston vs. Philadelphia"}, {"markets", json::array({g10b})}}});
    for (const auto& g : {g8, g9, g10a, g10b}) x.quotes[g["slug"].get<std::string>()] = g;
    x.events_league = "mlb";
  }
  Store st(":memory:");
  auto mk = [&](const std::string& t, const std::string& ev, const std::string& sub, double bid, double ask) {
    Market m; m.ticker = t; m.event_ticker = ev; m.series_ticker = "KXMLBGAME"; m.title = sub + " wins"; m.yes_sub_title = sub; m.status = "active";
    m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now_s() + 30 * 3600); m.yes_bid = bid; m.yes_ask = ask; m.tick = 0.01;
    m.yes_bid_size = 20; m.yes_ask_size = 30; m.quote_ts = now_s();
    st.upsert_market(m);
  };
  Series s; s.ticker = "KXMLBGAME"; s.category = "Sports"; s.fee_type = "quadratic_with_maker_fees"; s.fee_multiplier = 1;
  st.upsert_series(s);
  mk("KXMLBGAME-26SEP081840HOUPHI-HOU", "KXMLBGAME-26SEP081840HOUPHI", "Houston", 0.78, 0.80);
  mk("KXMLBGAME-26SEP081840HOUPHI-PHI", "KXMLBGAME-26SEP081840HOUPHI", "Philadelphia", 0.20, 0.22);
  mk("KXMLBGAME-26SEP091840HOUPHI-HOU", "KXMLBGAME-26SEP091840HOUPHI", "Houston", 0.54, 0.56);
  mk("KXMLBGAME-26SEP091840HOUPHI-PHI", "KXMLBGAME-26SEP091840HOUPHI", "Philadelphia", 0.44, 0.46);
  mk("KXMLBGAME-26SEP101310HOUPHI-HOU", "KXMLBGAME-26SEP101310HOUPHI", "Houston", 0.50, 0.52);
  mk("KXMLBGAME-26SEP101310HOUPHI-PHI", "KXMLBGAME-26SEP101310HOUPHI", "Philadelphia", 0.48, 0.50);
  mk("KXMLBGAME-26SEP101840HOUPHI-HOU", "KXMLBGAME-26SEP101840HOUPHI", "Houston", 0.45, 0.47);
  mk("KXMLBGAME-26SEP101840HOUPHI-PHI", "KXMLBGAME-26SEP101840HOUPHI", "Philadelphia", 0.53, 0.55);
  // A game Polymarket does not list (the 11th): no pair, whatever the day before looks like.
  mk("KXMLBGAME-26SEP111840HOUPHI-HOU", "KXMLBGAME-26SEP111840HOUPHI", "Houston", 0.50, 0.52);
  mk("KXMLBGAME-26SEP111840HOUPHI-PHI", "KXMLBGAME-26SEP111840HOUPHI", "Philadelphia", 0.48, 0.50);
  Config cfg = poly_cfg(json{{"leagues", json::array({"mlb"})}});
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), nullptr, "tests");
  PolyDesk desk(cfg, st, client, nullptr);
  long long now = now_s();
  desk.refresh_universe(now);
  desk.match(now);
  auto pairs = desk.pairs_by_ticker();
  CHECK(pairs.size() == 8);
  CHECK(pairs.count("KXMLBGAME-26SEP081840HOUPHI-HOU") && pairs["KXMLBGAME-26SEP081840HOUPHI-HOU"].slug == "aec-mlb-hou-phi-2026-09-08");
  CHECK(pairs.count("KXMLBGAME-26SEP091840HOUPHI-PHI") && pairs["KXMLBGAME-26SEP091840HOUPHI-PHI"].slug == "aec-mlb-hou-phi-2026-09-09");
  CHECK(pairs.count("KXMLBGAME-26SEP101310HOUPHI-HOU") && pairs["KXMLBGAME-26SEP101310HOUPHI-HOU"].slug == "aec-mlb-hou-phi-2026-09-10-1");
  CHECK(pairs.count("KXMLBGAME-26SEP101840HOUPHI-HOU") && pairs["KXMLBGAME-26SEP101840HOUPHI-HOU"].slug == "aec-mlb-hou-phi-2026-09-10-2");
  CHECK(!pairs.count("KXMLBGAME-26SEP111840HOUPHI-HOU") && !pairs.count("KXMLBGAME-26SEP111840HOUPHI-PHI"));
  // Every Polymarket market is used by exactly one Kalshi event.
  std::map<std::string, std::set<std::string>> owners;
  for (const auto& [t, p] : pairs) owners[p.slug].insert(p.kalshi_event);
  bool one_each = true;
  for (const auto& [slug, evs] : owners) if (evs.size() != 1) one_each = false;
  CHECK(one_each && owners.size() == 4);
}

static void test_account_rails_and_orders() {
  MockPoly x;
  seed_poly(x);
  Store st(":memory:");
  seed_kalshi(st);
  Config cfg = poly_cfg(json{{"max_order_dollars", 10.0}, {"max_total_dollars", 20.0}});
  std::string seed = random_seed_b64();
  auto auth = std::make_shared<PolyAuth>("key-1", seed);
  x.public_key = auth->public_key_b64();
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), auth, "tests");
  PolyDesk desk(cfg, st, client, nullptr);
  CHECK(desk.armed());
  long long now = now_s();
  desk.refresh_universe(now);
  desk.match(now);
  PolyOrderRequest r;
  r.slug = "aec-nfl-ne-sea-2026-09-09"; r.action = "BUY"; r.price = 0.38; r.quantity = 5;
  CHECK_THROWS(desk.ticket(r, "t"), "not synced");
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.positions = json{{"aec-nfl-dal-nyg-2026-09-13", {{"netPositionDecimal", "3"}, {"cost", {{"value", "1.80"}, {"currency", "USD"}}},
                                                       {"cashValue", {{"value", "0.05"}, {"currency", "USD"}}}, {"qtyAvailableDecimal", "3"}}}};
    x.open_orders = json::array({{{"id", "po-old"}, {"marketSlug", "aec-nfl-ne-sea-2026-09-09"}, {"side", "ORDER_SIDE_BUY"}, {"state", "ORDER_STATE_NEW"},
                                  {"price", {{"value", "0.370"}, {"currency", "USD"}}}, {"quantity", 2.0}, {"cumQuantity", 0.0}, {"leavesQuantity", 2.0}}});
    x.activities = json::array({{{"type", "ACTIVITY_TYPE_TRADE"}, {"trade", {{"id", "t-old"}, {"marketSlug", "aec-nfl-dal-nyg-2026-09-13"},
                                                                             {"price", {{"value", "0.60"}, {"currency", "USD"}}}, {"qtyDecimal", "3"},
                                                                             {"isAggressor", false}, {"createTime", "2026-09-08T20:00:00Z"}}}},
                                {{"type", "ACTIVITY_TYPE_ACCOUNT_DEPOSIT"}, {"accountBalanceChange", {{"transactionId", "dep-1"}, {"amount", {{"value", "100.00"}, {"currency", "USD"}}},
                                                                                                      {"updateTime", "2026-09-01T00:00:00Z"}}}}});
  }
  desk.refresh_account(now, true);
  CHECK(x.bad_auth == 0);
  json v = desk.view();
  CHECK_NEAR(v["balance"]["cash"].get<double>(), 100.0, 1e-9);
  CHECK(v["positions"].size() == 1 && v["positions"][0]["net"] == 3.0 && v["orders"].size() == 1 && v["trades"].size() == 1);
  CHECK(!st.query("SELECT * FROM poly_flows WHERE id='dep-1' AND amount=100").empty());
  json pnl = desk.pnl();
  CHECK(pnl["synced"] == true && std::fabs(pnl["moved_in_total"].get<double>() - 100.0) < 1e-9);
  // Rails.
  PolyOrderRequest bad = r; bad.price = 0.55;
  CHECK_THROWS(desk.ticket(bad, "t"), "from the mid");
  bad = r; bad.quantity = 30;   // 30 x 0.38 = 11.4 > 10
  CHECK_THROWS(desk.ticket(bad, "t"), "order cap");
  bad = r; bad.action = "SELL"; bad.quantity = 15; bad.price = 0.385;   // margin 15 x 0.615 = 9.2 fits; total 9.2 + 1.8 + 0.74 = 11.8 < 20 fine
  bad.quantity = 25;   // 25 x 0.615 = 15.4 > 10 order cap
  CHECK_THROWS(desk.ticket(bad, "t"), "order cap");
  bad = r; bad.quantity = 20;   // 7.6 fits the order cap; total 7.6 + 1.8 (position) + 0.74 (resting) = 10.14 < 20 fine
  { std::lock_guard<std::mutex> lk(x.mu); x.buying_power = 5.0; }
  desk.refresh_account(now + 1, true);
  CHECK_THROWS(desk.ticket(bad, "t"), "buying power");
  { std::lock_guard<std::mutex> lk(x.mu); x.buying_power = 100.0; }
  desk.refresh_account(now + 2, true);
  st.kv_set("kill", "1");
  CHECK_THROWS(desk.ticket(r, "t"), "kill switch");
  st.kv_set("kill", "0");
  // A ticket: signed, the documented body, executions read back.
  x.fill_fraction = 1.0;
  json o = desk.ticket(r, "hand");
  CHECK(x.orders == 1 && x.bad_auth == 0);
  CHECK(x.last_order["marketSlug"] == r.slug && x.last_order["action"] == "ORDER_ACTION_BUY" && x.last_order["outcomeSide"] == "OUTCOME_SIDE_YES");
  CHECK(x.last_order["price"]["value"] == "0.380" && x.last_order["quantity"] == 5.0 && x.last_order["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_MANUAL");
  CHECK(o["order_id"] == "po-1" && o["fill_count"] == 5.0 && std::fabs(o["average_fill_price"].get<double>() - 0.38) < 1e-9 && o["state"] == "filled");
  CHECK(!st.query("SELECT * FROM poly_orders WHERE id='po-1' AND state='filled' AND origin='ticket'").empty());
  // Resting: a GTC that does not fill stays 'new'.
  x.fill_fraction = 0.0;
  json o2 = desk.ticket(r, "rest");
  CHECK(o2["state"] == "new" && o2["remaining_count"] == 5.0);
  json c = desk.cancel("po-2", "");
  CHECK(x.cancels == 1 && x.last_cancel["id"] == "po-2" && x.last_cancel["body"]["marketSlug"] == r.slug && c["canceled"] == true);
  CHECK(!st.query("SELECT * FROM poly_orders WHERE id='po-2' AND state='canceled'").empty());
  // Close the DAL position: long 3 -> SELL 3 IOC at the bid.
  x.fill_fraction = 1.0;
  json cl = desk.close_position("aec-nfl-dal-nyg-2026-09-13");
  CHECK(x.last_order["action"] == "ORDER_ACTION_SELL" && x.last_order["quantity"] == 3.0 && x.last_order["tif"] == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL");
  CHECK(x.last_order["price"]["value"] == "0.600" && cl["fill_count"] == 3.0);
  CHECK_THROWS(desk.close_position("aec-nfl-ne-sea-2026-09-09"), "no Polymarket position");
  // The kill sweep pulls the resting order the sync reported.
  CHECK(desk.cancel_all("test") == 1 && x.cancel_alls == 1);
  // Preview goes out signed.
  json pv = desk.preview(r);
  CHECK(pv.contains("order") && x.bad_auth == 0);
  // The book.
  json b = desk.book("aec-nfl-ne-sea-2026-09-09");
  CHECK(std::fabs(b["best_bid"].get<double>() - 0.38) < 1e-9 && b["bid_size"] == 1000.0);
}

static void test_pair_rule_paper() {
  MockPoly x;
  seed_poly(x);
  Store st(":memory:");
  seed_kalshi(st);
  Config cfg = poly_cfg();
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), nullptr, "tests");
  PolyDesk desk(cfg, st, client, nullptr);
  long long now = now_s();
  desk.refresh_universe(now);
  desk.match(now);
  // NE: Kalshi 0.36/0.38, Polymarket 0.42/0.425 -> buy Kalshi at 0.37 (maker), sell Polymarket at 0.42 (taker): gross 0.05.
  x.quote("aec-nfl-ne-sea-2026-09-09", 0.42, 0.425);
  desk.refresh_quotes(now);
  CHECK_THROWS(desk.configure_algo(json{{"paper", false}}), "live needs");
  CHECK_THROWS(desk.configure_algo(json{{"settings", {{"kalshi_style", "hopeful"}}}}), "maker or taker");
  json v = desk.configure_algo(json{{"running", true}, {"paper", true}, {"settings", {{"min_lock", 0.02}, {"contracts", 4}, {"max_pair_dollars", 20}, {"min_hours_to_start", 0}}}});
  CHECK(v["algo"]["running"] == true && v["algo"]["paper"] == true);
  desk.step(now + 1);
  json open = st.query("SELECT * FROM xvenue_trades ORDER BY id");
  CHECK(open.size() == 1);
  if (open.size() == 1) {
    const json& t = open[0];
    CHECK(t["kalshi_ticker"] == "KXNFLGAME-26SEP09NESEA-NE" && t["state"] == "kalshi_working" && t["route"] == "buy_kalshi_sell_pm");
    CHECK(t["kalshi_side"] == "yes" && std::fabs(t["kalshi_price"].get<double>() - 0.37) < 1e-9 && t["contracts"] == 4.0);
    CHECK(t["pm_action"] == "SELL" && std::fabs(t["pm_price"].get<double>() - 0.42) < 1e-9 && t["mode"] == "paper");
  }
  // One pair per event: SEA is not opened while NE works.
  desk.step(now + 2);
  CHECK(st.query("SELECT * FROM xvenue_trades").size() == 1);
  // The Kalshi offer trades down through 0.37: the paper leg fills, and the hedge goes at once.
  {
    Market m = *st.get_market("KXNFLGAME-26SEP09NESEA-NE");
    m.yes_bid = 0.35; m.yes_ask = 0.37; m.quote_ts = now + 3;
    st.upsert_market(m);
  }
  desk.step(now + 3);
  json t = st.query("SELECT * FROM xvenue_trades ORDER BY id")[0];
  CHECK(t["state"] == "locked" && t["kalshi_filled"] == 4.0 && t["pm_filled"] == 4.0);
  FeeModel kf{"quadratic_with_maker_fees", 1};
  double kfee = kf.fee(0.37, 4, false), pfee = 4 * poly_fee(0.42, true);
  CHECK_NEAR(t["kalshi_cost"].get<double>(), 4 * 0.37 + kfee, 1e-6);
  CHECK_NEAR(t["pm_cost"].get<double>(), 4 * (1 - 0.42), 1e-6);
  CHECK_NEAR(t["locked"].get<double>(), 0.42 - 0.37 - kfee / 4 - poly_fee(0.42, true), 1e-4);
  // Settlement: NE wins. Kalshi pays $4, the Polymarket short loses its margin.
  st.exec("UPDATE markets SET result='yes', status='finalized' WHERE ticker='KXNFLGAME-26SEP09NESEA-NE'");
  desk.step(now + 4);
  t = st.query("SELECT * FROM xvenue_trades ORDER BY id")[0];
  CHECK(t["state"] == "settled");
  CHECK_NEAR(t["realized_pnl"].get<double>(), 4.0 - (4 * 0.37 + kfee) - 4 * (1 - 0.42) - pfee, 1e-4);
  // NE loses instead: Kalshi pays nothing, the short keeps $1 a contract. Same number either way: it is locked.
  st.exec("UPDATE xvenue_trades SET state='locked', realized_pnl=0, closed_ts=0");
  st.exec("UPDATE markets SET result='no' WHERE ticker='KXNFLGAME-26SEP09NESEA-NE'");
  desk.step(now + 5);
  t = st.query("SELECT * FROM xvenue_trades ORDER BY id")[0];
  CHECK_NEAR(t["realized_pnl"].get<double>(), 4.0 - (4 * 0.37 + kfee) - 4 * (1 - 0.42) - pfee, 1e-4);
  CHECK(t["state"] == "settled");
  // The pm fee is part of the lock, not of the cost basis: check it was recorded.
  CHECK_NEAR(t["pm_fee"].get<double>(), pfee, 1e-4);
  // Persistence: a new desk resumes running with the settings.
  PolyDesk again(cfg, st, client, nullptr);
  CHECK(again.view()["algo"]["running"] == true && again.view()["algo"]["settings"]["min_lock"] == 0.02);
}

static void test_pair_rule_live() {
  std::string pem = "/tmp/desk_poly_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip live pair test: openssl CLI not available\n"); return; }
  MockPoly x;
  seed_poly(x);
  MockKalshi k;
  {
    std::lock_guard<std::mutex> lk(k.mu);
    k.markets["KXNFLGAME-26SEP09NESEA-NE"] = json{{"event_ticker", "KXNFLGAME-26SEP09NESEA"}, {"yes_bid", 0.36}, {"yes_ask", 0.38}};
  }
  Store st(":memory:");
  seed_kalshi(st);
  Config cfg = poly_cfg();
  std::string seed = random_seed_b64();
  auto auth = std::make_shared<PolyAuth>("key-1", seed);
  x.public_key = auth->public_key_b64();
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), auth, "tests");
  KalshiClient kclient(k.base(), signer, "tests");
  Executor ex(cfg, st, kclient);
  ex.reconcile(true);
  std::atomic<bool> reconcile_asked{false};
  PolyDesk desk(cfg, st, client, &ex, PolyDesk::Hooks{[] { return false; }, [&] { reconcile_asked = true; }});
  long long now = now_s();
  desk.refresh_universe(now);
  desk.match(now);
  x.quote("aec-nfl-ne-sea-2026-09-09", 0.42, 0.425);
  desk.refresh_quotes(now);
  desk.refresh_account(now, true);
  desk.configure_algo(json{{"paper", false}, {"running", true}, {"settings", {{"min_lock", 0.02}, {"contracts", 4}, {"kalshi_ttl_seconds", 60}, {"min_hours_to_start", 0}}}});
  k.fill_all = 0;
  desk.step(now + 1);
  CHECK(k.writes == 1);
  CHECK(k.last_order["side"] == "bid" && k.last_order["price"] == "0.3700" && k.last_order["post_only"] == true && k.last_order["count"] == "4.00");
  json t = st.query("SELECT * FROM xvenue_trades ORDER BY id")[0];
  CHECK(t["state"] == "kalshi_working" && t["kalshi_order_id"] == "kord-1" && t["mode"] == "live");
  CHECK(!st.query("SELECT * FROM proposals WHERE strategy='xvenue' AND status='submitted'").empty());
  // Still resting: nothing more.
  desk.step(now + 10);
  CHECK(k.writes == 1 && x.orders == 0);
  // The Kalshi leg fills (the order row and the position say so): the Polymarket leg goes IOC at the bid.
  st.exec("UPDATE orders SET fill_count=4, remaining_count=0, status='executed' WHERE order_id='kord-1'");
  k.hold("KXNFLGAME-26SEP09NESEA-NE", 4, 1.48);
  ex.reconcile(false);
  x.fill_fraction = 1.0;
  desk.step(now + 11);
  CHECK(x.orders == 1 && x.bad_auth == 0);
  CHECK(x.last_order["action"] == "ORDER_ACTION_SELL" && x.last_order["price"]["value"] == "0.420" && x.last_order["quantity"] == 4.0);
  CHECK(x.last_order["tif"] == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL" && x.last_order["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_AUTOMATIC");
  t = st.query("SELECT * FROM xvenue_trades ORDER BY id")[0];
  CHECK(t["state"] == "locked" && t["pm_filled"] == 4.0 && t["pm_order_id"] == "po-1");
  Position mixed;mixed.ticker="KXNFLGAME-26SEP09NESEA-NE";mixed.position=7;
  CHECK_NEAR(manual_position_quantity(st,mixed),3.0,1e-9);
  // A pair fill that arrives through fills before either counter still owns its contracts.
  st.exec("UPDATE xvenue_trades SET kalshi_filled=0 WHERE id=?",{t["id"].get<long long>()});
  st.exec("UPDATE orders SET fill_count=0 WHERE order_id='kord-1'");
  Fill early;early.fill_id="pair-early-fill";early.order_id="kord-1";early.ticker=mixed.ticker;
  early.outcome_side="yes";early.count=4;early.yes_price=.37;early.no_price=.63;early.ts=now;
  st.upsert_fill(early);
  CHECK_NEAR(manual_position_quantity(st,mixed),3.0,1e-9);
  st.exec("UPDATE xvenue_trades SET kalshi_filled=4 WHERE id=?",{t["id"].get<long long>()});
  st.exec("UPDATE orders SET fill_count=4 WHERE order_id='kord-1'");
  CHECK(reconcile_asked);
  // A second pair in another event: the Kalshi leg is pulled after its TTL when it never fills.
  {
    std::lock_guard<std::mutex> lk(k.mu);
    k.markets["KXNFLGAME-26SEP13DALNYG-NYG"] = json{{"event_ticker", "KXNFLGAME-26SEP13DALNYG"}, {"yes_bid", 0.39}, {"yes_ask", 0.40}};
  }
  x.quote("aec-nfl-dal-nyg-2026-09-13", 0.55, 0.555);   // NYG-YES on Polymarket = 1 - 0.555 / 1 - 0.55 = 0.445 / 0.45; Kalshi NYG 0.39/0.40: buy Kalshi 0.40? maker 0.40? spread is 1 tick -> maker at 0.39? min(0.39, 0.40)=0.39... lock = 0.445 - 0.39 - fees
  desk.refresh_quotes(now + 12);
  desk.step(now + 12);
  CHECK(k.writes == 2);
  CHECK(k.last_order["ticker"] == "KXNFLGAME-26SEP13DALNYG-NYG");
  {
    std::lock_guard<std::mutex> lk(k.mu);
    k.resting_orders = json::array({{{"order_id", "kord-2"}, {"ticker", "KXNFLGAME-26SEP13DALNYG-NYG"},
        {"status", "resting"}, {"side", "yes"}, {"action", "buy"}, {"yes_price_dollars", "0.3900"},
        {"fill_count_fp", "0.00"}, {"remaining_count_fp", "4.00"}, {"initial_count_fp", "4.00"}}});
  }
  // An externally purchased position in that ticker must not be adopted as the pair's fill.
  k.hold("KXNFLGAME-26SEP13DALNYG-NYG",10,3.90);ex.reconcile(false);
  desk.step(now+13);
  CHECK(x.orders==1);
  desk.step(now + 12 + 61);
  CHECK(k.cancels == 1);
  json rows = st.query("SELECT * FROM xvenue_trades ORDER BY id");
  CHECK(rows.size() == 2 && rows[1]["state"] == "canceled");
  std::remove(pem.c_str());
}

static void test_pm_hedge_ownership_and_reconciliation() {
  MockPoly x; seed_poly(x); Store st(":memory:"); seed_kalshi(st); Config cfg = poly_cfg();
  auto auth = std::make_shared<PolyAuth>("key-1", random_seed_b64()); x.public_key = auth->public_key_b64();
  auto client = std::make_shared<PolyClient>(x.base(), x.base(), auth, "tests");
  st.kv_set("poly_algo:v1", json{{"paper", false}, {"running", false}, {"settings", {{"hedge_ttl_seconds", 5.0}}}}.dump());
  PolyDesk desk(cfg, st, client, nullptr);
  const long long now = now_s();
  const std::string slug = "aec-nfl-ne-sea-2026-09-09", ticker = "KXNFLGAME-26SEP09NESEA-NE";
  desk.refresh_universe(now); desk.match(now); x.quote(slug, .42, .425); desk.refresh_quotes(now); desk.refresh_account(now, true);
  // An automatic SELL must not consume a manual long position. A hand ticket retains its
  // existing ability to close that position, and unrelated resting orders remain untouched.
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.positions[slug] = {{"netPositionDecimal", "3"}, {"cost", {{"value", "1.2"}}}, {"qtyAvailableDecimal", "3"}};
  }
  desk.refresh_account(now, true);
  PolyOrderRequest manual; manual.slug = slug; manual.action = "SELL"; manual.price = .42; manual.quantity = 1; manual.tif = "IOC";
  PolyOrderRequest automatic = manual; automatic.manual = false;
  CHECK_THROWS(desk.ticket(automatic, "pair ownership test"), "opposite"); CHECK(x.orders == 0);
  desk.ticket(manual, "manual close"); CHECK(x.orders == 1);
  {
    std::lock_guard<std::mutex> lk(x.mu); x.positions = json::object();
    x.open_orders = json::array({{{"id", "external-order"}, {"marketSlug", slug}, {"side", "ORDER_SIDE_SELL"},
        {"state", "ORDER_STATE_NEW"}, {"quantity", 2}, {"cumQuantity", 0}, {"leavesQuantity", 2}, {"price", {{"value", ".425"}}}}});
  }
  desk.refresh_account(now, true);
  CHECK_THROWS(desk.ticket(automatic, "pair ownership test"), "another Polymarket order");
  CHECK(x.cancels == 0 && x.orders == 1);
  { std::lock_guard<std::mutex> lk(x.mu); x.open_orders = json::array(); x.fill_fraction = 0; }
  desk.refresh_account(now, true);
  // Seed only a known filled Kalshi leg. No real Kalshi executor is needed to test its PM hedge.
  json trade = json::object();
  auto columns = st.query("PRAGMA table_info(xvenue_trades)");
  for (const auto& column : columns) if (column["name"] != "id")
    trade[column["name"].get<std::string>()] = column["type"] == "TEXT" ? json("") : json(0.0);
  trade.update(json{{"kalshi_ticker", ticker}, {"kalshi_event", "KXNFLGAME-26SEP09NESEA"}, {"slug", slug},
      {"mode", "live"}, {"state", "hedging"}, {"contracts", 4}, {"kalshi_side", "yes"}, {"kalshi_price", .37},
      {"kalshi_filled", 4}, {"kalshi_cost", 1.48}, {"kalshi_filled_ts", now}, {"opened_ts", now},
      {"pm_action", "SELL"}, {"pm_style", "maker"}, {"pm_price", .425}, {"pm_yes_is_kalshi_yes", 1},
      {"route", "buy_kalshi_sell_pm"}});
  std::string names, placeholders; std::vector<SqlValue> values;
  for (auto it = trade.begin(); it != trade.end(); ++it) {
    if (!names.empty()) { names += ","; placeholders += ","; }
    names += it.key(); placeholders += "?";
    if (it.value().is_string()) values.emplace_back(it.value().get<std::string>()); else values.emplace_back(it.value().get<double>());
  }
  st.exec("INSERT INTO xvenue_trades(" + names + ") VALUES(" + placeholders + ")", values);
  auto latest = [&] { return st.query("SELECT * FROM xvenue_trades ORDER BY id DESC LIMIT 1")[0]; };
  desk.step(now + 1); CHECK(x.orders == 2); const std::string own_id = latest()["pm_order_id"];
  desk.step(now + 2); CHECK(x.orders == 2 && latest()["pm_filled"] == 0.0);
  {
    std::lock_guard<std::mutex> lk(x.mu);
    auto& o = x.order_details[own_id]; o["cumQuantity"] = 1; o["leavesQuantity"] = 3;
    o["state"] = "ORDER_STATE_PARTIALLY_FILLED"; o["avgPx"] = o["price"]; o["commissionNotionalTotalCollected"] = {{"value", ".01"}};
  }
  desk.step(now + 3); CHECK(x.orders == 2 && latest()["pm_filled"] == 1.0);
  { std::lock_guard<std::mutex> lk(x.mu); x.fail_order_lookup = true; }
  desk.step(now + 4); CHECK(x.orders == 2 && latest()["pm_filled"] == 1.0);
  {
    std::lock_guard<std::mutex> lk(x.mu); x.fail_order_lookup = false; x.reject_cancel = true;
    auto& o = x.order_details[own_id]; o["cumQuantity"] = 2; o["leavesQuantity"] = 2;
  }
  desk.step(now + 6); CHECK(x.orders == 2 && latest()["pm_filled"] == 2.0); // failed cancel must not roll back booked fills
  {
    std::lock_guard<std::mutex> lk(x.mu); x.reject_cancel = false; x.fill_on_cancel = 1; x.fill_fraction = 1;
  }
  desk.step(now + 7);
  CHECK(x.orders == 3 && x.last_order["quantity"] == 1.0); // two prior fills + one cancellation-race fill
  CHECK(latest()["state"] == "locked" && latest()["pm_filled"] == 4.0);
  CHECK_NEAR(latest()["pm_cost"].get<double>(), 3 * .575 + .58, 1e-4);
  CHECK_NEAR(latest()["pm_fee"].get<double>(), .02, 1e-4);
  // Kalshi settlement is not proof that the PM leg has settled or released any money.
  st.exec("UPDATE markets SET status='finalized',result='yes' WHERE ticker=?", {ticker});
  desk.step(now + 8); CHECK(latest()["state"] == "locked" && latest()["realized_pnl"] == 0.0);
  CHECK(latest()["reason"].get<std::string>().find("independent Polymarket") != std::string::npos);
  // A 5xx from the submission endpoint has an uncertain outcome: never retry automatically.
  st.exec("UPDATE markets SET status='active',result='' WHERE ticker=?", {ticker});
  st.exec("UPDATE xvenue_trades SET state='hedging',pm_order_id='',pm_filled=0,pm_cost=0,pm_fee=0,kalshi_filled_ts=?,pm_style='taker'", {now + 10});
  { std::lock_guard<std::mutex> lk(x.mu); x.uncertain_submit = true; }
  desk.step(now + 11); const int attempted = x.orders;
  desk.step(now + 12); CHECK(x.orders == attempted);
  CHECK(st.kv_get("poly_pair_send_pending:" + slug).value_or("") != "");
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--hedge-lifecycle") {
    test_pm_hedge_ownership_and_reconciliation();
    std::fprintf(stderr, "%d checks, %d failures\n", checks, fails); return fails == 0 ? 0 : 1;
  }
  test_auth();
  test_parsers_and_math();
  test_matching_quotes_and_locks();
  test_matching_daily_league();
  test_account_rails_and_orders();
  test_pair_rule_paper();
  test_pair_rule_live();
  test_pm_hedge_ownership_and_reconciliation();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
