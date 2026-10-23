// The fake Polymarket US over loopback, seen from a raw client (tests/fake_exchange/client.h): its
// read routes and their shapes (the old desk's mock, tests/test_poly.cpp), the engine's books,
// orders, positions and cash as those routes list them, paging, auth with a real Ed25519 signature
// (a key made here, never a real one) and its 30 s window, the wire log, TLS, and fake-kalshi
// --poly. Then the writes: an asynchronous create and its delayed executions, a synchronous one,
// GTD and its expiry, post-only in both modes, cancels and cancel-all, preview, every fault, the
// rate limit and a maintenance window. Nothing here talks to anything but 127.0.0.1.
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "client.h"
#include "deskd/build_paths.h"
#include "fake_poly.h"
#include "tls_pki.h"

extern char** environ;

using namespace fakex;

namespace {

constexpr Micros D = kMicrosPerDollar;
constexpr std::int64_t T0 = 1791300600000;  // 2026-10-06T15:30:00Z
const std::string NE = "aec-nfl-ne-sea-2026-09-09";
const std::string DAL = "aec-nfl-dal-nyg-2026-09-13";
const std::string HOU = "aec-mlb-hou-phi-2026-09-08";
const std::string KEY_ID = "a422f5a3-0000-4000-8000-000000000000";

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

PolySide side(const std::string& desc, bool is_long, const std::string& abbr, const std::string& name,
              const std::string& safe, const std::string& alias) {
  return PolySide{desc, is_long, PolyTeam{abbr, name, safe, alias}};
}

// The mock's two NFL games (seed_poly) and one MLB game, as the league events list them.
void list_games(FakePoly& f) {
  PolyMarketSpec ne;
  ne.slug = NE;
  ne.league = "nfl";
  ne.event_slug = "nfl-ne-sea-2026-09-09";
  ne.event_title = "New England vs. Seattle";
  ne.question = "Who will win Patriots vs Seahawks?";
  ne.start_ms = 1788999600000;  // 2026-09-10T00:20:00Z
  ne.end_ms = 1789086000000;    // 2026-09-11T00:20:00Z
  ne.sides = {side("Patriots", true, "ne", "New England Patriots", "New England", "Patriots"),
              side("Seahawks", false, "sea", "Seattle Seahawks", "Seattle", "Seahawks")};
  f.list_market(ne);
  PolyMarketSpec dal = ne;
  dal.slug = DAL;
  dal.event_slug = "nfl-dal-nyg-2026-09-13";
  dal.event_title = "Dallas vs. New York G";
  dal.question = "Who will win Cowboys vs Giants?";
  dal.sides = {side("Cowboys", true, "dal", "Dallas Cowboys", "Dallas", "Cowboys"),
               side("Giants", false, "nyg", "New York Giants", "New York", "Giants")};
  f.list_market(dal);
  PolyMarketSpec hou = ne;
  hou.slug = HOU;
  hou.league = "mlb";
  hou.event_slug = "mlb-hou-phi-2026-09-08";
  hou.event_title = "Houston vs. Philadelphia";
  hou.sports_type = "baseball_team_full_game_winner";
  hou.tick = 100;           // a 1c market
  hou.coeff_micro = 69500;  // core.js's 0.0695
  hou.start_ms = hou.end_ms = 0;
  hou.sides = {};
  f.list_market(hou);
}

FakePolyConfig unlimited(FakePolyConfig cfg) {
  cfg.rate_limits = false;
  return cfg;
}

// One test's venue on a free port: the three games, NE's street book YES 0.380 x10 and 0.375 x20
// bid, 0.385 x9 and 0.390 x5 offered (NO bids at 0.615 and 0.610), and $100 deposited. Its rate
// limit is off (test_rate_limit turns it on).
struct Venue {
  FakePoly f;
  bool ok = false;
  explicit Venue(FakePolyConfig cfg = {}) : f(unlimited(std::move(cfg))) {
    list_games(f);
    f.with_engine([](Engine& e, std::int64_t now) {
      e.street_add(NE, false, 3800, 1000, now);
      e.street_add(NE, false, 3750, 2000, now);
      e.street_add(NE, true, 6150, 900, now);
      e.street_add(NE, true, 6100, 500, now);
    });
    f.deposit(100 * D);
    std::string err;
    ok = f.start(&err);
    if (!ok) std::fprintf(stderr, "fake start: %s\n", err.c_str());
  }
  int port() const { return f.port(); }
};

std::string path_only(const std::string& target) { return target.substr(0, target.find('?')); }

// ---- Ed25519, with OpenSSL directly: a key made here for the test, and its signatures ----------

struct Key {
  EVP_PKEY* k = nullptr;
  Key() {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (c && EVP_PKEY_keygen_init(c) == 1) EVP_PKEY_keygen(c, &k);
    EVP_PKEY_CTX_free(c);
  }
  explicit Key(const std::string& seed) {
    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(seed.data()),
                                     seed.size());
  }
  ~Key() { EVP_PKEY_free(k); }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  std::string public_raw() const {
    unsigned char pub[32];
    std::size_t len = sizeof pub;
    if (!k || EVP_PKEY_get_raw_public_key(k, pub, &len) != 1) return "";
    return std::string(reinterpret_cast<const char*>(pub), len);
  }
  std::string public_b64() const { return net::base64(public_raw()); }
  std::string sign_raw(const std::string& text) const {
    unsigned char sig[64];
    std::size_t len = sizeof sig;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
                    EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(text.data()), text.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok ? std::string(reinterpret_cast<const char*>(sig), len) : "";
  }
  std::string sign(const std::string& text) const { return net::base64(sign_raw(text)); }
};

std::string unhex(const std::string& h) {
  std::string out;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16)));
  return out;
}

// X-PM-Access-Key, -Timestamp and -Signature for method + path (no query), signed by `key` or
// carrying a placeholder signature (enough for AuthMode::PRESENCE).
Headers pm_auth(const std::string& method, const std::string& path, std::int64_t ts_ms, const Key* key = nullptr,
                const std::string& key_id = KEY_ID) {
  const std::string ts = std::to_string(ts_ms);
  return {{"X-PM-Access-Key", key_id},
          {"X-PM-Timestamp", ts},
          {"X-PM-Signature", key ? key->sign(ts + method + path) : std::string("c2lnbmF0dXJl")}};
}

// A signed GET (any signature passes PRESENCE).
HttpResult get(HttpConn& c, const std::string& target, const Key* key = nullptr) {
  return c.request("GET", target, "", pm_auth("GET", path_only(target), wall_ms(), key));
}

json money(const std::string& v) { return json{{"value", v}, {"currency", "USD"}}; }

std::set<std::string> keys(const json& j) {
  std::set<std::string> k;
  if (j.is_object())
    for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
  return k;
}

bool logged(FakePoly& f, const std::string& line) {
  for (const auto& l : f.wire_log())
    if (l == line) return true;
  return false;
}

// ---- the public reads -----------------------------------------------------------------------

void test_markets() {
  Venue v;
  CHECK(v.ok);
  HttpConn c(v.port());
  // the league's events, each with its markets; no credential needed
  auto r = c.request("GET", "/v2/leagues/nfl/events?limit=200&offset=0");
  CHECK(r.status() == 200 && r.j["league"] == json({{"slug", "nfl"}}) && r.j["events"].size() == 2);
  const json& ev = r.j["events"][0];
  CHECK(ev["slug"] == "nfl-ne-sea-2026-09-09" && ev["title"] == "New England vs. Seattle" && ev["markets"].size() == 1);
  const json& m = ev["markets"][0];
  CHECK((keys(m) == std::set<std::string>{"slug", "question", "sportsMarketType", "orderPriceMinTickSize",
                                          "minimumTradeQty", "feeCoefficient", "active", "closed", "bestBidQuote",
                                          "bestAskQuote", "gameStartTime", "endDate", "updatedAt", "marketSides"}));
  CHECK(m["slug"] == NE && m["question"] == "Who will win Patriots vs Seahawks?" &&
        m["sportsMarketType"] == "football_team_full_game_winner");
  CHECK(m["orderPriceMinTickSize"] == 0.005 && m["minimumTradeQty"] == 1 && m["feeCoefficient"] == 0.06);
  CHECK(m["active"] == true && m["closed"] == false);
  CHECK(m["bestBidQuote"] == money("0.3800") && m["bestAskQuote"] == money("0.3850"));
  CHECK(m["gameStartTime"] == "2026-09-10T00:20:00.000000Z" && m["endDate"] == "2026-09-11T00:20:00.000000Z");
  CHECK(m["updatedAt"].is_string() && m["marketSides"].size() == 2);
  const json& s0 = m["marketSides"][0];
  CHECK(s0["description"] == "Patriots" && s0["long"] == true && s0["price"] == "0.3850");
  CHECK(s0["team"] == json({{"abbreviation", "ne"}, {"name", "New England Patriots"}, {"safeName", "New England"},
                             {"alias", "Patriots"}, {"league", "nfl"}}));
  CHECK(m["marketSides"][1]["long"] == false && m["marketSides"][1]["price"] == "0.6200");  // 1 - the 0.38 bid
  // a market with an empty book lists no quote and no side price
  const json& dal = r.j["events"][1]["markets"][0];
  CHECK(dal["slug"] == DAL && !dal.contains("bestBidQuote") && !dal.contains("bestAskQuote"));
  CHECK(!dal["marketSides"][0].contains("price") && !dal["marketSides"][1].contains("price"));
  // paging by offset; another league's events; one with none
  r = c.request("GET", "/v2/leagues/nfl/events?limit=1");
  CHECK(r.status() == 200 && r.j["events"].size() == 1 && r.j["events"][0]["slug"] == "nfl-ne-sea-2026-09-09");
  r = c.request("GET", "/v2/leagues/nfl/events?limit=1&offset=1");
  CHECK(r.status() == 200 && r.j["events"].size() == 1 && r.j["events"][0]["slug"] == "nfl-dal-nyg-2026-09-13");
  r = c.request("GET", "/v2/leagues/nfl/events?offset=5");
  CHECK(r.status() == 200 && r.j["events"].empty());
  r = c.request("GET", "/v2/leagues/mlb/events");
  CHECK(r.status() == 200 && r.j["events"].size() == 1);
  const json& hou = r.j["events"][0]["markets"][0];
  CHECK(hou["orderPriceMinTickSize"] == 0.01 && hou["feeCoefficient"] == 0.0695 &&
        hou["sportsMarketType"] == "baseball_team_full_game_winner");
  CHECK(!hou.contains("gameStartTime") && !hou.contains("endDate") && hou["marketSides"].empty());
  r = c.request("GET", "/v2/leagues/epl/events");
  CHECK(r.status() == 200 && r.j["events"].empty() && r.j["league"]["slug"] == "epl");
  for (const char* bad : {"limit=0", "limit=1001", "limit=x", "offset=-1", "limit="}) {
    r = c.request("GET", std::string("/v2/leagues/nfl/events?") + bad);
    CHECK(r.status() == 400 && r.j["code"] == 3);
  }
  // the batch read by slug, in the order asked, an unknown slug left out
  r = c.request("GET", "/v1/markets?slug=" + HOU + "&slug=nope&slug=" + NE);
  CHECK(r.status() == 200 && r.j["markets"].size() == 2 && r.j["markets"][0]["slug"] == HOU &&
        r.j["markets"][1]["slug"] == NE);
  CHECK(r.j["markets"][1]["bestBidQuote"] == money("0.3800"));
  r = c.request("GET", "/v1/markets?limit=1");  // scripts/check_polymarket.py's reachability probe
  CHECK(r.status() == 200 && r.j["markets"].size() == 1 && r.j["markets"][0]["slug"] == NE);
  r = c.request("GET", "/v1/markets?offset=1");
  CHECK(r.status() == 200 && r.j["markets"].size() == 2 && r.j["markets"][0]["slug"] == DAL);
  // the book, best first both ways
  r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.status() == 200 && keys(r.j) == std::set<std::string>{"marketData"});
  CHECK(r.j["marketData"]["bids"] == json::parse(R"([{"px":{"value":"0.3800","currency":"USD"},"qty":"10.00"},
                                                     {"px":{"value":"0.3750","currency":"USD"},"qty":"20.00"}])"));
  CHECK(r.j["marketData"]["asks"] == json::parse(R"([{"px":{"value":"0.3850","currency":"USD"},"qty":"9.00"},
                                                     {"px":{"value":"0.3900","currency":"USD"},"qty":"5.00"}])"));
  r = c.request("GET", "/v1/markets/" + DAL + "/book");
  CHECK(r.status() == 200 && r.j["marketData"]["bids"].empty() && r.j["marketData"]["asks"].empty());
  r = c.request("GET", "/v1/markets/nope/book");
  CHECK(r.status() == 404 && r.j == json({{"code", 5}, {"message", "market not found"}}));
  // a pause leaves the market listed, not active; a close empties its book
  v.f.with_engine([](Engine& e, std::int64_t now) { e.pause(DAL, now); });
  r = c.request("GET", "/v1/markets?slug=" + DAL);
  CHECK(r.j["markets"][0]["active"] == false && r.j["markets"][0]["closed"] == false);
  v.f.with_engine([](Engine& e, std::int64_t now) { e.close(NE, now); });
  r = c.request("GET", "/v1/markets?slug=" + NE);
  CHECK(r.j["markets"][0]["active"] == false && r.j["markets"][0]["closed"] == true &&
        !r.j["markets"][0].contains("bestBidQuote"));
  // only GET; anything else is no route
  r = c.request("POST", "/v1/markets", "{}");
  CHECK(r.status() == 405 && r.j["code"] == 12);
  r = c.request("GET", "/v1/nothing");
  CHECK(r.status() == 404 && r.j["code"] == 5);
  r = c.request("GET", "/trade-api/v2/markets");
  CHECK(r.status() == 404);
  // a listing again keeps the book: the spec is replaced, the engine's market stays
  PolyMarketSpec again;
  again.slug = DAL;
  again.league = "nfl";
  again.event_slug = "nfl-dal-nyg-2026-09-13";
  again.event_title = "Dallas vs. New York G";
  again.question = "Cowboys or Giants?";
  v.f.with_engine([](Engine& e, std::int64_t now) {
    e.resume(DAL, now);
    e.street_add(DAL, false, 6000, 100, now);
  });
  v.f.list_market(again);
  r = c.request("GET", "/v1/markets?slug=" + DAL);
  CHECK(r.j["markets"][0]["question"] == "Cowboys or Giants?" && r.j["markets"][0]["bestBidQuote"] == money("0.6000") &&
        r.j["markets"][0]["active"] == true);
  r = c.request("GET", "/v2/leagues/nfl/events");
  CHECK(r.j["events"].size() == 2 && r.j["events"][1]["markets"].size() == 1);
  // the engine runs each listed market on its own grid with Polymarket's fees and no STP
  CHECK(v.f.with_engine([](Engine& e, std::int64_t) {
    const Market* m = e.market(HOU);
    return m && m->spec.fee_type == FeeType::POLY && m->spec.poly_coeff_micro == 69500 && on_grid(m->spec.ticks, 100) &&
           !on_grid(m->spec.ticks, 50) && on_grid(e.market(NE)->spec.ticks, 50) && !on_grid(e.market(NE)->spec.ticks, 0) &&
           !on_grid(e.market(NE)->spec.ticks, kDollar) && e.market(NE)->spec.event_ticker == "nfl-ne-sea-2026-09-09";
  }));
  // the wire log: public reads carry no credential, and one that does says so
  CHECK(logged(v.f, "http 200 GET /v2/leagues/nfl/events") && logged(v.f, "http 404 GET /v1/markets/nope/book"));
  v.f.clear_wire_log();
  r = c.request("GET", "/v1/markets/" + DAL + "/book", "", pm_auth("GET", "/v1/markets/" + DAL + "/book", wall_ms()));
  CHECK(r.status() == 200 && logged(v.f, "http 200 GET /v1/markets/" + DAL + "/book (signed)"));
  r = c.request("GET", "/v1/markets?slug=" + DAL, "", {{"X-PM-Timestamp", "1"}});
  CHECK(r.status() == 200 && logged(v.f, "http 200 GET /v1/markets (signed)") && v.f.wire_log().size() == 2);
}

// ---- the account ------------------------------------------------------------------------------

std::string create(FakePoly& f, const std::string& slug, Side s, Qty q, Px p, Tif tif, std::int64_t exp_s = 0) {
  return f.with_engine([&](Engine& e, std::int64_t now) {
    NewOrder o;
    o.ticker = slug;
    o.side = s;
    o.count = q;
    o.price = p;
    o.tif = tif;
    o.stp = Stp::NONE;
    o.expiration_s = exp_s;
    const CreateResult r = e.create(o, now);
    if (r.err) std::fprintf(stderr, "create refused: %s\n", r.err.message.c_str());
    return r.order_id;
  });
}

void test_account() {
  FakePolyConfig cfg;
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  cfg.auth_max_skew_ms = 0;  // the fake's clock is T0; the client signs with the wall clock
  Venue v(cfg);
  CHECK(v.ok);
  HttpConn c(v.port());
  auto r = get(c, "/v1/account/balances");
  CHECK(r.status() == 200 && r.j["balances"].size() == 1);
  CHECK((r.j["balances"][0] == json{{"currentBalance", 100.0}, {"currency", "USD"}, {"buyingPower", 100.0},
                                    {"assetNotional", 0.0}, {"assetAvailable", 0.0}}));
  r = get(c, "/v1/orders/open");
  CHECK(r.status() == 200 && r.j == json({{"orders", json::array()}}));
  r = get(c, "/v1/portfolio/positions");
  CHECK(r.status() == 200 && r.j == json({{"positions", json::object()}, {"nextCursor", ""}, {"eof", true}}));

  // BUY 5 at 0.385 IOC takes 5 of the 9 offered: 1.925 and a 7c fee (0.06 x 5 x 0.385 x 0.615 = 7.1c)
  const std::string ioc = create(v.f, NE, Side::BID, 500, 3850, Tif::IOC);
  r = get(c, "/v1/order/" + ioc);
  CHECK(r.status() == 200 && keys(r.j) == std::set<std::string>{"order"});
  const json& o = r.j["order"];
  CHECK((keys(o) == std::set<std::string>{"id", "marketSlug", "type", "side", "tif", "state", "price", "quantity",
                                          "cumQuantity", "leavesQuantity", "avgPx", "commissionNotionalTotalCollected",
                                          "createTime", "updateTime"}));
  CHECK(o["id"] == ioc && o["marketSlug"] == NE && o["type"] == "ORDER_TYPE_LIMIT" && o["side"] == "ORDER_SIDE_BUY");
  CHECK(o["tif"] == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL" && o["state"] == "ORDER_STATE_FILLED");
  CHECK(o["price"] == money("0.3850") && o["quantity"] == 5 && o["cumQuantity"] == 5 && o["leavesQuantity"] == 0);
  CHECK(o["avgPx"] == money("0.3850") && o["commissionNotionalTotalCollected"] == money("0.0700"));
  CHECK(o["createTime"] == "2026-10-06T15:30:00.000000Z");
  r = get(c, "/v1/account/balances");
  CHECK(r.j["balances"][0]["currentBalance"] == 98.005);
  // the position, marked at the best bid: 5 x 0.38
  r = get(c, "/v1/portfolio/positions");
  CHECK(r.status() == 200 && r.j["eof"] == true && r.j["positions"].size() == 1);
  CHECK((r.j["positions"][NE] == json{{"netPositionDecimal", "5"}, {"cost", money("1.9250")}, {"realized", money("0.0000")},
                                      {"cashValue", money("1.9000")}, {"qtyAvailableDecimal", "5"}, {"expired", false},
                                      {"updateTime", "2026-10-06T15:30:00.000000Z"}}));
  r = get(c, "/v1/account/balances");
  CHECK(r.j["balances"][0]["assetNotional"] == 1.9 && r.j["balances"][0]["assetAvailable"] == 1.9);

  // a GTC BUY 6 at 0.385 takes the other 4 (6c) and rests 2; a GTD BUY 3 at 0.370 rests
  v.f.advance_ms(1000);
  const std::string part = create(v.f, NE, Side::BID, 600, 3850, Tif::GTC);
  const std::string gtd = create(v.f, NE, Side::BID, 300, 3700, Tif::GTC, T0 / 1000 + 60);
  r = get(c, "/v1/orders/open");
  CHECK(r.status() == 200 && r.j["orders"].size() == 2);
  const json& p = r.j["orders"][0];
  CHECK(p["id"] == part && p["state"] == "ORDER_STATE_PARTIALLY_FILLED" && p["tif"] == "TIME_IN_FORCE_GOOD_TILL_CANCEL");
  CHECK(p["quantity"] == 6 && p["cumQuantity"] == 4 && p["leavesQuantity"] == 2 && p["avgPx"] == money("0.3850"));
  CHECK(p["commissionNotionalTotalCollected"] == money("0.0600") && p["updateTime"] == "2026-10-06T15:30:01.000000Z");
  const json& g = r.j["orders"][1];
  CHECK(g["id"] == gtd && g["state"] == "ORDER_STATE_NEW" && g["tif"] == "TIME_IN_FORCE_GOOD_TILL_DATE");
  CHECK(g["cumQuantity"] == 0 && g["leavesQuantity"] == 3 && !g.contains("avgPx") &&
        g["commissionNotionalTotalCollected"] == money("0.0000"));
  // our resting 2 at 0.385 are the best bid now, and the cash they hold is not buying power
  r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.j["marketData"]["bids"][0] == json::parse(R"({"px":{"value":"0.3850","currency":"USD"},"qty":"2.00"})"));
  CHECK(r.j["marketData"]["asks"][0]["px"] == money("0.3900"));
  r = get(c, "/v1/account/balances");
  const auto [cash, avail] =
      v.f.with_engine([](Engine& e, std::int64_t) { return std::make_pair(e.cash(0, 0), e.available(0, 0)); });
  CHECK(cash == 98005000 - 1540000 - 60000);
  CHECK(r.j["balances"][0]["currentBalance"] == static_cast<double>(cash) / D &&
        r.j["balances"][0]["buyingPower"] == static_cast<double>(avail) / D);
  CHECK(cash - avail >= 2 * 385000 + 3 * 370000);

  // the street sells 2 into our bid: a maker fill, paid its 1c rebate (0.0125 x 2 x 0.385 x 0.615
  // = 0.59c), so the order's commission nets to 5c
  v.f.advance_ms(1000);
  v.f.with_engine([](Engine& e, std::int64_t now) { e.street_take(NE, Side::ASK, 3850, 200, now); });
  r = get(c, "/v1/order/" + part);
  CHECK(r.j["order"]["state"] == "ORDER_STATE_FILLED" && r.j["order"]["cumQuantity"] == 6 &&
        r.j["order"]["leavesQuantity"] == 0 && r.j["order"]["commissionNotionalTotalCollected"] == money("0.0500"));

  // a SELL with nothing held opens a short: 3 at 0.600 hold NO at 0.40, marked at the 0.395 NO bid
  v.f.with_engine([](Engine& e, std::int64_t now) {
    e.street_add(DAL, false, 6000, 1000, now);
    e.street_add(DAL, true, 3950, 1000, now);
  });
  const std::string sell = create(v.f, DAL, Side::ASK, 300, 6000, Tif::IOC);
  r = get(c, "/v1/order/" + sell);
  CHECK(r.j["order"]["side"] == "ORDER_SIDE_SELL" && r.j["order"]["state"] == "ORDER_STATE_FILLED");
  CHECK(r.j["order"]["avgPx"] == money("0.6000") && r.j["order"]["commissionNotionalTotalCollected"] == money("0.0400"));
  r = get(c, "/v1/portfolio/positions");
  CHECK(r.j["positions"].size() == 2 && r.j["eof"] == true && r.j["nextCursor"] == "");
  CHECK(r.j["positions"][DAL]["netPositionDecimal"] == "-3" && r.j["positions"][DAL]["cost"] == money("1.2000") &&
        r.j["positions"][DAL]["cashValue"] == money("1.1850") && r.j["positions"][DAL]["qtyAvailableDecimal"] == "3");
  CHECK(r.j["positions"][NE]["netPositionDecimal"] == "11");
  // paging by slug
  r = get(c, "/v1/portfolio/positions?limit=1");
  CHECK(r.j["positions"].size() == 1 && r.j["positions"].contains(DAL) && r.j["nextCursor"] == DAL &&
        r.j["eof"] == false);
  r = get(c, "/v1/portfolio/positions?limit=1&cursor=" + DAL);
  CHECK(r.j["positions"].size() == 1 && r.j["positions"].contains(NE) && r.j["nextCursor"] == "" && r.j["eof"] == true);
  r = get(c, "/v1/portfolio/positions?limit=0");
  CHECK(r.status() == 400 && r.j["code"] == 3);

  // the GTD expires at its second; a cancel is a cancel
  v.f.advance_ms(60000);
  r = get(c, "/v1/order/" + gtd);
  CHECK(r.j["order"]["state"] == "ORDER_STATE_EXPIRED" && r.j["order"]["leavesQuantity"] == 0);
  const std::string rest = create(v.f, NE, Side::BID, 100, 3500, Tif::GTC);
  v.f.with_engine([&](Engine& e, std::int64_t now) { e.cancel(rest, NE, -2, 0, now); });
  r = get(c, "/v1/order/" + rest);
  CHECK(r.j["order"]["state"] == "ORDER_STATE_CANCELED");
  r = get(c, "/v1/orders/open");
  CHECK(r.j["orders"].empty());
  r = get(c, "/v1/order/nope");
  CHECK(r.status() == 404 && r.j == json({{"code", 5}, {"message", "order not found"}}));

  // a withdrawal; then the history, newest first: the withdrawal, the four fills, the deposit
  v.f.deposit(-10 * D);
  // since `cash`: the maker's 2 at 0.385 less their rebate, the short's 3 at 0.40 and 4c, the $10
  CHECK(v.f.with_engine([](Engine& e, std::int64_t) { return e.cash(0, 0); }) ==
        cash - 770000 + 10000 - 1200000 - 40000 - 10 * D);
  r = get(c, "/v1/portfolio/activities");
  CHECK(r.status() == 200 && r.j["eof"] == true && r.j["nextCursor"] == "" && r.j["activities"].size() == 6);
  const json a = r.j["activities"];  // a copy: the pages below replace r
  CHECK(a[0]["type"] == "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL" && a[0]["accountBalanceChange"]["amount"] == money("10.0000"));
  CHECK(a[5]["type"] == "ACTIVITY_TYPE_ACCOUNT_DEPOSIT" && a[5]["accountBalanceChange"]["amount"] == money("100.0000"));
  CHECK((keys(a[5]["accountBalanceChange"]) == std::set<std::string>{"transactionId", "amount", "updateTime"}));
  CHECK(a[5]["accountBalanceChange"]["updateTime"] == "2026-10-06T15:30:00.000000Z" &&
        a[0]["accountBalanceChange"]["transactionId"] != a[5]["accountBalanceChange"]["transactionId"]);
  CHECK(a[1]["type"] == "ACTIVITY_TYPE_TRADE" && keys(a[1]) == std::set<std::string>({"type", "trade"}));
  CHECK((keys(a[1]["trade"]) == std::set<std::string>{"id", "marketSlug", "price", "qtyDecimal", "isAggressor", "createTime"}));
  CHECK(a[1]["trade"]["marketSlug"] == DAL && a[1]["trade"]["price"] == money("0.6000") &&
        a[1]["trade"]["qtyDecimal"] == "3" && a[1]["trade"]["isAggressor"] == true);
  CHECK(a[2]["trade"]["marketSlug"] == NE && a[2]["trade"]["qtyDecimal"] == "2" && a[2]["trade"]["isAggressor"] == false &&
        a[2]["trade"]["createTime"] == "2026-10-06T15:30:02.000000Z");
  CHECK(a[3]["trade"]["qtyDecimal"] == "4" && a[4]["trade"]["qtyDecimal"] == "5" && a[4]["trade"]["price"] == money("0.3850"));
  std::set<std::string> ids;
  for (int i = 1; i <= 4; ++i) ids.insert(a[i]["trade"]["id"].get<std::string>());
  CHECK(ids.size() == 4);
  // paging: two at a time to the end, each page after the cursor of the one before
  std::vector<json> walked;
  std::string cursor;
  for (int page = 0; page < 5; ++page) {
    r = get(c, "/v1/portfolio/activities?limit=2" + (cursor.empty() ? "" : "&cursor=" + cursor));
    CHECK(r.status() == 200);
    for (const auto& x : r.j["activities"]) walked.push_back(x);
    if (r.j["eof"] == true) {
      CHECK(r.j["nextCursor"] == "");
      break;
    }
    cursor = r.j["nextCursor"].get<std::string>();
    CHECK(!cursor.empty());
  }
  CHECK(walked.size() == 6 && json(walked) == a);
  r = get(c, "/v1/portfolio/activities?cursor=nope");
  CHECK(r.status() == 400 && r.j["code"] == 3);
  r = get(c, "/v1/portfolio/activities?limit=1001");
  CHECK(r.status() == 400);
  // the signed routes, logged as such
  CHECK(logged(v.f, "http 200 GET /v1/portfolio/activities (signed)") && logged(v.f, "http 404 GET /v1/order/nope (signed)"));
  r = c.request("POST", "/v1/order/" + ioc, "{}", pm_auth("POST", "/v1/order/" + ioc, wall_ms()));
  CHECK(r.status() == 405);
}

// ---- auth ---------------------------------------------------------------------------------------

void test_auth() {
  // RFC 8032's first vector: the test's signer is plain Ed25519, as docs/polymarket.md has it
  {
    Key rfc(unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
    CHECK(rfc.public_raw() == unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    CHECK(rfc.sign_raw("") == unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39"
                                    "701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
  }
  Key key, other;
  CHECK(key.k && other.k && key.public_b64().size() == 44);
  FakePolyConfig cfg;
  cfg.auth = AuthMode::VERIFY;
  cfg.key_id = KEY_ID;
  cfg.public_key_b64 = key.public_b64();
  Venue v(cfg);
  CHECK(v.ok);
  HttpConn c(v.port());
  const std::string bal = "/v1/account/balances", pos = "/v1/portfolio/positions";
  auto status = [&](const std::string& target, const Headers& h, const std::string& method = "GET") {
    return c.request(method, target, "", h).status();
  };
  CHECK(status(bal, pm_auth("GET", bal, wall_ms(), &key)) == 200);
  // timestamp + METHOD + path, the path without its query
  CHECK(status(pos + "?limit=5", pm_auth("GET", pos, wall_ms(), &key)) == 200);
  auto r = c.request("GET", pos + "?limit=5", "", pm_auth("GET", pos + "?limit=5", wall_ms(), &key));
  CHECK(r.status() == 401 && r.j == json({{"code", 16}, {"message", "invalid signature"}}));
  CHECK(status(bal, pm_auth("POST", bal, wall_ms(), &key)) == 401);    // signed for another method
  CHECK(status(bal, pm_auth("GET", pos, wall_ms(), &key)) == 401);     // or another path
  CHECK(status(bal, pm_auth("GET", bal, wall_ms(), &other)) == 401);   // or by another key
  CHECK(status(bal, pm_auth("GET", bal, wall_ms())) == 401);           // a made-up signature
  {
    Headers h = pm_auth("GET", bal, wall_ms(), &key);
    h[2].second = net::base64(std::string(64, '\0'));
    CHECK(status(bal, h) == 401);
    h[2].second = "not base64!";
    CHECK(status(bal, h) == 401);
  }
  // the 30 s window, both ways
  CHECK(status(bal, pm_auth("GET", bal, wall_ms() - 25000, &key)) == 200);
  CHECK(status(bal, pm_auth("GET", bal, wall_ms() + 25000, &key)) == 200);
  r = c.request("GET", bal, "", pm_auth("GET", bal, wall_ms() - 35000, &key));
  CHECK(r.status() == 401 && r.j["message"] == "X-PM-Timestamp is too far from the exchange clock");
  CHECK(status(bal, pm_auth("GET", bal, wall_ms() + 35000, &key)) == 401);
  // a key id the venue does not know is refused before its signature is looked at
  r = c.request("GET", bal, "", pm_auth("GET", bal, wall_ms(), &key, "b0b0b0b0-0000-4000-8000-000000000000"));
  CHECK(r.status() == 401 && r.j == json({{"code", 16}, {"message", "API key not found"}}));
  r = c.request("GET", bal, "", pm_auth("GET", bal, wall_ms(), nullptr, "b0b0b0b0-0000-4000-8000-000000000000"));
  CHECK(r.j["message"] == "API key not found");
  // every header, and a timestamp in milliseconds
  r = c.request("GET", bal);
  CHECK(r.status() == 401 && r.j["message"] == "X-PM-Access-Key, X-PM-Timestamp and X-PM-Signature are required");
  {
    Headers h = pm_auth("GET", bal, wall_ms(), &key);
    h.pop_back();
    CHECK(status(bal, h) == 401);
    h = pm_auth("GET", bal, wall_ms(), &key);
    h[1].second = "1.5e12";
    CHECK(status(bal, h) == 401);
    h[1].second = "1234567890123456";
    CHECK(status(bal, h) == 401);
  }
  // every signed route checks; no public one does
  for (const std::string& t : {pos, std::string("/v1/portfolio/activities"), std::string("/v1/orders/open"),
                               std::string("/v1/order/x"), std::string("/v1/orders"), std::string("/v1/account/x")})
    CHECK(status(t, {}) == 401);
  CHECK(status("/v1/order/x", pm_auth("GET", "/v1/order/x", wall_ms(), &key)) == 404);
  CHECK(status("/v2/leagues/nfl/events", {}) == 200 && status("/v1/markets?slug=" + NE, {}) == 200 &&
        status("/v1/markets/" + NE + "/book", {}) == 200);
  CHECK(logged(v.f, "http 200 GET /v2/leagues/nfl/events") && logged(v.f, "http 401 GET /v1/account/balances (signed)") &&
        logged(v.f, "http 401 GET /v1/account/balances"));
  // VERIFY with any key id when none is configured; never without the public half
  {
    FakePolyConfig any = cfg;
    any.key_id = "";
    Venue w(any);
    HttpConn d(w.port());
    CHECK(d.request("GET", bal, "", pm_auth("GET", bal, wall_ms(), &key, "whatever")).status() == 200);
    CHECK(d.request("GET", bal, "", pm_auth("GET", bal, wall_ms(), &other, "whatever")).status() == 401);
  }
  for (const std::string& pub : {std::string(), std::string("c2lnbmF0dXJl"), net::base64(std::string(31, 'x'))}) {
    FakePolyConfig nk = cfg;
    nk.public_key_b64 = pub;
    FakePoly f(nk);
    std::string err;
    CHECK(!f.start(&err) && err.find("public_key_b64") != std::string::npos);
  }
  // PRESENCE takes any signature, still within the window; OFF takes none
  {
    FakePolyConfig pc;
    Venue w(pc);
    HttpConn d(w.port());
    CHECK(d.request("GET", bal, "", pm_auth("GET", bal, wall_ms(), &other, "anyone")).status() == 200);
    CHECK(d.request("GET", bal, "", pm_auth("GET", bal, wall_ms() - 31000)).status() == 401);
    CHECK(d.request("GET", bal).status() == 401);
    FakePolyConfig off;
    off.auth = AuthMode::OFF;
    Venue x(off);
    HttpConn e(x.port());
    CHECK(e.request("GET", bal).status() == 200);
  }
}

// ---- the writes ---------------------------------------------------------------------------------

const std::string BUY = "ORDER_ACTION_BUY", SELL = "ORDER_ACTION_SELL";
const std::string IOC = "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL", FOK = "TIME_IN_FORCE_FILL_OR_KILL",
                  GTC = "TIME_IN_FORCE_GOOD_TILL_CANCEL", GTD = "TIME_IN_FORCE_GOOD_TILL_DATE";

// A create as deskd sends it: the old desk's body (src/poly.cpp poly_order_body), asynchronous
json order_body(const std::string& slug, const std::string& action, const std::string& px, const json& qty,
                const std::string& tif = IOC, bool manual = true) {
  return json{{"marketSlug", slug},
              {"type", "ORDER_TYPE_LIMIT"},
              {"price", money(px)},
              {"quantity", qty},
              {"tif", tif},
              {"outcomeSide", "OUTCOME_SIDE_YES"},
              {"action", action},
              {"manualOrderIndicator", manual ? "MANUAL_ORDER_INDICATOR_MANUAL" : "MANUAL_ORDER_INDICATOR_AUTOMATIC"}};
}
// A GTD post-only expiring at `exp` (RFC 3339), AUTOMATIC as a Systematic order goes
json post_body(const std::string& slug, const std::string& action, const std::string& px, const json& qty,
               const std::string& exp) {
  json b = order_body(slug, action, px, qty, GTD, false);
  b["participateDontInitiate"] = true;
  b["expireTime"] = exp;
  return b;
}

// A signed POST (any signature passes PRESENCE)
HttpResult post(HttpConn& c, const std::string& path, const json& body) {
  return c.request("POST", path, body.dump(), pm_auth("POST", path, wall_ms()));
}
HttpResult post_raw(HttpConn& c, const std::string& path, const std::string& body) {
  return c.request("POST", path, body, pm_auth("POST", path, wall_ms()));
}
json order_of(HttpConn& c, const std::string& id) { return get(c, "/v1/order/" + id).j["order"]; }
std::size_t engine_orders(FakePoly& f) {
  return f.with_engine([](Engine& e, std::int64_t) { return e.user_orders().size(); });
}

FakePolyConfig manual_clock() {
  FakePolyConfig cfg;
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  cfg.auth_max_skew_ms = 0;  // the fake's clock is T0; the client signs with the wall clock
  return cfg;
}

void test_create() {
  FakePolyConfig cfg = manual_clock();
  cfg.async_delay_ms = 200;
  Venue v(cfg);
  CHECK(v.ok);
  HttpConn c(v.port());
  // BUY 5 at 0.385 IOC: accepted, and nothing yet: no execution, no book change, no trade
  auto r = post(c, "/v1/orders", order_body(NE, BUY, "0.385", 5));
  CHECK(r.status() == 200 && keys(r.j) == std::set<std::string>({"id", "executions"}) && r.j["executions"] == json::array());
  const std::string ioc = r.j["id"].get<std::string>();
  CHECK(ioc.size() == 36 && v.f.engine_id(ioc).empty() && engine_orders(v.f) == 0);
  json o = order_of(c, ioc);
  CHECK((keys(o) == std::set<std::string>{"id", "marketSlug", "type", "side", "tif", "state", "price", "quantity",
                                          "cumQuantity", "leavesQuantity", "commissionNotionalTotalCollected",
                                          "createTime", "updateTime"}));
  CHECK(o["id"] == ioc && o["marketSlug"] == NE && o["side"] == "ORDER_SIDE_BUY" && o["tif"] == IOC);
  CHECK(o["state"] == "ORDER_STATE_PENDING_NEW" && o["price"] == money("0.3850") && o["quantity"] == 5 &&
        o["cumQuantity"] == 0 && o["leavesQuantity"] == 5 && o["createTime"] == "2026-10-06T15:30:00.000000Z");
  r = get(c, "/v1/orders/open");
  CHECK(r.j["orders"].size() == 1 && r.j["orders"][0] == o);
  r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.j["marketData"]["asks"][0] == json::parse(R"({"px":{"value":"0.3850","currency":"USD"},"qty":"9.00"})"));
  CHECK(get(c, "/v1/portfolio/activities").j["activities"].size() == 1);  // the deposit
  // the engine takes it 200 ms on: 5 of the 9 offered and a 7c fee, the trade at the take
  v.f.advance_ms(199);
  CHECK(order_of(c, ioc)["state"] == "ORDER_STATE_PENDING_NEW");
  v.f.advance_ms(1);
  o = order_of(c, ioc);
  CHECK(o["id"] == ioc && o["state"] == "ORDER_STATE_FILLED" && o["cumQuantity"] == 5 && o["leavesQuantity"] == 0);
  CHECK(o["avgPx"] == money("0.3850") && o["commissionNotionalTotalCollected"] == money("0.0700"));
  CHECK(o["createTime"] == "2026-10-06T15:30:00.000000Z" && o["updateTime"] == "2026-10-06T15:30:00.200000Z");
  const std::string eid = v.f.engine_id(ioc);
  CHECK(!eid.empty() && eid != ioc);
  CHECK(get(c, "/v1/order/" + eid).status() == 404);  // the engine's id is not the venue's
  r = get(c, "/v1/portfolio/activities");
  CHECK(r.j["activities"].size() == 2 && r.j["activities"][0]["trade"]["qtyDecimal"] == "5" &&
        r.j["activities"][0]["trade"]["isAggressor"] == true &&
        r.j["activities"][0]["trade"]["createTime"] == "2026-10-06T15:30:00.200000Z");
  CHECK(get(c, "/v1/orders/open").j["orders"].empty());
  // what was sent is kept, as it came
  CHECK(v.f.sent(ioc) == order_body(NE, BUY, "0.385", 5) && v.f.sent("nope").is_null());

  // a GTD post rests until its expireTime, then expires
  v.f.set_async_delay_ms(0);
  r = post(c, "/v1/orders", post_body(NE, BUY, "0.370", "3", "2026-10-06T15:31:00Z"));
  CHECK(r.status() == 200 && r.j["executions"].empty());
  const std::string gtd = r.j["id"].get<std::string>();
  o = order_of(c, gtd);  // no delay: the engine took it before the reply
  CHECK(o["state"] == "ORDER_STATE_NEW" && o["tif"] == GTD && o["quantity"] == 3 && o["leavesQuantity"] == 3);
  CHECK(v.f.sent(gtd)["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_AUTOMATIC" &&
        v.f.sent(gtd)["participateDontInitiate"] == true);
  r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.j["marketData"]["bids"].size() == 3 &&
        r.j["marketData"]["bids"][2] == json::parse(R"({"px":{"value":"0.3700","currency":"USD"},"qty":"3.00"})"));
  v.f.advance_ms(59799);  // 15:30:59.999
  CHECK(order_of(c, gtd)["state"] == "ORDER_STATE_NEW");
  v.f.advance_ms(1);
  o = order_of(c, gtd);
  CHECK(o["state"] == "ORDER_STATE_EXPIRED" && o["leavesQuantity"] == 0 && get(c, "/v1/orders/open").j["orders"].empty());

  // synchronous: taken at once, the reply's executions as the old desk read them, one per fill,
  // each tradeId its trade activity's id: 4 at 0.385 (6c) and 2 at 0.390 (3c)
  json sync = order_body(NE, BUY, "0.390", 6.0);
  sync["synchronousExecution"] = true;
  sync["maxBlockTime"] = "3";
  r = post(c, "/v1/orders", sync);
  CHECK(r.status() == 200 && r.j["executions"].size() == 2);
  json ex = r.j["executions"];
  CHECK((keys(ex[0]) == std::set<std::string>{"tradeId", "lastShares", "lastPx", "commissionNotionalCollected", "order"}));
  CHECK(ex[0]["lastShares"] == "4.00" && ex[0]["lastPx"] == money("0.3850") &&
        ex[0]["commissionNotionalCollected"] == money("0.0600") && ex[0]["order"] == json({{"state", "ORDER_STATE_FILLED"}}));
  CHECK(ex[1]["lastShares"] == "2.00" && ex[1]["lastPx"] == money("0.3900") &&
        ex[1]["commissionNotionalCollected"] == money("0.0300"));
  r = get(c, "/v1/portfolio/activities?limit=2");
  CHECK(r.j["activities"][0]["trade"]["id"] == ex[1]["tradeId"] && r.j["activities"][1]["trade"]["id"] == ex[0]["tradeId"]);
  // FOK: 20 wanted, 3 offered: nothing
  r = post(c, "/v1/orders", order_body(NE, BUY, "0.390", "20", FOK));
  o = order_of(c, r.j["id"].get<std::string>());
  CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["cumQuantity"] == 0 && o["tif"] == FOK);

  // no self-trade prevention: our SELL meets our resting BUY, two trades of ours
  r = post(c, "/v1/orders", order_body(DAL, BUY, "0.600", 2, GTC));
  const std::string bid = r.j["id"].get<std::string>();
  CHECK(order_of(c, bid)["state"] == "ORDER_STATE_NEW" && order_of(c, bid)["tif"] == GTC);
  r = post(c, "/v1/orders", order_body(DAL, SELL, "0.600", 2));
  CHECK(order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_FILLED" &&
        order_of(c, bid)["state"] == "ORDER_STATE_FILLED");
  r = get(c, "/v1/portfolio/activities?limit=2");
  json t0 = r.j["activities"][0]["trade"], t1 = r.j["activities"][1]["trade"];
  CHECK(t0["marketSlug"] == DAL && t1["marketSlug"] == DAL && t0["id"] != t1["id"] &&
        t0["isAggressor"] != t1["isAggressor"]);
  CHECK(!get(c, "/v1/portfolio/positions").j["positions"].contains(DAL));

  // what the engine refuses when it takes the order: REJECTED, never open
  v.f.set_async_delay_ms(100);
  r = post(c, "/v1/orders", order_body(NE, BUY, "0.380", 500, GTC));  // $190 against some $95
  const std::string broke = r.j["id"].get<std::string>();
  CHECK(r.status() == 200 && order_of(c, broke)["state"] == "ORDER_STATE_PENDING_NEW");
  r = post(c, "/v1/orders", order_body(DAL, BUY, "0.300", 1, GTC));
  const std::string paused = r.j["id"].get<std::string>();
  v.f.with_engine([](Engine& e, std::int64_t now) { e.pause(DAL, now); });
  v.f.advance_ms(100);
  o = order_of(c, broke);
  CHECK(o["state"] == "ORDER_STATE_REJECTED" && o["cumQuantity"] == 0 && o["leavesQuantity"] == 0 &&
        o["updateTime"] == "2026-10-06T15:31:00.100000Z");
  CHECK(order_of(c, paused)["state"] == "ORDER_STATE_REJECTED" && v.f.engine_id(paused).empty());
  CHECK(get(c, "/v1/orders/open").j["orders"].empty());
  CHECK(logged(v.f, "http 200 POST /v1/orders (signed)"));
}

void test_create_refusals() {
  FakePolyConfig cfg = manual_clock();
  cfg.engine.min_expiry_lead_s = 60;  // the GTD minimum lead tools/poly_checks reads off the venue
  Venue v(cfg);
  CHECK(v.ok);
  HttpConn c(v.port());
  auto refused = [&](const json& b, int status, const std::string& message) {
    const auto r = post(c, "/v1/orders", b);
    const bool ok = r.status() == status && r.j == json({{"code", status == 404 ? 5 : 3}, {"message", message}});
    if (!ok) std::fprintf(stderr, "refusal: %d %s\n", r.status(), r.res.body.c_str());
    return ok;
  };
  const json good = order_body(NE, BUY, "0.385", 1);
  auto with = [&](const char* k, const json& val) {
    json b = good;
    b[k] = val;
    return b;
  };
  auto without = [&](const char* k) {
    json b = good;
    b.erase(k);
    return b;
  };
  CHECK(post_raw(c, "/v1/orders", "{\"marketSlug\":").status() == 400);
  CHECK(post_raw(c, "/v1/orders", "{\"marketSlug\":").j == json({{"code", 3}, {"message", "the body is not JSON"}}));
  CHECK(refused(json(), 400, "the order must be a JSON object") && refused(json::array({good}), 400, "the order must be a JSON object"));
  CHECK(refused(with("participateDontInitate", true), 400, "unknown field participateDontInitate"));
  CHECK(refused(without("marketSlug"), 400, "marketSlug is required") && refused(with("marketSlug", 5), 400, "marketSlug is required"));
  CHECK(refused(with("marketSlug", "nope"), 404, "market not found"));
  CHECK(refused(with("type", "ORDER_TYPE_MARKET"), 400, "type must be ORDER_TYPE_LIMIT"));
  CHECK(refused(with("action", "BUY"), 400, "action must be ORDER_ACTION_BUY or ORDER_ACTION_SELL"));
  CHECK(refused(without("outcomeSide"), 400, "outcomeSide must be OUTCOME_SIDE_YES") &&
        refused(with("outcomeSide", "OUTCOME_SIDE_NO"), 400, "outcomeSide must be OUTCOME_SIDE_YES"));
  CHECK(refused(without("manualOrderIndicator"), 400,
                "manualOrderIndicator must be MANUAL_ORDER_INDICATOR_MANUAL or MANUAL_ORDER_INDICATOR_AUTOMATIC"));
  CHECK(refused(with("synchronousExecution", "false"), 400, "synchronousExecution must be true or false"));
  CHECK(refused(with("maxBlockTime", json::array()), 400, "maxBlockTime must be a number of seconds"));
  const std::string tifs = "tif must be TIME_IN_FORCE_IMMEDIATE_OR_CANCEL, _FILL_OR_KILL, _GOOD_TILL_CANCEL or _GOOD_TILL_DATE";
  CHECK(refused(without("tif"), 400, tifs) && refused(with("tif", "IOC"), 400, tifs));
  // GTD and its expireTime, both or neither; RFC 3339 in UTC, a whole second, the lead
  CHECK(refused(with("expireTime", "2026-10-06T15:32:00Z"), 400, "expireTime goes only with TIME_IN_FORCE_GOOD_TILL_DATE"));
  CHECK(refused(with("tif", GTD), 400, "expireTime is required with TIME_IN_FORCE_GOOD_TILL_DATE"));
  for (const json& bad : {json("2026-10-06T15:32:00.5Z"), json("2026-10-06 15:32:00Z"), json("2026-10-06T15:32:00+00:00"),
                          json("2026-10-06T15:32:00"), json("2026-13-06T15:32:00Z"), json(1791300720), json("1791300720")}) {
    json b = post_body(NE, BUY, "0.370", 1, "");
    b["expireTime"] = bad;
    CHECK(refused(b, 400, "expireTime must be RFC 3339 in UTC, a whole second"));
  }
  CHECK(refused(post_body(NE, BUY, "0.370", 1, "2026-10-06T15:31:00Z"), 400, "expireTime must be more than 60 s ahead"));
  CHECK(refused(with("participateDontInitiate", true), 400,
                "participateDontInitiate goes only with TIME_IN_FORCE_GOOD_TILL_CANCEL or _GOOD_TILL_DATE"));
  // the price: {value, currency USD}, in (0, 1) on the market's 0.005 grid
  const std::string prices = "price must be {value, currency USD}, the value a decimal string";
  CHECK(refused(with("price", "0.385"), 400, prices) && refused(with("price", 0.385), 400, prices) &&
        refused(with("price", json{{"value", "0.385"}}), 400, prices) &&
        refused(with("price", json{{"value", "0.385"}, {"currency", "EUR"}}), 400, prices) &&
        refused(with("price", money("0.38500")), 400, prices) && refused(with("price", money("-0.385")), 400, prices));
  for (const char* px : {"0.3875", "0", "1", "1.000", "0.996"})
    CHECK(refused(with("price", money(px)), 400, "price must be between 0 and 1 on the market's tick"));
  // whole contracts, at least one
  CHECK(refused(with("quantity", true), 400, "quantity must be a number") && refused(with("quantity", "x"), 400, "quantity must be a number") &&
        refused(without("quantity"), 400, "quantity must be a number") && refused(with("quantity", 1.005), 400, "quantity must be a number"));
  for (const json& q : {json(2.5), json("2.5"), json(0), json(-1), json("-3")})
    CHECK(refused(with("quantity", q), 400, "quantity must be whole contracts, at least the market's minimumTradeQty"));
  // nothing placed by any of them; POST only
  CHECK(engine_orders(v.f) == 0 && get(c, "/v1/orders/open").j["orders"].empty());
  auto r = get(c, "/v1/orders");
  CHECK(r.status() == 405 && r.j == json({{"code", 12}, {"message", "POST only"}}));
  // what passes: the quantity as a string or a float, the lead met, MANUAL post-only GTC
  CHECK(post(c, "/v1/orders", with("quantity", "2.00")).status() == 200 && post(c, "/v1/orders", with("quantity", 2.0)).status() == 200);
  r = post(c, "/v1/orders", post_body(NE, BUY, "0.370", 1, "2026-10-06T15:31:01.000000Z"));
  CHECK(r.status() == 200 && order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_NEW");
  json gtc = order_body(NE, SELL, "0.400", "1", GTC);
  gtc["participateDontInitiate"] = false;
  CHECK(post(c, "/v1/orders", gtc).status() == 200 && get(c, "/v1/orders/open").j["orders"].size() == 2);
}

void test_post_only() {
  // REFUSE_400, the engine's default: refused when synchronous, REJECTED when not
  {
    Venue v(manual_clock());
    HttpConn c(v.port());
    auto r = post(c, "/v1/orders", post_body(NE, BUY, "0.385", 2, "2026-10-06T15:31:00Z"));  // the best offer
    CHECK(r.status() == 200 && order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_REJECTED");
    json sync = post_body(NE, SELL, "0.380", 2, "2026-10-06T15:31:00Z");  // the best bid
    sync["synchronousExecution"] = true;
    r = post(c, "/v1/orders", sync);
    CHECK(r.status() == 400 && r.j == json({{"code", 3}, {"message", "post only cross"}}));
    r = post(c, "/v1/orders", post_body(NE, BUY, "0.380", 2, "2026-10-06T15:31:00Z"));  // joins the bid
    CHECK(order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_NEW");
    CHECK(engine_orders(v.f) == 1 && get(c, "/v1/portfolio/activities").j["activities"].size() == 1);
  }
  // CANCEL: taken, then cancelled at once, either way
  {
    FakePolyConfig cfg = manual_clock();
    cfg.engine.post_only_cross = PostOnlyCross::CANCEL;
    Venue v(cfg);
    HttpConn c(v.port());
    auto r = post(c, "/v1/orders", post_body(NE, BUY, "0.390", 2, "2026-10-06T15:31:00Z"));
    json o = order_of(c, r.j["id"].get<std::string>());
    CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["cumQuantity"] == 0 && o["leavesQuantity"] == 0);
    json sync = post_body(NE, BUY, "0.385", 2, "2026-10-06T15:31:00Z");
    sync["synchronousExecution"] = true;
    r = post(c, "/v1/orders", sync);
    CHECK(r.status() == 200 && r.j["executions"].empty() &&
          order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_CANCELED");
    CHECK(get(c, "/v1/orders/open").j["orders"].empty() && get(c, "/v1/portfolio/activities").j["activities"].size() == 1);
    r = c.request("GET", "/v1/markets/" + NE + "/book");
    CHECK(r.j["marketData"]["asks"][0]["qty"] == "9.00");
  }
}

void test_cancel() {
  Venue v(manual_clock());
  CHECK(v.ok);
  HttpConn c(v.port());
  auto r = post(c, "/v1/orders", order_body(NE, BUY, "0.350", 2, GTC));
  const std::string a = r.j["id"].get<std::string>();
  const std::string cancel_a = "/v1/order/" + a + "/cancel";
  // its market by slug, required and right
  r = post(c, cancel_a, json::object());
  CHECK(r.status() == 400 && r.j == json({{"code", 3}, {"message", "marketSlug is required"}}));
  CHECK(post_raw(c, cancel_a, "").status() == 400 && post_raw(c, cancel_a, "{").j["message"] == "the body is not JSON");
  r = post(c, cancel_a, {{"marketSlug", DAL}});
  CHECK(r.status() == 404 && r.j == json({{"code", 5}, {"message", "order not found"}}));
  r = post(c, cancel_a, {{"marketSlug", NE}});
  CHECK(r.status() == 200 && r.j == json::object());
  json o = order_of(c, a);
  CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["leavesQuantity"] == 0 && get(c, "/v1/orders/open").j["orders"].empty());
  CHECK(post(c, cancel_a, {{"marketSlug", NE}}).status() == 404);  // no longer open
  CHECK(post(c, "/v1/order/nope/cancel", {{"marketSlug", NE}}).status() == 404);
  CHECK(c.request("GET", cancel_a, "", pm_auth("GET", cancel_a, wall_ms())).status() == 405);
  // a filled order is not open either
  r = post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1));
  CHECK(post(c, "/v1/order/" + r.j["id"].get<std::string>() + "/cancel", {{"marketSlug", NE}}).status() == 404);

  // one the engine has not taken is cancelled before it is: it never trades
  v.f.set_async_delay_ms(500);
  r = post(c, "/v1/orders", order_body(NE, BUY, "0.390", 3));
  const std::string b = r.j["id"].get<std::string>();
  CHECK(post(c, "/v1/order/" + b + "/cancel", {{"marketSlug", DAL}}).status() == 404);  // not in that market
  CHECK(order_of(c, b)["state"] == "ORDER_STATE_PENDING_NEW");
  CHECK(post(c, "/v1/order/" + b + "/cancel", {{"marketSlug", NE}}).status() == 200);
  v.f.advance_ms(500);
  o = order_of(c, b);
  CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["cumQuantity"] == 0 && v.f.engine_id(b).empty());
  CHECK(get(c, "/v1/portfolio/activities").j["activities"].size() == 2);  // the deposit and the 1 at 0.385

  // cancel-all: every open order of ours, oldest first, the one not taken yet and one made
  // in the engine by hand among them; the street's stay
  v.f.set_async_delay_ms(0);
  const std::string g1 = post(c, "/v1/orders", order_body(NE, BUY, "0.300", 1, GTC)).j["id"].get<std::string>();
  v.f.advance_ms(1);
  const std::string g2 = post(c, "/v1/orders", post_body(DAL, SELL, "0.700", 2, "2026-10-06T15:40:00Z")).j["id"].get<std::string>();
  v.f.advance_ms(1);
  const std::string by_hand = create(v.f, NE, Side::BID, 100, 3100, Tif::GTC);
  v.f.advance_ms(1);
  v.f.set_async_delay_ms(500);
  const std::string later = post(c, "/v1/orders", order_body(NE, BUY, "0.320", 1, GTC)).j["id"].get<std::string>();
  r = get(c, "/v1/orders/open");
  CHECK(r.j["orders"].size() == 4 && r.j["orders"][0]["id"] == g1 && r.j["orders"][1]["id"] == g2 &&
        r.j["orders"][2]["id"] == by_hand && r.j["orders"][3]["id"] == later &&
        r.j["orders"][3]["state"] == "ORDER_STATE_PENDING_NEW");
  r = post(c, "/v1/orders/open/cancel", json::object());
  CHECK(r.status() == 200 && r.j == json({{"canceledOrderIds", {g1, g2, by_hand, later}}}));
  CHECK(get(c, "/v1/orders/open").j["orders"].empty());
  for (const std::string& id : {g1, g2, by_hand, later}) CHECK(order_of(c, id)["state"] == "ORDER_STATE_CANCELED");
  v.f.advance_ms(500);
  CHECK(order_of(c, later)["state"] == "ORDER_STATE_CANCELED" && v.f.engine_id(later).empty());
  r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.j["marketData"]["bids"].size() == 2 && r.j["marketData"]["bids"][0]["qty"] == "10.00");
  // again: none; no body is a body; anything but an object is not
  r = post_raw(c, "/v1/orders/open/cancel", "");
  CHECK(r.status() == 200 && r.j == json({{"canceledOrderIds", json::array()}}));
  CHECK(post_raw(c, "/v1/orders/open/cancel", "[]").status() == 400);
  // one made in the engine by hand is cancelled by the engine's id, which is its own
  const std::string hand2 = create(v.f, DAL, Side::ASK, 100, 7000, Tif::GTC);
  CHECK(v.f.engine_id(hand2) == hand2 && post(c, "/v1/order/" + hand2 + "/cancel", {{"marketSlug", DAL}}).status() == 200);
  CHECK(logged(v.f, "http 200 POST /v1/orders/open/cancel (signed)") && logged(v.f, "http 200 POST " + cancel_a + " (signed)"));

  // preview: checked as a create is, nothing placed
  const std::size_t before = engine_orders(v.f);
  r = post(c, "/v1/order/preview", {{"request", order_body(NE, BUY, "0.385", 2)}});
  CHECK(r.status() == 200 && r.j == json({{"order", {{"id", ""}, {"commissionsBasisPoints", "600"}}}}));
  r = post(c, "/v1/order/preview", {{"request", order_body(HOU, SELL, "0.41", 2)}});
  CHECK(r.status() == 200 && r.j["order"]["commissionsBasisPoints"] == "695");
  r = post(c, "/v1/order/preview", {{"request", order_body(HOU, SELL, "0.415", 2)}});
  CHECK(r.status() == 400 && r.j["message"] == "price must be between 0 and 1 on the market's tick");
  CHECK(post(c, "/v1/order/preview", order_body(NE, BUY, "0.385", 2)).status() == 400);
  CHECK(post(c, "/v1/order/preview", {{"request", order_body("nope", BUY, "0.385", 2)}}).status() == 404);
  CHECK(engine_orders(v.f) == before && get(c, "/v1/order/preview").status() == 404);
}

void test_faults() {
  Venue v;  // the wall clock: stalls are real
  CHECK(v.ok);
  using K = HttpFault::Kind;
  auto rest2 = [](const std::string& px) { return order_body(DAL, BUY, px, 2, GTC); };
  auto open_now = [&] {
    HttpConn c(v.port());
    return get(c, "/v1/orders/open").j["orders"].size();
  };
  std::size_t open = 0;
  {  // 429: nothing processed, and the connection stays up
    HttpConn c(v.port());
    v.f.add_fault(poly_fault(K::RATE_LIMIT));
    CHECK(v.f.faults_pending() == 1);
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.status() == 429 && r.j == json({{"code", 8}, {"message", "too many requests"}}) &&
          r.res.headers["retry-after"] == "1" && open_now() == open);
    CHECK(post(c, "/v1/orders", rest2("0.300")).status() == 200 && open_now() == ++open && v.f.faults_pending() == 0);
    CHECK(logged(v.f, "http 429 POST /v1/orders (signed) (fault)"));
  }
  {  // dropped before the reply: nothing placed
    HttpConn c(v.port());
    v.f.add_fault(poly_fault(K::DROP_BEFORE_REPLY));
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.dropped && r.status() == 0 && open_now() == open);
  }
  {  // dropped after accepting: the order exists and the client heard nothing (UNKNOWN)
    HttpConn c(v.port());
    v.f.add_fault(poly_fault(K::DROP_AFTER_ACCEPT));
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.dropped && open_now() == ++open && logged(v.f, "http drop-after-accept 200 POST /v1/orders (signed)"));
  }
  for (const int status : {500, 503}) {  // 5xx after accepting
    HttpConn c(v.port());
    HttpFault f = poly_fault(K::STATUS_AFTER_ACCEPT);
    f.status = status;
    v.f.add_fault(f);
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.status() == status && r.j == json({{"code", status == 500 ? 13 : 14}, {"message", "internal server error"}}) &&
          open_now() == ++open);
  }
  {  // a body cut mid-way: the head promised more
    HttpConn c(v.port());
    HttpFault f = poly_fault(K::CUT_BODY);
    f.cut_bytes = 10;
    v.f.add_fault(f);
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.cut && r.status() == 200 && r.res.body.size() == 10 && r.j.is_discarded() && !r.res.complete);
    CHECK(std::stoul(r.res.headers["content-length"]) > 10 && open_now() == ++open);
    CHECK(logged(v.f, "http cut 200 POST /v1/orders (signed)"));
  }
  {  // chunked, and the connection still in step after
    HttpConn c(v.port());
    HttpFault f = poly_fault(K::CHUNKED);
    f.chunk_bytes = 7;
    v.f.add_fault(f);
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.status() == 200 && r.res.chunked && r.res.complete && r.j["id"].is_string() && open_now() == ++open);
    CHECK(logged(v.f, "http 200 POST /v1/orders (signed) (chunked)") && get(c, "/v1/account/balances").status() == 200);
  }
  {  // a stall before: the order is placed only after it
    HttpConn c(v.port());
    HttpFault f = poly_fault(K::STALL_BEFORE);
    f.stall_ms = 300;
    v.f.add_fault(f);
    const std::int64_t t = wall_ms();
    auto r = post(c, "/v1/orders", rest2("0.300"));
    CHECK(r.status() == 200 && r.ms >= 290);
    const std::string eid = v.f.engine_id(r.j["id"].get<std::string>());
    CHECK(v.f.with_engine([&](Engine& e, std::int64_t) { return e.order(eid)->created_ms; }) >= t + 290);
    ++open;
  }
  {  // a stall after: the order rests at once, and a cancel on another connection lands before
     // the reply (plan section 8: "cancel before the ack")
    HttpConn a(v.port()), b(v.port());
    HttpFault f = poly_fault(K::STALL_AFTER);
    f.stall_ms = 600;
    v.f.add_fault(f);
    HttpResult ra;
    std::thread th([&] { ra = post(a, "/v1/orders", rest2("0.290")); });
    std::string id;
    for (int i = 0; i < 100 && id.empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const json orders = get(b, "/v1/orders/open").j["orders"];
      for (const auto& o : orders)
        if (o["price"] == money("0.2900")) id = o["id"].get<std::string>();
    }
    CHECK(!id.empty() && post(b, "/v1/order/" + id + "/cancel", {{"marketSlug", DAL}}).status() == 200);
    th.join();
    CHECK(ra.status() == 200 && ra.j["id"] == id && ra.ms >= 590 && order_of(b, id)["state"] == "ORDER_STATE_CANCELED");
  }
  {  // closed after the reply
    HttpConn c(v.port());
    v.f.add_fault(poly_fault(K::CLOSE_AFTER_REPLY, "GET", "/v1/account"));
    auto r = get(c, "/v1/account/balances");
    CHECK(r.status() == 200 && r.res.headers["connection"] == "close" && c.closed_by_server());
  }
  {  // a fill during a cancel: the street takes 1 of 3 first; the cancel ends the rest
    HttpConn c(v.port());
    const std::string id = post(c, "/v1/orders", order_body(DAL, BUY, "0.280", 3, GTC)).j["id"].get<std::string>();
    HttpFault f = poly_fault(K::FILL_DURING_CANCEL, "POST", "/v1/order/");
    f.qty = 100;
    v.f.add_fault(f);
    auto r = post(c, "/v1/order/" + id + "/cancel", {{"marketSlug", DAL}});
    CHECK(r.status() == 200 && r.j == json::object());
    json o = order_of(c, id);
    CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["cumQuantity"] == 1 && o["leavesQuantity"] == 0 &&
          o["avgPx"] == money("0.2800"));
    json t = get(c, "/v1/portfolio/activities?limit=1").j["activities"][0]["trade"];
    CHECK(t["marketSlug"] == DAL && t["qtyDecimal"] == "1" && t["isAggressor"] == false);
  }
  {  // the fill before the ack: with a delay the engine would wait out, the order is taken and
     // hit before the reply; and an IOC taken and filled before it
    HttpConn c(v.port());
    v.f.set_async_delay_ms(5000);
    HttpFault f = poly_fault(K::HIT_AFTER_REST);
    f.qty = 100;
    v.f.add_fault(f);
    auto r = post(c, "/v1/orders", order_body(DAL, BUY, "0.270", 2, GTC));
    CHECK(r.status() == 200 && r.j["executions"].empty());
    json o = order_of(c, r.j["id"].get<std::string>());
    CHECK(o["state"] == "ORDER_STATE_PARTIALLY_FILLED" && o["cumQuantity"] == 1 && o["leavesQuantity"] == 1);
    v.f.add_fault(poly_fault(K::FILL_BEFORE_REPLY));
    r = post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1));
    CHECK(r.status() == 200 && r.j["executions"].empty() && order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_FILLED");
    r = post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1));  // no fault: it waits
    CHECK(order_of(c, r.j["id"].get<std::string>())["state"] == "ORDER_STATE_PENDING_NEW");
    CHECK(post(c, "/v1/order/" + r.j["id"].get<std::string>() + "/cancel", {{"marketSlug", NE}}).status() == 200);
    v.f.set_async_delay_ms(0);
  }
  {  // the touch pulled just before a create that would take it; logged
    HttpConn c(v.port());
    v.f.with_engine([](Engine& e, std::int64_t now) { e.street_add(DAL, true, 6000, 500, now); });  // 0.400 offered
    v.f.add_fault(poly_fault(K::PULL_TOUCH));
    auto r = post(c, "/v1/orders", order_body(DAL, BUY, "0.400", 2));
    json o = order_of(c, r.j["id"].get<std::string>());
    CHECK(o["state"] == "ORDER_STATE_CANCELED" && o["cumQuantity"] == 0 && logged(v.f, "http pull-touch " + DAL + " 5.00"));
  }
  {  // a fault naming a market passes over another market's creates; one on the cancel path
     // passes over the creates
    HttpConn c(v.port());
    HttpFault f = poly_fault(K::STATUS_AFTER_ACCEPT);
    f.ticker = NE;
    v.f.add_fault(f);
    v.f.add_fault(poly_fault(K::DROP_BEFORE_REPLY, "POST", "/v1/order/"));
    CHECK(post(c, "/v1/orders", rest2("0.200")).status() == 200);
    CHECK(post(c, "/v1/orders", order_body(NE, BUY, "0.300", 1, GTC)).status() == 500 && v.f.faults_pending() == 1);
    v.f.clear_faults();
    CHECK(v.f.faults_pending() == 0);
  }
  {  // every kept-open connection dropped under its client
    HttpConn c(v.port());
    CHECK(get(c, "/v1/orders/open").status() == 200);
    v.f.drop_http_connections();
    CHECK(get(c, "/v1/orders/open").status() == 0);
    HttpConn d(v.port());
    CHECK(get(d, "/v1/orders/open").status() == 200);
  }
}

void test_rate_limit() {
  Venue v(manual_clock());
  CHECK(v.ok);
  v.f.set_rate_limits(true);  // 20 a second, holding a second's worth
  HttpConn c(v.port());
  int ok = 0;
  for (int i = 0; i < 20; ++i) ok += c.request("GET", "/v1/markets?slug=" + NE).status() == 200;
  CHECK(ok == 20);
  auto r = c.request("GET", "/v2/leagues/nfl/events");
  CHECK(r.status() == 429 && r.j == json({{"code", 8}, {"message", "too many requests"}}) && r.res.headers["retry-after"] == "1");
  // one pool for both hosts: a signed create is refused too, and places nothing
  r = post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1));
  CHECK(r.status() == 429 && engine_orders(v.f) == 0 && logged(v.f, "http 429 POST /v1/orders (signed)"));
  v.f.advance_ms(50);  // one back
  CHECK(post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1)).status() == 200 && engine_orders(v.f) == 1);
  CHECK(get(c, "/v1/account/balances").status() == 429);
  v.f.advance_ms(5000);  // never more than a second's worth
  ok = 0;
  for (int i = 0; i < 25; ++i) ok += get(c, "/v1/account/balances").status() == 200;
  CHECK(ok == 20);
  // another rate
  v.f.set_rate_limits(true, 5);
  ok = 0;
  for (int i = 0; i < 6; ++i) ok += c.request("GET", "/v1/markets").status() == 200;
  CHECK(ok == 5);
  v.f.set_rate_limits(false);
  ok = 0;
  for (int i = 0; i < 50; ++i) ok += c.request("GET", "/v1/markets").status() == 200;
  CHECK(ok == 50);
  // a venue made without a Retry-After
  FakePolyConfig cfg = manual_clock();
  cfg.retry_after_s = 0;
  FakePoly f(cfg);  // its limit on: Venue's is off
  std::string err;
  CHECK(f.start(&err));
  HttpConn d(f.port());
  for (int i = 0; i < 20; ++i) d.request("GET", "/v1/markets");
  r = d.request("GET", "/v1/markets");
  CHECK(r.status() == 429 && !r.res.headers.count("retry-after"));
}

void test_maintenance() {
  Venue v(manual_clock());
  CHECK(v.ok);
  HttpConn c(v.port());
  const std::string id = post(c, "/v1/orders", post_body(NE, BUY, "0.370", 2, "2026-10-06T15:33:00Z")).j["id"].get<std::string>();
  v.f.set_maintenance(T0 + 1000, T0 + 61000);
  v.f.advance_ms(999);
  CHECK(c.request("GET", "/v1/markets").status() == 200);
  v.f.advance_ms(1);
  // every request, either host, signed or not, before auth: 503, and nothing done
  const std::string down = json({{"code", 14}, {"message", "the exchange is down for maintenance"}}).dump();
  auto r = c.request("GET", "/v1/markets/" + NE + "/book");
  CHECK(r.status() == 503 && r.res.body == down);
  CHECK(get(c, "/v1/account/balances").status() == 503 && c.request("GET", "/v1/account/balances").status() == 503);
  CHECK(post(c, "/v1/orders", order_body(NE, BUY, "0.385", 1)).status() == 503);
  CHECK(post(c, "/v1/order/" + id + "/cancel", {{"marketSlug", NE}}).status() == 503);
  CHECK(post(c, "/v1/orders/open/cancel", json::object()).status() == 503);
  CHECK(logged(v.f, "http 503 GET /v1/markets/" + NE + "/book") && logged(v.f, "http 503 POST /v1/orders (signed)"));
  // the engine runs on: the post still rests, and nothing new is in it
  const std::string eid = v.f.engine_id(id);
  CHECK(v.f.with_engine([&](Engine& e, std::int64_t) { return e.user_orders().size() == 1 && e.order(eid)->status == OrdStatus::RESTING; }));
  v.f.advance_ms(59999);
  CHECK(c.request("GET", "/v1/markets").status() == 503);
  v.f.advance_ms(1);
  CHECK(c.request("GET", "/v1/markets").status() == 200 && order_of(c, id)["state"] == "ORDER_STATE_NEW");
  // it expires on time in a window, and is seen expired after
  v.f.set_maintenance(T0 + 61000, T0 + 300000);
  v.f.advance_ms(120000);  // 15:33:01
  CHECK(v.f.with_engine([&](Engine& e, std::int64_t) { return e.order(eid)->reason == "ExpiryCancel"; }));
  CHECK(get(c, "/v1/orders/open").status() == 503);
  v.f.set_maintenance(0, 0);
  CHECK(order_of(c, id)["state"] == "ORDER_STATE_EXPIRED");
}

// ---- TLS, kept-open connections ----------------------------------------------------------------

// One GET over TLS on a connection of its own, trusting `ca` for 127.0.0.1: the reply, or "".
std::string tls_get(int port, const std::string& ca, const std::string& target) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  std::string reply;
  const int fd = net::connect_loopback(port, 2000);
  if (ctx && fd >= 0 && SSL_CTX_load_verify_locations(ctx, ca.c_str(), nullptr) == 1) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    SSL* ssl = SSL_new(ctx);
    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), "127.0.0.1");
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
      const std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
      SSL_write(ssl, req.data(), static_cast<int>(req.size()));
      char buf[4096];
      int n;
      while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) reply.append(buf, static_cast<std::size_t>(n));
    }
    SSL_free(ssl);
  }
  if (fd >= 0) ::close(fd);
  SSL_CTX_free(ctx);
  return reply;
}

void test_tls() {
  TempDir dir;
  TestPki pki;
  std::string err;
  CHECK(make_test_pki(dir.path, &pki, &err));
  FakePolyConfig cfg;
  cfg.tls_cert_file = pki.server_chain;
  cfg.tls_key_file = pki.server_key;
  Venue v(cfg);
  CHECK(v.ok && v.f.tls() && v.f.base() == "https://127.0.0.1:" + std::to_string(v.port()));
  const std::string reply = tls_get(v.port(), pki.ca_cert, "/v1/markets/" + NE + "/book");
  CHECK(reply.compare(0, 15, "HTTP/1.1 200 OK") == 0 && reply.find("\"marketData\"") != std::string::npos);
  CHECK(reply.find("\r\nDate: ") != std::string::npos);
  CHECK(logged(v.f, "http 200 GET /v1/markets/" + NE + "/book"));
  // a plain client gets no HTTP from a TLS listener
  HttpConn plain(v.port(), 500);
  CHECK(plain.request("GET", "/v1/markets", "", {}, 1000).status() == 0);
  // a chain that will not load refuses to start
  FakePolyConfig bad;
  bad.tls_cert_file = dir.file("missing.pem");
  bad.tls_key_file = pki.server_key;
  FakePoly f(bad);
  CHECK(!f.start(&err) && err.find("TLS listener") != std::string::npos);
  // plain: many requests on one kept-open connection, then the idle close
  FakePolyConfig idle;
  idle.idle_close_ms = 300;
  Venue w(idle);
  HttpConn c(w.port());
  for (int i = 0; i < 20; ++i) CHECK(c.request("GET", "/v1/markets?slug=" + NE).status() == 200);
  CHECK(w.f.connections_accepted() == 1 && w.f.http_requests() == 20);
  CHECK(c.closed_by_server(2000) && w.f.idle_closes() == 1 && logged(w.f, "http idle-close"));
}

// ---- fake-kalshi --poly -------------------------------------------------------------------------

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
  int out = -1;
  const int pid = spawn({FAKE_KALSHI_BIN, "--port", "0", "--poly", "--poly-cash", "250.5", "--no-rate-limits"}, &out);
  CHECK(pid > 0);
  if (pid <= 0) return;
  const std::string head = read_until(out, "POLY http://", 5000);
  CHECK(head.find("REST http://127.0.0.1:") != std::string::npos);
  const auto at = head.find("POLY http://127.0.0.1:");
  CHECK(at != std::string::npos);
  const int port = at == std::string::npos ? 0 : std::atoi(head.c_str() + at + 22);
  CHECK(port > 0);
  HttpConn c(port);
  auto r = get(c, "/v1/account/balances");  // --auth presence by default
  CHECK(r.status() == 200 && r.j["balances"][0]["currentBalance"] == 250.5);
  CHECK(c.request("GET", "/v1/account/balances").status() == 401);
  r = c.request("GET", "/v2/leagues/nfl/events");
  CHECK(r.status() == 200 && r.j["events"].empty());
  r = get(c, "/v1/portfolio/activities");
  CHECK(r.j["activities"].size() == 1 && r.j["activities"][0]["type"] == "ACTIVITY_TYPE_ACCOUNT_DEPOSIT");
  // --no-rate-limits reaches it too: past its 20 a second
  int ok = 0;
  for (int i = 0; i < 25; ++i) ok += c.request("GET", "/v1/markets").status() == 200;
  CHECK(ok == 25);
  c.close();
  ::kill(pid, SIGTERM);
  int st = 0;
  ::waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(read_until(out, "connections\n", 2000).find("POLY stopped after 29 requests on 1 connections") != std::string::npos);
  ::close(out);
  // --auth and --tls apply to both venues: $100 by default, unsigned, over TLS
  TempDir dir;
  const int tls = spawn({FAKE_KALSHI_BIN, "--port", "0", "--poly", "--auth", "off", "--tls", dir.path}, &out);
  const std::string h1 = read_until(out, "POLY https://", 5000);
  const auto at1 = h1.find("POLY https://127.0.0.1:");
  CHECK(at1 != std::string::npos && h1.find("REST https://") != std::string::npos);
  const std::string reply =
      tls_get(at1 == std::string::npos ? 0 : std::atoi(h1.c_str() + at1 + 23), dir.file("ca.pem"), "/v1/account/balances");
  CHECK(reply.compare(0, 15, "HTTP/1.1 200 OK") == 0 && reply.find("\"currentBalance\":100.0") != std::string::npos);
  ::kill(tls, SIGTERM);
  ::waitpid(tls, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  ::close(out);
  // without --poly, no Polymarket; a bad --poly-cash is a usage error
  const int plain = spawn({FAKE_KALSHI_BIN, "--port", "0"}, &out);
  const std::string h2 = read_until(out, "WS   ws://", 5000);
  CHECK(h2.find("REST http://") != std::string::npos && h2.find("POLY") == std::string::npos);
  ::kill(plain, SIGTERM);
  ::waitpid(plain, &st, 0);
  CHECK(read_until(out, "stopped after", 2000).find("POLY") == std::string::npos);
  ::close(out);
  const int bad = spawn({FAKE_KALSHI_BIN, "--poly", "--poly-cash", "-5"}, &out);
  CHECK(read_until(out, "--poly-cash DOLLARS", 2000).find("usage: fake-kalshi") != std::string::npos);
  ::waitpid(bad, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
  ::close(out);
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_markets();
  test_account();
  test_auth();
  test_create();
  test_create_refusals();
  test_post_only();
  test_cancel();
  test_faults();
  test_rate_limit();
  test_maintenance();
  test_tls();
  test_binary();
  return finish("deskd-fake-poly");
}
