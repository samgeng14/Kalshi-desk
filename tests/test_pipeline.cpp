// The canonical probability pipeline (session 15): ProbabilityQuote -> edge engine -> decision
// router. Unit level: quotes (A), the edge engine and YES/NO price normalization (B), the router
// (C), the parallel lanes of one synthetic cycle (D) and event ownership (E). The engine-level
// versions (a real cycle, manual revalidation, order traceability) are in test_main.cpp.
#include <cmath>
#include <cstdio>
#include <string>

#include "desk/decision_router.h"
#include "desk/edge.h"
#include "desk/fees.h"
#include "desk/log.h"
#include "desk/probability.h"
#include "desk/systematic.h"
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

static Market market(const std::string& ticker, const std::string& series, double yb, double ya) {
  Market m;
  m.ticker = ticker;
  m.event_ticker = ticker.substr(0, ticker.rfind('-'));
  m.series_ticker = series;
  m.status = "active";
  m.yes_bid = yb;
  m.yes_ask = ya;
  m.no_bid = ya > 0 && ya < 1 ? 1 - ya : 0;
  m.no_ask = yb > 0 && yb < 1 ? 1 - yb : 0;
  return m;
}

static ProbabilityQuote quote(double p, double sigma, const std::string& model = "test_model") {
  ProbabilityQuote q;
  q.ticker = "T-1";
  q.event_ticker = "T";
  q.model_name = model;
  q.model_version = "v1";
  q.p_yes = p;
  q.sigma = sigma;
  q.model_ts = 1000;
  q.received_ts = 1001;
  q.inputs_valid = true;
  q.settlement_verified = true;
  q.auto_eligible = true;
  q.diagnostics = {{"p_raw", p}};
  return q;
}

// ---- A. ProbabilityQuote ----------------------------------------------------------------------
static void test_quote() {
  // Boundaries are part of the range; sigma may be zero.
  CHECK(quote_valid(quote(0.0, 0.0)));
  CHECK(quote_valid(quote(1.0, 0.1)));
  CHECK(quote_valid(quote(0.5, 0.0)));
  for (double bad : {-0.01, 1.01, double(NAN), double(INFINITY)}) CHECK(!quote_valid(quote(bad, 0.05)));
  CHECK(!quote_valid(quote(0.5, -0.01)));
  CHECK(!quote_valid(quote(0.5, NAN)));
  ProbabilityQuote noticker = quote(0.5, 0.1); noticker.ticker.clear();
  CHECK(!quote_valid(noticker));
  ProbabilityQuote nots = quote(0.5, 0.1); nots.model_ts = 0;
  CHECK(!quote_valid(nots));

  // The adapter wraps a model's Fair without touching its number, keeps its timestamps, its own
  // research verdict and its diagnostics, and marks an impossible estimate invalid.
  Market m = market("KXHIGHNY-26SEP11-B80", "KXHIGHNY", 0.60, 0.62);
  m.rules_primary = "settles on the National Weather Service daily climate report";
  Fair f;
  f.p = 0.6731;
  f.p_raw = 0.7012;
  f.sigma = 0.043;
  f.market_blend_weight = 0.5;
  f.note = "ensemble 21 members";
  f.inputs = {{"valid", true}, {"forecast_issued_ts", 900}, {"observation_ts", 950}, {"warnings", json::array({"w"})}};
  ProbabilityQuote q = quote_from_fair(m, "weather", "weather_nws", f, true, "same-day gate", 1000, 1002);
  CHECK(q.p_yes == f.p && q.sigma == f.sigma);          // model math unchanged: bit for bit
  CHECK(q.ticker == m.ticker && q.event_ticker == m.event_ticker && q.category == "weather");
  CHECK(q.model_name == "weather_nws" && q.model_version == "v1" && q.model_key() == "weather_nws_v1");
  CHECK(q.model_ts == 1000 && q.received_ts == 1002 && q.underlying_ts == 950);
  CHECK(q.diagnostics["source_timestamps"]["forecast_issued_ts"] == 900);
  CHECK(q.inputs_valid && q.auto_eligible);
  CHECK(q.research_only && q.research_reason == "same-day gate");
  CHECK_NEAR(q.p_raw(), 0.7012, 1e-12);
  CHECK(q.diagnostics["note"] == "ensemble 21 members" && q.diagnostics["market_blend_weight"] == 0.5);
  CHECK(q.diagnostics["inputs"]["warnings"].size() == 1);
  CHECK(q.settlement_verified);                          // NWS-settled, mapped station
  // Round trip: nothing lost on the way to the board and back.
  ProbabilityQuote back = quote_from_json(quote_to_json(q));
  CHECK(back.p_yes == q.p_yes && back.sigma == q.sigma && back.model_ts == q.model_ts &&
        back.underlying_ts == q.underlying_ts && back.received_ts == q.received_ts);
  CHECK(back.model_key() == q.model_key() && back.research_only && back.settlement_verified);
  CHECK(back.diagnostics == q.diagnostics);
  // A model output carries no execution parameters.
  const json qj = quote_to_json(q);
  for (const char* k : {"price", "count", "size", "style", "route", "limit", "take_profit", "stop_loss"})
    CHECK(!qj.contains(k));
  // An impossible estimate is explicitly invalid, and never auto-eligible.
  Fair bad = f;
  bad.p = 1.4;
  ProbabilityQuote qb = quote_from_fair(m, "weather", "weather_nws", bad, false, "", 1000, 1000);
  CHECK(!qb.inputs_valid && !qb.auto_eligible && qb.diagnostics.contains("invalid_reasons"));
  // Failed inputs and a manual-only source come through as the model said.
  Fair failed = f;
  failed.inputs = {{"valid", false}, {"reasons", json::array({"stale forecast"})}};
  CHECK(!quote_from_fair(m, "weather", "weather_nws", failed, false, "", 1000, 1000).inputs_valid);
  Fair manual_only = f;
  manual_only.inputs = {{"valid", true}, {"auto_eligible", false}, {"source_ts", 990}};
  ProbabilityQuote qm = quote_from_fair(m, "sports", "sports_live", manual_only, false, "", 1000, 1000);
  CHECK(qm.inputs_valid && !qm.auto_eligible && qm.underlying_ts == 990);
  // xvenue reports an age: the quote turns it into a timestamp.
  Fair xv = f;
  xv.inputs = {{"valid", true}, {"polymarket_quote_age_s", 7}};
  CHECK(quote_from_fair(m, "sports", "xvenue", xv, false, "", 1000, 1000).underlying_ts == 993);

  // Settlement verification, per model, from the evidence the desk has.
  Market twc = m;
  twc.rules_primary = "settles on The Weather Company's reported high";
  CHECK(!settlement_verified_for("weather_nws", twc));
  CHECK(!settlement_verified_for("weather_nws", market("KXHIGHHOU-26SEP11-B90", "KXHIGHHOU", .5, .52)));  // rules unread
  Market btc = market("KXBTC-26SEP1117-B60000", "KXBTC", .40, .42);
  CHECK(!settlement_verified_for("crypto_lognormal", btc));
  btc.rules_primary = "the average of CF Benchmarks' Bitcoin Real-Time Index";
  CHECK(settlement_verified_for("crypto_lognormal", btc));
  CHECK(settlement_verified_for("sports_inplay", market("KXBRASILEIROGAME-26SEP11FLAPAL-FLA", "KXBRASILEIROGAME", .4, .42)));
  CHECK(!settlement_verified_for("sports_inplay", market("KXEPLGAME-26SEP11ARSCHE-ARS", "KXEPLGAME", .4, .42)));
  CHECK(settlement_verified_for("sports_totals", btc) && settlement_verified_for("sports_live", btc));
  CHECK(settlement_verified_for("manual_fair", btc));
  CHECK(!settlement_verified_for("xvenue", btc) && !settlement_verified_for("unknown_model", btc));
  CHECK(model_version_for("weather_nws") == "v1");

  // A board row without a quote object is rebuilt, failing closed on settlement.
  json row = {{"ticker", "T-1"}, {"event_ticker", "T"}, {"fair_strategy", "m"}, {"p_model", .6}, {"p_raw", .62},
              {"sigma", .05}, {"fair_ts", 1000}, {"model_inputs", {{"valid", true}}}};
  ProbabilityQuote rq = quote_from_board_row(row);
  CHECK(rq.p_yes == .6 && rq.model_key() == "m_v1" && !rq.settlement_verified && rq.auto_eligible);
  CHECK_NEAR(rq.p_raw(), .62, 1e-12);
  row["settlement_verified"] = true;
  CHECK(quote_from_board_row(row).settlement_verified);
}

// ---- B. YES/NO normalization and the edge engine ----------------------------------------------
static void test_normalization() {
  // Normal two-sided book: NO is the YES book from the other side, sizes included.
  MarketSidePrices p = normalize_prices(0.60, 0.62, 0, 0, 20, 30);
  CHECK_NEAR(p.no_bid, 0.38, 1e-12);
  CHECK_NEAR(p.no_ask, 0.40, 1e-12);
  CHECK(p.no_ask_size == 20 && p.no_bid_size == 30);   // the NO ask rests on the YES bid
  CHECK(p.two_sided(Side::Yes) && p.two_sided(Side::No) && !p.crossed);
  CHECK_NEAR(p.yes_mid(), 0.61, 1e-12);
  // Explicit NO prices win when they are real prices.
  MarketSidePrices ex = normalize_prices(0.60, 0.62, 0.37, 0.41);
  CHECK_NEAR(ex.no_bid, 0.37, 1e-12);
  CHECK_NEAR(ex.no_ask, 0.41, 1e-12);
  // One-sided: bids only. The NO ask exists (1 - bid), the NO bid does not.
  MarketSidePrices bids = normalize_prices(0.90, 0);
  CHECK(bids.yes_ask == 0 && bids.no_bid == 0);
  CHECK_NEAR(bids.no_ask, 0.10, 1e-12);
  CHECK(!bids.two_sided(Side::Yes) && !bids.two_sided(Side::No));
  // The 1c / 99c edges are real prices; 0 and 1 are not.
  MarketSidePrices edges = normalize_prices(0.01, 0.99);
  CHECK(edges.two_sided(Side::Yes) && edges.two_sided(Side::No));
  CHECK_NEAR(edges.no_ask, 0.99, 1e-12);
  CHECK_NEAR(edges.no_bid, 0.01, 1e-12);
  MarketSidePrices ends = normalize_prices(0.0, 1.0);
  CHECK(ends.yes_bid == 0 && ends.yes_ask == 0 && ends.no_bid == 0 && ends.no_ask == 0);
  // Missing side entirely, and garbage.
  MarketSidePrices none = normalize_prices(0, 0);
  CHECK(!none.two_sided(Side::Yes) && !none.two_sided(Side::No));
  MarketSidePrices nan = normalize_prices(NAN, -0.2, 1.3, INFINITY);
  CHECK(nan.yes_bid == 0 && nan.yes_ask == 0 && nan.no_bid == 0 && nan.no_ask == 0);
  // YES derived from a NO-only book.
  MarketSidePrices no_only = normalize_prices(0, 0, 0.38, 0.40);
  CHECK_NEAR(no_only.yes_bid, 0.60, 1e-12);
  CHECK_NEAR(no_only.yes_ask, 0.62, 1e-12);
  // Crossed: not tradable as read, on either side; a locked book still is.
  MarketSidePrices crossed = normalize_prices(0.65, 0.60);
  CHECK(crossed.crossed && !crossed.two_sided(Side::Yes) && !crossed.two_sided(Side::No));
  CHECK(normalize_prices(0.60, 0.60).two_sided(Side::Yes));
  // The same helper reads a Market and a board row the same way.
  Market m = market("T-1", "T", 0.60, 0.62);
  m.no_bid = 0; m.no_ask = 0;
  CHECK_NEAR(normalize_prices(m).no_ask, 0.40, 1e-12);
  CHECK_NEAR(normalize_prices_from_row(json{{"yes_bid", .6}, {"yes_ask", .62}}).no_bid, 0.38, 1e-12);
}

static void test_edges() {
  const FeeModel fm{"quadratic", 1};
  EdgeSettings es;   // 0.5 x sigma, no slippage
  const MarketSidePrices px = normalize_prices(0.60, 0.62, 0, 0, 20, 30);
  // YES taker at the actual ask, never the mid.
  EdgeCandidate y = taker_candidate(quote(0.70, 0.05), px, Side::Yes, fm, es);
  CHECK(y.executable && y.style == "taker" && y.entry_price == 0.62);
  CHECK_NEAR(y.fair_probability, 0.70, 1e-12);
  CHECK_NEAR(y.entry_fee, fm.fee(0.62, 1, true), 1e-12);
  CHECK_NEAR(y.raw_edge, 0.08, 1e-12);
  CHECK_NEAR(y.net_edge, 0.08 - fm.fee(0.62, 1, true), 1e-12);
  CHECK_NEAR(y.uncertainty_buffer, 0.025, 1e-12);
  CHECK_NEAR(y.executable_edge, y.net_edge - 0.025, 1e-12);
  CHECK_NEAR(y.ev_per_contract, y.net_edge, 1e-12);
  CHECK_NEAR(y.kelly, (0.70 - 0.62 - y.entry_fee - 0.025) / (1 - 0.62 - y.entry_fee), 1e-12);
  CHECK(y.displayed_size == 30 && y.bid == 0.60 && y.ask == 0.62);
  CHECK_NEAR(y.spread, 0.02, 1e-12);
  // NO from the same quote: fair 1 - p, bought at the NO ask (1 - YES bid).
  EdgeCandidate n = taker_candidate(quote(0.30, 0.05), normalize_prices(0.38, 0.40), Side::No, fm, es);
  CHECK_NEAR(n.fair_probability, 0.70, 1e-12);
  CHECK_NEAR(n.entry_price, 0.62, 1e-12);
  CHECK_NEAR(n.net_edge, y.net_edge, 1e-12);
  // Both sides of one quote are evaluated from the same number.
  EdgeCandidate ny = taker_candidate(quote(0.70, 0.05), px, Side::No, fm, es);
  CHECK_NEAR(ny.fair_probability, 0.30, 1e-12);
  CHECK_NEAR(ny.entry_price, 0.40, 1e-12);
  CHECK(ny.raw_edge < 0);
  // Slippage costs a taker, not a maker.
  EdgeSettings slip = es;
  slip.taker_slippage = 0.01;
  EdgeCandidate ys = taker_candidate(quote(0.70, 0.05), px, Side::Yes, fm, slip);
  CHECK_NEAR(ys.executable_edge, y.executable_edge - 0.01, 1e-12);
  CHECK_NEAR(ys.ev_per_contract, y.ev_per_contract - 0.01, 1e-12);
  EdgeCandidate ms = maker_candidate(quote(0.70, 0.05), normalize_prices(0.60, 0.64), Side::Yes, fm, 0.01, slip);
  CHECK(ms.estimated_slippage == 0);
  // The sigma buffer scales with the multiplier.
  EdgeSettings two = es;
  two.uncertainty_multiplier = 2.0;
  CHECK_NEAR(taker_candidate(quote(0.70, 0.05), px, Side::Yes, fm, two).uncertainty_buffer, 0.10, 1e-12);
  // Maker price: one tick inside a two-tick spread, joining a one-tick spread, never at the ask.
  EdgeCandidate mk = maker_candidate(quote(0.66, 0.03), normalize_prices(0.60, 0.64), Side::Yes, fm, 0.01, es);
  CHECK(mk.executable && mk.style == "maker");
  CHECK_NEAR(mk.entry_price, 0.61, 1e-12);
  CHECK(mk.entry_fee == 0);                                  // makers pay nothing on quadratic series
  CHECK_NEAR(mk.net_edge, 0.05, 1e-12);
  CHECK_NEAR(maker_candidate(quote(0.66, 0.03), px, Side::Yes, fm, 0.01, es).entry_price, 0.61, 1e-12);   // 2-tick spread
  CHECK_NEAR(maker_candidate(quote(0.66, 0.03), normalize_prices(0.60, 0.61), Side::Yes, fm, 0.01, es).entry_price, 0.60, 1e-12);
  // A maker-fee series charges the maker.
  const FeeModel mf{"quadratic_with_maker_fees", 1};
  EdgeCandidate mkf = maker_candidate(quote(0.66, 0.03), normalize_prices(0.60, 0.64), Side::Yes, mf, 0.01, es);
  CHECK_NEAR(mkf.entry_fee, mf.fee(0.61, 1, false), 1e-12);
  CHECK(mkf.entry_fee > 0 && mkf.net_edge < mk.net_edge);
  // One-sided books: an offer can be lifted, a bid cannot be posted into from nothing.
  const MarketSidePrices asks = normalize_prices(0, 0.12, 0, 0, 0, 50);
  EdgeCandidate lift = taker_candidate(quote(0.30, 0.02), asks, Side::Yes, fm, es);
  CHECK(lift.executable && lift.entry_price == 0.12 && lift.diagnostics.value("one_sided", false));
  CHECK(!maker_candidate(quote(0.30, 0.02), asks, Side::Yes, fm, 0.01, es).executable);
  CHECK(!taker_candidate(quote(0.30, 0.02), normalize_prices(0.10, 0), Side::Yes, fm, es).executable);
  CHECK(!taker_candidate(quote(0.70, 0.05), normalize_prices(0.65, 0.60), Side::Yes, fm, es).executable);   // crossed
  // An arbitrary price: the dialog's typed limit, from the same arithmetic.
  EdgeCandidate at = candidate_at(quote(0.70, 0.05), px, Side::Yes, "taker", 0.64, fm, es);
  CHECK_NEAR(at.net_edge, 0.70 - 0.64 - fm.fee(0.64, 1, true), 1e-12);
  // No probability, no edge.
  CHECK(!taker_candidate(quote(NAN, 0.05), px, Side::Yes, fm, es).executable);
}

// ---- C. The router ------------------------------------------------------------------------------
// A board row the router can take: two-sided, fresh, settling in 5 h, verified, allowed.
static json row(const std::string& ticker, const std::string& model, double p, double sigma, double bid, double ask,
                long long now, const std::string& category = "Climate and Weather") {
  const std::string event = ticker.substr(0, ticker.rfind('-'));
  ProbabilityQuote q = quote(p, sigma, model);
  q.ticker = ticker;
  q.event_ticker = event;
  q.model_ts = now - 2;
  q.received_ts = now - 2;
  return json{{"ticker", ticker}, {"event_ticker", event}, {"series_ticker", event.substr(0, event.find('-'))},
              {"category", category}, {"fee_type", "quadratic"}, {"fee_multiplier", 1.0}, {"yes_bid", bid},
              {"yes_ask", ask}, {"no_bid", 1 - ask}, {"no_ask", 1 - bid}, {"yes_bid_size", 50}, {"yes_ask_size", 50},
              {"no_bid_size", 50}, {"no_ask_size", 50}, {"tick", 0.01}, {"quote_ts", now - 3}, {"quote_stale", false},
              {"fair_ts", now - 2}, {"fair_stale", false}, {"close_ts", now + 6 * 3600}, {"market_status", "active"},
              {"minutes_to_close", 360}, {"settlement_ts", now + 5 * 3600}, {"p_model", p}, {"p_raw", p},
              {"sigma", sigma}, {"fair_strategy", model}, {"quote", quote_to_json(q)},
              {"entry_context", {{"allowed", true}, {"kind", "new"}, {"reason", ""}}}};
}

static double number_or(const json& j, const char* k, double fallback = 0) {
  return j.contains(k) && j[k].is_number() ? j[k].get<double>() : fallback;
}
static json settings_with(const json& patch) {
  json s = systematic_defaults();
  for (auto it = patch.begin(); it != patch.end(); ++it) s[it.key()] = it.value();
  return validate_systematic_settings(json::object(), s);
}
// Weather on auto (the outer permission), the auto model routed auto and graduated, the manual one
// routed manual, the research one routed research.
static json lane_settings() {
  return settings_with(json{{"weather", "auto"}, {"kelly_fraction", 0},
                            {"model_routes", {{"auto_model_v1", "auto"}, {"manual_model_v1", "manual"},
                                              {"research_model_v1", "research"}}},
                            {"graduated_models", json::array({"auto_model_v1"})}});
}

static void test_router() {
  const long long now = now_s();
  RouterContext ctx;
  ctx.now = now;
  const json s = lane_settings();
  // A clean candidate for a graduated auto model: AUTO, taker at the ask.
  const json clean = row("KXHIGHNY-26SEP11-B70", "auto_model", 0.74, 0.05, 0.60, 0.62, now);
  TradeDecision d = decide(clean, Side::Yes, s, ctx);
  CHECK(d.route == DecisionRoute::Auto && d.verdict == "algo" && d.reasons.empty());
  CHECK(d.candidate.style == "taker" && d.candidate.entry_price == 0.62);
  CHECK(d.decision_id == decision_id_for("KXHIGHNY-26SEP11-B70", Side::Yes, "auto_model_v1", now - 2, now - 3));
  // The same inputs give the same id; a new book or a new estimate gives a new one.
  CHECK(decide(clean, Side::Yes, s, ctx).decision_id == d.decision_id);
  json newer = clean;
  newer["quote_ts"] = now - 1;
  CHECK(decide(newer, Side::Yes, s, ctx).decision_id != d.decision_id);
  CHECK(decide(clean, Side::No, s, ctx).decision_id != d.decision_id);

  // Blocked beats everything: a stale quote on a research-only estimate that is also manual.
  json stale = clean;
  stale["quote_stale"] = true;
  stale["quote"]["research_only"] = true;
  json st = settings_with(json{{"weather", "manual"}});
  TradeDecision b = decide(stale, Side::Yes, st, ctx);
  CHECK(b.route == DecisionRoute::Blocked && b.verdict == "blocked");
  CHECK(b.reasons.front().find("stale") != std::string::npos);
  // Research beats manual when the model is not ready to recommend.
  json unverified = clean;
  unverified["quote"]["settlement_verified"] = false;
  TradeDecision r1 = decide(unverified, Side::Yes, st, ctx);
  CHECK(r1.route == DecisionRoute::Research);
  bool says_settlement = false;
  for (const auto& why : r1.reasons) says_settlement = says_settlement || why.find("settlement not verified") != std::string::npos;
  CHECK(says_settlement);
  json flagged = clean;
  flagged["quote"]["research_only"] = true;
  flagged["quote"]["research_reason"] = "fit did not converge";
  CHECK(decide(flagged, Side::Yes, s, ctx).route == DecisionRoute::Research);
  CHECK(decide(row("KXHIGHNY-26SEP11-B71", "research_model", 0.74, 0.05, 0.60, 0.62, now), Side::Yes, s, ctx).route ==
        DecisionRoute::Research);
  TradeDecision unknown = decide(row("KXHIGHNY-26SEP11-B72", "brand_new_model", 0.74, 0.05, 0.60, 0.62, now), Side::Yes, s, ctx);
  CHECK(unknown.route == DecisionRoute::Research && unknown.reasons.front().find("no route yet") != std::string::npos);
  // Manual for a valid edge from a model that has not graduated.
  json ungrad = s;
  ungrad["graduated_models"] = json::array();
  TradeDecision m1 = decide(clean, Side::Yes, ungrad, ctx);
  CHECK(m1.route == DecisionRoute::Manual && m1.reasons.front().find("graduated") != std::string::npos);
  TradeDecision m2 = decide(row("KXHIGHNY-26SEP11-B73", "manual_model", 0.74, 0.05, 0.60, 0.62, now), Side::Yes, s, ctx);
  CHECK(m2.route == DecisionRoute::Manual && m2.reasons.front().find("routed to manual") != std::string::npos);
  // A category on auto never promotes a model that is not: the lower route wins.
  CHECK(effective_route(RouteMode::Auto, RouteMode::Manual) == RouteMode::Manual);
  CHECK(effective_route(RouteMode::Manual, RouteMode::Auto) == RouteMode::Manual);
  CHECK(effective_route(RouteMode::Off, RouteMode::Auto) == RouteMode::Off);
  CHECK(effective_route(RouteMode::Research, RouteMode::Manual) == RouteMode::Research);
  json cat_manual = s;
  cat_manual["weather"] = "manual";
  CHECK(decide(clean, Side::Yes, cat_manual, ctx).route == DecisionRoute::Manual);
  // Category OFF prevents a graduated auto model from trading at all.
  json cat_off = s;
  cat_off["weather"] = "off";
  TradeDecision off = decide(clean, Side::Yes, cat_off, ctx);
  CHECK(off.route == DecisionRoute::Blocked && off.reasons.front().find("switched off") != std::string::npos);
  // The desk.json strategy mode still caps the model: research there is research here.
  RouterContext capped = ctx;
  capped.strategy_modes["auto_model"] = "research";
  TradeDecision cap = decide(clean, Side::Yes, s, capped);
  CHECK(cap.route == DecisionRoute::Research && cap.reasons.front().find("research mode") != std::string::npos);
  capped.strategy_modes["auto_model"] = "bot";
  CHECK(decide(clean, Side::Yes, s, capped).route == DecisionRoute::Auto);
  // The AUTO master switch: with it off the same row is yours (and must clear your floor).
  RouterContext disarmed = ctx;
  disarmed.auto_armed = false;
  disarmed.auto_off_reason = "the algorithm is stopped: offered to you instead";
  TradeDecision sw = decide(clean, Side::Yes, s, disarmed);
  CHECK(sw.route == DecisionRoute::Manual);
  CHECK(sw.reasons.back().find("stopped") != std::string::npos);
  json thin = row("KXHIGHNY-26SEP11-B74", "auto_model", 0.70, 0.05, 0.60, 0.62, now);   // 0.0385 after the buffer
  CHECK(decide(thin, Side::Yes, s, ctx).route == DecisionRoute::Auto);
  TradeDecision thin_off = decide(thin, Side::Yes, s, disarmed);
  CHECK(thin_off.route == DecisionRoute::Blocked && thin_off.verdict == "weak");   // under the 0.05 manual floor
  // The kill switch blocks everything.
  RouterContext killed = ctx;
  killed.killed = true;
  CHECK(decide(clean, Side::Yes, s, killed).route == DecisionRoute::Blocked);
  // Elevated sigma goes to you; so do a source not approved for unattended use, a wide spread,
  // an unusual model/market gap and an edge over the cap.
  CHECK(decide(row("KXHIGHNY-26SEP11-B75", "auto_model", 0.80, 0.12, 0.60, 0.62, now), Side::Yes, s, ctx).route ==
        DecisionRoute::Manual);
  json manual_src = clean;
  manual_src["quote"]["auto_eligible"] = false;
  CHECK(decide(manual_src, Side::Yes, s, ctx).route == DecisionRoute::Manual);
  CHECK(decide(row("KXHIGHNY-26SEP11-B76", "auto_model", 0.80, 0.05, 0.60, 0.70, now), Side::Yes, s, ctx).route ==
        DecisionRoute::Manual);
  CHECK(decide(row("KXHIGHNY-26SEP11-B77", "auto_model", 0.85, 0.05, 0.60, 0.62, now), Side::Yes, s, ctx).route ==
        DecisionRoute::Manual);
  // Settlement outside the desk's horizon is blocked; unknown settlement is blocked.
  json far = clean;
  far["settlement_ts"] = now + 50 * 3600;
  CHECK(decide(far, Side::Yes, s, ctx).route == DecisionRoute::Blocked);
  json unknown_settle = clean;
  unknown_settle["settlement_ts"] = 0;
  CHECK(decide(unknown_settle, Side::Yes, s, ctx).route == DecisionRoute::Blocked);
  // The disagreement gate: raw model 0.35 from a liquid mid is nobody's trade.
  json gap = clean;
  gap["quote"]["diagnostics"]["p_raw"] = 0.96;
  CHECK(decide(gap, Side::Yes, s, ctx).route == DecisionRoute::Blocked);
  // Invalid probability and an invalid book are blocked.
  json invalid = clean;
  invalid["quote"]["p_yes"] = nullptr;
  CHECK(decide(invalid, Side::Yes, s, ctx).route == DecisionRoute::Blocked);
  json crossed = clean;
  crossed["yes_bid"] = 0.65;
  crossed["no_ask"] = 0.35;
  CHECK(decide(crossed, Side::Yes, s, ctx).route == DecisionRoute::Blocked);
  // Exactly one route per candidate, and the displayed verdict always agrees with it.
  json board = {{"markets", json::array({clean, stale, unverified, flagged, thin, far, gap, invalid})}};
  for (const auto& dd : decide_board(board, s, ctx)) {
    const bool agree = (dd.route == DecisionRoute::Auto && dd.verdict == "algo") ||
                       (dd.route == DecisionRoute::Manual && dd.verdict == "manual") ||
                       (dd.route == DecisionRoute::Research && dd.verdict == "research") ||
                       (dd.route == DecisionRoute::Blocked && (dd.verdict == "blocked" || dd.verdict == "weak"));
    CHECK(agree);
    CHECK(!dd.decision_id.empty());
  }
  // The screen row carries the decision: route, id, model, and the edge breakdown the dialog shows.
  json r = decision_to_row(d);
  CHECK(r["route"] == "auto" && r["decision_id"] == d.decision_id && r["model_key"] == "auto_model_v1");
  for (const char* k : {"raw_edge", "executable_edge", "fee", "slippage", "uncertainty_buffer", "sigma", "p_win", "price",
                        "bid", "ask", "displayed_size", "model_ts", "book_ts", "quote"})
    CHECK(r.contains(k));
  CHECK_NEAR(r["raw_edge"].get<double>(), 0.74 - 0.62, 1e-4);
}

// ---- D. Parallel lanes: one synthetic cycle, four routes ----------------------------------------
static void test_parallel_lanes() {
  const long long now = now_s();
  RouterContext ctx;
  ctx.now = now;
  json a = row("KXHIGHNY-26SEP11-B70", "auto_model", 0.74, 0.05, 0.60, 0.62, now);       // graduated, auto
  json b = row("KXHIGHCHI-26SEP11-B80", "manual_model", 0.74, 0.05, 0.60, 0.62, now);    // routed manual
  json c = row("KXHIGHMIA-26SEP11-B90", "research_model", 0.74, 0.05, 0.60, 0.62, now);  // routed research
  json d = row("KXHIGHLAX-26SEP11-B75", "auto_model", 0.74, 0.05, 0.60, 0.62, now);      // stale book
  d["quote_stale"] = true;
  json board = {{"markets", json::array({a, b, c, d})}};
  json screen = systematic_screen(board, lane_settings(), ctx);
  auto route_of_row = [&](const std::string& ticker) {
    for (const auto& r : screen["rows"])
      if (r["ticker"] == ticker && r["side"] == "yes") return r["route"].get<std::string>();
    return std::string("missing");
  };
  CHECK(route_of_row("KXHIGHNY-26SEP11-B70") == "auto");
  CHECK(route_of_row("KXHIGHCHI-26SEP11-B80") == "manual");
  CHECK(route_of_row("KXHIGHMIA-26SEP11-B90") == "research");
  CHECK(route_of_row("KXHIGHLAX-26SEP11-B75") == "blocked");
  // Both sides of every market are decided: the research model's two sides are research.
  CHECK(screen["counts"]["algo"] == 1 && screen["counts"]["manual"] == 1 && screen["counts"]["research"] == 2);
  CHECK(screen["counts"]["sides"] == 8);
  // The AUTO row comes first, exactly as the algorithm reads the screen.
  CHECK(screen["rows"][0]["route"] == "auto");
}

// ---- E. Ownership --------------------------------------------------------------------------------
static void test_ownership() {
  const long long now = now_s();
  const json s = lane_settings();
  json auto_row = row("KXHIGHNY-26SEP11-B70", "auto_model", 0.74, 0.05, 0.60, 0.62, now);
  json manual_row = row("KXHIGHNY-26SEP11-B71", "manual_model", 0.74, 0.05, 0.60, 0.62, now);
  json elsewhere = row("KXHIGHCHI-26SEP11-B80", "auto_model", 0.74, 0.05, 0.60, 0.62, now);
  // Your position in the event: the algorithm opens nothing in it, and nor is a new market offered.
  RouterContext yours;
  yours.now = now;
  yours.held_events = {"KXHIGHNY-26SEP11"};
  CHECK(decide(auto_row, Side::Yes, s, yours).route == DecisionRoute::Blocked);
  CHECK(decide(manual_row, Side::Yes, s, yours).route == DecisionRoute::Blocked);
  CHECK(decide(elsewhere, Side::Yes, s, yours).route == DecisionRoute::Auto);   // unrelated events still flow
  // A qualified same-side addition to your own position is yours to review, never the algorithm's.
  json add = auto_row;
  add["entry_context_yes"] = {{"allowed", true}, {"kind", "addition"}, {"required_edge", 0.08},
                              {"approval_required", true}, {"reason", "qualified same-side addition"}};
  TradeDecision a = decide(add, Side::Yes, s, yours);
  CHECK(a.route == DecisionRoute::Manual && a.reasons.front().find("addition") != std::string::npos);
  CHECK(decide(add, Side::No, s, yours).route == DecisionRoute::Blocked);         // not the side you hold
  json thin_add = add;
  thin_add["entry_context_yes"]["required_edge"] = 0.20;                           // the addition bar is higher
  CHECK(decide(thin_add, Side::Yes, s, yours).verdict == "weak");
  // The algorithm's position in the event: blocked for you too, addition or not, as it always was.
  RouterContext algos = yours;
  algos.algo_events = {"KXHIGHNY-26SEP11"};
  CHECK(decide(add, Side::Yes, s, algos).route == DecisionRoute::Blocked);
  CHECK(decide(manual_row, Side::Yes, s, algos).route == DecisionRoute::Blocked);
  CHECK(decide(elsewhere, Side::Yes, s, algos).route == DecisionRoute::Auto);
  // A working order or a queued request owns its event the same way.
  RouterContext working = yours;
  working.working_events = {"KXHIGHNY-26SEP11"};
  CHECK(decide(add, Side::Yes, s, working).route == DecisionRoute::Blocked);
  // Cooldowns after an exit.
  RouterContext cool;
  cool.now = now;
  cool.cooldowns["KXHIGHNY-26SEP11"] = now + 600;
  CHECK(decide(auto_row, Side::Yes, s, cool).route == DecisionRoute::Blocked);
}

// A displayed decision priced again (the order dialog's typed price, the maker ladder's steps): the
// same candidate_at on the decision's own quote and book, with the slippage and the uncertainty
// deduction the router used, for YES and for NO.
static void test_decision_repricing() {
  const long long now = now_s();
  RouterContext ctx;
  ctx.now = now;
  const json s = settings_with(json{{"weather", "auto"}, {"kelly_fraction", 0}, {"slippage_estimate", 0.01},
                                    {"model_routes", {{"auto_model_v1", "auto"}}}, {"graduated_models", json::array({"auto_model_v1"})}});
  const FeeModel fm{"quadratic", 1};
  for (Side side : {Side::Yes, Side::No}) {
    const double p = side == Side::Yes ? 0.80 : 0.20;   // a big edge on the side under test
    const TradeDecision d = decide(row("KXHIGHNY-26SEP11-B80", "auto_model", p, 0.04, 0.60, 0.64, now), side, s, ctx);
    const json r = decision_to_row(d);
    // The book the row carries, back through the one normalization.
    const MarketSidePrices px = decision_prices(r);
    CHECK_NEAR(px.yes_bid, 0.60, 1e-9);
    CHECK_NEAR(px.yes_ask, 0.64, 1e-9);
    CHECK_NEAR(px.no_bid, 0.36, 1e-9);
    CHECK_NEAR(px.no_ask, 0.40, 1e-9);
    // At the decision's own price and style, the decision's numbers.
    const EdgeCandidate same = decision_edge_at(r, d.candidate.style, d.candidate.entry_price, s);
    CHECK_NEAR(same.executable_edge, d.candidate.executable_edge, 1e-9);
    CHECK_NEAR(same.kelly, d.candidate.kelly, 1e-9);
    CHECK_NEAR(same.fair_probability, d.candidate.fair_probability, 1e-9);
    // Lifting the offer costs the fee and the slippage estimate; posting costs neither.
    const double ask = side == Side::Yes ? 0.64 : 0.40;
    const EdgeCandidate lift = decision_edge_at(r, "taker", ask, s);
    CHECK_NEAR(lift.estimated_slippage, 0.01, 1e-12);
    CHECK_NEAR(lift.executable_edge, 0.80 - ask - fm.fee(ask, 1, true) - 0.01 - 0.5 * 0.04, 1e-9);
    const EdgeCandidate post = decision_edge_at(r, "maker", ask - 0.02, s);
    CHECK_NEAR(post.estimated_slippage, 0.0, 1e-12);
    CHECK_NEAR(post.executable_edge, 0.80 - (ask - 0.02) - fm.fee(ask - 0.02, 1, false) - 0.5 * 0.04, 1e-9);
    // A price with no edge left is not a price the floor accepts.
    CHECK(decision_edge_at(r, "maker", 0.79, s).executable_edge < number_or(s, "manual_min_edge"));
  }
}

// Routes and graduation are per name AND version: a bare-name route caps every version and grants
// none, so neither a route nor a graduation carries over to a model's next numbers.
static void test_versioned_routes() {
  json s = settings_with(json{{"model_routes", {{"crypto_lognormal", "off"}}}});
  CHECK(s["model_routes"]["crypto_lognormal_v1"] == "manual");                 // the default is still there...
  bool known = false;
  CHECK(model_route(s, "crypto_lognormal", "v1", &known) == RouteMode::Off && known);   // ...and the name caps it
  s = settings_with(json{{"model_routes", {{"newmodel", "auto"}}}, {"graduated_models", json::array({"newmodel"})}});
  CHECK(model_route(s, "newmodel", "v7") == RouteMode::Research);              // a bare name grants nothing
  CHECK(!model_graduated(s, "newmodel", "v7"));
  s = settings_with(json{{"model_routes", {{"newmodel_v7", "auto"}, {"newmodel", "manual"}}},
                         {"graduated_models", json::array({"newmodel_v7"})}});
  CHECK(model_route(s, "newmodel", "v7") == RouteMode::Manual);                // the cap wins over the grant
  CHECK(model_graduated(s, "newmodel", "v7") && !model_graduated(s, "newmodel", "v8"));
  CHECK(model_route(s, "newmodel", "v8", &known) == RouteMode::Research && known);   // v8 has no grant of its own
  s = settings_with(json::object());
  CHECK(model_route(s, "ladder_arb", "") == RouteMode::Auto);                  // an unversioned basket, by name
  CHECK(model_route(s, "brand_new", "v1", &known) == RouteMode::Research && !known);
  // In the router: a category on auto with the model capped by name is offered to nobody.
  const long long now = now_s();
  RouterContext ctx;
  ctx.now = now;
  json capped = settings_with(json{{"weather", "auto"}, {"model_routes", {{"auto_model_v1", "auto"}, {"auto_model", "research"}}},
                                   {"graduated_models", json::array({"auto_model_v1"})}});
  CHECK(decide(row("KXHIGHNY-26SEP11-B90", "auto_model", 0.74, 0.05, 0.60, 0.62, now), Side::Yes, capped, ctx).route ==
        DecisionRoute::Research);
}

int main() {
  test_quote();
  test_normalization();
  test_edges();
  test_router();
  test_parallel_lanes();
  test_ownership();
  test_decision_repricing();
  test_versioned_routes();
  std::printf("pipeline: %d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
