// Minimal self-contained tests: fees, sizing, fixed-point, signing, store round trip,
// order body mapping, engine candidates, weather bracket math, time parsing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <httplib.h>

#include "desk/auth.h"
#include "desk/client.h"
#include "desk/engine.h"
#include "desk/eval.h"
#include "desk/fees.h"
#include "desk/risk.h"
#include "desk/hedge.h"
#include "desk/ladder.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/store.h"
#include "desk/strategies.h"
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

static void test_fees() {
  FeeModel taker_only{"quadratic", 1.0};
  CHECK_NEAR(taker_only.fee(0.50, 1, true), 0.0175, 1e-9);
  CHECK_NEAR(taker_only.fee(0.50, 1, false), 0.0, 1e-12);
  CHECK_NEAR(taker_only.fee(0.90, 1, true), 0.0063, 1e-9);
  CHECK_NEAR(taker_only.fee(0.50, 10, true), 0.175, 1e-9);
  FeeModel with_maker{"quadratic_with_maker_fees", 1.0};
  CHECK_NEAR(with_maker.fee(0.50, 1, false), 0.0044, 1e-9);  // 0.004375 rounded up to centicent
  CHECK_NEAR(with_maker.fee(0.50, 1, true), 0.0175, 1e-9);
  FeeModel none{"quadratic", 0.0};
  CHECK_NEAR(none.fee(0.50, 1, true), 0.0, 1e-12);
  FeeModel half{"quadratic", 0.5};
  CHECK_NEAR(half.fee(0.50, 1, true), 0.0088, 1e-9);  // 0.00875 rounded up to a centicent
  // Kelly
  CHECK_NEAR(kelly_fraction(0.55, 0.5175), (0.55 - 0.5175) / (1 - 0.5175), 1e-12);
  CHECK_NEAR(kelly_fraction(0.50, 0.5175), 0.0, 1e-12);
  CHECK_NEAR(size_contracts(100, 0.55, 0.5175, 0.25, 0.10), 3, 1e-9);
  CHECK_NEAR(size_contracts(100, 0.60, 0.5175, 0.25, 0.10), 8, 1e-9);
  CHECK_NEAR(size_contracts(100, 0.99, 0.5175, 0.25, 0.10), 19, 1e-9);  // cap 10% = $10 / 0.5175
}

static void test_fixed() {
  CHECK(fmt_dollars(0.56) == "0.5600");
  CHECK(fmt_fp(10) == "10.00");
  CHECK_NEAR(parse_num("0.5600"), 0.56, 1e-12);
  CHECK_NEAR(floor_to_tick(0.567, 0.01), 0.56, 1e-9);
  CHECK_NEAR(ceil_to_tick(0.561, 0.01), 0.57, 1e-9);
  CHECK_NEAR(clamp_price(1.2), 0.99, 1e-9);
  CHECK_NEAR(clamp_price(0.0), 0.01, 1e-9);
}

static void test_time() {
  CHECK(parse_iso("2026-09-05T16:00:00Z") == 1788624000LL);
  CHECK(parse_iso("2026-09-05T16:00Z") == 1788624000LL);           // ESPN style, no seconds
  CHECK(parse_iso("2026-09-05T15:51:00+00:00") == 1788623460LL);   // NWS obs style
  int off = 0;
  long long t = parse_iso("2026-09-05T12:00:00-04:00", &off);
  CHECK(off == -4 * 3600);
  CHECK(t == 1788624000LL);
  CHECK(kalshi_date("26SEP06") == "2026-09-06");
  CHECK(event_date("KXHIGHNY-26SEP06") == "2026-09-06");
  CHECK(event_date("KXBTC-26SEP0512") == "2026-09-05");
  CHECK(date_of(1788624000LL, -4 * 3600) == "2026-09-05");
  CHECK(hour_of(1788624000LL, -4 * 3600) == 12);
  CHECK(series_from_event("KXHIGHNY-26SEP06") == "KXHIGHNY");
}

static void test_signing() {
  // Generate a throwaway RSA key with openssl and make sure sign/verify round-trips.
  std::string pem = "/tmp/desk_test_key.pem";
  int rc = std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str());
  if (rc != 0) {
    std::fprintf(stderr, "skip signing test: openssl CLI not available\n");
    return;
  }
  Signer s("test-key-id", pem);
  auto h = s.headers("GET", "/trade-api/v2/portfolio/balance", 1234567890000LL);
  CHECK(h["KALSHI-ACCESS-KEY"] == "test-key-id");
  CHECK(h["KALSHI-ACCESS-TIMESTAMP"] == "1234567890000");
  CHECK(s.verify("1234567890000GET/trade-api/v2/portfolio/balance", h["KALSHI-ACCESS-SIGNATURE"]));
  CHECK(!s.verify("1234567890001GET/trade-api/v2/portfolio/balance", h["KALSHI-ACCESS-SIGNATURE"]));
  // PSS is randomized: two signatures differ but both verify.
  std::string s2 = s.sign("abc");
  CHECK(s2 != s.sign("abc"));
  CHECK(s.verify("abc", s2));
  // Cross-check against the openssl CLI verifier (same padding parameters Kalshi uses).
  {
    std::ofstream("/tmp/desk_msg.txt", std::ios::binary) << "1234567890000GET/trade-api/v2/portfolio/balance";
    std::string sig = Signer::unbase64(h["KALSHI-ACCESS-SIGNATURE"]);
    std::ofstream("/tmp/desk_sig.bin", std::ios::binary) << sig;
    int pk = std::system(("openssl pkey -in " + pem + " -pubout -out /tmp/desk_pub.pem 2>/dev/null").c_str());
    CHECK(pk == 0);
    int v = std::system(
        "openssl dgst -sha256 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:digest "
        "-verify /tmp/desk_pub.pem -signature /tmp/desk_sig.bin /tmp/desk_msg.txt >/dev/null 2>&1");
    CHECK(v == 0);
  }
  std::remove(pem.c_str());
}

static void test_order_body() {
  OrderRequest r;
  r.ticker = "KXHIGHNY-26SEP06-B75.5";
  r.outcome_side = "yes";
  r.price = 0.40;
  r.count = 5;
  r.post_only = true;
  json b = KalshiClient::order_body(r);
  CHECK(b["side"] == "bid");
  CHECK(b["price"] == "0.4000");
  CHECK(b["count"] == "5.00");
  CHECK(b["post_only"] == true);
  r.outcome_side = "no";
  r.price = 0.30;  // buy NO at 30c == ask YES at 70c
  b = KalshiClient::order_body(r);
  CHECK(b["side"] == "ask");
  CHECK(b["price"] == "0.7000");
}

static void test_parse_market() {
  json j = json::parse(R"({"ticker":"KXHIGHNY-26SEP06-B75.5","event_ticker":"KXHIGHNY-26SEP06",
    "strike_type":"between","floor_strike":75,"cap_strike":76,"status":"active",
    "yes_bid_dollars":"0.3200","yes_ask_dollars":"0.3300","no_bid_dollars":"0.6700","no_ask_dollars":"0.6800",
    "last_price_dollars":"0.3200","yes_ask_size_fp":"120.00","volume_fp":"55.00",
    "price_ranges":[{"start":"0.0000","end":"1.0000","step":"0.0100"}],"close_time":"2026-09-07T05:00:00Z"})");
  Market m = parse_market(j);
  CHECK(m.series_ticker == "KXHIGHNY");
  CHECK(m.floor_strike && *m.floor_strike == 75);
  CHECK(m.cap_strike && *m.cap_strike == 76);
  CHECK_NEAR(m.yes_ask, 0.33, 1e-9);
  CHECK_NEAR(m.yes_ask_size, 120, 1e-9);
  CHECK_NEAR(m.tick, 0.01, 1e-12);
  CHECK(m.tradable());
  json ob = json::parse(R"({"orderbook_fp":{"yes_dollars":[["0.3000","10.00"],["0.3200","5.00"]],"no_dollars":[["0.6500","7.00"],["0.6700","3.00"]]}})");
  Orderbook b = parse_orderbook(ob, "T");
  CHECK_NEAR(b.best_yes_bid(), 0.32, 1e-9);
  CHECK_NEAR(b.best_no_bid(), 0.67, 1e-9);
  CHECK_NEAR(b.best_yes_ask(), 0.33, 1e-9);
  CHECK_NEAR(b.best_no_ask(), 0.68, 1e-9);
}

static void test_store() {
  std::string path = "/tmp/desk_test.sqlite";
  std::remove(path.c_str());
  {
    Store st(path);
    Series s{"KXHIGHNY", "Climate and Weather", "Highest temperature in NYC", "quadratic", "daily", 1.0};
    st.upsert_series(s);
    auto got = st.get_series("KXHIGHNY");
    CHECK(got && got->category == "Climate and Weather");
    Market m;
    m.ticker = "KXHIGHNY-26SEP06-B75.5";
    m.event_ticker = "KXHIGHNY-26SEP06";
    m.series_ticker = "KXHIGHNY";
    m.status = "active";
    m.strike_type = "between";
    m.floor_strike = 75;
    m.cap_strike = 76;
    m.yes_bid = 0.32;
    m.yes_ask = 0.33;
    st.upsert_market(m);
    auto ms = st.open_markets("KXHIGHNY");
    CHECK(ms.size() == 1 && ms[0].cap_strike && *ms[0].cap_strike == 76);
    m.yes_ask = 0.35;
    st.upsert_market(m);
    CHECK_NEAR(st.get_market(m.ticker)->yes_ask, 0.35, 1e-9);
    st.insert_quote(1, m);
    Proposal p;
    p.ticker = m.ticker;
    p.event_ticker = m.event_ticker;
    p.strategy = "manual_fair";
    p.mode = "manual";
    p.outcome_side = "yes";
    p.exec_style = "maker";
    p.status = "pending";
    p.price = 0.30;
    p.count = 4;
    p.p_model = 0.40;
    st.insert_proposal(p);
    CHECK(p.id > 0);
    auto f = st.find_open_proposal(m.ticker, "yes", "manual_fair");
    CHECK(f && f->id == p.id);
    p.status = "submitted";
    p.order_id = "abc";
    st.update_proposal(p);
    CHECK(st.get_proposal(p.id)->status == "submitted");
    Order o;
    o.order_id = "abc";
    o.ticker = m.ticker;
    o.status = "resting";
    o.remaining_count = 4;
    st.upsert_order(o, p.id);
    CHECK(st.proposal_for_order("abc") == p.id);
    CHECK(st.orders("resting").size() == 1);
    FairValue fv{"X", "note", 0.6, 0.05, 0};
    st.set_fair(fv);
    CHECK(st.fairs().size() == 1);
    st.clear_fair("X");
    CHECK(st.fairs().empty());
    st.kv_set("k", "v");
    CHECK(st.kv_get("k") && *st.kv_get("k") == "v");
    Hypothesis h;
    h.key = "T-1";
    h.category = "Test";
    h.title = "t";
    h.status = "idea";
    st.upsert_hypothesis(h);
    CHECK(st.hypotheses().size() == 1);
    // expire: proposals older than TTL
    CHECK(st.expire_proposals(now_s() + 10) == 0);  // the only proposal is submitted, not pending
    json rows = st.query("SELECT COUNT(*) AS n FROM quotes");
    CHECK(rows[0]["n"] == 1);
  }
  std::remove(path.c_str());
}

static void test_engine_candidates() {
  std::string path = "/tmp/desk_test2.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  KalshiClient client("https://example.invalid/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  Series s{"KXHIGHNY", "Climate and Weather", "", "quadratic", "daily", 1.0};
  Market m;
  m.ticker = "T";
  m.event_ticker = "E";
  m.yes_bid = 0.40;
  m.yes_ask = 0.42;
  m.no_bid = 0.58;
  m.no_ask = 0.60;
  m.yes_ask_size = 50;
  m.yes_bid_size = 50;
  Fair f;
  f.p = 0.50;
  f.sigma = 0.02;
  auto c = eng.candidates(m, s, f);
  CHECK(!c.empty());
  // Best: taker YES at 0.42 -> edge 0.08 (maker fee 0 on quadratic); maker YES at min(0.41, floor(0.47)) = 0.41 -> edge 0.09
  CHECK(c.front().outcome_side == "yes");
  CHECK(c.front().exec_style == "maker");
  CHECK_NEAR(c.front().price, 0.41, 1e-9);
  CHECK_NEAR(c.front().net_edge, 0.09, 1e-9);
  double n = eng.size_for(c.front(), m, 100, 0, 0, 0);
  // quarter Kelly: f* = (0.5-0.41)/(0.59)=0.1525 -> 0.0381 -> $3.81/0.41 = 9 contracts
  CHECK_NEAR(n, 9, 1e-9);
  // Event cap: 10% of 100 = $10 already used -> no room
  CHECK_NEAR(eng.size_for(c.front(), m, 100, 10.0, 0, 0), 0, 1e-9);
  // No edge: p == market
  f.p = 0.41;
  CHECK(eng.candidates(m, s, f).empty());
  // NO side edge
  f.p = 0.30;
  auto c2 = eng.candidates(m, s, f);
  CHECK(!c2.empty() && c2.front().outcome_side == "no");
  // Maker fee series lowers maker edge
  Series s2{"KXCPI", "Economics", "", "quadratic_with_maker_fees", "monthly", 1.0};
  f.p = 0.50;
  auto c3 = eng.candidates(m, s2, f);
  CHECK(!c3.empty());
  bool found_maker = false;
  for (const auto& x : c3)
    if (x.exec_style == "maker" && x.outcome_side == "yes") {
      found_maker = true;
      CHECK_NEAR(x.net_edge, 0.09 - 0.0043, 1e-9);  // 0.0175*0.41*0.59=0.00423 -> 0.0043
    }
  CHECK(found_maker);
  std::remove(path.c_str());
}

static void test_weather_math() {
  Market m;
  m.strike_type = "between";
  m.floor_strike = 79;
  m.cap_strike = 80;
  // No obs: T ~ N(79.5, 2): P(78.5 <= T < 80.5)
  double p = WeatherNws::bracket_prob(m, 79.5, 2.0, -999, false, false);
  CHECK_NEAR(p, norm_cdf(0.5) - norm_cdf(-0.5), 1e-9);
  // Observed max already 81 -> bracket impossible
  CHECK_NEAR(WeatherNws::bracket_prob(m, 79.5, 2.0, 81, true, false), 0.0, 1e-12);
  // Observed 80 (in bracket): P(T < 80.5) - P(T < 78.5) = Phi(0.5) - 0
  CHECK_NEAR(WeatherNws::bracket_prob(m, 79.5, 2.0, 80, true, false), norm_cdf(0.5), 1e-9);
  Market g;
  g.strike_type = "greater";
  g.floor_strike = 80;  // 81 or above
  CHECK_NEAR(WeatherNws::bracket_prob(g, 79.5, 2.0, 81, true, false), 1.0, 1e-12);
  CHECK_NEAR(WeatherNws::bracket_prob(g, 79.5, 2.0, -999, false, false), 1 - norm_cdf(0.5), 1e-9);
  Market l;
  l.strike_type = "less";
  l.cap_strike = 79;  // 78 or below
  CHECK_NEAR(WeatherNws::bracket_prob(l, 79.5, 2.0, -999, false, false), norm_cdf(-0.5), 1e-9);
  CHECK_NEAR(WeatherNws::bracket_prob(l, 79.5, 2.0, 79, true, false), 0.0, 1e-12);
  // Lognormal sanity: ATM-ish, short horizon ~ 0.5
  CHECK_NEAR(lognormal_prob_between(100, INFINITY, 100, 0.5, 1.0 / 8760), 0.5, 0.01);
  CHECK_NEAR(lognormal_prob_between(0, 100, 100, 0.5, 1.0 / 8760) + lognormal_prob_between(100, INFINITY, 100, 0.5, 1.0 / 8760), 1.0, 1e-9);
}

static void test_bootstrap() {
  std::vector<double> x(200, 0.05);
  BootCI b = bootstrap_mean(x);
  CHECK_NEAR(b.mean, 0.05, 1e-12);
  CHECK_NEAR(b.lo, 0.05, 1e-12);
  std::vector<double> y;
  for (int i = 0; i < 400; ++i) y.push_back(i % 2 ? 1.0 : -0.9);
  BootCI c = bootstrap_mean(y);
  CHECK(c.lo < c.mean && c.mean < c.hi);
  CHECK_NEAR(c.mean, 0.05, 1e-9);
}

static void test_sports() {
  MatchProbs m = match_probs(1.5, 1.1);
  CHECK_NEAR(m.home + m.draw + m.away, 1.0, 1e-9);
  CHECK(m.home > m.away);
  auto [lh, la] = fit_lambdas(m.home, m.draw);
  CHECK_NEAR(lh, 1.5, 0.06);
  CHECK_NEAR(la, 1.1, 0.06);
  // Dixon-Coles: same (p_home, p_draw) fits with more goals when rho < 0
  auto [lh2, la2] = fit_lambdas(0.19, 0.31, -0.13);
  auto [lh3, la3] = fit_lambdas(0.19, 0.31, 0.0);
  CHECK(lh2 + la2 > lh3 + la3 + 0.3);
  MatchProbs dc = match_probs(1.5, 1.1, 0, -0.13);
  CHECK(dc.draw > m.draw);
  // tied late: draw dominates; a goal up late: leader dominates
  double r = 15.0 / 95.0 * 1.2;
  CHECK(match_probs(1.5 * r, 1.1 * r, 0).draw > 0.6);
  CHECK(match_probs(1.5 * r, 1.1 * r, 1).home > 0.8);
  std::string h, a;
  CHECK(SportsInplay::parse_teams("If Tie is the result of the Nottingham Forest vs Coventry professional EPL soccer game originally scheduled for Sep 19, 2026 after 90 minutes", h, a));
  CHECK(h == "Nottingham Forest");
  CHECK(a == "Coventry");
  CHECK(SportsInplay::parse_teams("If Coventry wins the Nottingham Forest vs Coventry professional EPL soccer game", h, a));
  CHECK(h == "Nottingham Forest" && a == "Coventry");
  CHECK_NEAR(SportsInplay::name_match("Coventry", "Coventry City"), 1.0, 1e-9);
  CHECK_NEAR(SportsInplay::name_match("Nottingham Forest", "Nottingham Forest"), 1.0, 1e-9);
  CHECK(SportsInplay::name_match("Manchester City", "Manchester United") < 0.6);
  CHECK_NEAR(SportsInplay::name_match("AFC Bournemouth", "Bournemouth"), 1.0, 1e-9);
  CHECK_NEAR(SportsInplay::parse_clock("67'", 2, "STATUS_SECOND_HALF"), 67, 1e-9);
  CHECK_NEAR(SportsInplay::parse_clock("90'+7'", 2, "STATUS_SECOND_HALF"), 97, 1e-9);
  CHECK_NEAR(SportsInplay::parse_clock("45'+2'", 1, "STATUS_FIRST_HALF"), 47, 1e-9);
  CHECK_NEAR(SportsInplay::parse_clock("0'", 2, "STATUS_HALFTIME"), 45, 1e-9);
  Market mk;
  mk.ticker = "KXEPLGAME-26AUG30LEEBRE-LEE";
  mk.title = "Leeds United vs Brentford Winner?";
  mk.rules_primary = "If Leeds United wins the Leeds United vs Brentford professional EPL soccer game originally scheduled";
  CHECK(SportsInplay::market_side(mk, "Leeds United", "Brentford") == "home");
  mk.rules_primary = "If Brentford wins the Leeds United vs Brentford professional EPL soccer game";
  CHECK(SportsInplay::market_side(mk, "Leeds United", "Brentford") == "away");
  mk.ticker = "KXEPLGAME-26AUG30LEEBRE-TIE";
  mk.rules_primary = "If Tie is the result of the Leeds United vs Brentford professional EPL soccer game";
  CHECK(SportsInplay::market_side(mk, "Leeds United", "Brentford") == "tie");
}

static void test_auto_rules() {
  std::string path = "/tmp/desk_test3.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_params["weather_nws"] = json{{"auto", {{"min_edge", 0.04}, {"max_cost", 6.0}, {"styles", json::array({"maker"})}}}};
  KalshiClient client("https://example.invalid/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  Market m;
  m.ticker = "T";
  m.event_ticker = "E";
  m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z";
  st.upsert_market(m);
  Proposal p;
  p.ticker = "T";
  p.strategy = "weather_nws";
  p.mode = "auto";
  p.exec_style = "maker";
  p.price = 0.40;
  p.count = 10;
  p.fee_per = 0;
  p.net_edge = 0.05;
  p.sigma = 0.05;
  std::string why;
  CHECK(eng.auto_rule_ok(p, &why));
  p.net_edge = 0.03;
  CHECK(!eng.auto_rule_ok(p, &why));
  p.net_edge = 0.05;
  p.count = 20;  // $8 > $6
  CHECK(!eng.auto_rule_ok(p, &why));
  p.count = 10;
  p.exec_style = "taker";
  CHECK(!eng.auto_rule_ok(p, &why));
  p.exec_style = "maker";
  p.sigma = 0.3;
  CHECK(!eng.auto_rule_ok(p, &why));
  p.sigma = 0.05;
  m.close_time = "2000-01-01T00:00:00Z";  // already closed -> minutes_to_close negative
  st.upsert_market(m);
  CHECK(!eng.auto_rule_ok(p, &why));
  // default rule for a strategy without params
  p.strategy = "crypto_lognormal";
  m.close_time = "2099-01-01T00:00:00Z";
  st.upsert_market(m);
  CHECK(eng.auto_rule_ok(p, &why));
  st.log_event("info", "hello");
  CHECK(st.events(5).size() == 1);
  std::remove(path.c_str());
}

static void test_event_cap() {
  std::string path = "/tmp/desk_test4.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_modes["manual_fair"] = "manual";
  cfg.watch_series = {"KXT"};
  Series sr{"KXT", "Crypto", "", "quadratic", "hourly", 1.0};
  st.upsert_series(sr);
  for (int i = 0; i < 4; ++i) {
    Market m;
    m.ticker = "KXT-E-B" + std::to_string(i);
    m.event_ticker = "KXT-E";
    m.series_ticker = "KXT";
    m.status = "active";
    m.strike_type = "between";
    m.floor_strike = i;
    m.cap_strike = i + 1;
    m.close_time = "2099-01-01T00:00:00Z";
    m.yes_bid = 0.10;
    m.yes_ask = 0.12;
    m.no_bid = 0.88;
    m.no_ask = 0.90;
    m.yes_bid_size = 500;
    m.yes_ask_size = 500;
    st.upsert_market(m);
    FairValue f{m.ticker, "t", 0.02, 0.01, 0};  // NO is worth 0.98: big edge on every bracket
    st.set_fair(f);
  }
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");  // unreachable: refresh fails, store is used
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  // series cache comes from the store when the exchange is unreachable
  for (int k = 0; k < 4; ++k) eng.cycle();
  double used = 0;
  int n = 0;
  for (const auto& p : st.proposals("pending", 100)) { used += p.count * p.price; ++n; }
  CHECK(n >= 1);
  CHECK(used <= cfg.risk.max_event_frac * cfg.bankroll_dollars + 1e-9);  // never over the per-event cap
  // The surviving proposal is refreshed every cycle (updated_ts moves), so it never starves into TTL expiry.
  auto pend = st.proposals("pending", 10);
  CHECK(!pend.empty());
  long long before = pend.front().updated_ts;
  st.exec("UPDATE proposals SET updated_ts=updated_ts-100 WHERE status='pending'");
  eng.cycle();
  auto pend2 = st.proposals("pending", 10);
  CHECK(!pend2.empty() && pend2.front().id == pend.front().id);
  CHECK(pend2.front().updated_ts >= before);
  std::remove(path.c_str());
}


static void test_risk_regressions() {
  Store st(":memory:"); Config cfg;
  Series series{"KXHIGHNY","Climate and Weather","","quadratic","daily",1.0}; st.upsert_series(series);
  Market m; m.ticker="KXHIGHNY-26SEP05-B79.5"; m.event_ticker="KXHIGHNY-26SEP05";
  m.series_ticker="KXHIGHNY";m.status="active";m.close_time="2099-01-01T00:00:00Z";
  m.rules_primary="According to the National Weather Service daily climate report";
  m.exchange_index=2; st.upsert_market(m);
  CHECK(st.get_market(m.ticker)->exchange_index==2);
  CHECK(weather_source_supported(m));
  m.rules_primary="According to The Weather Company";st.upsert_market(m);
  CHECK(!weather_source_supported(*st.get_market(m.ticker)));
  CHECK(st.get_market(m.ticker)->rules_primary==m.rules_primary);
  CHECK(!weather_source_supported(Market{}));
  Position pos;pos.ticker=m.ticker;pos.position=12.94;pos.exposure=2.9762;st.replace_positions({pos});
  Order o;o.order_id="rest";o.ticker=m.ticker;o.outcome_side="yes";o.status="resting";o.remaining_count=.06;o.yes_price=.23;st.upsert_order(o);
  Proposal p;p.ticker=m.ticker;p.event_ticker=m.event_ticker;p.series_ticker=m.series_ticker;
  p.strategy="ticket";p.status="pending";p.outcome_side="yes";p.exec_style="maker";p.price=.2;p.count=1;p.cluster="wrong-old-cluster";st.insert_proposal(p);
  auto exposure=committed_exposure(st);
  CHECK_NEAR(exposure.total(),2.99,1e-8); // unsent proposal excluded
  CHECK_NEAR(exposure.clusters["wx|2026-09-05"],2.99,1e-8); // held + resting use same cluster
  CHECK(exposure.clusters.size()==1);
  st.insert_account(now_s()-10,100,0);st.insert_account(now_s(),90,10);
  st.kv_set("day_start:"+now_iso().substr(0,10),"0");
  auto d=daily_equity(st,cfg);CHECK_NEAR(d["start_pv"].get<double>(),100,1e-8);
  CHECK_NEAR(d["pnl"].get<double>(),0,1e-8); // cash -> contracts is not a trading loss
  st.kv_set("portfolio_sync_ts",std::to_string(now_s()));
  auto error=[&](const Proposal& candidate){try{validate_entry(st,cfg,candidate,m);return std::string();}catch(const std::exception& e){return std::string(e.what());}};
  CHECK(error(p).empty()); // discretionary ticket still allowed under caps
  p.strategy="weather_nws";CHECK(error(p).find("research-only")!=std::string::npos);p.strategy="ticket";
  p.count=40;CHECK(error(p).find("Event cap")!=std::string::npos);p.count=1;
  st.kv_set("portfolio_sync_ts","0");CHECK(error(p).find("stale")!=std::string::npos);st.kv_set("portfolio_sync_ts",std::to_string(now_s()));
  p.count=.019;CHECK(!error(p).empty());p.count=.01;CHECK(error(p).empty());p.count=1;
  p.price=std::nan("");CHECK(!error(p).empty());p.price=.2;
  st.insert_account(now_s(),80,9);CHECK(error(p).find("Daily equity")!=std::string::npos);
  st.insert_account(now_s(),90,10);
  cfg.risk.max_cluster_frac=.03;CHECK(error(p).find("Cluster cap")!=std::string::npos);cfg.risk.max_cluster_frac=.25;
  cfg.risk.max_open_frac=.03;CHECK(error(p).find("Total exposure")!=std::string::npos);cfg.risk.max_open_frac=.60;
  st.kv_set("kill","1");CHECK(error(p).find("kill")!=std::string::npos);st.kv_set("kill","0");
  CHECK_NEAR(floor_contracts(12.94),12.94,1e-10);CHECK_NEAR(floor_contracts(.06),.06,1e-10);
  OrderRequest req;req.ticker=m.ticker;req.count=12.94;req.price=.72;req.outcome_side="no";req.reduce_only=true;req.exchange_index=2;
  auto b=KalshiClient::order_body(req);CHECK(b["count"]=="12.94");CHECK(b["price"]=="0.2800");CHECK(b["exchange_index"]==2);CHECK(b["reduce_only"]==true);
  req.exchange_index=-1;CHECK(KalshiClient::order_body(req)["exchange_index"]==-1);
}



// A strategy that returns whatever the test wants for every KXT market.
struct TestStrategy : public Strategy {
  Fair f;
  int prepared = 0;
  std::string name() const override { return "test_model"; }
  std::string category() const override { return "*"; }
  bool covers(const Market& m, const Series&) const override { return m.series_ticker == "KXT"; }
  void prepare(const std::vector<Market>&, Store&) override { ++prepared; }
  std::optional<Fair> fair(const Market&, const Series&, Store&) override { return f; }
};

static void seed_kxt(Store& st, double yes_bid, double yes_ask) {
  Series sr{"KXT", "Crypto", "", "quadratic", "hourly", 1.0};
  st.upsert_series(sr);
  for (int i = 0; i < 2; ++i) {
    Market m;
    m.ticker = "KXT-E-B" + std::to_string(i);
    m.event_ticker = "KXT-E";
    m.series_ticker = "KXT";
    m.status = "active";
    m.strike_type = "between";
    m.floor_strike = i;
    m.cap_strike = i + 1;
    m.close_time = "2099-01-01T00:00:00Z";
    m.yes_bid = yes_bid;
    m.yes_ask = yes_ask;
    m.no_bid = 1 - yes_ask;
    m.no_ask = 1 - yes_bid;
    m.yes_bid_size = 500;
    m.yes_ask_size = 500;
    st.upsert_market(m);
  }
}

static void test_instant_lane() {
  // The fast lane: an edge this big is gone by the time a human confirms it, so a proposal that
  // clears the bar goes in the cycle it appears - even from a strategy in manual mode - while
  // every other guard stays exactly where it was.
  std::string path = "/tmp/desk_instant.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.risk.instant_edge = 0.05;
  cfg.risk.instant_max_cost = 5.0;
  cfg.strategy_modes["test_model"] = "manual";
  cfg.watch_series = {"KXT"};
  seed_kxt(st, 0.10, 0.12);
  st.insert_account(now_s(), 100, 0);
  st.kv_set("portfolio_sync_ts", std::to_string(now_s()));
  // auto_submit only runs for an armed executor, so this test needs a signing key. A throwaway
  // one is enough: the client points at a dead port and every send fails at the network, which
  // is exactly what we want - the question is whether the lane TRIED without being asked.
  std::string pem = "/tmp/desk_instant_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip instant lane test: openssl CLI not available\n");
    return;
  }
  auto signer = std::make_shared<Signer>("test-key-id", pem);
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", signer, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  CHECK(ex.armed());
  CHECK_NEAR(eng.instant_edge(), 0.05, 1e-9);        // the config value is the default
  eng.set_instant_edge(0.25);                        // and the dashboard can move it
  CHECK_NEAR(eng.instant_edge(), 0.25, 1e-9);
  eng.set_instant_edge(0.05);
  bool threw = false;
  try { eng.set_instant_edge(1.5); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  CHECK_NEAR(eng.instant_edge(), 0.05, 1e-9);        // a bad value changes nothing
  // The lane needs AUTO armed like every other unattended send.
  auto ts = std::make_unique<TestStrategy>();
  TestStrategy* tsp = ts.get();
  tsp->f.p = 0.30;                                   // vs an ask of 0.12: a big edge
  tsp->f.sigma = 0.02;
  eng.add_strategy(std::move(ts));
  eng.cycle();
  auto pend = st.proposals("pending", 10);
  CHECK(!pend.empty());                              // queued, because AUTO is off
  double best_edge = 0;
  for (const auto& pr : pend) best_edge = std::max(best_edge, pr.net_edge);
  CHECK(best_edge > 0.05);
  for (const auto& pr : pend) CHECK(pr.status == "pending");
  // Armed: the same proposal is picked up without a click. The client points at a dead port, so
  // the send fails at the network - what matters is that the lane tried, and said why.
  eng.set_auto_armed(true);
  eng.cycle();
  bool tried = false;
  for (const auto& pr : st.proposals("", 40))
    if (pr.note.find("INSTANT") != std::string::npos) tried = true;
  CHECK(tried);                                      // manual mode, no click, and it went for it
  // Off again: the same row goes back to waiting.
  eng.set_instant_edge(0.90);
  for (auto pr : st.proposals("pending", 10)) { pr.note = ""; st.update_proposal(pr); }
  eng.cycle();
  bool tried_again = false;
  for (const auto& pr : st.proposals("pending", 40))
    if (pr.note.find("INSTANT") != std::string::npos) tried_again = true;
  CHECK(!tried_again);
  std::remove(path.c_str());
}

static void test_research_gates() {
  // 1. Disagreement gate: a model 49 points from a two-sided market logs its number, proposes nothing.
  {
    std::string path = "/tmp/desk_test5.sqlite";
    std::remove(path.c_str());
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    seed_kxt(st, 0.10, 0.12);
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.60;  // vs mid 0.11
    tsp->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    eng.cycle();
    CHECK(tsp->prepared == 1);
    CHECK(st.proposals("pending", 10).empty());
    CHECK(!st.query("SELECT * FROM model_fairs WHERE strategy='test_model'", {}).empty());  // Brier sample still logged
    auto ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.note.find("RESEARCH ONLY") != std::string::npos);
    CHECK(ff && ff->first.note.find("disagreement") != std::string::npos);
    // Same market, model close to the market (0.20 vs 0.11): proposes as before.
    tsp->f.p = 0.20;
    eng.cycle();
    CHECK(!st.proposals("pending", 10).empty());
    // A pending proposal is withdrawn once the model drifts into research-only territory.
    tsp->f.p = 0.60;
    eng.cycle();
    CHECK(st.proposals("pending", 10).empty());
    // Strategy-flagged research_only behaves the same way.
    tsp->f.p = 0.20;
    tsp->f.research_only = true;
    tsp->f.research_reason = "unit test";
    eng.cycle();
    CHECK(st.proposals("pending", 10).empty());
    ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.note.find("unit test") != std::string::npos);
    // "research" mode: prepare() still runs (collectors keep logging), nothing is proposed.
    tsp->f.research_only = false;
    eng.set_mode("test_model", "research");
    int before = tsp->prepared;
    eng.cycle();
    CHECK(tsp->prepared == before + 1);
    CHECK(st.proposals("pending", 10).empty());
    bool threw = false;
    try { eng.set_mode("test_model", "bogus"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    std::remove(path.c_str());
  }
  // 1b. One-sided books still carry an opinion: bids only at 0.90 vs a model at 0.30 is exactly
  //     the trade a wrong model would make (sell into the bid), so the gate applies to the side
  //     that exists. Asks only at 0.12 vs a model at 0.60: same. A model inside the side's bound
  //     (0.95 above a 0.90 bid) is not a disagreement.
  {
    std::string path = "/tmp/desk_test5b.sqlite";
    std::remove(path.c_str());
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    seed_kxt(st, 0.90, 1.0);  // bids only (ask placeholder 1.00)
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.30;
    tsp->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    eng.cycle();
    auto ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.research_only && ff->first.note.find("disagreement 0.60") != std::string::npos);
    CHECK(st.proposals("pending", 10).empty());
    tsp->f.p = 0.95;  // above the bid: the bid does not contradict it
    eng.cycle();
    ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && !ff->first.research_only);
    seed_kxt(st, 0.0, 0.12);  // asks only
    tsp->f.p = 0.60;
    eng.cycle();
    ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.research_only && ff->first.note.find("disagreement 0.48") != std::string::npos);
    tsp->f.p = 0.05;  // below the ask: fine
    eng.cycle();
    ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && !ff->first.research_only);
    std::remove(path.c_str());
  }
  // 2. Same-day weather: residual carry, never centered below the running max, research-only by default.
  {
    WeatherNws wx(json::object(), "t");
    WeatherNws::DayForecast d;
    d.date = "2026-09-05";
    d.fc_max = 79; d.fc_min = 68; d.fc_day_max = 79;
    d.obs_max = 79; d.obs_min = 66; d.have_obs = true;
    d.t_now = 79; d.fc_at_now = 76; d.resid = 3; d.have_resid = true;
    d.offset_seconds = -7 * 3600;  // PDT
    d.fetched = 1;
    wx.set_day_forecast("KXHIGHLAX", d);
    wx.set_clock(parse_iso("2026-09-05T18:33:00Z"));  // 11:33 PDT, the LAX B79.5 trade
    Market m;
    m.ticker = "KXHIGHLAX-26SEP05-B79.5"; m.event_ticker = "KXHIGHLAX-26SEP05"; m.series_ticker = "KXHIGHLAX";
    m.strike_type = "between"; m.floor_strike = 79; m.cap_strike = 80;
    Series s{"KXHIGHLAX", "Climate and Weather", "", "quadratic", "daily", 1.0};
    Store st(":memory:");
    auto f = wx.fair(m, s, st);
    CHECK(f.has_value());
    CHECK(f->research_only);  // WX-1b: same-day is research-only until same_day_trading is set
    CHECK(f->p < 0.55);       // the old model said 0.68 here; the day settled at 81
    CHECK(f->note.find("resid +3.0") != std::string::npos);
    // The >80 market from the same afternoon: old model 0.32, settled YES.
    Market g = m; g.ticker = "KXHIGHLAX-26SEP05-T80"; g.strike_type = "greater"; g.floor_strike = 80; g.cap_strike.reset();
    auto fg = wx.fair(g, s, st);
    CHECK(fg && fg->p > 0.40);
    // Opt-in flag switches same-day proposals back on.
    WeatherNws wx2(json{{"same_day_trading", true}}, "t");
    wx2.set_day_forecast("KXHIGHLAX", d);
    wx2.set_clock(parse_iso("2026-09-05T18:33:00Z"));
    auto f2 = wx2.fair(m, s, st);
    CHECK(f2 && !f2->research_only);
    // Day-ahead is research-only too since WX-1c: the tape beat this model at every decision
    // point the day before, and the same-day flag does not speak for it.
    wx.set_clock(parse_iso("2026-09-04T18:33:00Z"));
    auto f3 = wx.fair(m, s, st);
    CHECK(f3 && f3->research_only && f3->research_reason.find("WX-1c") != std::string::npos);
    WeatherNws wxd(json{{"day_ahead_trading", true}}, "t");
    wxd.set_day_forecast("KXHIGHLAX", d);
    wxd.set_clock(parse_iso("2026-09-04T18:33:00Z"));
    auto f3b = wxd.fair(m, s, st);
    CHECK(f3b && !f3b->research_only);       // the override is there for when a model earns it
    // The same-day flag alone does not unlock the day before.
    wx2.set_clock(parse_iso("2026-09-04T18:33:00Z"));
    auto f3c = wx2.fair(m, s, st);
    CHECK(f3c && f3c->research_only && f3c->research_reason.find("WX-1c") != std::string::npos);
    // Day-ahead uses the whole-day forecast, never a remaining-hours number left over from an
    // evening refresh (QA: after local midnight the old code priced off the 11pm forecast).
    WeatherNws::DayForecast e = d;
    e.fc_max = 70; e.fc_day_max = 84; e.have_obs = false; e.have_resid = false;
    WeatherNws wx3(json::object(), "t");
    wx3.set_day_forecast("KXHIGHLAX", e);
    wx3.set_clock(parse_iso("2026-09-04T18:33:00Z"));
    auto f4 = wx3.fair(g, s, st);  // >80 with a day-ahead forecast of 84
    CHECK(f4 && f4->p > 0.80);
    // The day has ended (00:30 PDT next day) while the market waits for the climate report:
    // the day's METAR max settles it (plus the sensor shift), the stale forecast does not.
    WeatherNws::DayForecast past = d;
    past.fc_max = 70; past.obs_max = 81.3; past.day_complete = true;
    WeatherNws wx4(json::object(), "t");
    wx4.set_day_forecast("KXHIGHLAX", past);
    wx4.set_clock(parse_iso("2026-09-06T07:30:00Z"));
    auto f5 = wx4.fair(g, s, st);  // >80: obs 81 -> YES under every shift
    CHECK(f5 && f5->p > 0.99 && f5->research_only);
    CHECK(f5->note.find("day over") != std::string::npos);
    auto f6 = wx4.fair(m, s, st);  // between 79-80: obs 81 -> NO under every shift
    CHECK(f6 && f6->p < 0.01);
    Market h = g; h.ticker = "KXHIGHLAX-26SEP05-B81.5"; h.strike_type = "between"; h.floor_strike = 81; h.cap_strike = 82;
    auto f7 = wx4.fair(h, s, st);  // 81-82: hit for shift 0 and +1 (0.63 + 0.31)
    CHECK(f7 && std::fabs(f7->p - 0.94) < 1e-6);
    // Without the day's observations there is no opinion at all after midnight.
    past.day_complete = false;
    wx4.set_day_forecast("KXHIGHLAX", past);
    CHECK(!wx4.fair(g, s, st).has_value());
  }
  // 3. Daily equity: Kalshi's reported cash already includes resting reservations (observed
  //    2026-09-05: cash moved only by fills and fees while orders rested), so a resting order
  //    changes neither equity nor the daily P&L; the cash check subtracts reservations instead.
  {
    Store st(":memory:");
    Config cfg;
    Series series{"KXHIGHNY", "Climate and Weather", "", "quadratic", "daily", 1.0};
    st.upsert_series(series);
    Market m; m.ticker = "KXHIGHNY-26SEP05-B79.5"; m.event_ticker = "KXHIGHNY-26SEP05"; m.series_ticker = "KXHIGHNY";
    m.status = "active"; m.close_time = "2099-01-01T00:00:00Z"; st.upsert_market(m);
    st.insert_account(now_s() - 10, 100, 0);
    auto d0 = daily_equity(st, cfg);
    CHECK_NEAR(d0["start_pv"].get<double>(), 100, 1e-8);
    Order o; o.order_id = "r1"; o.ticker = m.ticker; o.outcome_side = "yes"; o.status = "resting";
    o.remaining_count = 100; o.yes_price = 0.30; st.upsert_order(o);
    st.insert_account(now_s(), 100, 0);
    auto d1 = daily_equity(st, cfg);
    CHECK_NEAR(d1["pv"].get<double>(), 100, 1e-8);
    CHECK_NEAR(d1["pnl"].get<double>(), 0, 1e-8);
    CHECK(d1["pv"].get<double>() > d1["stop_pv"].get<double>());
    CHECK_NEAR(committed_exposure(st).resting, 30.0, 1e-8);  // what the cash check subtracts
    // Moving $25 to the perpetuals instance is not a loss: the baseline shifts with the flow.
    st.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"t1", now_s(), "to_margined", -25.0});
    st.insert_account(now_s() + 1, 75, 0);
    auto d2 = daily_equity(st, cfg);
    CHECK_NEAR(d2["external_flows"].get<double>(), -25.0, 1e-8);
    CHECK_NEAR(d2["pnl"].get<double>(), 0.0, 1e-8);
    CHECK_NEAR(d2["stop_pv"].get<double>(), 75.0 * (1 - cfg.risk.daily_loss_stop_frac), 1e-8);
    // A deposit does not count as profit either.
    st.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"d1", now_s() + 1, "deposit", 50.0});
    st.insert_account(now_s() + 2, 125, 0);
    auto d3 = daily_equity(st, cfg);
    CHECK_NEAR(d3["pnl"].get<double>(), 0.0, 1e-8);
  }
}


static void test_event_netting() {
  // Payoff-space risk per event: the same bet twice is not a hedge; NO on many brackets nets.
  Store st(":memory:");
  Series sr{"KXHIGHLAX", "Climate and Weather", "", "quadratic", "daily", 1.0};
  st.upsert_series(sr);
  auto mk = [&](const std::string& t, const std::string& stype, double lo, double hi) {
    Market m; m.ticker = t; m.event_ticker = "KXHIGHLAX-26SEP05"; m.series_ticker = "KXHIGHLAX"; m.status = "active";
    m.strike_type = stype; if (stype != "less") m.floor_strike = lo; if (stype != "greater") m.cap_strike = hi;
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.30; m.yes_ask = 0.34; m.no_bid = 0.66; m.no_ask = 0.70;
    st.upsert_market(m);
  };
  mk("KXHIGHLAX-26SEP05-T78", "less", 0, 79);
  mk("KXHIGHLAX-26SEP05-B79.5", "between", 79, 80);
  mk("KXHIGHLAX-26SEP05-T80", "greater", 80, 0);
  std::vector<Holding> lax{{"KXHIGHLAX-26SEP05-B79.5", true, 18, 5.94}, {"KXHIGHLAX-26SEP05-T80", false, 13, 3.90}};
  EventRisk r = event_risk(st, "KXHIGHLAX-26SEP05", lax);
  CHECK_NEAR(r.worst_loss, 9.84, 1e-9);           // both die at 81: no credit
  CHECK(r.worst_state.find("KXHIGHLAX-26SEP05-T80=YES") != std::string::npos);
  CHECK(stacks_event(st, "KXHIGHLAX-26SEP05", {lax[0]}, lax[1]));                                   // the 2026-09-05 pair
  CHECK(!stacks_event(st, "KXHIGHLAX-26SEP05", {lax[0]}, Holding{"KXHIGHLAX-26SEP05-B79.5", false, 18, 12.0}));  // closing it
  CHECK(!stacks_event(st, "KXHIGHLAX-26SEP05", {}, lax[0]));                                        // first bet in the event
  // A real hedge: YES 79-80 plus NO 79-80 nets to nothing at risk.
  EventRisk h = event_risk(st, "KXHIGHLAX-26SEP05", {lax[0], Holding{"KXHIGHLAX-26SEP05-B79.5", false, 18, 12.06}});
  CHECK_NEAR(h.worst_loss, 0.0, 1e-9);
  // NO on two brackets: the conservative book does not assume the brackets exclude each other
  // (the local market list may be incomplete), so no cross-market credit and no second market
  // in an event that already carries risk.
  std::vector<Holding> nos{{"KXHIGHLAX-26SEP05-T78", false, 10, 8.5}, {"KXHIGHLAX-26SEP05-B79.5", false, 10, 8.5}};
  EventRisk n2 = event_risk(st, "KXHIGHLAX-26SEP05", nos);
  CHECK_NEAR(n2.worst_loss, 17.0, 1e-9);
  CHECK(stacks_event(st, "KXHIGHLAX-26SEP05", {nos[0]}, nos[1]));
  // Resting orders are optional fills: a resting hedge releases no capacity.
  EventRisk hr = event_risk(st, "KXHIGHLAX-26SEP05", {lax[0], Holding{"KXHIGHLAX-26SEP05-B79.5", false, 18, 12.06, true}});
  CHECK_NEAR(hr.worst_loss, 5.94, 1e-9);
  // committed_exposure uses the same arithmetic for held positions and resting orders.
  Position p1; p1.ticker = "KXHIGHLAX-26SEP05-B79.5"; p1.position = 18; p1.exposure = 5.94;
  Position p2; p2.ticker = "KXHIGHLAX-26SEP05-T80"; p2.position = -13; p2.exposure = 3.90;
  st.replace_positions({p1, p2});
  Exposure ex = committed_exposure(st);
  CHECK_NEAR(ex.events["KXHIGHLAX-26SEP05"], 9.84, 1e-9);
  CHECK_NEAR(ex.clusters["wx|2026-09-05"], 9.84, 1e-9);
  CHECK_NEAR(ex.total(), 9.84, 1e-9);
  // validate_entry: a strategy proposal that stacks is refused; a hand ticket is not.
  Config cfg;
  st.insert_account(now_s(), 94.06, 5.94);  // cash after the first LAX fill; event cap = $10
  st.kv_set("portfolio_sync_ts", std::to_string(now_s()));
  st.replace_positions({p1});
  Proposal q; q.ticker = "KXHIGHLAX-26SEP05-T80"; q.event_ticker = "KXHIGHLAX-26SEP05"; q.series_ticker = "KXHIGHLAX";
  q.strategy = "weather_nws"; q.status = "pending"; q.outcome_side = "no"; q.exec_style = "maker"; q.price = 0.30; q.count = 13;
  q.updated_ts = now_s();
  Market t80 = *st.get_market("KXHIGHLAX-26SEP05-T80");
  t80.rules_primary = "National Weather Service";  // so the settlement-source gate is not what fires
  st.upsert_market(t80);
  std::string err;
  try { validate_entry(st, cfg, q, t80); } catch (const std::exception& e) { err = e.what(); }
  CHECK(err.find("Stacks") != std::string::npos);
  q.strategy = "ticket";
  err.clear();
  try { validate_entry(st, cfg, q, t80); } catch (const std::exception& e) { err = e.what(); }
  CHECK(err.empty());  // 5.94 + 3.90 = 9.84 fits the $10 event cap: a hand ticket may still do it
  q.count = 14;        // one more contract and the cap refuses it, netting-aware ("adds $4.20")
  err.clear();
  try { validate_entry(st, cfg, q, t80); } catch (const std::exception& e) { err = e.what(); }
  CHECK(err.find("Event cap") != std::string::npos && err.find("adds $4.20") != std::string::npos);
  // The engine refuses to propose the stacking side and still proposes a broadening NO.
  {
    std::string path = "/tmp/desk_test6.sqlite";
    std::remove(path.c_str());
    Store st2(path);
    Config cfg2; cfg2.bankroll_dollars = 100; cfg2.strategy_modes["test_model"] = "manual"; cfg2.watch_series = {"KXT"};
    seed_kxt(st2, 0.10, 0.12);
    Position held; held.ticker = "KXT-E-B0"; held.position = -5; held.exposure = 4.5;  // NO on bracket 0
    st2.replace_positions({held});
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex2(cfg2, st2, client);
    Engine eng(cfg2, st2, client, ex2);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.02; tsp->f.sigma = 0.02;  // NO everywhere: NO B1 next to NO B0 is a second market in a risky event -> refused
    eng.add_strategy(std::move(ts));
    eng.cycle();
    CHECK(st2.proposals("pending", 10).empty());
    auto ff0 = eng.fair_for("KXT-E-B1");
    CHECK(ff0 && ff0->first.note.find("stacks") != std::string::npos);
    st2.exec("UPDATE proposals SET status='expired'");
    held.position = 5; held.exposure = 0.55;  // YES on bracket 0
    st2.replace_positions({held});
    tsp->f.p = 0.30;                            // YES everywhere: YES B1 loses wherever YES B0 loses -> refused
    eng.cycle();
    CHECK(st2.proposals("pending", 10).empty());
    auto ff = eng.fair_for("KXT-E-B1");
    CHECK(ff && ff->first.note.find("stacks") != std::string::npos);
    std::remove(path.c_str());
  }
}


static void test_sports_live() {
  // Lead/time model: even game, half left, 10-point lead in an NBA game (sigma 12).
  double p = SportsLive::leadtime_prob(10, 0, 0.5, 12.0);
  CHECK_NEAR(p, norm_cdf(10.0 / (12.0 * std::sqrt(0.5))), 1e-12);
  CHECK(p > 0.87 && p < 0.89);
  CHECK_NEAR(SportsLive::leadtime_prob(0, 0, 1.0, 12.0), 0.5, 1e-12);            // tip-off, pick'em
  CHECK_NEAR(SportsLive::leadtime_prob(0, 6, 1.0, 12.0), norm_cdf(0.5), 1e-12);  // 6-point favorite
  CHECK_NEAR(SportsLive::leadtime_prob(3, 0, 0.0, 12.0), 1.0, 1e-12);            // final, ahead
  CHECK_NEAR(SportsLive::leadtime_prob(0, 0, 0.0, 12.0), 0.5, 1e-12);            // tie at the buzzer -> OT
  CHECK(SportsLive::leadtime_prob(-7, 14, 0.9, 16.0) > 0.5);                     // a 14-point favorite down a TD early is still favored
  SportsLive::League nba{"basketball", "nba", 12.0, 4, 12, {}, true};
  CHECK_NEAR(SportsLive::frac_left(nba, 1, "12:00", "in"), 1.0, 1e-9);
  CHECK_NEAR(SportsLive::frac_left(nba, 3, "6:00", "in"), 18.0 / 48.0, 1e-9);
  CHECK_NEAR(SportsLive::frac_left(nba, 4, "0:00", "in"), 0.0, 1e-9);
  CHECK(SportsLive::frac_left(nba, 5, "3:00", "in") <= 0.05);                     // overtime
  CHECK_NEAR(SportsLive::frac_left(nba, 4, "5:00", "post"), 0.0, 1e-9);
  SportsLive::League cfb{"football", "college-football", 16.0, 4, 15, {"80", "81"}, true};
  CHECK_NEAR(SportsLive::frac_left(cfb, 2, "7:30", "in"), 37.5 / 60.0, 1e-9);
  CHECK_NEAR(SportsLive::spread_to_prob(-14.5, 11.0), norm_cdf(-14.5 / 11.0), 1e-12);
  // Kalshi US-sports rules.
  std::string a, b;
  CHECK(SportsLive::parse_us_teams("If Los Angeles D wins the Cincinnati vs Los Angeles D professional baseball game originally scheduled for Sep 8, 2026 at 10:10 PM EDT, then the market resolves to Yes.", a, b));
  CHECK(a == "Cincinnati" && b == "Los Angeles D");
  CHECK(SportsLive::parse_us_teams("If Utah Tech wins the Northern Arizona vs Utah Tech college football game originally scheduled for Sep 19, 2026, then the market resolves to Yes.", a, b));
  CHECK(a == "Northern Arizona" && b == "Utah Tech");
  CHECK(SportsLive::parse_us_teams("If New York G wins the NY Giants vs LA Rams Pro Football game originally scheduled for Sep 21, 2026, then the market resolves to Yes.", a, b));
  CHECK(a == "NY Giants" && b == "LA Rams");
  CHECK(!SportsLive::parse_us_teams("If the high temperature is 80 or above", a, b));
  CHECK(SportsLive::default_leagues().count("KXNFLGAME") == 1);
  CHECK(us_eastern_offset(parse_iso("2026-09-06T04:00:00Z")) == -4 * 3600);  // EDT
  CHECK(us_eastern_offset(parse_iso("2026-12-06T04:00:00Z")) == -5 * 3600);  // EST
  CHECK(us_eastern_offset(parse_iso("2026-11-01T05:30:00Z")) == -4 * 3600);  // 01:30 EDT, before the 2am switch
  CHECK(us_eastern_offset(parse_iso("2026-11-01T07:30:00Z")) == -5 * 3600);  // after it
}

static void test_exits_and_spread_rule() {
  std::string path = "/tmp/desk_test7.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_modes["test_model"] = "manual";
  cfg.watch_series = {"KXT"};
  seed_kxt(st, 0.40, 0.44);  // B0, B1 quoted 0.40 / 0.44
  Position held; held.ticker = "KXT-E-B0"; held.position = 10; held.exposure = 3.0;  // long YES from 0.30
  st.replace_positions({held});
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  auto ts = std::make_unique<TestStrategy>();
  TestStrategy* tsp = ts.get();
  tsp->f.p = 0.36; tsp->f.sigma = 0.02;  // fair 0.36 (shrunk 50/50 with the 0.42 mid -> 0.39)
  eng.add_strategy(std::move(ts));
  eng.cycle();
  // Maker exit: sell YES at ceil(0.39 + 0.03) = 0.42 -> buy NO at 0.58, reduce-only, our whole 10.
  Proposal* exitp = nullptr;
  auto pend = st.proposals("pending", 20);
  for (auto& p : pend) if (p.strategy == "exit") exitp = &p;
  CHECK(exitp != nullptr);
  if (exitp) {
    CHECK(exitp->ticker == "KXT-E-B0" && exitp->outcome_side == "no" && exitp->exec_style == "maker");
    CHECK_NEAR(exitp->price, 0.58, 1e-9);
    CHECK_NEAR(exitp->count, 10, 1e-9);
    CHECK(exitp->mode == "manual");
    CHECK(exitp->note.find("exit: sell YES") != std::string::npos);
  }
  // No entry proposal on the held market (never add to a position), and B1 gets an ordinary entry? fair 0.39 vs
  // ask 0.44: no YES edge; NO p_win 0.61 vs no_ask 0.60 -> maker NO at floor(0.61-0.03)=0.58 <= no_bid 0.56? 0.58 >= 0.56 ok, edge 0.03 -> allowed.
  for (const auto& p : pend) CHECK(!(p.ticker == "KXT-E-B0" && p.strategy != "exit"));
  // The bid gets rich: someone bids 0.50 for YES (shrunk fair 0.435) -> taker exit into the bid.
  st.exec("UPDATE markets SET yes_bid=0.50, yes_ask=0.52, no_bid=0.48, no_ask=0.50 WHERE ticker='KXT-E-B0'");
  eng.cycle();
  exitp = nullptr;
  pend = st.proposals("pending", 20);
  for (auto& p : pend) if (p.strategy == "exit") exitp = &p;
  CHECK(exitp != nullptr);
  if (exitp) {
    CHECK(exitp->exec_style == "taker");
    CHECK_NEAR(exitp->price, 0.50, 1e-9);  // buy NO at 1 - 0.50
  }
  // Position gone: the pending exit is withdrawn.
  st.replace_positions({});
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(p.strategy != "exit");
  // Research mode never proposes exits.
  st.replace_positions({held});
  eng.set_mode("test_model", "research");
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(p.strategy != "exit");
  // min_spread auto rule.
  cfg.strategy_params = json{{"test_model", {{"auto", {{"min_edge", 0.02}, {"max_cost", 10.0}, {"max_sigma", 0.2}, {"min_minutes_to_close", 0}, {"min_spread", 0.06}}}}}};
  Engine eng2(cfg, st, client, ex);
  Proposal q; q.ticker = "KXT-E-B1"; q.strategy = "test_model"; q.net_edge = 0.05; q.count = 1; q.price = 0.4; q.sigma = 0.02; q.exec_style = "maker";
  std::string why;
  CHECK(!eng2.auto_rule_ok(q, &why) && why.find("spread") != std::string::npos);  // B1 spread 0.04 < 0.06
  st.exec("UPDATE markets SET yes_bid=0.30, yes_ask=0.44 WHERE ticker='KXT-E-B1'");
  CHECK(eng2.auto_rule_ok(q, &why));
  std::remove(path.c_str());
}


static void test_brackets() {
  std::string path = "/tmp/desk_test8.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.watch_series = {"KXT"};
  seed_kxt(st, 0.38, 0.42);
  Position held; held.ticker = "KXT-E-B0"; held.position = 10; held.exposure = 3.0;  // long YES, avg 0.30
  st.replace_positions({held});
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  long long now = now_s();
  // Bracket: +$1.50 target (0.45), $0.80 risk (0.22), auto.
  st.exec("INSERT INTO brackets(ticker,tp_price,sl_price,trail,mode,tp_dollars,sl_dollars,note,created_ts,updated_ts,alerted) VALUES(?,?,?,?,?,?,?,?,?,?,'')",
          {"KXT-E-B0", 0.45, 0.22, 0.0, "auto", 1.5, 0.8, "", now, now});
  eng.cycle();
  auto pend = st.proposals("pending", 20);
  const Proposal* tp = nullptr;
  for (const auto& p : pend) if (p.strategy == "target") tp = &p;
  CHECK(tp != nullptr);
  if (tp) {
    CHECK(tp->outcome_side == "no" && tp->exec_style == "maker");
    CHECK_NEAR(tp->price, 0.55, 1e-9);  // buy NO at 1 - 0.45: a resting sell of our YES at 0.45
    CHECK_NEAR(tp->count, 10, 1e-9);
    CHECK(tp->mode == "bot");
  }
  for (const auto& p : pend) CHECK(p.strategy != "exit");  // the bracket owns this position's exit
  // Trailing stop: trail 0.05, bid 0.38 -> stop ratchets to 0.33 (above the 0.22 set by hand).
  st.exec("UPDATE brackets SET trail=0.05 WHERE ticker='KXT-E-B0'");
  eng.cycle();
  json b = st.query("SELECT sl_price, alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  CHECK(!b.empty() && std::fabs(b[0]["sl_price"].get<double>() - 0.33) < 1e-9);
  CHECK(b[0]["alerted"].get<std::string>().empty());
  // Bid falls through the stop: target withdrawn, a taker stop proposal appears at the bid.
  st.exec("UPDATE markets SET yes_bid=0.30, yes_ask=0.34, no_bid=0.66, no_ask=0.70 WHERE ticker='KXT-E-B0'");
  eng.cycle();
  pend = st.proposals("pending", 20);
  const Proposal* stop = nullptr; bool target_pending = false;
  for (const auto& p : pend) { if (p.strategy == "stop") stop = &p; if (p.strategy == "target") target_pending = true; }
  CHECK(stop != nullptr && !target_pending);
  if (stop) {
    CHECK(stop->exec_style == "taker" && stop->outcome_side == "no");
    CHECK_NEAR(stop->price, 0.70, 1e-9);  // buy NO at 1 - 0.30
    CHECK(stop->mode == "bot");
  }
  b = st.query("SELECT alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  CHECK(!b.empty() && b[0]["alerted"].get<std::string>() == "stop");
  // Alert mode never creates proposals.
  st.exec("UPDATE proposals SET status='expired'");
  st.exec("UPDATE brackets SET mode='alert', alerted='' WHERE ticker='KXT-E-B0'");
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(p.strategy != "stop" && p.strategy != "target");
  b = st.query("SELECT alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  CHECK(!b.empty() && b[0]["alerted"].get<std::string>() == "stop");  // still flagged for the screen
  // Position closed: bracket removed.
  st.replace_positions({});
  eng.cycle();
  CHECK(st.query("SELECT ticker FROM brackets", {}).empty());
  std::remove(path.c_str());
}


static void test_marks() {
  Market m;
  m.yes_bid = 0.40; m.yes_ask = 0.44; CHECK_NEAR(m.mid(), 0.42, 1e-12);
  m.yes_bid = 0; m.yes_ask = 0.01; m.last = 0.01; CHECK_NEAR(m.mid(), 0.01, 1e-12);   // dead YES, real 1c offer
  m.yes_bid = 0; m.yes_ask = 1.00; m.last = 0.01; CHECK_NEAR(m.mid(), 0.01, 1e-12);   // closed market: 0 / 1.00 is an empty book
  m.yes_bid = 0; m.yes_ask = 0; m.last = 0.99; CHECK_NEAR(m.mid(), 0.99, 1e-12);
  m.result = "no"; CHECK_NEAR(m.mark(), 0.0, 1e-12);
  m.result = "yes"; CHECK_NEAR(m.mark(), 1.0, 1e-12);
  // The Sept 6 screen: AUS 96-97 YES x2 at 0.10 in a closed market quoted 0 / 1.00 must mark near zero, not +$1.80.
  Store st(":memory:");
  Market a; a.ticker = "KXHIGHAUS-26SEP05-B96.5"; a.event_ticker = "KXHIGHAUS-26SEP05"; a.series_ticker = "KXHIGHAUS";
  a.status = "closed"; a.yes_bid = 0; a.yes_ask = 1.0; a.last = 0.01; a.close_time = "2026-09-06T06:00:00Z"; st.upsert_market(a);
  Position p; p.ticker = a.ticker; p.position = 2; p.exposure = 0.20; st.replace_positions({p});
  json pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["settling"].get<double>(), 2 * 0.01 - 0.20, 1e-9);   // trading closed: settling, not unrealized
  CHECK_NEAR(pnl["unrealized"].get<double>(), 0.0, 1e-9);
}

static void test_hedging() {
  // The 2026-09-05 LAX ladder, this time with the exchange's own mutually_exclusive flag on
  // file: the risk book stops guessing and the hedge becomes arithmetic.
  Store st(":memory:");
  Series sr{"KXHIGHLAX", "Climate and Weather", "", "quadratic", "daily", 1.0};
  st.upsert_series(sr);
  auto mk = [&](const std::string& t, const std::string& stype, double lo, double hi, double bid, double ask) {
    Market m; m.ticker = t; m.event_ticker = "KXHIGHLAX-26SEP05"; m.series_ticker = "KXHIGHLAX"; m.status = "active";
    m.strike_type = stype; if (stype != "less") m.floor_strike = lo; if (stype != "greater") m.cap_strike = hi;
    m.close_time = "2099-01-01T00:00:00Z";
    m.yes_bid = bid; m.yes_ask = ask; m.no_bid = 1 - ask; m.no_ask = 1 - bid;
    m.yes_bid_size = 200; m.yes_ask_size = 200;
    m.yes_sub_title = t.substr(t.find_last_of('-') + 1);
    st.upsert_market(m);
  };
  mk("KXHIGHLAX-26SEP05-T78", "less", 0, 79, 0.20, 0.24);     // 78 or below
  mk("KXHIGHLAX-26SEP05-B79.5", "between", 79, 80, 0.30, 0.34);
  mk("KXHIGHLAX-26SEP05-T80", "greater", 80, 0, 0.44, 0.48);  // 81 or above
  const std::string EV = "KXHIGHLAX-26SEP05";
  std::vector<Holding> held{{"KXHIGHLAX-26SEP05-B79.5", true, 18, 5.94}};
  // Without the flag the desk assumes nothing and offers nothing.
  CHECK(!event_structure(st, EV).exclusive);
  CHECK(event_payoffs(st, EV, held).empty());
  Config cfg; cfg.bankroll_dollars = 100;
  HedgePlan blind = plan_hedge(st, cfg, EV, held, std::nullopt, 5.0);
  CHECK(!blind.have_structure && blind.options.empty());
  CHECK(blind.note.find("mutually exclusive") != std::string::npos);
  // Now the exchange has confirmed it, and the strikes tile the line: three states, no "none".
  st.exec("INSERT INTO event_meta(event_ticker,title,series_ticker,mutually_exclusive,exhaustive,checked_ts) "
          "VALUES(?,?,?,1,0,?)", {EV, "LAX high", "KXHIGHLAX", now_s()});
  EventStructure es = event_structure(st, EV);
  CHECK(es.exclusive && es.exhaustive && es.tickers.size() == 3);
  CHECK(es.tickers.front() == "KXHIGHLAX-26SEP05-T78");   // ordered by strike
  auto states = event_payoffs(st, EV, held);
  CHECK(states.size() == 3);
  for (const auto& s2 : states) {
    if (s2.ticker == "KXHIGHLAX-26SEP05-B79.5") CHECK_NEAR(s2.loss, 5.94 - 18, 1e-9);  // it wins
    else CHECK_NEAR(s2.loss, 5.94, 1e-9);
  }
  CHECK_NEAR(event_risk(st, EV, held).worst_loss, 5.94, 1e-9);
  // The pair he actually put on: YES 79-80 and NO 81+. With the structure known, this is no
  // longer "the same bet twice" — the NO leg does win if it settles 78 — but it concentrates
  // everything into the 81+ state, which is the state that happened. The cap sees that number.
  Holding no_t80{"KXHIGHLAX-26SEP05-T80", false, 13, 3.90};
  CHECK(!stacks_event(st, EV, held, no_t80));
  std::vector<Holding> pair{held[0], no_t80};
  EventRisk pr = event_risk(st, EV, pair);
  CHECK_NEAR(pr.worst_loss, 9.84, 1e-9);                       // both legs lose at 81+
  CHECK(pr.worst_state == "KXHIGHLAX-26SEP05-T80=YES");
  for (const auto& s2 : event_payoffs(st, EV, pair))
    if (s2.ticker == "KXHIGHLAX-26SEP05-T78") CHECK_NEAR(s2.loss, 9.84 - 13, 1e-9);   // the NO leg pays here
  // Adding to the bracket that is already winning pays in no worst state: still refused.
  CHECK(stacks_event(st, EV, held, Holding{"KXHIGHLAX-26SEP05-B79.5", true, 5, 1.70}));
  // Buying the bracket that wins in the worst state is a hedge, and is allowed.
  Holding yes_t80{"KXHIGHLAX-26SEP05-T80", true, 10, 4.80};
  CHECK(!stacks_event(st, EV, held, yes_t80));
  // The suggester, asked to cap the loss at $3: it must pick a leg that pays in a worst state.
  HedgePlan plan = plan_hedge(st, cfg, EV, held, std::nullopt, 3.0);
  CHECK(plan.have_structure && plan.exhaustive);
  CHECK_NEAR(plan.worst_before, 5.94, 1e-9);
  CHECK(!plan.options.empty());
  const HedgeLeg& best = plan.options.front();
  CHECK(best.worst_after < plan.worst_before - 0.01);
  CHECK(best.side == "yes" || best.ticker == "KXHIGHLAX-26SEP05-B79.5");   // never "NO on a bracket we do not hold"
  // Every offered leg genuinely reduces the worst case, and its payoff table is consistent.
  for (const auto& l : plan.options) {
    CHECK(l.worst_after < l.worst_before - 0.01);
    CHECK(l.after.size() == 3);
    double worst = 0;
    for (const auto& s2 : l.after) worst = std::max(worst, s2.loss);
    CHECK_NEAR(worst, l.worst_after, 1e-9);
  }
  // A book with nothing at risk gets no suggestion.
  HedgePlan flat = plan_hedge(st, cfg, EV, {}, std::nullopt, 3.0);
  CHECK(flat.options.empty() && flat.note.find("nothing at risk") != std::string::npos);
  // A gap in the ladder is not exhaustive: the "none of them" state is carried.
  Store st2(":memory:");
  st2.upsert_series(sr);
  auto mk2 = [&](const std::string& t, const std::string& stype, double lo, double hi) {
    Market m; m.ticker = t; m.event_ticker = "E"; m.series_ticker = "KXHIGHLAX"; m.status = "active";
    m.strike_type = stype; if (stype != "less") m.floor_strike = lo; if (stype != "greater") m.cap_strike = hi;
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.30; m.yes_ask = 0.34; st2.upsert_market(m);
  };
  mk2("A", "less", 0, 79); mk2("B", "greater", 85, 0);   // 79..85 covered by nothing
  st2.exec("INSERT INTO event_meta(event_ticker,mutually_exclusive,exhaustive,checked_ts) VALUES('E',1,0,?)", {now_s()});
  CHECK(event_structure(st2, "E").exclusive && !event_structure(st2, "E").exhaustive);
  auto gap = event_payoffs(st2, "E", {Holding{"A", true, 10, 3.0}});
  CHECK(gap.size() == 3 && gap.back().ticker.empty());
  CHECK_NEAR(gap.back().loss, 3.0, 1e-9);   // the gap state loses the premium
}

static void test_ladder_arb() {
  // Three mutually exclusive brackets whose YES bids sum past a dollar: selling all three pays
  // at least two of them whatever settles, so the basket is locked before any forecast.
  Store st(":memory:");
  Series sr{"KXBTC", "Crypto", "", "quadratic", "hourly", 1.0};
  st.upsert_series(sr);
  auto mk = [&](const std::string& t2, double lo, double hi, double bid, double ask, double size) {
    Market m; m.ticker = t2; m.event_ticker = "KXBTC-26SEP0612"; m.series_ticker = "KXBTC"; m.status = "active";
    m.strike_type = "between"; m.floor_strike = lo; m.cap_strike = hi;
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = bid; m.yes_ask = ask;
    m.no_bid = 1 - ask; m.no_ask = 1 - bid; m.yes_bid_size = size; m.yes_ask_size = size;
    m.yes_sub_title = t2.substr(t2.find_last_of('-') + 1);
    st.upsert_market(m);
    return m;
  };
  std::vector<Market> ms{mk("KXBTC-26SEP0612-B1", 1, 2, 0.45, 0.48, 40),
                         mk("KXBTC-26SEP0612-B2", 2, 3, 0.42, 0.45, 25),
                         mk("KXBTC-26SEP0612-B3", 3, 4, 0.20, 0.23, 60)};
  std::vector<const Market*> ptrs{&ms[0], &ms[1], &ms[2]};
  Config cfg; cfg.bankroll_dollars = 100;
  const std::string EV = "KXBTC-26SEP0612";
  // Without the exchange's flag there is no argument, so no basket.
  CHECK(!find_ladder_arb(st, cfg, EV, ptrs, 20.0).has_value());
  st.exec("INSERT INTO event_meta(event_ticker,mutually_exclusive,exhaustive,checked_ts) VALUES(?,1,0,?)",
          {EV, now_s()});
  auto arb = find_ladder_arb(st, cfg, EV, ptrs, 20.0);
  CHECK(arb.has_value());
  CHECK(arb->kind == "sell");
  CHECK(arb->legs.size() >= 2);
  // Every leg buys NO at 1 - bid, and the locked edge is the bid sum over a dollar, less fees.
  double sum_bid = 0, fees = 0, cost = 0;
  for (const auto& l : arb->legs) {
    CHECK(l.side == "no");
    CHECK_NEAR(l.price, 1.0 - l.yes_bid, 1e-12);
    sum_bid += l.yes_bid;
    fees += l.fee_per;
    cost += l.price + l.fee_per;
  }
  CHECK_NEAR(arb->sum_prices, sum_bid, 1e-12);
  CHECK_NEAR(arb->edge_per_contract, sum_bid - 1.0 - fees, 1e-12);
  CHECK(arb->edge_per_contract > 0);
  CHECK_NEAR(arb->cost_per_contract, cost, 1e-12);
  // Size cannot exceed the thinnest displayed bid among the legs, nor the cash cap.
  double thinnest = 1e9;
  for (const auto& l : arb->legs) thinnest = std::min(thinnest, l.size_available);
  CHECK(arb->count <= thinnest + 1e-9);
  CHECK(arb->count * arb->cost_per_contract <= 20.0 + 1e-9);
  // The payoff is genuinely bounded below: worst case over the event's states is a profit.
  std::vector<Holding> book;
  for (const auto& l : arb->legs)
    book.push_back(Holding{l.ticker, false, arb->count, arb->count * (l.price + l.fee_per)});
  for (const auto& sp : event_payoffs(st, EV, book)) CHECK(sp.loss < 0);   // loss < 0 == profit
  CHECK(event_risk(st, EV, book).worst_loss <= 0.0);
  // Bids that sum under a dollar are just a market: nothing locked, nothing proposed.
  std::vector<Market> tight{mk("KXBTC-26SEP0612-B1", 1, 2, 0.30, 0.33, 40),
                            mk("KXBTC-26SEP0612-B2", 2, 3, 0.30, 0.33, 25),
                            mk("KXBTC-26SEP0612-B3", 3, 4, 0.20, 0.23, 60)};
  std::vector<const Market*> tptr{&tight[0], &tight[1], &tight[2]};
  CHECK(!find_ladder_arb(st, cfg, EV, tptr, 20.0).has_value());
  // A basket worth less than the configured minimum is noise and is skipped.
  cfg.risk.ladder_min_edge = 0.50;
  CHECK(!find_ladder_arb(st, cfg, EV, ptrs, 20.0).has_value());
}

static void test_inplay_lambda_fit() {
  // 2026-09-06, Bologna 0-1 Sassuolo at half time. The desk restarted mid-match and refitted the
  // goal rates from a book that already contained the goal; the full-match fit handed the goal to
  // the rates and fair() then applied the same goal again, reading 0.82 for the away side against
  // a market at 0.535. Fitting with the score and the time left reproduces the market instead.
  const double rho = -0.13;
  double rem = 0.5;                       // about half the match left
  int d = -1;                             // away leads by one
  double p_home = 0.175, p_draw = 0.295, p_away = 0.535;
  // The old path: fit as if the match had not started, then apply the score.
  auto wrong = fit_lambdas(p_home, p_draw, rho);
  MatchProbs bad = match_probs(wrong.first * rem, wrong.second * rem, d);
  CHECK(bad.away > 0.75);                 // the double count: 0.82 against a market at 0.535
  // The fix: fit given the state.
  auto right = fit_lambdas_inplay(p_home, p_draw, rem, d, rho);
  MatchProbs good = match_probs(right.first * rem, right.second * rem, d);
  CHECK_NEAR(good.home, p_home, 0.02);
  CHECK_NEAR(good.draw, p_draw, 0.02);
  CHECK_NEAR(good.away, p_away, 0.03);    // no invented edge at the moment of the fit
  // Rates fitted this way stay usable: another goal moves the model well past the market.
  MatchProbs after_goal = match_probs(right.first * rem, right.second * rem, -2);
  CHECK(after_goal.away > good.away + 0.10);
  // A genuine pre-match fit is unchanged by the new path (no score, whole match left).
  auto pre = fit_lambdas(0.50, 0.265, rho);
  auto pre_inplay = fit_lambdas_inplay(0.50, 0.265, 1.0, 0, rho);
  CHECK_NEAR(pre.first, pre_inplay.first, 0.06);
  CHECK_NEAR(pre.second, pre_inplay.second, 0.06);
}

static void test_dominance_arb() {
  // "84 or above" implies "80 or above" on the same day. If the book bids more for the harder
  // one than it asks for the easier one, selling the hard leg and buying the easy one pays at
  // least $1 in every state. No exclusivity flag is involved: the implication is in the strikes.
  Store st(":memory:");
  Series sr{"KXHIGHLAX", "Climate and Weather", "", "quadratic", "daily", 1.0};
  st.upsert_series(sr);
  auto mk = [&](const std::string& tk, const std::string& stype, double lo, double hi, double bid, double ask, double sz) {
    Market m; m.ticker = tk; m.event_ticker = "KXHIGHLAX-26SEP07"; m.series_ticker = "KXHIGHLAX"; m.status = "active";
    m.strike_type = stype;
    if (stype != "less") m.floor_strike = lo;
    if (stype != "greater") m.cap_strike = hi;
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = bid; m.yes_ask = ask;
    m.no_bid = 1 - ask; m.no_ask = 1 - bid; m.yes_bid_size = sz; m.yes_ask_size = sz;
    m.yes_sub_title = tk.substr(tk.find_last_of('-') + 1);
    st.upsert_market(m);
    return m;
  };
  Config cfg; cfg.bankroll_dollars = 100;
  const std::string EV = "KXHIGHLAX-26SEP07";
  {
    // T83 pays above 83; T79 pays above 79, so T83's outcomes sit inside T79's.
    std::vector<Market> ms{mk("KXHIGHLAX-26SEP07-T83", "greater", 83, 0, 0.60, 0.63, 30),
                           mk("KXHIGHLAX-26SEP07-T79", "greater", 79, 0, 0.50, 0.52, 20)};
    std::vector<const Market*> p{&ms[0], &ms[1]};
    auto arb = find_dominance_arb(st, cfg, EV, p, 20.0);
    CHECK(arb.has_value());
    CHECK(arb->kind == "dominance" && arb->legs.size() == 2);
    CHECK(arb->legs[0].ticker == "KXHIGHLAX-26SEP07-T83" && arb->legs[0].side == "no");   // sell the harder
    CHECK(arb->legs[1].ticker == "KXHIGHLAX-26SEP07-T79" && arb->legs[1].side == "yes");  // buy the easier
    CHECK_NEAR(arb->legs[0].price, 0.40, 1e-12);
    CHECK_NEAR(arb->legs[1].price, 0.52, 1e-12);
    CHECK(arb->edge_per_contract > 0);
    CHECK_NEAR(arb->edge_per_contract, 0.60 - 0.52 - arb->legs[0].fee_per - arb->legs[1].fee_per, 1e-12);
    CHECK(arb->count <= 20);                                   // the thinner of the two sizes
    CHECK(arb->count * arb->cost_per_contract <= 20.0 + 1e-9);  // and the cash cap
    // Payoff check over every settle value that matters: 78 (neither), 81 (only T79), 85 (both).
    for (int t2 : {78, 81, 85}) {
      double payout = 0;
      bool hard = t2 > 83, easy = t2 > 79;
      payout += hard ? 0.0 : 1.0;    // the NO leg on T83
      payout += easy ? 1.0 : 0.0;    // the YES leg on T79
      CHECK(payout >= 1.0 - 1e-9);
    }
  }
  {
    // Priced consistently (harder bid below easier ask): nothing to do.
    std::vector<Market> ms{mk("KXHIGHLAX-26SEP07-T83", "greater", 83, 0, 0.40, 0.43, 30),
                           mk("KXHIGHLAX-26SEP07-T79", "greater", 79, 0, 0.60, 0.62, 20)};
    std::vector<const Market*> p{&ms[0], &ms[1]};
    CHECK(!find_dominance_arb(st, cfg, EV, p, 20.0).has_value());
  }
  {
    // A range bracket sits inside a threshold market: same rule, found the same way.
    std::vector<Market> ms{mk("KXHIGHLAX-26SEP07-B84.5", "between", 84, 85, 0.55, 0.58, 25),
                           mk("KXHIGHLAX-26SEP07-T79", "greater", 79, 0, 0.45, 0.47, 25)};
    std::vector<const Market*> p{&ms[0], &ms[1]};
    auto arb = find_dominance_arb(st, cfg, EV, p, 20.0);
    CHECK(arb.has_value() && arb->legs[0].ticker == "KXHIGHLAX-26SEP07-B84.5");
    CHECK_NEAR(arb->edge_per_contract, 0.55 - 0.47 - arb->legs[0].fee_per - arb->legs[1].fee_per, 1e-12);
  }
  {
    // Disjoint brackets imply nothing about each other.
    std::vector<Market> ms{mk("KXHIGHLAX-26SEP07-B84.5", "between", 84, 85, 0.55, 0.58, 25),
                           mk("KXHIGHLAX-26SEP07-B78.5", "between", 78, 79, 0.45, 0.47, 25)};
    std::vector<const Market*> p{&ms[0], &ms[1]};
    CHECK(!find_dominance_arb(st, cfg, EV, p, 20.0).has_value());
  }
}

static void test_quotes_persist_and_age() {
  // QA: displayed sizes were never written to the markets table, so every market the engine read
  // back had size 0 and size_for() refused every taker proposal ("never lift an invisible offer").
  // Sizes now round-trip, and a market whose stored quote is older than three polls is skipped.
  {
    std::string path = "/tmp/desk_test_quotes.sqlite";
    std::remove(path.c_str());
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    seed_kxt(st, 0.10, 0.12);
    auto back = st.open_markets("KXT");
    CHECK(back.size() == 2 && back[0].yes_ask_size == 500 && back[0].yes_bid_size == 500 && back[0].quote_ts > 0);
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.30;  // vs ask 0.12: a taker candidate with real displayed size
    tsp->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    eng.cycle();
    auto props = st.proposals("pending", 10);
    bool sized = false;
    for (const auto& p : props) if (p.count > 0) sized = true;
    CHECK(sized);
    // Age the stored quotes past the gate: the next cycle withdraws and proposes nothing new.
    st.exec("UPDATE markets SET updated_ts=?", {now_s() - 3600});
    eng.cycle();
    CHECK(st.proposals("pending", 10).empty());
    std::remove(path.c_str());
  }
}

static void test_settling_leaves_caps() {
  Store st(":memory:");
  Series sr{"KXHIGHLAX", "Climate and Weather", "", "quadratic", "daily", 1.0};
  st.upsert_series(sr);
  Market live; live.ticker = "KXHIGHLAX-26SEP06-B79.5"; live.event_ticker = "KXHIGHLAX-26SEP06"; live.series_ticker = "KXHIGHLAX";
  live.status = "active"; live.close_time = "2099-01-01T00:00:00Z"; live.yes_bid = 0.3; live.yes_ask = 0.34; st.upsert_market(live);
  Market done = live; done.ticker = "KXHIGHLAX-26SEP05-B79.5"; done.event_ticker = "KXHIGHLAX-26SEP05"; done.close_time = "2026-09-01T08:00:00Z"; st.upsert_market(done);
  Position a; a.ticker = live.ticker; a.position = 5; a.exposure = 1.5;
  Position b; b.ticker = done.ticker; b.position = 18; b.exposure = 5.94;
  st.replace_positions({a, b});
  Exposure e = committed_exposure(st);
  CHECK_NEAR(e.filled, 1.5, 1e-9);            // only the live market counts toward the caps
  CHECK_NEAR(e.settling, 5.94, 1e-9);         // yesterday's closed market waits for payout
  CHECK(e.events.count("KXHIGHLAX-26SEP06") == 1 && e.events.count("KXHIGHLAX-26SEP05") == 0);
  CHECK_NEAR(e.total(), 1.5, 1e-9);
}

static void test_trading_day_and_pnl_ledger() {
  // Local day boundaries: 04:30Z on Sep 6 is still Sep 5 in Chicago (23:30 CDT) and New York (00:30 EDT is Sep 6).
  long long t = parse_iso("2026-09-06T04:30:00Z");
  CHECK(trading_day("America/Chicago", t) == "2026-09-05");
  CHECK(trading_day("America/New_York", t) == "2026-09-06");
  CHECK(trading_day("UTC", t) == "2026-09-06");
  CHECK(tz_offset_seconds("America/Chicago", t) == -5 * 3600);                          // CDT
  CHECK(tz_offset_seconds("America/Chicago", parse_iso("2026-12-06T04:30:00Z")) == -6 * 3600);  // CST
  CHECK(tz_offset_seconds("America/Phoenix", t) == -7 * 3600);                          // no DST
  CHECK(tz_offset_seconds("America/Los_Angeles", t) == -7 * 3600);                      // PDT
  // P&L: a position closed before settlement keeps its realized P&L through the ledger.
  Store st(":memory:");
  Series sr{"KXHIGHNY", "Climate and Weather", "", "quadratic", "daily", 1.0};
  st.upsert_series(sr);
  Market m; m.ticker = "KXHIGHNY-26SEP05-T79"; m.event_ticker = "KXHIGHNY-26SEP05"; m.series_ticker = "KXHIGHNY";
  m.status = "active"; m.close_time = "2026-09-06T05:00:00Z"; m.expected_expiration_time = "2026-09-06T19:00:00Z";
  m.yes_bid = 0.99; m.yes_ask = 1.0; st.upsert_market(m);
  // Kalshi's realized number for the flat market (gross), fees separate.
  st.exec("INSERT INTO realized(ticker,realized_pnl,fees_paid,updated_ts) VALUES(?,?,?,?)", {m.ticker, 9.88, 0.0091, now_s()});
  json pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["realized_closed"].get<double>(), 9.88 - 0.0091, 1e-9);
  CHECK_NEAR(pnl["total"].get<double>(), 9.88 - 0.0091, 1e-9);
  // Once a zero-count settlement record arrives it is excluded from "settled", and the ledger still counts once.
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
          {m.ticker, m.event_ticker, "yes", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, "2026-09-06T19:05:00Z"});
  pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["total"].get<double>(), 9.88 - 0.0091, 1e-9);
  // A settled market with a ledger row: Kalshi's realized number (post-settlement, gross) is the
  // single source; the settlement arithmetic is not added on top.
  Market m2 = m; m2.ticker = "KXHIGHLAX-26SEP05-B79.5"; m2.event_ticker = "KXHIGHLAX-26SEP05"; m2.series_ticker = "KXHIGHLAX"; st.upsert_market(m2);
  st.exec("INSERT INTO realized(ticker,realized_pnl,fees_paid,updated_ts) VALUES(?,?,?,?)", {m2.ticker, -3.29, 0.02, now_s()});
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
          {m2.ticker, m2.event_ticker, "no", 13.0, 0.0, 4.29, 0.0, 0.0, 0.0, "2026-09-06T19:05:00Z"});
  pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["settled_pnl"].get<double>(), -3.29 - 0.02, 1e-9);
  CHECK(pnl["n_settled"].get<int>() == 1);
  // A pre-ledger settlement (July history) still uses the settlement arithmetic.
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
          {"KXOLD-26JUL08-X", "KXOLD-26JUL08", "yes", 10.0, 0.0, 6.0, 0.0, 10.0, 0.1, "2026-07-08T19:05:00Z"});
  pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["settled_pnl"].get<double>(), -3.31 + (10.0 - 6.0 - 0.1), 1e-9);
  // Position rows say when trading has closed so the screen shows "settling" instead of a close button.
  Position pos; pos.ticker = m2.ticker; pos.position = 13; pos.exposure = 4.29; st.replace_positions({pos});
  pnl = build_pnl(st, "America/Chicago");
  CHECK(pnl["positions"].size() == 1 && pnl["positions"][0]["trading_closed"].get<bool>() == true);
}

static void test_order_integration() {
  std::string key="/tmp/desk_integration_key.pem";
  CHECK(std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "+key+" 2>/dev/null").c_str())==0);
  auto signer=std::make_shared<Signer>("test",key);
  httplib::Server server;
  std::atomic<int> writes{0}; json sent, canceled;
  std::atomic<bool> funded{false}, held{false};
  std::atomic<bool> fill_between_reads{false}, race_filled{false}, reject_close{false};
  std::atomic<int> reads{0}, order_read{0}, position_read{0};
  json m={{"ticker","KXETH-26SEP05-B100"},{"event_ticker","KXETH-26SEP05"},{"status","active"},
    {"exchange_index",2},{"close_time","2099-01-01T00:00:00Z"},{"yes_bid_dollars","0.7200"},{"yes_ask_dollars","0.7400"}};
  server.Get("/trade-api/v2/markets/KXETH-26SEP05-B100",[&](const httplib::Request&,httplib::Response& r){r.set_content(json{{"market",m}}.dump(),"application/json");});
  server.Get("/trade-api/v2/series/KXETH",[&](const httplib::Request&,httplib::Response& r){r.set_content(R"({"series":{"ticker":"KXETH","category":"Crypto","fee_type":"quadratic","fee_multiplier":1}})","application/json");});
  server.Get("/trade-api/v2/portfolio/balance",[&](const httplib::Request& q,httplib::Response& r){
    json b={{"balance_dollars",q.has_param("exchange_index")&&!funded?"0.0000":"100.0000"},{"portfolio_value",0},{"updated_ts",now_s()-3600}}; // unchanged balance timestamp must not block fresh reads
    r.set_content(b.dump(),"application/json");
  });
  server.Get("/trade-api/v2/portfolio/positions",[&](const httplib::Request&,httplib::Response& r){
    position_read=++reads;
    json a=json::array();
    if(fill_between_reads) {
      if(race_filled) a.push_back({{"ticker",m["ticker"]},{"position_fp","10.00"},{"market_exposure_dollars","8.0000"}});
      race_filled=true; // the resting order fills immediately after the first portfolio read
    } else if(held) a.push_back({{"ticker",m["ticker"]},{"position_fp","12.94"},{"market_exposure_dollars","2.9762"}});
    r.set_content(json{{"market_positions",a}}.dump(),"application/json");
  });
  server.Get("/trade-api/v2/portfolio/orders",[&](const httplib::Request&,httplib::Response& r){
    order_read=++reads;json a=json::array();
    if(fill_between_reads) {
      if(!race_filled) a.push_back({{"order_id","race-order"},{"ticker",m["ticker"]},{"status","resting"},
        {"outcome_side","yes"},{"yes_price_dollars","0.8000"},{"no_price_dollars","0.2000"},
        {"initial_count_fp","10.00"},{"remaining_count_fp","10.00"},{"fill_count_fp","0.00"}});
      race_filled=true;
    }
    r.set_content(json{{"orders",a}}.dump(),"application/json");
  });
  server.Get("/trade-api/v2/exchange/user_data_timestamp",[&](const httplib::Request&,httplib::Response& r){r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})","application/json");});
  server.Post("/trade-api/v2/portfolio/events/orders",[&](const httplib::Request& q,httplib::Response& r){
    ++writes;sent=json::parse(q.body);
    if(reject_close && sent.value("reduce_only",false)) {
      r.status=400;r.set_content(R"({"error":{"code":"invalid_order","message":"close rejected by test exchange"}})","application/json");return;
    }
    r.status=201;
    r.set_content(json{{"order_id","test-order"},{"fill_count",sent["count"]},{"remaining_count","0.00"}}.dump(),"application/json");
  });
  server.Delete("/trade-api/v2/portfolio/events/orders/test-order",[&](const httplib::Request& q,httplib::Response& r){
    canceled={{"ticker",q.get_param_value("market_ticker")},{"exchange",q.get_param_value("exchange_index")}};
    r.set_content(R"({"order_id":"test-order","reduced_by":"0.00"})","application/json");
  });
  int port=server.bind_to_any_port("127.0.0.1");CHECK(port>0);
  std::thread thread([&]{server.listen_after_bind();});
  try {
    Store st(":memory:");Config cfg;KalshiClient client("http://127.0.0.1:"+std::to_string(port)+"/trade-api/v2",signer,"tests");Executor ex(cfg,st,client);
    OrderRequest req;req.ticker=m["ticker"];req.price=.2;req.count=1;req.post_only=true;
    try{ex.ticket(req,"test");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("shard 2")!=std::string::npos);}
    CHECK(writes==0);CHECK(order_read<position_read);
    funded=true;req.count=100;
    try{ex.ticket(req,"over cap");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("Event cap")!=std::string::npos);}
    CHECK(writes==0);
    // A fill between the two reads must not disappear from both positions and orders.
    fill_between_reads=true;race_filled=false;req.count=15;
    try{ex.ticket(req,"fill during refresh");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("Event cap")!=std::string::npos);}
    CHECK(writes==0);CHECK(committed_exposure(st).total()>=8.0);
    fill_between_reads=false;
    req.count=1;ex.ticket(req,"valid entry");CHECK(writes==1);CHECK(sent["exchange_index"]==2);
    st.exec("DELETE FROM kv WHERE key='last_write_ts'");
    held=true;ex.close_position(m["ticker"]);CHECK(writes==2);CHECK(sent["count"]=="12.94");
    CHECK(sent["reduce_only"]==true);CHECK(sent["side"]=="ask");CHECK(sent["price"]=="0.7200");CHECK(sent["exchange_index"]==2);
    ex.cancel("test-order");CHECK(canceled["ticker"]==m["ticker"]);CHECK(canceled["exchange"]=="-1");
    st.exec("DELETE FROM kv WHERE key='last_write_ts'");
    reject_close=true;
    try{ex.close_position(m["ticker"]);CHECK(false);}catch(const ApiError& e){CHECK(e.status==400);}
    CHECK(writes==3);
    bool failed_close=false;
    for(const auto& p:st.proposals("failed",100)) if(p.strategy=="close") failed_close=true;
    CHECK(failed_close);
    for(const auto& p:st.proposals("pending",100)) CHECK(p.strategy!="close");
    // A stale close Signal must never become an unrestricted entry when approved.
    st.exec("DELETE FROM kv WHERE key='last_write_ts'");
    reject_close=false;
    Proposal retry;retry.ticker=m["ticker"];retry.strategy="close";retry.status="pending";
    retry.outcome_side="no";retry.exec_style="taker";retry.price=.28;retry.count=1;retry.updated_ts=now_s();
    try{ex.submit(retry);CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("close")!=std::string::npos);}
    CHECK(writes==3);
  }catch(const std::exception& e){std::cerr<<"Integration error: "<<e.what()<<"\n";CHECK(false);}
  server.stop();thread.join();std::remove(key.c_str());
}

int main() {
  test_order_integration();
  test_research_gates();
  test_event_netting();
  test_sports_live();
  test_exits_and_spread_rule();
  test_brackets();
  test_trading_day_and_pnl_ledger();
  test_marks();
  test_quotes_persist_and_age();
  test_hedging();
  test_ladder_arb();
  test_dominance_arb();
  test_inplay_lambda_fit();
  test_settling_leaves_caps();
  test_risk_regressions();
  test_event_cap();
  test_sports();
  test_auto_rules();
  test_instant_lane();
  test_fees();
  test_fixed();
  test_time();
  test_signing();
  test_order_body();
  test_parse_market();
  test_store();
  test_engine_candidates();
  test_weather_math();
  test_bootstrap();
  std::printf("%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
