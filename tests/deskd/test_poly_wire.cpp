// deskd gw/poly_wire and gw/poly_sign: Polymarket US on the wire.
//
//   - values: prices as deskd writes them, money in the venue's three forms, sizes, RFC 3339 times
//   - builders: intents in venue terms (TAKE IOC or FOK, POST GTD post-only no sooner than the
//     venue's minimum lead), the create body field by field, every refusal of check_create,
//     cancels, cancel-all, preview and the reads' paths and queries
//   - parsers: the old desk's mock shapes (tests/test_poly.cpp) and the fake's, a missing field
//     nullopt (never 0), a malformed row failing the whole reply
//   - the ledger's rows: each state, the cost of what the side bought, end_why, a fill's fee
//   - classify_write: ACK, REFUSED with its class, UNKNOWN (a cut, a 5xx, a 408, no venue body,
//     an unreadable 2xx), NOT_SENT
//   - fees: the worked rebate rows, and the taker allowance against every way an order of up to
//     12 contracts can split into fills, at every limit on the 0.005 grid
//   - the maintenance window, Thursday 02:00-06:00 ET, on both sides of both DST changes
//   - the signer: an Ed25519 known answer (RFC 8032 key 1, computed with Python's cryptography
//     50.0.1), the query never signed, 32- and 64-byte secrets, a 64-byte secret with another
//     key's public half, the file's mode, and no secret in any error
//   - round trips against the fake Polymarket (its own parsers and Ed25519 check, a key made
//     here): the public reads unsigned, an asynchronous IOC that fills, a GTD post that rests and
//     is cancelled, cancel-all, preview, the synchronous fallback, the soft refusals, a create off
//     the grid or under the market's minimum, maintenance, and a key the venue does not know
#include <openssl/evp.h>
#include <sys/stat.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/gw/poly_sign.h"
#include "deskd/gw/poly_wire.h"
#include "fake_poly.h"
#include "net_fixture.h"

using namespace deskd;
using json = nlohmann::json;
using K = kalshi::WriteOutcome::Kind;

namespace {

constexpr std::int64_t T0 = 1791300600000;   // 2026-10-06T15:30:00Z, a Tuesday
const std::string NE = "aec-nfl-ne-sea-2026-09-09";
const std::string DAL = "aec-nfl-dal-nyg-2026-09-13";
const std::string KEY_ID = "a422f5a3-0000-4000-8000-000000000000";
// RFC 8032 section 7.1 test 1's secret key: a published test vector, not a credential
const std::string RFC_SEED = "nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A=";
const std::string RFC_PUB = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";

std::int64_t at(const std::string& rfc3339) { return *kalshi::parse_time_ms(rfc3339); }

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

net::Exchange replied(int status, const std::string& body) {
  net::Exchange x;
  x.fate = net::Exchange::Fate::REPLIED;
  x.res.status = status;
  x.res.body = body;
  return x;
}

json usd(const std::string& v) { return json{{"value", v}, {"currency", "USD"}}; }

json team(const std::string& abbr, const std::string& name, const std::string& safe, const std::string& alias) {
  return json{{"abbreviation", abbr}, {"name", name}, {"safeName", safe}, {"alias", alias}, {"league", "nfl"}};
}
// A game-winner market as /v2/leagues/{league}/events lists it (tests/test_poly.cpp pm_winner),
// its short side listed first.
json pm_winner(const std::string& slug) {
  json sides = json::array();
  sides.push_back({{"description", "Seahawks"}, {"long", false}, {"team", team("SEA", "Seattle Seahawks", "Seattle", "Seahawks")},
                   {"price", "0.6200"}});
  sides.push_back({{"description", "Patriots"}, {"long", true}, {"team", team("NE", "New England Patriots", "New England", "Patriots")},
                   {"price", "0.3850"}});
  return json{{"slug", slug},
              {"question", "Who will win Patriots vs Seahawks?"},
              {"sportsMarketType", "football_team_full_game_winner"},
              {"orderPriceMinTickSize", 0.005},
              {"minimumTradeQty", 1},
              {"feeCoefficient", 0.06},
              {"active", true},
              {"closed", false},
              {"bestBidQuote", usd("0.3800")},
              {"bestAskQuote", usd("0.385")},
              {"gameStartTime", "2026-09-10T00:20:00Z"},
              {"endDate", "2026-09-24T00:20:00Z"},
              {"updatedAt", "2026-09-08T20:00:00.123Z"},
              {"marketSides", sides}};
}
// An order as GET /v1/order/{id} lists it (the fake's shape, fake_poly.cpp order_json)
json pm_order(const std::string& state, double qty, double cum, double leaves, const char* avg = nullptr) {
  json o{{"id", "po-1"},
         {"marketSlug", NE},
         {"type", "ORDER_TYPE_LIMIT"},
         {"side", "ORDER_SIDE_BUY"},
         {"tif", "TIME_IN_FORCE_GOOD_TILL_DATE"},
         {"state", state},
         {"price", usd("0.3850")},
         {"quantity", qty},
         {"cumQuantity", cum},
         {"leavesQuantity", leaves},
         {"commissionNotionalTotalCollected", usd("0.0300")},
         {"createTime", "2026-10-06T15:30:00Z"},
         {"updateTime", "2026-10-06T15:30:01.500Z"}};
  if (avg) o["avgPx"] = usd(avg);
  return o;
}
json trade_activity(const std::string& id, const char* price, const char* qty, bool aggressor) {
  return json{{"type", "ACTIVITY_TYPE_TRADE"},
              {"trade",
               {{"id", id}, {"marketSlug", NE}, {"price", usd(price)}, {"qtyDecimal", qty}, {"isAggressor", aggressor},
                {"createTime", "2026-10-06T15:30:02Z"}}}};
}

PlaceIntent intent(Owner owner, Side side, Px px, Qty count, Tif tif) {
  PlaceIntent in;
  in.cid = "M-1";
  in.owner = owner;
  in.venue = Venue::P;
  in.ticker = NE;
  in.side = side;
  in.price = px;
  in.count = count;
  in.tif = tif;
  return in;
}

// a reply's parse: a FAIL (never a crash) when it did not parse, and then T{}
template <class T>
T got(const std::optional<T>& v) {
  CHECK(v.has_value());
  return v.value_or(T{});
}

std::set<std::string> keys(const json& j) {
  std::set<std::string> out;
  for (auto it = j.begin(); it != j.end(); ++it) out.insert(it.key());
  return out;
}

}  // namespace

// ---- values ------------------------------------------------------------------------------------

static void test_values() {
  CHECK(poly::price_wire(3850) == "0.385");
  CHECK(poly::price_wire(3800) == "0.380");
  CHECK(poly::price_wire(3825) == "0.3825");
  CHECK(poly::price_wire(50) == "0.005");
  CHECK(poly::price_wire(9950) == "0.995");

  // money: {value, currency}, a decimal string, a number; to the micro-dollar
  CHECK(poly::parse_amount(usd("0.385")) == 385000);
  CHECK(poly::parse_amount(json{{"value", "0.0100"}}) == 10000);   // the old mock's, no currency
  CHECK(poly::parse_amount(json("3.10")) == 3100000);
  CHECK(poly::parse_amount(json("-0.25")) == -250000);
  CHECK(poly::parse_amount(json(100.5)) == 100500000);
  CHECK(poly::parse_amount(json(80)) == 80000000);
  CHECK(poly::parse_amount(json(0.1)) == 100000);
  CHECK(poly::parse_amount(json(123456.789012)) == 123456789012);
  CHECK(poly::parse_amount(json{{"value", 0.25}, {"currency", "USD"}}) == 250000);
  CHECK(!poly::parse_amount(json{{"value", "1"}, {"currency", "EUR"}}));
  CHECK(!poly::parse_amount(json{{"currency", "USD"}}));
  CHECK(!poly::parse_amount(json{{"value", json{{"value", "1"}}}}));
  CHECK(!poly::parse_amount(json("0.0000001")));   // finer than a micro-dollar
  CHECK(!poly::parse_amount(json(0.0000005)));
  CHECK(!poly::parse_amount(json(true)));
  CHECK(!poly::parse_amount(json("abc")));
  CHECK(!poly::parse_amount(json()));
  CHECK(!poly::parse_amount(json(1e300)));

  // a price: on the 1/10,000 grid and within 0..1
  CHECK(poly::parse_px(usd("0.385")) == 3850);
  CHECK(poly::parse_px(usd("0.3850")) == 3850);
  CHECK(poly::parse_px(json(0.005)) == 50);
  CHECK(poly::parse_px(json("1")) == 10000);
  CHECK(!poly::parse_px(usd("0.383333")));   // an average, not a price
  CHECK(!poly::parse_px(usd("1.5")));
  CHECK(!poly::parse_px(usd("-0.1")));

  // a size: whole or 2 places, a number or a string
  CHECK(poly::parse_qty(json(5)) == 500);
  CHECK(poly::parse_qty(json("5")) == 500);
  CHECK(poly::parse_qty(json("5.00")) == 500);
  CHECK(poly::parse_qty(json(2.5)) == 250);
  CHECK(poly::parse_qty(json(5.0)) == 500);
  CHECK(!poly::parse_qty(json("2.505")));
  CHECK(!poly::parse_qty(json(2.505)));
  CHECK(!poly::parse_qty(json(-5)));
  CHECK(!poly::parse_qty(json("-5")));
  CHECK(poly::parse_qty(json(-5), true) == -500);
  CHECK(poly::parse_qty(json("-5"), true) == -500);
  CHECK(!poly::parse_qty(json(true)));
  CHECK(!poly::parse_qty(json(1e300)));

  CHECK(poly::time_wire(T0 + 60000) == "2026-10-06T15:31:00Z");
  CHECK(poly::time_wire(T0 + 60999) == "2026-10-06T15:31:00Z");   // the second below
  CHECK(at(poly::time_wire(T0 + 61000)) == T0 + 61000);
}

// ---- builders ----------------------------------------------------------------------------------

static void test_builders() {
  std::string why;
  const std::int64_t now = T0 + 400;

  // intents in venue terms
  const auto ioc = poly::from_intent(intent(Owner::MAN, Side::BUY, 3850, 500, Tif::IOC), now, 0);
  CHECK(ioc.slug == NE && ioc.action == poly::Action::BUY && ioc.price == 3850 && ioc.count == 500 &&
        ioc.tif == poly::PmTif::IOC && !ioc.post_only && ioc.good_till_ms == 0 && ioc.manual && ioc.asynchronous);
  const auto fok = poly::from_intent(intent(Owner::SYS, Side::SELL, 3850, 500, Tif::FOK), now, 0);
  CHECK(fok.action == poly::Action::SELL && fok.tif == poly::PmTif::FOK && !fok.manual);
  PlaceIntent post = intent(Owner::SYS, Side::SELL, 4000, 100, Tif::POST);
  post.ttl_s = 15;
  auto gtd = poly::from_intent(post, now, 0);
  CHECK(gtd.tif == poly::PmTif::GTD && gtd.post_only && gtd.good_till_ms == T0 + 15000 && !gtd.manual);
  // the venue's minimum lead moves the expiry out: the first whole second past now + lead
  gtd = poly::from_intent(post, now, 60000);
  CHECK(gtd.good_till_ms == T0 + 61000);
  post.ttl_s = 0;
  post.exp_ms = T0 + 30000;   // a game's start
  CHECK(poly::from_intent(post, now, 0).good_till_ms == T0 + 30000);
  CHECK(poly::from_intent(post, now, 60000).good_till_ms == T0 + 61000);
  post.owner = Owner::MAN;
  CHECK(poly::from_intent(post, now, 0).manual);

  // create: exactly the fake's fields (it refuses one it does not know), no client id
  poly::WireRequest r;
  CHECK(poly::build_create(ioc, &r, &why));
  CHECK(r.method == "POST" && r.path == "/v1/orders" && r.query.empty() && r.host == poly::Host::API && r.auth &&
        r.write && r.op == WireOp::CREATE);
  json b = json::parse(r.body);
  CHECK(keys(b) == (std::set<std::string>{"marketSlug", "type", "price", "quantity", "tif", "outcomeSide", "action",
                                          "manualOrderIndicator", "synchronousExecution"}));
  CHECK(b["marketSlug"] == NE && b["type"] == "ORDER_TYPE_LIMIT" && b["price"] == usd("0.385") &&
        b["quantity"].is_number_integer() && b["quantity"] == 5 && b["tif"] == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL" &&
        b["outcomeSide"] == "OUTCOME_SIDE_YES" && b["action"] == "ORDER_ACTION_BUY" &&
        b["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_MANUAL" && b["synchronousExecution"] == false);
  CHECK(poly::build_create(fok, &r, &why));
  b = json::parse(r.body);
  CHECK(b["tif"] == "TIME_IN_FORCE_FILL_OR_KILL" && b["action"] == "ORDER_ACTION_SELL" &&
        b["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_AUTOMATIC");
  gtd = poly::from_intent(intent(Owner::SYS, Side::SELL, 4000, 100, Tif::POST), now, 60000);
  CHECK(poly::build_create(gtd, &r, &why));
  b = json::parse(r.body);
  CHECK(keys(b).size() == 11 && b["tif"] == "TIME_IN_FORCE_GOOD_TILL_DATE" && b["participateDontInitiate"] == true &&
        b["expireTime"] == "2026-10-06T15:31:01Z" && b["price"] == usd("0.400") && b["quantity"] == 1);
  // the synchronous fallback (never by default)
  auto sync = ioc;
  sync.asynchronous = false;
  CHECK(poly::build_create(sync, &r, &why));
  b = json::parse(r.body);
  CHECK(b["synchronousExecution"] == true && b["maxBlockTime"] == "1");

  // every refusal; build_create keeps out what is wrong in itself, check_create the market's and the clock's
  auto bad = [&](std::function<void(poly::CreateOrder&)> f, const char* needle, bool build_too) {
    auto o = ioc;
    f(o);
    const std::string w = poly::check_create(o, 50, 100, now);
    poly::WireRequest untouched;
    untouched.path = "x";
    std::string bw;
    const bool built = poly::build_create(o, &untouched, &bw);
    const bool ok = w.find(needle) != std::string::npos && built != build_too && (built || untouched.path == "x");
    if (!ok) std::fprintf(stderr, "  check_create: \"%s\" (wanted \"%s\"), built %d\n", w.c_str(), needle, built);
    CHECK(ok);
  };
  CHECK(poly::check_create(ioc, 50, 100, now).empty() && poly::check_create(gtd, 50, 100, now).empty());
  bad([](auto& o) { o.price = 3830; }, "off the market's 0.005 grid", false);
  bad([](auto& o) { o.count = 250; }, "whole contracts", true);
  bad([](auto& o) { o.count = 0; }, "whole contracts", true);
  bad([](auto& o) { o.price = 0; }, "strictly between 0 and 1", true);
  bad([](auto& o) { o.price = kOneDollar; }, "strictly between 0 and 1", true);
  bad([](auto& o) { o.slug = "aec nfl"; }, "a bad market slug", true);
  bad([](auto& o) { o.slug = ""; }, "a bad market slug", true);
  bad([](auto& o) { o.tif = poly::PmTif::GTC; }, "no GTC", true);
  bad([](auto& o) { o.post_only = true; }, "post-only (participateDontInitiate) only with GTD", true);
  bad([](auto& o) { o.good_till_ms = T0 + 60000; }, "an expiry only with GTD", true);
  bad([&](auto& o) { o = gtd; o.good_till_ms = T0 + 60500; }, "a whole second", true);
  bad([&](auto& o) { o = gtd; o.good_till_ms = 0; }, "a whole second", true);
  bad([&](auto& o) { o = gtd; o.good_till_ms = T0; }, "the GTD expiry has passed", false);
  CHECK(poly::check_create(ioc, 0, 100, now) == "the market's tick is not known");
  CHECK(poly::check_create(ioc, 100, 100, now).find("off the market's 0.010 grid") != std::string::npos);
  CHECK(poly::check_create(ioc, 50, 1000, now) == "under the market's minimum of 10 contracts");
  CHECK(poly::check_create(ioc, 50, 500, now).empty() && poly::check_create(ioc, 50, 0, now).empty());

  // cancels
  CHECK(poly::build_cancel("po-1", NE, &r, &why));
  CHECK(r.method == "POST" && r.path == "/v1/order/po-1/cancel" && json::parse(r.body) == (json{{"marketSlug", NE}}) &&
        r.write && r.op == WireOp::CANCEL && r.auth);
  CHECK(!poly::build_cancel("po/1", NE, &r, &why) && !poly::build_cancel("po-1", "", &r, &why));
  r = poly::build_cancel_all();
  CHECK(r.method == "POST" && r.path == "/v1/orders/open/cancel" && r.body == "{}" && r.write && r.op == WireOp::CANCEL_ALL);
  CHECK(poly::build_preview(gtd, &r, &why));
  CHECK(r.method == "POST" && r.path == "/v1/order/preview" && !r.write && r.op == WireOp::OTHER &&
        keys(json::parse(r.body)) == std::set<std::string>{"request"} && json::parse(r.body)["request"]["expireTime"].is_string());

  // reads: the signed ones on the api host, the public ones on the gateway, unsigned
  r = poly::build_get_order("po-1");
  CHECK(r.method == "GET" && r.path == "/v1/order/po-1" && r.auth && !r.write && r.host == poly::Host::API);
  CHECK(poly::build_get_open_orders().target() == "/v1/orders/open");
  CHECK(poly::build_get_activities("c 2", 50).target() == "/v1/portfolio/activities?limit=50&cursor=c%202");
  CHECK(poly::build_get_activities("", 0).target() == "/v1/portfolio/activities");
  CHECK(poly::build_get_positions("aec-x", 200).target() == "/v1/portfolio/positions?limit=200&cursor=aec-x");
  CHECK(poly::build_get_balances().target() == "/v1/account/balances" && poly::build_get_balances().auth);
  CHECK(poly::build_get_markets({NE, DAL}, &r, &why));
  CHECK(r.target() == "/v1/markets?slug=" + NE + "&slug=" + DAL && r.host == poly::Host::GATEWAY && !r.auth);
  CHECK(!poly::build_get_markets({}, &r, &why) && why.find("1 to 40") != std::string::npos);
  CHECK(!poly::build_get_markets(std::vector<std::string>(41, NE), &r, &why));
  CHECK(poly::build_get_markets(std::vector<std::string>(40, NE), &r, &why));
  CHECK(!poly::build_get_markets({NE, "a&b"}, &r, &why));
  r = poly::build_get_book(NE);
  CHECK(r.target() == "/v1/markets/" + NE + "/book" && r.host == poly::Host::GATEWAY && !r.auth);
  r = poly::build_get_league_events("nfl", 200, 200);
  CHECK(r.target() == "/v2/leagues/nfl/events?limit=200&offset=200" && r.host == poly::Host::GATEWAY && !r.auth);
  CHECK(poly::build_get_league_events("nfl", 0, 200).target() == "/v2/leagues/nfl/events?limit=200");

  // the bytes: a write closes its connection, a read keeps it
  CHECK(poly::build_create(ioc, &r, &why));
  std::string bytes = poly::request_bytes(r, "api.polymarket.us", {{"X-PM-Access-Key", "k"}});
  CHECK(bytes.rfind("POST /v1/orders HTTP/1.1\r\n", 0) == 0 && bytes.find("Host: api.polymarket.us\r\n") != std::string::npos &&
        bytes.find("X-PM-Access-Key: k\r\n") != std::string::npos &&
        bytes.find("Content-Type: application/json\r\n") != std::string::npos &&
        bytes.find("Connection: close\r\n") != std::string::npos && bytes.find("Accept-Encoding") == std::string::npos);
  bytes = poly::request_bytes(poly::build_get_book(NE), "gateway.polymarket.us", {});
  CHECK(bytes.find("Connection: keep-alive\r\n") != std::string::npos && bytes.find("X-PM-") == std::string::npos &&
        bytes.find("Content-Type") == std::string::npos);
}

// ---- parsers -----------------------------------------------------------------------------------

static void test_parsers() {
  std::string why;

  // league events: every market of every event, the long side first, the team code lower-cased
  json other = pm_winner(DAL);
  other.erase("bestBidQuote");
  other.erase("bestAskQuote");
  other.erase("feeCoefficient");
  other.erase("orderPriceMinTickSize");
  other.erase("minimumTradeQty");
  other.erase("active");
  other.erase("closed");
  other.erase("gameStartTime");
  other["startDate"] = "2026-09-13T20:25:00Z";
  other["marketSides"] = json::array();
  const json events{{"events", json::array({json{{"slug", "nfl-ne-sea-2026-09-09"}, {"title", "New England vs. Seattle"},
                                                 {"markets", json::array({pm_winner(NE), other})}},
                                            json{{"slug", "nfl-empty"}, {"title", "nothing listed"}}})},
                    {"league", {{"slug", "nfl"}}}};
  auto lp = poly::parse_league_events(events.dump(), "nfl", &why);
  CHECK(lp && lp->events == 2 && lp->markets.size() == 2);
  if (lp && lp->markets.size() == 2) {
    const auto& m = lp->markets[0];
    CHECK(m.slug == NE && m.event_slug == "nfl-ne-sea-2026-09-09" && m.event_title == "New England vs. Seattle" &&
          m.league == "nfl" && m.question == "Who will win Patriots vs Seahawks?" &&
          m.sports_type == "football_team_full_game_winner");
    CHECK(m.tick == 50 && m.min_qty == 100 && m.fee_coeff && *m.fee_coeff == 0.06 && m.bid == 3800 && m.ask == 3850);
    CHECK(m.start_ms == at("2026-09-10T00:20:00Z") && m.end_ms == at("2026-09-24T00:20:00Z") &&
          m.updated_ms == at("2026-09-08T20:00:00.123Z") && m.active && !m.closed);
    CHECK(m.sides.size() == 2 && m.sides[0].is_long && m.sides[0].abbr == "ne" && m.sides[0].name == "New England Patriots" &&
          m.sides[0].safe_name == "New England" && m.sides[0].alias == "Patriots" && m.sides[1].abbr == "sea");
    // what the venue left out is unknown, never 0 or open
    const auto& n = lp->markets[1];
    CHECK(!n.bid && !n.ask && !n.fee_coeff && !n.tick && !n.min_qty && !n.active && !n.closed && n.sides.empty() &&
          n.start_ms == at("2026-09-13T20:25:00Z"));
  }
  CHECK(got(poly::parse_league_events(R"({"events":[]})", "nfl", &why)).events == 0);
  // one malformed field fails the page, and says where
  json broken = events;
  broken["events"][0]["markets"][1]["bestBidQuote"] = usd("abc");
  CHECK(!poly::parse_league_events(broken.dump(), "nfl", &why) && why.find("bestBidQuote") != std::string::npos);
  broken = events;
  broken["events"][0]["markets"][0]["marketSides"][0]["team"] = "SEA";
  CHECK(!poly::parse_league_events(broken.dump(), "nfl", &why) && why.find("team") != std::string::npos);
  broken = events;
  broken["events"][0]["markets"][0]["orderPriceMinTickSize"] = 0;
  CHECK(!poly::parse_league_events(broken.dump(), "nfl", &why));
  CHECK(!poly::parse_league_events(R"({"league":{"slug":"nfl"}})", "nfl", &why) && why.find("events") != std::string::npos);
  CHECK(!poly::parse_league_events("not json", "nfl", &why) && why == "not JSON");

  auto ms = poly::parse_markets(json{{"markets", json::array({pm_winner(DAL), pm_winner(NE)})}}.dump(), &why);
  CHECK(ms && ms->size() == 2 && (*ms)[0].slug == DAL && (*ms)[1].slug == NE && (*ms)[0].league.empty());
  CHECK(!poly::parse_markets("{}", &why));

  // the book, best first (the old mock's body)
  auto book = poly::parse_book(
      R"({"marketData":{"bids":[{"px":{"value":"0.3800","currency":"USD"},"qty":"1000.00"},{"px":{"value":"0.3750","currency":"USD"},"qty":"20"}],"asks":[{"px":{"value":"0.3850","currency":"USD"},"qty":"900.00"}]}})",
      &why);
  CHECK(book && book->bids.size() == 2 && book->bids[0] == std::make_pair(Px{3800}, Qty{100000}) &&
        book->bids[1] == std::make_pair(Px{3750}, Qty{2000}) && book->asks.size() == 1 &&
        book->asks[0] == std::make_pair(Px{3850}, Qty{90000}));
  CHECK(got(poly::parse_book(R"({"marketData":{"bids":[]}})", &why)).asks.empty());
  CHECK(!poly::parse_book(R"({"marketData":{"bids":[{"px":{"value":"0.37"},"qty":"1"},{"px":{"value":"0.38"},"qty":"1"}]}})", &why) &&
        why.find("best first") != std::string::npos);
  CHECK(!poly::parse_book(R"({"marketData":{"asks":[{"px":{"value":"0.39"},"qty":"1"},{"px":{"value":"0.39"},"qty":"2"}]}})", &why));
  CHECK(!poly::parse_book(R"({"marketData":{"bids":[{"px":{"value":"0"},"qty":"1"}]}})", &why));
  CHECK(!poly::parse_book(R"({"marketData":{"bids":[{"px":{"value":"0.38"},"qty":"0"}]}})", &why));
  CHECK(!poly::parse_book(R"({"bids":[]})", &why) && why.find("marketData") != std::string::npos);

  // an order, each state
  auto o = poly::parse_order(json{{"order", pm_order("ORDER_STATE_PARTIALLY_FILLED", 5, 2, 3, "0.3825")}}.dump(), &why);
  CHECK(o && o->id == "po-1" && o->slug == NE && o->action == poly::Action::BUY && o->tif == poly::PmTif::GTD &&
        o->state == poly::OrderState::PARTIALLY_FILLED && o->price == 3850 && o->quantity == 500 && o->cum == 200 &&
        o->leaves == 300 && o->avg_px == 382500 && o->commission == 30000 && o->created_ms == at("2026-10-06T15:30:00Z") &&
        o->updated_ms == at("2026-10-06T15:30:01.500Z"));
  const std::pair<const char*, poly::OrderState> states[] = {
      {"ORDER_STATE_PENDING_NEW", poly::OrderState::PENDING_NEW}, {"ORDER_STATE_NEW", poly::OrderState::NEW},
      {"ORDER_STATE_FILLED", poly::OrderState::FILLED},           {"ORDER_STATE_CANCELED", poly::OrderState::CANCELED},
      {"ORDER_STATE_REJECTED", poly::OrderState::REJECTED},       {"ORDER_STATE_EXPIRED", poly::OrderState::EXPIRED},
      {"ORDER_STATE_PENDING_CANCEL", poly::OrderState::PENDING_CANCEL}};
  for (const auto& [name, st] : states) {
    auto x = poly::parse_order(json{{"order", pm_order(name, 5, 0, 0)}}.dump(), &why);
    CHECK(x && x->state == st && !x->avg_px);
  }
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_DONE", 5, 0, 0)}}.dump(), &why) &&
        why.find("state") != std::string::npos);
  // the old mock's {"value":"0"} before any fill is no price
  CHECK(!got(poly::parse_order(json{{"order", pm_order("ORDER_STATE_NEW", 5, 0, 5, "0")}}.dump(), &why)).avg_px);
  // what no order holds
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_FILLED", 5, 6, 0, "0.385")}}.dump(), &why));
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_NEW", 5, 2, 4, "0.385")}}.dump(), &why));
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_FILLED", 5, 5, 0)}}.dump(), &why) &&
        why.find("avgPx") != std::string::npos);
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_FILLED", 5, 5, 0, "1.0")}}.dump(), &why));
  CHECK(!poly::parse_order(json{{"order", pm_order("ORDER_STATE_NEW", 0, 0, 0)}}.dump(), &why));
  json no_fee = pm_order("ORDER_STATE_NEW", 5, 0, 5);
  no_fee.erase("commissionNotionalTotalCollected");
  CHECK(!poly::parse_order(json{{"order", no_fee}}.dump(), &why) && why.find("commission") != std::string::npos);
  json sell = pm_order("ORDER_STATE_NEW", 5, 0, 5);
  sell["side"] = "ORDER_SIDE_SELL";
  sell["tif"] = "TIME_IN_FORCE_GOOD_TILL_CANCEL";   // one placed on the website
  CHECK(got(poly::parse_order(json{{"order", sell}}.dump(), &why)).action == poly::Action::SELL);
  sell["side"] = "SELL";
  CHECK(!poly::parse_order(json{{"order", sell}}.dump(), &why));
  CHECK(!poly::parse_order(pm_order("ORDER_STATE_NEW", 5, 0, 5).dump(), &why));   // not under "order"

  auto open = poly::parse_open_orders(
      json{{"orders", json::array({pm_order("ORDER_STATE_NEW", 5, 0, 5), pm_order("ORDER_STATE_PENDING_NEW", 1, 0, 1)})}}.dump(),
      &why);
  CHECK(open && open->size() == 2 && poly::order_open((*open)[0].state) && poly::order_open((*open)[1].state));
  CHECK(!poly::parse_open_orders(
      json{{"orders", json::array({pm_order("ORDER_STATE_NEW", 5, 0, 5), pm_order("ORDER_STATE_NEW", -1, 0, 5)})}}.dump(), &why));
  CHECK(got(poly::parse_open_orders(R"({"orders":[]})", &why)).empty() && !poly::parse_open_orders("{}", &why));

  // activities: trades, money in and out, and a type nobody reads yet kept as OTHER
  json acts{{"activities",
             json::array({trade_activity("t-2", "0.3850", "5", true),
                          json{{"type", "ACTIVITY_TYPE_ACCOUNT_DEPOSIT"},
                               {"accountBalanceChange",
                                {{"transactionId", "d-1"}, {"amount", usd("100.0000")}, {"updateTime", "2026-10-06T15:00:00Z"}}}},
                          json{{"type", "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL"},
                               {"accountBalanceChange",
                                {{"transactionId", "w-1"}, {"amount", usd("25.5")}, {"createTime", "2026-10-06T14:00:00Z"}}}},
                          json{{"type", "ACTIVITY_TYPE_SOMETHING_NEW"}, {"whatever", 1}}})},
            {"nextCursor", "c2"},
            {"eof", false}};
  auto page = poly::parse_activities(acts.dump(), &why);
  CHECK(page && page->rows.size() == 4 && page->cursor == "c2");
  if (page && page->rows.size() == 4) {
    const auto& t = page->rows[0];
    CHECK(t.kind == poly::Activity::Kind::TRADE && t.trade.id == "t-2" && t.trade.slug == NE && t.trade.price == 3850 &&
          t.trade.qty == 500 && t.trade.aggressor && t.ts_ms == at("2026-10-06T15:30:02Z") && !t.trade.cost_basis);
    CHECK(page->rows[1].kind == poly::Activity::Kind::DEPOSIT && page->rows[1].id == "d-1" && page->rows[1].amount == 100000000);
    CHECK(page->rows[2].kind == poly::Activity::Kind::WITHDRAWAL && page->rows[2].amount == -25500000 &&
          page->rows[2].ts_ms == at("2026-10-06T14:00:00Z"));
    CHECK(page->rows[3].kind == poly::Activity::Kind::OTHER && page->rows[3].type == "ACTIVITY_TYPE_SOMETHING_NEW");
  }
  acts["eof"] = true;
  CHECK(got(poly::parse_activities(acts.dump(), &why)).cursor.empty());
  acts["eof"] = false;
  acts["nextCursor"] = "";
  CHECK(!poly::parse_activities(acts.dump(), &why) && why.find("nextCursor") != std::string::npos);
  acts.erase("eof");
  CHECK(got(poly::parse_activities(acts.dump(), &why)).cursor.empty());   // no eof: the cursor says
  json old_trade = trade_activity("t-3", "0.38", "5", false);
  old_trade["trade"].erase("qtyDecimal");
  old_trade["trade"]["qty"] = 5;
  old_trade["trade"]["costBasis"] = usd("1.90");
  auto ot = poly::parse_activities(json{{"activities", json::array({old_trade})}}.dump(), &why);
  CHECK(ot && ot->rows[0].trade.qty == 500 && !ot->rows[0].trade.aggressor && ot->rows[0].trade.cost_basis == 1900000);
  json no_aggr = trade_activity("t-4", "0.38", "5", false);
  no_aggr["trade"].erase("isAggressor");
  CHECK(!poly::parse_activities(json{{"activities", json::array({no_aggr})}}.dump(), &why) &&
        why.find("isAggressor") != std::string::npos);
  json no_trade{{"type", "ACTIVITY_TYPE_TRADE"}};
  CHECK(!poly::parse_activities(json{{"activities", json::array({no_trade})}}.dump(), &why));
  CHECK(!poly::parse_activities("{}", &why));

  // positions: an object keyed by slug
  json pos{{"positions",
            {{NE,
              {{"netPositionDecimal", "-5"}, {"cost", usd("3.10")}, {"realized", usd("0")}, {"cashValue", usd("0.15")},
               {"qtyAvailableDecimal", "5"}, {"expired", false}, {"updateTime", "2026-10-06T15:30:00Z"}}},
             {DAL, {{"netPosition", 2}, {"cost", usd("0.80")}}}}},
           {"nextCursor", ""},
           {"eof", true}};
  auto pp = poly::parse_positions(pos.dump(), &why);
  CHECK(pp && pp->rows.size() == 2 && pp->cursor.empty());
  if (pp && pp->rows.size() == 2) {
    const auto& d = pp->rows[0];   // keys in order: aec-nfl-dal first
    const auto& n = pp->rows[1];
    CHECK(d.slug == DAL && d.net == 200 && d.cost == 800000 && !d.realized && !d.cash_value && !d.available && !d.expired &&
          !d.updated_ms);
    CHECK(n.slug == NE && n.net == -500 && n.cost == 3100000 && n.realized == 0 && n.cash_value == 150000 &&
          n.available == 500 && n.expired == false && n.updated_ms == at("2026-10-06T15:30:00Z"));
  }
  CHECK(got(poly::parse_positions(R"({"positions":{},"eof":true})", &why)).rows.empty());
  json bad_pos = pos;
  bad_pos["positions"][NE].erase("cost");
  CHECK(!poly::parse_positions(bad_pos.dump(), &why) && why.find("cost") != std::string::npos);
  CHECK(!poly::parse_positions(R"({"positions":[{"marketSlug":"x","netPosition":1}]})", &why));
  CHECK(!poly::parse_positions("{}", &why));

  // balances: the mock's numbers, strings, amounts; the USD row only
  auto bal = poly::parse_balance(
      R"({"balances":[{"currentBalance":100.5,"currency":"USD","buyingPower":80.25,"assetNotional":20.0,"assetAvailable":20.0}]})", &why);
  CHECK(bal && bal->current == 100500000 && bal->buying_power == 80250000 && bal->asset_notional == 20000000 &&
        bal->asset_available == 20000000);
  bal = poly::parse_balance(
      R"({"balances":[{"currency":"EUR","currentBalance":1,"buyingPower":1},{"currentBalance":"10.25","currency":"USD","buyingPower":{"value":"9.5"}}]})",
      &why);
  CHECK(bal && bal->current == 10250000 && bal->buying_power == 9500000 && !bal->asset_notional && !bal->asset_available);
  CHECK(!poly::parse_balance(R"({"balances":[{"currentBalance":1,"currency":"EUR","buyingPower":1}]})", &why) &&
        why.find("USD") != std::string::npos);
  CHECK(!poly::parse_balance(R"({"balances":[{"currentBalance":1,"currency":"USD"}]})", &why) &&
        why.find("buyingPower") != std::string::npos);
  CHECK(!poly::parse_balance(R"({"currentBalance":1,"buyingPower":1})", &why));

  // a create's reply: asynchronous, then the synchronous fallback's executions
  auto ack = poly::parse_create_ack(R"({"id":"po-1","executions":[]})", &why);
  CHECK(ack && ack->order_id == "po-1" && ack->executions.empty());
  CHECK(got(poly::parse_create_ack(R"({"id":"po-1"})", &why)).executions.empty());
  ack = poly::parse_create_ack(
      R"({"id":"po-2","executions":[{"tradeId":"t-1","lastShares":"4.00","lastPx":{"value":"0.385","currency":"USD"},"commissionNotionalCollected":{"value":"0.0100","currency":"USD"},"order":{"state":"ORDER_STATE_FILLED"}}]})",
      &why);
  CHECK(ack && ack->executions.size() == 1 && ack->executions[0].trade_id == "t-1" && ack->executions[0].qty == 400 &&
        ack->executions[0].price == 3850 && ack->executions[0].fee == 10000 &&
        ack->executions[0].state == poly::OrderState::FILLED);
  CHECK(!poly::parse_create_ack(R"({"executions":[]})", &why) && why.find("id") != std::string::npos);
  CHECK(!poly::parse_create_ack(R"({"id":"po-1","executions":[{"lastShares":"4","lastPx":{"value":"0.385"}}]})", &why));
  CHECK(!poly::parse_create_ack(
      R"({"id":"po-1","executions":[{"tradeId":"t","lastShares":"4","lastPx":{"value":"0.385"},"order":{"state":"FILLED"}}]})", &why));
  CHECK(!poly::parse_create_ack(R"({"id":"po-1","executions":{}})", &why));

  auto ids = poly::parse_cancel_all(R"({"canceledOrderIds":["po-1","po-2"]})", &why);
  CHECK(ids && *ids == (std::vector<std::string>{"po-1", "po-2"}));
  CHECK(got(poly::parse_cancel_all(R"({"canceledOrderIds":[]})", &why)).empty());
  CHECK(!poly::parse_cancel_all("{}", &why) && !poly::parse_cancel_all(R"({"canceledOrderIds":[1]})", &why));
}

// ---- the ledger's rows -------------------------------------------------------------------------

static void test_ledger_rows() {
  std::string why;
  auto order = [&](const char* state, double qty, double cum, double leaves, const char* avg = nullptr) {
    return got(poly::parse_order(json{{"order", pm_order(state, qty, cum, leaves, avg)}}.dump(), &why));
  };
  auto o = order("ORDER_STATE_PARTIALLY_FILLED", 5, 4, 1, "0.3825");
  auto row = poly::to_ledger(o);
  CHECK(row.order_id == "po-1" && row.client_order_id.empty() && row.ticker == NE && row.side == kalshi::BookSide::BID &&
        row.status == kalshi::OrderStatus::RESTING && row.yes_price == 3850 && row.initial == 500 && row.filled == 400 &&
        row.remaining == 100 && row.taker_fees == 30000 && row.maker_fees == 0 && row.exchange_index == poly::kLedgerShard);
  // the cost of what the side bought: four YES at 0.3825; the average back from it as Kalshi's rows give it
  CHECK(row.taker_cost == 1530000 && row.taker_cost / row.filled == 3825);
  CHECK(poly::end_why(o).empty());
  o.action = poly::Action::SELL;   // a SELL bought NO at 1 - 0.3825
  row = poly::to_ledger(o);
  CHECK(row.side == kalshi::BookSide::ASK && row.taker_cost == 2470000);
  CHECK(poly::to_ledger(order("ORDER_STATE_PENDING_NEW", 5, 0, 5)).status == kalshi::OrderStatus::RESTING);
  CHECK(poly::to_ledger(order("ORDER_STATE_PENDING_CANCEL", 5, 0, 5)).remaining == 500);
  row = poly::to_ledger(order("ORDER_STATE_FILLED", 5, 5, 0, "0.385"));
  CHECK(row.status == kalshi::OrderStatus::EXECUTED && row.remaining == 0 && row.filled == 500 && row.taker_cost == 1925000);
  for (const char* st : {"ORDER_STATE_CANCELED", "ORDER_STATE_REJECTED", "ORDER_STATE_EXPIRED"}) {
    // a terminal row holds nothing open, whatever leaves says
    row = poly::to_ledger(order(st, 5, 2, 3, "0.385"));
    CHECK(row.status == kalshi::OrderStatus::CANCELED && row.remaining == 0 && row.filled == 200);
  }
  CHECK(poly::end_why(order("ORDER_STATE_REJECTED", 5, 0, 0)) == "EXCHANGE");
  CHECK(poly::end_why(order("ORDER_STATE_EXPIRED", 5, 0, 0)) == "EXPIRY");
  CHECK(poly::end_why(order("ORDER_STATE_CANCELED", 5, 0, 0)).empty());

  // a trade, tied to its order by the caller; its fee the model's, per fill to the cent
  auto page = poly::parse_activities(
      json{{"activities", json::array({trade_activity("t-1", "0.3850", "5", true), trade_activity("t-2", "0.3850", "5", false)})}}
          .dump(),
      &why);
  CHECK(page && page->rows.size() == 2);
  if (!page || page->rows.size() != 2) return;
  auto f = poly::to_ledger(page->rows[0].trade, "po-1", poly::Action::BUY, 0.06);
  // taker: 0.06 x 5 x 0.385 x 0.615 = 7.10c -> 7c
  CHECK(f.fill_id == "t-1" && f.trade_id == "t-1" && f.order_id == "po-1" && f.ticker == NE && f.side == kalshi::BookSide::BID &&
        f.count == 500 && f.yes_price == 3850 && f.taker && f.fee == 70000 && f.ts_ms == at("2026-10-06T15:30:02Z") &&
        f.exchange_index == poly::kLedgerShard && !f.subaccount);
  // maker: paid 0.0125 x 5 x 0.385 x 0.615 = 1.48c -> -1c
  f = poly::to_ledger(page->rows[1].trade, "po-1", poly::Action::SELL, 0.06);
  CHECK(!f.taker && f.fee == -10000 && f.side == kalshi::BookSide::ASK);
}

// ---- what a write's reply means ----------------------------------------------------------------

static void test_classify() {
  using RC = RejectClass;
  auto w = [](WireOp op, int st, const std::string& body) { return poly::classify_write(op, replied(st, body)); };
  auto refused = [&](WireOp op, int st, const std::string& body, RC c) {
    const auto r = w(op, st, body);
    const bool ok = r.kind == K::REFUSED && r.cls == c && r.http == st;
    if (!ok)
      std::fprintf(stderr, "  %d %s: %s %s\n", st, body.c_str(), kalshi::write_kind_name(r.kind), reject_class_name(r.cls));
    return ok;
  };
  auto err = [](int code, const std::string& msg) { return json{{"code", code}, {"message", msg}}.dump(); };

  net::Exchange x;
  x.fate = net::Exchange::Fate::NOT_SENT;
  x.why = "connect refused";
  CHECK(poly::classify_write(WireOp::CREATE, x).kind == K::NOT_SENT && poly::classify_write(WireOp::CREATE, x).why == "connect refused");
  x.fate = net::Exchange::Fate::UNKNOWN;
  x.why = "timeout after the write";   // a timeout, a reset, a body cut mid-way
  CHECK(poly::classify_write(WireOp::CREATE, x).kind == K::UNKNOWN);
  CHECK(poly::classify_write(WireOp::CANCEL_ALL, x).kind == K::UNKNOWN);

  // 2xx: an ACK only with a body that reads as the op's
  CHECK(w(WireOp::CREATE, 200, R"({"id":"po-1","executions":[]})").kind == K::ACK);
  CHECK(w(WireOp::CREATE, 201, R"({"id":"po-1","executions":[]})").kind == K::ACK);
  CHECK(w(WireOp::CREATE, 200, "{}").kind == K::UNKNOWN);   // taken, but which order?
  CHECK(w(WireOp::CREATE, 200, "<html>").kind == K::UNKNOWN);
  CHECK(w(WireOp::CANCEL, 200, "{}").kind == K::ACK);
  CHECK(w(WireOp::CANCEL, 200, "").kind == K::UNKNOWN);
  CHECK(w(WireOp::CANCEL_ALL, 200, R"({"canceledOrderIds":["po-1"]})").kind == K::ACK);
  CHECK(w(WireOp::CANCEL_ALL, 200, "{}").kind == K::UNKNOWN);
  CHECK(w(WireOp::OTHER, 200, "anything").kind == K::ACK);

  // after the write: a 5xx (the maintenance 503 among them), a 408, a redirect
  CHECK(w(WireOp::CREATE, 503, err(14, "the exchange is down for maintenance")).kind == K::UNKNOWN);
  CHECK(w(WireOp::CREATE, 500, err(13, "internal")).kind == K::UNKNOWN);
  CHECK(w(WireOp::CANCEL, 502, "bad gateway").kind == K::UNKNOWN);
  CHECK(w(WireOp::CREATE, 408, "").kind == K::UNKNOWN);
  CHECK(w(WireOp::CREATE, 302, "").kind == K::UNKNOWN);
  // a 4xx without the venue's own {code, message}: something in between answered
  CHECK(w(WireOp::CREATE, 403, "<html>Forbidden</html>").kind == K::UNKNOWN);
  CHECK(w(WireOp::CREATE, 400, R"({"message":"bad"})").kind == K::UNKNOWN);
  CHECK(w(WireOp::CREATE, 400, R"({"code":"3","message":"bad"})").kind == K::UNKNOWN);

  // refusals
  CHECK(refused(WireOp::CREATE, 429, "", RC::RETRYABLE));
  CHECK(refused(WireOp::CANCEL_ALL, 429, err(8, "too many requests"), RC::RETRYABLE));
  CHECK(refused(WireOp::CREATE, 401, err(16, "API key not found"), RC::AUTH));
  CHECK(refused(WireOp::CANCEL, 401, err(16, "invalid signature"), RC::AUTH));
  CHECK(refused(WireOp::CREATE, 403, err(7, "permission denied"), RC::AUTH));
  CHECK(refused(WireOp::CANCEL, 404, err(5, "order not found"), RC::GONE));
  CHECK(refused(WireOp::CREATE, 404, err(5, "market not found"), RC::HARD_INVALID));
  CHECK(refused(WireOp::CREATE, 400, err(3, "post only cross"), RC::SOFT_MARKET));
  CHECK(refused(WireOp::CREATE, 400, err(3, "insufficient balance"), RC::SOFT_COLLATERAL));
  CHECK(refused(WireOp::CREATE, 400, err(9, "Insufficient buying power"), RC::SOFT_COLLATERAL));
  CHECK(refused(WireOp::CREATE, 400, err(9, "market is closed"), RC::SOFT_MARKET));
  CHECK(refused(WireOp::CREATE, 400, err(9, "trading is paused"), RC::SOFT_MARKET));
  CHECK(refused(WireOp::CREATE, 400, err(3, "unknown field clientOrderId"), RC::HARD_INVALID));
  CHECK(refused(WireOp::CREATE, 422, err(3, "price must be between 0 and 1 on the market's tick"), RC::HARD_INVALID));
  // no client id, so no duplicate class: Kalshi reads this 409 as DUPLICATE (reconcile by cid)
  CHECK(refused(WireOp::CREATE, 409, err(6, "order already exists"), RC::UNKNOWN));
  CHECK(classify_refusal(WireOp::CREATE, 409, "6 order already exists") == RC::DUPLICATE);
  // no self-trade class either: the venue lists no self-trade prevention
  CHECK(refused(WireOp::CREATE, 418, err(2, "self trade"), RC::UNKNOWN));
  CHECK(refused(WireOp::CREATE, 418, err(2, "i'm a teapot"), RC::UNKNOWN));
  const auto r = w(WireOp::CREATE, 400, err(3, "post only cross"));
  CHECK(r.error.venue_body && r.error.code == "3" && r.error.message == "post only cross" && r.error.http == 400 &&
        r.why.find("post only cross") != std::string::npos);
  CHECK(reject_trips_kill(w(WireOp::CREATE, 400, err(3, "unknown field x")).cls) &&
        !reject_trips_kill(w(WireOp::CREATE, 400, err(3, "post only cross")).cls) &&
        !reject_trips_kill(w(WireOp::CANCEL, 404, err(5, "order not found")).cls));
}

// ---- fees ------------------------------------------------------------------------------------

namespace {
// every partition of n into parts of at most `most`, each as a list of parts
void partitions(int n, int most, std::vector<int>& cur, const std::function<void(const std::vector<int>&)>& f) {
  if (n == 0) {
    f(cur);
    return;
  }
  for (int k = std::min(n, most); k >= 1; --k) {
    cur.push_back(k);
    partitions(n - k, k, cur, f);
    cur.pop_back();
  }
}
}  // namespace

static void test_fees() {
  constexpr Micros c = 10000;   // a cent
  // the worked rows (core.js Desk.fees.pmFill): a maker's rebate rounds to the cent per fill
  CHECK(poly_maker_rebate_fill(5000, 100) == 0 && poly_maker_rebate_fill(5000, 200) == -c);
  CHECK(poly_maker_rebate_fill(9000, 400) == 0 && poly_maker_rebate_fill(9000, 500) == -c);
  CHECK(min_rebate_size(5000) == 200 && min_rebate_size(9000) == 500);
  CHECK(poly_taker_fee_fill(5000, 100, 0.06) == 2 * c && poly_taker_fee_fill(5000, 300, 0.06) == 4 * c);   // 1.5 -> 2, 4.5 -> 4

  // the allowance: the fee at the worst price the order can fill at, then the rounding
  CHECK(poly::taker_fee_allowance(Side::BUY, 4000, 100, 0.06) == poly_taker_fee_fill(4000, 100, 0.06) + c);
  CHECK(poly::taker_fee_allowance(Side::BUY, 7000, 100, 0.06) == poly_taker_fee_fill(5000, 100, 0.06) + c);
  CHECK(poly::taker_fee_allowance(Side::SELL, 7000, 100, 0.06) == poly_taker_fee_fill(7000, 100, 0.06) + c);
  CHECK(poly::taker_fee_allowance(Side::SELL, 3000, 100, 0.06) == poly_taker_fee_fill(5000, 100, 0.06) + c);
  CHECK(poly::taker_fee_allowance(Side::BUY, 5000, 0, 0.06) == 0 && poly::taker_fee_allowance(Side::BUY, 5000, 100, 0) == 0);
  // P1's case: ten one-contract fills at 50c are charged 20c against an exact 15c, more than the
  // fee rounded once plus 1c (16c)
  Micros ten = 0;
  for (int i = 0; i < 10; ++i) ten += poly_taker_fee_fill(5000, 100, 0.06);
  CHECK(ten == 20 * c && ten > poly_taker_fee_fill(5000, 1000, 0.06) + c);
  CHECK(poly::taker_fee_allowance(Side::BUY, 5000, 1000, 0.06) >= ten);

  // every way an order of 1-12 contracts splits into whole-contract fills, at every limit on the
  // 0.005 grid, both sides, three coefficients: no split is charged more than the allowance, and
  // the worst price the allowance names is the dearest of the prices the order can fill at
  int splits = 0, over = 0, worst_wrong = 0;
  for (const double coeff : {0.06, 0.0695, 0.02}) {
    for (Px limit = 50; limit < kOneDollar; limit += 50) {
      for (const Side s : {Side::BUY, Side::SELL}) {
        const Px worst = s == Side::BUY ? std::min<Px>(limit, 5000) : std::max<Px>(limit, 5000);
        for (int n = 1; n <= 12; ++n) {
          const Qty q = n * kOneContract;
          Micros dearest = 0;
          for (Px p = 50; p < kOneDollar; p += 50)
            if (s == Side::BUY ? p <= limit : p >= limit) dearest = std::max(dearest, poly_taker_fee_fill(p, q, coeff));
          if (dearest != poly_taker_fee_fill(worst, q, coeff)) ++worst_wrong;
          const Micros allow = poly::taker_fee_allowance(s, limit, q, coeff);
          std::vector<int> cur;
          partitions(n, n, cur, [&](const std::vector<int>& parts) {
            Micros charged = 0;
            for (int k : parts) charged += poly_taker_fee_fill(worst, k * kOneContract, coeff);
            ++splits;
            if (charged > allow) ++over;
          });
        }
      }
    }
  }
  std::printf("taker allowance: %d splits checked\n", splits);
  CHECK(splits > 100000 && over == 0 && worst_wrong == 0);
}

// ---- the maintenance window ----------------------------------------------------------------------

static void test_maintenance() {
  auto in = [](const char* t) { return poly::in_maintenance(at(t)); };
  // Thursday 2026-10-29, EDT (UTC-4): 02:00-06:00 ET is 06:00-10:00 UTC
  CHECK(!in("2026-10-29T05:59:59Z") && in("2026-10-29T06:00:00Z") && in("2026-10-29T09:59:59Z") && !in("2026-10-29T10:00:00Z"));
  // Thursday 2026-11-05, the first after DST ends on Sunday 2026-11-01: EST (UTC-5), 07:00-11:00 UTC
  CHECK(!in("2026-11-05T06:59:59Z") && in("2026-11-05T07:00:00Z") && in("2026-11-05T10:59:59Z") && !in("2026-11-05T11:00:00Z"));
  CHECK(!in("2026-11-05T06:00:00Z"));   // a fixed EDT offset would say yes
  // Thursday 2027-03-11, the last before DST starts on Sunday 2027-03-14: EST; 2027-03-18: EDT
  CHECK(!in("2027-03-11T06:30:00Z") && in("2027-03-11T07:00:00Z") && in("2027-03-11T10:59:59Z") && !in("2027-03-11T11:00:00Z"));
  CHECK(!in("2027-03-18T05:59:59Z") && in("2027-03-18T06:00:00Z") && in("2027-03-18T09:59:59Z") && !in("2027-03-18T10:30:00Z"));
  // only Thursdays
  CHECK(!in("2026-11-04T08:00:00Z") && !in("2026-11-06T08:00:00Z") && !in("2026-11-01T08:00:00Z"));
  // the window now is in, else the next
  auto win = poly::maintenance_window(at("2026-11-03T12:00:00Z"));
  CHECK(win.from_ms == at("2026-11-05T07:00:00Z") && win.to_ms == at("2026-11-05T11:00:00Z"));
  win = poly::maintenance_window(at("2026-11-05T08:00:00Z"));
  CHECK(win.from_ms == at("2026-11-05T07:00:00Z") && win.to_ms == at("2026-11-05T11:00:00Z"));
  win = poly::maintenance_window(at("2026-11-05T11:00:00Z"));
  CHECK(win.from_ms == at("2026-11-12T07:00:00Z"));
  win = poly::maintenance_window(at("2026-10-28T23:00:00Z"));
  CHECK(win.from_ms == at("2026-10-29T06:00:00Z") && win.to_ms == at("2026-10-29T10:00:00Z"));
  win = poly::maintenance_window(at("2027-03-11T12:00:00Z"));
  CHECK(win.from_ms == at("2027-03-18T06:00:00Z"));
}

// ---- the signer --------------------------------------------------------------------------------

namespace {
// a key made here, its seed and public half (raw)
struct NewKey {
  std::string seed, pub;
  NewKey() {
    EVP_PKEY* k = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    unsigned char b[32];
    std::size_t n = sizeof b;
    EVP_PKEY_get_raw_private_key(k, b, &n);
    seed.assign(reinterpret_cast<const char*>(b), n);
    n = sizeof b;
    EVP_PKEY_get_raw_public_key(k, b, &n);
    pub.assign(reinterpret_cast<const char*>(b), n);
    EVP_PKEY_free(k);
  }
};
std::string b64(const std::string& raw) { return base64_encode(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()); }
// OpenSSL directly, with the public half alone: not PolySigner::verify
bool verify_raw(const std::string& pub, const std::string& msg, const std::string& sig_b64) {
  const std::string sig = base64_decode(sig_b64);
  EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(pub.data()),
                                            pub.size());
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const bool ok = k && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
                  EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                                   reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(k);
  return ok;
}
void write_file(const std::string& path, const std::string& text, mode_t mode) {
  std::ofstream(path) << text;
  ::chmod(path.c_str(), mode);
}
}  // namespace

static void test_signer() {
  std::string err;
  // known answers (RFC 8032 key 1; Python's cryptography 50.0.1 signed the same messages, and gives
  // the RFC's own signature for the empty one)
  auto kat = PolySigner::from_secret("kat", RFC_SEED, &err);
  CHECK(kat != nullptr);
  if (kat) {
    CHECK(kat->public_b64() == RFC_PUB && kat->key_id() == "kat");
    CHECK(PolySigner::message(1788900000000, "GET", "/v1/portfolio/positions?limit=200") ==
          "1788900000000GET/v1/portfolio/positions");
    CHECK(kat->sign(1788900000000, "GET", "/v1/portfolio/positions") ==
          "+XREw/Q+9/k8M5M77twNp4YhubrvmnNHlM6370yEeSnWSY0XlWQ0fz9h1IqfWGYjWpWkeqmEO3EelHmiRn/YDg==");
    // the query is never signed
    CHECK(kat->sign(1788900000000, "GET", "/v1/portfolio/positions?limit=200&cursor=x") ==
          kat->sign(1788900000000, "GET", "/v1/portfolio/positions"));
    CHECK(kat->sign(1788900000123, "POST", "/v1/orders") ==
          "CjGt93bbY5taBQ9Kc/jwSXpaPkaf9Sw7VuhTzQ8cU9ajGYX2EYMG4I1r3HB2XSLpu8MxCgQEpGryEPn4XGznCQ==");
    const auto h = kat->headers(1788900000123, "POST", "/v1/orders");
    CHECK(h.size() == 3 && h[0] == std::make_pair(std::string("X-PM-Access-Key"), std::string("kat")) &&
          h[1] == std::make_pair(std::string("X-PM-Timestamp"), std::string("1788900000123")) &&
          h[2].first == "X-PM-Signature" && h[2].second == kat->sign(1788900000123, "POST", "/v1/orders"));
    CHECK(kat->verify(1788900000123, "POST", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000124, "POST", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000123, "GET", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000123, "POST", "/v1/orders", "not base64!"));
  }

  // a key made here: its signatures verify under OpenSSL with the public half alone
  const NewKey nk;
  auto s = PolySigner::from_secret(KEY_ID, b64(nk.seed), &err);
  CHECK(s && s->public_b64() == b64(nk.pub));
  if (s) {
    const std::string sig = s->sign(1788900000000, "DELETE", "/v1/order/x");
    CHECK(sig.size() == 88 && verify_raw(nk.pub, "1788900000000DELETE/v1/order/x", sig));
  }
  // 64 bytes, seed then public half: the same key; another key's public half is refused
  auto s64 = PolySigner::from_secret(KEY_ID, b64(nk.seed + nk.pub), &err);
  CHECK(s64 && s64->public_b64() == b64(nk.pub));
  const NewKey other;
  const std::string mixed = b64(nk.seed + other.pub);
  CHECK(!PolySigner::from_secret(KEY_ID, mixed, &err) && err.find("second half") != std::string::npos &&
        err.find(mixed.substr(0, 20)) == std::string::npos && err.find(b64(nk.seed).substr(0, 20)) == std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, b64(nk.seed.substr(0, 31)), &err) && err.find("32 or 64 bytes") != std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, "not-a-key", &err) && err.find("32 or 64 bytes") != std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, "", &err));
  CHECK(!PolySigner::from_secret("", b64(nk.seed), &err) && err.find("key id") != std::string::npos);
  CHECK(!PolySigner::from_secret("k\r\nX-Other: 1", b64(nk.seed), &err));   // nothing that splits a header
  CHECK(PolySigner::from_secret(KEY_ID, "  " + b64(nk.seed) + "\n", &err) != nullptr);

  // the file: the owner's alone, else refused, and never quoted
  TempDir d;
  const std::string secret = b64(nk.seed);
  write_file(d.file("pm.secret"), secret + "\n", 0600);
  auto f = PolySigner::load(KEY_ID, d.file("pm.secret"), &err);
  CHECK(f && f->public_b64() == b64(nk.pub));
  write_file(d.file("pm.secret"), secret + "\n", 0400);
  CHECK(PolySigner::load(KEY_ID, d.file("pm.secret"), &err) != nullptr);
  ::chmod(d.file("pm.secret").c_str(), 0644);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err) && err.find("mode 0644") != std::string::npos &&
        err.find("chmod 600") != std::string::npos && err.find(secret.substr(0, 20)) == std::string::npos);
  ::chmod(d.file("pm.secret").c_str(), 0640);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err) && err.find("mode 0640") != std::string::npos);
  ::chmod(d.file("pm.secret").c_str(), 0604);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err));
  CHECK(!PolySigner::load(KEY_ID, d.file("missing"), &err) &&
        err == "cannot open the Polymarket secret file " + d.file("missing"));
  CHECK(!PolySigner::load(KEY_ID, d.path, &err) && err.find("not a regular file") != std::string::npos);
  write_file(d.file("junk"), "this is not a secret at all", 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("junk"), &err) && err.find("32 or 64 bytes") != std::string::npos &&
        err.find("not a secret") == std::string::npos);
  write_file(d.file("big"), std::string(5000, 'A'), 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("big"), &err) && err.find("too large") != std::string::npos);
  write_file(d.file("mixed"), mixed, 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("mixed"), &err) && err.find(d.file("mixed")) != std::string::npos &&
        err.find(mixed.substr(0, 20)) == std::string::npos);
  // "~/" is $HOME's
  const char* home = std::getenv("HOME");
  const std::string saved = home ? home : "";
  ::setenv("HOME", d.path.c_str(), 1);
  write_file(d.file("pm.secret"), secret, 0600);
  CHECK(PolySigner::load(KEY_ID, "~/pm.secret", &err) != nullptr);
  if (home) ::setenv("HOME", saved.c_str(), 1);
  else ::unsetenv("HOME");

  // four threads signing at once over one key
  if (s) {
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([&, t] {
        for (int i = 0; i < 200; ++i) {
          const std::string p = "/v1/order/o" + std::to_string(t);
          if (!s->verify(i, "GET", p, s->sign(i, "GET", p))) ++bad;
        }
      });
    for (auto& t : ts) t.join();
    CHECK(bad == 0);
  }
}

// ---- round trips against the fake Polymarket ----------------------------------------------------

namespace {
struct PmClient {
  net::ConnFactory f{fx::demo_guard()};
  net::Endpoint ep;
  const PolySigner* signer;
  PmClient(int port, const PolySigner* s) : ep(*net::Endpoint::parse("http://127.0.0.1:" + std::to_string(port), nullptr)), signer(s) {}
  net::Exchange send(const poly::WireRequest& r, const PolySigner* as = nullptr) {
    std::string why;
    auto c = f.open(ep, 3000, &why);
    if (!c) return net::Exchange{};
    net::HeaderList h;
    const PolySigner* k = as ? as : signer;
    if (r.auth && k) h = k->headers(wall_ms(), r.method, r.path);
    return net::exchange(*c, poly::request_bytes(r, ep.host_header(), h), fx::deadline_ms(3000));
  }
  kalshi::WriteOutcome write(const poly::WireRequest& r, net::Exchange* out = nullptr, const PolySigner* as = nullptr) {
    net::Exchange x = send(r, as);
    if (out) *out = x;
    return poly::classify_write(r.op, x);
  }
  // a create, and its reply when acknowledged
  std::optional<poly::CreateAck> create(const poly::CreateOrder& o, kalshi::WriteOutcome* w = nullptr) {
    poly::WireRequest r;
    std::string why;
    if (!poly::build_create(o, &r, &why)) return std::nullopt;
    net::Exchange x;
    const auto out = write(r, &x);
    if (w) *w = out;
    if (out.kind != K::ACK) return std::nullopt;
    return poly::parse_create_ack(x.res.body, &why);
  }
};

poly::CreateOrder pm(poly::Action a, Px px, int contracts, poly::PmTif tif = poly::PmTif::IOC) {
  poly::CreateOrder o;
  o.slug = NE;
  o.action = a;
  o.price = px;
  o.count = contracts * kOneContract;
  o.tif = tif;
  if (tif == poly::PmTif::GTD) {
    o.post_only = true;
    o.manual = false;
    o.good_till_ms = (wall_ms() / 1000 + 60) * 1000;
  }
  return o;
}
}  // namespace

static void test_round_trips() {
  std::string err, why;
  const NewKey nk;
  auto signer = PolySigner::from_secret(KEY_ID, b64(nk.seed), &err);
  CHECK(signer != nullptr);
  if (!signer) return;
  fakex::FakePolyConfig cfg;
  cfg.auth = fakex::AuthMode::VERIFY;
  cfg.key_id = KEY_ID;
  cfg.public_key_b64 = signer->public_b64();
  cfg.rate_limits = false;
  fakex::FakePoly v(cfg);
  fakex::PolyMarketSpec ne;
  ne.slug = NE;
  ne.league = "nfl";
  ne.event_slug = "nfl-ne-sea-2026-09-09";
  ne.event_title = "New England vs. Seattle";
  ne.question = "Who will win Patriots vs Seahawks?";
  ne.start_ms = at("2026-09-10T00:20:00Z");
  ne.sides = {fakex::PolySide{"Patriots", true, fakex::PolyTeam{"ne", "New England Patriots", "New England", "Patriots"}},
              fakex::PolySide{"Seahawks", false, fakex::PolyTeam{"sea", "Seattle Seahawks", "Seattle", "Seahawks"}}};
  v.list_market(ne);
  fakex::PolyMarketSpec dal = ne;   // a market whose minimum is five contracts
  dal.slug = DAL;
  dal.event_slug = "nfl-dal-nyg-2026-09-13";
  dal.min_qty = 5 * kOneContract;
  v.list_market(dal);
  v.with_engine([](fakex::Engine& e, std::int64_t now) {
    e.street_add(NE, false, 3800, 1000, now);   // YES 0.380 x10 bid
    e.street_add(NE, true, 6150, 900, now);     // NO 0.615 x9: YES 0.385 x9 offered
  });
  v.deposit(100 * fakex::kMicrosPerDollar);
  CHECK(v.start(&err));
  PmClient c(v.port(), signer.get());

  // the public reads, unsigned
  net::Exchange x = c.send(poly::build_get_league_events("nfl", 0, 200));
  auto lp = poly::parse_league_events(x.res.body, "nfl", &why);
  CHECK(x.res.status == 200 && lp && lp->events == 2 && lp->markets.size() == 2);
  if (lp && lp->markets.size() == 2) {
    const auto& m = lp->markets[0];
    CHECK(m.slug == NE && m.tick == 50 && m.min_qty == 100 && m.fee_coeff == 0.06 && m.bid == 3800 && m.ask == 3850 &&
          m.active && !m.closed && m.start_ms == ne.start_ms && m.sides.size() == 2 && m.sides[0].abbr == "ne");
  }
  poly::WireRequest r;
  CHECK(poly::build_get_markets({NE, DAL}, &r, &why));
  auto ms = poly::parse_markets(c.send(r).res.body, &why);
  CHECK(ms && ms->size() == 2 && (*ms)[0].slug == NE && (*ms)[1].slug == DAL && (*ms)[1].min_qty == 500);
  auto book = poly::parse_book(c.send(poly::build_get_book(NE)).res.body, &why);
  CHECK(book && book->bids.size() == 1 && book->bids[0] == std::make_pair(Px{3800}, Qty{1000}) && book->asks.size() == 1 &&
        book->asks[0] == std::make_pair(Px{3850}, Qty{900}));
  for (const auto& line : v.wire_log()) CHECK(line.find("(signed)") == std::string::npos);
  v.clear_wire_log();

  // the account, signed
  auto bal = poly::parse_balance(c.send(poly::build_get_balances()).res.body, &why);
  CHECK(bal && bal->current == 100000000 && bal->buying_power == 100000000);

  // an asynchronous IOC that takes two contracts: acknowledged with no executions, filled by its read
  kalshi::WriteOutcome w;
  auto ack = c.create(pm(poly::Action::BUY, 3850, 2), &w);
  CHECK(ack && ack->executions.empty() && !ack->order_id.empty());
  if (!ack) {
    std::fprintf(stderr, "  create: %s %s\n", kalshi::write_kind_name(w.kind), w.why.c_str());
    return;
  }
  const json sent = v.sent(ack->order_id);
  CHECK(sent["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_MANUAL" && sent["synchronousExecution"] == false &&
        !sent.contains("participateDontInitiate"));
  auto o = poly::parse_order(c.send(poly::build_get_order(ack->order_id)).res.body, &why);
  CHECK(o && o->id == ack->order_id && o->state == poly::OrderState::FILLED && o->cum == 200 && o->leaves == 0 &&
        o->avg_px == 385000 && o->tif == poly::PmTif::IOC);
  // 0.06 x 2 x 0.385 x 0.615 = 2.84c -> 3c, the fake's per fill and the model's alike
  CHECK(o && o->commission == 30000 && poly::to_ledger(*o).status == kalshi::OrderStatus::EXECUTED);
  auto acts = poly::parse_activities(c.send(poly::build_get_activities("", 100)).res.body, &why);
  CHECK(acts && acts->rows.size() == 2 && acts->cursor.empty());
  if (acts && acts->rows.size() == 2) {
    const auto& t = acts->rows[0];   // newest first
    CHECK(t.kind == poly::Activity::Kind::TRADE && t.trade.slug == NE && t.trade.price == 3850 && t.trade.qty == 200 &&
          t.trade.aggressor);
    CHECK(o && poly::to_ledger(t.trade, o->id, o->action, 0.06).fee == o->commission);
    CHECK(acts->rows[1].kind == poly::Activity::Kind::DEPOSIT && acts->rows[1].amount == 100000000);
  }
  auto pos = poly::parse_positions(c.send(poly::build_get_positions("", 0)).res.body, &why);
  CHECK(pos && pos->rows.size() == 1 && pos->rows[0].slug == NE && pos->rows[0].net == 200 && pos->rows[0].available == 200);

  // a GTD post-only rests, is listed, is cancelled; a second cancel finds it gone
  auto post = pm(poly::Action::SELL, 4000, 1, poly::PmTif::GTD);
  CHECK(poly::check_create(post, 50, 100, wall_ms()).empty());
  ack = c.create(post, &w);
  CHECK(ack && ack->executions.empty());
  if (!ack) return;
  const json psent = v.sent(ack->order_id);
  CHECK(psent["manualOrderIndicator"] == "MANUAL_ORDER_INDICATOR_AUTOMATIC" && psent["participateDontInitiate"] == true &&
        psent["expireTime"] == poly::time_wire(post.good_till_ms));
  auto open = poly::parse_open_orders(c.send(poly::build_get_open_orders()).res.body, &why);
  CHECK(open && open->size() == 1 && (*open)[0].id == ack->order_id && (*open)[0].state == poly::OrderState::NEW &&
        (*open)[0].tif == poly::PmTif::GTD && (*open)[0].action == poly::Action::SELL && (*open)[0].leaves == 100);
  CHECK(poly::build_cancel(ack->order_id, NE, &r, &why));
  CHECK(c.write(r).kind == K::ACK);
  w = c.write(r);
  CHECK(w.kind == K::REFUSED && w.cls == RejectClass::GONE);
  o = poly::parse_order(c.send(poly::build_get_order(ack->order_id)).res.body, &why);
  CHECK(o && o->state == poly::OrderState::CANCELED && poly::end_why(*o).empty());

  // cancel-all names what it cancelled
  ack = c.create(pm(poly::Action::BUY, 3000, 1, poly::PmTif::GTD), &w);
  CHECK(ack.has_value());
  x = net::Exchange{};
  CHECK(c.write(poly::build_cancel_all(), &x).kind == K::ACK);
  auto ids = poly::parse_cancel_all(x.res.body, &why);
  CHECK(ack && ids && *ids == std::vector<std::string>{ack->order_id});

  // preview checks a create and places nothing
  CHECK(poly::build_preview(pm(poly::Action::BUY, 3850, 1), &r, &why));
  CHECK(c.write(r).kind == K::ACK);
  CHECK(got(poly::parse_open_orders(c.send(poly::build_get_open_orders()).res.body, &why)).empty());

  // the synchronous fallback: the reply carries the fill
  auto sync = pm(poly::Action::BUY, 3850, 1);
  sync.asynchronous = false;
  ack = c.create(sync, &w);
  CHECK(ack && ack->executions.size() == 1 && ack->executions[0].qty == 100 && ack->executions[0].price == 3850 &&
        ack->executions[0].fee == 10000 && ack->executions[0].state == poly::OrderState::FILLED);

  // the soft refusals: a post-only that would cross, more than the buying power
  auto cross = pm(poly::Action::BUY, 3850, 1, poly::PmTif::GTD);
  cross.asynchronous = false;
  c.create(cross, &w);
  CHECK(w.kind == K::REFUSED && w.cls == RejectClass::SOFT_MARKET);
  auto big = pm(poly::Action::BUY, 3850, 5);
  big.count = 1000 * kOneContract;
  big.asynchronous = false;
  c.create(big, &w);
  CHECK(w.kind == K::REFUSED && w.cls == RejectClass::SOFT_COLLATERAL);
  // asynchronous, the same refusal arrives later: REJECTED, which the ledger ends EXCHANGE
  big.asynchronous = true;
  ack = c.create(big, &w);
  CHECK(ack.has_value());
  if (ack) {
    o = poly::parse_order(c.send(poly::build_get_order(ack->order_id)).res.body, &why);
    CHECK(o && o->state == poly::OrderState::REJECTED && poly::end_why(*o) == "EXCHANGE");
  }
  // off the grid: build_create sends it, the venue refuses it, and the refusal is hard (check_create
  // is what keeps it from the wire)
  auto off = pm(poly::Action::BUY, 3830, 1);
  CHECK(!poly::check_create(off, 50, 100, wall_ms()).empty());
  c.create(off, &w);
  CHECK(w.kind == K::REFUSED && w.cls == RejectClass::HARD_INVALID && reject_trips_kill(w.cls));
  // under the market's minimum: the same
  auto small = pm(poly::Action::BUY, 3850, 1);
  small.slug = DAL;
  CHECK(poly::check_create(small, 50, 500, wall_ms()) == "under the market's minimum of 5 contracts");
  c.create(small, &w);
  CHECK(w.kind == K::REFUSED && w.cls == RejectClass::HARD_INVALID);

  // a key the venue does not know, and the right key id with another key's signature
  auto stranger = PolySigner::from_secret("b0000000-0000-4000-8000-000000000000", b64(nk.seed), &err);
  const NewKey ok2;
  auto forger = PolySigner::from_secret(KEY_ID, b64(ok2.seed), &err);
  CHECK(stranger && forger);
  if (stranger && forger) {
    CHECK(poly::build_create(pm(poly::Action::BUY, 3850, 1), &r, &why));
    w = c.write(r, nullptr, stranger.get());
    CHECK(w.kind == K::REFUSED && w.cls == RejectClass::AUTH && w.error.message == "API key not found");
    w = c.write(r, nullptr, forger.get());
    CHECK(w.kind == K::REFUSED && w.cls == RejectClass::AUTH && w.error.message == "invalid signature");
  }
  // every account request carried the X-PM headers
  for (const auto& line : v.wire_log())
    if (line.find(" /v1/order") != std::string::npos || line.find(" /v1/portfolio") != std::string::npos ||
        line.find(" /v1/account") != std::string::npos)
      CHECK(line.find("(signed)") != std::string::npos);

  // maintenance: a 503 after the write is UNKNOWN
  v.set_maintenance(v.now_ms() - 1000, v.now_ms() + 60000);
  c.create(pm(poly::Action::BUY, 3850, 1), &w);
  CHECK(w.kind == K::UNKNOWN && w.http == 503);
  v.set_maintenance(0, 0);
  v.stop();
}

int main() {
  test_values();
  test_builders();
  test_parsers();
  test_ledger_rows();
  test_classify();
  test_fees();
  test_maintenance();
  test_signer();
  test_round_trips();
  return finish("deskd-poly_wire");
}
