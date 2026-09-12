// Minimal self-contained tests: fees, sizing, fixed-point, signing, store round trip,
// order body mapping, engine candidates, weather bracket math, time parsing.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <set>
#include <map>
#include <chrono>
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
#include "desk/universe.h"
#include "desk/record.h"
#include "desk/wxdist.h"
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

// ---------------------------------------------------------------------------------------------
// Match phase and the restart that poisoned the rates on 2026-09-06.
//
// prepare() used to fit scoring rates in its second section and only ingest ESPN state in its
// fourth. On the first cycle after any restart the state map was therefore empty, the fit read
// that emptiness as "before kick-off", and calibrated a full-match model to a book that already
// contained a 1-2 scoreline. fair() then applied the same goals again through the score term.
// Marseille-Paris printed 0.04 for a home side the market held at 0.205, and the contaminated
// rates were written to disk tagged "pre", so they survived the next restart too.
static std::string fixture_path(const char* rel) {
  const char* roots[] = {DESK_SOURCE_DIR "/", "", "../"};
  for (const char* r : roots) {
    std::string p = std::string(r) + rel;
    std::ifstream f(p);
    if (f.good()) return p;
  }
  return rel;
}

static void test_match_phase() {
  // --- mapping, against provider fixtures captured live on 2026-09-06 ------------------------
  std::ifstream fx(fixture_path("tests/fixtures/espn/status_shapes.json"));
  CHECK(fx.good());
  json doc = json::parse(fx, nullptr, false);
  CHECK(doc.is_object() && doc.contains("events"));
  int halftime = 0, second = 0, full = 0, pre = 0;
  for (const auto& e : doc["events"]) {
    const auto& stat = e["status"];
    std::string state = stat["type"].value("state", "");
    std::string name = stat["type"].value("name", "");
    int period = stat.contains("period") && stat["period"].is_number() ? stat["period"].get<int>() : 0;
    MatchPhase p = phase_from_espn(state, name, period);
    if (name == "STATUS_HALFTIME") {
      ++halftime;
      // The whole reason phase exists. Halftime arrives as state="in" with period=1, so the
      // coarse state would have called this open play and the period would have called it the
      // first half. Only type.name distinguishes it.
      CHECK(state == "in");
      CHECK(period == 1);
      CHECK(p == MatchPhase::Halftime);
      CHECK(phase_started(p));
      CHECK(!phase_in_play(p));
      CHECK_NEAR(SportsInplay::parse_clock(stat.value("displayClock", ""), period, name), 45, 1e-9);
    } else if (name == "STATUS_SECOND_HALF") {
      ++second;
      CHECK(p == MatchPhase::SecondHalf);
      CHECK(phase_started(p) && phase_in_play(p));
    } else if (name == "STATUS_FULL_TIME") {
      ++full;
      CHECK(p == MatchPhase::Finished);
      CHECK(phase_started(p) && !phase_in_play(p));
    } else if (name == "STATUS_SCHEDULED") {
      ++pre;
      CHECK(p == MatchPhase::Prematch);
      CHECK(!phase_started(p) && !phase_in_play(p));
    }
  }
  CHECK(halftime == 1 && second == 1 && full == 1 && pre == 1);

  // --- names this desk must handle that today's capture did not contain ----------------------
  // From ESPN's soccer status vocabulary rather than from a fixture: these assert our mapping,
  // not the provider's behaviour.
  CHECK(phase_from_espn("in", "STATUS_FIRST_HALF", 1) == MatchPhase::FirstHalf);
  CHECK(phase_from_espn("in", "STATUS_END_OF_PERIOD", 1) == MatchPhase::Halftime);
  CHECK(phase_from_espn("in", "STATUS_END_OF_PERIOD", 2) == MatchPhase::Stoppage);
  CHECK(phase_from_espn("in", "STATUS_END_OF_EXTRATIME", 2) == MatchPhase::Stoppage);
  CHECK(phase_from_espn("in", "STATUS_SHOOTOUT", 2) == MatchPhase::Stoppage);
  CHECK(phase_from_espn("post", "STATUS_ABANDONED", 2) == MatchPhase::Suspended);
  CHECK(phase_from_espn("pre", "STATUS_POSTPONED", 0) == MatchPhase::Suspended);
  CHECK(phase_from_espn("pre", "STATUS_DELAYED", 0) == MatchPhase::Suspended);
  CHECK(phase_from_espn("post", "STATUS_FINAL", 2) == MatchPhase::Finished);

  // --- the invariant: absence is never kick-off ----------------------------------------------
  CHECK(phase_from_espn("", "", 0) == MatchPhase::Unknown);
  CHECK(phase_from_espn("", "STATUS_SOMETHING_ESPN_ADDS_LATER", 0) == MatchPhase::Unknown);
  CHECK(!phase_started(MatchPhase::Unknown));
  CHECK(!phase_in_play(MatchPhase::Unknown));
  CHECK(phase_from_espn("in", "", 0) == MatchPhase::FirstHalf);   // coarse state still works alone
  CHECK(phase_from_espn("in", "", 2) == MatchPhase::SecondHalf);
  for (MatchPhase p : {MatchPhase::Unknown, MatchPhase::Prematch, MatchPhase::FirstHalf,
                       MatchPhase::Halftime, MatchPhase::SecondHalf, MatchPhase::Stoppage,
                       MatchPhase::Finished, MatchPhase::Suspended})
    CHECK(phase_parse(phase_name(p)) == p);

  // --- the numerical failure itself ----------------------------------------------------------
  // The Marseille-Paris book at half time: home 0.205, draw 0.285, away 0.515, with the away side
  // already two goals to one up and half the match left.
  double ph = 0.205, pd = 0.285, rem = 0.5;
  auto good = fit_lambdas_inplay(ph, pd, rem, -1, -0.13);
  MatchProbs chk = match_probs(good.first * rem, good.second * rem, -1);
  // Conditioning on the score reproduces the book. That is the success condition for an in-play
  // fit, and precisely why such a fit carries no edge at its own calibration point.
  CHECK(std::fabs(chk.home - ph) < 0.02);
  CHECK(std::fabs(chk.draw - pd) < 0.02);
  // Fitting the full-match model to the same book does not, because it hands the goals already in
  // the prices to the scoring rates and then counts them again.
  auto bad = fit_lambdas(ph, pd, -0.13);
  MatchProbs wrong = match_probs(bad.first * rem, bad.second * rem, -1);
  CHECK(std::fabs(wrong.home - ph) > 0.10);
}

static void test_inplay_restart() {
  std::string path = "/tmp/desk_inplay_restart.sqlite";
  std::remove(path.c_str());
  Store st(path);

  // An event dated today so prepare()'s +-36h window accepts it.
  static const char* MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  std::string iso = now_iso();
  std::string ev = "KXTESTGAME-" + iso.substr(2, 2) + MON[std::atoi(iso.substr(5, 2).c_str()) - 1] +
                   iso.substr(8, 2) + "AAABBB";

  // The Marseille-Paris book: the away side is a heavy favourite because it is already 2-1 up.
  auto mk = [&](const char* suffix, const std::string& rules, double bid, double ask) {
    Market m;
    m.ticker = ev + "-" + suffix;
    m.event_ticker = ev;
    m.series_ticker = "KXTESTGAME";
    m.status = "active";
    m.rules_primary = rules;
    m.yes_bid = bid;
    m.yes_ask = ask;
    return m;
  };
  std::string fixture = " the Alpha FC vs Beta FC professional test soccer game";
  std::vector<Market> markets{mk("AAA", "If Alpha FC wins the" + fixture.substr(4), 0.20, 0.21),
                              mk("TIE", "If Tie is the result of" + fixture, 0.28, 0.29),
                              mk("BBB", "If Beta FC wins the" + fixture.substr(4), 0.51, 0.52)};

  // A league slug the provider does not serve, so no scoreboard can arrive. This is the restart
  // case and the feed-outage case at once: markets are visible, match state is not.
  json params;
  params["leagues"] = json::object();
  params["leagues"]["KXTESTGAME"] = "zzz.nonexistent.league";

  {
    SportsInplay s(params, "test");
    s.prepare(markets, st);
    // No state means no phase, and no phase means no calibration. Before this change the desk
    // fitted a full-match model here and wrote it to disk tagged "pre".
    CHECK(!st.kv_get("lam:" + ev).has_value());
    CHECK(st.query("SELECT id FROM rate_fits").empty());
    for (const auto& m : markets) CHECK(!s.fair(m, Series{}, st).has_value());
  }

  // Now the restart proper: a phase and score persisted by the previous process, second half,
  // 1-2, and still no reachable provider.
  st.exec(
      "INSERT INTO match_state(event_ticker,espn_id,phase,phase_ts,home_score,away_score,minute,"
      "period,red_home,red_away,status_name,state,data_ts,updated_ts) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {ev, std::string("401876471"), std::string("second_half"), now_s() - 600, 1LL, 2LL, 48.0, 2LL,
       0LL, 0LL, std::string("STATUS_SECOND_HALF"), std::string("in"), now_s() - 600, now_s() - 600});
  {
    SportsInplay s(params, "test");
    s.prepare(markets, st);
    // The restored phase proves the match is under way, which keeps the pre-match branch shut.
    // It is not first-hand state, so it cannot authorise an in-play calibration either.
    CHECK(!st.kv_get("lam:" + ev).has_value());
    CHECK(st.query("SELECT id FROM rate_fits").empty());
    // And nothing may be quoted off a state restored from disk.
    for (const auto& m : markets) CHECK(!s.fair(m, Series{}, st).has_value());
  }

  // A finished match must not be revived by a stale row either.
  st.exec("UPDATE match_state SET phase='finished', status_name='STATUS_FULL_TIME' WHERE event_ticker=?", {ev});
  {
    SportsInplay s(params, "test");
    s.prepare(markets, st);
    CHECK(!st.kv_get("lam:" + ev).has_value());
    CHECK(st.query("SELECT id FROM rate_fits").empty());
  }
}

// ---------------------------------------------------------------------------------------------
// The tradable universe. The property that matters is that every boundary is judged in the
// contract's own zone: New York and Los Angeles contracts change day three hours apart, and a
// universe that rolls on one clock serves stale selections for those three hours.
static void test_universe() {
  // --- reading the settlement out of Kalshi's own words ---------------------------------------
  std::string src, prod;
  CHECK(parse_settlement(
      "If the maximum temperature recorded at Chicago (CLIMDW) for Sep 6, 2026, is greater than "
      "82\xC2\xB0 fahrenheit according to The Weather Company, then the market resolves to Yes.",
      &src, &prod));
  CHECK(src == "The Weather Company");
  CHECK(prod == "CLIMDW");
  // The older wording, still present in the local database. Its clause ends after a bracket, so
  // the parser must stop at the comma rather than at the first ')'.
  CHECK(parse_settlement(
      "If the highest temperature recorded at Chicago Midway, IL for July 09, 2026, is between "
      "86-87\xC2\xB0 according to the National Weather Service's Climatological Report (Daily), "
      "then the market resolves to Yes.",
      &src, &prod));
  CHECK(src == "National Weather Service's Climatological Report (Daily)");
  CHECK(prod.empty());   // the old text names no product code
  CHECK(!parse_settlement("If the Dallas Cowboys win, then the market resolves to Yes.", &src, &prod));

  // --- the settlement map ---------------------------------------------------------------------
  CHECK(settlement_for("KXHIGHNY") && std::string(settlement_for("KXHIGHNY")->product) == "CLINYC");
  CHECK(settlement_for("KXHIGHNY")->sparse);          // Central Park, ~38 reports a day
  CHECK(settlement_for("KXHIGHAUS") && !settlement_for("KXHIGHAUS")->sparse);
  CHECK(settlement_for("KXLOWTMIN") && settlement_for("KXLOWTMIN")->low);
  CHECK(settlement_for("KXHIGHHOU") && std::string(settlement_for("KXHIGHHOU")->product).empty());
  CHECK(settlement_for("KXHIGHTNYC") == nullptr);     // Kalshi returns 404 for this series
  for (const auto& s : settlement_specs()) CHECK(std::string(s.tz).find('/') != std::string::npos);

  // --- discovered settlement mappings (research/wx_discover.py -> config/weather_discovered.json)
  // The safety property is that discovery can only ADD cities. If a discovered entry could
  // re-point a hand-read mapping, a bad parse of one contract's rules would silently redirect the
  // desk's idea of what an existing city settles on, and the gate would still say "verified".
  {
    CHECK(settlement_for("KXWXDISCOVERTEST") == nullptr);
    const json good = {{"KXWXDISCOVERTEST", {{"kind", "weather_cli"}, {"product", "CLIZZZ"},
                                             {"station", "KZZZ"}, {"tz", "America/Chicago"},
                                             {"sparse", true}, {"low", true}}}};
    CHECK(add_settlement_specs(good) == 1);
    const SettlementSpec* d = settlement_for("KXWXDISCOVERTEST");
    CHECK(d && std::string(d->product) == "CLIZZZ" && std::string(d->station) == "KZZZ");
    CHECK(d->sparse && d->low);
    // The strings must be owned here, not borrowed from the json that has since gone away.
    CHECK(std::string(d->tz) == "America/Chicago");

    // Additive only: a series already in the compiled table is never replaced.
    const json hijack = {{"KXHIGHNY", {{"kind", "weather_cli"}, {"product", "CLIWRONG"},
                                       {"station", "KWRONG"}, {"tz", "America/Chicago"}}}};
    CHECK(add_settlement_specs(hijack) == 0);
    CHECK(std::string(settlement_for("KXHIGHNY")->product) == "CLINYC");

    // An incomplete entry is rejected outright rather than stored with an empty product, because
    // an empty product is exactly what settlement_verified_for() reads as "rules unread".
    const json partial = {{"KXWXPARTIAL", {{"kind", "weather_cli"}, {"product", "CLIAAA"}}}};
    CHECK(add_settlement_specs(partial) == 0);
    CHECK(settlement_for("KXWXPARTIAL") == nullptr);
    const json empty_product = {{"KXWXEMPTY", {{"kind", "weather_cli"}, {"product", ""},
                                               {"station", "KAAA"}, {"tz", "America/Chicago"}}}};
    CHECK(add_settlement_specs(empty_product) == 0);
    CHECK(settlement_for("KXWXEMPTY") == nullptr);
    CHECK(add_settlement_specs(json::array()) == 0);
    CHECK(add_settlement_specs(json("not an object")) == 0);

    // A discovered city must be able to clear the settlement gate, or discovery is decoration.
    Market m{};
    m.ticker = "KXWXDISCOVERTEST-26SEP13-B70.5";
    m.series_ticker = "KXWXDISCOVERTEST";
    m.rules_primary = "If the minimum temperature recorded at Nowhere (CLIZZZ) for Sep 13, 2026 "
                      "is between 70-71 degrees according to the National Weather Service, then "
                      "the market resolves to Yes.";
    CHECK(settlement_verified_for("weather_nws", m));
  }

  // --- The Weather Company as a settlement authority --------------------------------------
  // Kalshi moved these to TWC on 2026-08-27. Rejecting every TWC mention kept 111 of 111 active
  // weather markets research-only; accepting TWC blindly would verify a contract whose station
  // had quietly changed. The rule is that TWC is fine so long as the contract still names the
  // climate product the desk has mapped for that series.
  {
    Market twc{};
    twc.series_ticker = "KXHIGHCHI";
    twc.rules_primary = "If the maximum temperature recorded at Chicago (CLIMDW) for Sep 13, 2026, "
                        "is between 82-83 degrees fahrenheit according to The Weather Company, "
                        "then the market resolves to Yes.";
    CHECK(weather_source_supported(twc));
    CHECK(settlement_verified_for("weather_nws", twc));

    // Same authority, a product this series is not mapped to: the desk would be modelling one
    // station and settling on another, so this must NOT verify.
    Market moved = twc;
    moved.rules_primary = "If the maximum temperature recorded at Chicago (CLIORD) for Sep 13, "
                          "2026, is between 82-83 degrees according to The Weather Company, then "
                          "the market resolves to Yes.";
    CHECK(!weather_source_supported(moved));
    CHECK(!settlement_verified_for("weather_nws", moved));

    // TWC with no product named at all: nothing ties the contract to a station we can read.
    Market bare = twc;
    bare.rules_primary = "If the maximum temperature in Chicago for Sep 13, 2026 is between 82-83 "
                         "degrees according to The Weather Company, then the market resolves to Yes.";
    CHECK(!weather_source_supported(bare));

    // A series with no mapping cannot be rescued by naming any product.
    Market unmapped = twc;
    unmapped.series_ticker = "KXHIGHNOWHERE";
    CHECK(!weather_source_supported(unmapped));

    // A series mapped with an EMPTY product stays unverified however the rules read: an empty
    // product is the desk's own record that nobody read that series' rules.
    Market unread{};
    unread.series_ticker = "KXHIGHHOU";   // compiled table carries product ""
    unread.rules_primary = "If the maximum temperature recorded at Houston (CLIHOU) for Sep 13, "
                           "2026 is above 95 according to The Weather Company, resolves Yes.";
    CHECK(settlement_for("KXHIGHHOU") && std::string(settlement_for("KXHIGHHOU")->product).empty());
    CHECK(!weather_source_supported(unread));

    // The pre-August wording still verifies, so nothing that worked before stops working.
    Market old_form{};
    old_form.series_ticker = "KXHIGHCHI";
    old_form.rules_primary = "If the highest temperature recorded at Chicago Midway, IL for July "
                             "09, 2026 is between 86-87 according to the National Weather "
                             "Service's Climatological Report (Daily), resolves Yes.";
    CHECK(weather_source_supported(old_form));
  }

  // --- rollover, the same instant seen from three zones ----------------------------------------
  // 2026-09-07T04:30Z is 00:30 in New York, 23:30 the previous day in Chicago, 21:30 in Los
  // Angeles. One contract date therefore sits in a different phase in each.
  long long t = parse_iso("2026-09-07T04:30:00Z");
  auto phase = [&](const char* date, const char* tz, long long close, long long now) {
    ContractTimes ct{date, tz, close, 0, "active", ""};
    return classify_contract(ct, now);
  };
  long long ny_close = parse_iso("2026-09-07T05:00:00Z");
  CHECK(phase("2026-09-06", "America/New_York", ny_close, t) == ContractPhase::DayDone);
  CHECK(phase("2026-09-06", "America/Chicago", parse_iso("2026-09-07T06:00:00Z"), t) == ContractPhase::Today);
  CHECK(phase("2026-09-06", "America/Los_Angeles", parse_iso("2026-09-07T08:00:00Z"), t) == ContractPhase::Today);
  CHECK(phase("2026-09-07", "America/New_York", 0, t) == ContractPhase::Today);
  CHECK(phase("2026-09-07", "America/Chicago", 0, t) == ContractPhase::Tomorrow);
  CHECK(phase("2026-09-07", "America/Los_Angeles", 0, t) == ContractPhase::Tomorrow);
  CHECK(phase("2026-09-09", "America/New_York", 0, t) == ContractPhase::Future);

  // Trading stops an hour after local midnight, so the day-done window is real and bounded.
  CHECK(phase("2026-09-06", "America/New_York", ny_close, ny_close - 1) == ContractPhase::DayDone);
  CHECK(phase("2026-09-06", "America/New_York", ny_close, ny_close) == ContractPhase::AwaitingSettlement);

  // --- nothing may be silently assumed ---------------------------------------------------------
  CHECK(phase("2026-09-06", "", 0, t) == ContractPhase::Unknown);   // no zone: no opinion
  CHECK(phase("", "America/New_York", 0, t) == ContractPhase::Unknown);
  ContractTimes done{"2026-09-06", "America/New_York", ny_close, 0, "finalized", "yes"};
  CHECK(classify_contract(done, t) == ContractPhase::Settled);
  ContractTimes closed{"2026-09-06", "America/New_York", 0, 0, "closed", ""};
  CHECK(classify_contract(closed, t) == ContractPhase::AwaitingSettlement);
  for (ContractPhase p : {ContractPhase::Unknown, ContractPhase::Future, ContractPhase::Tomorrow,
                          ContractPhase::Today, ContractPhase::DayDone,
                          ContractPhase::AwaitingSettlement, ContractPhase::Settled})
    CHECK(phase_from_label(phase_label(p)) == p);

  // Soccer carries its close three days after kick-off as a backstop and its expected expiration
  // hours after it, so the classifier must take the earlier of the two or a finished match sits
  // in the universe as tradable for days.
  ContractTimes soc{"2026-09-20", "America/Sao_Paulo", parse_iso("2026-09-23T04:30:00Z"),
                    parse_iso("2026-09-21T01:30:00Z"), "active", ""};
  CHECK(classify_contract(soc, parse_iso("2026-09-20T18:00:00Z")) == ContractPhase::Today);
  CHECK(classify_contract(soc, parse_iso("2026-09-21T02:00:00Z")) == ContractPhase::AwaitingSettlement);
  CHECK(settlement_for("KXBRASILEIROGAME") &&
        std::string(settlement_for("KXBRASILEIROGAME")->kind) == "soccer_90");
  CHECK(std::string(settlement_for("KXHIGHNY")->kind) == "weather_cli");

  // --- build and store --------------------------------------------------------------------------
  std::string path = "/tmp/desk_universe.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Market m;
  m.ticker = "KXHIGHNY-26SEP06-T80";
  m.event_ticker = "KXHIGHNY-26SEP06";
  m.series_ticker = "KXHIGHNY";
  m.status = "active";
  m.close_time = "2026-09-07T05:00:00Z";
  m.expected_expiration_time = "2026-09-07T19:00:00Z";
  m.rules_primary = "If the maximum temperature recorded at New York City (CLINYC) for Sep 6, 2026, "
                    "is greater than 80 fahrenheit according to The Weather Company, then the "
                    "market resolves to Yes.";
  Market unknown_series = m;
  unknown_series.ticker = "KXWHATEVER-26SEP06-T1";
  unknown_series.event_ticker = "KXWHATEVER-26SEP06";
  unknown_series.series_ticker = "KXWHATEVER";
  unknown_series.rules_primary = "If something happens, then the market resolves to Yes.";
  auto rows = build_universe({m, unknown_series}, t);
  CHECK(rows.size() == 2);
  CHECK(rows[0].tz == "America/New_York" && rows[0].settle_product == "CLINYC");
  CHECK(rows[0].settle_source == "The Weather Company");
  CHECK(rows[0].local_date == "2026-09-06" && rows[0].phase == ContractPhase::DayDone);
  // A series with no settlement entry is kept and marked, not dropped: the gap is the finding.
  CHECK(rows[1].tz.empty() && rows[1].settle_product.empty());
  CHECK(rows[1].phase == ContractPhase::Unknown);
  save_universe(st, rows, t);
  auto back = st.query("SELECT phase,settle_product FROM contract_universe WHERE ticker=?", {m.ticker});
  CHECK(back.size() == 1 && back[0].value("phase", "") == "day_done");
  CHECK(back[0].value("settle_product", "") == "CLINYC");
  // Re-running discovery updates in place rather than accumulating duplicates.
  rows[0].phase = ContractPhase::AwaitingSettlement;
  save_universe(st, rows, t + 60);
  CHECK(st.query("SELECT ticker FROM contract_universe").size() == 2);
  CHECK(st.query("SELECT phase FROM contract_universe WHERE ticker=?", {m.ticker})[0]
            .value("phase", "") == "awaiting_settlement");
}

// ---------------------------------------------------------------------------------------------
// Recording and replay. The invariant under test is that a replay cannot read anything it could
// not have read live, and that the ways a backtest usually cheats are all closed.
static void test_replay() {
  std::string path = "/tmp/desk_replay.sqlite";
  std::remove(path.c_str());
  Store st(path);
  long long noon = parse_iso("2026-09-06T12:00:00Z");

  // A forecast labelled with the 12Z cycle that did not actually land until 13:40Z. This is the
  // trap the whole module exists for: the cycle label is not availability, and a study that reads
  // by init_ts hands itself an hour and forty minutes of the future.
  Observation fc;
  fc.source = "noaa.nbm";
  fc.subject = "KAUS";
  fc.kind = "forecast";
  fc.valid_ts = parse_iso("2026-09-06T21:00:00Z");   // the hour it forecasts
  fc.init_ts = noon;                                  // the 12Z run
  fc.published_ts = parse_iso("2026-09-06T13:35:00Z");
  fc.received_ts = parse_iso("2026-09-06T13:40:00Z");
  fc.payload = "{\"t\":96}";
  record_observation(st, fc);

  // At 12:30 the run exists in name only.
  ReplayCursor early(st, parse_iso("2026-09-06T12:30:00Z"));
  CHECK(early.latest("noaa.nbm", "KAUS").empty());
  CHECK(early.withheld() == 1);   // it was offered the future and refused it

  // At 13:45 it has arrived, and it still knows which cycle it came from.
  ReplayCursor later(st, parse_iso("2026-09-06T13:45:00Z"));
  auto got = later.latest("noaa.nbm", "KAUS");
  CHECK(got.size() == 1);
  CHECK(got[0].init_ts == noon);
  CHECK(got[0].received_ts > got[0].init_ts);
  CHECK(got[0].valid_ts > got[0].received_ts);   // it forecasts a time still in the future
  CHECK(later.withheld() == 0);

  // Two observations of the same station, one late. Newest-arrived first, and only the arrived.
  for (int i = 0; i < 3; ++i) {
    Observation o;
    o.source = "nws.observations";
    o.subject = "KNYC";
    o.kind = "observation";
    o.valid_ts = noon + i * 3600;
    o.published_ts = noon + i * 3600 + 120;
    o.received_ts = noon + i * 3600 + 180;
    o.payload = "{\"t\":" + std::to_string(70 + i) + "}";
    record_observation(st, o);
  }
  ReplayCursor mid(st, noon + 3600 + 200);       // after the second, before the third
  auto obs = mid.latest("nws.observations", "KNYC", 5);
  CHECK(obs.size() == 2);
  CHECK(obs[0].valid_ts == noon + 3600);          // newest first
  CHECK(mid.withheld() == 1);                     // the third observation, not yet received

  // Unknown publication time is not zero lag. Under the published basis such a row is withheld;
  // under the received basis it is fine, because receipt is recorded.
  Observation nolag;
  nolag.source = "espn.scoreboard";
  nolag.subject = "KXBRASILEIROGAME-26SEP20CAPBAH";
  nolag.kind = "state";
  nolag.valid_ts = noon;
  nolag.published_ts = 0;                         // provider does not say
  nolag.received_ts = noon + 30;
  nolag.payload = "{\"minute\":45}";
  record_observation(st, nolag);
  ReplayCursor by_pub(st, noon + 3600, Arrival::Published);
  CHECK(by_pub.latest("espn.scoreboard", nolag.subject).empty());
  CHECK(by_pub.withheld() == 1);
  ReplayCursor by_recv(st, noon + 3600, Arrival::Received);
  CHECK(by_recv.latest("espn.scoreboard", nolag.subject).size() == 1);
  CHECK(std::string(arrival_label(Arrival::Published)) == "published");
  CHECK(std::string(study_label(StudyKind::PredictionOnly)) == "prediction_only");

  // The clock only runs forwards. A study that rewinds has already seen what comes next.
  ReplayCursor walk(st, noon);
  walk.advance_to(noon + 60);
  bool threw = false;
  try { walk.advance_to(noon); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  CHECK(walk.as_of() == noon + 60);
  threw = false;
  try { ReplayCursor bad(st, 0); } catch (const std::exception&) { threw = true; }
  CHECK(threw);

  // Determinism: the same cursor time yields the same answer, whatever was recorded afterwards.
  Observation future_obs = fc;
  future_obs.received_ts = parse_iso("2026-09-07T00:00:00Z");
  future_obs.payload = "{\"t\":999}";
  record_observation(st, future_obs);
  ReplayCursor again(st, parse_iso("2026-09-06T13:45:00Z"));
  auto repeat = again.latest("noaa.nbm", "KAUS");
  CHECK(repeat.size() == 1 && repeat[0].payload == "{\"t\":96}");
}

// ---------------------------------------------------------------------------------------------
// Cash reconciliation. A desk that cannot explain its own balance cannot trust its daily stop,
// and this one could not: it counted every fill as a purchase, so closing a position looked like
// spending money. On 2026-09-06 that left $46 of the day unexplained.
static void test_cash_reconciliation() {
  std::string path = "/tmp/desk_cash.sqlite";
  std::remove(path.c_str());
  Store st(path);
  long long t0 = parse_iso("2026-09-06T12:00:00Z");

  // Open 7 NO at 0.79, then buy 7 YES at 0.21 on the same ticker. Kalshi nets the matched pair
  // and returns $1 a contract at once, so the second fill costs 1.47 and releases 7.00: a net
  // inflow of 5.53, not an outflow.
  auto fill = [&](const char* id, const char* ticker, const char* side, double n, double yes_px,
                  double fee, long long ts) {
    st.exec("INSERT INTO fills(fill_id,ticker,outcome_side,count,yes_price,no_price,fee,ts) "
            "VALUES(?,?,?,?,?,?,?,?)",
            {std::string(id), std::string(ticker), std::string(side), n, yes_px, 1.0 - yes_px, fee, ts});
  };
  fill("f1", "KXTEST-A", "no", 7, 0.21, 0.02, t0 + 60);    // pays 7 * 0.79 = 5.53
  fill("f2", "KXTEST-A", "yes", 7, 0.21, 0.01, t0 + 120);  // pays 1.47, releases 7.00

  st.exec("INSERT INTO account(ts,balance,portfolio_value) VALUES(?,?,?)", {t0, 100.0, 0.0});
  // 100 - 5.53 - 0.02 - 1.47 - 0.01 + 7.00 = 99.97
  st.exec("INSERT INTO account(ts,balance,portfolio_value) VALUES(?,?,?)", {t0 + 200, 99.97, 0.0});

  auto r = reconcile_cash(st, t0, t0 + 300);
  CHECK_NEAR(r.paid, 7.0, 1e-9);            // 5.53 + 1.47
  CHECK_NEAR(r.opened, 5.53, 1e-9);         // only the first fill opened risk
  CHECK_NEAR(r.released, 7.0, 1e-9);
  CHECK_NEAR(r.fees, 0.03, 1e-9);
  CHECK_NEAR(r.predicted, -0.03, 1e-9);
  CHECK_NEAR(r.actual, -0.03, 1e-9);
  CHECK(r.explained);

  // A settlement and a transfer out both have to land, or the residual reports the difference.
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,revenue,settled_time) "
          "VALUES(?,?,?,?,?)", {std::string("KXTEST-B"), std::string("KXTEST"),
                                std::string("yes"), 4.0, iso_of(t0 + 150)});
  st.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)",
          {std::string("c1"), t0 + 160, std::string("to_margined"), -25.0});
  st.exec("INSERT INTO account(ts,balance,portfolio_value) VALUES(?,?,?)", {t0 + 300, 78.97, 0.0});
  auto r2 = reconcile_cash(st, t0, t0 + 400);
  CHECK_NEAR(r2.settlements, 4.0, 1e-9);
  CHECK_NEAR(r2.flows, -25.0, 1e-9);
  CHECK_NEAR(r2.predicted, -21.03, 1e-9);
  CHECK_NEAR(r2.actual, -21.03, 1e-9);
  CHECK(r2.explained);

  // Money the desk cannot see is money the risk book must not silently omit. The transfer above
  // left the balance this desk reads, and there is no documented way to read that instance back.
  CHECK_NEAR(untracked_transferred(st), 25.0, 1e-9);
  // Sending money back brings it into view again, and the figure nets both directions.
  st.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)",
          {std::string("c2"), t0 + 170, std::string("from_margined"), 10.0});
  CHECK_NEAR(untracked_transferred(st), 15.0, 1e-9);
  // And it never reports a negative: more coming back than went out is not negative blindness.
  st.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)",
          {std::string("c3"), t0 + 175, std::string("from_margined"), 40.0});
  CHECK_NEAR(untracked_transferred(st), 0.0, 1e-9);
  st.exec("DELETE FROM cashflows WHERE id IN ('c2','c3')");

  // Break it: a balance that moved for no recorded reason must be reported, not absorbed.
  st.exec("INSERT INTO account(ts,balance,portfolio_value) VALUES(?,?,?)", {t0 + 500, 90.0, 0.0});
  auto r3 = reconcile_cash(st, t0, t0 + 600);
  CHECK(!r3.explained);
  CHECK_NEAR(r3.residual, 11.03, 1e-9);
}

// ---------------------------------------------------------------------------------------------
// The Marseille-Paris book of 2026-09-06: 7 Paris YES, then 8 Marseille NO. Two contracts with
// different names on opposite-sounding sides, which are in fact the same bet - Paris YES pays
// only if Paris wins, Marseille NO pays on a draw OR a Paris win, so one contains the other. The
// guard let it through and the worst case went from $1.55 to $7.89.
static void test_nested_legs_are_not_a_hedge() {
  std::string path = "/tmp/desk_nested.sqlite";
  std::remove(path.c_str());
  Store st(path);
  const std::string EV = "KXLIGUE1GAME-26SEP06OLMPAR";
  for (const char* side : {"OLM", "PAR", "TIE"}) {
    Market m;
    m.ticker = EV + "-" + side;
    m.event_ticker = EV;
    m.series_ticker = "KXLIGUE1GAME";
    m.status = "active";
    m.yes_bid = 0.20;
    m.yes_ask = 0.21;
    st.upsert_market(m);
  }
  // Three outcomes, exactly one settles yes.
  st.exec("INSERT INTO event_meta(event_ticker,title,series_ticker,mutually_exclusive,exhaustive,checked_ts) "
          "VALUES(?,?,?,1,1,?)", {EV, std::string("Marseille vs Paris FC"), std::string("KXLIGUE1GAME"), now_s()});

  std::vector<Holding> paris{{EV + "-PAR", true, 7, 1.55}};   // 7 Paris YES at 0.21 plus fee
  CHECK_NEAR(event_risk(st, EV, paris).worst_loss, 1.55, 1e-9);

  Holding marseille_no{EV + "-OLM", false, 8, 6.34};          // 8 Marseille NO at 0.79 plus fee
  StackCheck c = stack_check(st, EV, paris, marseille_no);
  CHECK_NEAR(c.worst_before, 1.55, 1e-9);
  CHECK_NEAR(c.worst_after, 7.89, 1e-9);      // Marseille wins: both legs lose everything
  CHECK_NEAR(c.added_cost, 6.34, 1e-9);
  CHECK_NEAR(c.protection, 0.0, 1e-9);        // nothing bought back
  CHECK(c.stacks);

  // It does pay on a draw, which is what the old guard looked at and why it allowed this.
  auto states = event_payoffs(st, EV, {paris[0], marseille_no});
  CHECK(states.size() == 3);
  for (const auto& s : states) {
    if (s.ticker == EV + "-OLM") CHECK_NEAR(s.loss, 7.89, 1e-9);        // both lose
    if (s.ticker == EV + "-TIE") CHECK_NEAR(s.loss, 7.89 - 8, 1e-9);    // only the NO leg pays
    if (s.ticker == EV + "-PAR") CHECK_NEAR(s.loss, 7.89 - 15, 1e-9);   // both pay
  }

  // A lone Paris position loses equally on a Marseille win and on a draw, so no single leg can
  // reduce its worst case: whichever of the two it covers, the other gets deeper by the cost.
  // Both directions on Marseille therefore stack, and that is the right answer rather than a
  // limitation - to cover two states you have to buy in two of them.
  Holding marseille_yes{EV + "-OLM", true, 5, 1.10};
  CHECK(stack_check(st, EV, paris, marseille_yes).stacks);
  CHECK(stack_check(st, EV, paris, Holding{EV + "-TIE", true, 5, 1.10}).stacks);

  // What does reduce it is the leg that pays in every state the book currently loses in. Paris NO
  // pays on a Marseille win and on a draw, which is exactly the pair of states at the worst.
  Holding paris_no{EV + "-PAR", false, 7, 5.55};
  StackCheck h = stack_check(st, EV, paris, paris_no);
  CHECK(!h.stacks);
  CHECK(h.worst_after < h.worst_before);
  CHECK(h.protection > 0);
  CHECK(h.reason.find("bought back") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Kelly answers "how much given this probability" and inherits whatever the probability is worth.
// Until a strategy has beaten the price it trades against on settled markets, it gets a flat cap.
static void test_kelly_needs_calibration() {
  std::string path = "/tmp/desk_calib.sqlite";
  std::remove(path.c_str());
  Store st(path);
  auto logged = [&](const char* strat, const char* ticker, double p, double bid, double ask,
                    const char* result) {
    Market m;
    m.ticker = ticker;
    m.event_ticker = "KXCAL-E";
    m.series_ticker = "KXCAL";
    m.status = "finalized";
    m.result = result;
    st.upsert_market(m);
    st.exec("INSERT INTO model_fairs(ts,ticker,strategy,p,sigma,yes_bid,yes_ask) VALUES(?,?,?,?,?,?,?)",
            {now_s(), std::string(ticker), std::string(strat), p, 0.05, bid, ask});
  };

  // Nothing logged yet: not demonstrated, and the reason says so rather than claiming failure.
  Calibration none = strategy_calibration(st, "fresh_model", 10);
  CHECK(none.n == 0 && !none.demonstrated);
  CHECK(none.note.find("10 needed") != std::string::npos);

  // A model that is confidently wrong while the market is right. Twelve settled markets, the
  // model says 0.90 every time, the market says 0.20, and the answer is no every time.
  for (int i = 0; i < 12; ++i)
    logged("overconfident", ("KXCAL-BAD" + std::to_string(i)).c_str(), 0.90, 0.19, 0.21, "no");
  Calibration bad = strategy_calibration(st, "overconfident", 10);
  CHECK(bad.n == 12);
  CHECK(bad.brier_model > bad.brier_market);
  CHECK(!bad.demonstrated);
  CHECK(bad.note.find("the price is the better estimate") != std::string::npos);

  // A model that reads the settled outcome better than the mid does.
  for (int i = 0; i < 12; ++i)
    logged("sharp", ("KXCAL-GOOD" + std::to_string(i)).c_str(), 0.95, 0.55, 0.57, "yes");
  Calibration good = strategy_calibration(st, "sharp", 10);
  CHECK(good.n == 12 && good.brier_model < good.brier_market);
  CHECK(good.demonstrated);

  // A market with no two-sided quote cannot judge anybody and is skipped rather than scored.
  logged("sharp", "KXCAL-NOBOOK", 0.95, 0.0, 0.0, "yes");
  CHECK(strategy_calibration(st, "sharp", 10).n == 12);

  // Sample size alone is not enough: raise the bar above what exists and it stops counting.
  CHECK(!strategy_calibration(st, "sharp", 500).demonstrated);
}

// ---------------------------------------------------------------------------------------------
// The distribution of the settled daily maximum. The claim being tested is that the maximum of
// the expected hourly values is not the expected maximum, and that the difference is large enough
// at these uncertainties to matter in one and two degree brackets.
static void test_wx_distribution() {
  TrajectoryInputs in;
  in.hourly = {90, 92, 93, 93, 92, 90};   // a flat-topped afternoon: several hours near the peak
  in.sigma_day = 2.5;
  in.sigma_hour = 1.0;
  in.rho = 0.7;

  MaxDistribution d = simulate_extreme(in, 40000, 7);
  CHECK(!d.empty());
  // The old model centred the day on 93, the largest hourly forecast. Whichever of the hours near
  // the top runs warm becomes the day's high, so the true centre sits above it: +0.34F here,
  // which is a few points of probability in a one-degree bracket.
  CHECK(d.mean() > 93.2 && d.mean() < 93.6);
  CHECK(d.sd() > 2.0 && d.sd() < 4.0);

  // The lift comes from hours varying INDEPENDENTLY near the peak, so it grows with the
  // hour-to-hour noise and shrinks as the hours move together. Those two relationships are the
  // physical content of the correction; the exact number is a consequence of them.
  auto lift = [&](double sigma_hour, double rho) {
    TrajectoryInputs v = in;
    v.sigma_hour = sigma_hour;
    v.rho = rho;
    return simulate_extreme(v, 40000, 7).mean() - 93.0;
  };
  CHECK(lift(0.5, 0.7) < lift(1.0, 0.7));
  CHECK(lift(1.0, 0.7) < lift(2.0, 0.7));
  CHECK(lift(1.0, 0.95) < lift(1.0, 0.7));
  CHECK(lift(1.0, 0.7) < lift(1.0, 0.0));
  CHECK(lift(0.5, 0.7) > 0.0);      // and it is never negative: a maximum cannot be dragged down

  // Read off one distribution, a ladder cannot help summing to one.
  double below = d.p_less(92), mid = d.p_between(92, 94), above = d.p_greater(94);
  CHECK_NEAR(below + mid + above, 1.0, 1e-9);
  CHECK(mid > 0.15 && mid < 0.60);

  // The day-level bias is the term that sets the width. Without it the hours are nearly
  // independent draws around a known curve and the day looks far more certain than it is.
  TrajectoryInputs no_bias = in;
  no_bias.sigma_day = 0.0;
  CHECK(simulate_extreme(no_bias, 40000, 7).sd() < d.sd() * 0.6);

  // A single remaining hour has nothing to take a maximum over, so the peak bias disappears and
  // only the forecast error is left.
  TrajectoryInputs one = in;
  one.hourly = {93};
  MaxDistribution od = simulate_extreme(one, 40000, 7);
  CHECK(std::fabs(od.mean() - 93.0) < 0.3);
  CHECK(od.mean() < d.mean());

  // An observation already above every remaining forecast sets the floor: the day cannot come in
  // lower than what has been measured.
  TrajectoryInputs obs = in;
  obs.running_max = 99;
  MaxDistribution hd = simulate_extreme(obs, 40000, 7);
  CHECK(hd.p_at_or_below(98) < 1e-9);
  CHECK(hd.mean() >= 99.0);

  // The settlement shift is carried as a mixture, so it moves the centre by its own mean and
  // widens the answer rather than being averaged away first.
  TrajectoryInputs shifted = in;
  shifted.settle_shift = {{0, 0.5}, {1, 0.3}, {2, 0.2}};   // mean +0.7
  MaxDistribution sd2 = simulate_extreme(shifted, 40000, 7);
  CHECK(sd2.mean() > d.mean() + 0.4);
  CHECK(sd2.mean() < d.mean() + 1.0);
  CHECK(sd2.sd() > d.sd());

  // Determinism: a fair recomputed on the same inputs is the same number.
  CHECK_NEAR(simulate_extreme(in, 40000, 7).mean(), d.mean(), 1e-12);
  CHECK(std::fabs(simulate_extreme(in, 40000, 99).mean() - d.mean()) < 0.15);   // seed only jitters

  // A daily minimum is the same problem with the sign turned round: the extreme runs downward,
  // so the centre sits BELOW the lowest hourly forecast.
  TrajectoryInputs lo = in;
  lo.low = true;
  lo.hourly = {70, 68, 67, 67, 68, 70};
  MaxDistribution ld = simulate_extreme(lo, 40000, 7);
  CHECK(ld.mean() < 66.9 && ld.mean() > 66.3);   // -0.35F, the mirror of the warm-side lift
  CHECK_NEAR(ld.p_less(66) + ld.p_between(66, 68) + ld.p_greater(68), 1.0, 1e-9);

  // Nothing to go on is nothing to say.
  TrajectoryInputs blank;
  CHECK(simulate_extreme(blank, 100, 7).empty());
}

// ---------------------------------------------------------------------------------------------
// Exits. Kalshi accepts reduce_only only on immediate-or-cancel orders, and a resting exit that
// carries it is refused outright. That refusal used to trip the kill switch, which stopped the
// desk while leaving open the position it had been trying to close - four times over 2026-09-06
// and 07, on KXMLBGAME-26SEP061340SFNYM-SF and KXNCAAFGAME-26SEP06LOUMISS-MISS.
static void test_exit_order_shape() {
  OrderRequest maker;
  maker.ticker = "KXT-E-B0";
  maker.outcome_side = "yes";
  maker.price = 0.40;
  maker.count = 5;
  maker.post_only = true;
  maker.tif = "good_till_canceled";
  maker.reduce_only = (maker.tif == "immediate_or_cancel");
  json body = KalshiClient::order_body(maker);
  CHECK(body.value("time_in_force", "") == "good_till_canceled");
  CHECK(!body.contains("reduce_only"));        // the combination the exchange refuses
  CHECK(body.value("post_only", false));

  OrderRequest taker = maker;
  taker.post_only = false;
  taker.tif = "immediate_or_cancel";
  taker.reduce_only = (taker.tif == "immediate_or_cancel");
  json tb = KalshiClient::order_body(taker);
  CHECK(tb.value("time_in_force", "") == "immediate_or_cancel");
  CHECK(tb.value("reduce_only", false));       // here it is both legal and wanted
}

// ---------------------------------------------------------------------------------------------
// The proposal state machine, and the fill that lands while a cancel is in flight.
static void test_order_states_and_cancel_race() {
  // --- legal moves ----------------------------------------------------------------------------
  CHECK(Store::proposal_transition_ok("", "pending"));          // creation
  CHECK(Store::proposal_transition_ok("pending", "submitted"));
  CHECK(Store::proposal_transition_ok("pending", "expired"));
  CHECK(Store::proposal_transition_ok("submitted", "filled"));
  CHECK(Store::proposal_transition_ok("submitted", "canceled"));
  CHECK(Store::proposal_transition_ok("submitted", "unknown"));
  CHECK(Store::proposal_transition_ok("unknown", "filled"));
  CHECK(Store::proposal_transition_ok("unknown", "submitted"));  // located by reconcile
  CHECK(Store::proposal_transition_ok("submitted", "submitted")); // idempotent rewrite

  // Terminal is terminal. A filled or cancelled proposal returning to pending would be picked up
  // and sent a second time; a submitted one reset to pending would lose the order it owns.
  for (const char* term : {"filled", "canceled", "expired", "failed", "rejected"}) {
    CHECK(Store::proposal_terminal(term));
    CHECK(!Store::proposal_transition_ok(term, "pending"));
    CHECK(!Store::proposal_transition_ok(term, "submitted"));
  }
  CHECK(!Store::proposal_transition_ok("submitted", "pending"));
  CHECK(!Store::proposal_transition_ok("filled", "canceled"));

  // --- and it is enforced on write ------------------------------------------------------------
  std::string path = "/tmp/desk_states.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Proposal p;
  p.ticker = "KXT-E-B0";
  p.event_ticker = "KXT-E";
  p.series_ticker = "KXT";
  p.strategy = "test_model";
  p.mode = "manual";
  p.outcome_side = "yes";
  p.exec_style = "maker";
  p.status = "pending";
  p.price = 0.40;
  p.count = 3;
  p.id = st.insert_proposal(p);
  CHECK(p.id > 0);
  p.status = "filled";
  st.update_proposal(p);
  CHECK(st.get_proposal(p.id)->status == "filled");
  // Now try to revive it. The write must be refused and the row left alone.
  p.status = "pending";
  p.count = 999;
  st.update_proposal(p);
  auto back = st.get_proposal(p.id);
  CHECK(back->status == "filled");
  CHECK_NEAR(back->count, 3, 1e-9);       // the whole update was dropped, not just the status
  CHECK(!st.query("SELECT msg FROM events_log WHERE msg LIKE '%illegal proposal transition%'").empty());
}

// A cancel that does not take back everything that was resting means the rest filled while the
// cancel was in flight. Reading fill_count from the local table instead reports whatever the desk
// last synced, so those contracts vanish from the desk's view of its own position.
static void test_fill_during_cancel() {
  // Cancelling is a signed call, so this needs a key. A throwaway one is enough; the mock server
  // below never checks the signature.
  std::string pem = "/tmp/desk_cancel_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip cancel race test: openssl CLI not available\n");
    return;
  }
  auto signer = std::make_shared<Signer>("test-key-id", pem);
  httplib::Server server;
  std::string reduced = "2.00";
  int positions_read = 0;
  server.Delete("/trade-api/v2/portfolio/events/orders/ord-1", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"order_id", "ord-1"}, {"reduced_by", reduced}}.dump(), "application/json");
  });
  server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"balance":10000,"portfolio_value":0})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
    ++positions_read;
    r.set_content(R"({"market_positions":[],"cursor":""})", "application/json");
  });
  server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})",
                  "application/json");
  });
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread thread([&] { server.listen_after_bind(); });

  std::string path = "/tmp/desk_cancel_race.sqlite";
  std::remove(path.c_str());
  {
    Store st(path);
    Config cfg;
    Proposal p;
    p.ticker = "KXT-E-B0";
    p.event_ticker = "KXT-E";
    p.series_ticker = "KXT";
    p.strategy = "test_model";
    p.mode = "manual";
    p.outcome_side = "yes";
    p.exec_style = "maker";
    p.status = "submitted";
    p.price = 0.40;
    p.count = 5;
    p.order_id = "ord-1";
    long long pid = st.insert_proposal(p);
    Order o;
    o.order_id = "ord-1";
    o.ticker = "KXT-E-B0";
    o.outcome_side = "yes";
    o.status = "resting";
    o.initial_count = 5;
    o.remaining_count = 5;   // five resting a moment ago
    o.fill_count = 0;        // and the desk's last sync saw none of them filled
    st.upsert_order(o, pid);

    KalshiClient client("http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2", signer, "t");
    Executor ex(cfg, st, client);
    ex.cancel("ord-1");

    // Only two came back off the book, so three filled while the cancel was travelling.
    auto after = st.get_proposal(pid);
    CHECK(after->status == "filled");
    CHECK(after->note.find("partial 3.00") != std::string::npos);
    CHECK(!st.query("SELECT msg FROM events_log WHERE msg LIKE '%filled while cancelling%'").empty());
    CHECK(!st.query("SELECT msg FROM events_log WHERE msg LIKE '%fill during cancel%'").empty());
    // And the portfolio was re-read rather than left stale. If that read itself fails the desk
    // must record it, because carrying on from a position it knows is wrong is the actual danger.
    bool reread = positions_read > 0;
    bool said_it_could_not = !st.query("SELECT msg FROM events_log WHERE msg LIKE '%sync incomplete%'").empty();
    CHECK(reread || said_it_could_not);

    // A clean cancel takes everything back and says nothing about fills.
    reduced = "4.00";
    Proposal q = p;
    q.order_id = "ord-2";
    long long qid = st.insert_proposal(q);
    Order o2 = o;
    o2.order_id = "ord-2";
    o2.remaining_count = 4;
    st.upsert_order(o2, qid);
    server.Delete("/trade-api/v2/portfolio/events/orders/ord-2", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(json{{"order_id", "ord-2"}, {"reduced_by", reduced}}.dump(), "application/json");
    });
    ex.cancel("ord-2");
    CHECK(st.get_proposal(qid)->status == "canceled");
  }
  server.stop();
  thread.join();
}

// What a position returns if you get out now. bid_value is gross, and a position page answering
// "is this worth taking" with a number that ignores the cost of taking it is answering a
// different question.
static void test_sell_now_is_net_of_the_fee() {
  std::string path = "/tmp/desk_sellnow.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  st.upsert_series(Series{"KXT", "Crypto", "Test", "quadratic", "daily", 1.0});
  Market m;
  m.ticker = "KXT-E-B0";
  m.event_ticker = "KXT-E";
  m.series_ticker = "KXT";
  m.status = "active";
  m.yes_bid = 0.60;
  m.yes_ask = 0.62;
  st.upsert_market(m);
  Position p;
  p.ticker = "KXT-E-B0";
  p.position = 5;          // long 5 YES
  p.exposure = 2.50;       // paid 0.50 each
  st.replace_positions({p});

  json rb = risk_book(st, cfg);
  CHECK(rb["positions"].size() == 1);
  const auto& row = rb["positions"][0];
  CHECK_NEAR(row.value("bid_value", 0.0), 3.00, 1e-9);          // 5 x 0.60 gross
  // Selling 5 at 0.60 as a taker costs 0.07 * 0.60 * 0.40 = 0.0168 a contract.
  CHECK_NEAR(row.value("sell_net", 0.0), 3.00 - 0.084, 1e-6);
  CHECK_NEAR(row.value("sell_pnl", 0.0), 3.00 - 0.084 - 2.50, 1e-6);
  CHECK(row.value("sell_net", 0.0) < row.value("bid_value", 0.0));   // never flattering

  // No bid to sell into: the honest answer is the whole cost is at risk, not a fabricated value.
  m.yes_bid = 0;
  st.upsert_market(m);
  json rb2 = risk_book(st, cfg);
  CHECK_NEAR(rb2["positions"][0].value("sell_net", -1.0), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------------------------
// The exit decision. Two numbers: what the position is worth if held to settlement, and what
// selling returns after the fee to sell. The price it was bought at is in neither of them - it
// sets the profit that gets reported and says nothing about whether holding from here is
// attractive. The plan's example: a bid of 0.72 means opposite things at fair 0.66 and fair 0.85.
static void test_exit_decision() {
  FeeModel fees{"quadratic", 1.0};
  ExitPolicy pol;

  // Bid 0.72, fair 0.66: the market is paying more than the position is worth. Sell.
  ExitDecision cheap = exit_decision(10, 0.66, 0.72, fees, false, true, pol);
  CHECK(cheap.action == "close");
  CHECK_NEAR(cheap.hold_value, 6.60, 1e-9);
  CHECK(cheap.sell_now > 7.0 && cheap.sell_now < 7.2);     // 7.20 less the fee to sell
  CHECK(cheap.per_contract < 0);
  CHECK_NEAR(cheap.suggest_count, 10, 1e-9);
  CHECK(cheap.why.find("paying more than the position is worth") != std::string::npos);

  // The same bid at fair 0.85: selling would give away the reason for holding.
  ExitDecision rich = exit_decision(10, 0.85, 0.72, fees, false, true, pol);
  CHECK(rich.action == "hold");
  CHECK(rich.per_contract > 0.10);
  CHECK_NEAR(rich.suggest_count, 0, 1e-9);

  // And the entry price changes neither of them, because it is in neither calculation.
  CHECK(exit_decision(10, 0.66, 0.72, fees, false, true, pol).action == "close");
  CHECK(exit_decision(10, 0.85, 0.72, fees, false, true, pol).action == "hold");

  // Near fair: take part off rather than betting the position on a number barely ahead. Selling
  // 10 at 0.72 nets 0.7058 a contract after the fee, so the reduce band sits just under that.
  ExitDecision close_call = exit_decision(10, 0.70, 0.72, fees, false, true, pol);
  CHECK(close_call.action == "reduce");
  CHECK_NEAR(close_call.suggest_count, 5, 1e-9);
  CHECK(close_call.why.find("Close to fair") != std::string::npos);

  // A stale model must stop additions without forcing a liquidation: a number too old to enter on
  // is too old to sell on.
  ExitDecision stale = exit_decision(10, 0.20, 0.72, fees, true, true, pol);
  CHECK(stale.action == "stale");
  CHECK_NEAR(stale.suggest_count, 0, 1e-9);
  CHECK(stale.why.find("left alone") != std::string::npos);

  // No model at all is not a reason to sell either.
  ExitDecision blind = exit_decision(10, 0, 0.72, fees, false, false, pol);
  CHECK(blind.action == "unpriced");
  CHECK(blind.why.find("nothing to compare") != std::string::npos);

  // A bid of one cent is not an exit, whatever the model thinks.
  ExitDecision thin = exit_decision(10, 0.05, 0.01, fees, false, true, pol);
  CHECK(thin.action == "no_bid");
  CHECK_NEAR(thin.suggest_count, 0, 1e-9);
  CHECK(exit_decision(10, 0.05, 0, fees, false, true, pol).why.find("no exit here") != std::string::npos);

  // Selling always costs something, so an exactly fair bid still favours holding.
  ExitDecision fair = exit_decision(10, 0.50, 0.50, fees, false, true, pol);
  CHECK(fair.sell_now < fair.hold_value);
  CHECK(fair.action == "hold" || fair.action == "reduce");

  // Size scales the totals but not the per-contract comparison, which is what the decision uses.
  ExitDecision small = exit_decision(1, 0.66, 0.72, fees, false, true, pol);
  ExitDecision big = exit_decision(100, 0.66, 0.72, fees, false, true, pol);
  CHECK(small.action == big.action);
  CHECK(std::fabs(small.per_contract - big.per_contract) < 0.01);
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
  // Four brackets of one event, a hand fair that makes NO worth 0.98 on every one of them. Before
  // session 15 the proposal lane sized each against the per-event cap; now a model's entry is a
  // decision: every bracket is offered to you, the dialog's size stays inside the cap, and once one
  // bracket has a request queued the event is taken and the others leave the board.
  std::string path = "/tmp/desk_test4.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_modes["manual_fair"] = "manual";
  cfg.watch_series = {"KXT"};
  Series sr{"KXT", "Crypto", "", "quadratic", "hourly", 1.0};
  st.upsert_series(sr);
  const std::string expiry = iso_of(now_s() + 5 * 3600);
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
    m.expected_expiration_time = expiry;
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
  eng.configure_systematic(json{{"settings", {{"entry_style", "maker"}}}});
  eng.cycle();
  CHECK(st.proposals("pending", 100).empty());    // a model's entry is never a strategy proposal now
  int offered = 0, event_rows = 0, routed_manual = 0;
  std::string first_id, first_ticker;
  {
    // The router offers every bracket; the board shows the event once, its best row, and says how
    // many rows the event has on that route.
    const json sv = eng.systematic_view(0);
    for (const auto& r : sv["screen"]["rows"])
      if (r.value("event_ticker", "") == "KXT-E" && r.value("route", "") == "manual" && r.value("side", "") == "no")
        ++routed_manual;
    const json d = eng.decisions_view(true, 0);
    for (const auto& r : d["rows"])
      if (r.value("event_ticker", "") == "KXT-E" && r.value("route", "") == "manual" && r.value("side", "") == "no") {
        ++offered;
        event_rows = r.value("event_rows", 0);
        if (first_id.empty()) { first_id = r.value("decision_id", ""); first_ticker = r.value("ticker", ""); }
        CHECK(r.value("model_key", "") == "manual_fair_v1");
        CHECK(r.value("rank", 0) == 1);
      }
  }
  CHECK(routed_manual == 4);
  CHECK(offered == 1 && event_rows == 4);
  // The size the dialog suggests stays inside the per-event cap ($10 on a $100 bankroll).
  json sz = eng.size_suggestion("KXT-E-B0", "no", 0.89, "maker");
  CHECK(sz.value("count", 0.0) * 0.89 <= cfg.risk.max_event_frac * cfg.bankroll_dollars + 1e-9);
  // One event, one entry decision: a request queued on one bracket takes the event off the board.
  CHECK(!first_id.empty());
  json req = {{"client_request_id", "0b8f3c0e-1d2a-4c55-9e0f-5a6b7c8d9e01"}, {"ticker", first_ticker},
              {"outcome_side", "no"}, {"price", 0.89}, {"count", 1}, {"post_only", true},
              {"reviewed_ts", now_s()}, {"decision_id", first_id}};
  eng.manual_orders().enqueue(req, now_s());
  {
    const json v = eng.systematic_view(0);
    int still_offered = 0;
    for (const auto& r : v["screen"]["rows"])
      if (r.value("event_ticker", "") == "KXT-E" && (r.value("route", "") == "manual" || r.value("route", "") == "auto"))
        ++still_offered;
    CHECK(still_offered == 0);
  }
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
  st.exec("DELETE FROM orders");  // the entry-policy checks below are not a working-order test
  st.insert_account(now_s()-10,100,0);st.insert_account(now_s(),90,10);
  st.kv_set("day_start:"+now_iso().substr(0,10),"0");
  auto d=daily_equity(st,cfg);CHECK_NEAR(d["start_pv"].get<double>(),100,1e-8);
  CHECK_NEAR(d["pnl"].get<double>(),0,1e-8); // cash -> contracts is not a trading loss
  st.kv_set("portfolio_sync_ts",std::to_string(now_s()));
  auto error=[&](const Proposal& candidate){try{validate_entry(st,cfg,candidate,m);return std::string();}catch(const std::exception& e){return std::string(e.what());}};
  p.net_edge=.09;
  CHECK(error(p).empty()); // qualified same-side addition is allowed under caps
  p.strategy="weather_nws";CHECK(error(p).find("research-only")!=std::string::npos);p.strategy="ticket";
  p.count=40;CHECK(error(p).find("Event cap")!=std::string::npos);p.count=1;
  st.kv_set("portfolio_sync_ts","0");CHECK(error(p).find("stale")!=std::string::npos);st.kv_set("portfolio_sync_ts",std::to_string(now_s()));
  p.count=.019;CHECK(!error(p).empty());p.count=.01;CHECK(error(p).empty());p.count=1;
  p.price=std::nan("");CHECK(!error(p).empty());p.price=.2;
  st.insert_account(now_s(),80,9);CHECK(error(p).find("Daily equity")!=std::string::npos);
  set_account_stop_policy(st,cfg,json{{"enabled",false}});
  CHECK(!daily_equity(st,cfg).value("stop_enabled",true));
  CHECK(error(p).empty()); // disabling this rule does not disable the other entry checks
  st.kv_set("kill","1");CHECK(error(p).find("kill")!=std::string::npos);st.kv_set("kill","0");
  set_account_stop_policy(st,cfg,json{{"enabled",true},{"loss_fraction",.70}});
  CHECK(error(p).empty());
  bool bad_policy=false;try{set_account_stop_policy(st,cfg,json{{"loss_fraction",1.5}});}catch(...){bad_policy=true;}
  CHECK(bad_policy);
  CHECK_NEAR(account_stop_policy(st,cfg)["loss_fraction"].get<double>(),.70,1e-9);
  set_account_stop_policy(st,cfg,json{{"loss_fraction",.10}});
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
  // A test model has checked what its contracts settle on unless a test says otherwise.
  bool verified = true;
  std::optional<bool> settlement_verified(const Market&) const override { return verified; }
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

// A market the decision router can act on: seed_kxt's two brackets, settling in five hours.
static void seed_kxt_live(Store& st, double yes_bid, double yes_ask) {
  seed_kxt(st, yes_bid, yes_ask);
  st.exec("UPDATE markets SET expected_expiration_time=?", {iso_of(now_s() + 5 * 3600)});
}
// Route the test model as production routes an unproven one (manual), or anywhere else. These
// exchanges are unreachable, so no book is captured and a taker sees no displayed size: the
// tests that want an offered row post as makers.
static void route_test_model(Engine& eng, const std::string& route, const std::string& style = "maker") {
  eng.configure_systematic(json{{"settings", {{"model_routes", {{"test_model_v1", route}}}, {"entry_style", style}}}});
}
// The screen row (every route, blocked and weak included) for one side of a market.
static json screen_row(Engine& eng, const std::string& ticker, const std::string& side) {
  json v = eng.systematic_view(0);
  for (const auto& r : v["screen"]["rows"])
    if (r.value("ticker", "") == ticker && r.value("side", "") == side) return r;
  return json();
}
// The cycle's published decision (MANUAL, AUTO or RESEARCH) for one side, or null.
static json decision_row(Engine& eng, const std::string& ticker, const std::string& side) {
  json v = eng.decisions_view(true, 0);
  for (const auto& r : v["rows"])
    if (r.value("ticker", "") == ticker && r.value("side", "") == side) return r;
  return json();
}

// ---------------------------------------------------------------------------------------------
// A resting exit outliving its position. Kalshi refuses reduce_only on anything that rests, so
// the desk enforces it: without this, an exit left working after the position settled or was
// closed by hand would open the opposite position if it filled - the failure the exchange flag
// existed to prevent. run_exits() used to iterate positions, so a ticker with no position left
// was never looked at again.
static void test_resting_exit_outliving_its_position() {
  std::string path = "/tmp/desk_exit_sweep.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.watch_series = {"KXT"};
  seed_kxt(st, 0.10, 0.12);

  // An exit proposal that reached the exchange and is resting for 5 contracts.
  Proposal p;
  p.ticker = "KXT-E-B0";
  p.event_ticker = "KXT-E";
  p.series_ticker = "KXT";
  p.strategy = "exit";
  p.mode = "manual";
  p.outcome_side = "no";
  p.exec_style = "maker";
  p.status = "submitted";
  p.price = 0.90;
  p.count = 5;
  p.order_id = "resting-exit-1";
  long long pid = st.insert_proposal(p);
  CHECK(pid > 0);
  Order o;
  o.order_id = "resting-exit-1";
  o.ticker = "KXT-E-B0";
  o.outcome_side = "no";
  o.status = "resting";
  o.initial_count = 5;
  o.remaining_count = 5;
  st.upsert_order(o, pid);
  CHECK(st.orders("resting").size() == 1);
  CHECK(st.proposal_for_order("resting-exit-1") == pid);

  // No position in that ticker at all: the thing this exit was closing is gone.
  CHECK(st.positions().empty());

  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  eng.cycle();

  // The cancel cannot succeed against an unreachable exchange, and that is the point: the desk
  // must still record that an order it cannot pull is resting against nothing.
  auto pulled = st.query(
      "SELECT msg FROM events_log WHERE msg LIKE 'pulling resting exit%' AND msg LIKE '%KXT-E-B0%'");
  CHECK(pulled.size() == 1);
  CHECK(pulled[0].value("msg", "").find("5.00 resting against 0.00 held") != std::string::npos);
  auto failed = st.query("SELECT msg FROM events_log WHERE msg LIKE '%could NOT be pulled%'");
  CHECK(failed.size() == 1);

  // An exit no larger than the position it is closing is left alone.
  std::string path2 = "/tmp/desk_exit_sweep2.sqlite";
  std::remove(path2.c_str());
  Store st2(path2);
  seed_kxt(st2, 0.10, 0.12);
  long long pid2 = st2.insert_proposal(p);
  Order o2 = o;
  st2.upsert_order(o2, pid2);
  Position held;
  held.ticker = "KXT-E-B0";
  held.position = -5;          // exactly what the exit is selling
  st2.replace_positions({held});
  st2.kv_set("model_exit:KXT-E-B0", "1"); // explicit ownership; size sweep alone is under test
  KalshiClient c2("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex2(cfg, st2, c2);
  Engine eng2(cfg, st2, c2, ex2);
  eng2.cycle();
  CHECK(st2.query("SELECT msg FROM events_log WHERE msg LIKE 'pulling resting exit%'").empty());
}


static void test_instant_lane() {
  // The fast lane sent a big-edge proposal the cycle it appeared. Since session 15 a model's entry
  // is never a proposal - the decision router owns execution eligibility and only the algorithm
  // sends an AUTO decision - so the lane has nothing of a model's to send, in any mode. The
  // setting itself still validates and persists.
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
  // A model in bot mode, AUTO armed, an edge far over the instant threshold: nothing is
  // proposed and nothing is tried. The row is the router's (offered to you: not graduated).
  auto ts = std::make_unique<TestStrategy>();
  TestStrategy* tsp = ts.get();
  tsp->f.p = 0.30;                                   // vs an ask of 0.12: a big edge
  tsp->f.sigma = 0.02;
  eng.add_strategy(std::move(ts));
  eng.set_mode("test_model", "bot");
  eng.set_auto_armed(true);
  eng.cycle();
  eng.cycle();
  CHECK(st.proposals("pending", 10).empty());
  bool tried = false;
  for (const auto& pr : st.proposals("", 40))
    if (pr.note.find("INSTANT") != std::string::npos || pr.strategy == "test_model") tried = true;
  CHECK(!tried);
  std::remove(path.c_str());
}

static void test_research_gates() {
  // 1. Disagreement gate: a model 41 points from a two-sided market logs its number and is offered
  //    to nobody. Since session 15 the observable is the decision, not a proposal: a model's entry
  //    is never a strategy proposal any more.
  {
    std::string path = "/tmp/desk_test5.sqlite";
    std::remove(path.c_str());
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    seed_kxt_live(st, 0.60, 0.62);
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.20;  // vs mid 0.61
    tsp->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    route_test_model(eng, "manual");
    eng.cycle();
    CHECK(tsp->prepared == 1);
    CHECK(st.proposals("pending", 10).empty());
    CHECK(!st.query("SELECT * FROM model_fairs WHERE strategy='test_model'", {}).empty());  // Brier sample still logged
    auto ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.note.find("RESEARCH ONLY") != std::string::npos);
    CHECK(ff && ff->first.note.find("disagreement") != std::string::npos);
    json r = screen_row(eng, "KXT-E-B0", "no");
    CHECK(r.is_object() && r["route"] == "blocked" && r["reasons"].dump().find("disagreement") != std::string::npos);
    CHECK(decision_row(eng, "KXT-E-B0", "no").is_null());
    // Same market, model inside the gate (0.80 vs 0.61; blended half way to the mid for an
    // uncalibrated model, 0.705): offered to you, and still no proposal.
    tsp->f.p = 0.80;
    eng.cycle();
    json d = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(d.is_object() && d["route"] == "manual" && d["model_key"] == "test_model_v1");
    CHECK(st.proposals("pending", 10).empty());
    // It leaves the board once the model drifts into disagreement again.
    tsp->f.p = 0.20;
    eng.cycle();
    CHECK(decision_row(eng, "KXT-E-B0", "yes").is_null());
    // A model-flagged research estimate is priced and recorded, offered to nobody.
    tsp->f.p = 0.80;
    tsp->f.research_only = true;
    tsp->f.research_reason = "unit test";
    eng.cycle();
    d = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(d.is_object() && d["route"] == "research" && d["reasons"].dump().find("unit test") != std::string::npos);
    ff = eng.fair_for("KXT-E-B0");
    CHECK(ff && ff->first.note.find("unit test") != std::string::npos);
    // "research" mode: prepare() still runs (collectors keep logging), and the mode still caps the
    // model's route: research, whatever the model route says.
    tsp->f.research_only = false;
    eng.set_mode("test_model", "research");
    int before = tsp->prepared;
    eng.cycle();
    CHECK(tsp->prepared == before + 1);
    d = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(d.is_object() && d["route"] == "research" && d["reasons"].dump().find("research mode") != std::string::npos);
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
    // The settled value is the climate report's maximum, which is the hourly maximum plus a
    // station-specific shift. Configure a known shift and check the mixture arithmetic, rather
    // than a constant that moves every time the shift is remeasured: obs 81.3 rounds to 81, so
    // shift 0 and +1 land inside 81-82 and +2 does not.
    json wxp;
    wxp["stations"]["KXHIGHLAX"]["settle_shift"] = {{"0", 0.5}, {"1", 0.3}, {"2", 0.2}};
    WeatherNws wx5(wxp, "t");
    wx5.set_day_forecast("KXHIGHLAX", past);
    wx5.set_clock(parse_iso("2026-09-06T07:30:00Z"));
    auto f7 = wx5.fair(h, s, st);
    CHECK(f7 && std::fabs(f7->p - 0.8) < 1e-6);
    // A series whose settlement rules nobody has read cannot trade, whatever the model thinks.
    // KXHIGHHOU has no confirmed climate product, so it is research-only on that ground alone.
    Market hou = g;
    hou.ticker = "KXHIGHHOU-26SEP05-T80";
    hou.event_ticker = "KXHIGHHOU-26SEP05";
    hou.series_ticker = "KXHIGHHOU";
    WeatherNws wx6(json::object(), "t");
    WeatherNws::DayForecast hd = past;
    wx6.set_day_forecast("KXHIGHHOU", hd);
    wx6.set_clock(parse_iso("2026-09-06T07:30:00Z"));
    auto f8 = wx6.fair(hou, s, st);
    CHECK(f8 && f8->research_only);
    CHECK(f8->research_reason.find("settlement product") != std::string::npos);
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
  // validate_entry: once an event is held, another contract in it is a managed-event decision.
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
  CHECK(err.find("already has exposure") != std::string::npos);
  q.strategy = "ticket";
  err.clear();
  try { validate_entry(st, cfg, q, t80); } catch (const std::exception& e) { err = e.what(); }
  CHECK(err.find("already has exposure") != std::string::npos);  // a hand ticket uses the same rule
  // The router in a held event (session 15; this used to be the proposal lane's job): the other
  // brackets are blocked, and a qualified same-side addition to a position you hold is offered to
  // you, never to the algorithm. No proposal is made either way.
  {
    std::string path = "/tmp/desk_test6.sqlite";
    std::remove(path.c_str());
    Store st2(path);
    Config cfg2; cfg2.bankroll_dollars = 100; cfg2.strategy_modes["test_model"] = "manual"; cfg2.watch_series = {"KXT"};
    seed_kxt_live(st2, 0.30, 0.32);
    Position held; held.ticker = "KXT-E-B0"; held.position = -5; held.exposure = 3.4;  // NO on bracket 0
    st2.replace_positions({held});
    KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
    Executor ex2(cfg2, st2, client);
    Engine eng(cfg2, st2, client, ex2);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.05; tsp->f.sigma = 0.02;  // NO everywhere (0.82 after the blend, against a 0.70 offer)
    eng.add_strategy(std::move(ts));
    route_test_model(eng, "manual");
    eng.cycle();
    CHECK(st2.proposals("pending", 10).empty());
    json b1 = screen_row(eng, "KXT-E-B1", "no");   // a second market in an event that carries risk
    CHECK(b1.is_object() && b1["route"] == "blocked" && b1["reasons"].dump().find("already has exposure") != std::string::npos);
    json add = decision_row(eng, "KXT-E-B0", "no");   // more of the NO you hold: yours to review
    CHECK(add.is_object() && add["route"] == "manual" && add["reasons"].dump().find("addition") != std::string::npos);
    json flip = screen_row(eng, "KXT-E-B0", "yes");   // the other side of your own position
    CHECK(flip.is_object() && flip["route"] == "blocked");
    held.position = 5; held.exposure = 3.1;  // YES on bracket 0
    st2.replace_positions({held});
    seed_kxt_live(st2, 0.60, 0.62);
    tsp->f.p = 0.85;                            // YES everywhere: YES B1 loses wherever YES B0 loses -> blocked
    eng.cycle();
    CHECK(st2.proposals("pending", 10).empty());
    add = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(add.is_object() && add["route"] == "manual" && add["reasons"].dump().find("addition") != std::string::npos);
    CHECK(decision_row(eng, "KXT-E-B1", "yes").is_null());
    json b1y = screen_row(eng, "KXT-E-B1", "yes");
    CHECK(b1y.is_object() && b1y["reasons"].dump().find("already has exposure") != std::string::npos);
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
  // A model must never acquire an externally/manual-held position merely because it covers
  // that market. Legacy model exits require an explicit per-position opt-in.
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(p.strategy != "exit");
  st.kv_set("model_exit:KXT-E-B0", "1");
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


// A position the systematic trader opened is its to manage: the engine's model exit must not
// queue a second exit on it, or two managers would fight over one position.
static void test_algo_owned_positions_are_left_alone() {
  std::string path = "/tmp/desk_test_algo_owned.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_modes["test_model"] = "manual";
  cfg.watch_series = {"KXT"};
  seed_kxt(st, 0.40, 0.44);
  Position held; held.ticker = "KXT-E-B0"; held.position = 10; held.exposure = 3.0;
  st.replace_positions({held});
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);   // creates systematic_trades
  auto ts = std::make_unique<TestStrategy>();
  ts->f.p = 0.36; ts->f.sigma = 0.02;
  eng.add_strategy(std::move(ts));
  st.exec("INSERT INTO systematic_trades(ticker,event_ticker,series_ticker,side,state,mode,opened_ts,filled_ts,filled,entry_price,entry_cost,count) "
          "VALUES('KXT-E-B0','KXT-E','KXT','yes','open','live',?,?,10,0.30,3.0,10)", {now_s() - 60, now_s() - 60});
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(!(p.strategy == "exit" && p.ticker == "KXT-E-B0"));
  // That row leaves unrealized/realized_pnl NULL. The systematic view read them as numbers and
  // threw, and book capture (which reads the view every cycle) went down with it.
  bool plan_threw = false;
  try { eng.book_capture_plan(); } catch (const std::exception&) { plan_threw = true; }
  CHECK(!plan_threw);
  CHECK(eng.status().value("last_error", std::string()).find("books") == std::string::npos);
  // Releasing does not hand the position to another model exit manager.
  st.exec("UPDATE systematic_trades SET state='closed', closed_ts=? WHERE ticker='KXT-E-B0'", {now_s() - 600});
  eng.cycle();
  bool have_exit = false;
  for (const auto& p : st.proposals("pending", 20)) if (p.strategy == "exit" && p.ticker == "KXT-E-B0") have_exit = true;
  CHECK(!have_exit);
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
  // Three contracts belong to the algorithm; a manual bracket owns only the remaining seven.
  st.exec("INSERT INTO systematic_trades(ticker,event_ticker,series_ticker,side,state,mode,opened_ts,filled_ts,filled,entry_price,entry_cost,count) "
          "VALUES('KXT-E-B0','KXT-E','KXT','yes','open','live',?,?,3,0.30,0.90,3)", {now,now});
  CHECK_NEAR(manual_position_quantity(st, held), 7.0, 1e-9);
  // Fill history can precede both order and strategy counters during reconciliation.
  st.exec("UPDATE systematic_trades SET state='entering',filled=0,entry_order_id='early-fill' WHERE ticker='KXT-E-B0'");
  Fill early;early.fill_id="early-fill-record";early.order_id="early-fill";early.ticker=held.ticker;
  early.outcome_side="yes";early.count=2;early.yes_price=.3;early.no_price=.7;early.ts=now;
  st.upsert_fill(early);
  CHECK_NEAR(manual_position_quantity(st, held), 8.0, 1e-9);
  st.exec("UPDATE systematic_trades SET state='open',filled=3 WHERE ticker='KXT-E-B0'");
  Position opposite=held;opposite.position=-10;
  CHECK_NEAR(manual_position_quantity(st, opposite), 10.0, 1e-9);
  // Trailing stop: trail 0.05, bid 0.38 -> stop ratchets to 0.33 (above the 0.22 set by hand).
  st.exec("UPDATE brackets SET trail=0.05 WHERE ticker='KXT-E-B0'");
  eng.cycle();
  json b = st.query("SELECT sl_price, alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  for (const auto& p : st.proposals("pending", 20)) if (p.strategy=="target") CHECK_NEAR(p.count,7.0,1e-9);
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
    CHECK_NEAR(stop->count,7.0,1e-9);
  }
  // A stop does not cancel a foreign working order to make room for itself.
  st.exec("UPDATE proposals SET status='expired'");
  Order foreign;foreign.order_id="manual-rest";foreign.ticker=held.ticker;foreign.status="resting";
  foreign.outcome_side="yes";foreign.yes_price=.10;foreign.no_price=.90;foreign.remaining_count=1;
  st.upsert_order(foreign,0);
  eng.cycle();
  CHECK(st.orders("resting").size()==1);
  for(const auto& p:st.proposals("pending",20)) CHECK(p.strategy!="stop");
  foreign.status="canceled";foreign.remaining_count=0;st.upsert_order(foreign,0);
  b = st.query("SELECT alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  CHECK(!b.empty() && b[0]["alerted"].get<std::string>() == "stop");
  // Alert mode never creates proposals.
  st.exec("UPDATE proposals SET status='expired'");
  st.exec("UPDATE brackets SET mode='alert', alerted='' WHERE ticker='KXT-E-B0'");
  eng.cycle();
  for (const auto& p : st.proposals("pending", 20)) CHECK(p.strategy != "stop" && p.strategy != "target");
  b = st.query("SELECT alerted FROM brackets WHERE ticker='KXT-E-B0'", {});
  CHECK(!b.empty() && b[0]["alerted"].get<std::string>() == "stop");  // still flagged for the screen
  st.exec("UPDATE systematic_trades SET filled=10 WHERE ticker='KXT-E-B0'");
  st.exec("UPDATE brackets SET mode='auto' WHERE ticker='KXT-E-B0'");
  eng.cycle();
  CHECK_NEAR(manual_position_quantity(st,held),0,1e-9);
  for(const auto& p:st.proposals("pending",20)) CHECK(p.strategy!="stop" && p.strategy!="target");
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
  // The pair he actually put on: YES 79-80 and NO 81+. The NO leg does win if it settles 78, and
  // the old guard allowed it on exactly that ground. But 78 and 81+ were EQUALLY the worst states
  // before it, so covering one of them only deepened the other, by the whole cost of the leg -
  // and 81+ is the state that settled. Asking what happened to the worst case gets this right
  // where asking "does it pay in some worst state" did not.
  Holding no_t80{"KXHIGHLAX-26SEP05-T80", false, 13, 3.90};
  StackCheck sc = stack_check(st, EV, held, no_t80);
  CHECK(sc.stacks);
  CHECK_NEAR(sc.worst_before, 5.94, 1e-9);
  CHECK_NEAR(sc.worst_after, 9.84, 1e-9);
  CHECK_NEAR(sc.protection, 0.0, 1e-9);        // it bought back nothing
  CHECK(sc.reason.find("same bet again") != std::string::npos);
  std::vector<Holding> pair{held[0], no_t80};
  EventRisk pr = event_risk(st, EV, pair);
  CHECK_NEAR(pr.worst_loss, 9.84, 1e-9);                       // both legs lose at 81+
  CHECK(pr.worst_state == "KXHIGHLAX-26SEP05-T80=YES");
  for (const auto& s2 : event_payoffs(st, EV, pair))
    if (s2.ticker == "KXHIGHLAX-26SEP05-T78") CHECK_NEAR(s2.loss, 9.84 - 13, 1e-9);   // the NO leg pays here
  // Adding to the bracket that is already winning pays in no worst state: still refused.
  CHECK(stacks_event(st, EV, held, Holding{"KXHIGHLAX-26SEP05-B79.5", true, 5, 1.70}));
  // A first leg in an empty event cannot stack anything.
  CHECK(!stacks_event(st, EV, {}, no_t80));
  // Buying the bracket that wins at 81+ covers one of the two worst states and leaves the other
  // deeper by its cost, so on its own it is not a hedge either.
  Holding yes_t80{"KXHIGHLAX-26SEP05-T80", true, 10, 4.80};
  CHECK(stacks_event(st, EV, held, yes_t80));
  // Once the book has a single binding worst state, the leg that pays there genuinely reduces it
  // and is allowed. This is the shape a real hedge has.
  StackCheck real = stack_check(st, EV, pair, yes_t80);
  CHECK(!real.stacks);
  CHECK_NEAR(real.worst_before, 9.84, 1e-9);
  CHECK(real.worst_after < real.worst_before - 1.0);
  CHECK(real.protection > 0);
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
    // The board the router reads carries the stored displayed sizes, not zeros.
    bool sized = false;
    {
      const json b = eng.board_view(0);
      for (const auto& r : b["markets"])
        if (r.value("ticker", "") == "KXT-E-B0" && r.value("yes_ask_size", 0.0) == 500) sized = true;
    }
    CHECK(sized);
    // Age the stored quotes past the gate: the next cycle's rows are stale, and nothing is offered.
    st.exec("UPDATE markets SET updated_ts=?", {now_s() - 3600});
    eng.cycle();
    {
      const json b = eng.board_view(0);
      for (const auto& r : b["markets"]) CHECK(r.value("quote_stale", false) == true);
      const json d = eng.decisions_view(true, 0);
      CHECK(d["rows"].empty());
    }
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
  // A settled market with a ledger row: the SETTLEMENT RECORD is the single source, and the
  // ledger is not consulted. This assertion used to read the other way round, and that is what
  // made the board report a loss on an account that was up: Kalshi's realized_pnl covers only the
  // portion closed before expiry, so preferring it threw away whatever settled at expiry.
  Market m2 = m; m2.ticker = "KXHIGHLAX-26SEP05-B79.5"; m2.event_ticker = "KXHIGHLAX-26SEP05"; m2.series_ticker = "KXHIGHLAX"; st.upsert_market(m2);
  st.exec("INSERT INTO realized(ticker,realized_pnl,fees_paid,updated_ts) VALUES(?,?,?,?)", {m2.ticker, -3.29, 0.02, now_s()});
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
          {m2.ticker, m2.event_ticker, "no", 13.0, 0.0, 4.29, 0.0, 0.0, 0.0, "2026-09-06T19:05:00Z"});
  pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["settled_pnl"].get<double>(), 0.0 - 4.29, 1e-9);  // 13 YES bought, NO won: total loss
  CHECK(pnl["n_settled"].get<int>() == 1);
  // A July settlement, no ledger row, unchanged by the fix: one side, revenue already gross.
  st.exec("INSERT INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
          {"KXOLD-26JUL08-X", "KXOLD-26JUL08", "yes", 10.0, 0.0, 6.0, 0.0, 10.0, 0.1, "2026-07-08T19:05:00Z"});
  pnl = build_pnl(st, "America/Chicago");
  CHECK_NEAR(pnl["settled_pnl"].get<double>(), -4.29 + (10.0 - 6.0 - 0.1), 1e-9);

  // settled_pnl() against two real markets from prod.sqlite, both worked out by hand.
  //
  // A market traded in BOTH directions. 12 NO at .17, then 9 YES at .57, which closes 9 of the NO
  // at .43. Kalshi realized +2.34 on that close; the 3 NO left settled for 3.00 against a .51
  // basis; fees .2731. True P&L +4.5569. `revenue` on this record is 3.00 - the NET position's
  // payout - so reading revenue directly reports +0.33 less than half the truth, and the old code
  // preferred the ledger's +2.34 and lost the settlement leg entirely.
  json both = {{"market_result", "no"}, {"yes_count", 9.0}, {"no_count", 12.0},
               {"yes_cost", 5.13}, {"no_cost", 2.04}, {"revenue", 3.0}, {"fee", 0.2731}};
  CHECK_NEAR(settled_pnl(both), 4.5569, 1e-9);
  CHECK(settled_pnl(both) > both["revenue"].get<double>() - 5.13 - 2.04 - 0.2731);  // strictly better than the naive read
  // A market closed partway and then expired worthless. 5.73 YES at a .2594 average, 1 closed at
  // .19 (-0.0694), 4.73 expired (-1.2268), fees .0812: -1.3774 by hand. The record's stored costs
  // are rounded to the tenth of a cent, so the record gives -1.3772; the 2e-4 between them is that
  // rounding and not a disagreement. Pinned to what the record actually yields.
  json part = {{"market_result", "no"}, {"yes_count", 5.73}, {"no_count", 1.0},
               {"yes_cost", 1.486}, {"no_cost", 0.81}, {"revenue", 0.0}, {"fee", 0.0812}};
  CHECK_NEAR(settled_pnl(part), -1.3772, 1e-9);
  CHECK(std::fabs(settled_pnl(part) - (-1.3774)) < 3e-4);  // and it agrees with the hand figure
  // A position opened and fully closed before expiry: equal counts, revenue 0. The matched pairs
  // are worth their count, so the loss is the two costs net of it - not the whole of both costs.
  json flat = {{"market_result", "yes"}, {"yes_count", 3.0}, {"no_count", 3.0},
               {"yes_cost", 2.67}, {"no_cost", 0.54}, {"revenue", 0.0}, {"fee", 0.0}};
  CHECK_NEAR(settled_pnl(flat), 3.0 - 2.67 - 0.54, 1e-9);
  // Fees are charged once. The settlement fee and the summed fill fees are the same money on all
  // 77 settled markets on this account; subtracting both was the second of the three defects.
  json fee_once = {{"market_result", "yes"}, {"yes_count", 4.0}, {"no_count", 0.0},
                   {"yes_cost", 2.0}, {"no_cost", 0.0}, {"revenue", 4.0}, {"fee", 0.25}};
  CHECK_NEAR(settled_pnl(fee_once), 4.0 - 2.0 - 0.25, 1e-9);
  // ---- all-time account P&L, top-down ------------------------------------------------
  // The real numbers off prod.sqlite on 2026-09-12, because this is the figure that says whether
  // the account made money and it was wrong for weeks.
  Store ac(":memory:");
  ac.exec("INSERT INTO account(ts,balance,portfolio_value) VALUES(?,?,?)", {now_s(), 114.8953, 0.0});
  ac.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"d1", 1783444741, "deposit", 10.0});
  ac.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"d2", 1788700000, "deposit", 100.0});
  ac.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"t1", 1788800000, "to_margined", -25.0});
  // No perps tables yet: absent must read as zero rather than throwing, and the transfer out must
  // not be mistaken for money that left the building.
  json ap = build_pnl(ac, "America/Chicago")["account"];
  CHECK_NEAR(ap["deposits"].get<double>(), 110.0, 1e-9);
  CHECK_NEAR(ap["perps_equity"].get<double>(), 0.0, 1e-9);
  CHECK_NEAR(ap["credits"].get<double>(), 0.0, 1e-9);
  CHECK_NEAR(ap["equity"].get<double>(), 114.8953, 1e-9);
  // Now the perps side: $25 transferred in, $25 credited by the exchange for volume, 8 fills that
  // between them lost 13.51 cents. Equity 49.8649.
  ac.exec("CREATE TABLE IF NOT EXISTS perp_account(ts INTEGER PRIMARY KEY, settled_funds REAL, position_value REAL, equity REAL, maintenance_margin REAL, initial_margin REAL, available REAL)");
  ac.exec("CREATE TABLE IF NOT EXISTS perp_fills(fill_id TEXT PRIMARY KEY, order_id TEXT, ticker TEXT, side TEXT, count REAL, price REAL, entry_price REAL, fees REAL, realized_pnl REAL, is_taker INTEGER, created_time TEXT, ts INTEGER, order_source TEXT)");
  ac.exec("INSERT INTO perp_account(ts,settled_funds,position_value,equity,maintenance_margin,initial_margin,available) VALUES(?,?,?,?,?,?,?)",
          {now_s(), 49.8649, 0.0, 49.8649, 0.0, 0.0, 49.8649});
  ac.exec("INSERT INTO perp_fills(fill_id,realized_pnl,fees,ts) VALUES(?,?,?,?)", {"f1", -0.0015, 0.1336, now_s()});
  ap = build_pnl(ac, "America/Chicago")["account"];
  CHECK_NEAR(ap["equity"].get<double>(), 164.7602, 1e-9);            // 114.8953 + 49.8649
  CHECK_NEAR(ap["credits"].get<double>(), 25.0, 1e-9);               // 49.8649 - 25.00 - (-0.1351)
  CHECK_NEAR(ap["gain_over_deposits"].get<double>(), 54.7602, 1e-9);
  CHECK_NEAR(ap["all_time"].get<double>(), 29.7602, 1e-9);           // the number on the board
  // A withdrawal is money leaving and must reduce the gain; a transfer never does. Same equity,
  // so taking $10 out means the trading earned $10 more than the balance alone suggests.
  ac.exec("INSERT INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)", {"w1", 1788900000, "withdrawal", -10.0});
  ap = build_pnl(ac, "America/Chicago")["account"];
  CHECK_NEAR(ap["deposits"].get<double>(), 100.0, 1e-9);
  CHECK_NEAR(ap["all_time"].get<double>(), 39.7602, 1e-9);

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
    // The per-order cap is the outermost limit and fires first, before anything portfolio-shaped
    // is consulted. 100 contracts at 0.20 is $20 against a $10 default.
    try{ex.ticket(req,"over order cap");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("Order cap")!=std::string::npos);}
    CHECK(writes==0);
    cfg.risk.max_order_dollars=1000;   // held by reference; lift it to reach the portfolio caps
    try{ex.ticket(req,"over cap");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("Event cap")!=std::string::npos);}
    CHECK(writes==0);
    // Turnover is a separate limit from exposure: it counts what has been bought today whether or
    // not any of it is still held, because the fee is charged on volume rather than on profit.
    cfg.risk.max_turnover_dollars_per_day=0.01;
    req.count=1;
    try{ex.ticket(req,"over turnover");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("Turnover cap")!=std::string::npos);}
    cfg.risk.max_turnover_dollars_per_day=100.0;
    // A fill between the two reads must not disappear from both positions and orders.
    fill_between_reads=true;race_filled=false;req.count=15;
    try{ex.ticket(req,"fill during refresh");CHECK(false);}catch(const std::exception& e){CHECK(std::string(e.what()).find("working order")!=std::string::npos);}
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

static void test_position_aware_entry_policy() {
  Store st(":memory:");
  Config cfg;
  cfg.risk.max_entry_horizon_hours = 48;
  cfg.risk.add_min_edge = .08;
  cfg.risk.add_edge_improvement = .03;
  cfg.risk.add_cooldown_seconds = 900;
  cfg.risk.auto_additions = false;
  Series s{"KXNCAAFGAME", "Sports", "", "quadratic", "daily", 1.0};
  st.upsert_series(s);
  long long now = now_s();
  auto market = [&](const std::string& ticker, const std::string& event, long long settle) {
    Market m; m.ticker=ticker; m.event_ticker=event; m.series_ticker=s.ticker; m.status="active";
    m.close_time=iso_of(settle-60); m.expected_expiration_time=iso_of(settle);
    m.yes_bid=.40; m.yes_ask=.42; m.no_bid=.58; m.no_ask=.60; st.upsert_market(m); return m;
  };
  Market wis=market("KXNCAAFGAME-WISND-WIS","KXNCAAFGAME-WISND",now+3600);
  Market nd=market("KXNCAAFGAME-WISND-ND","KXNCAAFGAME-WISND",now+3600);
  Position pos; pos.ticker=wis.ticker; pos.position=5; pos.exposure=2; st.replace_positions({pos});

  Proposal p; p.ticker=nd.ticker; p.event_ticker=nd.event_ticker; p.series_ticker=s.ticker;
  p.strategy="sports_live"; p.outcome_side="yes"; p.exec_style="maker"; p.price=.4; p.count=1; p.net_edge=.50;
  EntryContext c=entry_context(st,cfg,p,nd,now);
  CHECK(!c.allowed && c.kind=="managed" && c.reason.find("already has exposure")!=std::string::npos);

  p.ticker=wis.ticker; p.event_ticker=wis.event_ticker; p.net_edge=.07;
  c=entry_context(st,cfg,p,wis,now);
  CHECK(!c.allowed && c.kind=="addition" && c.required_edge==.08);
  p.net_edge=.09;
  c=entry_context(st,cfg,p,wis,now);
  CHECK(c.allowed && c.kind=="addition" && c.approval_required);

  Proposal prior=p; prior.status="filled"; prior.net_edge=.10; st.insert_proposal(prior);
  Order o; o.order_id="prior"; o.ticker=wis.ticker; o.outcome_side="yes"; o.status="filled"; st.upsert_order(o,prior.id);
  Fill f; f.fill_id="prior-fill"; f.order_id=o.order_id; f.ticker=wis.ticker; f.outcome_side="yes"; f.count=1; f.ts=now-30; st.upsert_fill(f);
  p.net_edge=.20;
  c=entry_context(st,cfg,p,wis,now);
  CHECK(!c.allowed && c.cooldown_remaining==870 && c.required_edge>=.13-.000001);
  c=entry_context(st,cfg,p,wis,now+901);
  CHECK(c.allowed && c.required_edge>=.13-.000001);

  Market far=market("KXNCAAFGAME-LATER-A","KXNCAAFGAME-LATER",now+49*3600);
  p.ticker=far.ticker; p.event_ticker=far.event_ticker; p.net_edge=.50;
  c=entry_context(st,cfg,p,far,now);
  CHECK(!c.allowed && c.reason.find("T+2")!=std::string::npos);
  Market near=market("KXNCAAFGAME-SOON-A","KXNCAAFGAME-SOON",now+47*3600);
  p.ticker=near.ticker; p.event_ticker=near.event_ticker;
  CHECK(entry_context(st,cfg,p,near,now).allowed);
}

// A daily-loss halt is a threshold, not a fault: once the threshold is switched off there is
// nothing left for a person to review, so it must not survive the restart that switched it off.
// Every other kill reason does survive, because editing a config file does not undo whatever
// caused it.
static void test_daily_halt_clears_when_the_stop_is_off() {
  const std::string path = "/tmp/desk_test_daily_halt.sqlite";
  KalshiClient client("https://example.invalid/trade-api/v2", nullptr, "t");
  auto build = [&](Config cfg, const std::string& reason, bool stop_enabled) {
    std::remove(path.c_str());
    Store st(path);
    st.kv_set("kill", "1");
    st.kv_set("kill_reason", reason);
    cfg.risk.daily_loss_stop_enabled = stop_enabled;
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    return std::make_pair(eng.killed(), st.kv_get("kill").value_or("?"));
  };
  const std::string daily = "Daily loss stop: equity $92.16 <= floor $92.59 (day start $102.88, money moved 0.00)";

  auto off = build(Config{}, daily, false);
  CHECK(off.first == false && off.second == "0");        // cleared, and the clearing persisted

  auto on = build(Config{}, daily, true);
  CHECK(on.first == true);                                // the stop still applies: the halt stands

  auto other = build(Config{}, "order rejected by the exchange", false);
  CHECK(other.first == true);                             // an unrelated halt is never auto-cleared
  std::remove(path.c_str());
}

// The book is one switch over lanes that already exist. What matters is that it tells the truth
// about each lane, that one lane refusing does not keep the others dark, and that it will not
// start anything while the kill switch is on.
static void test_book_two_lanes() {
  const std::string path = "/tmp/desk_test_book.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  KalshiClient client("https://example.invalid/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);

  json b = eng.book();
  CHECK(b["lanes"].size() == 3);
  std::set<std::string> names;
  for (const auto& l : b["lanes"]) names.insert(l.value("name", ""));
  CHECK(names.count("systematic") && names.count("pairs") && names.count("perps"));
  // Nothing is armed in this fixture, so every lane must say why rather than look merely "off".
  for (const auto& l : b["lanes"])
    if (!l.value("available", true)) CHECK(!l.value("unavailable_reason", "").empty());
  CHECK(b["running"] == false && b["lanes_on"] == 0);
  // Lane 2 is counted from a screen, not invented: before any screen exists it is unknown (null),
  // never a reassuring zero. (It used to read a view with no screen in it and always said 0.)
  CHECK(b.contains("for_you") && b.contains("blocked") && b["for_you"].is_null() && b["blocked"].is_null());

  // A malformed body is refused rather than interpreted.
  bool threw = false;
  try { eng.book_configure(json::object()); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { eng.book_configure(json{{"running", "yes"}}); } catch (const std::exception&) { threw = true; }
  CHECK(threw);

  // Starting reports per lane. Lanes that cannot arm refuse; the call still succeeds, because a
  // missing Polymarket key must not keep weather and crypto switched off.
  json r = eng.book_configure(json{{"running", true}});
  CHECK(r.contains("changed") && r.contains("refused"));
  CHECK(r["changed"].size() + r["refused"].size() == 3);

  json off = eng.book_configure(json{{"running", false}});
  CHECK(off["running"] == false && off["lanes_on"] == 0);

  // The kill switch outranks the switch.
  eng.set_kill(true, "test");
  threw = false;
  std::string why;
  try { eng.book_configure(json{{"running", true}}); }
  catch (const std::exception& e) { threw = true; why = e.what(); }
  CHECK(threw && why.find("kill switch") != std::string::npos);
  CHECK(eng.book()["kill"] == true);
  std::remove(path.c_str());
}

// The calibration gate decides which strategies may be sized by Kelly and which are capped flat,
// so it has to count evidence honestly. It used to count logged fairs: one weather contract
// quoted every cycle for a day was a hundred samples sharing one outcome, which turned 107
// settled markets into 11,390 and let a 200-sample bar be cleared on a fraction of the evidence.
static void test_calibration_counts_markets_not_fairs() {
  const std::string path = "/tmp/desk_test_calib.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Market m; m.ticker = "T1"; m.event_ticker = "E1"; m.series_ticker = "S"; m.result = "yes";
  st.upsert_market(m);
  for (int i = 0; i < 50; ++i)
    st.exec("INSERT INTO model_fairs(ts,ticker,strategy,p,sigma,yes_bid,yes_ask) VALUES(?,?,?,?,?,?,?)",
            {static_cast<long long>(1000 + i), std::string("T1"), std::string("s"), 0.50, 0.05, 0.89, 0.91});
  Calibration c = strategy_calibration(st, "s", 1);
  CHECK(c.n == 1);                    // one market, however many times it was quoted
  CHECK(!c.demonstrated);             // and one market demonstrates nothing

  Market m2; m2.ticker = "T2"; m2.event_ticker = "E2"; m2.series_ticker = "S"; m2.result = "no";
  st.upsert_market(m2);
  for (int i = 0; i < 10; ++i)
    st.exec("INSERT INTO model_fairs(ts,ticker,strategy,p,sigma,yes_bid,yes_ask) VALUES(?,?,?,?,?,?,?)",
            {static_cast<long long>(2000 + i), std::string("T2"), std::string("s"), 0.02, 0.05, 0.60, 0.62});
  Calibration c2 = strategy_calibration(st, "s", 1);
  CHECK(c2.n == 2);
  // The model is ahead on both, yet two markets cannot carry an interval clear of zero.
  CHECK(c2.diff < 0);
  CHECK(c2.diff_lo <= c2.diff && c2.diff <= c2.diff_hi);
  CHECK(c2.diff_hi > 0 && !c2.demonstrated);
  // Below the market threshold it says so rather than passing.
  CHECK(!strategy_calibration(st, "s", 999).demonstrated);
  std::remove(path.c_str());
}


// Executor::cancel marks a working proposal cancel_pending before it calls the exchange. The state
// machine used to refuse submitted -> cancel_pending, so the move was dropped (and logged as an
// illegal transition, 16 times on 2026-09-10/11) and the proposal went on claiming it rested.
static void test_cancel_pending_transitions() {
  CHECK(Store::proposal_transition_ok("submitted", "cancel_pending"));
  CHECK(Store::proposal_transition_ok("unknown", "cancel_pending"));
  CHECK(Store::proposal_transition_ok("cancel_pending", "canceled"));
  CHECK(Store::proposal_transition_ok("cancel_pending", "filled"));      // a fill that raced the cancel
  CHECK(!Store::proposal_transition_ok("cancel_pending", "pending"));
  CHECK(!Store::proposal_transition_ok("cancel_pending", "submitted"));
  for (const char* term : {"filled", "canceled", "expired", "failed", "rejected"})
    CHECK(!Store::proposal_transition_ok(term, "cancel_pending"));
  Store st(":memory:");
  Proposal p;
  p.ticker = "KXT-E-B0"; p.event_ticker = "KXT-E"; p.series_ticker = "KXT"; p.strategy = "test_model";
  p.mode = "manual"; p.outcome_side = "yes"; p.exec_style = "maker"; p.status = "pending"; p.price = 0.40; p.count = 3;
  p.id = st.insert_proposal(p);
  p.status = "submitted"; p.order_id = "o-1"; st.update_proposal(p);
  p.status = "cancel_pending"; st.update_proposal(p);
  CHECK(st.get_proposal(p.id)->status == "cancel_pending");
  // Still the open proposal for its ticker/side/strategy, so the engine does not stack a second
  // order behind a cancel whose outcome is not known yet.
  auto open = st.find_open_proposal("KXT-E-B0", "yes", "test_model");
  CHECK(open && open->id == p.id && open->status == "cancel_pending");
  p.status = "canceled"; st.update_proposal(p);
  CHECK(st.get_proposal(p.id)->status == "canceled");
  CHECK(st.query("SELECT msg FROM events_log WHERE msg LIKE '%illegal proposal transition%'").empty());
}

// Routes are strings ("auto" | "manual" | "research" | "off"). Book capture read them as booleans
// and threw on every cycle from 2026-09-11 13:05 UTC (last observation 13:04:38). Capture follows
// the route, not execution: manual and research need the same live book as auto; off needs none.
// Also here: the one freshness limit, which the board states and applies, and the Book panel's
// lane 2, which must come from a screen rather than read zero.
static void test_route_capture_and_freshness() {
  const std::string path = "/tmp/desk_test_routes.sqlite";
  std::remove(path.c_str());
  Store st(path);
  Config cfg;
  cfg.bankroll_dollars = 100;
  cfg.strategy_modes["test_model"] = "research";
  cfg.watch_series = {"KXT"};
  cfg.market_data_max_age_seconds = 20;
  seed_kxt(st, 0.10, 0.12);
  KalshiClient client("https://127.0.0.1:1/trade-api/v2", nullptr, "t");
  Executor ex(cfg, st, client);
  Engine eng(cfg, st, client, ex);
  auto ts = std::make_unique<TestStrategy>();
  TestStrategy* tsp = ts.get();
  ts->f.p = 0.80;   // an edge over the floor: the case whose route lookup threw
  ts->f.sigma = 0.02;
  eng.add_strategy(std::move(ts));
  eng.cycle();
  CHECK(eng.book()["for_you"].is_number());          // the cycle's own decisions are counted
  auto has = [](const std::vector<std::string>& v, const std::string& t) { return std::find(v.begin(), v.end(), t) != v.end(); };
  std::vector<std::string> plan;
  bool threw = false;
  try { plan = eng.book_capture_plan(); } catch (const std::exception&) { threw = true; }
  CHECK(!threw);
  CHECK(has(plan, "KXT-E-B0") && has(plan, "KXT-E-B1"));   // crypto defaults to auto
  CHECK(eng.status().value("last_error", std::string()).find("books") == std::string::npos);
  for (const char* r : {"manual", "research"}) {
    eng.configure_systematic(json{{"settings", {{"crypto", r}}}});
    CHECK(has(eng.book_capture_plan(), "KXT-E-B0"));   // same book for a person as for the algorithm
  }
  eng.configure_systematic(json{{"settings", {{"crypto", "off"}}}});
  CHECK(!has(eng.book_capture_plan(), "KXT-E-B0"));    // nobody is looking: no request spent
  eng.cycle();
  CHECK(eng.status().value("last_error", std::string()).find("books") == std::string::npos);
  // One freshness limit, stated by the board and applied to every row.
  json b = eng.board_view(0);
  CHECK(b.value("market_data_max_age_seconds", 0) == 20);
  st.exec("UPDATE markets SET updated_ts=?", {now_s() - 21});
  b = eng.board_view(0);
  CHECK(!b["markets"].empty());
  for (const auto& r : b["markets"]) CHECK(r.value("quote_stale", false) == true);
  st.exec("UPDATE markets SET updated_ts=?", {now_s() - 5});
  b = eng.board_view(0);
  for (const auto& r : b["markets"]) CHECK(r.value("quote_stale", true) == false);
  // The strategy engine reads the same number: a 21 s old quote (fresh under the old 25 s rule)
  // gets no new model estimate on a 20 s desk, and the same quote 5 s old does.
  tsp->f.p = 0.30;   // inside the disagreement gate, with edge against the 0.12 offer
  eng.set_mode("test_model", "manual");
  auto fair_ts = [&] {
    const json board = eng.board_view(0);
    for (const auto& r : board["markets"])
      if (r.value("ticker", "") == "KXT-E-B0") return r.value("fair_ts", 0LL);
    return -1LL;
  };
  const long long before = fair_ts();
  st.exec("UPDATE markets SET updated_ts=?", {now_s() - 21});
  eng.cycle();
  CHECK(fair_ts() == before);
  st.exec("UPDATE markets SET updated_ts=?", {now_s() - 5});
  const long long t1 = now_s();
  eng.cycle();
  CHECK(fair_ts() >= t1);
  CHECK(st.proposals("pending", 10).empty());   // and a model entry is never a proposal now
  // Lane 2 is counted from the screen once one exists.
  eng.configure_systematic(json{{"settings", {{"crypto", "manual"}}}});
  st.exec("UPDATE markets SET updated_ts=?", {now_s() - 5});
  eng.systematic_view(10);
  json bk = eng.book();
  CHECK(bk["for_you"].is_number() && bk["blocked"].is_number());
  CHECK(bk["blocked"].get<int>() > 0);               // these rows fail gates (no settlement time)
  CHECK(bk.value("screen_ts", 0LL) > 0);
  std::remove(path.c_str());
}

// A ticker whose book read fails for a slow cycle keeps its captured depth for management. The
// capture is past the entry limit, so nothing new opens on it, but inside the management limit,
// which is where a stop reads whether the bid is deep enough to hit. Before this held, one failed
// read dropped the capture, zeroed the depth, and a stop that should hit the bid rested instead.
static void test_research_board_keeps_manageable_capture() {
  httplib::Server server;
  std::atomic<bool> book_ok{true};
  const std::string expiry = iso_of(now_s() + 5 * 3600);
  auto mk = [&](const std::string& t) {
    return json{{"ticker", t}, {"event_ticker", "KXT-E"}, {"series_ticker", "KXT"}, {"status", "active"},
                {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiry},
                {"yes_bid_dollars", "0.1000"}, {"yes_ask_dollars", "0.1200"}, {"yes_bid_size_fp", "500.00"}, {"yes_ask_size_fp", "500.00"}};
  };
  server.Get("/trade-api/v2/markets", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"markets", json::array({mk("KXT-E-B0")})}, {"cursor", ""}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/markets/([^/]+)/orderbook)", [&](const httplib::Request&, httplib::Response& r) {
    if (!book_ok) { r.status = 404; r.set_content(R"({"error":{"code":"not_found"}})", "application/json"); return; }
    r.set_content(R"({"orderbook_fp":{"yes_dollars":[["0.1000","250.00"]],"no_dollars":[["0.8800","300.00"]]}})", "application/json");
  });
  server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
    r.set_content(json{{"market", mk(q.matches[1].str())}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"series":{"ticker":"KXT","category":"Crypto","fee_type":"quadratic","fee_multiplier":1}})", "application/json");
  });
  server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"series":[],"cursor":""})", "application/json");
  });
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread thread([&] { server.listen_after_bind(); });
  const std::string path = "/tmp/desk_test_capture_age.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "research";
    cfg.watch_series = {"KXT"};
    cfg.market_data_max_age_seconds = 1;   // entry limit 1 s; the management limit stays 60 s
    Series sr{"KXT", "Crypto", "", "quadratic", "hourly", 1.0};
    st.upsert_series(sr);
    KalshiClient client("http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2", nullptr, "t");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    ts->f.p = 0.80;
    ts->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    auto yes_row = [&]() {
      json v = eng.systematic_view(0);
      for (const auto& r : v["screen"]["rows"])
        if (r.value("ticker", "") == "KXT-E-B0" && r.value("side", "") == "yes") return r;
      return json();
    };
    eng.cycle();
    json first = yes_row();
    CHECK(first.is_object());
    CHECK(std::fabs(first.value("bid_size", 0.0) - 250) < 1e-9);   // the captured book, not the list's 500
    book_ok = false;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    eng.cycle();   // the market row is refreshed, the book read fails: the capture is ~3 s old
    json second = yes_row();
    CHECK(second.is_object());
    CHECK(std::fabs(second.value("bid_size", 0.0) - 250) < 1e-9);  // still the captured depth
    CHECK(std::fabs(second.value("ask_size", 0.0) - 300) < 1e-9);
    CHECK(now_s() - second.value("quote_ts", 0LL) >= 2);           // with the capture's own time
    bool entry_stale = false;
    for (const auto& why : second.value("reasons", json::array()))
      if (why.is_string() && why.get<std::string>().find("quote missing or stale") != std::string::npos) entry_stale = true;
    CHECK(entry_stale);                                             // and nothing new opens on it
    CHECK(second.value("verdict", std::string()) == "blocked");
  } catch (const std::exception& e) {
    std::cerr << "capture age test error: " << e.what() << "\n";
    CHECK(false);
  }
  server.stop();
  thread.join();
  std::remove(path.c_str());
}

// The write watermark after a refused entry. Kalshi's own 4xx (its {"error":{...}} body) means
// nothing was written, so the old watermark is put back and the next account read does not wait
// for a write that never happened. A 4xx without that body (a proxy's page) or a 408 does not
// prove it, so the watermark stays.
static void test_refusal_watermark() {
  std::string key = "/tmp/desk_watermark_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + key + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip watermark test: openssl CLI not available\n");
    return;
  }
  auto signer = std::make_shared<Signer>("test", key);
  httplib::Server server;
  std::mutex mu;
  std::atomic<int> writes{0};
  long status = 400;
  std::string body;
  const std::string expiry = iso_of(now_s() + 5 * 3600);
  server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
    json mk = {{"ticker", q.matches[1].str()}, {"event_ticker", "KXT-E"}, {"series_ticker", "KXT"}, {"status", "active"},
               {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiry},
               {"yes_bid_dollars", "0.1000"}, {"yes_ask_dollars", "0.1200"}, {"yes_bid_size_fp", "500.00"}, {"yes_ask_size_fp", "500.00"}};
    r.set_content(json{{"market", mk}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"series":{"ticker":"KXT","category":"Crypto","fee_type":"quadratic","fee_multiplier":1}})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}, {"updated_ts", now_s() - 3600}}.dump(), "application/json");
  });
  server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"market_positions":[]})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[]})", "application/json");
  });
  server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
  });
  server.Post("/trade-api/v2/portfolio/events/orders", [&](const httplib::Request&, httplib::Response& r) {
    std::lock_guard<std::mutex> lk(mu);
    ++writes;
    r.status = static_cast<int>(status);
    r.set_content(body, "application/json");
  });
  server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})", "application/json");
  });
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread thread([&] { server.listen_after_bind(); });
  const std::string path = "/tmp/desk_test_watermark.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.risk.max_entry_horizon_hours = 48;
    seed_kxt(st, 0.10, 0.12);
    st.exec("UPDATE markets SET expected_expiration_time=?", {expiry});
    KalshiClient client("http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2", signer, "tests");
    Executor ex(cfg, st, client);
    OrderRequest req;
    req.ticker = "KXT-E-B0";
    req.outcome_side = "yes";
    req.price = 0.11;
    req.count = 1;
    req.post_only = true;
    auto refused = [&](long s, const std::string& b) -> std::string {
      { std::lock_guard<std::mutex> lk(mu); status = s; body = b; }
      st.kv_set("last_write_ts", "1000");
      const int before = writes;
      try { ex.ticket(req, "watermark"); CHECK(false); }
      catch (const ApiError& e) { CHECK(e.status == s); }
      catch (const std::exception& e) { std::cerr << "watermark test: " << e.what() << "\n"; CHECK(false); }
      CHECK(writes == before + 1);   // the order really went to the (test) exchange
      return st.kv_get("last_write_ts").value_or("");
    };
    CHECK(refused(400, R"({"error":{"code":"invalid_order","message":"refused by test exchange"}})") == "1000");
    CHECK(refused(400, "<html><body>400 Bad Request</body></html>") != "1000");
    CHECK(refused(408, R"({"error":{"code":"request_timeout","message":"timed out"}})") != "1000");
  } catch (const std::exception& e) {
    std::cerr << "watermark test error: " << e.what() << "\n";
    CHECK(false);
  }
  server.stop();
  thread.join();
  std::remove(key.c_str());
  std::remove(path.c_str());
}

// ---- the canonical pipeline, end to end (session 15) ----------------------------------------------
// A small exchange: KXT and KXU series (Crypto), two brackets each, books with depth, an account,
// and an order endpoint that rests maker orders and fills takers. Prices move when a test says so.
struct PipelineExchange {
  httplib::Server server;
  std::mutex mu;
  std::map<std::string, std::pair<double, double>> px;   // ticker -> (yes_bid, yes_ask)
  std::map<std::string, json> resting;                    // order_id -> order
  int writes = 0, deletes = 0, next_id = 1;
  json last_order;
  int port = 0;
  std::thread thread;
  const std::string expiry = iso_of(now_s() + 5 * 3600);
  static std::string d4(double x) { char b[16]; std::snprintf(b, sizeof b, "%.4f", x); return b; }
  json market(const std::string& t) {
    const auto [bid, ask] = px[t];
    const std::string series = t.substr(0, t.find('-'));
    return json{{"ticker", t}, {"event_ticker", t.substr(0, t.rfind('-'))}, {"series_ticker", series}, {"status", "active"},
                {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiry},
                {"yes_bid_dollars", d4(bid)}, {"yes_ask_dollars", d4(ask)}, {"no_bid_dollars", d4(1 - ask)},
                {"no_ask_dollars", d4(1 - bid)}, {"yes_bid_size_fp", "300.00"}, {"yes_ask_size_fp", "300.00"},
                {"rules_primary", "test contract"}};
  }
  PipelineExchange() {
    for (const char* t : {"KXT-E-B0", "KXT-E-B1", "KXU-F-B0", "KXU-F-B1"}) px[t] = {0.60, 0.62};
    server.Get("/trade-api/v2/markets", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      const std::string series = q.get_param_value("series_ticker");
      json ms = json::array();
      for (const auto& [t, v] : px) if (series.empty() || t.rfind(series + "-", 0) == 0) ms.push_back(market(t));
      r.set_content(json{{"markets", ms}, {"cursor", ""}}.dump(), "application/json");
    });
    server.Get(R"(/trade-api/v2/markets/([^/]+)/orderbook)", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      const auto [bid, ask] = px[q.matches[1].str()];
      r.set_content(json{{"orderbook_fp", {{"yes_dollars", json::array({json::array({d4(bid), "250.00"})})},
                                           {"no_dollars", json::array({json::array({d4(1 - ask), "300.00"})})}}}}.dump(),
                    "application/json");
    });
    server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"market", market(q.matches[1].str())}}.dump(), "application/json");
    });
    server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      r.set_content(json{{"series", {{"ticker", q.matches[1].str()}, {"category", "Crypto"}, {"fee_type", "quadratic"},
                                     {"fee_multiplier", 1}}}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}, {"updated_ts", now_s() - 3600}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"market_positions":[]})", "application/json");
    });
    server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      json os = json::array();
      if (q.get_param_value("status") == "resting" || q.get_param_value("status").empty())
        for (const auto& [id, o] : resting) os.push_back(o);
      r.set_content(json{{"orders", os}, {"cursor", ""}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
    });
    server.Post("/trade-api/v2/portfolio/events/orders", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++writes;
      last_order = json::parse(q.body);
      const std::string id = "po-" + std::to_string(next_id++);
      const bool post_only = last_order.value("post_only", false);
      const std::string count = last_order.value("count", std::string("1.00"));
      if (post_only) {
        const std::string side = last_order.value("side", std::string("bid")) == "bid" ? "yes" : "no";
        resting[id] = {{"order_id", id}, {"ticker", last_order.value("ticker", std::string())}, {"status", "resting"},
                       {"outcome_side", side}, {"book_side", last_order.value("side", std::string("bid"))},
                       {"yes_price_dollars", last_order.value("price", std::string("0.5000"))},
                       {"no_price_dollars", d4(1 - std::stod(last_order.value("price", std::string("0.5"))))},
                       {"initial_count_fp", count}, {"fill_count_fp", "0.00"}, {"remaining_count_fp", count}};
        r.set_content(json{{"order_id", id}, {"fill_count", "0.00"}, {"remaining_count", count}}.dump(), "application/json");
      } else {
        r.set_content(json{{"order_id", id}, {"fill_count", count}, {"remaining_count", "0.00"}}.dump(), "application/json");
      }
    });
    server.Delete(R"(/trade-api/v2/portfolio/events/orders/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++deletes;
      resting.erase(q.matches[1].str());
      r.set_content(json{{"order_id", q.matches[1].str()}, {"reduced_by", "1.00"}}.dump(), "application/json");
    });
    server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"series":[],"cursor":""})", "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([&] { server.listen_after_bind(); });
  }
  ~PipelineExchange() { server.stop(); thread.join(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2"; }
  void move(const std::string& t, double bid, double ask) { std::lock_guard<std::mutex> lk(mu); px[t] = {bid, ask}; }
};

// A second model, on the KXU series, for the lane the first one is not in.
struct TestAutoStrategy : public TestStrategy {
  std::string name() const override { return "test_auto_model"; }
  bool covers(const Market& m, const Series&) const override { return m.series_ticker == "KXU"; }
};

// DONE-WHEN 4: one cycle produces MANUAL and AUTO decisions at once, from one router, and the
// automatic lane takes its AUTO row in that same cycle while the MANUAL rows wait for you. The
// category (crypto) is on auto for both: it is the outer permission, and it does not promote the
// model that is routed manual.
static void test_one_cycle_both_lanes() {
  PipelineExchange x;
  const std::string path = "/tmp/desk_test_lanes.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.strategy_modes["test_auto_model"] = "bot";
    cfg.watch_series = {"KXT", "KXU"};
    KalshiClient client(x.base(), nullptr, "tests");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto manual_model = std::make_unique<TestStrategy>();
    manual_model->f.p = 0.80;   // 0.705 after the blend, against a 0.62 offer
    manual_model->f.sigma = 0.02;
    auto auto_model = std::make_unique<TestAutoStrategy>();
    auto_model->f.p = 0.80;
    auto_model->f.sigma = 0.02;
    eng.add_strategy(std::move(manual_model));
    eng.add_strategy(std::move(auto_model));
    eng.configure_systematic(json{{"mode", "paper"}, {"running", true},
                                  {"settings", {{"crypto", "auto"}, {"max_model_gap", 0.25},
                                                {"model_routes", {{"test_model_v1", "manual"}, {"test_auto_model_v1", "auto"}}},
                                                {"graduated_models", json::array({"test_auto_model_v1"})}}}});
    eng.cycle();
    const json v = eng.decisions_view(true, 0);
    std::set<std::string> auto_tickers, manual_tickers;
    long long gen = v.value("generated_ts", 0LL);
    for (const auto& r : v["rows"]) {
      if (r["route"] == "auto") auto_tickers.insert(r["ticker"].get<std::string>());
      if (r["route"] == "manual") manual_tickers.insert(r["ticker"].get<std::string>());
    }
    CHECK(gen > 0);
    CHECK(auto_tickers.count("KXU-F-B0") || auto_tickers.count("KXU-F-B1"));
    CHECK(manual_tickers.size() == 1 && (manual_tickers.count("KXT-E-B0") || manual_tickers.count("KXT-E-B1")));   // one row an event
    for (const auto& t : auto_tickers) CHECK(t.rfind("KXU-", 0) == 0);
    for (const auto& t : manual_tickers) CHECK(t.rfind("KXT-", 0) == 0);
    // The paper lane took its AUTO decision this cycle; nothing was opened on the MANUAL rows.
    const json sys = eng.systematic_view(0);
    int paper_kxu = 0, paper_kxt = 0;
    for (const auto& pos : sys["positions"]) {
      const std::string t = pos.value("ticker", "");
      if (t.rfind("KXU-", 0) == 0) ++paper_kxu;
      if (t.rfind("KXT-", 0) == 0) ++paper_kxt;
    }
    CHECK(paper_kxu == 1 && paper_kxt == 0);
    // Both lanes' rows were snapshotted as offered, with the route they had.
    CHECK(!st.query("SELECT id FROM trade_decisions WHERE event='offered' AND route='auto' AND ticker LIKE 'KXU-%'").empty());
    CHECK(!st.query("SELECT id FROM trade_decisions WHERE event='offered' AND route='manual' AND ticker LIKE 'KXT-%'").empty());
    // The paper lane's entry is on record against the very decision it took, and only that one.
    const json took = st.query("SELECT decision_id, ticker FROM trade_decisions WHERE event='auto'");
    CHECK(took.size() == 1 && took[0].value("ticker", "").rfind("KXU-", 0) == 0);
    if (!took.empty())
      CHECK(!st.query("SELECT id FROM trade_decisions WHERE event='offered' AND decision_id=?", {took[0].value("decision_id", "")}).empty());
    // The Book panel's lane 2 is this cycle's MANUAL decisions.
    CHECK(eng.book()["for_you"].is_number() && eng.book()["for_you"].get<int>() >= 1);
    json dv = eng.decisions_view(false, 0);
    CHECK(dv["algorithm"].value("running", false) && dv["algorithm"].value("mode", "") == "paper" && dv["algorithm"].value("auto_armed", false));
    // Nothing real was sent, and a model entry is never a proposal.
    CHECK(x.writes == 0);
    CHECK(st.proposals("pending", 20).empty());
  } catch (const std::exception& e) {
    std::cerr << "one cycle both lanes test error: " << e.what() << "\n";
    CHECK(false);
  }
  std::remove(path.c_str());
}

// A request whose model estimate is only a little old when the queue reaches it (the queue's first
// pass of a cycle runs before the strategies step) waits for the next estimate instead of failing
// for good; the estimate arrives in the same cycle and the order goes, re-checked on it.
static void test_manual_request_waits_for_fresh_estimate() {
  std::string key = "/tmp/desk_wait_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + key + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip estimate-wait test: openssl CLI not available\n");
    return;
  }
  PipelineExchange x;
  const std::string path = "/tmp/desk_test_wait.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.reconcile_every = 1;
    cfg.market_data_max_age_seconds = 3;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    KalshiClient client(x.base(), std::make_shared<Signer>("test", key), "tests");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    ts->f.p = 0.80;
    ts->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    route_test_model(eng, "manual");
    eng.configure_systematic(json{{"settings", {{"max_model_gap", 0.25}}}});
    eng.cycle();
    json shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    json req = {{"client_request_id", "0b8f3c0e-1d2a-4c55-9e0f-00000000a001"}, {"ticker", "KXT-E-B0"}, {"outcome_side", "yes"},
                {"price", shown.value("price", 0.0)}, {"count", 1}, {"post_only", true}, {"reviewed_ts", now_s()},
                {"decision_id", shown.value("decision_id", "")}};
    eng.manual_orders().enqueue(req, now_s());
    std::this_thread::sleep_for(std::chrono::milliseconds(4200));   // the estimate is now past the 3 s limit
    eng.cycle();
    json r = st.query("SELECT status, error FROM manual_requests WHERE id=?", {req["client_request_id"].get<std::string>()});
    CHECK(!r.empty() && r[0]["status"] == "submitted");
    CHECK(x.writes == 1);
  } catch (const std::exception& e) {
    std::cerr << "estimate-wait test error: " << e.what() << "\n";
    CHECK(false);
  }
  std::remove(path.c_str());
  std::remove(key.c_str());
}

// DONE-WHEN 7 and 8, and the manual lane's revalidation (F): a MANUAL decision is shown, the click
// names it, and the backend rechecks it against the desk as it is when the order goes. A moved
// market or a vanished edge is refused with what changed; a valid one is sent on the fresh
// decision, and the order leads back to the probability and the book that caused it. Then the
// kill switch still cancels what rests (G).
static void test_manual_revalidation_and_trace() {
  std::string key = "/tmp/desk_reval_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + key + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip revalidation test: openssl CLI not available\n");
    return;
  }
  PipelineExchange x;
  const std::string path = "/tmp/desk_test_reval.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.reconcile_every = 1;
    cfg.strategy_modes["test_model"] = "manual";
    cfg.watch_series = {"KXT"};
    KalshiClient client(x.base(), std::make_shared<Signer>("test", key), "tests");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    TestStrategy* tsp = ts.get();
    tsp->f.p = 0.80;
    tsp->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    route_test_model(eng, "manual");
    eng.configure_systematic(json{{"settings", {{"max_model_gap", 0.25}}}});
    eng.cycle();
    json shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual" && shown["style"] == "maker");
    const std::string shown_id = shown.value("decision_id", "");
    const double shown_price = shown.value("price", 0.0);
    int uuid = 0;
    auto request = [&](const std::string& decision_id, double price) {
      char id[40];
      std::snprintf(id, sizeof id, "0b8f3c0e-1d2a-4c55-9e0f-%012d", ++uuid);
      return json{{"client_request_id", id}, {"ticker", "KXT-E-B0"}, {"outcome_side", "yes"}, {"price", price},
                  {"count", 1}, {"post_only", true}, {"reviewed_ts", now_s()}, {"decision_id", decision_id}};
    };
    auto status_of = [&](const json& req) {
      json r = st.query("SELECT status, error FROM manual_requests WHERE id=?", {req["client_request_id"].get<std::string>()});
      return r.empty() ? json() : r[0];
    };
    // A decision that was never on the board is refused at the click.
    bool refused = false;
    try { eng.manual_orders().enqueue(request("d0123456789abcdef", shown_price), now_s()); }
    catch (const std::exception& e) { refused = std::string(e.what()).find("no longer on the board") != std::string::npos; }
    CHECK(refused);
    // The order dialog's numbers come from the backend: the decision you are reviewing, priced at
    // the dialog's price and style by the same edge engine, with the floor it has to clear.
    json sz = eng.size_suggestion("KXT-E-B0", "yes", shown_price, "maker", shown_id);
    CHECK(sz["decision"].is_object() && sz["decision"].value("found", false) && sz["decision"].value("route", "") == "manual");
    CHECK(sz["edge"].is_object() && std::fabs(sz["edge"].value("executable_edge", 0.0) - shown.value("executable_edge", 0.0)) < 1e-3);
    CHECK(std::fabs(sz["edge"].value("fair_probability", 0.0) - shown.value("p_win", 0.0)) < 1e-3);
    CHECK(std::fabs(sz["edge"].value("uncertainty_buffer", 0.0) - shown.value("uncertainty_buffer", 0.0)) < 1e-3);
    CHECK(sz["decision"].value("clears_floor", false) && sz.value("price", 0.0) == shown_price && sz.value("style", "") == "maker");
    json dear = eng.size_suggestion("KXT-E-B0", "yes", 0.70, "maker", shown_id);   // most of the edge paid away
    CHECK(dear["edge"].value("executable_edge", 1.0) < 0.05 && !dear["decision"].value("clears_floor", true));
    json gone = eng.size_suggestion("KXT-E-B0", "yes", shown_price, "maker", "d0123456789abcdef");
    CHECK(!gone["decision"].value("found", true) && gone["edge"].is_null());
    json plain = eng.size_suggestion("KXT-E-B0", "yes", shown_price, "maker");   // a hand ticket: latest quote, stored book
    CHECK(plain["decision"].is_null() && plain["edge"].is_object() && plain["edge"].value("model", "") == "test_model_v1");
    // A ticket priced past the edge is refused at the click, on the numbers you were shown.
    bool floor_refused = false;
    try { eng.manual_orders().enqueue(request(shown_id, 0.70), now_s()); }
    catch (const std::exception& e) { floor_refused = std::string(e.what()).find("manual floor") != std::string::npos; }
    CHECK(floor_refused);
    CHECK(st.query("SELECT id FROM manual_requests").empty() && x.writes == 0);
    // The market moves after you looked: the model is now 0.34 from the mid, past the desk's gate.
    json moved = request(shown_id, shown_price);
    eng.manual_orders().enqueue(moved, now_s());
    x.move("KXT-E-B0", 0.45, 0.47);
    eng.cycle();   // the queue is worked first: a fresh read of the book, the router, the floor
    json ms = status_of(moved);
    CHECK(ms["status"] == "failed");
    CHECK(ms.value("error", "").find("no longer offered") != std::string::npos);
    CHECK(ms.value("error", "").find("nothing sent") != std::string::npos);
    CHECK(x.writes == 0);
    CHECK(!st.query("SELECT id FROM trade_decisions WHERE decision_id=? AND event='rejected'", {shown_id}).empty());
    // Back where it was, but the model itself has come down: at the price you reviewed the edge
    // after the fee, slippage and the uncertainty buffer is now under the manual floor.
    x.move("KXT-E-B0", 0.60, 0.62);
    eng.cycle();
    shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    const std::string id2 = shown.value("decision_id", "");
    tsp->f.p = 0.70;   // 0.655 after the blend
    eng.cycle();
    json vanished = request(id2, 0.61);
    eng.manual_orders().enqueue(vanished, now_s());
    eng.cycle();
    json vs = status_of(vanished);
    CHECK(vs["status"] == "failed");
    CHECK(vs.value("error", "").find("floor") != std::string::npos || vs.value("error", "").find("no longer offered") != std::string::npos);
    CHECK(x.writes == 0);
    // An order on an unproven model's number is capped, exactly as the old proposal lane capped
    // every model signal it sized: 2% of a $100 bankroll by default.
    tsp->f.p = 0.80;
    eng.cycle();
    shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    json big = request(shown.value("decision_id", ""), shown.value("price", 0.0));
    big["count"] = 5;   // about $3 against the $2 cap
    eng.manual_orders().enqueue(big, now_s());
    eng.cycle();
    json big_status = status_of(big);
    CHECK(big_status["status"] == "failed");
    CHECK(big_status.value("error", "").find("capped at") != std::string::npos);
    CHECK(x.writes == 0);
    // Ownership is checked again when the order goes: a working order that appears in the event
    // after you reviewed the row (placed elsewhere, in the Kalshi app say) takes the event, and the
    // request is refused with that reason. The click itself could not have known.
    tsp->f.p = 0.80;
    eng.cycle();
    shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    const std::string id_owned = shown.value("decision_id", "");
    json owned = request(id_owned, shown.value("price", 0.0));
    eng.manual_orders().enqueue(owned, now_s());
    {
      std::lock_guard<std::mutex> lk(x.mu);
      x.resting["elsewhere-1"] = {{"order_id", "elsewhere-1"}, {"ticker", "KXT-E-B1"}, {"status", "resting"},
                                  {"outcome_side", "yes"}, {"book_side", "bid"}, {"yes_price_dollars", "0.5500"},
                                  {"no_price_dollars", "0.4500"}, {"initial_count_fp", "1.00"}, {"fill_count_fp", "0.00"},
                                  {"remaining_count_fp", "1.00"}};
    }
    Order elsewhere;
    elsewhere.order_id = "elsewhere-1"; elsewhere.ticker = "KXT-E-B1"; elsewhere.outcome_side = "yes";
    elsewhere.book_side = "bid"; elsewhere.status = "resting"; elsewhere.yes_price = 0.55; elsewhere.no_price = 0.45;
    elsewhere.initial_count = 1; elsewhere.remaining_count = 1;
    st.upsert_order(elsewhere);
    eng.cycle();
    json own_status = status_of(owned);
    CHECK(own_status["status"] == "failed");
    CHECK(own_status.value("error", "").find("no longer offered to you (blocked)") != std::string::npos);
    CHECK(x.writes == 0);
    CHECK(!st.query("SELECT id FROM trade_decisions WHERE decision_id=? AND event='rejected'", {id_owned}).empty());
    {
      std::lock_guard<std::mutex> lk(x.mu);
      x.resting.erase("elsewhere-1");
    }
    st.exec("UPDATE orders SET status='canceled', remaining_count=0 WHERE order_id='elsewhere-1'");
    // The same for ownership only the router knows: an algorithm trade opened in the event after
    // your click (no order or position of yours shows it; the router's context does).
    eng.cycle();
    shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    json algo_owned = request(shown.value("decision_id", ""), shown.value("price", 0.0));
    eng.manual_orders().enqueue(algo_owned, now_s());
    const std::string sys_mode = eng.systematic_view(0).value("mode", std::string("live"));
    st.exec("INSERT INTO systematic_trades(ticker,event_ticker,side,state,mode,opened_ts) VALUES('KXT-E-B1','KXT-E','no','entering',?,?)",
            {sys_mode, now_s()});
    eng.cycle();
    json algo_status = status_of(algo_owned);
    CHECK(algo_status["status"] == "failed");
    CHECK(algo_status.value("error", "").find("already has a position or working trade") != std::string::npos);
    CHECK(x.writes == 0);
    st.exec("DELETE FROM systematic_trades WHERE ticker='KXT-E-B1'");
    // A valid click is sent, on the decision as it stands when it goes.
    tsp->f.p = 0.80;
    eng.cycle();
    shown = decision_row(eng, "KXT-E-B0", "yes");
    CHECK(shown.is_object() && shown["route"] == "manual");
    const std::string id3 = shown.value("decision_id", "");
    json good = request(id3, shown.value("price", 0.0));
    eng.manual_orders().enqueue(good, now_s());
    eng.cycle();
    json gs = status_of(good);
    CHECK(gs["status"] == "submitted");
    CHECK(x.writes == 1);
    // Every actual order leads back to a stored probability and book (DONE-WHEN 8).
    json orders = st.query("SELECT order_id, decision_id FROM orders WHERE order_id <> 'elsewhere-1'");
    CHECK(orders.size() == 1);
    if (!orders.empty()) {
      const std::string oid = orders[0].value("order_id", "");
      const std::string did = orders[0].value("decision_id", "");
      CHECK(!did.empty());   // the fresh decision the order was sent on (same id if nothing moved)
      json trace = eng.decision_trace(oid);
      CHECK(trace["proposal"].is_object() && trace["proposal"]["decision_id"] == did);
      std::set<std::string> events;
      for (const auto& r : trace["decisions"]) events.insert(r.value("event", ""));
      CHECK(events.count("revalidated") && events.count("submitted"));
      // ...and back to the row you clicked: the order went on a fresh decision (the book re-read
      // at the send), whose record names the decision you approved.
      CHECK(did == id3 || (trace.value("source_decision_id", "") == id3 && events.count("approved")));
      bool has_book = false, has_quote = false;
      for (const auto& r : trace["decisions"]) {
        has_book = has_book || (r.value("bid", 0.0) > 0 && r.value("ask", 0.0) > 0 && r.value("book_ts", 0LL) > 0);
        has_quote = has_quote || (r.value("p_yes", 0.0) > 0 && r.value("model_name", "") == "test_model" &&
                                  r.value("quote_json", "").find("p_yes") != std::string::npos);
      }
      CHECK(has_book && has_quote);
      // The row you clicked is on record as approved, pointing at the request.
      CHECK(!st.query("SELECT id FROM trade_decisions WHERE decision_id=? AND event='approved'", {id3}).empty());
      // A proposal still waiting to be sent owns its event, as a working order does.
      Proposal waiting;
      waiting.ticker = "KXU-F-B0";
      waiting.event_ticker = "KXU-F";   // a market this desk does not watch: the proposal names its event
      waiting.strategy = "ladder_arb";
      waiting.outcome_side = "yes";
      waiting.price = 0.60;
      waiting.count = 1;
      waiting.status = "pending";
      st.insert_proposal(waiting);
      const RouterContext ctx = eng.router_context(now_s());
      CHECK(ctx.held_events.count("KXU-F") && ctx.working_events.count("KXU-F"));
      // G: the kill switch still cancels what rests.
      const int deletes_before = x.deletes;
      eng.set_kill(true, "test");
      CHECK(x.deletes > deletes_before);
    }
  } catch (const std::exception& e) {
    std::cerr << "revalidation test error: " << e.what() << "\n";
    CHECK(false);
  }
  std::remove(path.c_str());
  std::remove(key.c_str());
}

// The proposal queue's unattended sender (auto_submit). Since session 15 it never sends a model's
// entry (that is the decision router's, and the algorithm's for an AUTO decision); what it still
// sends unattended - exits, brackets, locked basket legs - reads an exchange refusal as before: a
// post-only price the book moved through is the market moving (the proposal is spent, the desk keeps
// running); a malformed order is the desk being wrong (the kill switch trips).
static void test_auto_submit_soft_rejection() {
  std::string key = "/tmp/desk_soft_reject_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + key + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip auto-submit rejection test: openssl CLI not available\n");
    return;
  }
  auto signer = std::make_shared<Signer>("test", key);
  httplib::Server server;
  std::atomic<int> writes{0};
  std::mutex mu;
  std::string refusal = R"({"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})";
  std::string ask_dollars = "0.1200";
  const std::string expiry = iso_of(now_s() + 5 * 3600);
  server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> lk(mu);
    json mk = {{"ticker", q.matches[1].str()}, {"event_ticker", "KXT-E"}, {"series_ticker", "KXT"}, {"status", "active"},
               {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiry},
               {"yes_bid_dollars", "0.1000"}, {"yes_ask_dollars", ask_dollars}, {"yes_bid_size_fp", "500.00"}, {"yes_ask_size_fp", "500.00"}};
    r.set_content(json{{"market", mk}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"series":{"ticker":"KXT","category":"Crypto","fee_type":"quadratic","fee_multiplier":1}})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}, {"updated_ts", now_s() - 3600}}.dump(), "application/json");
  });
  std::string held;   // hold 1 YES here while non-empty (under mu)
  server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
    std::lock_guard<std::mutex> lk(mu);
    json a = json::array();
    if (!held.empty()) a.push_back({{"ticker", held}, {"position_fp", "1.00"}, {"market_exposure_dollars", "0.1100"}});
    r.set_content(json{{"market_positions", a}}.dump(), "application/json");
  });
  server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[]})", "application/json");
  });
  server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
  });
  server.Post("/trade-api/v2/portfolio/events/orders", [&](const httplib::Request&, httplib::Response& r) {
    std::lock_guard<std::mutex> lk(mu);
    ++writes;
    r.status = 400;
    r.set_content(refusal, "application/json");
  });
  server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})", "application/json");
  });
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread thread([&] { server.listen_after_bind(); });
  const std::string path = "/tmp/desk_test_soft_reject.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.risk.max_entry_horizon_hours = 48;
    cfg.strategy_modes["test_model"] = "bot";
    cfg.watch_series = {"KXT"};
    cfg.reconcile_every = 1;   // positions are read every cycle (the bracket case below holds one)
    cfg.strategy_modes["ladder_arb"] = "bot";
    seed_kxt(st, 0.10, 0.12);
    st.exec("UPDATE markets SET expected_expiration_time=?", {expiry});
    KalshiClient client("http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2", signer, "tests");
    Executor ex(cfg, st, client);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    ts->f.p = 0.30;   // a maker bid at 0.11 under a 0.12 offer, with edge
    ts->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    eng.set_auto_armed(true);
    // 1. A model in bot mode, with AUTO armed and an edge, sends nothing (session 15): its number
    //    is a quote, the router decides, and only the algorithm sends an AUTO decision. No entry
    //    proposal is made for it either.
    eng.cycle();
    eng.cycle();
    CHECK(writes == 0 && !eng.killed());
    for (const auto& p : st.proposals("", 50)) CHECK(p.strategy != "test_model");
    // A model's pending entry left from before is withdrawn, never sent, and cannot be approved.
    Proposal legacy;
    legacy.ticker = "KXT-E-B1"; legacy.event_ticker = "KXT-E"; legacy.series_ticker = "KXT"; legacy.strategy = "test_model";
    legacy.mode = "bot"; legacy.outcome_side = "yes"; legacy.exec_style = "maker"; legacy.status = "pending";
    legacy.price = 0.11; legacy.count = 1; legacy.net_edge = 0.18;
    legacy.id = st.insert_proposal(legacy);
    bool refused_approval = false;
    try { eng.approve(legacy.id); } catch (const std::exception& e) {
      refused_approval = std::string(e.what()).find("Trades board") != std::string::npos;
    }
    CHECK(refused_approval);
    eng.cycle();
    CHECK(writes == 0);
    auto withdrawn = st.get_proposal(legacy.id);
    CHECK(withdrawn && withdrawn->status == "expired" && withdrawn->note.find("decision router") != std::string::npos);
    // One the model never reaches (a market it does not cover, as for a strategy since switched
    // off) is not withdrawn by it; the queue itself refuses to send a model's entry, cycle after cycle.
    Proposal stray = legacy;
    stray.id = 0;
    stray.ticker = "KXQ-E-B0"; stray.event_ticker = "KXQ-E"; stray.series_ticker = "KXQ";
    stray.id = st.insert_proposal(stray);
    eng.cycle();
    eng.cycle();
    CHECK(writes == 0);
    auto still = st.get_proposal(stray.id);
    CHECK(still && still->status == "pending" && still->order_id.empty());
    st.exec("UPDATE proposals SET status='expired' WHERE id=?", {stray.id});
    // 2. What the queue still sends unattended is a locked basket leg, and a refusal on it is read
    //    the same way as before. First the executor's own read: the exchange's offer is already at
    //    the 0.11 bid, so the post-only price would cross. Nothing is written, nothing stops, the
    //    leg stays pending with the reason, and it gets a breather rather than a retry every cycle.
    auto leg = [&](const std::string& ticker) {
      Proposal p;
      p.ticker = ticker; p.event_ticker = "KXT-E"; p.series_ticker = "KXT"; p.strategy = "ladder_arb"; p.mode = "bot";
      p.outcome_side = "yes"; p.exec_style = "maker"; p.status = "pending"; p.price = 0.11; p.count = 1;
      p.net_edge = 0.02; p.note = "test basket | basket KXT-E@1 | leg yes " + ticker;
      p.id = st.insert_proposal(p);
      return p.id;
    };
    { std::lock_guard<std::mutex> lk(mu); ask_dollars = "0.1100"; }
    const long long first_leg = leg("KXT-E-B0");
    st.exec("UPDATE markets SET updated_ts=?", {now_s()});
    eng.cycle();
    CHECK(writes == 0 && !eng.killed());
    auto noted_leg = st.get_proposal(first_leg);
    CHECK(noted_leg && noted_leg->status == "pending" && noted_leg->note.find("would cross") != std::string::npos);
    const std::string backed_off = "KXT-E-B0";
    { std::lock_guard<std::mutex> lk(mu); ask_dollars = "0.1200"; }
    st.exec("UPDATE markets SET updated_ts=?", {now_s()});
    eng.cycle();
    CHECK(writes == 0);                               // inside the breather: not tried again yet
    // The breather holds back entries only. A stop on the same ticker goes out inside it: hold
    // one YES there with an auto bracket whose stop is above the bid, and the stop is sent.
    { std::lock_guard<std::mutex> lk(mu); held = backed_off; }
    st.exec("INSERT INTO brackets(ticker,tp_price,sl_price,trail,mode,created_ts,updated_ts,alerted) VALUES(?,0,0.5,0,'auto',?,?,'')",
            {backed_off, now_s(), now_s()});
    st.exec("UPDATE markets SET updated_ts=?", {now_s()});
    eng.cycle();
    bool stop_sent = false;
    for (const auto& p : st.proposals("failed", 20))
      if (p.strategy == "stop" && p.ticker == backed_off && p.note.find("exit refused") != std::string::npos) stop_sent = true;
    CHECK(stop_sent);                                 // it reached the exchange (which refused it)
    CHECK(writes == 1);                               // and no entry went with it
    CHECK(!eng.killed());
    { std::lock_guard<std::mutex> lk(mu); held.clear(); }
    st.exec("DELETE FROM brackets WHERE ticker=?", {backed_off});
    // Out of the breather the leg is sent; the exchange refuses it as a post-only cross: soft.
    const int before_entries = writes;
    st.exec("UPDATE markets SET updated_ts=?", {now_s()});
    eng.clear_soft_backoff_for_tests();
    eng.cycle();
    CHECK(writes > before_entries);
    CHECK(!eng.killed());                             // the market moved; the desk is fine
    auto refused_leg = st.get_proposal(first_leg);
    CHECK(refused_leg && refused_leg->status == "failed" && refused_leg->note.find("soft_market_state") != std::string::npos);
    // The exchange's own soft refusal earns the same breather: a new leg is not sent next cycle.
    const int refused_writes = writes;
    leg("KXT-E-B0");
    st.exec("UPDATE markets SET updated_ts=?", {now_s()});
    eng.cycle();
    CHECK(writes == refused_writes);
    // A malformed order is still the desk's fault, and still stops it.
    { std::lock_guard<std::mutex> lk(mu); refusal = R"({"error":{"code":"invalid_order","message":"count must be positive"}})"; }
    int before = writes;
    for (int i = 0; i < 3 && !eng.killed(); ++i) {
      st.exec("UPDATE markets SET updated_ts=?", {now_s()});
      eng.clear_soft_backoff_for_tests();
      eng.cycle();
    }
    CHECK(writes > before);
    CHECK(eng.killed());
    // Every order that went out carries a decision snapshot (decision_log.h).
    for (const auto& o : st.query("SELECT order_id, decision_id FROM orders")) CHECK(!o.value("decision_id", std::string()).empty());
    for (const auto& p : st.proposals("failed", 20))
      if (p.strategy == "ladder_arb" || p.strategy == "stop") CHECK(!p.decision_id.empty());
  } catch (const std::exception& e) {
    std::cerr << "auto-submit rejection test error: " << e.what() << "\n";
    CHECK(false);
  }
  server.stop();
  thread.join();
  std::remove(key.c_str());
  std::remove(path.c_str());
}

// A cancel that fails once must be sent again. Before the cancel_pending fix the failed cancel
// left the proposal "submitted" and the next pass re-sent it by accident; with the state recorded
// properly, the engine has to retry on purpose while the order still rests (Kalshi cancels are
// idempotent), or the order would sit on the book on a number the desk has stopped trusting.
static void test_failed_cancel_is_retried() {
  std::string key = "/tmp/desk_cancel_retry_key.pem";
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + key + " 2>/dev/null").c_str()) != 0) {
    std::fprintf(stderr, "skip cancel-retry test: openssl CLI not available\n");
    return;
  }
  auto signer = std::make_shared<Signer>("test", key);
  httplib::Server server;
  std::atomic<int> deletes{0};
  std::atomic<bool> resting{true};
  const std::string expiry = iso_of(now_s() + 5 * 3600);
  auto mk = [&](const std::string& t) {
    return json{{"ticker", t}, {"event_ticker", "KXT-E"}, {"series_ticker", "KXT"}, {"status", "active"},
                {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiry},
                {"yes_bid_dollars", "0.1000"}, {"yes_ask_dollars", "0.1200"}, {"yes_bid_size_fp", "500.00"}, {"yes_ask_size_fp", "500.00"}};
  };
  server.Get("/trade-api/v2/markets", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"markets", json::array({mk("KXT-E-B0"), mk("KXT-E-B1")})}, {"cursor", ""}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
    r.set_content(json{{"market", mk(q.matches[1].str())}}.dump(), "application/json");
  });
  server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"series":{"ticker":"KXT","category":"Crypto","fee_type":"quadratic","fee_multiplier":1}})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}}.dump(), "application/json");
  });
  server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"market_positions":[]})", "application/json");
  });
  server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request& q, httplib::Response& r) {
    json orders = json::array();
    if (q.get_param_value("status") == "resting" && resting)
      orders.push_back({{"order_id", "o-1"}, {"ticker", "KXT-E-B0"}, {"outcome_side", "yes"}, {"book_side", "bid"},
                        {"status", "resting"}, {"yes_price_dollars", "0.1100"}, {"no_price_dollars", "0.8900"},
                        {"initial_count_fp", "3.00"}, {"fill_count_fp", "0.00"}, {"remaining_count_fp", "3.00"}});
    r.set_content(json{{"orders", orders}, {"cursor", ""}}.dump(), "application/json");
  });
  server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
  });
  server.Delete(R"(/trade-api/v2/portfolio/events/orders/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
    int n = ++deletes;
    if (n == 1) { r.status = 503; r.set_content(R"({"error":{"code":"service_unavailable"}})", "application/json"); return; }
    resting = false;
    r.set_content(json{{"order_id", q.matches[1].str()}, {"reduced_by", "3.00"}}.dump(), "application/json");
  });
  server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
    r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})", "application/json");
  });
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread thread([&] { server.listen_after_bind(); });
  const std::string path = "/tmp/desk_test_cancel_retry.sqlite";
  std::remove(path.c_str());
  try {
    Store st(path);
    Config cfg;
    cfg.bankroll_dollars = 100;
    cfg.risk.max_entry_horizon_hours = 48;
    cfg.strategy_modes["test_model"] = "bot";
    cfg.watch_series = {"KXT"};
    Series sr{"KXT", "Crypto", "", "quadratic", "hourly", 1.0};
    st.upsert_series(sr);
    KalshiClient client("http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2", signer, "tests");
    Executor ex(cfg, st, client);
    // A bot proposal that reached the exchange and rests: 3 YES @ 0.11.
    Proposal p;
    p.ticker = "KXT-E-B0"; p.event_ticker = "KXT-E"; p.series_ticker = "KXT"; p.strategy = "test_model";
    p.mode = "bot"; p.outcome_side = "yes"; p.exec_style = "maker"; p.status = "pending"; p.price = 0.11; p.count = 3;
    p.id = st.insert_proposal(p);
    p.status = "submitted"; p.order_id = "o-1"; st.update_proposal(p);
    Order o; o.order_id = "o-1"; o.ticker = "KXT-E-B0"; o.outcome_side = "yes"; o.book_side = "bid"; o.type = "limit";
    o.status = "resting"; o.yes_price = 0.11; o.no_price = 0.89; o.initial_count = 3; o.remaining_count = 3;
    st.upsert_order(o, p.id);
    Engine eng(cfg, st, client, ex);
    auto ts = std::make_unique<TestStrategy>();
    ts->f.p = 0.30; ts->f.sigma = 0.02;
    eng.add_strategy(std::move(ts));
    eng.set_mode("test_model", "research");   // the number is no longer trusted: pull the resting order
    for (int i = 0; i < 4 && resting; ++i) eng.cycle();
    CHECK(deletes >= 2);                        // the failed cancel was sent again
    CHECK(!resting);                            // and the order is off the book
    auto back = st.get_proposal(p.id);
    CHECK(back && back->status == "canceled");
    auto ord = st.query("SELECT status FROM orders WHERE order_id='o-1'");
    CHECK(!ord.empty() && ord[0].value("status", std::string()) == "canceled");
  } catch (const std::exception& e) {
    std::cerr << "cancel-retry test error: " << e.what() << "\n";
    CHECK(false);
  }
  server.stop();
  thread.join();
  std::remove(key.c_str());
  std::remove(path.c_str());
}

int main() {
  test_cancel_pending_transitions();
  test_failed_cancel_is_retried();
  test_route_capture_and_freshness();
  test_research_board_keeps_manageable_capture();
  test_refusal_watermark();
  test_auto_submit_soft_rejection();
  test_one_cycle_both_lanes();
  test_manual_revalidation_and_trace();
  test_manual_request_waits_for_fresh_estimate();
  test_algo_owned_positions_are_left_alone();
  test_position_aware_entry_policy();
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
  test_nested_legs_are_not_a_hedge();
  test_resting_exit_outliving_its_position();
  test_exit_decision();
  test_sell_now_is_net_of_the_fee();
  test_order_states_and_cancel_race();
  test_fill_during_cancel();
  test_exit_order_shape();
  test_wx_distribution();
  test_kelly_needs_calibration();
  test_cash_reconciliation();
  test_replay();
  test_universe();
  test_match_phase();
  test_inplay_restart();
  test_daily_halt_clears_when_the_stop_is_off();
  test_book_two_lanes();
  test_calibration_counts_markets_not_fairs();
  test_auto_rules();
  test_instant_lane();
  test_fees();
  test_fixed();
  test_time();
  test_signing();
  test_order_body();
  test_parse_market();
  test_store();
  test_weather_math();
  test_bootstrap();
  std::printf("%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
