#include "desk/decision_router.h"
#include "desk/lanes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "desk/fixed.h"
#include "desk/systematic.h"
#include "desk/timeutil.h"

namespace desk {
namespace {

using nlohmann::json;

bool finite_number(const json& j, const std::string& key) {
  return j.is_object() && j.contains(key) && j[key].is_number() && std::isfinite(j[key].get<double>());
}
double number(const json& j, const std::string& key, double fallback = 0) {
  return finite_number(j, key) ? j[key].get<double>() : fallback;
}
long long integer(const json& j, const std::string& key, long long fallback = 0) {
  return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<long long>() : fallback;
}
std::string str(const json& j, const std::string& key, const std::string& fallback = "") {
  return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
bool yes(const json& j, const std::string& key) {
  return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
double round4(double x) { return std::round(x * 10000.0) / 10000.0; }
// Fresh enough to OPEN risk on: the publisher's stale flag says so AND the timestamp is inside
// `max_age` (the desk's one entry limit, Config::data_max_age, which the board states).
bool fresh(const json& j, const std::string& ts_key, const std::string& stale_key, long long now, long long max_age) {
  double ts = number(j, ts_key);
  return j.contains(stale_key) && j[stale_key].is_boolean() && !yes(j, stale_key) && ts > 0 && ts <= now &&
         now - ts <= max_age;
}
int route_rank(RouteMode r) {
  switch (r) {
    case RouteMode::Off: return 0;
    case RouteMode::Research: return 1;
    case RouteMode::Manual: return 2;
    case RouteMode::Auto: return 3;
  }
  return 0;
}
bool contains_string(const json& arr, const std::string& v) {
  if (!arr.is_array()) return false;
  for (const auto& x : arr)
    if (x.is_string() && x.get<std::string>() == v) return true;
  return false;
}

}  // namespace

const char* decision_route_name(DecisionRoute r) {
  switch (r) {
    case DecisionRoute::Blocked: return "blocked";
    case DecisionRoute::Research: return "research";
    case DecisionRoute::Manual: return "manual";
    case DecisionRoute::Auto: return "auto";
  }
  return "blocked";
}

RouteMode model_route(const json& settings, const std::string& model_name, const std::string& model_version,
                      bool* known) {
  if (known) *known = false;
  if (model_name.empty() || !settings.contains("model_routes") || !settings["model_routes"].is_object())
    return RouteMode::Research;
  const json& routes = settings["model_routes"];
  auto read = [&](const std::string& key, RouteMode* out) {
    if (!routes.contains(key) || !routes[key].is_string()) return false;
    try {
      *out = parse_route(routes[key].get<std::string>());
    } catch (const std::invalid_argument&) {
      *out = RouteMode::Off;   // an unreadable route never sends
    }
    return true;
  };
  // Something that is not a versioned model (a locked basket) is routed by its name.
  if (model_version.empty()) {
    RouteMode r = RouteMode::Research;
    if (!read(model_name, &r)) return RouteMode::Research;
    if (known) *known = true;
    return r;
  }
  // A model's route is granted per name AND version, so nothing granted to one version's numbers
  // carries over to the next. A route under the bare name grants nothing: it caps every version
  // ("crypto_lognormal": "off" switches all of them off, whatever their own routes say).
  RouteMode versioned = RouteMode::Research, cap = RouteMode::Auto;
  const bool has_versioned = read(model_name + "_" + model_version, &versioned);
  const bool has_cap = read(model_name, &cap);
  if (known) *known = has_versioned || has_cap;
  return effective_route(versioned, cap);
}

bool model_graduated(const json& settings, const std::string& model_name, const std::string& model_version) {
  if (model_name.empty() || !settings.contains("graduated_models")) return false;
  const json& g = settings["graduated_models"];
  // Per version, like the route: graduating a bare name would graduate numbers nobody has tested
  // yet, the day the model changes.
  return contains_string(g, model_version.empty() ? model_name : model_name + "_" + model_version);
}

RouteMode effective_route(RouteMode category, RouteMode model) {
  return route_rank(category) <= route_rank(model) ? category : model;
}

double min_minutes_to_close_for(const json& m, const json& s) {
  const double flat = number(s, "min_minutes_to_close");
  const double open = number(m, "open_ts"), close = number(m, "close_ts");
  if (open <= 0 || close <= open) return flat;              // no life to measure: the flat floor
  const double life_minutes = (close - open) / 60.0;
  const double share = number(s, "min_life_fraction_to_close", 0.2) * life_minutes;
  // Never above the flat floor - that is the whole point of the rule - and never below a minute,
  // whatever fraction and whatever contract, because an order placed in the last seconds of a
  // market cannot be reconsidered, repriced or cancelled. A very short listed life must not be
  // able to talk the desk down to no floor at all.
  return std::min(flat, std::max(1.0, share));
}

std::string decision_id_for(const std::string& ticker, Side side, const std::string& model_key, long long model_ts,
                            long long book_ts) {
  // FNV-1a, 64 bits: stable across processes and builds, which std::hash is not.
  const std::string key = ticker + "|" + side_name(side) + "|" + model_key + "|" + std::to_string(model_ts) + "|" +
                          std::to_string(book_ts);
  unsigned long long h = 1469598103934665603ULL;
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
  char buf[20];
  std::snprintf(buf, sizeof buf, "d%016llx", h);
  return buf;
}

TradeDecision decide(const json& m, Side side, const json& s, const RouterContext& ctx) {
  TradeDecision d;
  d.generated_ts = ctx.now;
  d.market = m;
  const long long now = ctx.now;
  std::vector<std::string> blocked, research, weak, manual, warnings;
  const std::string cat = systematic_category(m);
  const std::string ticker = str(m, "ticker"), event = str(m, "event_ticker");
  if (ticker.empty() || event.empty()) blocked.push_back("missing market/event identity");
  std::string status = str(m, "market_status", str(m, "status"));
  if (status != "active" && status != "open") blocked.push_back("market is not open");
  if (!fresh(m, "quote_ts", "quote_stale", now, ctx.max_age)) blocked.push_back("quote missing or stale");
  if (!fresh(m, "fair_ts", "fair_stale", now, ctx.max_age)) blocked.push_back("model estimate missing or stale");
  if (ctx.killed) blocked.push_back("kill switch is on: nothing new is sent until it is cleared");

  // The model's output. A row published by the engine carries it; a hand-built one is rebuilt.
  const ProbabilityQuote q = m.contains("quote") && m["quote"].is_object() ? quote_from_json(m["quote"])
                                                                            : quote_from_board_row(m);
  const std::string model_key = q.model_key();

  // Lanes (M2): a model listed in settings.lanes is gated by its lane stage and, with the
  // asymmetric filter on, buys the cheap side only.
  const bool in_lane = lane_managed(s, q.model_name);
  const bool asymmetric = in_lane && lane_asymmetric(s, q.model_name);
  // Routes: the category is the outer permission, the model's own route decides inside it.
  bool model_known = false;
  const RouteMode cat_route = route_setting(s, cat);
  const RouteMode mod_route = model_route(s, q.model_name, q.model_version, &model_known);
  // The strategy's mode in config/desk.json caps it too (off < research < manual; auto and bot do
  // not cap): a model you parked in research there stays there.
  RouteMode mode_cap = RouteMode::Auto;
  std::string mode_name;
  if (auto lm = ctx.strategy_modes.find(q.model_name); lm != ctx.strategy_modes.end()) {
    mode_name = lm->second;
    if (mode_name == "off") mode_cap = RouteMode::Off;
    else if (mode_name == "research") mode_cap = RouteMode::Research;
    else if (mode_name == "manual") mode_cap = RouteMode::Manual;
  }
  const RouteMode route = effective_route(effective_route(cat_route, mode_cap), mod_route);
  // Name whichever permission is the binding one: the category, then the strategy's mode, then the
  // model's own route.
  auto binding = [&](RouteMode r) -> std::string {
    if (cat_route == r) return cat + " is set to " + route_name(r);
    if (mode_cap == r) return "strategy " + q.model_name + " is in " + mode_name + " mode (config/desk.json)";
    if (!model_known) return "model " + (model_key.empty() ? std::string("(none)") : model_key) + " has no route yet";
    return "model " + model_key + " is routed to " + route_name(r);
  };
  if (route == RouteMode::Off) blocked.push_back(binding(RouteMode::Off) + ": switched off");
  else if (route == RouteMode::Research) research.push_back(binding(RouteMode::Research) + ": priced and recorded, offered to nobody");
  else if (route == RouteMode::Manual) manual.push_back(binding(RouteMode::Manual) + ": priced for you, never sent");
  else if (in_lane && !lane_trades_live(s, q.model_name))
    // A lane's stage is the gate, not graduated_models: a lane the sizing rule demoted stops here
    // even if the model was once graduated by hand.
    manual.push_back("lane " + q.model_name + " is at stage " + lane_stage(s, q.model_name) +
                     ": the algorithm does not send it; priced for you");
  else if (!in_lane && !model_graduated(s, q.model_name, q.model_version))
    manual.push_back("model " + model_key + " has not graduated to unattended execution: priced for you, never sent");

  double close = number(m, "close_ts"), settle = number(m, "settlement_ts");
  double mins = number(m, "minutes_to_close", -1e6);
  const double close_floor = min_minutes_to_close_for(m, s);
  if (close > 0 && close <= now) blocked.push_back("market close time has passed");
  if (mins > -1e5 && mins < close_floor)
    blocked.push_back("closes in " + std::to_string(static_cast<long long>(mins)) + " min, under the " +
                      fmt2(close_floor) + " min minimum");
  if (settle <= 0) blocked.push_back("expected settlement time unknown");
  else if (settle <= now) blocked.push_back("expected settlement has passed");
  else if (settle > now + ctx.desk_horizon_hours * 3600)
    blocked.push_back("settles in " + fmt2((settle - now) / 3600.0) + "h, beyond the desk's " +
                      fmt2(ctx.desk_horizon_hours) + "h entry horizon");
  else if (settle > now + number(s, "max_horizon_hours") * 3600)
    manual.push_back("settles in " + fmt2((settle - now) / 3600.0) + "h, beyond the algorithm's " +
                     fmt2(number(s, "max_horizon_hours")) + "h horizon");
  if (yes(s, "near_day_categories_only") && (cat == "economics" || cat == "finance" || cat == "tech_science") &&
      settle > 0) {
    const std::string zone = str(s, "settlement_day_tz");
    const std::string today = trading_day(zone, now);
    // Derive tomorrow from the calendar date (not now+24h, which fails near DST midnight).
    const std::string tomorrow = iso_of(parse_iso(today + "T12:00:00Z") + 86400).substr(0, 10);
    const std::string day = trading_day(zone, static_cast<long long>(settle));
    if (day != today && day != tomorrow) blocked.push_back("only today or tomorrow settlement in " + zone + "; expected " + day);
  }
  // Input health is the model's own verdict on its data: absent means nothing was checked,
  // present-and-false means do not trade on it.
  const json inputs = q.diagnostics.contains("inputs") && q.diagnostics["inputs"].is_object() ? q.diagnostics["inputs"]
                                                                                                : json::object();
  if (!q.auto_eligible && q.inputs_valid) manual.push_back("model source is approved for manual research only");
  if (!q.inputs_valid) {
    blocked.push_back("model inputs failed their own checks");
    if (inputs.contains("reasons") && inputs["reasons"].is_array())
      for (const auto& r : inputs["reasons"]) if (r.is_string()) blocked.push_back(r.get<std::string>());
  }
  if (inputs.contains("warnings") && inputs["warnings"].is_array())
    for (const auto& w : inputs["warnings"]) if (w.is_string()) warnings.push_back(w.get<std::string>());
  // The entry policy for this side: the board computes it for the side a taker would buy, and for
  // the side of a position already held on this ticker (where a same-side addition can qualify).
  const std::string side_key = std::string("entry_context_") + side_name(side);
  const json side_entry = m.contains(side_key) && m[side_key].is_object() ? m[side_key] : json();
  const json entry = side_entry.is_object() ? side_entry
                     : m.contains("entry_context") && m["entry_context"].is_object() ? m["entry_context"] : json();
  // Only a context computed for THIS side can make it an addition: the row's generic one was
  // priced for whichever side a taker would buy.
  const bool addition = side_entry.is_object() && yes(side_entry, "allowed") && str(side_entry, "kind") == "addition";
  if (!entry.is_object() || !yes(entry, "allowed"))
    blocked.push_back("entry policy: " + (entry.is_object() ? str(entry, "reason", "not allowed") : std::string("no entry context")));
  if (ctx.held_events.count(event)) {
    // One event, one entry decision while it has exposure or working orders. The one exception is
    // a qualified same-side addition to a position you hold yourself: yours to review, never sent
    // by the algorithm, and never on top of the algorithm's own trade or a working order.
    const bool offer_additions = str(s, "manual_additions", "offer") != "hide";
    if (addition && !ctx.algo_events.count(event) && !ctx.working_events.count(event) && offer_additions)
      manual.push_back("an addition to the position you hold here (" + str(entry, "reason", "qualified") +
                       "): it adds concentration, so it is yours to review");
    else if (addition && !ctx.algo_events.count(event) && !ctx.working_events.count(event))
      // manual_additions=hide. Still a legitimate addition, and the algorithm may still take it;
      // it is simply not put in front of you again. An event you have already traded coming back
      // as a manual row every cycle crowds out the events you have not.
      blocked.push_back("already traded this event: additions are hidden from the manual board "
                        "(settings.manual_additions), though the algorithm may still add");
    else
      blocked.push_back("this event already has a position or working trade; it belongs to whoever opened it, and "
                        "nothing new is opened in it or on its other side");
  } else if (entry.is_object() && yes(entry, "approval_required")) {
    manual.push_back("entry policy asks for your review: " + str(entry, "reason", "addition"));
  }
  auto cd = ctx.cooldowns.find(event);
  if (cd != ctx.cooldowns.end() && cd->second > now)
    blocked.push_back("event cooling off for " + std::to_string((cd->second - now) / 60 + 1) + " min after the last exit");
  // The model's own research flag, as it gave it. The engine's disagreement flag (a legacy row
  // field) stays a gate for everybody.
  if (q.research_only && lower(q.research_reason).find("disagreement") != std::string::npos)
    blocked.push_back("model/market disagreement gate: " + q.research_reason);
  else if (q.research_only)
    research.push_back("research estimate, not a recommendation: " +
                       (q.research_reason.empty() ? std::string("the model flagged it") : q.research_reason));
  if (!q.settlement_verified)
    research.push_back("settlement not verified: what this contract settles on has not been checked against " +
                       (model_key.empty() ? std::string("the model") : model_key));

  const bool valid_model = std::isfinite(q.p_yes) && q.p_yes > 0 && q.p_yes < 1 && std::isfinite(q.sigma) &&
                           q.sigma >= 0 && q.sigma <= .5;
  if (!valid_model) blocked.push_back("probability or uncertainty is missing/invalid");
  // Priced for display even when invalid, as the screen always was: p 0.5, sigma 0.5.
  ProbabilityQuote eq = q;
  if (!valid_model) {
    eq.p_yes = .5;
    eq.sigma = .5;
    if (!finite_number(eq.diagnostics, "p_raw")) eq.diagnostics["p_raw"] = .5;
  }
  const MarketSidePrices px = normalize_prices_from_row(m);
  const bool two_sided = px.two_sided(side);
  if (!two_sided) blocked.push_back("no executable two-sided quote");
  const double spread = two_sided ? px.ask(side) - px.bid(side) : 0;
  if (two_sided && spread > number(s, "max_spread") + 1e-9)
    manual.push_back("spread " + fmt2(spread) + " is wider than the algorithm's " + fmt2(number(s, "max_spread")));
  const std::string fee_type = str(m, "fee_type");
  if (!finite_number(m, "fee_multiplier") || number(m, "fee_multiplier") < 0 ||
      (fee_type != "quadratic" && fee_type != "quadratic_with_maker_fees" && fee_type != "quadratic_with_combo_maker_fees"))
    blocked.push_back("fee schedule missing or unsupported");
  const FeeModel fm{str(m, "fee_type", "quadratic"), number(m, "fee_multiplier", 1)};
  double tick = number(m, "tick", .01);
  if (tick <= 0) tick = .01;
  EdgeSettings es;
  es.uncertainty_multiplier = number(s, "uncertainty_buffer");
  es.taker_slippage = number(s, "slippage_estimate");
  const EdgeCandidate taker = taker_candidate(eq, px, side, fm, es);
  const EdgeCandidate maker = maker_candidate(eq, px, side, fm, tick, es);
  // The style the algorithm would use. "auto": take when lifting the offer still clears the bar
  // after the buffer (a certain fill on an edge that survives the taker fee), otherwise post.
  const double min_edge = number(s, "min_edge");
  const std::string want = str(s, "entry_style", "auto");
  bool use_taker = want == "taker" || (want != "maker" && taker.executable_edge >= min_edge - 1e-9);
  d.candidate = use_taker ? taker : maker;
  d.alternate = use_taker ? maker : taker;
  const EdgeCandidate& c = d.candidate;
  const double sigma = eq.sigma;
  const double buffered = two_sided ? c.executable_edge : -1 - c.uncertainty_buffer;
  const double p_win = c.fair_probability;
  const double mid = px.yes_mid();
  const double raw_yes = eq.p_raw();
  const double gap = two_sided ? std::fabs(raw_yes - mid) : 0;
  // The desk's disagreement rule on the raw number: a row 0.3 from a liquid market must not be
  // labelled as anything a person should act on.
  if (two_sided && valid_model && gap > ctx.hard_gap + 1e-9)
    blocked.push_back("model/market disagreement " + fmt2(gap) + " > " + fmt2(ctx.hard_gap) +
                      ": the market is the better estimate here; the model needs looking at, not trading");
  else if (two_sided && valid_model && gap > number(s, "max_model_gap") + 1e-9)
    manual.push_back("raw model is " + fmt2(gap) + " from the market mid, over the " + fmt2(number(s, "max_model_gap")) +
                     " gap limit: check the model before trading this");
  if (two_sided && valid_model && c.net_edge > number(s, "max_edge") + 1e-9)
    manual.push_back("edge " + fmt2(c.net_edge) + " is above the algorithm's cap of " + fmt2(number(s, "max_edge")) +
                     ": an estimate this far from a live market is usually the estimate that is wrong; review by hand");
  if (valid_model && sigma > number(s, "auto_max_sigma", .10) + 1e-9)
    manual.push_back("uncertainty " + fmt2(sigma) + " is above the automatic cap of " + fmt2(number(s, "auto_max_sigma", .10)));
  // A same-side addition has its own, higher bar (risk.add_min_edge and the improvement rule).
  if (addition && two_sided && c.net_edge + 1e-9 < number(entry, "required_edge"))
    weak.push_back("an addition needs edge " + fmt2(number(entry, "required_edge")) + " at this price; it has " + fmt2(c.net_edge));
  // The win-probability floor refuses exactly the trades the asymmetric profile is built on (the
  // side bought at 20-45c wins less than half the time by construction), so for a lane with the
  // filter on the price ceiling below replaces it.
  if (!asymmetric && two_sided && valid_model && p_win + 1e-9 < number(s, "min_win_probability"))
    weak.push_back("win probability " + fmt2(p_win) + " is under the " + fmt2(number(s, "min_win_probability")) + " floor");
  if (asymmetric && two_sided && valid_model && c.entry_price > number(s, "asymmetric_max_price", .45) + 1e-9 &&
      buffered <= number(s, "asymmetric_override_edge", .10) + 1e-9)
    manual.push_back("asymmetric filter: the algorithm buys a side priced at or under " +
                     fmt2(number(s, "asymmetric_max_price", .45)) + " unless the edge after the buffer exceeds " +
                     fmt2(number(s, "asymmetric_override_edge", .10)) + "; this side costs " + fmt2(c.entry_price) +
                     " with edge " + fmt2(buffered));
  if (two_sided && valid_model && buffered + 1e-9 < min_edge)
    weak.push_back("edge " + fmt2(c.net_edge) + " less " + fmt2(es.uncertainty_multiplier) + "x sigma " + fmt2(sigma) +
                   (c.estimated_slippage > 0 ? " and slippage " + fmt2(c.estimated_slippage) : std::string()) + " = " +
                   fmt2(buffered) + " is under the " + fmt2(min_edge) + " minimum" +
                   (c.style == "maker" ? " (even as a maker)" : ""));
  // The master switch: an AUTO decision is one that will be sent. With the algorithm stopped the
  // same row is yours, and must clear your floor like any other.
  if (!ctx.auto_armed) manual.push_back(ctx.auto_off_reason);
  // The manual lane is a lane, not an overflow bin. A row a policy handed to you still has to
  // clear its own floor; under it nobody trades it and it is not worth your attention.
  if (two_sided && valid_model && !manual.empty() && buffered + 1e-9 < number(s, "manual_min_edge"))
    weak.push_back("edge " + fmt2(buffered) + " after the buffer is under the " + fmt2(number(s, "manual_min_edge")) +
                   " manual floor, and this row is not one the algorithm will send");
  if (c.style == "taker" && two_sided && c.ask_size < 1) blocked.push_back("no displayed size to lift");

  // Exactly one route. A data or risk gate first; then research (nobody is offered it, whatever it
  // scores); then the thresholds; then the policies that hand a tradable row to you; then AUTO.
  if (!blocked.empty()) { d.route = DecisionRoute::Blocked; d.verdict = "blocked"; }
  else if (!research.empty()) { d.route = DecisionRoute::Research; d.verdict = "research"; }
  else if (!weak.empty()) { d.route = DecisionRoute::Blocked; d.verdict = "weak"; }
  else if (!manual.empty()) { d.route = DecisionRoute::Manual; d.verdict = "manual"; }
  else { d.route = DecisionRoute::Auto; d.verdict = "algo"; }
  for (auto* v : {&blocked, &research, &weak, &manual})
    for (auto& r : *v) d.reasons.push_back(r);
  d.warnings = warnings;
  d.book_ts = integer(m, "quote_ts");
  d.decision_id = decision_id_for(ticker, side, model_key, q.model_ts, d.book_ts);
  d.candidate.diagnostics["model_gap"] = round4(gap);
  d.candidate.diagnostics["valid_model"] = valid_model;
  return d;
}

std::vector<TradeDecision> decide_board(const json& board, const json& settings, const RouterContext& ctx_in) {
  std::vector<TradeDecision> out;
  if (!board.contains("markets") || !board["markets"].is_array()) return out;
  RouterContext ctx = ctx_in;
  // The board says which freshness limit it applied; a hand-built board without one keeps the old cap.
  if (board.contains("market_data_max_age_seconds") && board["market_data_max_age_seconds"].is_number())
    ctx.max_age = board["market_data_max_age_seconds"].get<long long>();
  for (const auto& m : board["markets"]) {
    if (!m.is_object()) continue;
    for (Side side : {Side::Yes, Side::No}) out.push_back(decide(m, side, settings, ctx));
  }
  return out;
}

json decision_to_row(const TradeDecision& d) {
  const json& m = d.market;
  const EdgeCandidate& c = d.candidate;
  const ProbabilityQuote& q = c.fair;
  const bool valid_model = c.diagnostics.value("valid_model", false);
  const bool two_sided = c.bid > 0 && c.ask > 0 && c.bid < c.ask + 1e-9;
  const long long settle = integer(m, "settlement_ts");
  const long long now = d.generated_ts;
  // The taker and maker numbers, whichever style was chosen.
  const EdgeCandidate& tk = c.style == "taker" ? c : d.alternate;
  const EdgeCandidate& mk = c.style == "maker" ? c : d.alternate;
  json reasons = json::array(), warnings = json::array();
  for (const auto& r : d.reasons) reasons.push_back(r);
  for (const auto& w : d.warnings) warnings.push_back(w);
  const double buffered = two_sided ? c.executable_edge : -1 - c.uncertainty_buffer;
  return json{{"ticker", str(m, "ticker")}, {"event_ticker", str(m, "event_ticker")},
              {"series_ticker", str(m, "series_ticker")}, {"title", str(m, "title")},
              {"yes_sub_title", str(m, "yes_sub_title")}, {"event_date", str(m, "event_date")},
              {"category", systematic_category(m)}, {"side", side_name(c.side)},
              {"p_win", valid_model ? json(round4(c.fair_probability)) : json(nullptr)},
              {"p_raw_win", valid_model ? json(round4(c.raw_fair_probability)) : json(nullptr)},
              {"p_yes", std::isfinite(q.p_yes) ? q.p_yes : number(m, "p_model")},
              {"sigma", valid_model ? q.sigma : .5}, {"bid", c.bid}, {"ask", c.ask},
              {"bid_size", c.bid_size}, {"ask_size", c.ask_size}, {"spread", round4(two_sided ? c.ask - c.bid : 0)},
              {"fee_type", str(m, "fee_type")}, {"fee_multiplier", number(m, "fee_multiplier", 1)},
              {"tick", number(m, "tick", .01) > 0 ? number(m, "tick", .01) : .01},
              {"taker_edge", round4(two_sided ? tk.net_edge : -1)},
              {"maker_price", round4(two_sided ? mk.entry_price : 0)},
              {"maker_edge", round4(two_sided ? mk.net_edge : -1)},
              {"style", two_sided ? c.style : ""}, {"price", round4(two_sided ? c.entry_price : 0)},
              {"fee", round4(two_sided ? c.entry_fee : 0)}, {"edge", round4(two_sided ? c.net_edge : -1)},
              {"buffered_edge", round4(buffered)}, {"kelly", round4(two_sided ? c.kelly : 0)},
              {"model_gap", c.diagnostics.value("model_gap", 0.0)},
              {"minutes_to_close", number(m, "minutes_to_close", -1e6)}, {"settlement_ts", settle},
              {"hours_to_settlement", settle > 0 ? round4((settle - now) / 3600.0) : -1.0},
              {"research", q.research_only}, {"research_reason", q.research_reason},
              {"fair_strategy", str(m, "fair_strategy", q.model_name)}, {"fair_note", str(m, "fair_note")},
              {"quote_ts", integer(m, "quote_ts")}, {"fair_ts", integer(m, "fair_ts")},
              {"inputs_valid", q.inputs_valid}, {"verdict", d.verdict}, {"reasons", reasons}, {"warnings", warnings},
              // The decision itself: what both lanes act on.
              {"route", decision_route_name(d.route)}, {"decision_id", d.decision_id},
              {"model", q.model_name}, {"model_version", q.model_version}, {"model_key", q.model_key()},
              {"model_ts", q.model_ts}, {"book_ts", d.book_ts}, {"raw_edge", round4(two_sided ? c.raw_edge : -1)},
              {"executable_edge", round4(buffered)}, {"slippage", round4(c.estimated_slippage)},
              {"uncertainty_buffer", round4(c.uncertainty_buffer)}, {"ev_per_contract", round4(two_sided ? c.ev_per_contract : -1)},
              {"displayed_size", c.displayed_size}, {"settlement_verified", q.settlement_verified},
              {"auto_eligible", q.auto_eligible}, {"quote", quote_to_json(q)},
              {"alternate", {{"style", d.alternate.style}, {"price", round4(d.alternate.entry_price)},
                             {"fee", round4(d.alternate.entry_fee)}, {"raw_edge", round4(d.alternate.raw_edge)},
                             {"executable_edge", round4(d.alternate.executable_edge)}}}};
}

MarketSidePrices decision_prices(const json& row) {
  const double bid = number(row, "bid"), ask = number(row, "ask");
  const double bid_size = number(row, "bid_size"), ask_size = number(row, "ask_size");
  // The row carries its own side's touch; the other side is the same book seen from across.
  return side_from(str(row, "side", "yes")) == Side::Yes
             ? normalize_prices(bid, ask, 0, 0, bid_size, ask_size, 0, 0)
             : normalize_prices(0, 0, bid, ask, 0, 0, bid_size, ask_size);
}

EdgeCandidate decision_edge_at(const json& row, const std::string& style, double price, const json& s) {
  const ProbabilityQuote q = row.contains("quote") && row["quote"].is_object() ? quote_from_json(row["quote"])
                                                                               : quote_from_board_row(row);
  EdgeSettings es;
  es.uncertainty_multiplier = number(s, "uncertainty_buffer");
  es.taker_slippage = number(s, "slippage_estimate");
  const FeeModel fm{str(row, "fee_type", "quadratic"), number(row, "fee_multiplier", 1)};
  return candidate_at(q, decision_prices(row), side_from(str(row, "side", "yes")), style, price, fm, es);
}

json decisions_to_screen(const std::vector<TradeDecision>& decisions, const json& s, long long now, int markets) {
  json out = {{"settings", s}, {"generated_ts", now}, {"rows", json::array()}};
  int n_algo = 0, n_manual = 0, n_weak = 0, n_research = 0, n_blocked = 0;
  for (const auto& d : decisions) {
    if (d.verdict == "algo") ++n_algo; else if (d.verdict == "manual") ++n_manual; else if (d.verdict == "weak") ++n_weak;
    else if (d.verdict == "research") ++n_research; else ++n_blocked;
    out["rows"].push_back(decision_to_row(d));
  }
  auto rank = [](const std::string& v) { return v == "algo" ? 0 : v == "manual" ? 1 : v == "weak" ? 2 : v == "research" ? 3 : 4; };
  std::stable_sort(out["rows"].begin(), out["rows"].end(), [&](const json& a, const json& b) {
    int ra = rank(str(a, "verdict")), rb = rank(str(b, "verdict"));
    if (ra != rb) return ra < rb;
    if (ra == 0) return number(a, "kelly", -100) > number(b, "kelly", -100);
    return number(a, "edge", -100) > number(b, "edge", -100);
  });
  out["counts"] = {{"markets", markets}, {"sides", out["rows"].size()}, {"algo", n_algo}, {"manual", n_manual},
                   {"weak", n_weak}, {"research", n_research}, {"blocked", n_blocked}};
  return out;
}

}  // namespace desk
