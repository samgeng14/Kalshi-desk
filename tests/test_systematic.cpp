// The systematic trader: the screen and its verdicts, live entries and exits through a mock
// exchange, the exit rules, the daily budget and loss stop, the kill switch, and paper mode
// sending nothing. Built and run with `ninja -C build-ci && ./build-ci/systematic-tests`.
#include <atomic>
#include <cmath>
#include <deque>
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
#include "desk/executor.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/ladder.h"
#include "desk/log.h"
#include "desk/reject.h"
#include "desk/route.h"
#include "desk/store.h"
#include "desk/systematic.h"
#include "desk/lanes.h"
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

// One board row as Engine::research_board() would publish it. `p` is P(YES) after the market
// blend; `raw` the model before it.
static json row(const std::string& ticker, const std::string& event, const std::string& category, double p,
                double raw, double sigma, double bid, double ask, double bid_size, double ask_size, long long now,
                double hours_to_settle = 5.0, const std::string& fee_type = "quadratic") {
  return json{{"ticker", ticker}, {"event_ticker", event}, {"series_ticker", event.substr(0, event.find('-'))},
              {"title", ticker + " title"}, {"yes_sub_title", "sub"}, {"category", category}, {"fee_type", fee_type},
              {"fee_multiplier", 1.0}, {"event_date", "2026-09-08"}, {"yes_bid", bid}, {"yes_ask", ask},
              {"no_bid", 1 - ask}, {"no_ask", 1 - bid}, {"yes_bid_size", bid_size}, {"yes_ask_size", ask_size},
              {"no_bid_size", ask_size}, {"no_ask_size", bid_size}, {"tick", 0.01}, {"quote_ts", now - 3},
              {"close_ts", now + 6 * 3600}, {"market_status", "active"}, {"minutes_to_close", 360},
              {"settlement_ts", now + static_cast<long long>(hours_to_settle * 3600)},
              {"hours_to_settlement", hours_to_settle}, {"quote_stale", false}, {"fair_stale", false},
              {"fair_ts", now - 2}, {"p_model", p}, {"p_raw", raw}, {"sigma", sigma}, {"fair_strategy", "test_model"},
              {"fair_note", "test"}, {"model_inputs", {{"valid", true}}}, {"model_research_only", false},
              {"model_research_reason", ""}, {"research_only", false}, {"research_reason", ""},
              {"entry_context", {{"allowed", true}, {"kind", "new"}, {"reason", ""}}},
              // What the engine's quote says for a model whose settlement mapping is verified.
              {"settlement_verified", true}};
}

// The routes a test needs for the algorithm to send: the model categories on auto, and the test
// model routed auto AND graduated. The desk's defaults (manual everywhere, nothing graduated) are
// exactly what keeps an unproven model from sending, so a test that expects the algorithm to act
// says so here instead of relying on them.
static json algo_routes() {
  return json{{"weather", "auto"}, {"sports", "auto"}, {"crypto", "auto"},
              {"model_routes", {{"test_model_v1", "auto"}}}, {"graduated_models", json::array({"test_model_v1"})}};
}
static json algo_settings(json s = systematic_defaults()) {
  const json r = algo_routes();
  for (auto it = r.begin(); it != r.end(); ++it) s[it.key()] = it.value();
  return s;
}

static void test_settings() {
  json d = validate_systematic_settings(json::object(), systematic_defaults());
  CHECK(d == systematic_defaults());
  CHECK(d["max_positions"].is_number_integer());
  bool threw = false;
  try { validate_systematic_settings(json{{"nope", 1}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"min_edge", 0.2}, {"max_edge", 0.1}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"entry_style", "hopeful"}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"crypto", "yes"}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"per_trade_dollars", 50}, {"max_open_dollars", 10}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  // The nine category switches are routes now. A settings row written before that change holds
  // booleans, and they have to keep meaning what they meant or the desk loses its saved state on
  // the first restart: true was the algorithm's list, false already put the row in front of you.
  json ok = validate_systematic_settings(json{{"crypto", false}, {"max_positions", 3.0}}, d);
  CHECK(ok["crypto"] == "manual" && ok["max_positions"] == 3 && ok["weather"] == "manual");
  // Session 15: the three model categories default to manual, arbitrage stays auto, and every
  // model has a route of its own; nothing is graduated by default.
  CHECK(d["weather"] == "manual" && d["sports"] == "manual" && d["crypto"] == "manual" && d["arbitrage"] == "auto");
  CHECK(d["model_routes"]["weather_nws_v1"] == "manual" && d["model_routes"]["crypto_lognormal_v1"] == "manual" &&
        d["model_routes"]["sports_live_v1"] == "manual" && d["model_routes"]["sports_inplay_v1"] == "research" &&
        d["model_routes"]["ladder_arb"] == "auto");
  CHECK(d["graduated_models"].is_array() && d["graduated_models"].empty());
  // Model routes merge entry by entry: routing one model leaves the others where they were.
  json one = validate_systematic_settings(json{{"model_routes", {{"sports_live_v1", "research"}}}}, d);
  CHECK(one["model_routes"]["sports_live_v1"] == "research" && one["model_routes"]["weather_nws_v1"] == "manual");
  CHECK(validate_systematic_settings(json::object(), one)["model_routes"]["sports_live_v1"] == "research");
  threw = false;
  try { validate_systematic_settings(json{{"model_routes", {{"weather_nws_v1", "sometimes"}}}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"model_routes", "auto"}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"graduated_models", json::array({"bad name!"})}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { validate_systematic_settings(json{{"auto_max_sigma", 0}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  CHECK(validate_systematic_settings(json{{"crypto", true}}, d)["crypto"] == "auto");
  CHECK(validate_systematic_settings(json{{"crypto", "off"}}, d)["crypto"] == "off");
  threw = false;
  try { validate_systematic_settings(json{{"crypto", "sometimes"}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  // The manual lane's floor may not sit under the algorithm's own floor, which would mean the
  // rows you are shown are the ones the algorithm rejected as too thin.
  threw = false;
  try { validate_systematic_settings(json{{"min_edge", .08}, {"manual_min_edge", .04}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  CHECK(systematic_category(json{{"category", "Climate and Weather"}, {"series_ticker", "KXHIGHNY"}}) == "weather");
  CHECK(systematic_category(json{{"category", ""}, {"series_ticker", "KXBTC"}}) == "crypto");
  CHECK(systematic_category(json{{"category", "Sports"}, {"series_ticker", "KXEPLGAME"}}) == "sports");
  CHECK(systematic_category(json{{"category", "Economics"}, {"series_ticker", "KXCPI"}}) == "economics");
}

static const json* find_row(const json& screen, const std::string& ticker, const std::string& side) {
  for (const auto& r : screen["rows"])
    if (r["ticker"] == ticker && r["side"] == side) return &r;
  return nullptr;
}

// A resting entry is re-validated at its own limit every cycle (ported from the parallel
// session's work): a stale model, a vanished edge, a changed model source or failed inputs pull it.
static void test_resting_entry_revalidation() {
  long long now = now_s();
  json s = algo_settings();
  json m = row("WX-A", "WX", "Climate and Weather", .66, .67, .03, .60, .64, 20, 30, now);
  json t = {{"side", "yes"}, {"entry_price", .61}, {"entry_style", "maker"}, {"fair_strategy", "test_model"}};
  CHECK(systematic_entry_cancel_reason(t, m, s, now).empty());
  // Our own working entry appears in entry_context; it must not cancel itself every cycle.
  m["entry_context"] = {{"allowed", false}, {"kind", "working"}};
  CHECK(systematic_entry_cancel_reason(t, m, s, now).empty());
  json stale = m; stale["fair_ts"] = now - 121;
  CHECK(systematic_entry_cancel_reason(t, stale, s, now).find("stale") != std::string::npos);
  stale = m; stale["quote_ts"] = now + 1;
  CHECK(systematic_entry_cancel_reason(t, stale, s, now).find("stale") != std::string::npos);
  json fallen = m; fallen["p_model"] = .63; fallen["p_raw"] = .64;
  CHECK(systematic_entry_cancel_reason(t, fallen, s, now).find("edge vanished") != std::string::npos);
  json invalid = m; invalid["model_inputs"]["valid"] = false;
  CHECK(systematic_entry_cancel_reason(t, invalid, s, now).find("inputs") != std::string::npos);
  invalid = m; invalid["model_inputs"]["auto_eligible"] = false;
  auto permissive=s;permissive["allow_research_estimates"]=true;
  CHECK(systematic_entry_cancel_reason(t, invalid, permissive, now).find("not eligible") != std::string::npos);
  invalid = m; invalid["settlement_ts"] = now + 49 * 3600;
  CHECK(systematic_entry_cancel_reason(t, invalid, s, now).find("horizon") != std::string::npos);
  invalid = m; invalid["fair_strategy"] = "different_model";
  CHECK(systematic_entry_cancel_reason(t, invalid, s, now).find("source changed") != std::string::npos);
  json opposite = m; opposite["p_model"] = .34; opposite["p_raw"] = .33;
  opposite["yes_bid"] = .36; opposite["yes_ask"] = .40;
  opposite["no_bid"] = .60; opposite["no_ask"] = .64;
  t["side"] = "no";
  CHECK(systematic_entry_cancel_reason(t, opposite, s, now).empty());
  opposite["p_model"] = .37;
  CHECK(systematic_entry_cancel_reason(t, opposite, s, now).find("edge vanished") != std::string::npos);
  // A resting entry stays only while its decision would still be AUTO: the model rerouted, not
  // graduated, its settlement unverified or its estimate research-only each pull it.
  t["side"] = "yes";
  json rerouted = s; rerouted["model_routes"]["test_model_v1"] = "manual";
  CHECK(systematic_entry_cancel_reason(t, m, rerouted, now).find("routed to manual") != std::string::npos);
  json ungraduated = s; ungraduated["graduated_models"] = json::array();
  CHECK(systematic_entry_cancel_reason(t, m, ungraduated, now).find("graduated") != std::string::npos);
  json unverified = m; unverified["settlement_verified"] = false;
  CHECK(systematic_entry_cancel_reason(t, unverified, s, now).find("settlement") != std::string::npos);
  json research = m; research["model_research_only"] = true; research["model_research_reason"] = "same-day gate";
  CHECK(systematic_entry_cancel_reason(t, research, s, now).find("research") != std::string::npos);
}

static void test_screen() {
  long long now = now_s();
  json s = algo_settings();
  s["kelly_fraction"] = 0;
  json board = {{"markets", json::array()}};
  auto& ms = board["markets"];
  // A: clean weather candidate. Taker: 0.70 - 0.62 - fee(0.62)=0.0165 -> 0.0635; minus 0.5*0.05 = 0.0385 >= 0.03 -> algo, taker.
  ms.push_back(row("KXHIGHNY-26SEP08-B70", "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now));
  // B: too good to be true: 0.90 vs 0.62 -> edge 0.26 > 0.12 cap -> manual (raw 0.75 keeps the gap under 0.30).
  ms.push_back(row("KXHIGHCHI-26SEP08-B80", "KXHIGHCHI-26SEP08", "Climate and Weather", 0.90, 0.75, 0.05, 0.60, 0.62, 20, 30, now));
  // B2: the same edge with the raw model 0.34 from the mid: the desk's disagreement rule blocks it for everybody.
  ms.push_back(row("KXHIGHCHI-26SEP08-B82", "KXHIGHCHI-26SEP08", "Climate and Weather", 0.90, 0.95, 0.05, 0.60, 0.62, 20, 30, now));
  // C: research estimate. Since session 15 it is routed RESEARCH: logged and shown, offered to
  // nobody (it used to reach the algorithm under allow_research_estimates).
  json c = row("KXHIGHLAX-26SEP08-B75", "KXHIGHLAX-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now);
  c["model_research_only"] = true; c["model_research_reason"] = "same-day weather: WX-1b gate"; c["research_only"] = true;
  ms.push_back(c);
  // D: the engine's disagreement gate is never overridden.
  json d = row("KXHIGHMIA-26SEP08-B90", "KXHIGHMIA-26SEP08", "Climate and Weather", 0.70, 0.99, 0.05, 0.60, 0.62, 20, 30, now);
  d["model_research_only"] = true; d["model_research_reason"] = "model/market disagreement 0.38 > 0.30: check the model"; d["research_only"] = true;
  ms.push_back(d);
  // E: crypto, same numbers as A: algo when enabled, manual when switched off.
  ms.push_back(row("KXBTC-26SEP0815-B71000", "KXBTC-26SEP0815", "Crypto", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now, 1.5));
  // F: stale quote -> blocked.
  json f = row("KXETH-26SEP0815-B4000", "KXETH-26SEP0815", "Crypto", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now, 1.5);
  f["quote_stale"] = true;
  ms.push_back(f);
  // G: probability under the floor even though the price looks cheap: p 0.50 vs ask 0.40 -> weak.
  ms.push_back(row("KXEPLGAME-26SEP08ARSCHE-ARS", "KXEPLGAME-26SEP08ARSCHE", "Sports", 0.50, 0.50, 0.03, 0.38, 0.40, 50, 50, now, 3.0, "quadratic_with_maker_fees"));
  // H: settles in 50h: outside the desk's 48h entry horizon -> blocked for everybody (it used to be
  // handed to you as manual, and then the executor's T+2 gate refused it when you clicked).
  ms.push_back(row("KXNFLGAME-26SEP09KCBUF-KC", "KXNFLGAME-26SEP09KCBUF", "Sports", 0.74, 0.75, 0.05, 0.60, 0.62, 20, 30, now, 50.0, "quadratic_with_maker_fees"));
  // I: maker case. Taker 0.66-0.64-0.0161=0.0039 fails; maker at 0.61 (bid 0.60, spread 0.04): 0.05 - 0 fee, buffered 0.035 -> algo, maker.
  ms.push_back(row("KXHIGHAUS-26SEP08-B95", "KXHIGHAUS-26SEP08", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now));
  // J: NO side wins: P(YES) 0.30 -> NO p 0.70, NO ask = 1 - yes_bid = 0.62.
  ms.push_back(row("KXHIGHPHIL-26SEP08-B60", "KXHIGHPHIL-26SEP08", "Climate and Weather", 0.30, 0.28, 0.05, 0.38, 0.40, 20, 30, now));
  // K: entry policy says no (event already managed) -> blocked.
  json k = row("KXHIGHNY-26SEP08-B71", "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now);
  k["entry_context"] = {{"allowed", false}, {"kind", "managed"}, {"reason", "event already has exposure"}};
  ms.push_back(k);

  json sc = systematic_screen(board, s, now);
  auto A = find_row(sc, "KXHIGHNY-26SEP08-B70", "yes");
  CHECK(A && (*A)["verdict"] == "algo" && (*A)["style"] == "taker");
  if (A) {
    FeeModel fm{"quadratic", 1};
    CHECK_NEAR((*A)["edge"].get<double>(), 0.70 - 0.62 - fm.fee(0.62, 1, true), 1e-6);
    CHECK_NEAR((*A)["kelly"].get<double>(), (0.70 - 0.62 - 0.0165 - 0.025) / (1 - 0.6365), 1e-4);
  }
  auto Ano = find_row(sc, "KXHIGHNY-26SEP08-B70", "no");
  CHECK(Ano && (*Ano)["verdict"] == "weak");   // NO p = 0.30
  auto B = find_row(sc, "KXHIGHCHI-26SEP08-B80", "yes");
  CHECK(B && (*B)["verdict"] == "manual");
  if (B) CHECK((*B)["reasons"].dump().find("above the algorithm's cap") != std::string::npos);
  auto B2 = find_row(sc, "KXHIGHCHI-26SEP08-B82", "yes");
  CHECK(B2 && (*B2)["verdict"] == "blocked");
  if (B2) CHECK((*B2)["reasons"].dump().find("disagreement") != std::string::npos);
  // A longshot with a big edge is nobody's trade: weak outranks the policies that hand a row to you.
  json longshot = row("KXHIGHNY-26SEP08-B80.5", "KXHIGHNY-26SEP08X", "Climate and Weather", 0.52, 0.35, 0.05, 0.20, 0.22, 20, 30, now);
  json sc_l = systematic_screen(json{{"markets", json::array({longshot})}}, s, now);
  auto L = find_row(sc_l, "KXHIGHNY-26SEP08-B80.5", "no");   // NO p = 0.48, NO ask = 0.80, edge 0.48-0.80 < 0 -> weak anyway
  CHECK(L && (*L)["verdict"] == "weak");
  auto Ly = find_row(sc_l, "KXHIGHNY-26SEP08-B80.5", "yes");  // YES p 0.52 vs 0.22: edge 0.29 (cap) but p under the 0.55 floor -> weak
  CHECK(Ly && (*Ly)["verdict"] == "weak");
  auto C = find_row(sc, "KXHIGHLAX-26SEP08-B75", "yes");
  CHECK(C && (*C)["verdict"] == "research" && (*C)["route"] == "research" && (*C)["research"] == true);
  if (C) CHECK((*C)["reasons"].dump().find("research estimate") != std::string::npos);
  auto D = find_row(sc, "KXHIGHMIA-26SEP08-B90", "yes");
  CHECK(D && (*D)["verdict"] == "blocked");
  if (D) CHECK((*D)["reasons"].dump().find("disagreement") != std::string::npos);
  auto E = find_row(sc, "KXBTC-26SEP0815-B71000", "yes");
  CHECK(E && (*E)["verdict"] == "algo");
  auto F = find_row(sc, "KXETH-26SEP0815-B4000", "yes");
  CHECK(F && (*F)["verdict"] == "blocked");
  auto G = find_row(sc, "KXEPLGAME-26SEP08ARSCHE-ARS", "yes");
  CHECK(G && (*G)["verdict"] == "weak");
  if (G) CHECK((*G)["reasons"].dump().find("floor") != std::string::npos);
  auto H = find_row(sc, "KXNFLGAME-26SEP09KCBUF-KC", "yes");
  CHECK(H && (*H)["verdict"] == "blocked");
  if (H) CHECK((*H)["reasons"].dump().find("entry horizon") != std::string::npos);
  // Inside the desk's 48h but past a shorter horizon the algorithm was given: yours, not blocked.
  {
    json s24 = s; s24["max_horizon_hours"] = 24;
    json h30 = row("KXNFLGAME-26SEP09KCBUF-BUF", "KXNFLGAME-26SEP09KCBUF2", "Sports", 0.74, 0.75, 0.05, 0.60, 0.62, 20, 30, now, 30.0, "quadratic_with_maker_fees");
    const json sc30 = systematic_screen(json{{"markets", json::array({h30})}}, s24, now);
    auto H30 = find_row(sc30, "KXNFLGAME-26SEP09KCBUF-BUF", "yes");
    CHECK(H30 && (*H30)["verdict"] == "manual" && (*H30)["reasons"].dump().find("algorithm's") != std::string::npos);
  }
  auto I = find_row(sc, "KXHIGHAUS-26SEP08-B95", "yes");
  CHECK(I && (*I)["verdict"] == "algo" && (*I)["style"] == "maker");
  if (I) { CHECK_NEAR((*I)["price"].get<double>(), 0.61, 1e-9); CHECK_NEAR((*I)["edge"].get<double>(), 0.05, 1e-9); }
  auto J = find_row(sc, "KXHIGHPHIL-26SEP08-B60", "no");
  CHECK(J && (*J)["verdict"] == "algo");
  if (J) { CHECK_NEAR((*J)["ask"].get<double>(), 0.62, 1e-9); CHECK_NEAR((*J)["p_win"].get<double>(), 0.70, 1e-9); }
  auto K = find_row(sc, "KXHIGHNY-26SEP08-B71", "yes");
  CHECK(K && (*K)["verdict"] == "blocked");
  // Ordering: every algo row first, ranked by Kelly fraction.
  std::string last = "algo";
  double last_kelly = 1e9;
  for (const auto& r : sc["rows"]) {
    std::string v = r["verdict"];
    if (v == "algo") { CHECK(last == "algo"); CHECK(r["kelly"].get<double>() <= last_kelly + 1e-12); last_kelly = r["kelly"].get<double>(); }
    last = v;
  }
  CHECK(sc["counts"]["algo"].get<int>() == 4);   // A, E, I, J (C is research now)
  // Policies move rows between algo and manual, never into blocked. E's buffered edge is 0.0385,
  // so it only reaches you once the manual floor is at or under that.
  json s2 = s; s2["crypto"] = false; s2["allow_research_estimates"] = false; s2["manual_min_edge"] = .03;
  json sc2 = systematic_screen(board, s2, now);
  E = find_row(sc2, "KXBTC-26SEP0815-B71000", "yes");
  CHECK(E && (*E)["verdict"] == "manual");
  C = find_row(sc2, "KXHIGHLAX-26SEP08-B75", "yes");
  CHECK(C && (*C)["verdict"] == "research");   // a research estimate is nobody's, whatever the settings say
  // The manual lane is a lane, not an overflow bin: at the default 0.05 floor the same row is
  // nobody's trade, and it says so rather than sitting in your queue.
  json s2b = s2; s2b["manual_min_edge"] = .05;
  json sc2b = systematic_screen(board, s2b, now);
  E = find_row(sc2b, "KXBTC-26SEP0815-B71000", "yes");
  CHECK(E && (*E)["verdict"] == "weak");
  if (E) CHECK((*E)["reasons"].dump().find("manual floor") != std::string::npos);
  // "off" is not "manual": the row is not screened for anyone.
  json s2c = s; s2c["crypto"] = "off";
  json sc2c = systematic_screen(board, s2c, now);
  E = find_row(sc2c, "KXBTC-26SEP0815-B71000", "yes");
  CHECK(E && (*E)["verdict"] == "blocked");
  if (E) CHECK((*E)["reasons"].dump().find("switched off") != std::string::npos);
  // A row the algorithm will send is not subject to the manual floor: it is not a manual row.
  json s2d = s; s2d["manual_min_edge"] = .40;
  json sc2d = systematic_screen(board, s2d, now);
  auto A2 = find_row(sc2d, "KXHIGHNY-26SEP08-B70", "yes");
  CHECK(A2 && (*A2)["verdict"] == "algo");
  // Forced taker style: the maker-only candidate becomes weak.
  json s3 = s; s3["entry_style"] = "taker";
  json sc3 = systematic_screen(board, s3, now);
  I = find_row(sc3, "KXHIGHAUS-26SEP08-B95", "yes");
  CHECK(I && (*I)["verdict"] == "weak");
  // Held events and cooldowns are blocked for the algorithm.
  json sc4 = systematic_screen(board, s, now, {"KXHIGHNY-26SEP08"}, {{"KXHIGHAUS-26SEP08", now + 600}});
  A = find_row(sc4, "KXHIGHNY-26SEP08-B70", "yes");
  I = find_row(sc4, "KXHIGHAUS-26SEP08-B95", "yes");
  CHECK(A && (*A)["verdict"] == "blocked");
  CHECK(I && (*I)["verdict"] == "blocked");
}

// A mock exchange with one liquid weather market. Every order fills in full as sent.
struct MockExchange {
  httplib::Server server;
  std::thread thread;
  int port = 0;
  std::atomic<int> writes{0}, cancels{0};
  std::atomic<bool> reject{false};
  std::atomic<int> reject_next{0};   // refuse only the next N order writes
  std::deque<std::string> reject_queue;   // refuse the next writes with these bodies, in order (under mu)
  std::string reject_body = R"({"error":{"code":"invalid_order","message":"rejected by test exchange"}})";
  json last_order, market_positions = json::array();
  std::string ticker = "KXHIGHNY-26SEP08-B70";
  double fill_all = 1.0;   // fraction of each order that fills
  double yes_bid = 0.60, yes_ask = 0.62;
  std::string result;
  std::string expiration = iso_of(now_s() + 5 * 3600);   // inside the T+2 entry horizon
  std::map<std::string, json> markets;   // ticker -> {event_ticker, yes_bid, yes_ask} overrides
  double cancel_fill = 0.0;   // additional fills racing a cancel
  std::map<std::string, double> remaining_by_order;
  std::mutex mu;
  MockExchange() {
    server.Get(R"(/trade-api/v2/markets/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      std::string tk = q.matches[1].str();
      double b = yes_bid, a = yes_ask;
      std::string ev = "KXHIGHNY-26SEP08";
      if (markets.count(tk)) { b = markets[tk].value("yes_bid", b); a = markets[tk].value("yes_ask", a); ev = markets[tk].value("event_ticker", ev); }
      json m = {{"ticker", tk}, {"event_ticker", ev}, {"status", result.empty() ? "active" : "finalized"},
                {"exchange_index", 0}, {"close_time", "2099-01-01T00:00:00Z"}, {"expected_expiration_time", expiration},
                {"yes_bid_dollars", fmt_dollars(b)}, {"yes_ask_dollars", fmt_dollars(a)},
                {"yes_bid_size_fp", "20.00"}, {"yes_ask_size_fp", "30.00"}, {"result", result},
                {"price_level_structure", "linear"}, {"price_ranges", json::array({{{"start", "0.01"}, {"end", "0.99"}, {"step", "0.01"}}})}};
      r.set_content(json{{"market", m}}.dump(), "application/json");
    });
    server.Get(R"(/trade-api/v2/series/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      r.set_content(json{{"series", {{"ticker", q.matches[1].str()}, {"category", "Climate and Weather"}, {"fee_type", "quadratic"}, {"fee_multiplier", 1}}}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/balance", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(json{{"balance_dollars", "100.0000"}, {"portfolio_value", 0}, {"updated_ts", now_s() - 3600}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/positions", [&](const httplib::Request&, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      r.set_content(json{{"market_positions", market_positions}}.dump(), "application/json");
    });
    server.Get("/trade-api/v2/portfolio/orders", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"orders":[]})", "application/json");
    });
    server.Get("/trade-api/v2/exchange/user_data_timestamp", [&](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"as_of_time":"2099-01-01T00:00:00Z"})", "application/json");
    });
    server.Post("/trade-api/v2/portfolio/events/orders", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++writes;
      if (!reject_queue.empty()) {
        r.status = 400;
        r.set_content(reject_queue.front(), "application/json");
        reject_queue.pop_front();
        return;
      }
      if (reject || reject_next > 0) {
        if (reject_next > 0) --reject_next;
        r.status = 400;
        r.set_content(reject_body, "application/json");
        return;
      }
      last_order = json::parse(q.body);
      double count = parse_num(last_order.value("count", "0"));
      double filled = std::floor(count * fill_all * 100) / 100;
      r.status = 201;
      remaining_by_order["ord-" + std::to_string(writes.load())] = count - filled;
      r.set_content(json{{"order_id", "ord-" + std::to_string(writes.load())}, {"fill_count", fmt_fp(filled)},
                         {"remaining_count", fmt_fp(count - filled)}}.dump(), "application/json");
    });
    server.Delete(R"(/trade-api/v2/portfolio/events/orders/([^/]+))", [&](const httplib::Request& q, httplib::Response& r) {
      std::lock_guard<std::mutex> lk(mu);
      ++cancels;
      // The exchange reports what it took off the book; a fill that raced the cancel shows as less.
      r.set_content(json{{"order_id", q.matches[1].str()}, {"reduced_by", fmt_fp(std::max(0.0, remaining_by_order[q.matches[1].str()] - cancel_fill))}}.dump(), "application/json");
    });
    server.Get(".*", [](const httplib::Request&, httplib::Response& r) {
      r.set_content(R"({"orders":[],"fills":[],"settlements":[],"transfers":[],"deposits":[],"withdrawals":[],"cursor":""})", "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([&] { server.listen_after_bind(); });
  }
  ~MockExchange() { server.stop(); thread.join(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port) + "/trade-api/v2"; }
  void hold(double position, double exposure) {
    std::lock_guard<std::mutex> lk(mu);
    market_positions = json::array();
    if (position != 0)
      market_positions.push_back({{"ticker", ticker}, {"position_fp", fmt_fp(position)}, {"market_exposure_dollars", fmt_dollars(exposure)}});
  }
};

static std::shared_ptr<Signer> test_signer(const std::string& pem) {
  if (std::system(("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " + pem + " 2>/dev/null").c_str()) != 0)
    return nullptr;
  return std::make_shared<Signer>("test-key", pem);
}

static void test_live_entry_exit_and_budget() {
  std::string pem = "/tmp/desk_sys_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip live test: openssl CLI not available\n"); return; }
  MockExchange x;
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  std::atomic<bool> killed{false}, reconcile_asked{false};
  std::string kill_reason;
  SystematicTrader tr(st, &ex, cfg,
                      SystematicTrader::Hooks{[&] { return killed.load(); },
                                              [&](const std::string& why) { killed = true; kill_reason = why; },
                                              [&] { reconcile_asked = true; }});
  long long now = now_s();
  // The market row the executor reads and the account snapshot validate_entry needs.
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = "2026-09-09T12:00:00Z"; m.yes_bid = 0.60; m.yes_ask = 0.62; m.tick = 0.01;
  m.expected_expiration_time = iso_of(now + 5 * 3600);
  st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s);
  ex.reconcile(true);   // account snapshot + portfolio_sync_ts

  json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.60, 0.62, 20, 30, now)})}};
  // Nothing happens while stopped.
  tr.step(board, now);
  CHECK(x.writes == 0);
  json v = tr.configure(json{{"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"take_profit", 0.06}, {"stop_loss", 0.08}}},
                             {"mode", "live"}, {"running", true}});
  CHECK(v["running"] == true && v["mode"] == "live");
  tr.step(board, now + 1);
  CHECK(x.writes == 1);
  CHECK(x.last_order["side"] == "bid" && x.last_order["price"] == "0.6200" && x.last_order["count"] == "4.00");  // floor(3 / 0.6365)
  CHECK(x.last_order["time_in_force"] == "immediate_or_cancel");
  CHECK(x.last_order.value("post_only", true) == false);
  CHECK(reconcile_asked);
  // The order leads back to the router's decision: the automatic lane recorded the decision it
  // took, and the executor copied that same decision as "submitted" when the order went.
  {
    const json o = st.query("SELECT decision_id FROM orders");
    CHECK(o.size() == 1 && !o[0].value("decision_id", std::string()).empty());
    const std::string did = o.empty() ? std::string() : o[0].value("decision_id", std::string());
    std::set<std::string> events;
    for (const auto& r : st.query("SELECT event, route, model_name, p_yes, bid, ask FROM trade_decisions WHERE decision_id=?", {did})) {
      events.insert(r.value("event", std::string()));
      CHECK(r.value("route", std::string()) == "auto");
      CHECK(r["p_yes"].is_number() && r["ask"].is_number());
    }
    CHECK(events.count("auto") && events.count("submitted"));
  }
  v = tr.view();
  CHECK(v["positions"].size() == 1);
  const json& pos = v["positions"][0];
  CHECK(pos["state"] == "open" && pos["side"] == "yes" && pos["filled"] == 4.0);
  FeeModel fmq{"quadratic", 1};
  double entry_cost = 4 * 0.62 + fmq.fee(0.62, 4, true);
  CHECK_NEAR(pos["entry_cost"].get<double>(), entry_cost, 1e-6);
  CHECK_NEAR(v["spent_today"].get<double>(), entry_cost, 1e-6);
  // The exchange now shows the position; the same candidate must not be bought twice.
  x.hold(4, entry_cost);
  ex.reconcile(false);
  tr.step(board, now + 2);
  CHECK(x.writes == 1);
  // A 0.70 bid leaves less than 6c after BOTH fees (entry cost 0.6365 a contract); the target
  // only triggers at 0.72 -> reduce-only IOC into the bid.
  json board2 = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.70, 0.72, 20, 30, now + 3)})}};
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.70; x.yes_ask = 0.72; }
  tr.step(board2, now + 3);
  CHECK(x.writes == 1);
  board2["markets"][0]["yes_bid"] = .72; board2["markets"][0]["yes_ask"] = .74;
  board2["markets"][0]["no_ask"] = .28; board2["markets"][0]["no_bid"] = .26;
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.72; x.yes_ask = 0.74; }
  ++now;   // the trader ignores a duplicate cycle timestamp
  tr.step(board2, now + 3);
  CHECK(x.writes == 2);
  CHECK(x.last_order["side"] == "ask" && x.last_order["price"] == "0.7200" && x.last_order["count"] == "4.00");
  CHECK(x.last_order["reduce_only"] == true && x.last_order["time_in_force"] == "immediate_or_cancel");
  v = tr.view();
  CHECK(v["positions"].empty());
  CHECK(v["history"].size() == 1);
  const json& h = v["history"][0];
  CHECK(h["state"] == "closed" && std::string(h["exit_reason"]).find("take profit") != std::string::npos);
  double exit_fee = fmq.fee(0.28, 4, true);
  CHECK_NEAR(h["realized_pnl"].get<double>(), 4 * 0.72 - exit_fee - entry_cost, 1e-6);
  CHECK_NEAR(v["performance"]["live"]["realized"].get<double>(), 4 * 0.72 - exit_fee - entry_cost, 1e-6);
  CHECK(v["performance"]["live"]["closed_trades"] == 1 && v["performance"]["live"]["wins"] == 1);
  CHECK(v["cooldowns"].size() == 1);   // the event cools off after an exit
  // Cooling off: the same event is not re-entered.
  x.hold(0, 0);
  ex.reconcile(false);
  tr.step(board, now + 4);
  CHECK(x.writes == 2);
  // Daily budget: with $0.50 left nothing new goes out.
  tr.configure(json{{"settings", {{"cooldown_minutes", 0}, {"daily_budget_dollars", entry_cost + 0.50}}}});
  tr.step(board, now + 5);
  CHECK(x.writes == 2);
  CHECK(st.query("SELECT * FROM proposals WHERE strategy='systematic'").size() == 1);
  // Budget lifted: the next entry goes; the kill switch then stops everything.
  tr.configure(json{{"settings", {{"daily_budget_dollars", 30}}}});
  tr.step(board, now + 6);
  CHECK(x.writes == 3);
  x.hold(4, entry_cost);
  ex.reconcile(false);
  killed = true;
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.90; x.yes_ask = 0.92; }
  json board3 = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, 0.90, 0.92, 20, 30, now + 7)})}};
  tr.step(board3, now + 7);
  CHECK(x.writes == 3);   // no exit, no entry under kill
  killed = false;
  tr.step(board3, now + 8);
  CHECK(x.writes == 4);   // the exit goes once the desk is re-armed
  CHECK(tr.view()["positions"].empty());
  // A 4xx rejection stops the algorithm and asks for the kill switch.
  x.reject = true;
  x.hold(0, 0);
  ex.reconcile(false);
  tr.configure(json{{"settings", {{"cooldown_minutes", 0}}}});
  tr.step(board, now + 9);
  CHECK(x.writes == 5);
  CHECK(killed && kill_reason.find("rejected") != std::string::npos);
  v = tr.view();
  CHECK(v["running"] == false && v["halted"] == true);
  std::remove(pem.c_str());
}

static void test_paper_rules_and_loss_stop() {
  MockExchange x;
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  KalshiClient client(x.base(), nullptr, "tests");   // no key: live is impossible
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  auto board_at = [&](long long t, double bid, double ask, const std::string& status = "active") {
    json r = row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, bid, ask, 20, 30, t);
    r["market_status"] = status;
    return json{{"markets", json::array({r})}};
  };
  bool threw = false;
  try { tr.configure(json{{"mode", "live"}}); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  json v = tr.configure(json{{"mode", "paper"}, {"running", true},
                             {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"stop_loss", 0.08}, {"take_profit", 0.06},
                                           {"max_hold_minutes", 30}, {"daily_loss_stop_dollars", 0.5}, {"cooldown_minutes", 0}, {"paper_slippage", 0.01},
                                           {"stop_grace_seconds", 0}, {"stop_persist_seconds", 0}}}});
  CHECK(v["mode"] == "paper" && v["running"] == true);
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.60; m.yes_ask = 0.62;
  st.upsert_market(m);
  tr.step(board_at(now, 0.60, 0.62), now);
  CHECK(x.writes == 0);   // paper sends nothing
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["mode"] == "paper");
  double price = 0.63;   // ask + slippage
  FeeModel fmq{"quadratic", 1};
  double count = std::floor(3 / (price + fmq.fee(price, 1, true)));
  CHECK_NEAR(v["positions"][0]["filled"].get<double>(), count, 1e-9);
  // Stop: the mid falls to 0.55 (entry 0.63, drop 0.08 >= 0.08); no grace and no persistence in
  // this test, so it fires at once. Exit at 0.53 after slippage.
  long long t = now + 10;
  tr.step(board_at(t, 0.54, 0.56), t);
  v = tr.view();
  CHECK(v["positions"].empty() && v["history"].size() == 1);
  CHECK(v["history"].size() == 1 && std::string(v["history"][0]["exit_reason"]).find("stop") != std::string::npos);
  double pnl = v["history"][0]["realized_pnl"].get<double>();
  CHECK(pnl < 0);
  // That loss is past the $0.50 daily stop: halted, no new entries, until cleared.
  CHECK(v["halted"] == true);
  t = now + 20;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].empty());
  tr.configure(json{{"clear_halt", true}});
  t = now + 21;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].size() == 1);
  CHECK(tr.view()["halted"] == false);   // the stop re-armed from the loss so far
  // Time stop: 30 minutes on the clock. With a cooldown set, the event is not re-entered at once.
  tr.configure(json{{"settings", {{"cooldown_minutes", 5}}}});
  t = now + 21 + 31 * 60;
  tr.step(board_at(t, 0.60, 0.62), t);
  v = tr.view();
  CHECK(v["positions"].empty());
  CHECK(std::string(v["history"][0]["exit_reason"]).find("time stop") != std::string::npos);
  CHECK(v["cooldowns"].size() == 1);
  // Settlement: a position held into the result pays $1 a contract when its side wins.
  tr.configure(json{{"clear_halt", true}, {"settings", {{"max_hold_minutes", 0}, {"cooldown_minutes", 0}}}});
  t = now + 4000;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].size() == 1);
  st.exec("UPDATE markets SET result='yes', status='finalized' WHERE ticker=?", {x.ticker});
  t = now + 4010;
  tr.step(board_at(t, 0.60, 0.62, "finalized"), t);
  v = tr.view();
  CHECK(v["positions"].empty());
  CHECK(std::string(v["history"][0]["exit_reason"]).find("settled in our favour") != std::string::npos);
  double c = v["history"][0]["count"].get<double>();
  CHECK_NEAR(v["history"][0]["realized_pnl"].get<double>(), c - v["history"][0]["entry_cost"].get<double>(), 1e-6);
  // Hold-value rule: a bid richer than p(win) is money on the table.
  st.exec("UPDATE markets SET result='', status='active' WHERE ticker=?", {x.ticker});
  tr.configure(json{{"clear_halt", true}, {"settings", {{"take_profit", 0}, {"stop_loss", 0}}}});
  t = now + 5000;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].size() == 1);
  t = now + 5010;
  tr.step(board_at(t, 0.74, 0.76), t);
  v = tr.view();
  CHECK(v["positions"].empty());
  CHECK(std::string(v["history"][0]["exit_reason"]).find("more than the position is worth") != std::string::npos);
  // Stopping keeps managing what is open but opens nothing new.
  tr.configure(json{{"clear_halt", true}});
  t = now + 6000;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].size() == 1);
  tr.configure(json{{"running", false}});
  t = now + 6010;
  tr.step(board_at(t, 0.74, 0.76), t);
  v = tr.view();
  CHECK(v["positions"].empty() && v["running"] == false);
  t = now + 6020;
  tr.step(board_at(t, 0.60, 0.62), t);
  CHECK(tr.view()["positions"].empty());
  // Persistence: a new trader on the same store restores settings and the ledger.
  SystematicTrader tr2(st, &ex, cfg);
  json v2 = tr2.view();
  CHECK(v2["settings"]["daily_loss_stop_dollars"] == 0.5);
  CHECK(v2["history"].size() == v["history"].size());
  CHECK(v2["performance"]["paper"]["closed_trades"] == v["performance"]["paper"]["closed_trades"]);
}

// A patient market stop must not secretly remain an 8-cent or model-value stop.
static void test_patient_stop_policy() {
  Store st(":memory:");Config cfg; cfg.systematic_settings = algo_routes();SystematicTrader tr(st,nullptr,cfg);
  const auto now=now_s();
  const std::string ticker="KXHIGHNY-26SEP08-B70";
  auto board_at=[&](long long t,double bid,double ask,double p=.70) {
    return json{{"markets",json::array({row(ticker,"KXHIGHNY-26SEP08","Climate and Weather",p,p,.05,bid,ask,20,30,t)})}};
  };
  Market m;m.ticker=ticker;m.event_ticker="KXHIGHNY-26SEP08";m.series_ticker="KXHIGHNY";m.status="active";m.close_time=iso_of(now+7200);st.upsert_market(m);
  tr.configure({{"mode","paper"},{"running",true},{"settings",{
    {"kelly_fraction",0},{"per_trade_dollars",3},{"capital_budget_dollars",40},
    {"daily_loss_stop_dollars",.5},{"daily_loss_stop_enabled",false},
    {"stop_loss_mode","fraction"},{"stop_loss_fraction",.70},
    {"model_value_exit",false},{"thesis_stop_enabled",false},{"max_hold_minutes",0},
    {"stop_grace_seconds",0},{"stop_persist_seconds",30},{"paper_slippage",0},{"cooldown_minutes",60}
  }}});
  tr.step(board_at(now,.60,.62),now);
  CHECK(tr.view()["positions"].size()==1);
  // More than the old 8-cent stop and the $0.50 daily loss: neither may fire.
  tr.step(board_at(now+10,.49,.51,.05),now+10);
  CHECK(tr.view()["positions"].size()==1 && !tr.view()["halted"].get<bool>());
  // A brief 70% breach recovers before persistence, so keep holding.
  tr.step(board_at(now+20,.17,.19,.05),now+20);
  tr.step(board_at(now+25,.49,.51,.05),now+25);
  CHECK(tr.view()["positions"].size()==1);
  tr.step(board_at(now+60,.17,.19,.05),now+60);
  CHECK(tr.view()["positions"].size()==1);
  tr.step(board_at(now+91,.17,.19,.05),now+91);
  auto v=tr.view();
  CHECK(v["positions"].empty() && !v["halted"].get<bool>());
  CHECK(v["history"].size()==1 && std::string(v["history"][0]["exit_reason"]).find("capital loss stop")!=std::string::npos);
  SystematicTrader restored(st,nullptr,cfg);
  CHECK(restored.view()["settings"]["daily_loss_stop_enabled"]==false);
  CHECK(restored.view()["settings"]["thesis_stop_enabled"]==false);
}

static void test_maker_entry_lifecycle() {
  std::string pem = "/tmp/desk_sys_key2.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockExchange x;
  x.fill_all = 0;   // maker orders rest
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600); m.yes_bid = 0.60; m.yes_ask = 0.64; m.tick = 0.01;
  st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s);
  ex.reconcile(true);
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.60; x.yes_ask = 0.64; }
  // Taker edge 0.66-0.64-0.0161 fails the bar; maker at 0.61 clears it.
  json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now)})}};
  tr.configure(json{{"mode", "live"}, {"running", true}, {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"entry_ttl_seconds", 60}}}});
  tr.step(board, now);
  CHECK(x.writes == 1);
  CHECK(x.last_order["side"] == "bid" && x.last_order["price"] == "0.6100" && x.last_order["post_only"] == true);
  json v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "entering");
  CHECK_NEAR(v["spent_today"].get<double>(), 4 * 0.61, 1e-9);   // reserved: 4 contracts, no maker fee
  // Still resting inside the TTL: left alone.
  tr.step(board, now + 30);
  CHECK(x.cancels == 0);
  // Past the TTL, unfilled: pulled, and the reservation is given back.
  tr.step(board, now + 61);
  CHECK(x.cancels == 1);
  CHECK(x.writes == 1);   // not re-posted in the same breath
  v = tr.view();
  CHECK(v["positions"].empty());
  CHECK(v["history"].size() == 1 && v["history"][0]["state"] == "canceled");
  CHECK_NEAR(v["spent_today"].get<double>(), 0.0, 1e-9);
  // Posted again after the pause; this time it fills while resting (reconcile reports the fill and the position).
  tr.step(board, now + 80);
  CHECK(x.writes == 2);
  st.exec("UPDATE orders SET fill_count=4, remaining_count=0, status='executed' WHERE order_id='ord-2'");
  x.hold(4, 2.44);
  ex.reconcile(false);
  tr.step(board, now + 90);
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "open");
  CHECK_NEAR(v["positions"][0]["entry_cost"].get<double>(), 4 * 0.61, 1e-9);
  CHECK_NEAR(v["spent_today"].get<double>(), 4 * 0.61, 1e-9);
  // Released to manual: the algorithm forgets it and the engine may manage it again.
  tr.configure(json{{"release", x.ticker}});
  CHECK(tr.view()["positions"].empty());
  CHECK(tr.owned_tickers().count(x.ticker) == 0);   // released positions immediately belong to manual management
  std::remove(pem.c_str());
}

// Session 12: the entry ladder and the exits that no longer read a thin book as the market.
// A resting algorithm entry is pulled as soon as its model's strategy mode (config/desk.json, or the
// Strategies tab) caps the model below auto: the router already stopped offering it, and an order
// already on the exchange must not outlive that. Inside its TTL, with the edge intact.
static void test_mode_cap_pulls_resting_entry() {
  std::string pem = "/tmp/desk_sys_key_modecap.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockExchange x;
  x.fill_all = 0;   // maker orders rest
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600); m.yes_bid = 0.60; m.yes_ask = 0.64; m.tick = 0.01;
  st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s);
  ex.reconcile(true);
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.60; x.yes_ask = 0.64; }
  json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now)})}};
  tr.configure(json{{"mode", "live"}, {"running", true}, {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"entry_ttl_seconds", 600}}}});
  tr.step(board, now);
  CHECK(x.writes == 1 && x.last_order["post_only"] == true);
  tr.step(board, now + 10);
  CHECK(x.cancels == 0);                       // resting, inside its TTL, edge intact: left alone
  cfg.strategy_modes["test_model"] = "research";
  tr.step(board, now + 20);
  CHECK(x.cancels == 1);                       // parked in research: the order comes off
  json v = tr.view();
  CHECK(v["positions"].empty());
  bool said = false;
  for (const auto& e : v["log"]) said = said || e.value("msg", std::string()).find("research mode") != std::string::npos;
  for (const auto& h : v["history"]) said = said || h.value("exit_reason", std::string()).find("research mode") != std::string::npos ||
                                               h.value("note", std::string()).find("research mode") != std::string::npos;
  CHECK(said);
  std::remove(pem.c_str());
}

static void test_ladder_and_exit_rules() {
  std::string pem = "/tmp/desk_sys_key3.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockExchange x;
  x.fill_all = 0;
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600); m.yes_bid = 0.60; m.yes_ask = 0.66; m.tick = 0.01;
  st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s);
  ex.reconcile(true);
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.60; x.yes_ask = 0.66; }
  auto board_at = [&](long long t, double p, double bid, double ask) {
    return json{{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", p, p + 0.01, 0.03, bid, ask, 20, 30, t)})}};
  };
  tr.configure(json{{"mode", "live"}, {"running", true},
                    {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"entry_ttl_seconds", 60}, {"ladder_ticks", 3},
                                  {"stop_loss", 0.10}, {"stop_grace_seconds", 90}, {"stop_persist_seconds", 30}, {"exit_ttl_seconds", 120},
                                  {"take_profit", 0.06}, {"cooldown_minutes", 0}}}});
  // 1. p 0.70 on a 0.60/0.66 book: the taker edge fails the floor, the maker posts at bid + tick.
  tr.step(board_at(now, 0.70, 0.60, 0.66), now);
  CHECK(x.writes == 1 && x.last_order["price"] == "0.6100" && x.last_order["post_only"] == true);
  // Pulled after the TTL; each re-post climbs one tick while the edge floor allows: 0.62, 0.63, 0.64.
  const char* expect[] = {"0.6200", "0.6300", "0.6400"};
  long long t = now;
  for (int k = 0; k < 3; ++k) {
    t += 61;
    tr.step(board_at(t, 0.70, 0.60, 0.66), t);
    CHECK(x.cancels == k + 1);
    t += 19;
    tr.step(board_at(t, 0.70, 0.60, 0.66), t);
    CHECK(x.writes == k + 2 && x.last_order["price"] == expect[k] && x.last_order["post_only"] == true);
  }
  // The fourth pull is past the top of the ladder: the market is left alone for a while.
  t += 61;
  tr.step(board_at(t, 0.70, 0.60, 0.66), t);
  CHECK(x.cancels == 4);
  t += 19;
  tr.step(board_at(t, 0.70, 0.60, 0.66), t);
  CHECK(x.writes == 4);
  json v = tr.view();
  bool paused = false;
  for (const auto& e : v["log"]) if (std::string(e["msg"]).find("leaving it for 15 min") != std::string::npos) paused = true;
  CHECK(paused);
  // After the pause the ladder starts again from the bottom, and this time the post fills.
  x.fill_all = 1.0;
  t += 901;
  tr.step(board_at(t, 0.70, 0.60, 0.66), t);
  CHECK(x.writes == 5 && x.last_order["price"] == "0.6100");
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "open");
  double held = v["positions"][0]["filled"].get<double>();
  x.hold(held, held * 0.61);
  ex.reconcile(false);
  const long long filled_at = t;
  // 2. Grace: ten seconds after the fill the book shows 0.40 / 0.60 (our own fill emptied the
  //    bid). Entry 0.61 against a mid of 0.50 is past the stop, but nothing happens yet.
  x.fill_all = 0;
  tr.step(board_at(filled_at + 10, 0.70, 0.40, 0.60), filled_at + 10);
  CHECK(x.writes == 5);
  // Past the grace the stop is seen, but it must persist.
  tr.step(board_at(filled_at + 100, 0.70, 0.40, 0.60), filled_at + 100);
  CHECK(x.writes == 5);
  // It recovers: the stop clock resets. Then it is seen again.
  tr.step(board_at(filled_at + 115, 0.70, 0.60, 0.64), filled_at + 115);
  tr.step(board_at(filled_at + 130, 0.70, 0.40, 0.60), filled_at + 130);
  CHECK(x.writes == 5);
  // Persisted 30s: the bid at 0.40 is a hole (0.10 under the mid), so the exit is a resting sell
  // one tick inside the offer, 0.59, not a dump into 0.40.
  tr.step(board_at(filled_at + 161, 0.70, 0.40, 0.60), filled_at + 161);
  CHECK(x.writes == 6);
  CHECK(x.last_order["side"] == "ask" && x.last_order["price"] == "0.5900" && x.last_order["post_only"] == true);
  CHECK(x.last_order["time_in_force"] == "good_till_canceled" && !x.last_order.value("reduce_only", false));
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "exiting" && v["positions"][0]["exit_style"] == "maker");
  // Resting exit unfilled inside its TTL: left alone; past it: pulled, and the next one rests a
  // tick lower (the exit ladder).
  int cancels_before = x.cancels;
  tr.step(board_at(filled_at + 200, 0.70, 0.40, 0.60), filled_at + 200);
  CHECK(x.cancels == cancels_before);
  tr.step(board_at(filled_at + 290, 0.70, 0.40, 0.60), filled_at + 290);
  CHECK(x.cancels == cancels_before + 1);
  CHECK(x.writes == 7 && x.last_order["price"] == "0.5800" && x.last_order["post_only"] == true);
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "exiting" && v["positions"][0]["exit_steps"] == 1);
  // Twice the stop on the mid (entry 0.61, mid 0.41) while the sell rests: the resting exit is
  // pulled and the position goes into whatever bid there is, reduce-only IOC.
  x.fill_all = 1.0;
  tr.step(board_at(filled_at + 301, 0.70, 0.40, 0.42), filled_at + 301);
  CHECK(x.cancels == cancels_before + 2 && x.writes == 8);
  CHECK(x.last_order["side"] == "ask" && x.last_order["price"] == "0.4000" && x.last_order["reduce_only"] == true);
  v = tr.view();
  CHECK(v["positions"].empty() && v["history"].size() >= 1);
  CHECK(v["history"].size() >= 1 && std::string(v["history"][0]["exit_reason"]).find("twice the stop") != std::string::npos);
  // 3. Thesis stop: the model itself drops under entry - stop while the book has not moved far.
  x.hold(0, 0);
  ex.reconcile(false);
  tr.configure(json{{"settings", {{"stop_grace_seconds", 0}, {"stop_persist_seconds", 0}}}});
  int w = x.writes;
  tr.step(board_at(filled_at + 400, 0.70, 0.60, 0.66), filled_at + 400);
  CHECK(x.writes == w + 1);
  v = tr.view();
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "open");
  held = v["positions"][0]["filled"].get<double>();
  x.hold(held, held * 0.61);
  ex.reconcile(false);
  // p(win) 0.50 on a 0.48/0.52 book: the bid net (0.46) pays less than the position is worth, so
  // the hold-value rule stays quiet; entry - p(win) = 0.11 >= 0.10 is the thesis stop, into a bid
  // that is deep and next to the mid.
  tr.step(board_at(filled_at + 420, 0.50, 0.48, 0.52), filled_at + 420);
  CHECK(x.writes == w + 2 && x.last_order["price"] == "0.4800" && x.last_order["reduce_only"] == true);
  v = tr.view();
  CHECK(v["positions"].empty() && v["history"].size() >= 1 && std::string(v["history"][0]["exit_reason"]).find("thesis stop") != std::string::npos);
  std::remove(pem.c_str());
}

// A partial fill with a third contract filling during the cancel: the tail is pulled before the
// TTL, the raced fill is counted from the exchange's reduced_by, and the recorded fills price it.
static void test_partial_entry_cancel_race() {
  const std::string pem = "/tmp/desk_sys_race_key.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockExchange x; x.fill_all = 0;
  Store st(":memory:"); Config cfg; cfg.systematic_settings = algo_routes(); cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests"); Executor ex(cfg, st, client); SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY";
  m.status = "active"; m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600);
  m.yes_bid = .60; m.yes_ask = .64; m.tick = .01; st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s); ex.reconcile(true);
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = .60; x.yes_ask = .64; }
  json board = {{"markets", json::array({row(x.ticker, m.event_ticker, s.category, .66, .67, .03, .60, .64, 20, 30, now)})}};
  tr.configure({{"mode", "live"}, {"running", true}, {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}}}});
  tr.step(board, now);
  CHECK(x.writes == 1);
  st.exec("UPDATE orders SET fill_count=2, remaining_count=2 WHERE order_id='ord-1'");
  { std::lock_guard<std::mutex> lk(x.mu); x.remaining_by_order["ord-1"] = 2; x.cancel_fill = 1; }
  x.hold(3, 1.79);
  // The account snapshot still says two, while a third fills during cancellation.
  Position position; position.ticker = x.ticker; position.position = 2; position.exposure = 1.19;
  st.replace_positions({position});
  Fill f; f.fill_id = "partial-a"; f.order_id = "ord-1"; f.ticker = x.ticker; f.outcome_side = "yes";
  f.count = 2; f.yes_price = .59; f.no_price = .41; f.fee = .01; f.ts = now + 1;
  st.upsert_fill(f);
  f.fill_id = "partial-b"; f.count = 1; f.yes_price = .60; f.no_price = .40; f.fee = 0; st.upsert_fill(f);
  tr.step(board, now + 10);
  json v = tr.view();
  CHECK(x.cancels == 1);   // partial entry tail canceled before TTL, including its raced fill
  CHECK(v["positions"].size() == 1 && v["positions"][0]["state"] == "open");
  if (!v["positions"].empty()) {
    CHECK_NEAR(v["positions"][0]["filled"].get<double>(), 3, 1e-9);
    CHECK_NEAR(v["positions"][0]["entry_price"].get<double>(), 1.78 / 3, 1e-9);
    CHECK_NEAR(v["positions"][0]["entry_cost"].get<double>(), 1.79, 1e-9);
    CHECK(v["positions"][0]["filled_ts"] == now + 1);
  }
  CHECK_NEAR(v["spent_today"].get<double>(), 1.79, 1e-9);
  CHECK(v["execution_stats"]["live"]["filled"] == 1);
  std::remove(pem.c_str());
}

// Locked baskets: the finders leave ladder_arb legs pending in Signals; a running algorithm with
// arbitrage on sends every leg itself, and the entry policy must let the second leg of the same
// basket through even though the first one is already working in the event.
static void test_basket_send() {
  std::string pem = "/tmp/desk_sys_key3.pem";
  auto signer = test_signer(pem);
  if (!signer) return;
  MockExchange x;
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Series sr; sr.ticker = "KXHIGHLAX"; sr.category = "Climate and Weather"; sr.fee_type = "quadratic"; sr.fee_multiplier = 1;
  st.upsert_series(sr);
  const std::string EV = "KXHIGHLAX-26SEP08";
  auto mk = [&](const std::string& tk, double lo, double bid, double ask) {
    Market m; m.ticker = tk; m.event_ticker = EV; m.series_ticker = "KXHIGHLAX"; m.status = "active"; m.strike_type = "greater";
    m.floor_strike = lo; m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600);
    m.yes_bid = bid; m.yes_ask = ask; m.no_bid = 1 - ask; m.no_ask = 1 - bid; m.yes_bid_size = 30; m.yes_ask_size = 30; m.tick = 0.01;
    st.upsert_market(m);
    return m;
  };
  std::vector<Market> ms{mk(EV + "-T83", 83, 0.60, 0.63), mk(EV + "-T79", 79, 0.50, 0.52)};
  x.markets[EV + "-T83"] = {{"event_ticker", EV}, {"yes_bid", 0.60}, {"yes_ask", 0.63}};
  x.markets[EV + "-T79"] = {{"event_ticker", EV}, {"yes_bid", 0.50}, {"yes_ask", 0.52}};
  ex.reconcile(true);
  std::vector<const Market*> ptrs{&ms[0], &ms[1]};
  auto arb = find_dominance_arb(st, cfg, EV, ptrs, 20.0);
  CHECK(arb.has_value());
  if (!arb) return;
  std::string basket = "basket " + EV + "@" + std::to_string(now / 60);
  for (const auto& leg : arb->legs) {
    Proposal p;
    p.ticker = leg.ticker; p.event_ticker = EV; p.series_ticker = "KXHIGHLAX"; p.strategy = "ladder_arb"; p.mode = "auto";
    p.outcome_side = leg.side; p.exec_style = "taker"; p.status = "pending"; p.price = leg.price; p.count = 3;
    p.p_model = leg.side == "no" ? 1.0 - leg.yes_bid : leg.yes_ask; p.fee_per = leg.fee_per;
    p.net_edge = arb->edge_per_contract / 2; p.yes_bid = leg.yes_bid; p.yes_ask = leg.yes_ask; p.cluster = "wx|2026-09-08";
    p.note = arb->note + " | " + basket + " | leg " + leg.side + " " + leg.label + " | all legs or none";
    st.insert_proposal(p);
  }
  // Not running: nothing goes.
  tr.step(json{{"markets", json::array()}}, now);
  CHECK(x.writes == 0);
  tr.configure(json{{"mode", "live"}, {"running", true}});
  tr.step(json{{"markets", json::array()}}, now + 1);
  CHECK(x.writes == 2);
  int submitted = 0;
  for (const auto& p : st.proposals("", 20)) if (p.strategy == "ladder_arb" && p.status == "submitted") ++submitted;
  CHECK(submitted == 2);
  json v = tr.view();
  // Baskets have their own budget (M4): the model lanes' daily spend is untouched, and the
  // basket's stake shows as the arbitrage lane's volume today.
  CHECK(v["spent_today"].get<double>() == 0);
  double arb_volume = -1;
  for (const auto& l : v["lanes"]) if (l["model"] == "ladder_arb") arb_volume = l["today"]["volume"].get<double>();
  CHECK(arb_volume > 0);
  bool logged = false;
  for (const auto& e : v["log"]) if (std::string(e["msg"]).find("2/2 legs sent") != std::string::npos) logged = true;
  CHECK(logged);
  // Arbitrage switched off: pending legs are left for the AUTO lane or your click.
  Proposal extra; extra.ticker = EV + "-T83"; extra.event_ticker = EV; extra.series_ticker = "KXHIGHLAX"; extra.strategy = "ladder_arb";
  extra.mode = "auto"; extra.outcome_side = "no"; extra.exec_style = "taker"; extra.status = "pending"; extra.price = 0.40; extra.count = 1;
  extra.fee_per = 0.0168; extra.net_edge = 0.02; extra.note = "basket " + EV + "@later | leg";
  st.insert_proposal(extra);
  tr.configure(json{{"settings", {{"arbitrage", false}}}});
  tr.step(json{{"markets", json::array()}}, now + 2);
  CHECK(x.writes == 2);
  std::remove(pem.c_str());
}

// Shared account holdings are not strategy holdings. These fixtures put manual quantity
// and manual orders on the same ticker as one strategy-owned lot.
static void test_manual_ownership_isolation() {
  const std::string pem = "/tmp/desk_sys_ownership_key.pem";
  auto signer = test_signer(pem); if (!signer) return;
  MockExchange x; Store st(":memory:"); Config cfg; cfg.systematic_settings = algo_routes(); cfg.bankroll_dollars = 100;
  KalshiClient client(x.base(), signer, "tests"); Executor ex(cfg, st, client); SystematicTrader tr(st, &ex, cfg);
  const long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY";
  m.status = "active"; m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 3600);
  m.yes_bid = .60; m.yes_ask = .64; m.tick = .01; st.upsert_market(m);
  Series sr; sr.ticker = m.series_ticker; sr.category = "Climate and Weather"; sr.fee_type = "quadratic"; sr.fee_multiplier = 1;
  st.upsert_series(sr); ex.reconcile(true);
  auto board = [&](long long ts, double bid = .60, double ask = .64) {
    return json{{"markets", json::array({row(x.ticker, m.event_ticker, sr.category, .70, .71, .03, bid, ask, 20, 30, ts)})}};
  };
  auto hold = [&](double n) { x.hold(n, n * .61); Position p; p.ticker = x.ticker; p.position = n; p.exposure = n * .61; st.replace_positions({p}); };
  auto seed = [&](const std::string& state, double owned = 3) {
    st.exec("INSERT INTO systematic_trades(ticker,event_ticker,series_ticker,side,state,mode,opened_ts,filled_ts,entry_order_id,entry_style,entry_price,count,filled,entry_cost,entry_fee,realized_pnl,exit_fee,exit_proceeds,unrealized,fair_strategy) VALUES(?,?,?,?,?,'live',?,?,'owned-entry','maker',.61,?,?,?,0,0,0,0,0,'test_model')",
            {x.ticker, m.event_ticker, m.series_ticker, std::string("yes"), state, now - 120, now - 120, owned, state == "entering" ? 0.0 : owned, state == "entering" ? 0.0 : owned * .61});
    Order o; o.order_id = "owned-entry"; o.ticker = x.ticker; o.outcome_side = "yes"; o.status = "executed";
    o.fill_count = owned; o.initial_count = owned; o.yes_price = .61; o.no_price = .39; st.upsert_order(o);
  };
  // With no owned row, neither manual positions nor manual orders are an algorithm mandate.
  hold(10); tr.step(board(now, .85, .87), now); CHECK(x.writes == 0 && x.cancels == 0);
  seed("open");
  st.kv_set("portfolio_sync_ts", std::to_string(now + 100));
  tr.step(board(now + 1), now + 1);
  CHECK_NEAR(tr.view()["positions"][0]["filled"].get<double>(), 3, 1e-9);
  CHECK_NEAR(tr.view()["positions"][0]["entry_cost"].get<double>(), 1.83, 1e-9);
  Order manual; manual.order_id = "manual-resting"; manual.ticker = x.ticker; manual.status = "resting";
  manual.remaining_count = 2; manual.initial_count = 2; st.upsert_order(manual);
  tr.step(board(now + 12, .85, .87), now + 12);
  CHECK(x.writes == 0 && x.cancels == 0);
  CHECK(st.query("SELECT status FROM orders WHERE order_id='manual-resting'")[0]["status"] == "resting");
  st.exec("UPDATE orders SET status='canceled' WHERE order_id='manual-resting'"); // the user cancels it
  tr.step(board(now + 24, .85, .87), now + 24);
  CHECK(x.writes == 1 && x.last_order["count"] == "3.00"); // seven manual contracts are not sold
  CHECK(tr.view()["positions"].empty());
  // If an external sale reduces the available position, cap our lot downward without claiming
  // the unrelated fill as strategy P&L. Fresh external increases never enlarge it again.
  seed("open"); hold(2); st.kv_set("portfolio_sync_ts", std::to_string(now + 200));
  Fill external; external.fill_id = "manual-fill"; external.order_id = "manual-exit"; external.ticker = x.ticker;
  external.outcome_side = "no"; external.count = 50; external.no_price = .01; external.yes_price = .99; external.ts = now;
  st.upsert_fill(external);
  tr.step(board(now + 36), now + 36);
  CHECK_NEAR(tr.view()["positions"][0]["filled"].get<double>(), 2, 1e-9);
  CHECK_NEAR(tr.view()["positions"][0]["realized_pnl"].get<double>(), 0, 1e-9);
  hold(0); tr.step(board(now + 48), now + 48);
  CHECK(tr.view()["positions"].empty());
  CHECK(st.query("SELECT state FROM systematic_trades ORDER BY id DESC LIMIT 1")[0]["state"] == "released");
  // A released resting entry withdraws only its own remaining order and refunds its reservation.
  seed("entering", 4); st.exec("UPDATE orders SET status='resting',fill_count=0,remaining_count=4 WHERE order_id='owned-entry'");
  x.remaining_by_order["owned-entry"] = 4; st.upsert_order(manual);
  st.kv_set("sys:spent:" + tr.state()["day"].get<std::string>(), "2.44");
  int cancellations = x.cancels;
  tr.configure({{"release", x.ticker}});
  CHECK(x.cancels == cancellations + 1);
  CHECK(st.query("SELECT status FROM orders WHERE order_id='manual-resting'")[0]["status"] == "resting");
  CHECK(tr.view()["positions"].empty() && tr.owned_tickers().count(x.ticker) == 1); // earlier truly closed lot still has reconciliation shield
  CHECK_NEAR(tr.view()["spent_today"].get<double>(), 0, 1e-9);
  std::remove(pem.c_str());
}

// The line Sam asked for: a position he opened himself - from the dashboard ticket or from the
// Kalshi app - is not an invitation for the algorithm to trade that event. It must not open a new
// trade there (adding to his side, or worse, taking the other side and netting his contracts
// away), and the screen he reads must say so rather than showing a green "algo" row the sender
// will silently skip. All three paths (screen, live, paper) take the set from one place.
static void test_manual_position_blocks_new_entry() {
  const std::string pem = "/tmp/desk_sys_manual_entry_key.pem";
  auto signer = test_signer(pem); if (!signer) return;
  MockExchange x; Store st(":memory:"); Config cfg; cfg.systematic_settings = algo_routes(); cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests"); Executor ex(cfg, st, client); SystematicTrader tr(st, &ex, cfg);
  const long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY";
  m.status = "active"; m.close_time = "2099-01-01T00:00:00Z"; m.expected_expiration_time = iso_of(now + 5 * 3600);
  m.yes_bid = .60; m.yes_ask = .64; m.tick = .01; st.upsert_market(m);
  Series sr; sr.ticker = m.series_ticker; sr.category = "Climate and Weather";
  sr.fee_type = "quadratic"; sr.fee_multiplier = 1; st.upsert_series(sr); ex.reconcile(true);
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = .60; x.yes_ask = .62; }
  // A candidate the algorithm would certainly take: 0.70 - 0.62 - fee is well inside its band.
  json board = {{"markets", json::array({row(x.ticker, m.event_ticker, sr.category, .70, .72, .05, .60, .62, 20, 30, now)})}};
  tr.configure({{"mode", "live"}, {"running", true}, {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}}}});

  // Nothing held: it is the algorithm's to take, and the screen says so.
  json before = tr.screen(board, now);
  auto A = find_row(before, x.ticker, "yes");
  CHECK(A && (*A)["verdict"] == "algo");

  // Now Sam holds ten contracts in that event, bought outside this desk. No systematic_trades row
  // exists for them, so nothing but the account position marks the event as his.
  x.hold(10, 10 * .61);
  Position his; his.ticker = x.ticker; his.position = 10; his.exposure = 10 * .61;
  st.replace_positions({his});

  json after = tr.screen(board, now + 1);
  A = find_row(after, x.ticker, "yes");
  CHECK(A && (*A)["verdict"] == "blocked");
  auto N = find_row(after, x.ticker, "no");           // and it cannot take the other side either
  CHECK(N && (*N)["verdict"] == "blocked");
  tr.step(board, now + 2);
  CHECK(x.writes == 0);                                // the sender agrees with the screen
  CHECK(tr.view()["positions"].empty());               // and claims none of his contracts

  // Paper runs the same rule, so the paper ledger stays a forecast of live rather than a
  // different strategy that trades events live would refuse.
  tr.configure({{"mode", "paper"}, {"running", true}});
  tr.step(board, now + 3);
  CHECK(tr.view()["positions"].empty());
  std::remove(pem.c_str());
}

// Settings named in config/desk.json are applied over whatever the dashboard last saved, so a
// durable preference lives in a versioned file instead of only in a database blob.
static void test_config_seeds_settings() {
  Store st(":memory:");
  Config cfg;
  // What was saved from the dashboard.
  {
    SystematicTrader saved(st, nullptr, cfg);
    saved.configure({{"settings", {{"stop_loss_mode", "price"}, {"stop_loss", .08},
                                   {"daily_loss_stop_dollars", 6.0}, {"per_trade_dollars", 3.0}}}});
    CHECK(saved.view()["settings"]["stop_loss_mode"] == "price");
  }
  // What the config file asks for. Only the named keys move.
  cfg.systematic_settings = {{"stop_loss_mode", "fraction"}, {"stop_loss_fraction", .70},
                             {"daily_loss_stop_enabled", false}};
  SystematicTrader tr(st, nullptr, cfg);
  json set = tr.view()["settings"];
  CHECK(set["stop_loss_mode"] == "fraction");
  CHECK_NEAR(set["stop_loss_fraction"].get<double>(), .70, 1e-9);
  CHECK(set["daily_loss_stop_enabled"] == false);
  CHECK_NEAR(set["per_trade_dollars"].get<double>(), 3.0, 1e-9);   // untouched key keeps its save
  CHECK_NEAR(set["stop_loss"].get<double>(), .08, 1e-9);           // and so does an unnamed one

  // A bad config must not take the algorithm's saved settings down with it.
  cfg.systematic_settings = {{"stop_loss_fraction", 9.9}};
  SystematicTrader bad(st, nullptr, cfg);
  CHECK_NEAR(bad.view()["settings"]["stop_loss_fraction"].get<double>(), .70, 1e-9);
  cfg.systematic_settings = {{"no_such_setting", 1}};
  SystematicTrader unknown(st, nullptr, cfg);
  CHECK(unknown.view()["settings"]["stop_loss_mode"] == "fraction");
}


// Routes are four named states, never booleans (route.h). The strict parser refuses anything else;
// the tolerant reader keeps the pre-route booleans meaning what they meant and never turns an
// unreadable value into "auto".
static void test_routes() {
  CHECK(parse_route("off") == RouteMode::Off && parse_route("research") == RouteMode::Research &&
        parse_route("manual") == RouteMode::Manual && parse_route("auto") == RouteMode::Auto);
  for (const char* bad : {"", "yes", "Auto", "true", "sometimes"}) {
    bool threw = false;
    try { parse_route(bad); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
  }
  json s = {{"a", "auto"}, {"m", "manual"}, {"r", "research"}, {"o", "off"}, {"t", true}, {"f", false},
            {"junk", "sometimes"}, {"n", 3}};
  CHECK(route_setting(s, "a") == RouteMode::Auto && route_setting(s, "m") == RouteMode::Manual);
  CHECK(route_setting(s, "r") == RouteMode::Research && route_setting(s, "o") == RouteMode::Off);
  CHECK(route_setting(s, "t") == RouteMode::Auto && route_setting(s, "f") == RouteMode::Manual);
  CHECK(route_setting(s, "junk") == RouteMode::Off && route_setting(s, "n") == RouteMode::Off);
  CHECK(route_setting(s, "missing") == RouteMode::Off && route_setting(s, "missing", RouteMode::Manual) == RouteMode::Manual);
  CHECK(std::string(route_name(RouteMode::Research)) == "research");
  // Data follows the route, execution does not: every route but off is captured, only auto sends.
  CHECK(route_captures_data(RouteMode::Auto) && route_captures_data(RouteMode::Manual) && route_captures_data(RouteMode::Research));
  CHECK(!route_captures_data(RouteMode::Off));
  CHECK(route_sends_unattended(RouteMode::Auto) && !route_sends_unattended(RouteMode::Manual) &&
        !route_sends_unattended(RouteMode::Research) && !route_sends_unattended(RouteMode::Off));
  // The settings validator accepts research and still migrates the booleans.
  json d = systematic_defaults();
  CHECK(validate_systematic_settings(json{{"crypto", "research"}}, d)["crypto"] == "research");
  CHECK(validate_systematic_settings(json{{"crypto", true}}, d)["crypto"] == "auto");
  bool threw = false;
  std::string msg;
  try { validate_systematic_settings(json{{"weather", "yes"}}, d); } catch (const std::invalid_argument& e) { threw = true; msg = e.what(); }
  CHECK(threw && msg.find("research") != std::string::npos);
}

// One market, four routes: off is blocked, research is priced and recorded but offered to nobody
// (and outranks the thresholds), manual goes to you, auto to the algorithm. A research row can
// never come out as algo or manual.
static void test_screen_routes() {
  long long now = now_s();
  // Edge 0.74 - 0.62 - 0.0165 = 0.1035, 0.0785 after the buffer: over the algorithm's floor, under
  // its cap, and over the manual lane's own floor, so every route shows through unobstructed.
  json m = row("KXHIGHNY-26SEP08-B70", "KXHIGHNY-26SEP08", "Climate and Weather", 0.74, 0.75, 0.05, 0.60, 0.62, 20, 30, now);
  json board = {{"markets", json::array({m})}};
  auto verdict = [&](const char* route) {
    json s = algo_settings();
    s["kelly_fraction"] = 0;
    s["weather"] = route;
    json sc = systematic_screen(board, s, now);
    auto r = find_row(sc, "KXHIGHNY-26SEP08-B70", "yes");
    return r ? (*r)["verdict"].get<std::string>() : std::string("missing");
  };
  CHECK(verdict("auto") == "algo");
  CHECK(verdict("manual") == "manual");
  CHECK(verdict("research") == "research");
  CHECK(verdict("off") == "blocked");
  json s = algo_settings();
  s["weather"] = "research";
  json sc = systematic_screen(board, s, now);
  CHECK(sc["counts"]["research"] == 2 && sc["counts"]["algo"] == 0 && sc["counts"]["manual"] == 0);
  auto r = find_row(sc, "KXHIGHNY-26SEP08-B70", "yes");
  CHECK(r && (*r)["reasons"].dump().find("research") != std::string::npos);
  // A resting entry in a category moved to research or manual is not the algorithm's any more.
  json t = {{"side", "yes"}, {"entry_price", .64}, {"entry_style", "maker"}, {"fair_strategy", "test_model"}};
  CHECK(systematic_entry_cancel_reason(t, m, s, now).find("research") != std::string::npos);
  // The freshness limit comes from the board: a quote 21 s old is stale on a 20 s board even with
  // its flag still false, and fine on a board that does not say (the old 120 s backstop).
  json aged = m;
  aged["quote_ts"] = now - 21;
  json b20 = {{"markets", json::array({aged})}, {"market_data_max_age_seconds", 20}};
  json s2 = algo_settings();
  s2["kelly_fraction"] = 0;
  json sc20 = systematic_screen(b20, s2, now);
  auto a20 = find_row(sc20, "KXHIGHNY-26SEP08-B70", "yes");
  CHECK(a20 && (*a20)["verdict"] == "blocked" && (*a20)["reasons"].dump().find("stale") != std::string::npos);
  json bnone = {{"markets", json::array({aged})}};
  json scnone = systematic_screen(bnone, s2, now);
  auto anone = find_row(scnone, "KXHIGHNY-26SEP08-B70", "yes");
  CHECK(anone && (*anone)["verdict"] == "algo");
  CHECK(systematic_entry_cancel_reason(t, aged, s2, now, 0.30, 20).find("stale") != std::string::npos);
  CHECK(systematic_entry_cancel_reason(t, aged, s2, now, 0.30, 120).empty());
}

// What an exchange refusal means (reject.h). Only a refusal that says the desk is wrong trips the
// kill switch; the market moving never does.
static void test_rejection_classes() {
  const std::string cross = R"({"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})";
  CHECK(classify_rejection(400, cross) == RejectClass::SoftMarketState);
  CHECK(!reject_trips_kill(classify_rejection(400, cross)));
  CHECK(classify_rejection(400, R"({"error":{"code":"market_closed","message":"market is closed"}})") == RejectClass::SoftMarketState);
  CHECK(classify_rejection(400, R"({"error":{"code":"trading_paused","message":"trading is paused"}})") == RejectClass::SoftMarketState);
  CHECK(classify_rejection(400, R"({"error":{"code":"insufficient_balance","message":"insufficient balance"}})") == RejectClass::SoftCollateral);
  CHECK(classify_rejection(429, "slow down") == RejectClass::Retryable);
  CHECK(classify_rejection(503, "") == RejectClass::Retryable);
  CHECK(classify_rejection(401, R"({"error":{"code":"unauthorized"}})") == RejectClass::Auth);
  CHECK(classify_rejection(403, "forbidden") == RejectClass::Auth);
  CHECK(classify_rejection(400, R"({"error":{"code":"invalid_order","message":"rejected by test exchange"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(400, R"({"error":{"code":"invalid_order","message":"count must be positive"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(418, "teapot") == RejectClass::Unknown);
  // Narrow on purpose: these say the desk is wrong, whatever words they share with a soft refusal.
  CHECK(classify_rejection(400, R"({"error":{"code":"invalid_parameter","message":"post_only must be a boolean"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(400, R"({"error":{"code":"self_trade","message":"order would self trade"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(400, R"({"error":{"message":"insufficient permissions for this account"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(400, R"({"error":{"message":"API key is not active"}})") == RejectClass::HardInvalidOrder);
  CHECK(classify_rejection(404, R"({"error":{"code":"not_found","message":"market not found"}})") == RejectClass::Unknown);
  CHECK(is_post_only_cross("book moved: a post-only yes bid at 0.61 would cross the 0.61 offer; not sent"));
  CHECK(!is_post_only_cross("order would cross your own resting order"));
  CHECK(reject_class_from_name("soft_market_state") == RejectClass::SoftMarketState);
  CHECK(reject_class_from_name("nonsense") == RejectClass::Unknown);
  for (RejectClass c : {RejectClass::HardInvalidOrder, RejectClass::Auth, RejectClass::Unknown}) CHECK(reject_trips_kill(c));
  for (RejectClass c : {RejectClass::SoftMarketState, RejectClass::SoftCollateral, RejectClass::Retryable}) CHECK(!reject_trips_kill(c));
}

// A post-only entry that meets a moved book is the market, not a fault: nothing is placed, the
// algorithm keeps running, the kill switch stays off, and the entry is re-priced from a fresh read
// of the book and sent once more. A malformed order still stops everything (the end of
// test_live_entry_exit_and_budget). This is the 2026-09-11 02:32 UTC incident.
static void test_post_only_cross_does_not_halt() {
  std::string pem = "/tmp/desk_sys_cross_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip post-only test: openssl CLI not available\n"); return; }
  MockExchange x;
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.60; x.yes_ask = 0.64; }
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  cfg.risk.max_entry_horizon_hours = 48;
  KalshiClient client(x.base(), signer, "tests");
  Executor ex(cfg, st, client);
  std::atomic<bool> killed{false};
  std::string kill_reason;
  int fresh_reads = 0;
  json fresh_book = {{"yes_bid", 0.58}, {"yes_ask", 0.63}, {"no_bid", 0.37}, {"no_ask", 0.42}, {"yes_bid_size", 20.0},
                     {"yes_ask_size", 30.0}, {"no_bid_size", 30.0}, {"no_ask_size", 20.0}};
  SystematicTrader tr(st, &ex, cfg,
                      SystematicTrader::Hooks{[&] { return killed.load(); },
                                              [&](const std::string& why) { killed = true; kill_reason = why; },
                                              [&] {},
                                              [&](const std::string&) { ++fresh_reads; json b = fresh_book; b["quote_ts"] = now_s(); return b; }});
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.60; m.yes_ask = 0.64; m.tick = 0.01;
  m.expected_expiration_time = iso_of(now + 5 * 3600);
  st.upsert_market(m);
  Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
  st.upsert_series(s);
  ex.reconcile(true);
  // A maker candidate: taker 0.66-0.64-fee fails the floor, maker at 0.61 clears it.
  json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now)})}};
  tr.configure(json{{"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}}}, {"mode", "live"}, {"running", true}});
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.reject_body = R"({"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})";
  }
  x.reject_next = 1;
  tr.step(board, now + 1);
  CHECK(!killed);                                    // the global kill switch is untouched
  json v = tr.view();
  CHECK(v["running"] == true && v["halted"] == false);
  CHECK(fresh_reads == 1);                           // the book was re-read once
  CHECK(x.writes == 2);                              // refused once, re-priced and sent once
  CHECK(x.last_order.value("post_only", false) == true && x.last_order["price"] == "0.5900");   // posted at the new bid + a tick
  bool failed_first = false;
  for (const auto& p : st.proposals("failed", 10))
    if (p.strategy == "systematic" && p.note.find("soft_market_state") != std::string::npos) failed_first = true;
  CHECK(failed_first);                               // the refused one is recorded, with its class
  // ...and on its decision: taken, sent, refused by the exchange; then the re-priced decision,
  // taken and sent. Two decisions, because the fresh book is a new decision.
  {
    std::vector<std::string> seq;
    std::set<std::string> ids;
    for (const auto& r : st.query("SELECT decision_id, event FROM trade_decisions ORDER BY id")) {
      seq.push_back(r.value("event", std::string()));
      ids.insert(r.value("decision_id", std::string()));
    }
    CHECK((seq == std::vector<std::string>{"auto", "submitted", "rejected", "auto", "submitted"}));
    CHECK(ids.size() == 2);
  }
  bool logged_as_error = false;
  for (const auto& e : st.events(50))
    if (e.value("level", "") == "error" && e.value("msg", "").find("post only cross") != std::string::npos) logged_as_error = true;
  CHECK(!logged_as_error);                           // a warning, never an error
  // When the fresh read shows the edge gone, nothing is re-sent, and still nothing stops.
  x.hold(0, 0);
  ex.reconcile(false);
  tr.configure(json{{"release", x.ticker}});
  tr.configure(json{{"settings", {{"cooldown_minutes", 0}}}});
  { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.60; x.yes_ask = 0.64; }
  fresh_book = {{"yes_bid", 0.66}, {"yes_ask", 0.67}, {"no_bid", 0.33}, {"no_ask", 0.34}, {"yes_bid_size", 20.0},
                {"yes_ask_size", 30.0}, {"no_bid_size", 30.0}, {"no_ask_size", 20.0}};
  int writes_before = x.writes;
  x.reject_next = 1;
  json board2 = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08X", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now + 2)})}};
  tr.step(board2, now + 2);
  CHECK(x.writes == writes_before + 1);              // the refused send only
  CHECK(fresh_reads == 2);                           // the book was re-read...
  bool said_weak = false;
  const json vlog = tr.view();
  for (const auto& e : vlog["log"])
    if (e.value("msg", "").find("after a fresh read the row is weak") != std::string::npos) said_weak = true;
  CHECK(said_weak);                                  // ...and the re-screen said why nothing went
  CHECK(!killed && tr.view()["halted"] == false && tr.view()["running"] == true);
  // A breather follows: inside it the same market is not tried again.
  json board3 = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08X", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now + 5)})}};
  tr.step(board3, now + 5);
  CHECK(x.writes == writes_before + 1);
  // The executor's own read of the book catches a crossing post-only price before sending, and
  // re-prices it to one tick under the offer instead of refusing: never above the reviewed price.
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.yes_bid = 0.60; x.yes_ask = 0.61;
  }
  Proposal p;
  p.ticker = x.ticker; p.event_ticker = "KXHIGHNY-26SEP08Y"; p.series_ticker = "KXHIGHNY"; p.strategy = "systematic";
  p.mode = "bot"; p.outcome_side = "yes"; p.exec_style = "maker"; p.status = "pending"; p.price = 0.61; p.count = 1;
  st.insert_proposal(p);
  st.exec("DELETE FROM kv WHERE key='last_write_ts'");
  int w0 = x.writes;
  bool soft = false;
  try { ex.submit(p); } catch (const SoftRejection& e) { soft = e.cls == RejectClass::SoftMarketState; }
  catch (const std::exception&) {}
  CHECK(!soft && x.writes == w0 + 1);                // not refused: sent once...
  CHECK(x.last_order.value("post_only", false) == true && x.last_order["price"] == "0.6000");   // ...one tick under the 0.61 offer
  CHECK(std::fabs(p.price - 0.60) < 1e-9);           // and the proposal carries the price that went
  // No price left under the offer inside the risk range (min_price 0.05): the refusal stands, nothing
  // is sent, and it still reads as a post-only cross, so the algorithm's retry re-reads the book.
  {
    std::lock_guard<std::mutex> lk(x.mu);
    x.markets["KXHIGHNY-26SEP08Z-B40"] = {{"event_ticker", "KXHIGHNY-26SEP08Z"}, {"yes_bid", 0.03}, {"yes_ask", 0.05}};
  }
  Proposal q;
  q.ticker = "KXHIGHNY-26SEP08Z-B40"; q.event_ticker = "KXHIGHNY-26SEP08Z"; q.series_ticker = "KXHIGHNY"; q.strategy = "systematic";
  q.mode = "bot"; q.outcome_side = "yes"; q.exec_style = "maker"; q.status = "pending"; q.price = 0.05; q.count = 1;
  st.insert_proposal(q);
  st.exec("DELETE FROM kv WHERE key='last_write_ts'");
  const int w1 = x.writes;
  std::string floor_why;
  soft = false;
  try { ex.submit(q); } catch (const SoftRejection& e) { soft = e.cls == RejectClass::SoftMarketState; floor_why = e.what(); }
  catch (const std::exception& e) { floor_why = e.what(); }
  CHECK(soft && x.writes == w1);                     // refused before anything was sent
  CHECK(is_post_only_cross(floor_why));
  std::remove(pem.c_str());
}


// The other ways a crossing post-only price can go: the executor sees it first and re-prices it
// under the offer itself, so nothing is refused and nothing is retried; the retry after an exchange
// refusal meets a malformed-order refusal, which still stops everything; the retry is refused softly
// again, and there is no third try.
static void test_post_only_retry_paths() {
  std::string pem = "/tmp/desk_sys_retry_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip retry-path test: openssl CLI not available\n"); return; }
  const std::string cross = R"({"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})";
  const std::string malformed = R"({"error":{"code":"invalid_order","message":"count must be positive"}})";
  auto run = [&](double exchange_ask, std::deque<std::string> refusals, auto&& verify) {
    MockExchange x;
    { std::lock_guard<std::mutex> lk(x.mu); x.yes_bid = 0.57; x.yes_ask = exchange_ask; x.reject_queue = std::move(refusals); }
    Store st(":memory:");
    Config cfg; cfg.systematic_settings = algo_routes();
    cfg.bankroll_dollars = 100;
    cfg.risk.max_entry_horizon_hours = 48;
    KalshiClient client(x.base(), signer, "tests");
    Executor ex(cfg, st, client);
    std::atomic<bool> killed{false};
    int fresh_reads = 0;
    SystematicTrader tr(st, &ex, cfg,
                        SystematicTrader::Hooks{[&] { return killed.load(); }, [&](const std::string&) { killed = true; }, [&] {},
                                                [&](const std::string&) {
                                                  ++fresh_reads;
                                                  return json{{"yes_bid", 0.57}, {"yes_ask", 0.62}, {"no_bid", 0.38}, {"no_ask", 0.43},
                                                              {"yes_bid_size", 20.0}, {"yes_ask_size", 30.0}, {"no_bid_size", 30.0},
                                                              {"no_ask_size", 20.0}, {"quote_ts", now_s()}};
                                                }});
    long long now = now_s();
    Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.60; m.yes_ask = 0.64; m.tick = 0.01;
    m.expected_expiration_time = iso_of(now + 5 * 3600);
    st.upsert_market(m);
    Series s; s.ticker = "KXHIGHNY"; s.category = "Climate and Weather"; s.fee_type = "quadratic"; s.fee_multiplier = 1;
    st.upsert_series(s);
    ex.reconcile(true);
    tr.configure(json{{"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}}}, {"mode", "live"}, {"running", true}});
    // The screen posts at 0.61 (bid 0.60 + a tick under a 0.64 offer).
    json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.66, 0.67, 0.03, 0.60, 0.64, 20, 30, now)})}};
    tr.step(board, now + 1);
    verify(x, tr, killed.load(), fresh_reads);
  };
  // 1. The exchange's offer is already 0.61: the executor re-prices the 0.61 post-only bid to 0.60,
  //    one tick under that offer, and sends it once. Nothing was refused, so the book is not re-read.
  run(0.61, {}, [&](MockExchange& x, SystematicTrader& tr, bool killed, int fresh_reads) {
    CHECK(x.writes == 1 && fresh_reads == 0);
    CHECK(x.last_order["price"] == "0.6000" && x.last_order.value("post_only", false) == true);
    CHECK(!killed && tr.view()["halted"] == false);
  });
  // 2. The retry meets a malformed-order refusal: that is the desk being wrong, and it stops.
  run(0.64, {cross, malformed}, [&](MockExchange& x, SystematicTrader& tr, bool killed, int) {
    CHECK(x.writes == 2);
    json v = tr.view();
    CHECK(killed && v["halted"] == true && v["running"] == false);
    CHECK(v.value("halt_class", std::string()) == "hard_invalid_order");
  });
  // 3. Refused softly twice: one retry, no third write, still running.
  run(0.64, {cross, cross}, [&](MockExchange& x, SystematicTrader& tr, bool killed, int fresh_reads) {
    CHECK(x.writes == 2 && fresh_reads == 1);
    CHECK(!killed && tr.view()["halted"] == false && tr.view()["running"] == true);
  });
  std::remove(pem.c_str());
}

// Entries and management read different ages on purpose. A row 30 s old is not opened on under a
// 20 s entry limit, but a position on it is still managed (a stop must not be skipped because a
// cycle ran slow); only the management flag going stale stops that.
static void test_management_keeps_its_own_age() {
  MockExchange x;
  Store st(":memory:");
  Config cfg; cfg.systematic_settings = algo_routes();
  cfg.bankroll_dollars = 100;
  KalshiClient client(x.base(), nullptr, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  long long now = now_s();
  Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.60; m.yes_ask = 0.62;
  st.upsert_market(m);
  tr.configure(json{{"mode", "paper"}, {"running", true},
                    {"settings", {{"kelly_fraction", 0}, {"per_trade_dollars", 3}, {"take_profit", 0.06}, {"cooldown_minutes", 0},
                                  {"paper_slippage", 0.01}}}});
  auto board = [&](long long t, double bid, double ask, long long quote_age, bool manage_fresh) {
    json r = row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.70, 0.72, 0.05, bid, ask, 20, 30, t);
    r["quote_ts"] = t - quote_age;
    r["quote_stale"] = quote_age > 20;
    r["quote_stale_manage"] = !manage_fresh;
    return json{{"markets", json::array({r})}, {"market_data_max_age_seconds", 20}};
  };
  tr.step(board(now, 0.60, 0.62, 30, true), now);
  CHECK(tr.view()["positions"].empty());             // 30 s old: not opened on
  tr.step(board(now + 1, 0.60, 0.62, 2, true), now + 1);
  CHECK(tr.view()["positions"].size() == 1);         // fresh: opened
  tr.step(board(now + 2, 0.75, 0.77, 30, false), now + 2);
  CHECK(tr.view()["positions"].size() == 1);         // management flag stale: no decision on it
  tr.step(board(now + 3, 0.75, 0.77, 30, true), now + 3);
  json v = tr.view();
  CHECK(v["positions"].empty());                     // 30 s old but manageable: the profit is taken
  CHECK(v["history"].size() == 1 && std::string(v["history"][0]["exit_reason"]).find("take profit") != std::string::npos);
}

// A halt raised by an older build for a refusal that is no longer a stop reason is cleared at
// startup, and the algorithm is left stopped rather than resumed. A halt for a malformed order stands.
static void test_stale_halt_cleared_at_startup() {
  auto seed = [](Store& st, const std::string& reason) {
    json state = {{"schema", 2}, {"running", true}, {"mode", "paper"}, {"settings", systematic_defaults()},
                  {"settings_version", 1}, {"halted", true}, {"halt_reason", reason}, {"day", ""},
                  {"cooldowns", json::object()}, {"log", json::array()}, {"last_step_ts", 0}};
    st.kv_set("systematic:v2", state.dump());
  };
  Config cfg;
  {
    Store st(":memory:");
    seed(st, R"(order rejected by the exchange: {"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})");
    SystematicTrader tr(st, nullptr, cfg);
    json v = tr.view();
    CHECK(v["halted"] == false && v["halt_reason"] == "");
    CHECK(v["running"] == false);                    // cleared, not resumed
    bool said = false;
    for (const auto& e : v["log"]) if (e.value("msg", "").find("halt cleared at startup") != std::string::npos) said = true;
    CHECK(said);
  }
  {
    Store st(":memory:");
    seed(st, R"(order rejected by the exchange: {"error":{"code":"invalid_order","message":"count must be positive"}})");
    SystematicTrader tr(st, nullptr, cfg);
    CHECK(tr.view()["halted"] == true);
  }
  {
    Store st(":memory:");
    seed(st, "daily loss stop: today -6.00 <= -6.00");
    SystematicTrader tr(st, nullptr, cfg);
    CHECK(tr.view()["halted"] == true);              // other halts are not touched
  }
  {
    Store st(":memory:");
    seed(st, R"(order rejected by the exchange: {"error":{"message":"API key is not active"}})");
    SystematicTrader tr(st, nullptr, cfg);
    CHECK(tr.view()["halted"] == true);              // not one of the explicit market-state phrases
  }
  {
    // A halt written by this build carries its class, and the class decides, not the body.
    Store st(":memory:");
    json state = {{"schema", 2}, {"running", false}, {"mode", "paper"}, {"settings", systematic_defaults()},
                  {"settings_version", 1}, {"halted", true}, {"halt_class", "auth"},
                  {"halt_reason", R"(order rejected by the exchange: {"error":{"details":"post only cross"}})"},
                  {"day", ""}, {"cooldowns", json::object()}, {"log", json::array()}, {"last_step_ts", 0}};
    st.kv_set("systematic:v2", state.dump());
    SystematicTrader tr(st, nullptr, cfg);
    CHECK(tr.view()["halted"] == true);
  }
}

// M2 lanes: the live-sizing rule, the stage gate and the cheap-side filter in the router, and the
// trader's stake, hold-to-settlement and per-lane loss stop.
static void test_lanes() {
  // ---- the rule, pure ----
  json s = systematic_defaults();
  std::vector<double> win(30, 0.40), fee(30, 0.02);
  for (int i = 0; i < 30; i += 3) win[i] = -0.30;   // mean ~ +0.17, noisy
  LaneWindow w = evaluate_lane_window(win, fee, 1.2816);
  CHECK(w.n == 30 && w.mean > 0 && w.verdict == "double");
  std::vector<double> flat(30, 0.0);
  flat[0] = 5; flat[1] = -4.9;                       // mean ~0, wide band: hold
  CHECK(evaluate_lane_window(flat, fee, 1.2816).verdict == "hold");
  std::vector<double> lose(30, -0.10);
  CHECK(evaluate_lane_window(lose, fee, 1.2816).verdict == "halve");
  json lane = lane_object(json{{"stage", "live_candidate"}, {"stake", 5.0}}, 5.0);
  std::vector<std::string> notes;
  // 29 settled trades: no complete window, nothing moves.
  std::vector<double> p29(win.begin(), win.begin() + 29), f29(fee.begin(), fee.begin() + 29);
  json l0 = apply_lane_windows(lane, "m", p29, f29, s, &notes);
  CHECK(l0["stake"] == 5.0 && l0["evaluated_trades"] == 0 && notes.empty());
  // One good window: 5 -> 10, and the stage says live.
  json l1 = apply_lane_windows(lane, "m", win, fee, s, &notes);
  CHECK(l1["stake"] == 10.0 && l1["stage"] == "live" && l1["evaluated_trades"] == 30 && notes.size() == 1);
  // Applying the same history again changes nothing: each window is evaluated once.
  CHECK(apply_lane_windows(l1, "m", win, fee, s, nullptr) == l1);
  // Then three losing windows: 10 -> 5 -> 2.5 -> research.
  std::vector<double> hist = win, fh = fee;
  for (int k = 0; k < 3; ++k) { hist.insert(hist.end(), lose.begin(), lose.end()); fh.insert(fh.end(), fee.begin(), fee.end()); }
  notes.clear();
  json l2 = apply_lane_windows(l1, "m", hist, fh, s, &notes);
  CHECK(l2["stage"] == "research" && l2["negative_windows"] == 3 && l2["evaluated_trades"] == 120);
  CHECK(l2["windows"].size() == 4 && l2["windows"].back().value("demoted", false));
  CHECK(notes.back().find("back to research") != std::string::npos);
  // The cap: 40 does not double past lane_max_stake.
  json top = lane_object(json{{"stage", "live"}, {"stake", 40.0}}, 5.0);
  CHECK(apply_lane_windows(top, "m", win, fee, s, nullptr)["stake"] == 40.0);

  // ---- settings ----
  json d = validate_systematic_settings(json::object(), json::object());
  CHECK(d["lanes"].contains("sports_live") && d["lanes"]["sports_live"]["stage"] == "research" &&
        d["lanes"]["sports_live"]["stake"] == 5.0);
  bool threw = false;
  try { validate_systematic_settings(json{{"lanes", {{"x", {{"stage", "moon"}}}}}}, d); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  // A patch to one lane keeps the others.
  json d2 = validate_systematic_settings(json{{"lanes", {{"test_model", {{"stage", "research"}}}}}}, d);
  CHECK(d2["lanes"].contains("sports_live") && d2["lanes"]["test_model"]["stake"] == 5.0);

  // ---- the router ----
  long long now = now_s();
  // A cheap YES: p(win) 0.45 against an ask of 0.35. Under the 0.55 win-probability floor, which
  // for a lane with the asymmetric filter is replaced by the price ceiling.
  json cheap = row("KXNFLGAME-26SEP28A-A", "KXNFLGAME-26SEP28A", "Sports", 0.45, 0.45, 0.04, 0.33, 0.35, 50, 50, now);
  // An expensive YES: ask 0.65, edge about 0.07 after the buffer: over the manual floor, under
  // the 0.10 override.
  json dear = row("KXNFLGAME-26SEP28B-B", "KXNFLGAME-26SEP28B", "Sports", 0.76, 0.76, 0.04, 0.63, 0.65, 50, 50, now);
  json board = {{"markets", json::array({cheap, dear})}};
  auto lane_settings = [&](const std::string& stage) {
    json x = algo_settings();
    x["graduated_models"] = json::array();   // the lane stage is the gate now, not graduation
    x["kelly_fraction"] = 0;
    x["lanes"]["test_model"] = lane_object(json{{"stage", stage}}, 5.0);
    return validate_systematic_settings(json::object(), x);
  };
  json sc = systematic_screen(board, lane_settings("live_candidate"), now);
  auto a = find_row(sc, "KXNFLGAME-26SEP28A-A", "yes");
  CHECK(a && (*a)["verdict"] == "algo");
  auto b = find_row(sc, "KXNFLGAME-26SEP28B-B", "yes");
  CHECK(b && (*b)["verdict"] == "manual" && (*b)["reasons"].dump().find("asymmetric filter") != std::string::npos);
  // A lane at research: priced for you, not sent, whatever graduated_models says.
  json rs = lane_settings("research");
  rs["graduated_models"] = json::array({"test_model_v1"});
  sc = systematic_screen(board, rs, now);
  a = find_row(sc, "KXNFLGAME-26SEP28A-A", "yes");
  CHECK(a && (*a)["verdict"] == "manual" && (*a)["reasons"].dump().find("stage research") != std::string::npos);
  // A model that is not a lane is untouched: graduated, and the win-probability floor still binds.
  json legacy = algo_settings();
  legacy["kelly_fraction"] = 0;
  sc = systematic_screen(board, legacy, now);
  a = find_row(sc, "KXNFLGAME-26SEP28A-A", "yes");
  CHECK(a && (*a)["verdict"] == "weak" && (*a)["reasons"].dump().find("win probability") != std::string::npos);

  // ---- the trader, in paper ----
  MockExchange x;
  Store st(":memory:");
  Config cfg; cfg.bankroll_dollars = 100;
  json seed = algo_settings();
  seed["graduated_models"] = json::array();
  seed["lanes"] = {{"test_model", {{"stage", "live_candidate"}, {"stake", 5.0}}}};
  cfg.systematic_settings = seed;
  KalshiClient client(x.base(), nullptr, "tests");
  Executor ex(cfg, st, client);
  SystematicTrader tr(st, &ex, cfg);
  json v = tr.configure(json{{"mode", "paper"}, {"running", true},
                             {"settings", {{"per_trade_dollars", 2}, {"sports_per_trade_dollars", 2}, {"take_profit", 0.06},
                                           {"stop_loss", 0}, {"cooldown_minutes", 0}, {"paper_slippage", 0.0},
                                           {"daily_loss_stop_enabled", false}, {"lane_daily_loss_stop_frac", 0.02}}}});
  CHECK(v["settings"]["lanes"]["test_model"]["stage"] == "live_candidate");
  Market m; m.ticker = x.ticker; m.event_ticker = "KXNFLGAME-26SEP28A"; m.series_ticker = "KXNFLGAME"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.33; m.yes_ask = 0.35;
  st.upsert_market(m);
  auto board_at = [&](long long t, double p, double bid, double ask) {
    json r = row(x.ticker, "KXNFLGAME-26SEP28A", "Sports", p, p, 0.04, bid, ask, 50, 50, t);
    return json{{"markets", json::array({r})}};
  };
  tr.step(board_at(now, 0.45, 0.33, 0.35), now);
  v = tr.view();
  CHECK(v["positions"].size() == 1);
  // Sized by the lane's $5, not the $2 per-trade setting.
  FeeModel fm{"quadratic", 1};
  const double cost_per = 0.35 + fm.fee(0.35, 1, true);
  CHECK_NEAR(v["positions"][0]["filled"].get<double>(), std::floor(5.0 / cost_per), 1e-9);
  // The bid jumps past the take-profit and past p(win): a lane trade holds to settlement.
  long long t = now + 60;
  tr.step(board_at(t, 0.45, 0.52, 0.54), t);
  v = tr.view();
  CHECK(v["positions"].size() == 1);
  // Attribution names the lane.
  bool seen = false;
  for (const auto& l : v["lanes"]) if (l["model"] == "test_model") seen = l["lane"]["stage"] == "live_candidate";
  CHECK(seen);
  // The mark collapses past the $2 stop (2% of $100). In paper that changes nothing about the lane:
  // its stage is about live trading, and paper losses must not demote it (review 2026-09-23).
  t = now + 120;
  tr.step(board_at(t, 0.45, 0.05, 0.07), t);
  v = tr.view();
  CHECK(v["settings"]["lanes"]["test_model"]["stage"] == "live_candidate");
  // Demoted by hand: nothing new is opened in it.
  tr.configure(json{{"settings", {{"lanes", {{"test_model", {{"stage", "research"}}}}}}}});
  st.exec("DELETE FROM systematic_trades");
  t = now + 180;
  tr.step(board_at(t, 0.45, 0.33, 0.35), t);
  CHECK(tr.view()["positions"].empty());
  // Promoting it by hand starts its evidence afresh, at the base stake.
  tr.configure(json{{"settings", {{"lanes", {{"test_model", {{"stake", 20.0}}}}}}}});
  v = tr.configure(json{{"settings", {{"lanes", {{"test_model", {{"stage", "live_candidate"}}}}}}}});
  CHECK(v["settings"]["lanes"]["test_model"]["negative_windows"] == 0 &&
        v["settings"]["lanes"]["test_model"]["since_ts"].get<long long>() >= now);
  CHECK(v["settings"]["lanes"]["test_model"]["stake"] == 5.0);
}

// M2 lanes, live: the order is sized by the lane's stake, capped by the lane's daily volume.
static void test_lane_live_sizing() {
  std::string pem = "/tmp/desk_sys_lane_key.pem";
  auto signer = test_signer(pem);
  if (!signer) { std::fprintf(stderr, "skip live lane test: openssl CLI not available\n"); return; }
  auto first_order_count = [&](double volume_cap, double stake = 5.0) {
    MockExchange x;
    Store st(":memory:");
    Config cfg;
    json seed = algo_routes();
    seed["graduated_models"] = json::array();
    seed["lanes"] = {{"test_model", {{"stage", "live_candidate"}, {"stake", stake}}}};
    cfg.systematic_settings = seed;
    cfg.bankroll_dollars = 100;
    cfg.risk.max_entry_horizon_hours = 48;
    cfg.risk.max_order_dollars = 10;
    KalshiClient client(x.base(), signer, "tests");
    Executor ex(cfg, st, client);
    SystematicTrader tr(st, &ex, cfg);
    const long long now = now_s();
    Market m; m.ticker = x.ticker; m.event_ticker = "KXHIGHNY-26SEP08"; m.series_ticker = "KXHIGHNY"; m.status = "active";
    m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.33; m.yes_ask = 0.35; m.tick = 0.01;
    m.expected_expiration_time = iso_of(now + 5 * 3600);
    st.upsert_market(m);
    Series se; se.ticker = "KXHIGHNY"; se.category = "Climate and Weather"; se.fee_type = "quadratic"; se.fee_multiplier = 1;
    st.upsert_series(se);
    ex.reconcile(true);
    tr.configure(json{{"settings", {{"per_trade_dollars", 3}, {"lane_daily_volume_dollars", volume_cap}, {"entry_style", "taker"}}},
                      {"mode", "live"}, {"running", true}});
    json board = {{"markets", json::array({row(x.ticker, "KXHIGHNY-26SEP08", "Climate and Weather", 0.45, 0.45, 0.04, 0.33, 0.35, 50, 50, now)})}};
    tr.step(board, now + 1);
    return x.writes == 1 ? x.last_order.value("count", std::string()) : std::string("no order");
  };
  // $5 at 0.35 + 0.0160 fee a contract: 13 contracts (the $3 per-trade setting does not apply).
  CHECK(first_order_count(50) == "13.00");
  // $3 of lane volume left today: 8.
  CHECK(first_order_count(3) == "8.00");
  // A stake doubled past the desk's $10 per-order cap is sent at the cap, not refused: 27.
  CHECK(first_order_count(50, 40.0) == "27.00");
  // The lane's daily loss stop, live: a closed live loss today past 8% of $100 puts it back to research.
  {
    MockExchange x;
    Store st(":memory:");
    Config cfg;
    json seed = algo_routes();
    seed["graduated_models"] = json::array();
    seed["lanes"] = {{"test_model", {{"stage", "live_candidate"}, {"stake", 5.0}}}};
    cfg.systematic_settings = seed;
    cfg.bankroll_dollars = 100;
    KalshiClient client(x.base(), signer, "tests");
    Executor ex(cfg, st, client);
    SystematicTrader tr(st, &ex, cfg);
    tr.configure(json{{"mode", "live"}, {"running", true}});
    const long long now = now_s();
    tr.step(json{{"markets", json::array()}}, now);   // rolls the day in
    st.exec("INSERT INTO systematic_trades(ticker,event_ticker,side,state,mode,opened_ts,closed_ts,realized_pnl,fair_strategy) "
            "VALUES('T','E','yes','closed','live',?,?,-9.0,'test_model')", {now, now});
    tr.step(json{{"markets", json::array()}}, now + 1);
    CHECK(tr.view()["settings"]["lanes"]["test_model"]["stage"] == "research");
  }
  std::remove(pem.c_str());
}

// persist() writes the state only when its dump differs from the last one it wrote (2026-10-01):
// step() persists every cycle, and an unchanged blob was rewritten every time.
static void test_persist_skips_unchanged_state() {
  Store st(":memory:");
  Config cfg;
  SystematicTrader tr(st, nullptr, cfg);
  CHECK(json::parse(*st.kv_get("systematic:v2")) == tr.state());
  // A stand-in written behind the trader's back shows whether the next persist wrote at all.
  st.kv_set("systematic:v2", "sentinel");
  tr.configure(json::object());                     // persists, and nothing in the state changed
  CHECK(*st.kv_get("systematic:v2") == "sentinel");
  tr.configure(json{{"settings", {{"cooldown_minutes", 7}}}});   // a change is written
  json back = json::parse(*st.kv_get("systematic:v2"));
  CHECK(back["settings"]["cooldown_minutes"] == 7 && back == tr.state());
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--ownership") {
    test_manual_ownership_isolation(); test_manual_position_blocks_new_entry(); test_partial_entry_cancel_race();
    std::printf("%d checks, %d failures\n", checks, fails); return fails == 0 ? 0 : 1;
  }
  test_settings();
  test_persist_skips_unchanged_state();
  test_lanes();
  test_lane_live_sizing();
  test_routes();
  test_screen_routes();
  test_rejection_classes();
  test_stale_halt_cleared_at_startup();
  test_post_only_cross_does_not_halt();
  test_post_only_retry_paths();
  test_management_keeps_its_own_age();
  test_config_seeds_settings();
  test_resting_entry_revalidation();
  test_screen();
  test_live_entry_exit_and_budget();
  test_paper_rules_and_loss_stop();
  test_patient_stop_policy();
  test_maker_entry_lifecycle();
  test_mode_cap_pulls_resting_entry();
  test_ladder_and_exit_rules();
  test_partial_entry_cancel_race();
  test_manual_ownership_isolation();
  test_manual_position_blocks_new_entry();
  test_basket_send();
  std::printf("%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
