// The margined instance: parsers against the shapes the exchange documents, the rails on every
// ticket, cancel/close/transfer bodies, the account sync (orders, fills, positions), and the
// basis rule in paper and live mode through a mock margin exchange.
// Built and run with `ninja -C build-ci && ./build-ci/perps-tests`.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <httplib.h>

#include "desk/auth.h"
#include "desk/client.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/perps.h"
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

// A market row as /margin/markets returns it (sampled live on 2026-09-08).
static json market_json(const std::string& ticker, double bid, double ask, double reference, double mark = 0,
                        double size = 0.0001, long long ref_ts_ms = 0) {
  if (mark <= 0) mark = 0.5 * (bid + ask);
  if (ref_ts_ms <= 0) ref_ts_ms = now_s() * 1000;
  return json{{"ticker", ticker}, {"title", ticker + " perpetual"}, {"status", "active"}, {"asset_class", "crypto"},
              {"tick_size", "0.0001"}, {"contract_size", fmt_fp(size).size() ? "0.000100" : "0.000100"},
              {"fractional_trading_enabled", false}, {"leverage_estimate", 6.05}, {"price", fmt_dollars(bid)},
              {"bid", fmt_dollars(bid)}, {"ask", fmt_dollars(ask)}, {"volume_24h", "56007883.00"}, {"open_interest", "1000.00"},
              {"settlement_mark_price", {{"price", fmt_dollars(mark)}, {"ts_ms", ref_ts_ms}}},
              {"liquidation_mark_price", {{"price", fmt_dollars(mark)}, {"ts_ms", ref_ts_ms}}},
              {"reference_price", {{"price", fmt_dollars(reference)}, {"ts_ms", ref_ts_ms}}},
              {"schedule", {{"next_close_ts", "2099-01-01T00:00:00Z"}}}, {"exchange_index", 2}};
}

static void test_parsers() {
  json mj = market_json("KXBTCPERP", 7.8522, 7.8526, 7.8507);
  PerpMarket m = parse_perp_market(mj);
  CHECK(m.ticker == "KXBTCPERP" && m.status == "active" && m.is_open && !m.fractional);
  CHECK_NEAR(m.bid, 7.8522, 1e-9);
  CHECK_NEAR(m.ask, 7.8526, 1e-9);
  CHECK_NEAR(m.reference, 7.8507, 1e-9);
  CHECK_NEAR(m.mid(), 7.8524, 1e-9);
  CHECK_NEAR(m.basis_bps(), (7.8524 - 7.8507) / 7.8507 * 1e4, 1e-6);   // ~2.2 bps rich
  CHECK_NEAR(m.tick, 0.0001, 1e-12);
  CHECK(m.mark_ts_ms > 0 && m.reference_ts_ms > 0);
  // The book carries no venue clock, so the parser stamps OUR receive time. This is the field the
  // maker entry gates on: a post-only price built from a stale snapshot is what the exchange
  // refuses as "post only cross", and the same stale bid/ask also feed mid() and therefore the
  // basis the entry was decided on. Named for what it is - when it reached us - not when it was true.
  CHECK(m.quote_received_ts_ms > 0);
  CHECK(m.quote_received_ts_ms >= now_ms() - 5000 && m.quote_received_ts_ms <= now_ms() + 1000);
  // The venue sends no book timestamp, so the stamp must NOT come from the payload: a market JSON
  // with no timestamps at all still has to produce a usable receive time.
  json bare = mj;
  bare.erase("settlement_mark_price"); bare.erase("reference_price");
  CHECK(parse_perp_market(bare).quote_received_ts_ms > 0);
  // No reference: no basis, never a trade.
  json nj = mj; nj.erase("reference_price");
  CHECK(parse_perp_market(nj).basis_bps() == 0);
  // The book: the exchange lists from the touch outward; the desk keeps the touch last.
  json bj = {{"orderbook", {{"bids", json::array({{"7.8522", "12.00"}, {"7.8520", "30.00"}, {"7.8510", "5.00"}})},
                            {"asks", json::array({{"7.8526", "8.00"}, {"7.8530", "20.00"}})}}}};
  PerpBook b = parse_perp_book(bj, "KXBTCPERP");
  CHECK(b.bids.size() == 3 && b.asks.size() == 2);
  CHECK_NEAR(b.best_bid(), 7.8522, 1e-9);
  CHECK_NEAR(b.bid_size(), 12.0, 1e-9);
  CHECK_NEAR(b.best_ask(), 7.8526, 1e-9);
  CHECK_NEAR(b.ask_size(), 8.0, 1e-9);
  CHECK_NEAR(b.bids.front().price, 7.8510, 1e-9);
  // Balance: per-subaccount equity summed; settled funds at the top level.
  json bal = {{"settled_funds", "25.1200"},
              {"subaccount_balances", json::array({{{"subaccount", 0}, {"position_value", "1.5000"}, {"account_equity", "26.6200"},
                                                     {"maintenance_margin", "0.4000"}, {"initial_margin", "0.8000"},
                                                     {"resting_orders_margin", "0.1000"}, {"available_balance", "24.2000"}}})}};
  PerpBalance pb = parse_perp_balance(bal);
  CHECK_NEAR(pb.settled_funds, 25.12, 1e-9);
  CHECK_NEAR(pb.equity, 26.62, 1e-9);
  CHECK_NEAR(pb.available, 24.20, 1e-9);
  CHECK_NEAR(pb.resting_margin, 0.10, 1e-9);
  // A self-clearing member reports 0 equity: settled + position value is the fallback.
  bal["subaccount_balances"][0]["account_equity"] = "0.0000";
  CHECK_NEAR(parse_perp_balance(bal).equity, 26.62, 1e-9);
  // Position, fill, order.
  PerpPosition p = parse_perp_position(json{{"subaccount", 0}, {"market_ticker", "KXBTCPERP"}, {"position", "-2.00"},
                                            {"entry_price", "7.8600"}, {"unrealized_pnl", "0.0150"}, {"margin_used", "2.6000"},
                                            {"fees", "0.0094"}, {"is_portfolio", false}});
  CHECK(p.ticker == "KXBTCPERP" && p.position == -2 && std::fabs(p.entry_price - 7.86) < 1e-9 && std::fabs(p.unrealized_pnl - 0.015) < 1e-9);
  PerpFill f = parse_perp_fill(json{{"fill_id", "f1"}, {"order_id", "o1"}, {"ticker", "KXBTCPERP"}, {"side", "ask"}, {"count", "1.00"},
                                    {"price", "7.8600"}, {"entry_price", "7.8600"}, {"fees", "0.0039"}, {"realized_pnl", "0.0000"},
                                    {"is_taker", false}, {"created_time", "2026-09-08T20:00:00Z"}, {"order_source", "user"}});
  CHECK(f.fill_id == "f1" && f.side == "ask" && f.count == 1 && !f.is_taker && f.ts == parse_iso("2026-09-08T20:00:00Z"));
  PerpOrder o = parse_perp_order(json{{"order_id", "o1"}, {"client_order_id", "c1"}, {"ticker", "KXBTCPERP"}, {"side", "bid"},
                                      {"price", "7.8500"}, {"fill_count", "0.00"}, {"remaining_count", "1.00"},
                                      {"last_update_reason", ""}, {"created_time", "2026-09-08T20:00:00Z"}});
  CHECK(o.order_id == "o1" && o.side == "bid" && o.remaining_count == 1 && o.fill_count == 0);
  // The order body the exchange documents: fixed-point strings, STP, subaccount 0.
  PerpOrderRequest r;
  r.ticker = "KXBTCPERP"; r.side = "ask"; r.count = 1; r.price = 7.8526; r.post_only = true; r.client_order_id = "cid-1";
  json body = perp_order_body(r);
  CHECK(body["ticker"] == "KXBTCPERP" && body["side"] == "ask" && body["count"] == "1.00" && body["price"] == "7.8526");
  CHECK(body["time_in_force"] == "good_till_canceled" && body["post_only"] == true && body["self_trade_prevention_type"] == "taker_at_cross");
  CHECK(body["subaccount"] == 0 && body["client_order_id"] == "cid-1" && !body.contains("reduce_only"));
  r.reduce_only = true; r.tif = "immediate_or_cancel"; r.post_only = false;
  body = perp_order_body(r);
  CHECK(body["reduce_only"] == true && body["time_in_force"] == "immediate_or_cancel" && body["post_only"] == false);
}

static void test_config() {
  PerpsConfig d = perps_config(json::object());
  CHECK(d.enabled && d.max_order_notional == 30 && d.max_total_notional == 60 && d.max_leverage == 2 && d.price_band_pct == 2);
  CHECK(d.algo_markets.size() == 2 && d.algo_markets[0] == "KXBTCPERP");
  PerpsConfig c = perps_config(json{{"enabled", false}, {"max_order_notional", 12.5}, {"watch", json::array({"KXETHPERP"})},
                                    {"algo", {{"entry_bps", 40}, {"markets", json::array({"KXSOLPERP"})}}}});
  CHECK(!c.enabled && c.max_order_notional == 12.5 && c.watch.size() == 1 && c.watch[0] == "KXETHPERP");
  CHECK(c.algo_entry_bps == 40 && c.algo_markets.size() == 1 && c.algo_markets[0] == "KXSOLPERP" && c.algo_exit_bps == 5);
}

// ---- a mock margined instance ------------------------------------------------------------------

struct MockMargin {
  httplib::Server server;
  std::thread thread;
  int port = 0;
  std::mutex mu;
  std::atomic<int> writes{0}, cancels{0}, transfers{0}, balance_reads{0};
  std::atomic<bool> reject{false};
  json last_order, last_transfer, last_cancel_query;
  std::map<std::string, json> markets;            // ticker -> /margin/markets row
  json positions = json::array(), resting = json::array(), fills = json::array(), risk = json::array();
  double settled = 25.0, equity = 25.0, available = 24.0;
  double fill_fraction = 0.0;                     // of a new order, filled at once
  json book = {{"bids", json::array({{"7.8522", "12.00"}, {"7.8520", "30.00"}})}, {"asks", json::array({{"7.8526", "8.00"}, {"7.8530", "20.00"}})}};
  MockMargin() {
    server.Get("/trade-api/v2/margin/markets", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      json arr = json::array();
      for (const auto& [t, m] : markets) arr.push_back(m);
      r.set_content(json{{"markets", arr}}.dump(), "application/json");
    });
    server.Get(R"(/trade-api/v2/margin/markets/([^/]+)/orderbook)", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"orderbook", book}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/funding_rates/estimate", [&](const httplib::Request& q, httplib::Response& r) {
      r.set_content(json{{"market_ticker", q.get_param_value("ticker")}, {"funding_rate", 0.00016}, {"mark_price", "7.8524"},
                         {"computed_time", now_iso()}, {"next_funding_time", "2026-09-09T00:00:00Z"}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/balance", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++balance_reads;
      r.set_content(json{{"settled_funds", fmt_dollars(settled)},
                         {"subaccount_balances", json::array({{{"subaccount", 0}, {"position_value", "0.0000"}, {"account_equity", fmt_dollars(equity)},
                                                               {"maintenance_margin", "0.0000"}, {"initial_margin", "0.0000"},
                                                               {"resting_orders_margin", "0.0000"}, {"available_balance", fmt_dollars(available)}}})}}.dump(),
                    "application/json");
    });
    server.Get("/trade-api/v2/margin/positions", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"positions", positions}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/risk", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"account_leverage", 1.5}, {"total_position_notional", "7.8600"}, {"total_maintenance_margin", "1.3000"}, {"positions", risk}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/orders", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"orders", resting}, {"cursor", ""}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/fills", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"fills", fills}, {"cursor", ""}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/margin/fee_tiers", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(json{{"maker_fee_rates", {{"KXBTCPERP", 0.0005}, {"KXETHPERP", 0.0005}}},
                         {"taker_fee_rates", {{"KXBTCPERP", 0.0012}, {"KXETHPERP", 0.0012}}}}.dump(), "application/json");
    });
    server.Post("/trade-api/v2/margin/orders", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++writes;
      if (reject) {
        r.status = 400;
        r.set_content(R"({"error":{"code":"insufficient_margin","message":"rejected by test exchange"}})", "application/json");
        return;
      }
      last_order = json::parse(q.body);
      double count = parse_num(last_order.value("count", "0"));
      double filled = std::floor(count * fill_fraction * 100 + 1e-9) / 100;
      json resp{{"order_id", "po-" + std::to_string(writes.load())}, {"client_order_id", last_order.value("client_order_id", "")},
                {"fill_count", fmt_fp(filled)}, {"remaining_count", fmt_fp(count - filled)}};
      if (filled > 0) { resp["average_fill_price"] = last_order.value("price", "0"); resp["average_fee_paid"] = "0.0039"; }
      if (count - filled > 0)
        resting.push_back({{"order_id", resp["order_id"]}, {"client_order_id", resp["client_order_id"]}, {"ticker", last_order["ticker"]},
                           {"side", last_order["side"]}, {"price", last_order["price"]}, {"fill_count", fmt_fp(filled)},
                           {"remaining_count", fmt_fp(count - filled)}, {"last_update_reason", ""}, {"created_time", now_iso()}});
      r.status = 201;
      r.set_content(resp.dump(), "application/json");
    });
    server.Delete(R"(/trade-api/v2/margin/orders/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++cancels;
      last_cancel_query = json{{"subaccount", q.get_param_value("subaccount")}};
      std::string id = q.matches[1].str();
      double reduced = 0;
      json keep = json::array();
      for (const auto& o : resting) {
        if (o["order_id"] == id) reduced = parse_num(o.value("remaining_count", "0"));
        else keep.push_back(o);
      }
      resting = keep;
      r.set_content(json{{"order_id", id}, {"reduced_by", fmt_fp(reduced)}}.dump(), "application/json");
    });
    server.Post("/trade-api/v2/portfolio/intra_exchange_instance_transfer", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++transfers;
      last_transfer = json::parse(q.body);
      r.set_content(R"({"transfer_id":"tr-1"})", "application/json");
    });
    server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"orders":[],"fills":[],"cursor":""})", "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([&] { server.listen_after_bind(); });
  }
  ~MockMargin() { server.stop(); thread.join(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2"; }
  void quote(const std::string& t, double bid, double ask, double reference, long long ref_ts = 0) {
    std::lock_guard<std::mutex> lk(mu);
    markets[t] = market_json(t, bid, ask, reference, 0, 0.0001, ref_ts > 0 ? ref_ts * 1000 : 0);
  }
  // The exchange reports the resting order `id` filled in full (a fill row, the order gone).
  void fill_resting(const std::string& id, double price, bool taker = false) {
    std::lock_guard<std::mutex> lk(mu);
    json keep = json::array();
    for (const auto& o : resting) {
      if (o["order_id"] == id) {
        double n = parse_num(o.value("remaining_count", "0"));
        fills.push_back({{"fill_id", "fill-" + id}, {"order_id", id}, {"ticker", o["ticker"]}, {"side", o["side"]}, {"count", fmt_fp(n)},
                         {"price", fmt_dollars(price)}, {"entry_price", fmt_dollars(price)}, {"fees", "0.0039"}, {"realized_pnl", "0.0000"},
                         {"is_taker", taker}, {"created_time", now_iso()}, {"order_source", "user"}});
        double signed_n = o["side"] == "bid" ? n : -n;
        positions = json::array({{{"subaccount", 0}, {"market_ticker", o["ticker"]}, {"position", fmt_fp(signed_n)}, {"entry_price", fmt_dollars(price)},
                                  {"unrealized_pnl", "0.0000"}, {"margin_used", "1.3000"}, {"fees", "0.0039"}, {"is_portfolio", false}}});
        risk = json::array({{{"subaccount", 0}, {"market_ticker", o["ticker"]}, {"position", fmt_fp(signed_n)}, {"mark_price", fmt_dollars(price)},
                             {"position_notional", fmt_dollars(n * price)}, {"maintenance_margin_required", "1.3000"}, {"position_leverage", 6.0},
                             {"estimated_liquidation_price", fmt_dollars(signed_n > 0 ? price * 0.85 : price * 1.15)}, {"is_portfolio", false}}});
      } else keep.push_back(o);
    }
    resting = keep;
  }
  void flat() { std::lock_guard<std::mutex> lk(mu); positions = json::array(); risk = json::array(); }
};

static std::shared_ptr<Signer> test_signer(const std::string& pem) {
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str()) != 0)
    return nullptr;
  return std::make_shared<Signer>("test-key", pem);
}

static Config perps_cfg(const json& perps = json::object()) {
  Config cfg;
  cfg.bankroll_dollars = 100;
  json p = perps;
  if (!p.contains("refresh_seconds")) p["refresh_seconds"] = 0;
  if (!p.contains("account_refresh_seconds")) p["account_refresh_seconds"] = 0;
  // These tests exercise the live machinery, so they opt out of the M1 retirement explicitly.
  if (!p.contains("retired")) p["retired"] = false;
  cfg.strategy_params = json{{"perps", p}};
  return cfg;
}

// M1: retired perps refuse live mode, allow paper, and stop a saved live rule at startup.
static void test_perps_retired() {
  CHECK(perps_config(json::object()).retired == true);   // the default is retired
  Store st(":memory:");
  const std::string pem = "/tmp/perps-retired-test.pem";
  auto signer = test_signer(pem);
  KalshiClient client("http://127.0.0.1:1/trade-api/v2", signer, "t");
  // A live rule saved as running before the retirement...
  st.kv_set("perps_algo:v1", json{{"running", true}, {"paper", false}}.dump());
  Config cfg = perps_cfg(json{{"retired", true}});
  PerpsDesk desk(cfg, st, client);
  // ...comes back stopped.
  CHECK(desk.view()["algo"]["running"] == false);
  CHECK(!desk.live_rule_running());
  CHECK_THROWS(desk.configure_algo(json{{"paper", false}}), "retired");
  // Paper remains available for research.
  json v = desk.configure_algo(json{{"paper", true}, {"running", true}});
  CHECK(v["algo"]["running"] == true && v["algo"]["paper"] == true);
  CHECK(!desk.live_rule_running());
  CHECK(desk.summary()["retired"] == true);
  std::remove(pem.c_str());
}

static json algo_trades(Store& st, const std::string& state = "") {
  return state.empty() ? st.query("SELECT * FROM perp_algo_trades ORDER BY id") : st.query("SELECT * FROM perp_algo_trades WHERE state=? ORDER BY id", {state});
}

static void test_rails_and_tickets() {
  std::string pem = "/tmp/desk_perps_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip live tests: openssl CLI not available\n"); return; }
  MockMargin x;
  x.quote("KXBTCPERP", 7.8522, 7.8526, 7.8507);
  x.quote("KXETHPERP", 2.4856, 2.4857, 2.4857);
  Store st(":memory:");
  Config cfg = perps_cfg(json{{"max_order_notional", 30.0}, {"max_total_notional", 40.0}, {"max_leverage", 2.0}, {"price_band_pct", 2.0}});
  KalshiClient client(x.base(), signer, "tests");
  PerpsDesk desk(cfg, st, client);
  CHECK(desk.armed());
  long long now = now_s();
  // Before any sync: unknown market, then not synced.
  PerpOrderRequest r;
  r.ticker = "KXBTCPERP"; r.side = "bid"; r.count = 1; r.price = 7.8522; r.post_only = true;
  CHECK_THROWS(desk.ticket(r, "t"), "unknown perp market");
  desk.refresh_public(now);
  json v = desk.view();
  CHECK(v["markets"].size() == 2);
  CHECK(!st.query("SELECT * FROM perp_markets WHERE ticker='KXBTCPERP'").empty());
  CHECK(!st.query("SELECT * FROM perp_quotes WHERE ticker='KXBTCPERP'").empty());
  CHECK_THROWS(desk.ticket(r, "t"), "not synced");
  desk.refresh_account(now, true);
  v = desk.view();
  CHECK_NEAR(v["balance"]["equity"].get<double>(), 25.0, 1e-9);
  CHECK(v["fees"]["maker_fee_rates"]["KXBTCPERP"] == 0.0005);
  json summary = desk.summary();
  CHECK(summary["armed"] == true && summary["positions"] == 0 && std::fabs(summary["equity"].get<double>() - 25.0) < 1e-9);
  // Rails.
  PerpOrderRequest bad = r;
  bad.side = "buy";
  CHECK_THROWS(desk.ticket(bad, "t"), "side must be");
  bad = r; bad.count = 0.5;
  CHECK_THROWS(desk.ticket(bad, "t"), "whole contracts");
  bad = r; bad.count = 0;
  CHECK_THROWS(desk.ticket(bad, "t"), "at least 0.01");
  bad = r; bad.price = 7.5;   // 4.5% under the mark
  CHECK_THROWS(desk.ticket(bad, "t"), "from the mark");
  bad = r; bad.count = 4;     // $31.4 > $30 order cap
  CHECK_THROWS(desk.ticket(bad, "t"), "order cap");
  bad = r; bad.count = 3;     // $23.6 fits the order cap but 3 x 7.85 / 25 equity = 0.94x fine; total cap 40 fine
  // Total notional: the first order rests, the second would take the total over $40.
  x.fill_fraction = 0;
  json o1 = desk.ticket(bad, "first");
  CHECK(x.writes == 1 && o1["status"] == "resting" && o1["order_id"] == "po-1");
  CHECK(x.last_order["post_only"] == true && x.last_order["time_in_force"] == "good_till_canceled" && x.last_order["count"] == "3.00" && x.last_order["price"] == "7.8522");
  CHECK(x.last_order["client_order_id"].is_string() && x.last_order["client_order_id"].get<std::string>().size() == 36);
  CHECK(!st.query("SELECT * FROM perp_orders WHERE order_id='po-1' AND status='resting' AND origin='ticket'").empty());
  desk.refresh_account(now + 1, true);
  v = desk.view();
  CHECK(v["orders"].size() == 1);
  bad = r; bad.count = 3;     // 23.6 resting + 23.6 = 47 > 40
  CHECK_THROWS(desk.ticket(bad, "second"), "total perps notional");
  // Leverage: shrink equity so 23.6 + 7.85 would be over 2x.
  x.equity = 15.0; x.settled = 15.0;
  desk.refresh_account(now + 2, true);
  bad = r; bad.count = 1;     // total 31.4 / 15 = 2.1x
  CHECK_THROWS(desk.ticket(bad, "third"), "leverage would be");
  x.equity = 25.0; x.settled = 25.0;
  desk.refresh_account(now + 3, true);
  // Cancel: DELETE with the subaccount, row canceled.
  json c = desk.cancel("po-1");
  CHECK(x.cancels == 1 && c["reduced_by"] == "3.00" && x.last_cancel_query["subaccount"] == "0");
  CHECK(!st.query("SELECT * FROM perp_orders WHERE order_id='po-1' AND status='canceled'").empty());
  // Reduce-only skips the caps but not the band.
  bad = r; bad.count = 10; bad.reduce_only = true; bad.tif = "immediate_or_cancel"; bad.post_only = false;
  x.fill_fraction = 1.0;
  json o2 = desk.ticket(bad, "flatten");
  CHECK(o2["status"] == "done" && std::fabs(o2["fill_count"].get<double>() - 10) < 1e-9 && x.last_order["reduce_only"] == true);
  // The kill switch stops every ticket.
  st.kv_set("kill", "1");
  CHECK_THROWS(desk.ticket(r, "t"), "kill switch");
  st.kv_set("kill", "0");
  // A 4xx from the exchange comes back as ApiError with the body and is logged.
  x.reject = true;
  bool api_err = false;
  try { desk.ticket(r, "t"); } catch (const ApiError& e) { api_err = e.status == 400 && e.body.find("insufficient_margin") != std::string::npos; }
  CHECK(api_err);
  CHECK(!st.query("SELECT * FROM events_log WHERE msg LIKE 'PERPS REJECTED%'").empty());
  x.reject = false;
  // Book.
  json b = desk.book("KXBTCPERP");
  CHECK_NEAR(b["best_bid"].get<double>(), 7.8522, 1e-9);
  CHECK_NEAR(b["best_ask"].get<double>(), 7.8526, 1e-9);
  CHECK(b["bids"].size() == 2);
  // Close a position: a long is sold at the best bid, reduce-only IOC.
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.positions = json::array({{{"subaccount", 0}, {"market_ticker", "KXBTCPERP"}, {"position", "2.00"}, {"entry_price", "7.8000"},
                                {"unrealized_pnl", "0.1044"}, {"margin_used", "2.6000"}, {"fees", "0.0078"}, {"is_portfolio", false}}});
    x.risk = json::array({{{"subaccount", 0}, {"market_ticker", "KXBTCPERP"}, {"position", "2.00"}, {"mark_price", "7.8524"}, {"position_notional", "15.7048"},
                           {"maintenance_margin_required", "2.6000"}, {"position_leverage", 6.04}, {"estimated_liquidation_price", "6.7000"}, {"is_portfolio", false}}});
  }
  desk.refresh_account(now + 4, true);
  v = desk.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["side"] == "long" && std::fabs(v["positions"][0]["liquidation_price"].get<double>() - 6.7) < 1e-9);
  CHECK_NEAR(v["total_notional"].get<double>(), 2 * 7.8524, 1e-6);
  CHECK(desk.summary()["positions"] == 1);
  CHECK_THROWS(desk.close_position("KXETHPERP"), "no perps position");
  json cl = desk.close_position("KXBTCPERP");
  CHECK(x.last_order["side"] == "ask" && x.last_order["price"] == "7.8522" && x.last_order["count"] == "2.00" && x.last_order["reduce_only"] == true);
  CHECK(x.last_order["time_in_force"] == "immediate_or_cancel" && cl["fill_count"] == 2.0);
  // Transfers: centicents, both directions, refused between the same instance.
  json t = desk.transfer("event_contract", "margined", 5.0);
  CHECK(x.transfers == 1 && t["transfer_id"] == "tr-1");
  CHECK(x.last_transfer["amount"] == 50000 && x.last_transfer["source"] == "event_contract" && x.last_transfer["destination"] == "margined");
  CHECK(x.last_transfer["source_exchange_shard"] == 0 && x.last_transfer["destination_exchange_shard"] == 0);
  desk.transfer("margined", "event_contract", 2.5);
  CHECK(x.last_transfer["amount"] == 25000 && x.last_transfer["source"] == "margined");
  CHECK_THROWS(desk.transfer("margined", "margined", 1), "source and destination");
  CHECK_THROWS(desk.transfer("event_contract", "margined", 0), "between");
  // Kill sweep pulls whatever rests.
  x.fill_fraction = 0;
  x.flat();
  desk.refresh_account(now + 5, true);
  desk.ticket(r, "will be swept");
  desk.refresh_account(now + 6, true);
  int before = x.cancels;
  CHECK(desk.cancel_all("test kill") == 1 && x.cancels == before + 1);
  CHECK(st.query("SELECT * FROM perp_orders WHERE status='resting'").empty());
}

static void test_fill_reconciliation() {
  std::string pem = "/tmp/desk_perps_key.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockMargin x;
  x.quote("KXBTCPERP", 7.8522, 7.8526, 7.8507);
  Store st(":memory:");
  Config cfg = perps_cfg();
  KalshiClient client(x.base(), signer, "tests");
  PerpsDesk desk(cfg, st, client);
  long long now = now_s();
  desk.refresh_public(now);
  desk.refresh_account(now, true);
  PerpOrderRequest r;
  r.ticker = "KXBTCPERP"; r.side = "bid"; r.count = 2; r.price = 7.8522; r.post_only = true;
  json o = desk.ticket(r, "rest");
  desk.refresh_account(now + 1, true);
  CHECK(st.query("SELECT * FROM perp_orders WHERE order_id='po-1' AND status='resting'").size() == 1);
  // The exchange fills it: the order leaves the resting list, a fill arrives. The row must say
  // filled 2, not "done with 0 filled".
  x.fill_resting("po-1", 7.8522);
  desk.refresh_account(now + 2, true);
  json row = st.query("SELECT * FROM perp_orders WHERE order_id='po-1'");
  CHECK(row.size() == 1 && row[0]["status"] == "done" && std::fabs(row[0]["fill_count"].get<double>() - 2) < 1e-9 && row[0]["remaining_count"] == 0.0);
  CHECK(st.query("SELECT * FROM perp_fills").size() == 1);
  // A second sync sees the same fill again: still 2, not 4.
  desk.refresh_account(now + 3, true);
  row = st.query("SELECT * FROM perp_orders WHERE order_id='po-1'");
  CHECK(std::fabs(row[0]["fill_count"].get<double>() - 2) < 1e-9);
  json v = desk.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["position"] == 2.0 && v["fills"].size() == 1);
}

static void test_paper_rule() {
  MockMargin x;
  // Rich perp: mid 7.8601 vs index 7.8400 -> +25.6 bps.
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400);
  x.quote("KXETHPERP", 2.4856, 2.4857, 2.4857);   // flat basis: never trades
  Store st(":memory:");
  Config cfg = perps_cfg();
  KalshiClient client(x.base(), nullptr, "tests");   // no key: paper is all there is
  PerpsDesk desk(cfg, st, client);
  CHECK(!desk.armed());
  long long now = now_s();
  desk.refresh_public(now);
  CHECK_THROWS(desk.configure_algo(json{{"paper", false}}), "live needs an API key");
  CHECK_THROWS(desk.configure_algo(json{{"settings", {{"entry_bps", 5}, {"exit_bps", 10}}}}), "exit_bps must be below");
  CHECK_THROWS(desk.configure_algo(json{{"settings", {{"nope", 1}}}}), "unknown perps setting");
  json v = desk.configure_algo(json{{"settings", {{"entry_bps", 25}, {"exit_bps", 5}, {"stop_bps", 60}, {"contracts", 1}, {"cooldown_minutes", 0}}}});
  CHECK(v["algo"]["settings"]["entry_bps"] == 25 && v["algo"]["running"] == false);
  // Stopped: nothing.
  desk.step(now);
  CHECK(algo_trades(st).empty());
  v = desk.configure_algo(json{{"running", true}, {"paper", true}});
  CHECK(v["algo"]["running"] == true && v["algo"]["paper"] == true);
  desk.step(now + 1);
  json t = algo_trades(st);
  CHECK(t.size() == 1);
  if (t.size() == 1) {
    CHECK(t[0]["state"] == "entering" && t[0]["side"] == "ask" && t[0]["mode"] == "paper");
    CHECK_NEAR(t[0]["entry_price"].get<double>(), 7.8602, 1e-9);   // posted at the offer
    CHECK_NEAR(t[0]["entry_basis_bps"].get<double>(), (7.8601 - 7.84) / 7.84 * 1e4, 1e-6);
  }
  CHECK(x.writes == 0);
  // One position per market: no second entry while this one rests.
  desk.step(now + 2);
  CHECK(algo_trades(st).size() == 1);
  // The bid trades up through the posted offer: the paper order fills. Basis still wide: open.
  x.quote("KXBTCPERP", 7.8605, 7.8607, 7.8400);
  desk.refresh_public(now + 3);
  desk.step(now + 3);
  t = algo_trades(st);
  CHECK(t[0]["state"] == "open" && t[0]["filled_ts"] == now + 3);
  // Maker in at the 2 bps fallback: no API key, so /margin/fee_tiers never answers here.
  CHECK_NEAR(t[0]["fees"].get<double>(), 7.8602 * 2.0 / 1e4, 1e-9);   // 0.00157204
  json view = desk.view();
  CHECK(view["algo"]["positions"].size() == 1);
  // The perp comes back to the index: basis inside 5 bps -> out at the offer.
  x.quote("KXBTCPERP", 7.8400, 7.8402, 7.8400);
  desk.refresh_public(now + 4);
  desk.step(now + 4);
  t = algo_trades(st);
  CHECK(t[0]["state"] == "closed" && std::string(t[0]["reason"]).find("basis reverted") != std::string::npos);
  // Fallback fees both ways: maker 2 bps in, taker 12 bps out. 0.02 - 0.00157204 - 0.00940824 = 0.00901972.
  double expect = (7.8602 - 7.8402) - 7.8602 * 2.0 / 1e4 - 7.8402 * 12.0 / 1e4;
  CHECK_NEAR(t[0]["realized_pnl"].get<double>(), expect, 1e-6);
  CHECK_NEAR(t[0]["exit_price"].get<double>(), 7.8402, 1e-9);
  view = desk.view();
  CHECK_NEAR(view["algo"]["pnl_today"].get<double>(), expect, 1e-6);
  CHECK(view["algo"]["history"].size() == 1);
  // Cheap perp: buys at the bid. Then the stop: basis runs to -70 bps against a long.
  x.quote("KXBTCPERP", 7.8200, 7.8202, 7.8400);   // mid 7.8201 -> -25.4 bps
  desk.refresh_public(now + 5);
  desk.step(now + 5);
  t = algo_trades(st, "entering");
  CHECK(t.size() == 1 && t[0]["side"] == "bid" && std::fabs(t[0]["entry_price"].get<double>() - 7.82) < 1e-9);
  x.quote("KXBTCPERP", 7.8198, 7.8200, 7.8400);   // the offer comes down to the bid: filled
  desk.refresh_public(now + 6);
  desk.step(now + 6);
  CHECK(algo_trades(st, "open").size() == 1);
  x.quote("KXBTCPERP", 7.7850, 7.7852, 7.8400);   // -70 bps
  desk.refresh_public(now + 7);
  desk.step(now + 7);
  t = algo_trades(st);
  CHECK(t.size() == 2 && t[1]["state"] == "closed" && std::string(t[1]["reason"]).find("ran to") != std::string::npos);
  CHECK(t[1]["realized_pnl"].get<double>() < 0);
  // -70 bps is past the stop: not a candidate, whatever the entry threshold says.
  CHECK(algo_trades(st, "entering").empty());
  // Loss stop: a stop at $0.02 is already breached by that loss -> halted, no new entries.
  desk.configure_algo(json{{"settings", {{"daily_loss_stop", 0.02}}}});
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400);
  desk.refresh_public(now + 8);
  desk.step(now + 8);
  view = desk.view();
  CHECK(view["algo"]["halted"] == true);
  CHECK(algo_trades(st, "entering").empty());
  desk.configure_algo(json{{"clear_halt", true}});
  desk.step(now + 9);
  CHECK(desk.view()["algo"]["halted"] == false);
  CHECK(algo_trades(st, "entering").size() == 1);
  // TTL: the resting paper entry is pulled after entry_ttl_seconds unfilled, and the market
  // is not re-posted for another TTL even though the basis is still stretched.
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now + 100);
  desk.refresh_public(now + 100);
  desk.step(now + 9 + 91);
  CHECK(algo_trades(st, "entering").empty() && algo_trades(st, "canceled").size() == 1);
  desk.step(now + 100 + 30);
  CHECK(algo_trades(st, "entering").empty());
  // A stale index is not a basis: no entry.
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now - 600);
  desk.refresh_public(now + 100 + 95);
  desk.step(now + 100 + 95);
  CHECK(algo_trades(st, "entering").empty());
  // Cooldown: after a close, the market sits out cooldown_minutes even with a stretched basis.
  desk.configure_algo(json{{"settings", {{"cooldown_minutes", 30}}}});
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now + 200);
  st.exec("UPDATE perp_algo_trades SET closed_ts=? WHERE id=2", {now + 200});
  desk.refresh_public(now + 202);
  desk.step(now + 202);
  CHECK(algo_trades(st, "entering").empty());
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now + 200 + 31 * 60);
  desk.refresh_public(now + 200 + 31 * 60);
  desk.step(now + 200 + 31 * 60);
  CHECK(algo_trades(st, "entering").size() == 1);
  desk.configure_algo(json{{"settings", {{"cooldown_minutes", 0}}}});
  // Persistence: a new desk on the same store resumes running with the same settings.
  PerpsDesk again(cfg, st, client);
  json a = again.view()["algo"];
  CHECK(a["running"] == true && a["settings"]["daily_loss_stop"] == 0.02);
  // Stop.
  v = desk.configure_algo(json{{"running", false}});
  CHECK(v["algo"]["running"] == false);
}

static void test_live_rule() {
  std::string pem = "/tmp/desk_perps_key.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockMargin x;
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400);   // +25.6 bps rich
  Store st(":memory:");
  Config cfg = perps_cfg(json{{"algo", {{"markets", json::array({"KXBTCPERP"})}}}});
  KalshiClient client(x.base(), signer, "tests");
  PerpsDesk desk(cfg, st, client);
  long long now = now_s();
  desk.refresh_public(now);
  desk.refresh_account(now, true);
  desk.configure_algo(json{{"paper", false}, {"running", true}, {"settings", {{"entry_ttl_seconds", 60}, {"cooldown_minutes", 0}}}});
  x.fill_fraction = 0;
  desk.step(now + 1);
  CHECK(x.writes == 1);
  CHECK(x.last_order["side"] == "ask" && x.last_order["price"] == "7.8602" && x.last_order["post_only"] == true && x.last_order["count"] == "1.00");
  json t = algo_trades(st);
  CHECK(t.size() == 1 && t[0]["state"] == "entering" && t[0]["order_id"] == "po-1" && t[0]["mode"] == "live");
  CHECK(!st.query("SELECT * FROM perp_orders WHERE order_id='po-1' AND origin='algo'").empty());
  // Still resting after a sync: nothing changes, nothing more is sent.
  desk.refresh_account(now + 2, true);
  desk.step(now + 2);
  CHECK(x.writes == 1 && algo_trades(st, "entering").size() == 1);
  // Filled on the exchange: the sync brings the fill and the position; the trade opens.
  x.fill_resting("po-1", 7.8602);
  desk.refresh_account(now + 3, true);
  desk.step(now + 3);
  t = algo_trades(st);
  CHECK(t[0]["state"] == "open" && t[0]["count"] == 1.0);
  // Basis reverts: a reduce-only IOC at the offer goes out; the fill closes the trade.
  x.quote("KXBTCPERP", 7.8400, 7.8402, 7.8400);
  x.fill_fraction = 1.0;
  desk.refresh_public(now + 4);
  desk.step(now + 4);
  CHECK(x.writes == 2);
  CHECK(x.last_order["side"] == "bid" && x.last_order["reduce_only"] == true && x.last_order["time_in_force"] == "immediate_or_cancel" && x.last_order["price"] == "7.8402");
  t = algo_trades(st);
  CHECK(t[0]["state"] == "closed" && t[0]["exit_order_id"] == "po-2");
  double expect = (7.8602 - 7.8402) - 7.8602 * 5.0 / 1e4 - 7.8402 * 12.0 / 1e4;
  CHECK_NEAR(t[0]["realized_pnl"].get<double>(), expect, 1e-6);
  // A hand position in the market: the rule leaves the market alone.
  x.flat();
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.positions = json::array({{{"subaccount", 0}, {"market_ticker", "KXBTCPERP"}, {"position", "1.00"}, {"entry_price", "7.8000"},
                                {"unrealized_pnl", "0.0000"}, {"margin_used", "1.3000"}, {"fees", "0.0000"}, {"is_portfolio", false}}});
  }
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now + 5);
  x.fill_fraction = 0;
  desk.refresh_public(now + 5);
  desk.refresh_account(now + 5, true);
  desk.step(now + 5);
  CHECK(x.writes == 2);
  x.flat();
  desk.refresh_account(now + 6, true);
  // TTL: a resting live entry is pulled with a DELETE, and the exchange row goes with it.
  desk.step(now + 6);
  CHECK(x.writes == 3 && algo_trades(st, "entering").size() == 1);
  desk.refresh_account(now + 7, true);
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, now + 67);
  desk.refresh_public(now + 67);
  desk.step(now + 67);
  CHECK(x.cancels == 1 && algo_trades(st, "canceled").size() == 1);
  CHECK(st.query("SELECT * FROM perp_orders WHERE status='resting'").empty());
  // Not re-posted for another TTL.
  desk.step(now + 68);
  CHECK(x.writes == 3);
  // Kill switch: no entry while on, and a resting entry is pulled at once.
  long long t2 = now + 67 + 61;
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, t2);
  desk.refresh_public(t2);
  desk.step(t2);
  CHECK(x.writes == 4 && algo_trades(st, "entering").size() == 1);
  desk.refresh_account(t2 + 1, true);
  st.kv_set("kill", "1");
  desk.step(t2 + 2);
  CHECK(x.cancels == 2 && algo_trades(st, "canceled").size() == 2);
  x.quote("KXBTCPERP", 7.8600, 7.8602, 7.8400, t2 + 70);
  desk.refresh_public(t2 + 70);
  desk.step(t2 + 70);
  CHECK(x.writes == 4);
  st.kv_set("kill", "0");
  // The position vanished on the exchange (liquidated or closed by hand): the trade closes
  // from the fills, nothing is sent.
  x.fill_fraction = 0;
  desk.step(t2 + 71);
  CHECK(x.writes == 5);
  x.fill_resting("po-5", 7.8602);
  desk.refresh_account(t2 + 72, true);
  desk.step(t2 + 72);
  CHECK(algo_trades(st, "open").size() == 1);
  x.flat();
  desk.refresh_account(t2 + 73, true);
  desk.step(t2 + 73);
  t = algo_trades(st, "closed");
  CHECK(t.size() == 2 && std::string(t[1]["reason"]).find("position gone") != std::string::npos);
  // With cooldown 0 the still-stretched basis is posted against again in the same step.
  CHECK(x.writes == 6 && algo_trades(st, "entering").size() == 1);
  // An immediate maker fill on entry opens the trade at once: clear the resting one first.
  desk.configure_algo(json{{"running", false}});
  st.kv_set("kill", "1");
  desk.step(t2 + 74);   // pulls the resting entry
  st.kv_set("kill", "0");
  CHECK(x.cancels == 3 && algo_trades(st, "entering").empty());
  st.exec("UPDATE perp_algo_trades SET closed_ts=0 WHERE state='canceled'");   // no backoff for the test
  desk.configure_algo(json{{"running", true}});
  x.fill_fraction = 1.0;
  desk.refresh_account(t2 + 75, true);
  desk.step(t2 + 75);
  CHECK(x.writes == 7 && algo_trades(st, "open").size() == 1);
  json opened = algo_trades(st, "open");
  CHECK(opened.size() == 1 && opened[0]["filled_ts"] == t2 + 75 && opened[0]["order_id"] == "po-7");
  // A rejected entry is logged, nothing recorded.
  desk.configure_algo(json{{"running", false}});
  st.exec("DELETE FROM perp_algo_trades");
  x.reject = true;
  desk.configure_algo(json{{"running", true}});
  desk.step(t2 + 76);
  CHECK(algo_trades(st).empty());
  json a = desk.view()["algo"];
  bool logged = false;
  for (const auto& e : a["log"]) if (std::string(e["msg"]).find("entry not sent") != std::string::npos) logged = true;
  CHECK(logged);
}

int main() {
  test_perps_retired();
  test_parsers();
  test_config();
  test_rails_and_tickets();
  test_fill_reconciliation();
  test_paper_rule();
  test_live_rule();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
