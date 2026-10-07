// The fake Kalshi over loopback, seen from a raw client (tests/fake_exchange/client.h): every REST
// route and its shape, cursor paging, auth with a real RSA-PSS signature, the rate limits, the
// socket's commands, sids and seq, every channel, each fault the plan names (section 8), the
// recorded tape over the socket, many clients at once, and the fake-kalshi binary.
//
// Shapes are checked against the venue's documents (docs.kalshi.com, fetched 2026-10-06):
// api-reference/orders/*.md, portfolio/*.md, order-groups/*.md, websockets/*.md and
// getting_started/rate_limits.md. Nothing here talks to anything but 127.0.0.1.
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "check.h"
#include "client.h"
#include "deskd/build_paths.h"
#include "fake_kalshi.h"
#include "tape.h"

extern char** environ;

using namespace fakex;

namespace {

constexpr Micros D = kMicrosPerDollar;
constexpr std::int64_t T0 = 1791300600000;  // 2026-10-06T15:30:00Z
const std::string T = "KXTEST-26OCT06-A";            // shard 0, 1c grid
const std::string G = "KXNATGAS15M-26OCT061145-45";  // shard 2, tapered grid
const std::string V2 = "/trade-api/v2";
const std::string WS = "/trade-api/ws/v2";
const std::string ORDERS = "/portfolio/events/orders";

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

FakeConfig quiet() {
  FakeConfig c;
  c.rate_limits = false;
  return c;
}

MarketSpec spec(const std::string& ticker, int shard) {
  MarketSpec s;
  s.ticker = ticker;
  s.exchange_index = shard;
  if (shard == 2) {
    s.ticks = tapered_deci_cent();
    s.price_level_structure = "tapered_deci_cent";
  }
  return s;
}

// One test's venue on a free port: two markets, $1,000 on shards 0 and 2, and a street book on T
// (YES 0.40 x5 bid, 0.42 x3 ask).
struct Venue {
  FakeKalshi f;
  bool ok = false;
  explicit Venue(FakeConfig cfg = quiet(), bool markets = true) : f(std::move(cfg)) {
    if (markets)
      f.with_engine([](Engine& e, std::int64_t now) {
        e.add_market(spec(T, 0), now);
        e.add_market(spec(G, 2), now);
        e.set_cash(0, 0, 1000 * D, now);
        e.set_cash(0, 2, 1000 * D, now);
        e.street_add(T, false, 4000, 500, now);
        e.street_add(T, true, 5800, 300, now);
      });
    std::string err;
    ok = f.start(&err);
    if (!ok) std::fprintf(stderr, "fake start: %s\n", err.c_str());
  }
  int port() const { return f.port(); }
  bool has_cid(const std::string& cid) {
    return f.with_engine([&](Engine& e, std::int64_t) {
      for (const Order* o : e.user_orders())
        if (o->client_order_id == cid) return true;
      return false;
    });
  }
  // A copy, taken under the venue's lock: its timer thread may be changing the order.
  std::optional<Order> by_cid(const std::string& cid) {
    return f.with_engine([&](Engine& e, std::int64_t) -> std::optional<Order> {
      for (const Order* o : e.user_orders())
        if (o->client_order_id == cid) return *o;
      return std::nullopt;
    });
  }
};

std::string path_only(const std::string& target) { return target.substr(0, target.find('?')); }

// A signed request (AuthMode::PRESENCE takes any signature) to V2 + path.
HttpResult call(HttpConn& c, const std::string& method, const std::string& path, const json& body = json()) {
  const std::string target = V2 + path;
  return c.request(method, target, body.is_null() ? std::string() : body.dump(), kalshi_auth(method, path_only(target), wall_ms()));
}

json order(const std::string& ticker, const std::string& side, const std::string& count, const std::string& price,
           const std::string& tif, const std::string& cid = "", const json& extra = json::object()) {
  json b{{"ticker", ticker}, {"side", side},          {"count", count},
         {"price", price},   {"time_in_force", tif}, {"self_trade_prevention_type", "taker_at_cross"}};
  if (!cid.empty()) b["client_order_id"] = cid;
  for (const auto& [k, v] : extra.items()) b[k] = v;
  return b;
}
json post_only(const std::string& ticker, const std::string& count, const std::string& price, const std::string& cid) {
  return order(ticker, "bid", count, price, "good_till_canceled", cid, {{"post_only", true}});
}

std::set<std::string> keys(const json& j) {
  std::set<std::string> k;
  if (j.is_object())
    for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
  return k;
}
bool subset(const std::set<std::string>& a, const std::set<std::string>& b) {
  for (const auto& x : a)
    if (!b.count(x)) {
      std::fprintf(stderr, "  unexpected field %s\n", x.c_str());
      return false;
    }
  return true;
}

std::unique_ptr<WsConn> open_ws(int port) { return std::make_unique<WsConn>(port, kalshi_auth("GET", WS, wall_ms())); }

json cmd(int id, const std::string& name, const json& params = json()) {
  json j{{"id", id}, {"cmd", name}};
  if (!params.is_null()) j["params"] = params;
  return j;
}

std::optional<json> next_type(WsConn& ws, const std::string& type, int timeout_ms = 2000) {
  return ws.next_where([&](const json& m) { return m.value("type", "") == type; }, timeout_ms);
}

// The first wire-log line containing all of `parts`, or -1.
int log_at(const std::vector<std::string>& log, std::initializer_list<std::string> parts) {
  for (std::size_t i = 0; i < log.size(); ++i) {
    bool all = true;
    for (const auto& p : parts) all = all && log[i].find(p) != std::string::npos;
    if (all) return static_cast<int>(i);
  }
  return -1;
}

const std::set<std::string> kOrderFields = {
    "order_id", "user_id", "client_order_id", "ticker", "outcome_side", "book_side", "type", "status",
    "yes_price_dollars", "no_price_dollars", "fill_count_fp", "remaining_count_fp", "initial_count_fp",
    "taker_fees_dollars", "maker_fees_dollars", "taker_fill_cost_dollars", "maker_fill_cost_dollars",
    "expiration_time", "created_time", "last_update_time", "self_trade_prevention_type", "order_group_id",
    "cancel_order_on_pause", "subaccount_number", "exchange_index"};  // get-orders.md
const std::set<std::string> kFillFields = {
    "fill_id", "exchange_index", "trade_id", "order_id", "ticker", "market_ticker", "outcome_side", "book_side",
    "count_fp", "yes_price_dollars", "no_price_dollars", "is_taker", "fee_cost", "side", "action", "created_time",
    "subaccount_number", "ts"};  // get-fills.md

// ---- REST -----------------------------------------------------------------------------------------

void test_rest() {
  Venue v;
  CHECK(v.ok);
  HttpConn c(v.port());
  // create V2: a resting post-only bid
  auto r = call(c, "POST", ORDERS, post_only(T, "2", "0.39", "M-1"));
  CHECK(r.status() == 201);
  CHECK(keys(r.j) == std::set<std::string>({"order_id", "client_order_id", "fill_count", "remaining_count", "ts_ms"}));
  CHECK(r.j["fill_count"] == "0.00" && r.j["remaining_count"] == "2.00" && r.j["client_order_id"] == "M-1");
  CHECK(r.res.headers["content-type"] == "application/json" && r.res.complete && !r.res.chunked);
  const std::string m1 = r.j.value("order_id", "");
  // a taking IOC: its fill and averages come back in the 201 ("Only present when fill_count > 0")
  r = call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "M-2"));
  CHECK(r.status() == 201 && r.j["fill_count"] == "1.00" && r.j["remaining_count"] == "0.00");
  CHECK(r.j["average_fill_price"] == "0.4200" && r.j["average_fee_paid"] == "0.0171");
  const std::string m2 = r.j.value("order_id", "");

  // refused: 400 with {"error":{"code","message"}}; wrong types are refused, not coerced
  std::vector<json> bad;
  json b = order(T, "bid", "1", "0.40", "good_till_canceled");
  b.erase("self_trade_prevention_type");
  bad.push_back(b);
  bad.push_back(order(T, "bid", "1.234", "0.40", "good_till_canceled"));
  bad.push_back(order(T, "bid", "1", "0.5", "good_till_canceled"));
  bad.push_back(order(T, "bid", "1", "0.405", "good_till_canceled"));  // off the 1c grid
  bad.push_back(order(T, "bid", "1", "0.40", "gtc"));
  bad.push_back(order(T, "buy", "1", "0.40", "good_till_canceled"));
  b = order(T, "bid", "1", "0.40", "good_till_canceled");
  b["count"] = 1;
  bad.push_back(b);
  bad.push_back(order(T, "bid", "1", "0.40", "good_till_canceled", "", {{"expiration_time", "1791300700"}}));
  bad.push_back(order(T, "bid", "1", "0.40", "good_till_canceled", "", {{"reduce_only", true}}));
  bad.push_back(order(T, "bid", "1", "0.40", "good_till_canceled", "", {{"post_only", "yes"}}));
  bad.push_back(order(T, "bid", "1", "0.40", "good_till_canceled", "", {{"subaccount", 64}}));
  for (const auto& body : bad) {
    r = call(c, "POST", ORDERS, body);
    CHECK(r.status() == 400 && r.j["error"].is_object() && r.j["error"].contains("code") && r.j["error"].contains("message"));
  }
  r = c.request("POST", V2 + ORDERS, "not json", kalshi_auth("POST", V2 + ORDERS, wall_ms()));
  CHECK(r.status() == 400 && r.j["error"]["message"] == "the body is not JSON");
  r = call(c, "POST", ORDERS, post_only(T, "1", "0.30", "M-1"));
  CHECK(r.status() == 409);  // the client id is taken
  r = call(c, "PATCH", ORDERS, json::object());
  CHECK(r.status() == 405);

  // auth on /portfolio only; market data without it
  r = c.request("GET", V2 + "/portfolio/balance");
  CHECK(r.status() == 401 && r.j["error"]["code"] == "authentication_error");
  r = c.request("GET", V2 + "/exchange/status");
  CHECK(r.status() == 200 && r.j["exchange_active"] == true);
  r = c.request("GET", V2 + "/markets/" + T);
  CHECK(r.status() == 200 && r.j["market"]["ticker"] == T && r.j["market"]["exchange_index"] == 0 &&
        r.j["market"]["status"] == "active" && r.j["market"]["price_ranges"].size() == 1);
  r = c.request("GET", V2 + "/markets/" + G);
  CHECK(r.j["market"]["price_level_structure"] == "tapered_deci_cent" && r.j["market"]["exchange_index"] == 2 &&
        r.j["market"]["price_ranges"].size() == 3 && r.j["market"]["event_ticker"] == "KXNATGAS15M-26OCT061145");
  // the book ascending, the best last (what the venue sent the old desk: src/types.cpp)
  r = c.request("GET", V2 + "/markets/" + T + "/orderbook");
  CHECK(r.j["orderbook_fp"]["yes_dollars"] == json::parse(R"([["0.3900","2.00"],["0.4000","5.00"]])"));
  CHECK(r.j["orderbook_fp"]["no_dollars"] == json::parse(R"([["0.5800","2.00"]])"));
  r = c.request("GET", V2 + "/markets/" + T + "/orderbook?depth=1");
  CHECK(r.j["orderbook_fp"]["yes_dollars"] == json::parse(R"([["0.4000","5.00"]])"));
  CHECK(c.request("GET", V2 + "/markets/" + T + "/orderbook?depth=101").status() == 400);
  CHECK(c.request("GET", V2 + "/markets/NOPE").status() == 404);
  CHECK(c.request("GET", V2 + "/nope").status() == 404);
  CHECK(c.request("GET", "/elsewhere").status() == 404);

  // one order
  r = call(c, "GET", "/portfolio/orders/" + m1);
  // the documented fields, less those with no value and cancel_order_on_pause, as demo sent them
  CHECK(r.status() == 200 && subset(keys(r.j["order"]), kOrderFields) && keys(r.j["order"]).size() == kOrderFields.size() - 3);
  const json o = r.j["order"];
  CHECK(o["status"] == "resting" && o["yes_price_dollars"] == "0.3900" && o["no_price_dollars"] == "0.6100");
  CHECK(o["remaining_count_fp"] == "2.00" && o["client_order_id"] == "M-1" && !o.contains("expiration_time"));
  CHECK(!o.contains("order_group_id") && !o.contains("cancel_order_on_pause") && o["type"] == "limit" &&
        o["book_side"] == "bid" && o["outcome_side"] == "yes");
  CHECK(call(c, "GET", "/portfolio/orders/nope").status() == 404);

  // GET /portfolio/orders: newest first, limit and cursor until the cursor is ""
  std::map<std::string, std::string> ids;
  for (int i = 0; i < 5; ++i) {
    const std::string cid = "P-" + std::to_string(i);
    r = call(c, "POST", ORDERS, post_only(T, "1", "0.3" + std::to_string(i), cid));
    ids[cid] = r.j.value("order_id", "");
  }
  std::vector<std::string> seen;
  std::string cursor;
  int pages = 0;
  do {
    r = call(c, "GET", "/portfolio/orders?limit=2&status=resting" + (cursor.empty() ? "" : "&cursor=" + cursor));
    CHECK(r.status() == 200 && r.j["orders"].size() <= 2);
    for (const auto& x : r.j["orders"]) seen.push_back(x["client_order_id"]);
    cursor = r.j.value("cursor", "");
  } while (!cursor.empty() && ++pages < 10);
  CHECK(pages == 2 && seen == std::vector<std::string>({"P-4", "P-3", "P-2", "P-1", "P-0", "M-1"}));
  for (const char* q : {"limit=0", "limit=1001", "limit=x", "cursor=zzz", "status=open", "min_ts=soon"})
    CHECK(call(c, "GET", std::string("/portfolio/orders?") + q).status() == 400);
  CHECK(call(c, "GET", "/portfolio/orders?ticker=" + G).j["orders"].empty());
  CHECK(call(c, "GET", "/portfolio/orders?event_ticker=KXTEST-26OCT06").j["orders"].size() == 7);
  CHECK(call(c, "GET", "/portfolio/orders?min_ts=" + std::to_string(wall_ms() / 1000 + 100)).j["orders"].empty());

  // fills: two more takes, then pages of 2
  call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "M-3"));
  call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "M-4"));
  r = call(c, "GET", "/portfolio/fills?limit=2");
  CHECK(r.status() == 200 && r.j["fills"].size() == 2 && !r.j["cursor"].get<std::string>().empty());
  CHECK(subset(keys(r.j["fills"][0]), kFillFields));
  CHECK(r.j["fills"][0]["ticker"] == T && r.j["fills"][0]["market_ticker"] == T && r.j["fills"][0]["is_taker"] == true);
  CHECK(r.j["fills"][0]["count_fp"] == "1.00" && r.j["fills"][0]["fee_cost"] == "0.017100");
  r = call(c, "GET", "/portfolio/fills?limit=2&cursor=" + r.j["cursor"].get<std::string>());
  CHECK(r.j["fills"].size() == 1 && r.j["cursor"] == "" && r.j["fills"][0]["order_id"] == m2);
  CHECK(call(c, "GET", "/portfolio/fills?order_id=" + m2).j["fills"].size() == 1);
  CHECK(call(c, "GET", "/portfolio/fills?ticker=" + G).j["fills"].empty());

  // positions: one market a page, by ticker
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(G, true, 6000, 500, now); });
  CHECK(call(c, "POST", ORDERS, order(G, "bid", "1", "0.40", "immediate_or_cancel")).j["fill_count"] == "1.00");
  r = call(c, "GET", "/portfolio/positions?limit=1");
  CHECK(r.status() == 200 && r.j["market_positions"].size() == 1 && r.j["market_positions"][0]["ticker"] == G);
  CHECK(keys(r.j["market_positions"][0]) ==
        std::set<std::string>({"ticker", "exchange_index", "total_traded_dollars", "position_fp",
                               "market_exposure_dollars", "realized_pnl_dollars", "fees_paid_dollars", "last_updated_ts"}));
  CHECK(r.j["event_positions"].size() == 2 &&
        keys(r.j["event_positions"][0]) == std::set<std::string>({"event_ticker", "total_cost_dollars", "total_cost_shares_fp",
                                                                   "event_exposure_dollars", "realized_pnl_dollars",
                                                                   "fees_paid_dollars"}));
  r = call(c, "GET", "/portfolio/positions?limit=1&cursor=" + r.j["cursor"].get<std::string>());
  CHECK(r.j["market_positions"].size() == 1 && r.j["cursor"] == "");
  CHECK(r.j["market_positions"][0]["ticker"] == T && r.j["market_positions"][0]["position_fp"] == "3.00" &&
        r.j["market_positions"][0]["market_exposure_dollars"] == "1.2600");
  CHECK(call(c, "GET", "/portfolio/positions?exchange_index=2").j["market_positions"].size() == 1);
  CHECK(call(c, "GET", "/portfolio/positions?settlement_status=settled").j["market_positions"].empty());
  CHECK(call(c, "GET", "/portfolio/positions?count_filter=position").j["market_positions"].size() == 2);
  CHECK(call(c, "GET", "/portfolio/positions?count_filter=size").status() == 400);

  // balance: available cents, the dollars string, and every shard
  r = call(c, "GET", "/portfolio/balance");
  CHECK(keys(r.j) == std::set<std::string>({"balance", "balance_dollars", "portfolio_value", "updated_ts", "balance_breakdown"}));
  const Micros avail = v.f.with_engine([](Engine& e, std::int64_t) { return e.available(0, 0) + e.available(0, 2); });
  CHECK(r.j["balance"] == avail / 10000 && r.j["balance_dollars"] == usd4(avail));
  CHECK(r.j["balance_breakdown"].size() == 2 && keys(r.j["balance_breakdown"][0]) == std::set<std::string>({"exchange_index", "balance"}));
  r = call(c, "GET", "/portfolio/balance?exchange_index=2");
  CHECK(r.j["balance"] == v.f.with_engine([](Engine& e, std::int64_t) { return e.available(0, 2); }) / 10000);

  // cancel one: routed by market_ticker; without it, to shard 0
  r = call(c, "DELETE", ORDERS + "/" + m1 + "?market_ticker=" + T);
  CHECK(r.status() == 200 && keys(r.j) == std::set<std::string>({"order_id", "reduced_by", "ts_ms"}));   // demo, 2026-10-07
  CHECK(r.j["reduced_by"] == "2.00" && r.j["order_id"] == m1);
  CHECK(call(c, "DELETE", ORDERS + "/" + m1 + "?market_ticker=" + T).status() == 404);
  r = call(c, "POST", ORDERS, post_only(G, "1", "0.30", "G-1"));
  const std::string g1 = r.j.value("order_id", "");
  CHECK(call(c, "DELETE", ORDERS + "/" + g1).status() == 404);
  CHECK(call(c, "DELETE", ORDERS + "/" + g1 + "?exchange_index=x").status() == 400);
  CHECK(call(c, "DELETE", ORDERS + "/" + g1 + "?exchange_index=2").status() == 200);

  // batch cancel: one result an order, each with its error or null
  r = call(c, "DELETE", ORDERS + "/batched",
           {{"orders", json::array({{{"order_id", ids["P-0"]}, {"market_ticker", T}}, {{"order_id", "nope"}, {"exchange_index", 0}}})}});
  CHECK(r.status() == 200 && r.j["orders"].size() == 2);
  CHECK(r.j["orders"][0]["error"].is_null() && r.j["orders"][0]["reduced_by"] == "1.00" && r.j["orders"][0]["client_order_id"] == "P-0");
  CHECK(r.j["orders"][1]["error"]["code"] == "not_found" && r.j["orders"][1]["reduced_by"] == "0.00" &&
        r.j["orders"][1]["ts_ms"].is_null());
  CHECK(call(c, "DELETE", ORDERS + "/batched", {{"orders", json::array()}}).status() == 400);

  // cancel-all: 204 and no body; the connection stays up
  r = call(c, "DELETE", ORDERS);
  CHECK(r.status() == 204 && r.res.body.empty() && r.res.complete && c.ok());
  CHECK(call(c, "GET", "/portfolio/orders?status=resting").j["orders"].empty());

  // order groups
  r = call(c, "POST", "/portfolio/order_groups/create", {{"contracts_limit", 5}});
  CHECK(r.status() == 201 && keys(r.j) == std::set<std::string>({"order_group_id", "subaccount", "exchange_index"}));
  CHECK(r.j["subaccount"] == 0 && r.j["exchange_index"] == 0);
  const std::string gid = r.j.value("order_group_id", "");
  for (const json& bg : {json{{"contracts_limit", 5}, {"contracts_limit_fp", "6.00"}}, json::object(),
                         json{{"contracts_limit", 0}}, json{{"contracts_limit_fp", "0.50"}}, json{{"contracts_limit", "5"}}})
    CHECK(call(c, "POST", "/portfolio/order_groups/create", bg).status() == 400);
  r = call(c, "GET", "/portfolio/order_groups");
  CHECK(r.j["order_groups"].size() == 1 && r.j["order_groups"][0]["id"] == gid && r.j["order_groups"][0]["contracts_limit_fp"] == "5.00");
  r = call(c, "POST", ORDERS, order(T, "bid", "1", "0.30", "good_till_canceled", "GR-1", {{"order_group_id", gid}}));
  const std::string gr1 = r.j.value("order_id", "");
  r = call(c, "GET", "/portfolio/order_groups/" + gid);
  CHECK(r.j["contracts_limit_fp"] == "5.00" && r.j["orders"] == json::array({gr1}) && r.j["is_auto_cancel_enabled"] == true);
  CHECK(call(c, "PUT", "/portfolio/order_groups/" + gid + "/trigger").status() == 200);
  CHECK(call(c, "GET", "/portfolio/orders/" + gr1).j["order"]["status"] == "canceled");
  r = call(c, "POST", ORDERS, order(T, "bid", "1", "0.30", "good_till_canceled", "", {{"order_group_id", gid}}));
  CHECK(r.status() == 400 && r.j["error"]["details"] == "order group triggered");
  CHECK(call(c, "PUT", "/portfolio/order_groups/" + gid + "/trigger?exchange_index=2").status() == 404);
  CHECK(call(c, "PUT", "/portfolio/order_groups/" + gid + "/reset").status() == 200);
  CHECK(call(c, "PUT", "/portfolio/order_groups/" + gid + "/limit", {{"contracts_limit_fp", "10.00"}}).status() == 200);
  CHECK(call(c, "GET", "/portfolio/order_groups/" + gid).j["contracts_limit_fp"] == "10.00");
  CHECK(call(c, "PUT", "/portfolio/order_groups/" + gid + "/bogus").status() == 404);
  CHECK(call(c, "GET", "/portfolio/order_groups/" + gid + "/trigger").status() == 405);
  CHECK(call(c, "DELETE", "/portfolio/order_groups/" + gid).status() == 200);
  CHECK(call(c, "GET", "/portfolio/order_groups/" + gid).status() == 404);
  r = call(c, "POST", "/portfolio/order_groups/create", {{"contracts_limit", 5}, {"exchange_index", 2}});
  CHECK(r.status() == 201 && r.j["exchange_index"] == 2);

  // a transfer between shards, in centicents (the old desk's body, src/client.cpp)
  const Micros before = v.f.with_engine([](Engine& e, std::int64_t) { return e.cash(0, 2); });
  r = call(c, "POST", "/portfolio/intra_exchange_instance_transfer",
           {{"source", "event_contract"}, {"destination", "event_contract"}, {"amount", 500000},
            {"source_exchange_shard", 0}, {"destination_exchange_shard", 2}});
  CHECK(r.status() == 200 && !r.j.value("transfer_id", "").empty());
  CHECK(v.f.with_engine([](Engine& e, std::int64_t) { return e.cash(0, 2); }) == before + 50 * D);
  CHECK(call(c, "POST", "/portfolio/intra_exchange_instance_transfer", {{"source", "perps"}}).status() == 400);

  // a request line that is not HTTP: 400, then the close
  int fd = net::connect_loopback(v.port(), 2000);
  net::write_all(fd, "NONSENSE\r\n\r\n");
  std::string buf;
  net::Response res;
  net::read_response(fd, buf, res, 2000);
  CHECK(res.status == 400 && net::lower(res.headers["connection"]) == "close");
  ::close(fd);
  // a chunked request body, and two requests written before either reply (pipelined): each read
  // whole, answered in order on the one connection
  auto head = [](const std::string& method, const std::string& target) {
    std::string h = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    for (const auto& [k, val] : kalshi_auth(method, path_only(target), wall_ms())) h += k + ": " + val + "\r\n";
    return h;
  };
  fd = net::connect_loopback(v.port(), 2000);
  const std::string body = post_only(T, "1", "0.30", "CH-1").dump();
  net::write_all(fd, head("POST", V2 + ORDERS) + "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" +
                         net::chunked_body(body, 9) + head("GET", V2 + "/portfolio/balance") + "\r\n");
  buf.clear();
  CHECK(net::read_response(fd, buf, res, 2000) == net::ReadResult::OK && res.status == 201 &&
        json::parse(res.body)["client_order_id"] == "CH-1");
  CHECK(net::read_response(fd, buf, res, 2000) == net::ReadResult::OK && res.status == 200 &&
        json::parse(res.body).contains("balance"));
  ::close(fd);
}

// ---- auth -----------------------------------------------------------------------------------------

void test_auth() {
  EVP_PKEY* key = EVP_RSA_gen(2048);
  CHECK(key != nullptr);
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(bio, key);
  char* data = nullptr;
  const long n = BIO_get_mem_data(bio, &data);
  const std::string pem(data, static_cast<std::size_t>(n));
  BIO_free(bio);
  // RSA-PSS with SHA-256 and a digest-length salt over timestamp + method + path (src/auth.cpp)
  const SignFn sign = [&](const std::string& text) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), nullptr, key);
    EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
    EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
    std::size_t len = 0;
    EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    std::string sig(len, '\0');
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                   reinterpret_cast<const unsigned char*>(text.data()), text.size());
    sig.resize(len);
    EVP_MD_CTX_free(ctx);
    return net::base64(sig);
  };
  FakeConfig cfg = quiet();
  cfg.auth = AuthMode::VERIFY;
  cfg.public_key_pem = pem;
  cfg.auth_max_skew_ms = 5000;
  Venue v(cfg);
  CHECK(v.ok);
  HttpConn c(v.port());
  const std::string bal = V2 + "/portfolio/balance";
  // the path is signed without its query
  CHECK(c.request("GET", bal + "?exchange_index=2", "", kalshi_auth("GET", bal, wall_ms(), sign)).status() == 200);
  CHECK(c.request("GET", bal + "?exchange_index=2", "", kalshi_auth("GET", bal + "?exchange_index=2", wall_ms(), sign)).status() == 401);
  CHECK(c.request("GET", bal, "", kalshi_auth("POST", bal, wall_ms(), sign)).status() == 401);
  CHECK(c.request("GET", bal, "", kalshi_auth("GET", bal, wall_ms() - 60000, sign)).status() == 401);  // stale
  CHECK(c.request("GET", bal, "", kalshi_auth("GET", bal, wall_ms())).status() == 401);  // a made-up signature
  Headers huge = kalshi_auth("GET", bal, wall_ms(), sign);
  huge[1].second = "99999999999999999999999";
  CHECK(c.request("GET", bal, "", huge).status() == 401);
  const std::string create = V2 + ORDERS;
  CHECK(c.request("POST", create, post_only(T, "1", "0.30", "A-1").dump(), kalshi_auth("POST", create, wall_ms(), sign)).status() == 201);
  // the socket's handshake signs GET /trade-api/ws/v2
  WsConn good(v.port(), kalshi_auth("GET", WS, wall_ms(), sign));
  CHECK(good.ok());
  WsConn bad(v.port(), kalshi_auth("GET", WS, wall_ms()));
  CHECK(!bad.ok() && bad.handshake_status() == 401);
  // VERIFY with no key refuses to start
  FakeConfig nk = quiet();
  nk.auth = AuthMode::VERIFY;
  FakeKalshi f2(nk);
  std::string err;
  CHECK(!f2.start(&err) && !err.empty());
  EVP_PKEY_free(key);
}

// ---- rate limits (getting_started/rate_limits.md; costs from /account/endpoint_costs) -------------

void test_rate_limits() {
  FakeConfig cfg;  // limits on, Basic: writes 100/s holding 1 s, reads 200/s holding 3 s
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  Venue v(cfg);
  HttpConn c(v.port());
  int created = 0;
  for (int i = 0; i < 10; ++i) created += call(c, "POST", ORDERS, post_only(T, "1", "0.30", "R-" + std::to_string(i))).status() == 201;
  CHECK(created == 10);
  auto r = call(c, "POST", ORDERS, post_only(T, "1", "0.30", "R-10"));
  CHECK(r.status() == 429 && r.j == json({{"error", "too many requests"}}) && !v.has_cid("R-10"));
  // exchange_sharding.md: "Auto-routed single REST order writes are billed to the unscoped Write
  // bucket and every nonzero shard's Write bucket." The ten above emptied shard 2's bucket too, so
  // an order naming shard 2 is refused as well. deskd names the shard on every write.
  auto g_order = [](int shard) {
    json extra = json::object();
    if (shard >= 0) extra["exchange_index"] = shard;
    return order(G, "bid", "1", "0.30", "good_till_canceled", "", extra);
  };
  auto t_order = [](int shard) {
    json extra = json::object();
    if (shard >= 0) extra["exchange_index"] = shard;
    return order(T, "bid", "1", "0.30", "good_till_canceled", "", extra);
  };
  CHECK(call(c, "POST", ORDERS, g_order(2)).status() == 429);
  CHECK(call(c, "POST", ORDERS, t_order(0)).status() == 429);
  v.f.advance_ms(100);  // 10 tokens back in each bucket
  // "Single REST order writes that explicitly target a nonzero shard are rate-limited against that
  // shard's Write budget ... explicit shard 0 writes use only the unscoped Write budget"
  CHECK(call(c, "POST", ORDERS, g_order(2)).status() == 201);
  CHECK(call(c, "POST", ORDERS, t_order(0)).status() == 201);
  CHECK(call(c, "POST", ORDERS, g_order(-1)).status() == 429);
  v.f.advance_ms(100);
  CHECK(call(c, "POST", ORDERS, g_order(-1)).status() == 201);  // auto-routed: both buckets
  CHECK(call(c, "POST", ORDERS, g_order(2)).status() == 429);
  CHECK(call(c, "POST", ORDERS, t_order(0)).status() == 429);
  // a cancel costs 2
  const std::string r0 = v.by_cid("R-0")->order_id;
  const std::string r1 = v.by_cid("R-1")->order_id;
  v.f.advance_ms(20);
  CHECK(call(c, "DELETE", ORDERS + "/" + r0 + "?exchange_index=0").status() == 200);
  CHECK(call(c, "DELETE", ORDERS + "/" + r1 + "?exchange_index=0").status() == 429);
  // a batch costs 2 an order, and the whole batch must fit
  v.f.advance_ms(30);
  const json batch{{"orders", json::array({{{"order_id", r1}, {"exchange_index", 0}}, {{"order_id", "x"}, {"exchange_index", 0}}})}};
  CHECK(call(c, "DELETE", ORDERS + "/batched", batch).status() == 429);
  v.f.advance_ms(10);
  CHECK(call(c, "DELETE", ORDERS + "/batched", batch).status() == 200);
  // reads: 600 tokens at 10 a read
  int reads = 0;
  for (int i = 0; i < 61; ++i) reads += call(c, "GET", "/portfolio/balance").status() == 200;
  CHECK(reads == 60);
  v.f.advance_ms(10);  // 2 read tokens: GET one order costs 2
  CHECK(call(c, "GET", "/portfolio/orders/" + r0).status() == 200);
  CHECK(call(c, "GET", "/portfolio/orders/" + r0).status() == 429);
  // Advanced: 300 a second holding 3 s
  v.f.set_rate_limits(true, advanced_tier());
  created = 0;
  for (int i = 0; i < 91; ++i)
    created += call(c, "POST", ORDERS, post_only(T, "1", "0.30", "AD-" + std::to_string(i))).status() == 201;
  CHECK(created == 90);
}

// ---- the socket: commands, sids and seq ------------------------------------------------------------

void test_ws_protocol() {
  Venue v;
  auto ws = open_ws(v.port());
  CHECK(ws->ok() && ws->handshake_status() == 101);
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_ticker", T}}));
  auto m = ws->next();
  CHECK(m && (*m)["type"] == "subscribed" && (*m)["id"] == 1 && (*m)["msg"]["channel"] == "orderbook_delta" && (*m)["msg"]["sid"] == 1);
  CHECK(m && !m->contains("seq") && m->contains("sending_ts_ms"));  // subscribed takes no number
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_snapshot" && (*m)["sid"] == 1 && (*m)["seq"] == 1);
  CHECK(ws->last().payload.rfind(R"({"type":"orderbook_snapshot","sid":1,"seq":1,"msg":{)", 0) == 0);
  CHECK(m && (*m)["msg"]["market_ticker"] == T && !(*m)["msg"]["market_id"].get<std::string>().empty());
  CHECK(m && (*m)["msg"]["yes_dollars_fp"] == json::parse(R"([["0.4000","5.00"]])") &&
        (*m)["msg"]["no_dollars_fp"] == json::parse(R"([["0.5800","3.00"]])"));
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(T, false, 4100, 100, now); });
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_delta" && (*m)["seq"] == 2 && (*m)["msg"]["price_dollars"] == "0.4100" &&
        (*m)["msg"]["delta_fp"] == "1.00" && (*m)["msg"]["side"] == "yes" && !(*m)["msg"].contains("client_order_id"));
  // our order's change carries our client_order_id
  HttpConn c(v.port());
  call(c, "POST", ORDERS, post_only(T, "2", "0.39", "W-1"));
  m = ws->next();
  CHECK(m && (*m)["seq"] == 3 && (*m)["msg"]["client_order_id"] == "W-1" && (*m)["msg"]["delta_fp"] == "2.00");

  // ticker, with its initial snapshot: no seq on this channel
  ws->send_json(cmd(2, "subscribe", {{"channels", json::array({"ticker"})}, {"market_tickers", json::array({T})},
                                     {"send_initial_snapshot", true}}));
  m = ws->next();
  CHECK(m && (*m)["type"] == "subscribed" && (*m)["msg"]["sid"] == 2);
  m = ws->next();
  CHECK(m && (*m)["type"] == "ticker" && (*m)["sid"] == 2 && !m->contains("seq") && (*m)["msg"]["yes_bid_dollars"] == "0.4100");
  // two channels in one command: one ack each, sids in order
  ws->send_json(cmd(3, "subscribe", {{"channels", json::array({"trade", "orderbook_delta"})}, {"market_tickers", json::array({G})}}));
  m = ws->next();
  CHECK(m && (*m)["msg"]["channel"] == "trade" && (*m)["msg"]["sid"] == 3);
  m = ws->next();
  CHECK(m && (*m)["msg"]["channel"] == "orderbook_delta" && (*m)["msg"]["sid"] == 4);
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_snapshot" && (*m)["sid"] == 4 && (*m)["seq"] == 1 &&
        !(*m)["msg"].contains("yes_dollars_fp") && !(*m)["msg"].contains("no_dollars_fp"));  // an empty book: no sides
  // update_subscription: the ok takes a number in the sid's sequence, then the snapshot follows
  ws->send_json(cmd(4, "update_subscription", {{"sids", json::array({1})}, {"market_tickers", json::array({G})}, {"action", "add_markets"}}));
  m = ws->next();
  CHECK(m && (*m)["type"] == "ok" && (*m)["id"] == 4 && (*m)["sid"] == 1 && (*m)["seq"] == 4 &&
        (*m)["msg"]["market_tickers"] == json::array({T, G}));
  CHECK(ws->last().payload.rfind(R"({"id":4,"sid":1,"seq":4,"type":"ok",)", 0) == 0);
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_snapshot" && (*m)["seq"] == 5 && (*m)["msg"]["market_ticker"] == G);
  ws->send_json(cmd(5, "update_subscription", {{"sid", 1}, {"market_tickers", json::array({G})}, {"action", "get_snapshot"},
                                               {"skip_ticker_ack", true}}));
  m = ws->next();
  CHECK(m && (*m)["type"] == "ok" && (*m)["seq"] == 6 && (*m)["msg"] == json::object());
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_snapshot" && (*m)["seq"] == 7);
  ws->send_json(cmd(6, "update_subscription", {{"sids", json::array({1})}, {"market_tickers", json::array({G})}, {"action", "delete_markets"}}));
  m = ws->next();
  CHECK(m && (*m)["seq"] == 8 && (*m)["msg"]["market_tickers"] == json::array({T}));
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(G, false, 3000, 100, now); });
  m = ws->next();
  CHECK(m && (*m)["type"] == "orderbook_delta" && (*m)["sid"] == 4 && (*m)["seq"] == 2 && (*m)["msg"]["market_ticker"] == G);
  CHECK(!ws->next(200));  // nothing on sid 1 for G now, and no trade
  // unsubscribe takes a number too; list
  ws->send_json(cmd(7, "unsubscribe", {{"sids", json::array({3})}}));
  m = ws->next();
  CHECK(m && (*m)["type"] == "unsubscribed" && (*m)["id"] == 7 && (*m)["sid"] == 3 && (*m)["seq"] == 1);
  ws->send_json(cmd(8, "list_subscriptions"));
  m = ws->next();
  CHECK(m && (*m)["type"] == "ok" && (*m)["msg"].size() == 3 && (*m)["msg"][2]["sid"] == 4);

  // errors, with the documented codes (websocket-connection.md)
  struct E {
    json c;
    int code;
  };
  const std::vector<E> errors = {
      {cmd(9, "bogus"), 5},
      {cmd(10, "subscribe"), 2},
      {cmd(11, "subscribe", json::object()), 3},
      {cmd(12, "subscribe", {{"channels", json::array({"nope"})}}), 8},
      {cmd(13, "subscribe", {{"channels", json::array({"ticker"})}, {"market_ticker", T}, {"market_tickers", json::array({T})}}), 11},
      {cmd(14, "update_subscription", {{"sids", json::array({1})}}), 15},
      {cmd(15, "update_subscription", {{"sids", json::array({1, 2})}, {"action", "add_markets"}, {"market_tickers", json::array({T})}}), 12},
      {cmd(16, "update_subscription", {{"sids", json::array({1})}, {"action", "frob"}}), 13},
      {cmd(17, "update_subscription", {{"sids", json::array({1})}, {"action", "add_markets"}}), 14},
      {cmd(18, "update_subscription", {{"sids", json::array({99})}, {"action", "add_markets"}, {"market_tickers", json::array({T})}}), 7},
      {cmd(19, "unsubscribe", json::object()), 4},
  };
  for (const auto& e : errors) {
    ws->send_json(e.c);
    m = ws->next();
    CHECK(m && (*m)["type"] == "error" && (*m)["id"] == e.c["id"] && (*m)["msg"]["code"] == e.code);
  }
  ws->send_text("not json");
  m = ws->next();
  CHECK(m && (*m)["type"] == "error" && (*m)["msg"]["code"] == 1 && !m->contains("id"));
  ws->send_json(cmd(0, "bogus"));  // "omitted if command had id=0"
  m = ws->next();
  CHECK(m && (*m)["type"] == "error" && !m->contains("id"));
  // ping, pong, close
  ws->send_frame(net::PING, "hi");
  CHECK(!ws->next(200) && ws->pongs_seen() == 1);
  ws->send_frame(net::CLOSE, std::string("\x03\xe8", 2));
  CHECK(!ws->next(1000) && ws->closed() && ws->close_frame_seen());

  // no market list: every market (the old recorder's flood), snapshots in ticker order
  auto all = open_ws(v.port());
  all->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}}));
  CHECK(next_type(*all, "subscribed"));
  m = all->next();
  CHECK(m && (*m)["msg"]["market_ticker"] == G);
  m = all->next();
  CHECK(m && (*m)["msg"]["market_ticker"] == T && (*m)["seq"] == 2);

  // handshakes refused
  WsConn wrong_path(v.port(), kalshi_auth("GET", "/nope", wall_ms()), "/nope");
  CHECK(!wrong_path.ok() && wrong_path.handshake_status() == 404);
  WsConn no_auth(v.port(), {});
  CHECK(!no_auth.ok() && no_auth.handshake_status() == 401);  // "Authentication is required even for public channels"
  HttpConn raw(v.port());
  Headers h = kalshi_auth("GET", WS, wall_ms());
  h.push_back({"Upgrade", "websocket"});
  h.push_back({"Connection", "Upgrade"});
  h.push_back({"Sec-WebSocket-Version", "13"});
  CHECK(raw.request("GET", WS, "", h).status() == 400);  // no Sec-WebSocket-Key

  // with auth off, the account channels still need a key on the handshake
  FakeConfig off = quiet();
  off.auth = AuthMode::OFF;
  Venue vo(off);
  WsConn anon(vo.port(), {});
  CHECK(anon.ok());
  anon.send_json(cmd(1, "subscribe", {{"channels", json::array({"fill"})}}));
  m = anon.next();
  CHECK(m && (*m)["type"] == "error" && (*m)["msg"]["code"] == 9);
  anon.send_json(cmd(2, "subscribe", {{"channels", json::array({"ticker"})}}));
  CHECK(next_type(anon, "subscribed"));
}

// ---- the socket: private channels -----------------------------------------------------------------

void test_ws_private() {
  Venue v;
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"fill", "user_orders", "market_positions", "order_group_updates"})}}));
  for (int sid = 1; sid <= 4; ++sid) {
    auto m = ws->next();
    CHECK(m && (*m)["type"] == "subscribed" && (*m)["msg"]["sid"] == sid);
  }
  HttpConn c(v.port());
  auto r = call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "PV-1"));
  const std::string oid = r.j.value("order_id", "");
  auto m = ws->next();
  CHECK(m && (*m)["type"] == "fill" && (*m)["sid"] == 1 && !m->contains("seq"));
  CHECK(m && (*m)["msg"]["order_id"] == oid && (*m)["msg"]["client_order_id"] == "PV-1" && (*m)["msg"]["is_taker"] == true &&
        (*m)["msg"]["count_fp"] == "1.00" && (*m)["msg"]["fee_cost"] == "0.017100" && (*m)["msg"]["exchange_index"] == 0);
  m = ws->next();
  CHECK(m && (*m)["type"] == "user_order" && (*m)["sid"] == 2 && !m->contains("seq") && (*m)["msg"]["status"] == "executed");
  m = ws->next();
  CHECK(m && (*m)["type"] == "market_position" && (*m)["sid"] == 3 && (*m)["msg"]["position_fp"] == "1.00");
  // a resting order the street trades with: a maker fill
  r = call(c, "POST", ORDERS, post_only(T, "3", "0.39", "PV-2"));
  const std::string oid2 = r.j.value("order_id", "");
  m = ws->next();
  CHECK(m && (*m)["type"] == "user_order" && (*m)["msg"]["status"] == "resting" && (*m)["msg"]["remaining_count_fp"] == "3.00" &&
        !(*m)["msg"].contains("last_update_reason"));
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_take(T, Side::ASK, 3900, 600, now); });  // 5 at 0.40 first
  m = ws->next();
  CHECK(m && (*m)["type"] == "fill" && (*m)["msg"]["order_id"] == oid2 && (*m)["msg"]["is_taker"] == false &&
        (*m)["msg"]["count_fp"] == "1.00" && (*m)["msg"]["yes_price_dollars"] == "0.3900");
  m = ws->next();
  CHECK(m && (*m)["type"] == "user_order" && (*m)["msg"]["fill_count_fp"] == "1.00" &&
        (*m)["msg"]["remaining_count_fp"] == "2.00" && (*m)["msg"]["last_update_reason"] == "Trade");
  m = ws->next();
  CHECK(m && (*m)["type"] == "market_position" && (*m)["msg"]["position_fp"] == "2.00");
  // order groups are numbered on their sid
  r = call(c, "POST", "/portfolio/order_groups/create", {{"contracts_limit", 5}});
  const std::string gid = r.j.value("order_group_id", "");
  m = ws->next();
  CHECK(m && (*m)["type"] == "order_group_updates" && (*m)["sid"] == 4 && (*m)["seq"] == 1 &&
        (*m)["msg"]["event_type"] == "created" && (*m)["msg"]["order_group_id"] == gid && (*m)["msg"]["contracts_limit_fp"] == "5.00");
  call(c, "PUT", "/portfolio/order_groups/" + gid + "/trigger");
  m = ws->next();
  CHECK(m && (*m)["seq"] == 2 && (*m)["msg"]["event_type"] == "triggered");
  call(c, "PUT", "/portfolio/order_groups/" + gid + "/reset");
  m = ws->next();
  CHECK(m && (*m)["seq"] == 3 && (*m)["msg"]["event_type"] == "reset");
  // no market data on private subscriptions
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(T, false, 3000, 100, now); });
  CHECK(!ws->next(200));
}

// ---- faults on REST (plan section 8) ---------------------------------------------------------------

void test_faults() {
  Venue v;
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"fill", "user_orders"})}}));
  ws->drain(200);
  using K = HttpFault::Kind;
  auto fault = [](K k) {
    HttpFault f;
    f.kind = k;
    return f;
  };
  auto rest3 = [](const std::string& cid) { return post_only(T, "3", "0.30", cid); };

  {  // 429: nothing processed, and the connection stays up
    HttpConn c(v.port());
    v.f.add_fault(fault(K::RATE_LIMIT));
    auto r = call(c, "POST", ORDERS, rest3("F-429"));
    CHECK(r.status() == 429 && r.j == json({{"error", "too many requests"}}) && !v.has_cid("F-429"));
    CHECK(call(c, "POST", ORDERS, rest3("F-429b")).status() == 201);
  }
  {  // dropped before the reply: nothing processed
    HttpConn c(v.port());
    v.f.add_fault(fault(K::DROP_BEFORE_REPLY));
    auto r = call(c, "POST", ORDERS, rest3("F-d1"));
    CHECK(r.dropped && r.status() == 0 && !v.has_cid("F-d1"));
  }
  {  // dropped after accepting: the order exists and the client heard nothing (UNKNOWN)
    HttpConn c(v.port());
    v.f.add_fault(fault(K::DROP_AFTER_ACCEPT));
    auto r = call(c, "POST", ORDERS, rest3("F-d2"));
    CHECK(r.dropped && v.has_cid("F-d2") && v.by_cid("F-d2")->status == OrdStatus::RESTING);
  }
  for (const int status : {500, 503}) {  // 5xx after accepting
    HttpConn c(v.port());
    HttpFault f = fault(K::STATUS_AFTER_ACCEPT);
    f.status = status;
    v.f.add_fault(f);
    const std::string cid = "F-" + std::to_string(status);
    auto r = call(c, "POST", ORDERS, rest3(cid));
    CHECK(r.status() == status && r.j["error"]["code"] == "internal_server_error" && v.has_cid(cid));
  }
  {  // a body cut mid-way: the head promised more
    HttpConn c(v.port());
    HttpFault f = fault(K::CUT_BODY);
    f.cut_bytes = 10;
    v.f.add_fault(f);
    auto r = call(c, "POST", ORDERS, rest3("F-cut"));
    CHECK(r.cut && r.status() == 201 && r.res.body.size() == 10 && r.j.is_discarded() && !r.res.complete);
    CHECK(std::stoul(r.res.headers["content-length"]) > 10 && v.has_cid("F-cut"));
    HttpConn c2(v.port());
    v.f.add_fault(fault(K::CUT_BODY));
    r = call(c2, "POST", ORDERS, rest3("F-cut2"));
    CHECK(r.cut && r.res.body.size() == std::stoul(r.res.headers["content-length"]) / 2);
  }
  {  // chunked
    HttpConn c(v.port());
    HttpFault f = fault(K::CHUNKED);
    f.chunk_bytes = 7;
    v.f.add_fault(f);
    v.f.clear_wire_log();
    auto r = call(c, "POST", ORDERS, rest3("F-ch"));
    CHECK(r.status() == 201 && r.res.chunked && r.res.complete && r.j["client_order_id"] == "F-ch");
    CHECK(log_at(v.f.wire_log(), {"http 201 POST", "(chunked)"}) >= 0);
    CHECK(call(c, "GET", "/portfolio/balance").status() == 200);  // still in step
  }
  {  // a 2 s stall before: the order is placed only after it
    HttpConn c(v.port());
    HttpFault f = fault(K::STALL_BEFORE);
    v.f.add_fault(f);
    const std::int64_t t = wall_ms();
    auto r = call(c, "POST", ORDERS, rest3("F-sb"));
    CHECK(r.status() == 201 && r.ms >= 1900);
    CHECK(v.by_cid("F-sb") && v.by_cid("F-sb")->created_ms >= t + 1900);
  }
  {  // a 2 s stall after: the order rests at once, user_orders says so, and a cancel on another
     // connection lands before the 201 (plan section 8: "cancel before the 201")
    HttpConn a(v.port()), b(v.port());
    v.f.add_fault(fault(K::STALL_AFTER));
    HttpResult ra;
    const auto t = std::chrono::steady_clock::now();
    std::thread th([&] { ra = call(a, "POST", ORDERS, rest3("F-sa")); });
    auto u = ws->next_where([](const json& m) { return m.value("type", "") == "user_order" && m["msg"]["client_order_id"] == "F-sa"; }, 1500);
    CHECK(u && (*u)["msg"]["status"] == "resting");
    const std::string id = u ? (*u)["msg"]["order_id"].get<std::string>() : "";
    auto rc = call(b, "DELETE", ORDERS + "/" + id + "?market_ticker=" + T);
    const double cancel_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    CHECK(rc.status() == 200 && rc.j["reduced_by"] == "3.00" && cancel_ms < 1500);
    th.join();
    CHECK(ra.status() == 201 && ra.j["order_id"] == id && ra.j["remaining_count"] == "3.00" && ra.ms >= 1900);
  }
  {  // Connection: close after the reply, and the server does close
    HttpFault f = fault(K::CLOSE_AFTER_REPLY);
    v.f.add_fault(f);
    const int fd = net::connect_loopback(v.port(), 2000);
    const std::string body = rest3("F-close").dump();
    std::string req = "POST " + V2 + ORDERS + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    for (const auto& [k, val] : kalshi_auth("POST", V2 + ORDERS, wall_ms())) req += k + ": " + val + "\r\n";
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    net::write_all(fd, req);
    std::string buf;
    net::Response res;
    net::read_response(fd, buf, res, 2000);
    CHECK(res.status == 201 && net::lower(res.headers["connection"]) == "close");
    pollfd p{fd, POLLIN, 0};
    char ch;
    CHECK(::poll(&p, 1, 1000) == 1 && ::recv(fd, &ch, 1, 0) == 0);
    ::close(fd);
  }
  ws->drain(100);
  {  // the fill on the socket before the 201's first byte
    HttpConn c(v.port());
    v.f.add_fault(fault(K::FILL_BEFORE_REPLY));
    v.f.clear_wire_log();
    auto r = call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "F-fbr"));
    CHECK(r.status() == 201 && r.j["fill_count"] == "1.00");
    const auto log = v.f.wire_log();
    const int fill = log_at(log, {" fill "}), reply = log_at(log, {"http 201 POST"});
    CHECK(fill >= 0 && reply > fill);
  }
  ws->drain(100);
  {  // the street hits the order once it rests: the fill is out before the 201, which says 0
    HttpConn c(v.port());
    HttpFault f = fault(K::HIT_AFTER_REST);
    f.qty = 100;
    v.f.add_fault(f);
    v.f.clear_wire_log();
    auto r = call(c, "POST", ORDERS, rest3("F-hit"));
    CHECK(r.status() == 201 && r.j["fill_count"] == "0.00" && r.j["remaining_count"] == "3.00");
    const auto log = v.f.wire_log();
    const int fill = log_at(log, {" fill "}), reply = log_at(log, {"http 201 POST"});
    CHECK(fill >= 0 && reply > fill);
    auto fm = next_type(*ws, "fill");
    CHECK(fm && (*fm)["msg"]["is_taker"] == false && (*fm)["msg"]["count_fp"] == "1.00" && (*fm)["msg"]["client_order_id"] == "F-hit");
    const json o = call(c, "GET", "/portfolio/orders/" + r.j.value("order_id", "")).j["order"];
    CHECK(o["fill_count_fp"] == "1.00" && o["remaining_count_fp"] == "2.00");
  }
  ws->drain(100);
  {  // a cancel racing a fill: the street takes 1 first; the reply says 2, and the fill follows it
    HttpConn c(v.port());
    const std::string id = call(c, "POST", ORDERS, rest3("F-race")).j.value("order_id", "");
    ws->drain(100);
    HttpFault f = fault(K::FILL_DURING_CANCEL);
    f.method = "DELETE";
    f.qty = 100;
    v.f.add_fault(f);
    v.f.clear_wire_log();
    auto r = call(c, "DELETE", ORDERS + "/" + id + "?market_ticker=" + T);
    CHECK(r.status() == 200 && r.j["reduced_by"] == "2.00");
    std::vector<json> msgs = ws->drain(200);
    const auto log = v.f.wire_log();
    const int reply = log_at(log, {"http 200 DELETE"}), fill = log_at(log, {" fill "});
    CHECK(reply >= 0 && fill > reply);
    // the order's own story stays in order: partly filled, then cancelled
    std::vector<std::string> story;
    for (const auto& x : msgs)
      if (x.value("type", "") == "user_order" && x["msg"]["order_id"] == id)
        story.push_back(x["msg"]["status"].get<std::string>() + " " + x["msg"]["fill_count_fp"].get<std::string>());
    CHECK(story == std::vector<std::string>({"resting 1.00", "canceled 1.00"}));
    // ... or, asked for, the fill goes first
    const std::string id2 = call(c, "POST", ORDERS, rest3("F-race2")).j.value("order_id", "");
    f.private_order = PrivateOrder::EVENTS_FIRST;
    v.f.add_fault(f);
    v.f.clear_wire_log();
    CHECK(call(c, "DELETE", ORDERS + "/" + id2 + "?market_ticker=" + T).j["reduced_by"] == "2.00");
    const auto log2 = v.f.wire_log();
    CHECK(log_at(log2, {" fill "}) >= 0 && log_at(log2, {"http 200 DELETE"}) > log_at(log2, {" fill "}));
  }
  ws->drain(100);
  {  // the whole venue answering before its socket speaks
    HttpConn c(v.port());
    v.f.set_private_order(PrivateOrder::REPLY_FIRST);
    v.f.clear_wire_log();
    CHECK(call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "F-rf")).j["fill_count"] == "1.00");
    CHECK(next_type(*ws, "fill"));
    const auto log = v.f.wire_log();
    CHECK(log_at(log, {"http 201 POST"}) >= 0 && log_at(log, {" fill "}) > log_at(log, {"http 201 POST"}));
    v.f.set_private_order(PrivateOrder::NATURAL);
  }
  {  // kept-open REST connections closed under their clients; the socket stays up
    HttpConn c(v.port());
    CHECK(call(c, "GET", "/portfolio/balance").status() == 200);
    v.f.drop_http_connections();
    auto r = call(c, "GET", "/portfolio/balance");
    CHECK(r.status() == 0 && r.rr == net::ReadResult::CLOSED);
    HttpConn fresh(v.port());
    CHECK(call(fresh, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "F-after")).status() == 201);
    auto fm = next_type(*ws, "fill");
    CHECK(fm && (*fm)["msg"]["client_order_id"] == "F-after");
  }
  CHECK(v.f.faults_pending() == 0);

  // an idle kept-open connection is closed by the venue (its load balancer: about 60 s, F10)
  FakeConfig idle = quiet();
  idle.idle_close_ms = 300;
  Venue vi(idle);
  HttpConn c(vi.port());
  CHECK(call(c, "GET", "/portfolio/balance").status() == 200);
  CHECK(!c.closed_by_server(100));
  CHECK(c.closed_by_server(1000));
}

// ---- faults on the socket --------------------------------------------------------------------------

void test_ws_faults() {
  Venue v;
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_tickers", json::array({T})}}));
  ws->send_json(cmd(2, "subscribe", {{"channels", json::array({"fill"})}}));
  ws->drain(200);
  // a lost message: its number is spent, so the reader sees the gap
  v.f.drop_ws_messages("orderbook_delta", 1);
  for (const Px px : {3100, 3200, 3300}) v.f.with_engine([px](Engine& e, std::int64_t now) { e.street_add(T, false, px, 100, now); });
  std::vector<std::int64_t> seqs;
  std::vector<std::string> prices;
  for (int i = 0; i < 2; ++i) {
    auto m = ws->next();
    if (!m) break;
    seqs.push_back((*m)["seq"]);
    prices.push_back((*m)["msg"]["price_dollars"]);
  }
  CHECK(seqs == std::vector<std::int64_t>({3, 4}) && prices == std::vector<std::string>({"0.3200", "0.3300"}));
  CHECK(log_at(v.f.wire_log(), {"lost orderbook_delta"}) >= 0);
  // a fill message sent twice: the same trade_id
  v.f.duplicate_ws_messages("fill", 1);
  HttpConn c(v.port());
  call(c, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", "WF-1"));
  std::vector<std::string> trades;
  for (const auto& m : ws->drain(200))
    if (m.value("type", "") == "fill") trades.push_back(m["msg"]["trade_id"]);
  CHECK(trades.size() == 2 && trades[0] == trades[1]);
  // a message split into frames of 64 bytes, put back together
  v.f.fragment_ws(64);
  ws->send_json(cmd(3, "update_subscription", {{"sid", 1}, {"action", "get_snapshot"}}));
  CHECK(next_type(*ws, "ok"));
  auto snap = next_type(*ws, "orderbook_snapshot");
  CHECK(snap && ws->last().frames > 1 && ws->last().max_frame <= 64);
  CHECK(snap && (*snap)["msg"]["yes_dollars_fp"].size() == 4);  // 0.31 (its delta lost, not its level), 0.32, 0.33, 0.40
  v.f.fragment_ws(0);
  // frames past 8,192 and 65,535 bytes arrive whole, both ways (plan section 8: the old desk lost
  // frames cut at 8,192 and 4,096 bytes): an update naming n markets, and the ok listing them all
  for (const int n : {400, 2600}) {
    json tickers = json::array();
    for (int i = 0; i < n; ++i) {
      char t[32];
      std::snprintf(t, sizeof t, "KXBIG-26OCT06-%012d", i);
      tickers.push_back(t);
    }
    ws->send_json(cmd(4, "update_subscription", {{"sid", 1}, {"market_tickers", tickers}, {"action", "add_markets"}}));
    auto ok = next_type(*ws, "ok", 5000);
    CHECK(ok && (*ok)["msg"]["market_tickers"].size() == static_cast<std::size_t>(n + 1));
    CHECK(ws->last().payload.size() > (n == 400 ? 8192u : 65535u) && ws->last().frames == 1);
    std::size_t snaps = 0;
    for (const auto& m : ws->drain(300)) snaps += m.value("type", "") == "orderbook_snapshot";
    CHECK(snaps == static_cast<std::size_t>(n));
    ws->send_json(cmd(5, "update_subscription", {{"sid", 1}, {"market_tickers", tickers}, {"action", "delete_markets"}}));
    CHECK(next_type(*ws, "ok", 5000));
  }
  // a stalled socket: the message comes late, not lost
  v.f.stall_ws(400);
  const auto t = std::chrono::steady_clock::now();
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(T, false, 3400, 100, now); });
  auto late = ws->next(2000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  CHECK(late && (*late)["msg"]["price_dollars"] == "0.3400" && ms >= 350);
  // a dropped socket: no close frame; a new connection starts again at sid 1, seq 1
  v.f.drop_ws_connections();
  CHECK(!ws->next(1000) && ws->closed() && !ws->close_frame_seen());
  for (int i = 0; i < 100 && v.f.ws_connections() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(v.f.ws_connections() == 0);
  auto ws2 = open_ws(v.port());
  ws2->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_ticker", T}}));
  auto m = ws2->next();
  CHECK(m && (*m)["msg"]["sid"] == 1);
  m = ws2->next();
  CHECK(m && (*m)["type"] == "orderbook_snapshot" && (*m)["seq"] == 1);

  // the venue's own pings: answered by the client, and the socket goes on
  FakeConfig pings = quiet();
  pings.ws_ping_ms = 100;
  Venue vp(pings);
  auto wp = open_ws(vp.port());
  CHECK(!wp->next(450) && wp->pings_seen() >= 3);
  wp->send_json(cmd(1, "subscribe", {{"channels", json::array({"ticker"})}}));
  CHECK(next_type(*wp, "subscribed"));
}

// ---- review additions (2026-10-07) ----------------------------------------------------------------

// "Newly placed orders may also be cancelled during the minute after the request": on by default
// now. An order placed 1 s after a cancel-all is acknowledged (201, resting) and then cancelled;
// one placed 61 s after rests. KILL's re-arm therefore waits 60 s from the cancel-all's return,
// not from the click (gw/kill.h cancel_all_done).
void test_cancel_all_echo() {
  FakeConfig cfg = quiet();
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  Venue v(cfg);
  CHECK(v.f.with_engine([](Engine& e, std::int64_t) { return e.config().cancel_all_echo_ms; }) == 60000);
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"user_orders"})}}));
  CHECK(next_type(*ws, "subscribed"));
  HttpConn c(v.port());
  auto r = call(c, "POST", ORDERS, post_only(T, "1", "0.30", "EC-0"));
  CHECK(r.status() == 201);
  r = call(c, "DELETE", ORDERS);   // KILL's cancel-all
  CHECK(r.status() == 204);
  CHECK(v.by_cid("EC-0") && v.by_cid("EC-0")->status == OrdStatus::CANCELED);
  ws->drain(150);
  v.f.advance_ms(1000);   // an order the moment a re-arm timed from the click would allow it
  r = call(c, "POST", ORDERS, post_only(T, "1", "0.30", "EC-1"));
  CHECK(r.status() == 201 && r.j["remaining_count"] == "1.00");   // placed, says the 201 ...
  const std::string id1 = r.j.value("order_id", "");
  CHECK(call(c, "GET", "/portfolio/orders/" + id1).j["order"]["status"] == "canceled");   // ... then cancelled
  std::vector<std::string> st;
  for (const auto& m : ws->drain(200))
    if (m.value("type", "") == "user_order" && m["msg"]["order_id"] == id1) st.push_back(m["msg"]["status"]);
  CHECK(!st.empty() && st.back() == "canceled");
  v.f.advance_ms(60001);   // a minute after the cancel-all
  r = call(c, "POST", ORDERS, post_only(T, "1", "0.30", "EC-2"));
  CHECK(r.status() == 201);
  CHECK(call(c, "GET", "/portfolio/orders/" + r.j.value("order_id", "")).j["order"]["status"] == "resting");
}

// Errors 25 to 27: a subscription whose reader stopped reading is dropped with 25; a
// subscription past the market limit is refused 26, on subscribe and on add_markets; commands
// past the rate are refused 27.
void test_ws_limits() {
  {
    FakeConfig cfg = quiet();
    cfg.ws_sub_buffer = 50;
    Venue v(cfg);
    auto ws = open_ws(v.port());
    ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_ticker", T}}));
    ws->send_json(cmd(2, "subscribe", {{"channels", json::array({"ticker"})}, {"market_ticker", T}}));
    ws->drain(200);
    v.f.stall_ws(500);   // the socket stops moving: messages pile up unwritten
    for (int i = 0; i < 80; ++i)
      v.f.with_engine([i](Engine& e, std::int64_t now) { e.street_add(T, false, 1000 + 10 * i, 1, now); });
    std::vector<json> got = ws->drain(800);
    int deltas = 0;
    std::optional<json> err;
    for (const auto& m : got) {
      if (m.value("type", "") == "orderbook_delta") ++deltas;
      if (m.value("type", "") == "error") err = m;
    }
    CHECK(deltas == 50);
    CHECK(err && (*err)["msg"]["code"] == 25 && (*err)["sid"] == 1 && !err->contains("id"));
    CHECK(err && (*err)["msg"]["msg"] == "Subscription buffer overflow");
    CHECK(log_at(v.f.wire_log(), {"error 25 1"}) >= 0);
    // the subscription is gone; the other one on the connection goes on
    v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(T, false, 2500, 1, now); });
    auto after = ws->drain(300);
    for (const auto& m : after) CHECK(m.value("type", "") != "orderbook_delta");
    ws->send_json(cmd(3, "list_subscriptions"));
    auto list = next_type(*ws, "ok");
    CHECK(list && (*list)["msg"].size() == 1 && (*list)["msg"][0]["sid"] == 2);
    ws->send_json(cmd(4, "update_subscription", {{"sid", 1}, {"action", "get_snapshot"}}));
    auto e7 = next_type(*ws, "error");
    CHECK(e7 && (*e7)["msg"]["code"] == 7);
  }
  {
    FakeConfig cfg = quiet();
    cfg.ws_sub_market_limit = 3;
    Venue v(cfg);
    auto ws = open_ws(v.port());
    const json four = json::array({"KXA-1", "KXA-2", "KXA-3", "KXA-4"});
    ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"ticker"})}, {"market_tickers", four}}));
    auto e = next_type(*ws, "error");
    CHECK(e && (*e)["msg"]["code"] == 26 && (*e)["id"] == 1 && (*e)["msg"]["msg"] == "Subscription market limit exceeded");
    ws->send_json(cmd(2, "subscribe", {{"channels", json::array({"ticker"})}, {"market_tickers", json::array({"KXA-1", "KXA-2", "KXA-3"})}}));
    auto sub = next_type(*ws, "subscribed");
    CHECK(sub && (*sub)["msg"]["sid"] == 1);
    ws->send_json(cmd(3, "update_subscription", {{"sid", 1}, {"action", "add_markets"}, {"market_tickers", json::array({"KXA-2"})}}));
    CHECK(next_type(*ws, "ok"));   // already there: still 3
    ws->send_json(cmd(4, "update_subscription", {{"sid", 1}, {"action", "add_markets"}, {"market_tickers", json::array({"KXA-4"})}}));
    e = next_type(*ws, "error");
    CHECK(e && (*e)["msg"]["code"] == 26 && (*e)["sid"] == 1);
  }
  {
    FakeConfig cfg = quiet();
    cfg.ws_cmds_per_s = 5;
    cfg.manual_clock = true;
    cfg.start_ms = T0;
    Venue v(cfg);
    auto ws = open_ws(v.port());
    for (int i = 1; i <= 6; ++i) ws->send_json(cmd(i, "list_subscriptions"));
    int oks = 0;
    std::optional<json> e;
    for (const auto& m : ws->drain(300)) {
      oks += m.value("type", "") == "ok";
      if (m.value("type", "") == "error") e = m;
    }
    CHECK(oks == 5);
    CHECK(e && (*e)["msg"]["code"] == 27 && (*e)["id"] == 6 && (*e)["msg"]["msg"] == "Too many requests");
    v.f.advance_ms(1000);
    ws->send_json(cmd(7, "list_subscriptions"));
    CHECK(next_type(*ws, "ok"));
  }
}

// One order's private messages leave in the engine's order even when its create's reply is slow
// (REPLY_FIRST holds them) and a cancel on another connection is answered first. Until 2026-10-07
// the cancel's "canceled" could go out before the create's held "resting".
void test_private_order_per_order() {
  FakeConfig cfg = quiet();
  cfg.private_order = PrivateOrder::REPLY_FIRST;
  Venue v(cfg);
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"user_orders"})}}));
  CHECK(next_type(*ws, "subscribed"));
  HttpFault slow;
  slow.kind = HttpFault::Kind::STALL_AFTER;
  slow.method = "POST";
  slow.path = ORDERS;
  slow.stall_ms = 600;
  v.f.add_fault(slow);
  std::thread create([&v] {
    HttpConn a(v.port());
    auto r = call(a, "POST", ORDERS, post_only(T, "1", "0.30", "PO-1"));
    CHECK(r.status() == 201);
  });
  std::string id;
  for (int i = 0; i < 200 && id.empty(); ++i) {
    if (auto o = v.by_cid("PO-1")) id = o->order_id;
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(!id.empty());
  HttpConn b(v.port());
  const auto rb = call(b, "DELETE", ORDERS + "/" + id);   // answered while the create's reply stalls
  CHECK(rb.status() == 200);
  create.join();
  std::vector<std::string> st;
  for (const auto& m : ws->drain(300))
    if (m.value("type", "") == "user_order" && m["msg"]["order_id"] == id) st.push_back(m["msg"]["status"]);
  CHECK((st == std::vector<std::string>{"resting", "canceled"}));
  // and the wire log has the cancel's reply before either message: the messages waited
  const auto log = v.f.wire_log();
  const int del = log_at(log, {"http 200 DELETE"}), resting = log_at(log, {"user_order " + id});
  CHECK(del >= 0 && resting > del);
}

// ---- the recorded tape over the socket -------------------------------------------------------------

// The fixture played to its end on the manual clock, as fast as the engine goes: a client keeping
// books from snapshots and deltas, as deskd's md-k will, sees no seq gap, window 1's close as an
// empty snapshot, and window 2 end as the generator's own book says. (On the wall clock window 2's
// own close would follow the tape's end by 300 s of tape, 0.5 s at 600x: too close to race.)
void test_tape_over_wire() {
  const std::string fixture = std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.csv.gz";
  std::ifstream ein(std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.expect.json");
  std::stringstream ess;
  ess << ein.rdbuf();
  const json expect = json::parse(ess.str(), nullptr, false);
  const std::string w1 = "KXNATGAS15M-26OCT061145-45", w2 = "KXNATGAS15M-26OCT061200-00";
  FakeConfig cfg = quiet();
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  Venue v(cfg, false);
  Tape tape;
  std::string err;
  CHECK(load_archive_csv(fixture, tape, &err));
  const std::int64_t span = tape.t1() - tape.t0();
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_tickers", json::array({w1, w2})}}));
  CHECK(next_type(*ws, "subscribed"));
  v.f.play(std::move(tape), 1.0);
  // the socket's writer sends while the clock runs: the reader below takes what has queued
  for (std::int64_t t = 0; t <= span; t += 1000) v.f.advance_ms(1000);
  CHECK(v.f.tape_done());
  std::map<std::string, std::map<std::pair<bool, std::string>, std::int64_t>> book;  // ticker -> (no?, px) -> qty
  std::int64_t last_seq = 0;
  int gaps = 0, deltas = 0, empty_snaps = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    auto m = ws->next(300);
    if (!m) {
      if (v.f.wait_ws_flushed(500)) break;
      continue;
    }
    const std::string type = m->value("type", "");
    if (type != "orderbook_snapshot" && type != "orderbook_delta") continue;
    const std::int64_t seq = (*m)["seq"];
    gaps += seq != last_seq + 1;
    last_seq = seq;
    const json& msg = (*m)["msg"];
    const std::string t = msg["market_ticker"];
    if (type == "orderbook_snapshot") {
      book[t].clear();
      empty_snaps += !msg.contains("yes_dollars_fp") && !msg.contains("no_dollars_fp");
      for (const bool no : {false, true})
        for (const auto& lv : msg.value(no ? "no_dollars_fp" : "yes_dollars_fp", json::array()))
          book[t][{no, lv[0].get<std::string>()}] = *parse_fixed(lv[1].get<std::string>(), 2, false);
    } else {
      ++deltas;
      auto& q = book[t][{msg["side"] == "no", msg["price_dollars"].get<std::string>()}];
      q += *parse_fixed(msg["delta_fp"].get<std::string>(), 2, true);
    }
  }
  CHECK(gaps == 0 && deltas > 4000);
  CHECK(empty_snaps == 3);  // the two not-yet-listed windows at subscribe, then window 1's close
  for (auto it = book[w2].begin(); it != book[w2].end();) it = it->second == 0 ? book[w2].erase(it) : std::next(it);
  std::map<std::pair<bool, std::string>, std::int64_t> want;
  for (const bool no : {false, true})
    for (const auto& lv : expect["final"][w2][no ? "no" : "yes"])
      want[{no, lv[0].get<std::string>()}] = *parse_fixed(lv[1].get<std::string>(), 2, false);
  CHECK(book[w2] == want);
  CHECK(book[w1].empty());
}

// ---- many clients at once ---------------------------------------------------------------------------

// Eight clients place, take and cancel at once while the tape plays and a socket listens, half
// their writes on fresh connections as deskd's one-write-per-connection sender will make them.
void test_concurrency() {
  Venue v;
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(T, true, 5800, 100000, now); });
  Tape tape;
  std::string err;
  CHECK(load_archive_csv(std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.csv.gz", tape, &err));
  v.f.play(std::move(tape), 300.0);
  auto ws = open_ws(v.port());
  ws->send_json(cmd(1, "subscribe", {{"channels", json::array({"fill"})}}));
  ws->send_json(cmd(2, "subscribe", {{"channels", json::array({"orderbook_delta"})}, {"market_ticker", T}}));
  ws->drain(200);
  std::atomic<int> created{0}, filled{0}, cancelled{0}, failures{0};
  std::mutex mu;
  std::set<std::string> ids;
  std::vector<std::thread> th;
  const auto t = std::chrono::steady_clock::now();
  for (int k = 0; k < 8; ++k)
    th.emplace_back([&, k] {
      HttpConn keep(v.port());
      std::string last;
      for (int i = 0; i < 40; ++i) {
        const std::string cid = "C-" + std::to_string(k) + "-" + std::to_string(i);
        if (i % 3 == 0) {
          auto r = call(keep, "POST", ORDERS, post_only(T, "1", "0.30", cid));
          if (r.status() != 201) {
            ++failures;
            continue;
          }
          ++created;
          last = r.j["order_id"];
          std::lock_guard<std::mutex> lk(mu);
          ids.insert(last);
        } else if (i % 3 == 1) {
          HttpConn fresh(v.port());
          auto r = call(fresh, "POST", ORDERS, order(T, "bid", "1", "0.42", "immediate_or_cancel", cid));
          if (r.status() != 201 || r.j["fill_count"] != "1.00") {
            ++failures;
            continue;
          }
          ++created;
          ++filled;
          std::lock_guard<std::mutex> lk(mu);
          ids.insert(r.j["order_id"].get<std::string>());
        } else {
          auto r = call(keep, "DELETE", ORDERS + "/" + last + "?market_ticker=" + T);
          if (r.status() == 200)
            ++cancelled;
          else
            ++failures;
        }
      }
    });
  for (auto& x : th) x.join();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  std::printf("  8 clients, 320 writes (104 on fresh connections) in %.0f ms\n", ms);
  CHECK(failures == 0 && created == 8 * 27 && filled == 8 * 13 && cancelled == 8 * 13);
  CHECK(ids.size() == static_cast<std::size_t>(created.load()));
  CHECK(v.f.with_engine([](Engine& e, std::int64_t) { return e.fills().size(); }) == 104u);
  CHECK(v.f.wait_ws_flushed(3000));
  int fills = 0, gaps = 0;
  std::int64_t last_seq = 0;
  for (const auto& m : ws->drain(300)) {
    if (m.value("type", "") == "fill") ++fills;
    if (m.value("sid", 0) == 2) {
      gaps += m["seq"].get<std::int64_t>() != last_seq + 1 && last_seq != 0;
      last_seq = m["seq"];
    }
  }
  CHECK(fills == 104 && gaps == 0);
  CHECK(v.f.connections_accepted() >= 8 + 104 + 1);
}

// ---- the binary ------------------------------------------------------------------------------------

int spawn(const std::vector<std::string>& args, int* out_fd) {
  int p[2];
  if (::pipe(p) != 0) return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, p[1], 1);
  posix_spawn_file_actions_adddup2(&fa, p[1], 2);
  posix_spawn_file_actions_addclose(&fa, p[0]);
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, args[0].c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(p[1]);
  if (rc != 0) {
    ::close(p[0]);
    return -1;
  }
  *out_fd = p[0];
  return pid;
}

std::string read_until(int fd, const std::string& needle, int timeout_ms) {
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (out.find(needle) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) continue;
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

void test_binary() {
  TempDir dir;
  {
    std::ofstream s(dir.file("scenario.jsonl"));
    s << R"({"t":0,"market":{"ticker":"KXTEST-26OCT06-A","close_in_ms":600000}})" << "\n"
      << R"({"t":0,"snapshot":{"ticker":"KXTEST-26OCT06-A","yes":[["0.4000","10.00"]],"no":[["0.5800","5.00"]]}})" << "\n";
  }
  int out = -1;
  const int pid = spawn({FAKE_KALSHI_BIN, "--port", "0", "--scenario", dir.file("scenario.jsonl"), "--no-rate-limits"}, &out);
  CHECK(pid > 0);
  if (pid <= 0) return;
  const std::string head = read_until(out, "WS   ws://", 5000);
  const auto at = head.find("REST http://127.0.0.1:");
  CHECK(at != std::string::npos);
  const int port = at == std::string::npos ? 0 : std::atoi(head.c_str() + at + 22);
  CHECK(port > 0 && head.find("/trade-api/ws/v2") != std::string::npos);
  HttpConn c(port);
  CHECK(c.request("GET", V2 + "/exchange/status").status() == 200);
  auto r = c.request("GET", V2 + "/markets/" + T + "/orderbook");
  CHECK(r.status() == 200 && r.j["orderbook_fp"]["yes_dollars"] == json::parse(R"([["0.4000","10.00"]])"));
  r = call(c, "GET", "/portfolio/balance");
  CHECK(r.status() == 200 && r.j["balance"] == 20000);  // $100 on shards 0 and 2 by default
  c.close();
  ::kill(pid, SIGTERM);
  int st = 0;
  ::waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(read_until(out, "stopped after", 2000).find("stopped after") != std::string::npos);
  ::close(out);
  const int bad = spawn({FAKE_KALSHI_BIN, "--bogus"}, &out);
  CHECK(read_until(out, "usage: fake-kalshi", 2000).find("usage: fake-kalshi") != std::string::npos);
  ::waitpid(bad, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
  ::close(out);
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_rest();
  test_auth();
  test_rate_limits();
  test_ws_protocol();
  test_ws_private();
  test_faults();
  test_ws_faults();
  test_cancel_all_echo();
  test_ws_limits();
  test_private_order_per_order();
  test_tape_over_wire();
  test_concurrency();
  test_binary();
  return finish("deskd-fake-wire");
}
