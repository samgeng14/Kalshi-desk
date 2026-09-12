#include "desk/systematic.h"
#include "desk/decision_router.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "desk/decision_log.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/risk.h"
#include "desk/route.h"
#include "desk/timeutil.h"

namespace desk {
namespace {
constexpr const char* kStateKey = "systematic:v2";
constexpr long long kMaxSnapshotAge = 120;   // a quote or fair older than this is not a price
constexpr long long kRefusalBackoff = 300;   // seconds before an entry the desk refused is retried
constexpr long long kOwnedAfterClose = 180;  // closed rows stay "owned" until the portfolio confirms flat

bool finite_number(const json& j, const std::string& key) {
  return j.contains(key) && j[key].is_number() && std::isfinite(j[key].get<double>());
}
double number(const json& j, const std::string& key, double fallback = 0) {
  return finite_number(j, key) ? j[key].get<double>() : fallback;
}
long long integer(const json& j, const std::string& key, long long fallback = 0) {
  return finite_number(j, key) ? static_cast<long long>(j[key].get<double>()) : fallback;
}
std::string str(const json& j, const std::string& key, const std::string& fallback = "") {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
bool yes(const json& j, const std::string& key) {
  return j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}
// Where an asset category's rows go (route.h). "auto": the algorithm may send them. "manual":
// screened and priced and handed to you, never sent. "research": screened, priced and recorded,
// offered to nobody. "off": not screened at all. A boolean is the old shape of this setting and
// still means what it used to: true was the algorithm's list, false already put the row in front
// of you rather than dropping it. Never compared as a string or read as a bool anywhere else.
RouteMode route_of(const json& s, const std::string& key) { return route_setting(s, key); }
// Fresh enough to OPEN risk on: the publisher's stale flag says so AND the timestamp is inside
// `max_age`, the desk's one freshness limit for entries (Config::data_max_age), which the board
// also used to set the flag.
bool fresh(const json& j, const std::string& ts_key, const std::string& stale_key, long long now,
           long long max_age = kMaxSnapshotAge) {
  double ts = number(j, ts_key);
  return j.contains(stale_key) && j[stale_key].is_boolean() && !yes(j, stale_key) && ts > 0 && ts <= now &&
         now - ts <= max_age;
}
// Fresh enough to MANAGE what is already on: exits, stops and resting entries. Deliberately the
// older, looser rule (the board's `*_stale_manage` flag, 60 s, with the 120 s backstop): refusing to
// manage a position because a slow cycle made its quote 26 s old would skip a stop, which is a risk
// of its own, and pulling resting entries on it would restart the cancel storms of 2026-09-08.
bool fresh_manage(const json& j, const std::string& ts_key, const std::string& stale_key, long long now,
                  long long max_age = kMaxSnapshotAge) {
  const std::string manage_key = stale_key + "_manage";
  const bool has_manage = j.contains(manage_key) && j[manage_key].is_boolean();
  return fresh(j, ts_key, has_manage ? manage_key : stale_key, now, max_age);
}
double side_price(const json& row, const std::string& side, bool ask) {
  double v = number(row, side + (ask ? "_ask" : "_bid"));
  if (v > 0 && v < 1) return v;
  // The NO book is the YES book seen from the other side: NO ask = 1 - YES bid, NO bid = 1 - YES ask.
  if (side == "no") {
    double y = number(row, ask ? "yes_bid" : "yes_ask");
    return y > 0 && y < 1 ? 1.0 - y : 0.0;
  }
  return 0.0;
}
double side_depth(const json& row, const std::string& side, bool ask) {
  std::string key = side + (ask ? "_ask_size" : "_bid_size");
  if (finite_number(row, key) && number(row, key) > 0) return number(row, key);
  if (side == "no") return number(row, ask ? "yes_bid_size" : "yes_ask_size");
  return number(row, key);
}
FeeModel fee_model(const json& row) { return {str(row, "fee_type", "quadratic"), number(row, "fee_multiplier", 1)}; }
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
double round4(double x) { return std::round(x * 10000.0) / 10000.0; }

// Use exchange fills only when they cover the full acknowledged quantity. Partial/delayed
// fill downloads must not turn a known position into a cheaper or smaller one.
bool actual_fill_cost(Store& store, const std::string& order, const std::string& side,
                      double count, double* price, double* fee) {
  if (order.empty() || count <= 0) return false;
  double n = 0, cost = 0, fees = 0;
  for (const auto& f : store.query("SELECT count,yes_price,no_price,fee FROM fills WHERE order_id=? AND outcome_side=?",
                                  {order, side})) {
    n += number(f, "count");
    cost += number(f, "count") * number(f, side + "_price");
    fees += number(f, "fee");
  }
  if (std::fabs(n - count) > .0001) return false;
  *price = cost / n;
  *fee = fees;
  return true;
}
}  // namespace

std::string systematic_category(const json& m) {
  std::string c = lower(str(m, "category")), t = str(m, "series_ticker");
  if (c.find("weather") != std::string::npos || c.find("climate") != std::string::npos ||
      t.rfind("KXHIGH", 0) == 0 || t.rfind("KXLOW", 0) == 0 || t == "KXDENHIGH")
    return "weather";
  if (c.find("crypto") != std::string::npos || t == "KXBTC" || t == "KXETH" || t == "KXSOL" || t == "KXXRP" ||
      t == "KXDOGE")
    return "crypto";
  if (c.find("sport") != std::string::npos || ends_with(t, "GAME") || ends_with(t, "MATCH")) return "sports";
  if (c.find("commodit") != std::string::npos) return "commodities";
  if (c.find("econom") != std::string::npos) return "economics";
  if (c.find("financ") != std::string::npos || c.find("market") != std::string::npos) return "finance";
  if (c.find("tech") != std::string::npos || c.find("science") != std::string::npos) return "tech_science";
  return "other";
}

json systematic_defaults() {
  return {// what the algorithm may trade
          // auto | manual | research | off, per category: the OUTER permission. Booleans from an older
          // settings row are migrated on read. Weather, sports and crypto are manual until one of
          // their models graduates (session 15); arbitrage baskets are locked, not forecasts.
          {"weather", "manual"}, {"sports", "manual"}, {"crypto", "manual"}, {"other", "manual"}, {"arbitrage", "auto"},
          {"commodities", "manual"}, {"economics", "manual"}, {"finance", "manual"}, {"tech_science", "manual"},
          {"near_day_categories_only", true}, {"settlement_day_tz", "America/Chicago"},
          // Kept so older settings rows still validate. Since session 15 a research-only estimate is
          // routed RESEARCH whatever this says: offered to nobody, sent by nobody.
          {"allow_research_estimates", true},
          // Per-model routes (name_version, or name for every version). The category route above caps
          // them; a model with no entry is research. AUTO also needs the model in graduated_models.
          {"model_routes", {{"weather_nws_v1", "manual"}, {"crypto_lognormal_v1", "manual"}, {"sports_live_v1", "manual"},
                            {"sports_inplay_v1", "research"}, {"sports_totals_v1", "research"}, {"xvenue_v1", "manual"},
                            {"manual_fair_v1", "manual"}, {"ladder_arb", "auto"},
                            // Session 16. The terminal-price commodity models are new and unmeasured:
                            // they are priced and offered, and they are not the algorithm's to send.
                            {"commodity_terminal_gold_v1", "manual"}, {"commodity_terminal_silver_v1", "manual"},
                            {"commodity_terminal_wti_v1", "manual"}, {"commodity_terminal_natgas_v1", "manual"},
                            {"commodity_terminal_copper_v1", "manual"}}},
          {"graduated_models", json::array()},
          // An estimate this uncertain goes to you, not to the algorithm.
          {"auto_max_sigma", .10},
          // A taker's expected cost beyond the touch, per contract, deducted from the executable edge.
          {"slippage_estimate", 0.0},
          // the line between the algorithm and you
          {"min_win_probability", .55}, {"min_edge", .03}, {"max_edge", .12}, {"uncertainty_buffer", .5},
          // The manual lane's own floor. A row a policy hands to you still has to be worth the
          // look: under this it is "weak" and goes to nobody. Set it at or above min_edge.
          {"manual_min_edge", .05},
          {"max_spread", .06}, {"max_model_gap", .15}, {"max_horizon_hours", 48.0},
          {"min_minutes_to_close", 10.0},
          // A floor longer than the contract's whole trading life is not a floor, it is an off
          // switch: at 10 minutes nothing in a 15-minute gold window would ever be tradable. For a
          // contract that short the floor becomes this fraction of its own life instead (3 minutes
          // of a 15-minute window at 0.2). Long-dated markets are untouched: a fifth of their life
          // is hours, so the flat floor above is the smaller of the two and still binds.
          {"min_life_fraction_to_close", 0.2},
          // execution and size
          {"entry_style", "auto"}, {"entry_ttl_seconds", 180.0}, {"per_trade_dollars", 6.0}, {"sports_per_trade_dollars",2.0},
          {"kelly_fraction", .25}, {"max_open_dollars", 15.0}, {"max_positions", 5},
          {"max_entries_per_cycle", 1}, {"capital_budget_dollars", 40.0}, {"daily_budget_dollars", 40.0}, {"daily_loss_stop_dollars", 6.0}, {"daily_loss_stop_enabled",true},
          {"ladder_ticks", 3},
          // exits
          {"take_profit", .06}, {"stop_loss", .10}, {"stop_grace_seconds", 90.0}, {"stop_persist_seconds", 30.0},
          {"stop_loss_mode", "price"}, {"stop_loss_fraction", .70}, {"model_value_exit", true}, {"thesis_stop_enabled", true},
          {"exit_ttl_seconds", 240.0}, {"max_hold_minutes", 360.0}, {"cooldown_minutes", 30.0},
          {"stop_at_day_end", false}, {"paper_slippage", .01}};
}

json validate_systematic_settings(const json& patch, const json& previous) {
  if (!patch.is_object()) throw std::invalid_argument("settings must be an object");
  if (!previous.is_object()) throw std::invalid_argument("previous settings must be an object");
  const json defaults = systematic_defaults();
  json out = defaults;
  for (auto it = previous.begin(); it != previous.end(); ++it)
    if (defaults.contains(it.key())) out[it.key()] = it.value();
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    if (!defaults.contains(it.key())) throw std::invalid_argument("unknown setting: " + it.key());
    out[it.key()] = it.value();
  }
  // Model routes merge entry by entry (defaults, then the saved row, then the patch), so a new
  // model gets its default and setting one model's route never wipes the others.
  {
    json routes = defaults["model_routes"];
    for (const json* src : {&previous, &patch})
      if (src->contains("model_routes")) {
        const json& r = (*src)["model_routes"];
        if (!r.is_object()) throw std::invalid_argument("model_routes must be an object of model -> route");
        for (auto it = r.begin(); it != r.end(); ++it) routes[it.key()] = it.value();
      }
    out["model_routes"] = routes;
  }
  // The nine category switches used to be booleans. A settings row written before this change
  // still holds them that way, and rejecting it would lock the desk out of its own saved state,
  // so migrate in place: true was the algorithm's list, false already meant "show me instead".
  static const std::set<std::string> kRouteKeys{"weather", "sports", "crypto", "other", "arbitrage",
                                                "commodities", "economics", "finance", "tech_science"};
  for (const auto& k : kRouteKeys)
    if (out.contains(k) && out[k].is_boolean()) out[k] = out[k].get<bool>() ? "auto" : "manual";
  for (auto it = out.begin(); it != out.end(); ++it) {
    const json& d = defaults.at(it.key());
    if (d.is_boolean()) {
      if (!it.value().is_boolean()) throw std::invalid_argument(it.key() + " must be true or false");
    } else if (d.is_string()) {
      if (!it.value().is_string()) throw std::invalid_argument(it.key() + " must be a string");
    } else if (d.is_object()) {
      if (!it.value().is_object()) throw std::invalid_argument(it.key() + " must be an object");
    } else if (d.is_array()) {
      if (!it.value().is_array()) throw std::invalid_argument(it.key() + " must be a list");
    } else if (!it.value().is_number() || !std::isfinite(it.value().get<double>())) {
      throw std::invalid_argument(it.key() + " must be a finite number");
    }
  }
  auto range = [&](const std::string& k, double lo, double hi) {
    double x = out[k].get<double>();
    if (x < lo || x > hi)
      throw std::invalid_argument(k + " is outside its supported range [" + fmt2(lo) + ", " + fmt2(hi) + "]");
  };
  auto whole = [&](const std::string& k) {
    double x = out[k].get<double>();
    if (std::floor(x) != x) throw std::invalid_argument(k + " must be a whole number");
    out[k] = static_cast<int>(x);
  };
  for (const auto& k : kRouteKeys) {
    try {
      parse_route(out[k].get<std::string>());
    } catch (const std::invalid_argument&) {
      throw std::invalid_argument(k + " must be auto, manual, research or off");
    }
  }
  auto model_key_ok = [](const std::string& k) {
    if (k.empty() || k.size() > 80) return false;
    for (unsigned char c : k)
      if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
  };
  for (auto it = out["model_routes"].begin(); it != out["model_routes"].end(); ++it) {
    if (!model_key_ok(it.key())) throw std::invalid_argument("model route key \"" + it.key() + "\" is not a model name");
    try {
      if (!it.value().is_string()) throw std::invalid_argument("");
      parse_route(it.value().get<std::string>());
    } catch (const std::invalid_argument&) {
      throw std::invalid_argument("model route for " + it.key() + " must be auto, manual, research or off");
    }
  }
  {
    json grads = json::array();
    for (const auto& g : out["graduated_models"]) {
      if (!g.is_string() || !model_key_ok(g.get<std::string>()))
        throw std::invalid_argument("graduated_models must list model names");
      bool dup = false;
      for (const auto& h : grads) dup = dup || h == g;
      if (!dup) grads.push_back(g);
    }
    out["graduated_models"] = grads;
  }
  std::string style = out["entry_style"].get<std::string>();
  if (style != "auto" && style != "maker" && style != "taker")
    throw std::invalid_argument("entry_style must be auto, maker or taker");
  if (str(out,"stop_loss_mode") != "price" && str(out,"stop_loss_mode") != "fraction")
    throw std::invalid_argument("stop_loss_mode must be price or fraction");
  if (!std::set<std::string>{"UTC","America/Chicago","America/New_York","America/Los_Angeles","America/Denver"}.count(str(out,"settlement_day_tz")))
    throw std::invalid_argument("unsupported settlement day timezone");
  range("min_win_probability", .01, .99); range("min_edge", 0, .5); range("max_edge", .01, 1.0);
  range("manual_min_edge", 0, .5);
  range("uncertainty_buffer", 0, 5); range("max_spread", .01, .5); range("max_model_gap", .01, 1.0);
  range("max_horizon_hours", .1, 48); range("min_minutes_to_close", 0, 1440);
  // Not 0: that would switch the near-close guard off for every market on the desk, in both
  // lanes, which is not what a fraction of a contract's life is for.
  range("min_life_fraction_to_close", .05, 1);
  range("entry_ttl_seconds", 10, 3600); range("per_trade_dollars", .5, 1000); range("sports_per_trade_dollars",.5,1000);
  range("kelly_fraction", 0, 1); range("max_open_dollars", .5, 10000); range("max_positions", 1, 50);
  range("max_entries_per_cycle", 1, 10); range("daily_budget_dollars", .5, 10000);
  range("daily_loss_stop_dollars", .5, 10000); range("take_profit", 0, .99); range("stop_loss", 0, .99);
  range("max_hold_minutes", 0, 2880); range("cooldown_minutes", 0, 1440); range("paper_slippage", 0, .1);
  range("ladder_ticks", 0, 10); range("stop_grace_seconds", 0, 3600); range("stop_persist_seconds", 0, 3600);
  range("exit_ttl_seconds", 10, 3600);
  range("capital_budget_dollars", .5, 10000); range("stop_loss_fraction", .01, .99);
  range("auto_max_sigma", .001, .5); range("slippage_estimate", 0, .1);
  whole("max_positions"); whole("max_entries_per_cycle"); whole("ladder_ticks");
  if (number(out, "min_edge") > number(out, "max_edge"))
    throw std::invalid_argument("min_edge must not exceed max_edge");
  // A manual floor under the algorithm's own floor would mean the rows you see are the ones the
  // algorithm rejected as too thin, which is the wrong way round.
  if (number(out, "manual_min_edge") + 1e-12 < number(out, "min_edge"))
    throw std::invalid_argument("manual_min_edge must not be below min_edge");
  if (number(out, "per_trade_dollars") > number(out, "max_open_dollars"))
    throw std::invalid_argument("per_trade_dollars must not exceed max_open_dollars");
  return out;
}

// ---- the screen ----------------------------------------------------------------------------

json systematic_screen(const json& board, const json& settings, long long now,
                       const std::set<std::string>& held_events,
                       const std::map<std::string, long long>& cooldowns, double hard_gap) {
  // A policy view: what the router decides with the master switch armed. The engine passes its
  // real switch state through the RouterContext overload.
  RouterContext ctx;
  ctx.now = now;
  ctx.held_events = held_events;
  ctx.cooldowns = cooldowns;
  ctx.hard_gap = hard_gap;
  return systematic_screen(board, settings, ctx);
}

json systematic_screen(const json& board, const json& settings, const RouterContext& ctx) {
  const json s = validate_systematic_settings(json::object(), settings);
  int markets = 0;
  if (board.contains("markets") && board["markets"].is_array())
    for (const auto& m : board["markets"]) if (m.is_object()) ++markets;
  return decisions_to_screen(decide_board(board, s, ctx), s, ctx.now, markets);
}

// ---- the trader ----------------------------------------------------------------------------

// A resting entry is re-validated at its own limit price every cycle: a fresh model timestamp is
// not permission to leave an obsolete price on the exchange. Empty = leave it resting.
std::string systematic_entry_cancel_reason(const json& t, const json& m, const json& s,
                                           long long now, double hard_gap, long long max_age) {
  // A resting entry is managed, not opened: the management freshness rule applies (fresh_manage).
  if (!fresh_manage(m, "quote_ts", "quote_stale", now, max_age)) return "quote missing or stale";
  if (!fresh_manage(m, "fair_ts", "fair_stale", now, max_age)) return "model estimate missing or stale";
  if (str(m, "market_status", str(m, "status")) != "active" &&
      str(m, "market_status", str(m, "status")) != "open") return "market is not open";
  if (number(m, "settlement_ts") <= now || number(m, "close_ts") <= now)
    return "market close or settlement time has passed or is unknown";
  if (number(m, "settlement_ts") > now + number(s, "max_horizon_hours") * 3600)
    return "settlement is outside the algorithm horizon";
  if (number(m, "minutes_to_close", -1) < min_minutes_to_close_for(m, s)) return "too near market close";
  {
    const RouteMode r = route_of(s, systematic_category(m));
    if (r == RouteMode::Off) return "asset category switched off";
    if (r != RouteMode::Auto)
      return systematic_category(m) + " is set to " + route_name(r) + ": it is not the algorithm's to send";
  }
  const auto category = systematic_category(m);
  if (yes(s,"near_day_categories_only") &&
      (category=="economics" || category=="finance" || category=="tech_science")) {
    const auto zone=str(s,"settlement_day_tz"), today=trading_day(zone,now);
    const auto tomorrow=iso_of(parse_iso(today+"T12:00:00Z")+86400).substr(0,10);
    const auto day=trading_day(zone,integer(m,"settlement_ts"));
    if(day!=today && day!=tomorrow) return "only today or tomorrow settlement is enabled";
  }
  if (m.contains("model_inputs") && m["model_inputs"].is_object() &&
      m["model_inputs"].contains("valid") && !yes(m["model_inputs"], "valid")) return "model inputs failed their own checks";
  if (m.contains("model_inputs") && m["model_inputs"].is_object() &&
      m["model_inputs"].contains("auto_eligible") && !yes(m["model_inputs"], "auto_eligible"))
    return "model source is not eligible for automatic execution";
  if (!finite_number(m, "p_model") || number(m, "p_model") <= 0 || number(m, "p_model") >= 1 ||
      !finite_number(m, "sigma") || number(m, "sigma") < 0 || number(m, "sigma") > .5)
    return "probability or uncertainty is missing/invalid";
  if (!str(t, "fair_strategy").empty() && str(t, "fair_strategy") != str(m, "fair_strategy"))
    return "model source changed";
  // The model's own route and standing, as the router reads them: a resting entry stays only while
  // the decision that sent it would still be AUTO.
  const ProbabilityQuote q = m.contains("quote") && m["quote"].is_object() ? quote_from_json(m["quote"])
                                                                            : quote_from_board_row(m);
  {
    bool known = false;
    const RouteMode mr = model_route(s, q.model_name, q.model_version, &known);
    if (mr != RouteMode::Auto)
      return "model " + q.model_key() + " is routed to " + route_name(mr) + (known ? "" : " (no route set)") +
             ": it is not the algorithm's to send";
    if (!model_graduated(s, q.model_name, q.model_version))
      return "model " + q.model_key() + " has not graduated to unattended execution";
    if (!q.settlement_verified) return "settlement of this contract is not verified for " + q.model_key();
    if (q.research_only) return "model is no longer eligible for automatic execution: research estimate";
    if (std::isfinite(q.sigma) && q.sigma > number(s, "auto_max_sigma", .10) + 1e-9)
      return "model uncertainty is above the automatic cap";
  }
  if (yes(m, "model_research_only") && lower(str(m, "model_research_reason")).find("disagreement") != std::string::npos)
    return "model is no longer eligible for automatic execution";
  const MarketSidePrices book = normalize_prices_from_row(m);
  const Side held_side = side_from(str(t, "side"));
  const double bid = book.bid(held_side), ask = book.ask(held_side);
  if (bid <= 0 || ask <= 0 || bid > ask || ask >= 1) return "two-sided quote unavailable";
  if (ask - bid > number(s, "max_spread") + 1e-9) return "spread widened beyond the entry limit";
  double raw = number(m, "p_raw", number(m, "p_model"));
  double mid = .5 * (number(m, "yes_bid") + number(m, "yes_ask"));
  if (std::fabs(raw - mid) > std::min(hard_gap, number(s, "max_model_gap")) + 1e-9)
    return "model/market disagreement exceeds the entry limit";
  // The edge at the resting price, from the one edge engine: the model's quote against this book,
  // less the fee, the slippage estimate and the uncertainty deduction, exactly as the router prices.
  EdgeSettings es;
  es.uncertainty_multiplier = number(s, "uncertainty_buffer");
  es.taker_slippage = number(s, "slippage_estimate");
  const EdgeCandidate at = candidate_at(q, book, held_side,
                                        str(t, "entry_style") == "taker" ? "taker" : "maker", number(t, "entry_price"),
                                        fee_model(m), es);
  if (!(at.fair_probability >= number(s, "min_win_probability") - 1e-9)) return "win probability fell below the entry floor";
  if (!at.executable || at.executable_edge < number(s, "min_edge") - 1e-9) return "edge vanished at the resting order price";
  if (at.net_edge > number(s, "max_edge") + 1e-9) return "edge exceeds the automatic entry cap";
  return "";
}

namespace {
json initial_state() {
  return {{"schema", 2}, {"running", false}, {"mode", "live"}, {"settings", systematic_defaults()},
          {"settings_version", 1}, {"halted", false}, {"halt_reason", ""}, {"halt_class", ""}, {"day", ""},
          {"cooldowns", json::object()}, {"log", json::array()}, {"last_step_ts", 0},
          {"started_ts", 0}, {"stopped_ts", 0}, {"loss_stop_base", 0.0}};
}
}  // namespace

SystematicTrader::SystematicTrader(Store& store, Executor* exec, const Config& cfg, Hooks hooks)
    : store_(store), exec_(exec), cfg_(cfg), hooks_(std::move(hooks)), state_(initial_state()) {
  store_.exec(
      "CREATE TABLE IF NOT EXISTS systematic_trades(id INTEGER PRIMARY KEY, ticker TEXT, event_ticker TEXT, "
      "series_ticker TEXT, title TEXT, side TEXT, state TEXT, mode TEXT, opened_ts INTEGER, filled_ts INTEGER, "
      "closed_ts INTEGER, day TEXT, entry_proposal_id INTEGER, entry_order_id TEXT, entry_style TEXT, "
      "entry_price REAL, count REAL, filled REAL, entry_cost REAL, entry_fee REAL, exit_proposal_id INTEGER, "
      "exit_order_id TEXT, exit_price REAL, exit_proceeds REAL, exit_fee REAL, exit_reason TEXT, realized_pnl REAL, "
      "p_win REAL, edge REAL, kelly REAL, settings_version INTEGER, note TEXT, mark_price REAL, mark_ts INTEGER, "
      "unrealized REAL, exit_attempt_ts INTEGER, fair_strategy TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_systematic_state ON systematic_trades(state, mode)");
  // Session 12 columns: a stop that must persist before it fires, and resting (maker) exits.
  {
    std::set<std::string> cols;
    for (const auto& c : store_.query("PRAGMA table_info(systematic_trades)")) cols.insert(c.value("name", ""));
    if (!cols.count("stop_seen_ts")) store_.exec("ALTER TABLE systematic_trades ADD COLUMN stop_seen_ts INTEGER DEFAULT 0");
    if (!cols.count("exit_style")) store_.exec("ALTER TABLE systematic_trades ADD COLUMN exit_style TEXT DEFAULT ''");
    if (!cols.count("exit_posted_ts")) store_.exec("ALTER TABLE systematic_trades ADD COLUMN exit_posted_ts INTEGER DEFAULT 0");
    if (!cols.count("exit_pending_price")) store_.exec("ALTER TABLE systematic_trades ADD COLUMN exit_pending_price REAL DEFAULT 0");
    if (!cols.count("exit_steps")) store_.exec("ALTER TABLE systematic_trades ADD COLUMN exit_steps INTEGER DEFAULT 0");
  }
  if (auto saved = store_.kv_get(kStateKey)) {
    json parsed = json::parse(*saved, nullptr, false);
    if (parsed.is_object() && parsed.value("schema", 0) == 2) {
      try {
        parsed["settings"] = validate_systematic_settings(json::object(), parsed.value("settings", json::object()));
        state_.update(parsed);
      } catch (const std::exception& e) {
        LOG_WARN("systematic: persisted settings invalid (%s); defaults restored", e.what());
      }
    }
  }
  if (!state_.contains("log") || !state_["log"].is_array()) state_["log"] = json::array();
  // Session 15: weather, crypto and sports were AUTO in the saved settings although none of their
  // models had graduated. A saved AUTO route for those three becomes MANUAL once; a choice made
  // after this (on the Systematic tab, or in config/desk.json, applied next) stands. Probability
  // generation and book capture are untouched: manual still captures.
  if (store_.kv_get("systematic:route_migration").value_or("") != "s15") {
    std::string moved;
    for (const char* k : {"weather", "crypto", "sports"})
      if (str(state_["settings"], k) == "auto") {
        state_["settings"][k] = "manual";
        moved += std::string(moved.empty() ? "" : ", ") + k;
      }
    if (!moved.empty()) {
      state_["settings_version"] = integer(state_, "settings_version") + 1;
      log("warn", "routes changed at startup: " + moved +
                      " moved from auto to manual until a model graduates (session 15); nothing in them is sent unattended");
    }
    store_.kv_set("systematic:route_migration", "s15");
  }
  // Settings named in config/desk.json win over the saved blob, so a durable preference can live
  // in a file that is versioned and readable instead of only inside the database. It is applied
  // every start, on purpose: that is what makes it durable rather than a one-time nudge the next
  // dashboard save silently undoes.
  if (cfg_.systematic_settings.is_object() && !cfg_.systematic_settings.empty()) {
    try {
      json seeded = validate_systematic_settings(cfg_.systematic_settings, state_["settings"]);
      if (seeded != state_["settings"]) {
        std::string changed;
        for (auto it = seeded.begin(); it != seeded.end(); ++it)
          if (state_["settings"].value(it.key(), json()) != it.value())
            changed += (changed.empty() ? "" : ", ") + it.key() + "=" + it.value().dump();
        state_["settings"] = seeded;
        state_["settings_version"] = integer(state_, "settings_version") + 1;
        log("info", "settings from config/desk.json: " + changed);
      }
    } catch (const std::exception& e) {
      LOG_WARN("systematic: config settings rejected (%s); saved settings kept", e.what());
      log("warn", std::string("config settings rejected: ") + e.what());
    }
  }
  // A halt written by an older build for a refusal that is no longer a stop reason (reject.h):
  // "post only cross" and the other market-state refusals mean the book moved, not that the desk
  // is wrong. The halt is cleared, but the algorithm is left STOPPED rather than resumed: it has
  // been idle since, and the first live order after a restart should come from a person pressing
  // Start, not from a code change.
  {
    static const std::string kPrefix = "order rejected by the exchange: ";
    const std::string reason = str(state_, "halt_reason");
    if (yes(state_, "halted") && reason.rfind(kPrefix, 0) == 0) {
      // A halt written by this build carries its class. One written before carries only the
      // body, and every such halt came from a 4xx that was not a 429 (the old rule), so it is
      // read as a 400; a 401/403 halt from then cannot be told apart by its body and stays unless
      // its body is one of the explicit market-state phrases.
      const std::string saved = str(state_, "halt_class");
      const RejectClass cls = saved.empty() ? classify_rejection(400, reason.substr(kPrefix.size()))
                                            : reject_class_from_name(saved);
      if (!reject_trips_kill(cls)) {
        state_["halted"] = false;
        state_["halt_reason"] = "";
        state_["halt_class"] = "";
        const bool was_running = yes(state_, "running");
        state_["running"] = false;
        state_["stopped_ts"] = now_s();
        log("warn", "halt cleared at startup: it was raised by a " + std::string(reject_class_name(cls)) +
                        " refusal (" + reason.substr(kPrefix.size(), 100) + "), which no longer halts the algorithm; " +
                        (was_running ? "left stopped until you press Start" : "it stays stopped"));
      }
    }
  }
  if (state_["mode"] == "live" && (!exec_ || !exec_->armed())) {
    if (yes(state_, "running")) log("warn", "live mode needs an API key: algorithm stopped");
    state_["running"] = false;
  }
  if (yes(state_, "running")) log("warn", "algorithm resumed after a restart (" + str(state_, "mode") + ")");
  persist();
}

void SystematicTrader::persist() {
  json copy = state_;
  store_.kv_set(kStateKey, copy.dump());
}

void SystematicTrader::log(const std::string& level, const std::string& msg) {
  json e = {{"ts", now_s()}, {"level", level}, {"msg", msg}};
  state_["log"].push_back(e);
  while (state_["log"].size() > 200) state_["log"].erase(state_["log"].begin());
  if (level == "warn" || level == "error" || level == "trade") store_.log_event(level == "trade" ? "info" : level, "SYS: " + msg);
  if (level == "error") LOG_ERR("systematic: %s", msg.c_str());
  else if (level == "warn") LOG_WARN("systematic: %s", msg.c_str());
  else LOG_INFO("systematic: %s", msg.c_str());
}

bool SystematicTrader::running() { std::lock_guard<std::mutex> lk(mu_); return yes(state_, "running"); }

std::set<std::string> SystematicTrader::owned_tickers() {
  std::set<std::string> out;
  long long now = now_s();
  for (const auto& r : store_.query(
           "SELECT ticker FROM systematic_trades WHERE mode='live' AND (state IN ('entering','open','exiting','settling') "
           "OR (state='closed' AND closed_ts>?))", {now - kOwnedAfterClose}))
    out.insert(r.value("ticker", ""));
  return out;
}

std::set<std::string> SystematicTrader::blocked_events(const std::string& mode) {
  std::set<std::string> out;
  for (const auto& t : store_.query(
           "SELECT event_ticker FROM systematic_trades WHERE mode=? AND state IN "
           "('entering','open','exiting','settling')", {mode}))
    out.insert(t.value("event_ticker", ""));
  // Anything the account already holds, however it got there, belongs to whoever put it on.
  // Paper applies the same rule as live on purpose: a paper ledger that takes trades the live
  // sender would refuse is not a forecast of live, it is a different strategy.
  for (const auto& p : store_.positions())
    if (p.position != 0)
      if (auto m = store_.get_market(p.ticker)) out.insert(m->event_ticker);
  out.erase("");
  return out;
}

json SystematicTrader::trades(const std::string& state_filter) {
  if (state_filter.empty())
    return store_.query("SELECT * FROM systematic_trades ORDER BY id DESC LIMIT 400");
  return store_.query("SELECT * FROM systematic_trades WHERE state=? ORDER BY id", {state_filter});
}

void SystematicTrader::upsert_trade(json& t) {
  auto s = [&](const char* k) { return SqlValue(str(t, k)); };
  auto n = [&](const char* k) { return SqlValue(number(t, k)); };
  auto i = [&](const char* k) { return SqlValue(integer(t, k)); };
  if (integer(t, "id") == 0) {
    store_.exec(
        "INSERT INTO systematic_trades(ticker,event_ticker,series_ticker,title,side,state,mode,opened_ts,filled_ts,closed_ts,"
        "day,entry_proposal_id,entry_order_id,entry_style,entry_price,count,filled,entry_cost,entry_fee,exit_proposal_id,"
        "exit_order_id,exit_price,exit_proceeds,exit_fee,exit_reason,realized_pnl,p_win,edge,kelly,settings_version,note,"
        "mark_price,mark_ts,unrealized,exit_attempt_ts,fair_strategy) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {s("ticker"), s("event_ticker"), s("series_ticker"), s("title"), s("side"), s("state"), s("mode"), i("opened_ts"),
         i("filled_ts"), i("closed_ts"), s("day"), i("entry_proposal_id"), s("entry_order_id"), s("entry_style"),
         n("entry_price"), n("count"), n("filled"), n("entry_cost"), n("entry_fee"), i("exit_proposal_id"), s("exit_order_id"),
         n("exit_price"), n("exit_proceeds"), n("exit_fee"), s("exit_reason"), n("realized_pnl"), n("p_win"), n("edge"),
         n("kelly"), i("settings_version"), s("note"), n("mark_price"), i("mark_ts"), n("unrealized"), i("exit_attempt_ts"),
         s("fair_strategy")});
    json row = store_.query("SELECT last_insert_rowid() AS id");
    t["id"] = row.empty() ? 0 : row[0].value("id", 0LL);
    return;
  }
  store_.exec(
      "UPDATE systematic_trades SET state=?,filled_ts=?,closed_ts=?,entry_order_id=?,entry_price=?,count=?,filled=?,"
      "entry_cost=?,entry_fee=?,exit_proposal_id=?,exit_order_id=?,exit_price=?,exit_proceeds=?,exit_fee=?,exit_reason=?,"
      "realized_pnl=?,note=?,mark_price=?,mark_ts=?,unrealized=?,exit_attempt_ts=?,stop_seen_ts=?,exit_style=?,"
      "exit_posted_ts=?,exit_pending_price=?,exit_steps=? WHERE id=?",
      {s("state"), i("filled_ts"), i("closed_ts"), s("entry_order_id"), n("entry_price"), n("count"), n("filled"),
       n("entry_cost"), n("entry_fee"), i("exit_proposal_id"), s("exit_order_id"), n("exit_price"), n("exit_proceeds"),
       n("exit_fee"), s("exit_reason"), n("realized_pnl"), s("note"), n("mark_price"), i("mark_ts"), n("unrealized"),
       i("exit_attempt_ts"), i("stop_seen_ts"), s("exit_style"), i("exit_posted_ts"), n("exit_pending_price"), i("exit_steps"), i("id")});
}

double SystematicTrader::spent_today() {
  auto v = store_.kv_get("sys:spent:" + str(state_, "day"));
  return v ? std::atof(v->c_str()) : 0.0;
}
void SystematicTrader::add_spent(double cost) {
  store_.kv_set("sys:spent:" + str(state_, "day"), std::to_string(spent_today() + cost));
}

// Local midnight that started the current trading day, as an epoch.
static long long day_start_epoch(const std::string& day, const std::string& tz, long long now) {
  if (day.size() < 10) return 0;
  return parse_iso(day + "T00:00:00Z") - tz_offset_seconds(tz, now);
}

// Realized on trades closed today plus the mark on what is open: the number the loss stop reads.
double SystematicTrader::pnl_today(long long now) {
  std::string mode = str(state_, "mode");
  double realized = 0, unrealized = 0;
  long long since = day_start_epoch(str(state_, "day"), cfg_.risk.day_tz, now);
  for (const auto& r : store_.query(
           "SELECT COALESCE(SUM(realized_pnl),0) AS r FROM systematic_trades WHERE mode=? AND state='closed' AND closed_ts>=?",
           {mode, since}))
    realized = r.value("r", 0.0);
  for (const auto& r : store_.query(
           "SELECT COALESCE(SUM(unrealized),0) AS u FROM systematic_trades WHERE mode=? AND state IN ('open','exiting','settling')",
           {mode}))
    unrealized = r.value("u", 0.0);
  return realized + unrealized;
}

void SystematicTrader::roll_day(long long now) {
  std::string day = trading_day(cfg_.risk.day_tz, now);
  if (str(state_, "day") == day) return;
  bool first = str(state_, "day").empty();
  state_["day"] = day;
  if (first) return;
  state_["loss_stop_base"] = 0.0;
  if (yes(state_, "halted") && str(state_, "halt_reason").find("loss stop") != std::string::npos) {
    state_["halted"] = false;
    state_["halt_reason"] = "";
    state_["halt_class"] = "";
    log("info", "new trading day " + day + ": daily loss stop cleared");
  }
  if (yes(state_, "running") && yes(state_["settings"], "stop_at_day_end")) {
    state_["running"] = false;
    state_["stopped_ts"] = now;
    log("warn", "trading day ended: algorithm stopped (stop_at_day_end); open positions keep their exit rules until closed");
  } else {
    log("info", "new trading day " + day + ": budget reset");
  }
}

json systematic_capital(Store& store, const json& settings, const std::string& mode) {
  double committed=0, realized=0, transferred=std::atof(store.kv_get("systematic_transferred:"+mode).value_or("0").c_str());
  for (const auto& t : store.query("SELECT state,entry_cost,count,entry_price,entry_fee,realized_pnl FROM systematic_trades WHERE mode=?",{mode})) {
    const auto state = str(t,"state");
    realized += number(t,"realized_pnl");
    if (state=="released") transferred += number(t,"entry_cost");
    else if (state=="entering") committed += number(t,"count")*number(t,"entry_price")+number(t,"entry_fee");
    else if (state=="open"||state=="exiting"||state=="settling") committed += number(t,"entry_cost");
  }
  const double budget=number(settings,"capital_budget_dollars",40);
  const double available=std::max(0.0,budget+std::min(0.0,realized)-transferred-committed);
  return {{"budget",budget},{"committed",round4(committed)},{"realized",round4(realized)},
    {"released_to_manual",round4(transferred)},{"available",round4(available)},
    {"notice","Strategy allocation only; profits do not enlarge the cap. Manual trades, pairs and perps have separate ownership; this does not transfer money."}};
}

// ---- exit rule, shared by live and paper --------------------------------------------------

SystematicTrader::ExitPlan SystematicTrader::plan_exit(json& t, const json& m, double bid, double bid_size, double ask,
                                                       long long now, bool* stale) const {
  const json& s = state_["settings"];
  ExitPlan none;
  if (stale) *stale = false;
  if (bid <= 0 || bid >= 1) return none;
  FeeModel fm = fee_model(m);
  double entry = number(t, "entry_price"), filled = number(t, "filled");
  double tick = number(m, "tick", .01);
  if (tick <= 0) tick = .01;
  bool two_sided = ask > bid && ask < 1;
  double mid = two_sided ? .5 * (bid + ask) : bid;
  double cost_per = filled > 0 && number(t, "entry_cost") > 0 ? number(t, "entry_cost") / filled : entry;
  double gain = bid - fm.fee(bid, 1, true) - cost_per;   // per contract, after the fees on both legs
  const bool fractional_stop = str(s,"stop_loss_mode") == "fraction";
  double tp = number(s, "take_profit"), sl = fractional_stop ? cost_per*number(s,"stop_loss_fraction") : number(s, "stop_loss"), hold = number(s, "max_hold_minutes");
  const double stop_basis = fractional_stop ? cost_per : entry;
  const double stop_mark = fractional_stop ? mid - fm.fee(mid,1,true) : mid;
  double grace = number(s, "stop_grace_seconds"), persist = number(s, "stop_persist_seconds");
  long long since = integer(t, "filled_ts") > 0 ? integer(t, "filled_ts") : integer(t, "opened_ts");
  // Can the bid actually take us, and is it a price or a hole? On a thin book the bid after our
  // own fill is often whatever was left far below; a stop that reads it as the market dumps a
  // position worth 0.9 into a 0.6 print (AUS B100.5, 2026-09-08, twelve seconds after the fill).
  bool deep = bid_size + 1e-9 >= filled;
  bool hole = two_sided && (mid - bid) > std::max(.02, sl / 2);
  // A resting sell one tick inside the offer, or joining the offer when the spread is one tick.
  // Every resting exit that came back unfilled steps the next one a tick lower (exit_steps), down
  // to a tick above the bid; past that the ladder ends in the bid itself.
  int steps = static_cast<int>(integer(t, "exit_steps"));
  double maker_px = !two_sided ? 0 : (ask - bid >= 2 * tick - 1e-9 ? floor_to_tick(ask - tick, tick) : ask);
  bool ladder_exhausted = false;
  if (maker_px > 0 && steps > 0) {
    maker_px = floor_to_tick(maker_px - steps * tick, tick);
    if (maker_px <= bid + 1e-9) { maker_px = std::max(bid + tick, tick); ladder_exhausted = true; }
    if (maker_px >= ask - 1e-9) maker_px = ask - tick;
  }
  auto taker = [&](const std::string& why) { ExitPlan e; e.reason = why; e.style = "taker"; e.price = bid; return e; };
  auto maker = [&](const std::string& why) {
    if (maker_px <= 0 || (ladder_exhausted && deep)) return taker(why + (ladder_exhausted ? "; ladder exhausted" : ""));
    ExitPlan e; e.reason = why + " (resting at " + fmt2(maker_px) + (steps > 0 ? ", step " + std::to_string(steps) : "") + ")";
    e.style = "maker"; e.price = maker_px; return e;
  };
  auto best = [&](const std::string& why) { return (deep && !hole) ? taker(why) : maker(why); };
  if (tp > 0 && gain >= tp - 1e-9) return taker("take profit: bid " + fmt2(bid) + " nets " + fmt2(gain) + " over entry " + fmt2(entry));
  // Hold value against executable sale: the position is worth p(win) a contract at settlement;
  // if the bid, net of the fee to hit it, pays more than that, the market is paying more than the
  // position is worth. The entry price is sunk and plays no part in this comparison.
  bool fair_ok = fresh_manage(m, "fair_ts", "fair_stale", now) && finite_number(m, "p_model");
  double win = -1;
  if (fair_ok) {
    double p_yes = number(m, "p_model");
    win = str(t, "side") == "yes" ? p_yes : 1 - p_yes;
    if (yes(s,"model_value_exit") && win + .005 <= bid - fm.fee(bid, 1, true))
      return taker("market pays more than the position is worth: bid " + fmt2(bid) + " net vs p(win) " + fmt2(win));
    // The thesis stop: the model itself now values the contract sl under what was paid. The
    // estimate that put the trade on has moved against it by more than the whole edge; the
    // position is closed at the best price the book gives, not held on hope.
    if (yes(s,"thesis_stop_enabled") && sl > 0 && stop_basis - win >= sl - 1e-9)
      return best("thesis stop: p(win) " + fmt2(win) + " is " + fmt2(entry - win) + " under entry " + fmt2(entry));
  } else if (stale) {
    *stale = true;
  }
  // The market stop: measured on the mid of a two-sided book, never inside the grace period after
  // the fill (our own fill is what emptied the bid), and only once it has persisted. It fires into
  // the bid when the bid is real and near the mid, otherwise as a resting sell; twice the stop on
  // the mid is a disaster and goes into whatever bid there is.
  bool market_stop = sl > 0 && two_sided && stop_basis - stop_mark >= sl - 1e-9 && now - since >= static_cast<long long>(grace);
  if (market_stop) {
    if (integer(t, "stop_seen_ts") <= 0) { t["stop_seen_ts"] = now; if (persist > 0) return none; }
    if (now - integer(t, "stop_seen_ts") < static_cast<long long>(persist)) return none;
    std::string why = (fractional_stop ? "capital loss stop: " : "stop: ") + std::string("mid ") + fmt2(mid) + " is " + fmt2(stop_basis - stop_mark) + " under cost basis " + fmt2(stop_basis) + " for " +
                      std::to_string(now - integer(t, "stop_seen_ts")) + "s";
    if (stop_basis - stop_mark >= 2 * sl - 1e-9) return taker(why + "; twice the stop, out at any bid");
    if (deep && entry - bid <= sl + .03 + 1e-9) return taker(why);
    return maker(why);
  }
  if (integer(t, "stop_seen_ts") > 0) t["stop_seen_ts"] = 0;
  if (hold > 0 && since > 0 && now - since >= static_cast<long long>(hold * 60))
    return best("time stop: held " + std::to_string((now - since) / 60) + " min");
  return none;
}

std::string SystematicTrader::decide_exit(const json& trade, const json& m, double bid, double bid_size, long long now,
                                          bool* stale) const {
  json t = trade;
  return plan_exit(t, m, bid, bid_size, side_price(m, str(trade, "side"), true), now, stale).reason;
}

// ---- live --------------------------------------------------------------------------------------

void SystematicTrader::finalize(json& t, const std::string& reason, double realized, double proceeds, double exit_fee,
                                double exit_price, long long now) {
  t["state"] = "closed";
  t["closed_ts"] = now;
  t["exit_reason"] = reason;
  t["realized_pnl"] = round4(number(t, "realized_pnl") + realized);
  t["exit_proceeds"] = round4(number(t, "exit_proceeds") + proceeds);
  t["exit_fee"] = round4(number(t, "exit_fee") + exit_fee);
  t["exit_price"] = exit_price;
  t["unrealized"] = 0.0;
  t["filled"] = 0.0;
  t["exit_steps"] = 0;
  upsert_trade(t);
  const json& s = state_["settings"];
  if (number(s, "cooldown_minutes") > 0)
    state_["cooldowns"][str(t, "event_ticker")] = now + static_cast<long long>(number(s, "cooldown_minutes") * 60);
  log("trade", str(t, "ticker") + " " + str(t, "side") + " closed: " + reason + " -> P&L " +
                   (number(t, "realized_pnl") >= 0 ? "+" : "") + fmt2(number(t, "realized_pnl")));
}

bool SystematicTrader::live_send_exit(json& t, const std::string& reason, double bid, double bid_size, long long now,
                                      const std::string& style, double maker_price) {
  const std::string ticker = str(t, "ticker"), side = str(t, "side");
  const bool maker = style == "maker" && maker_price > 0 && maker_price < 1;
  const double px = maker ? maker_price : bid;
  double held = number(t, "filled");
  double n = held;
  if (!maker && bid_size > 0 && bid_size < n) n = std::floor(bid_size * 100) / 100;   // sell what the book shows
  if (n < 0.01) return false;
  t["exit_attempt_ts"] = now;
  // An order on the same ticker is not necessarily ours. Leave manual/external orders alone;
  // Executor will not allow an exit through a conflicting resting order either.
  for (const auto& o : store_.orders("resting"))
    if (o.ticker == ticker && o.order_id != str(t, "entry_order_id") && o.order_id != str(t, "exit_order_id")) {
      log("warn", ticker + ": systematic exit deferred; an unrelated order is resting (left untouched)");
      upsert_trade(t);
      return false;
    }
  // Only orders explicitly recorded on this strategy row may be canceled by its exit.
  for (const auto& o : store_.orders("resting"))
    if (o.ticker == ticker && (o.order_id == str(t, "entry_order_id") || o.order_id == str(t, "exit_order_id"))) {
      if (o.order_id == str(t, "exit_order_id") && str(t, "exit_style") == "maker") {
        json current_market = json::object();
        if (auto current = store_.get_market(ticker)) current_market = market_to_json(*current);
        if (auto sr = store_.get_series(str(t, "series_ticker"))) {
          current_market["fee_type"] = sr->fee_type; current_market["fee_multiplier"] = sr->fee_multiplier;
        }
        if (live_manage_resting_exit(t, current_market, held, now, true)) return str(t, "state") == "closed";
        held = number(t, "filled"); n = std::min(n, held);
        if (n < .01) return false;
        continue;
      }
      try {
        exec_->cancel(o.order_id);
        hooks_.request_reconcile();
        log("info", ticker + ": pulled resting order " + o.order_id.substr(0, 8) + " so the exit can go");
      } catch (const std::exception& e) {
        log("warn", ticker + ": could not cancel resting order before the exit: " + e.what());
        return false;
      }
    }
  auto m = store_.get_market(ticker);
  FeeModel fm;
  if (auto s = store_.get_series(m ? m->series_ticker : str(t, "series_ticker"))) fm = FeeModel::from_series(*s);
  Proposal p;
  p.ticker = ticker;
  p.event_ticker = str(t, "event_ticker");
  p.series_ticker = str(t, "series_ticker");
  p.strategy = "sys_exit";
  p.mode = "bot";
  p.outcome_side = side == "yes" ? "no" : "yes";   // sell our side = buy the other at 1 - price
  p.exec_style = maker ? "maker" : "taker";
  p.status = "pending";
  p.price = clamp_price(1.0 - px, m && m->tick > 0 ? m->tick : 0.01);
  p.count = n;
  p.fee_per = fm.fee(p.price, 1, !maker);
  p.yes_bid = m ? m->yes_bid : 0;
  p.yes_ask = m ? m->yes_ask : 0;
  p.cluster = m ? risk_cluster(store_, *m) : p.event_ticker;
  p.note = "systematic exit: " + reason;
  store_.insert_proposal(p);
  t["exit_proposal_id"] = p.id;
  try {
    exec_->submit_exit(p);
  } catch (const ApiError& e) {
    log("error", ticker + ": exit refused by the exchange (" + std::to_string(e.status) + "): " + e.body.substr(0, 120) +
                     " - position still open, retrying next cycle");
    upsert_trade(t);
    return false;
  } catch (const std::exception& e) {
    log("warn", ticker + ": exit not sent: " + e.what());
    upsert_trade(t);
    return false;
  }
  t["exit_order_id"] = p.order_id;
  hooks_.request_reconcile();
  json o = store_.query("SELECT fill_count, remaining_count FROM orders WHERE order_id=?", {p.order_id});
  double fill = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
  double remaining = o.empty() ? 0.0 : o[0].value("remaining_count", 0.0);
  if (maker && remaining > 0) {
    // Resting. The row waits in "exiting" and live_manage_resting_exit books whatever fills.
    t["state"] = "exiting";
    t["exit_style"] = "maker";
    t["exit_posted_ts"] = now;
    t["exit_pending_price"] = px;
    upsert_trade(t);
    log("trade", ticker + ": exit resting, " + fmt2(n) + " @ " + fmt2(px) + " (" + reason + ")");
    return true;
  }
  double exit_fee = fm.fee(px, fill, !maker);
  if (fill <= 0) {
    log("info", ticker + ": exit at " + fmt2(px) + " did not fill (" + reason + "); retrying next cycle");
    upsert_trade(t);
    return false;
  }
  // Proceeds of selling `fill` of our side at `px`: $1 released per matched pair minus the price
  // paid for the offsetting contract, i.e. px a contract, minus the fee. The recorded fills, when
  // they cover the whole quantity, replace the limit price with what was actually paid.
  double share = held > 0 ? fill / held : 1.0;
  double entry_cost_part = number(t, "entry_cost") * share;
  double actual_offset = p.price;
  double exit_price = px;
  if (actual_fill_cost(store_, p.order_id, p.outcome_side, fill, &actual_offset, &exit_fee)) exit_price = 1.0 - actual_offset;
  double proceeds = fill * exit_price;
  double realized = proceeds - exit_fee - entry_cost_part;
  if (fill >= held - 1e-9) {
    finalize(t, reason, realized, proceeds, exit_fee, exit_price, now);
  } else {
    t["filled"] = round4(held - fill);
    t["entry_cost"] = round4(number(t, "entry_cost") - entry_cost_part);
    t["realized_pnl"] = round4(number(t, "realized_pnl") + realized);
    t["exit_proceeds"] = round4(number(t, "exit_proceeds") + proceeds);
    t["exit_fee"] = round4(number(t, "exit_fee") + exit_fee);
    t["note"] = str(t, "note") + " | partial exit " + fmt2(fill) + " @ " + fmt2(px);
    upsert_trade(t);
    log("trade", ticker + ": partial exit " + fmt2(fill) + " of " + fmt2(held) + " @ " + fmt2(px) + " (" + reason + ")");
  }
  return true;
}

bool SystematicTrader::live_manage_resting_exit(json& t, const json& m, double held, long long now, bool killed) {
  const std::string ticker = str(t, "ticker");
  const json& s = state_["settings"];
  json o = store_.query("SELECT status, fill_count, remaining_count FROM orders WHERE order_id=?", {str(t, "exit_order_id")});
  double fill = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
  auto recorded = store_.query("SELECT COALESCE(SUM(count),0) AS n FROM fills WHERE order_id=? AND outcome_side=?",
                              {str(t, "exit_order_id"), str(t, "side") == "yes" ? std::string("no") : std::string("yes")});
  if (!recorded.empty()) fill = std::max(fill, number(recorded[0], "n"));
  bool still = !o.empty() && o[0].value("status", "") == "resting" && o[0].value("remaining_count", 0.0) > 0;
  if (!str(m, "result").empty()) still = false; // finalized markets cannot retain executable orders
  double before = number(t, "filled");
  // Only this exit order's acknowledged fills belong to the strategy. A hand sale or a
  // trade in another app can reduce the account's net position without filling our order.
  long long synced = std::atoll(store_.kv_get("portfolio_sync_ts").value_or("0").c_str());
  bool external_reduction = synced > integer(t, "exit_posted_ts") + 2 && held + fill < before - .009;
  bool expired = now - integer(t, "exit_posted_ts") >= static_cast<long long>(number(s, "exit_ttl_seconds"));
  // While it rests the market can fall away underneath it: twice the stop on the mid pulls the
  // resting sell and hands the position to the taker plan in this same cycle.
  const bool fractional_stop = str(s, "stop_loss_mode") == "fraction";
  const double cost_per = before > 0 && number(t, "entry_cost") > 0 ? number(t, "entry_cost") / before : number(t, "entry_price");
  double sl = fractional_stop ? cost_per * number(s, "stop_loss_fraction") : number(s, "stop_loss");
  double bid_now = side_price(m, str(t, "side"), false), ask_now = side_price(m, str(t, "side"), true);
  const double mid_now = .5 * (bid_now + ask_now);
  const double stop_basis = fractional_stop ? cost_per : number(t, "entry_price");
  const double stop_mark = fractional_stop ? mid_now - fee_model(m).fee(mid_now, 1, true) : mid_now;
  bool disaster = still && sl > 0 && bid_now > 0 && ask_now > bid_now && ask_now < 1 &&
                  fresh_manage(m, "quote_ts", "quote_stale", now) && stop_basis - stop_mark >= 2 * sl - 1e-9;
  if (still && (expired || killed || disaster || external_reduction)) {
    try {
      exec_->cancel(str(t, "exit_order_id"));
      hooks_.request_reconcile();
      still = false;
      json o2 = store_.query("SELECT fill_count FROM orders WHERE order_id=?", {str(t, "exit_order_id")});
      if (!o2.empty()) fill = std::max(fill, o2[0].value("fill_count", 0.0));
      log("info", ticker + ": resting exit pulled after " + std::to_string(now - integer(t, "exit_posted_ts")) + "s" +
                      (fill > 0 ? " with " + fmt2(fill) + " sold" : " unfilled") + (disaster ? "; the mid is twice the stop under entry" : ""));
      if (expired && fill <= 0) t["exit_steps"] = integer(t, "exit_steps") + 1;
    } catch (const std::exception& e) {
      log("warn", ticker + ": could not pull the resting exit: " + e.what());
    }
  }
  if (still) { upsert_trade(t); return true; }
  double px = number(t, "exit_pending_price");
  FeeModel fm = fee_model(m);
  fill = std::min(fill, before);
  if (fill > 0) {
    double exit_fee = fm.fee(px, fill, false);
    double share = before > 0 ? fill / before : 1.0;
    double entry_cost_part = number(t, "entry_cost") * share;
    double actual_offset = 1.0 - px;
    if (actual_fill_cost(store_, str(t, "exit_order_id"), str(t, "side") == "yes" ? "no" : "yes",
                         fill, &actual_offset, &exit_fee)) px = 1.0 - actual_offset;
    double proceeds = fill * px;
    double realized = proceeds - exit_fee - entry_cost_part;
    std::string reason = str(t, "exit_reason").empty() ? "resting exit filled" : str(t, "exit_reason");
    if (fill >= before - 1e-9) {
      t["exit_style"] = ""; t["exit_posted_ts"] = 0; t["exit_pending_price"] = 0.0;
      finalize(t, reason, realized, proceeds, exit_fee, px, now);
      return true;
    }
    t["filled"] = round4(before - fill);
    t["entry_cost"] = round4(number(t, "entry_cost") - entry_cost_part);
    t["realized_pnl"] = round4(number(t, "realized_pnl") + realized);
    t["exit_proceeds"] = round4(number(t, "exit_proceeds") + proceeds);
    t["exit_fee"] = round4(number(t, "exit_fee") + exit_fee);
    t["note"] = str(t, "note") + " | partial resting exit " + fmt2(fill) + " @ " + fmt2(px);
    log("trade", ticker + ": resting exit sold " + fmt2(fill) + " of " + fmt2(before) + " @ " + fmt2(px));
  }
  // Back to open: the next cycle plans again (a tick lower, or into the bid if it got worse).
  t["state"] = "open";
  t["exit_style"] = "";
  t["exit_order_id"] = "";
  t["exit_posted_ts"] = 0;
  t["exit_pending_price"] = 0.0;
  upsert_trade(t);
  return false;
}

void SystematicTrader::live_manage(const json& markets, long long now) {
  std::map<std::string, Position> positions;
  for (const auto& p : store_.positions()) positions[p.ticker] = p;
  std::map<std::string, Order> resting;
  for (const auto& o : store_.orders("resting")) resting[o.order_id] = o;
  bool killed = hooks_.killed();
  {
    for (auto t : store_.query("SELECT * FROM systematic_trades WHERE mode='live' AND state IN ('entering','open','exiting','settling') ORDER BY id")) {
      const std::string st = str(t, "state");
      const std::string ticker = str(t, "ticker"), side = str(t, "side");
      json m = markets.contains(ticker) ? markets[ticker] : json::object();
      auto stored = store_.get_market(ticker);
      if (m.empty() && stored) {
        m = market_to_json(*stored);
        m["fee_type"] = "quadratic"; m["fee_multiplier"] = 1.0;
        if (auto s = store_.get_series(stored->series_ticker)) { m["fee_type"] = s->fee_type; m["fee_multiplier"] = s->fee_multiplier; }
        m["quote_ts"] = stored->quote_ts; m["quote_stale"] = stored->quote_ts <= 0 || now - stored->quote_ts > kMaxSnapshotAge;
        m["quote_stale_manage"] = m["quote_stale"];
        m["fair_stale"] = true; m["fair_ts"] = 0;
      }
      auto pit = positions.find(ticker);
      double held = 0;
      if (pit != positions.end()) {
        bool ours = (side == "yes" && pit->second.position > 0) || (side == "no" && pit->second.position < 0);
        held = ours ? std::fabs(pit->second.position) : 0;
      }
      std::string result = stored ? stored->result : "";
      long long close_ts = stored ? parse_iso(stored->close_time) : 0;
      bool trading_closed = stored && (!stored->tradable() || (close_ts > 0 && close_ts <= now));

      if (st == "entering") {
        // A resting maker entry. Only fills on its recorded order ID establish ownership;
        // the account may also hold manual contracts on this ticker.
        json o = store_.query("SELECT status, fill_count, remaining_count FROM orders WHERE order_id=?", {str(t, "entry_order_id")});
        double fill = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
        auto recorded = store_.query("SELECT COALESCE(SUM(count),0) AS n FROM fills WHERE order_id=? AND outcome_side=?",
                                    {str(t, "entry_order_id"), side});
        if (!recorded.empty()) fill = std::max(fill, number(recorded[0], "n"));
        bool still = !o.empty() && o[0].value("status", "") == "resting" && o[0].value("remaining_count", 0.0) > 0;
        bool expired = now - integer(t, "opened_ts") >= static_cast<long long>(number(state_["settings"], "entry_ttl_seconds"));
        // Why the resting order should come off, if it should: the switch, a close, a partial fill
        // whose tail is better managed as a position, a thesis that no longer holds at this price,
        // or simply its TTL.
        std::string cancel_reason = killed ? "kill switch" : !yes(state_, "running") ? "algorithm paused" :
            yes(state_, "halted") ? "algorithm halted" : trading_closed ? "market trading closed" :
            fill > 0 ? "partial fill: cancel tail and manage the position" :
            systematic_entry_cancel_reason(t, m, state_["settings"], now, cfg_.risk.max_disagreement);
        if (cancel_reason.empty()) {
          // The strategy's mode (config/desk.json, or the Strategies tab) caps its route exactly as
          // the router does: a model parked in manual, research or off keeps nothing resting.
          const std::string model = str(t, "fair_strategy");
          auto mode = cfg_.strategy_modes.find(model);
          if (mode != cfg_.strategy_modes.end() &&
              (mode->second == "off" || mode->second == "research" || mode->second == "manual"))
            cancel_reason = "strategy " + model + " is in " + mode->second + " mode: it is not the algorithm's to send";
        }
        if (cancel_reason.empty() && expired) cancel_reason = "entry TTL expired";
        if (still && !cancel_reason.empty()) {
          try {
            double before_remaining = o.empty() ? 0.0 : number(o[0], "remaining_count");
            double before_filled = o.empty() ? 0.0 : number(o[0], "fill_count");
            json canceled = exec_->cancel(str(t, "entry_order_id"));
            hooks_.request_reconcile();
            still = false;
            // A fill that raced the cancel shows up as less reduced than was resting.
            if (canceled.is_object() && canceled.contains("reduced_by")) {
              double reduced = canceled["reduced_by"].is_string() ? parse_num(canceled["reduced_by"].get<std::string>())
                                                                  : number(canceled, "reduced_by", -1);
              if (reduced >= 0 && reduced <= before_remaining) fill = std::max(fill, before_filled + before_remaining - reduced);
            }
            json o2 = store_.query("SELECT fill_count FROM orders WHERE order_id=?", {str(t, "entry_order_id")});
            if (!o2.empty()) fill = std::max(fill, o2[0].value("fill_count", 0.0));
            log("info", ticker + ": resting entry pulled after " + std::to_string(now - integer(t, "opened_ts")) + "s" +
                            (fill > 0 ? " with " + fmt2(fill) + " filled" : " unfilled") + " (" + cancel_reason + ")");
          } catch (const std::exception& e) {
            log("warn", ticker + ": could not pull the resting entry: " + e.what());
          }
        }
        if (still) { upsert_trade(t); continue; }
        // The budget reserved the whole order when it was posted; settle it to what really filled.
        FeeModel fm = fee_model(m);
        double reserved = number(t, "count") * (number(t, "entry_price") + fm.fee(number(t, "entry_price"), 1, str(t, "entry_style") == "taker"));
        if (fill > 0) {
          unfilled_streak_.erase(ticker);
          improve_ticks_.erase(ticker);
          t["state"] = "open";
          t["filled"] = fill;
          t["count"] = fill;
          t["filled_ts"] = now;
          t["entry_fee"] = round4(fm.fee(number(t, "entry_price"), fill, str(t, "entry_style") == "taker"));
          // The recorded fills, when they cover the whole quantity, are the true price, fee and time.
          double actual_price = number(t, "entry_price"), actual_fee = number(t, "entry_fee");
          if (actual_fill_cost(store_, str(t, "entry_order_id"), side, fill, &actual_price, &actual_fee)) {
            t["entry_price"] = actual_price; t["entry_fee"] = actual_fee;
            auto timestamps = store_.query("SELECT MIN(ts) AS first_fill FROM fills WHERE order_id=?", {str(t, "entry_order_id")});
            if (!timestamps.empty() && integer(timestamps[0], "first_fill") > 0 && integer(timestamps[0], "first_fill") <= now)
              t["filled_ts"] = integer(timestamps[0], "first_fill");
          }
          t["entry_cost"] = round4(fill * number(t, "entry_price") + number(t, "entry_fee"));
          if (!cancel_reason.empty()) t["note"] = str(t, "note") + " | entry tail canceled: " + cancel_reason;
          add_spent(number(t, "entry_cost") - reserved);
          log("trade", ticker + " " + side + ": maker entry filled " + fmt2(fill) + " @ " + fmt2(number(t, "entry_price")));
        } else {
          t["state"] = "canceled";
          t["closed_ts"] = now;
          t["exit_reason"] = cancel_reason.empty() ? "entry canceled externally unfilled" : "entry canceled: " + cancel_reason;
          add_spent(-reserved);
          // A bid nobody hits is not liquidity provision, it is a queue the market walks past.
          // Post again after a breath; after three in a row, leave the market alone for a while.
          int streak = ++unfilled_streak_[ticker];
          int ladder = static_cast<int>(integer(state_["settings"], "ladder_ticks"));
          if (streak > ladder) {
            refused_until_[ticker] = now + 900;
            unfilled_streak_.erase(ticker);
            improve_ticks_.erase(ticker);
            log("info", ticker + ": " + std::to_string(streak) + " posts in a row went unfilled, the last at the top of the ladder; leaving it for 15 min");
          } else {
            improve_ticks_[ticker] = streak;
            refused_until_[ticker] = now + 15;
          }
        }
        upsert_trade(t);
        if (t["state"] != "open") continue;
      }

      // A resting maker exit: book what sold, pull it after its TTL, and re-plan from "open".
      if (st == "exiting" && str(t, "exit_style") == "maker" && !str(t, "exit_order_id").empty()) {
        if (!result.empty()) m["result"] = result;
        if (live_manage_resting_exit(t, m, held, now, killed)) {
          continue;
        } else if (str(t, "state") == "closed") {
          continue;
        }
      }
      // open / exiting / settling: the row's order ledger determines the strategy's quantity.
      double filled = number(t, "filled");
      if (!result.empty()) {
        // Settled. Payout is $1 a contract if our side won, nothing otherwise.
        bool won = (result == "yes") == (side == "yes");
        double payout = won ? filled : 0.0;
        finalize(t, std::string("settled ") + (won ? "in our favour" : "against us"), payout - number(t, "entry_cost"),
                 payout, 0.0, won ? 1.0 : 0.0, now);
        continue;
      }
      long long synced = std::atoll(store_.kv_get("portfolio_sync_ts").value_or("0").c_str());
      long long acknowledged_ts = std::max(integer(t, "filled_ts"), integer(t, "exit_attempt_ts"));
      auto entry_order = store_.query("SELECT updated_ts FROM orders WHERE order_id=?", {str(t, "entry_order_id")});
      if (!entry_order.empty()) acknowledged_ts = std::max(acknowledged_ts, integer(entry_order[0], "updated_ts"));
      // Refresh after a cancel/reconcile; never enlarge ownership to match account holdings.
      held = 0;
      for (const auto& current : store_.positions())
        if (current.ticker == ticker && ((side == "yes" && current.position > 0) || (side == "no" && current.position < 0)))
          held = std::fabs(current.position);
      if (st != "entering" && held < filled - .009 && synced > acknowledged_ts + 2) {
        // An external trade changed the net position. Remove the missing quantity from our
        // mandate without importing manual fills/prices into the algorithm's P&L.
        double per = filled > 0 ? number(t, "entry_cost") / filled : number(t, "entry_price");
        const double removed = filled - held;
        const auto transfer_key="systematic_transferred:"+str(t,"mode");
        const double prior_transfer=std::atof(store_.kv_get(transfer_key).value_or("0").c_str());
        store_.kv_set(transfer_key,std::to_string(prior_transfer+per*removed));
        t["note"] = str(t, "note") + " | external reduction: " + fmt2(removed) + " contracts, $" +
                    fmt2(per * removed) + " cost removed from strategy accounting; external P&L unattributed";
        log("warn", ticker + ": external position reduction; strategy ownership capped at " + fmt2(held) + ", manual fills left outside strategy P&L");
        t["entry_cost"] = round4(per * held);
        t["filled"] = held;
        filled = held;
        if (filled <= .009) {
          t["state"] = "released"; t["closed_ts"] = now; t["unrealized"] = 0.0;
          t["exit_reason"] = "external position reduction; manual P&L not attributed";
          upsert_trade(t);
          continue;
        }
      }
      double bid = side_price(m, side, false), bid_size = side_depth(m, side, false);
      bool quote_fresh = fresh_manage(m, "quote_ts", "quote_stale", now) && bid > 0 && bid < 1;
      FeeModel fm = fee_model(m);
      if (quote_fresh) {
        t["mark_price"] = bid;
        t["mark_ts"] = integer(m, "quote_ts");
        t["unrealized"] = round4(filled * bid - fm.fee(bid, filled, true) - number(t, "entry_cost"));
      }
      if (trading_closed) {
        if (t["state"] != "settling") { t["state"] = "settling"; log("info", ticker + ": trading closed, waiting for settlement"); }
        upsert_trade(t);
        continue;
      }
      if (t["state"] == "settling") t["state"] = "open";
      // Paired arbitrage legs form a basket. Independently taking profit or stopping one
      // leg can turn a covered payout into a directional position.
      if (str(t, "fair_strategy") == "ladder_arb") { upsert_trade(t); continue; }
      if (killed) { upsert_trade(t); continue; }
      // Manual brackets manage only manual lots. Our own quantity keeps its own exit rules;
      // live_send_exit still defers while an unrelated order is working on the same ticker.
      if (!quote_fresh) { upsert_trade(t); continue; }
      bool stale = false;
      double ask = side_price(m, side, true);
      ExitPlan plan = plan_exit(t, m, bid, bid_size, ask, now, &stale);
      if (plan.reason.empty() || now - integer(t, "exit_attempt_ts") < 10) { upsert_trade(t); continue; }
      t["state"] = "exiting";
      t["exit_reason"] = plan.reason;
      if (!live_send_exit(t, plan.reason, bid, bid_size, now, plan.style, plan.price)) {
        if (t["state"] == "exiting") { t["state"] = "open"; t["exit_reason"] = ""; upsert_trade(t); }
      }
    }
  }
}

bool SystematicTrader::live_send_entry(const json& row, double count, long long now, std::string* why,
                                       RejectClass* refused) {
  const std::string ticker = str(row, "ticker");
  auto m = store_.get_market(ticker);
  if (!m) { if (why) *why = "market not in the store"; return false; }
  Proposal p;
  p.ticker = ticker;
  p.event_ticker = str(row, "event_ticker");
  p.series_ticker = str(row, "series_ticker");
  p.strategy = "systematic";
  p.mode = "bot";
  p.outcome_side = str(row, "side");
  p.exec_style = str(row, "style");
  p.status = "pending";
  p.price = number(row, "price");
  p.count = count;
  p.p_model = number(row, "p_yes");
  p.sigma = number(row, "sigma");
  p.fee_per = number(row, "fee");
  p.net_edge = number(row, "edge");
  p.yes_bid = number(row, "yes_bid", m->yes_bid);
  p.yes_ask = number(row, "yes_ask", m->yes_ask);
  p.cluster = risk_cluster(store_, *m);
  p.note = "systematic: p(win) " + fmt2(number(row, "p_win")) + " edge " + fmt2(number(row, "edge")) + " kelly " +
           fmt2(number(row, "kelly")) + " via " + str(row, "fair_strategy") + (yes(row, "research") ? " (research estimate)" : "");
  // The router's decision this entry acts on. Its snapshot is recorded as the automatic lane takes
  // it, at the lane's price, style and size; the executor copies it as "submitted" when the order
  // goes, so the order leads back to the same probability and edge the board showed.
  p.decision_id = str(row, "decision_id");
  store_.insert_proposal(p);
  auto refused_snapshot = [&](const std::string& reason) {
    // The executor marks an order whose delivery is not known (a 5xx, a timeout) "unknown": that
    // is not a refusal, and the uncertain-order block holds everything until reconcile says.
    const bool unknown = p.status == "unknown";
    if (!p.decision_id.empty())
      record_decision(store_, row, unknown ? "unknown" : "rejected", now, p.id,
                      std::string(unknown ? "automatic lane, delivery unknown: " : "automatic lane, not sent: ") +
                          reason.substr(0, 300), count, p.price, p.exec_style);
  };
  if (!p.decision_id.empty())
    record_decision(store_, row, "auto", now, p.id, "automatic lane (live)", count, p.price, p.exec_style);
  try {
    exec_->submit(p);
  } catch (const ApiError& e) {
    // The desk's rule, narrowed to what it was for (reject.h): a refusal that says the DESK is
    // wrong - a malformed order, a signing failure, a 4xx nobody has classified - stops the
    // algorithm and asks for the kill switch. A refusal that says the MARKET moved (post-only
    // would cross, market closed, shard short) or the exchange is busy does not: the order was not
    // placed, and the caller re-reads the book before anything else is sent.
    const RejectClass cls = classify_rejection(e.status, e.body);
    std::string w = "exchange refused (" + std::to_string(e.status) + ", " + reject_class_name(cls) + "): " + e.body.substr(0, 120);
    if (why) *why = w;
    if (refused) *refused = cls;
    refused_snapshot(w);
    if (reject_trips_kill(cls)) {
      state_["running"] = false;
      state_["halted"] = true;
      state_["halt_reason"] = "order rejected by the exchange: " + e.body.substr(0, 120);
      state_["halt_class"] = reject_class_name(cls);   // kept, so a restart never re-guesses it
      log("error", ticker + ": " + w + " - algorithm stopped and the kill switch requested");
      hooks_.request_kill("systematic entry rejected: " + e.body.substr(0, 120));
    } else {
      if (cls == RejectClass::Retryable || cls == RejectClass::SoftCollateral)
        refused_until_[ticker] = std::max(refused_until_[ticker], now + 60);
      log("warn", ticker + ": " + w + " - not an error of the desk; nothing was placed, the algorithm keeps running");
    }
    return false;
  } catch (const SoftRejection& e) {
    // Refused before sending: the book had already moved through this price (a post-only bid at
    // or over the offer). Nothing went out; the caller re-reads the book and re-prices.
    if (why) *why = e.what();
    if (refused) *refused = e.cls;
    refused_snapshot(e.what());
    p.status = "failed";
    p.note += " | not sent: " + std::string(e.what());
    store_.update_proposal(p);
    log("info", ticker + " " + p.outcome_side + " x" + fmt2(count) + " @ " + fmt2(p.price) + " not sent: " + e.what());
    return false;
  } catch (const std::exception& e) {
    // Pre-flight refusal (a cap, stale account data, a working order): not an exchange rejection.
    if (why) *why = e.what();
    refused_snapshot(e.what());
    p.status = "failed";
    p.note += " | not sent: " + std::string(e.what());
    store_.update_proposal(p);
    refused_until_[ticker] = now + kRefusalBackoff;
    log("warn", ticker + " " + p.outcome_side + " x" + fmt2(count) + " @ " + fmt2(p.price) + " not sent: " + e.what());
    return false;
  }
  hooks_.request_reconcile();
  json o = store_.query("SELECT fill_count, remaining_count, status FROM orders WHERE order_id=?", {p.order_id});
  double fill = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
  double remaining = o.empty() ? 0.0 : o[0].value("remaining_count", 0.0);
  FeeModel fm = fee_model(row);
  double cost_est = count * (p.price + p.fee_per);
  json t = {{"ticker", ticker}, {"event_ticker", p.event_ticker}, {"series_ticker", p.series_ticker},
            {"title", str(row, "title")}, {"side", p.outcome_side}, {"mode", "live"}, {"opened_ts", now},
            {"day", str(state_, "day")}, {"entry_proposal_id", p.id}, {"entry_order_id", p.order_id},
            {"entry_style", p.exec_style}, {"entry_price", p.price}, {"count", count}, {"filled", 0.0},
            {"entry_cost", 0.0}, {"entry_fee", 0.0}, {"realized_pnl", 0.0}, {"exit_proceeds", 0.0}, {"exit_fee", 0.0},
            {"p_win", number(row, "p_win")}, {"edge", number(row, "edge")}, {"kelly", number(row, "kelly")},
            {"settings_version", integer(state_, "settings_version")}, {"note", p.note},
            {"mark_price", number(row, "bid")}, {"mark_ts", now}, {"unrealized", 0.0}, {"exit_attempt_ts", 0},
            {"fair_strategy", str(row, "fair_strategy")}, {"exit_reason", ""}, {"exit_order_id", ""},
            {"exit_proposal_id", 0}, {"exit_price", 0.0}, {"closed_ts", 0}, {"filled_ts", 0}};
  if (remaining > 0) {
    // Resting (a maker order, possibly with a partial fill already). The whole order is reserved
    // against the daily budget; the entering branch settles it to what really fills.
    t["state"] = "entering";
    add_spent(cost_est);
    log("trade", ticker + " " + p.outcome_side + ": resting " + fmt2(count) + " @ " + fmt2(p.price) + " (maker, p(win) " +
                     fmt2(number(row, "p_win")) + ", edge " + fmt2(number(row, "edge")) + ")" +
                     (fill > 0 ? ", " + fmt2(fill) + " filled at once" : ""));
  } else if (fill > 0) {
    t["state"] = "open";
    t["filled"] = fill;
    t["count"] = fill;
    t["filled_ts"] = now;
    t["entry_fee"] = round4(fm.fee(p.price, fill, p.exec_style == "taker"));
    double actual_price = p.price, actual_fee = number(t, "entry_fee");
    if (actual_fill_cost(store_, p.order_id, p.outcome_side, fill, &actual_price, &actual_fee)) {
      t["entry_price"] = actual_price; t["entry_fee"] = actual_fee;
    }
    t["entry_cost"] = round4(fill * number(t, "entry_price") + number(t, "entry_fee"));
    t["unrealized"] = round4(fill * number(row, "bid") - fm.fee(number(row, "bid"), fill, true) - number(t, "entry_cost"));
    add_spent(number(t, "entry_cost"));
    log("trade", ticker + " " + p.outcome_side + ": bought " + fmt2(fill) + " @ " + fmt2(p.price) + " (" + p.exec_style +
                     ", p(win) " + fmt2(number(row, "p_win")) + ", edge " + fmt2(number(row, "edge")) + ")");
  } else {
    t["state"] = "canceled";
    t["closed_ts"] = now;
    t["exit_reason"] = "taker order did not fill";
    log("info", ticker + " " + p.outcome_side + ": IOC at " + fmt2(p.price) + " did not fill");
    refused_until_[ticker] = now + 60;
  }
  upsert_trade(t);
  return t["state"] != "canceled";
}

void SystematicTrader::live_enter(const json& board, const json& screen, long long now) {
  const json& s = state_["settings"];
  std::set<std::string> held;
  double open_cost = 0;
  int open_n = 0;
  for (const auto& t : store_.query("SELECT event_ticker, entry_cost, count, entry_price, state FROM systematic_trades "
                                     "WHERE mode='live' AND state IN ('entering','open','exiting','settling')")) {
    ++open_n;
    open_cost += t.value("state", "") == "entering" ? t.value("count", 0.0) * t.value("entry_price", 0.0) : t.value("entry_cost", 0.0);
  }
  held = blocked_events("live");
  std::map<std::string, long long> cooldowns;
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    cooldowns[it.key()] = it.value().get<long long>();
  // The entries come from the router's AUTO decisions for this cycle (verdict "algo"); the trader
  // only decides how to execute them. Ownership and cooldowns are re-read here because managing
  // positions earlier in this step can change them.
  int sent = 0;
  double budget_left = std::min(number(s, "daily_budget_dollars") - spent_today(),
                               number(systematic_capital(store_,s,"live"),"available"));
  double bankroll = std::min(cfg_.bankroll_dollars,number(s,"capital_budget_dollars"));
  if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
  // One sizing rule for a first send and for a re-priced one.
  auto dollars_for = [&](const json& r, double cost_per) {
    double dollars = number(s, str(r,"category")=="sports"?"sports_per_trade_dollars":"per_trade_dollars");
    if (number(s, "kelly_fraction") > 0)
      dollars = std::min(dollars, std::max(cost_per, number(s, "kelly_fraction") * number(r, "kelly") * bankroll));
    return std::min({dollars, budget_left, number(s, "max_open_dollars") - open_cost});
  };
  auto count_for = [&](const json& r, double cost_per) {
    double count = std::floor(dollars_for(r, cost_per) / cost_per);
    if (str(r, "style") == "taker") count = std::min(count, std::floor(number(r, "ask_size")));
    return count;
  };
  std::map<std::string, const json*> market_rows;
  if (board.contains("markets") && board["markets"].is_array())
    for (const auto& m : board["markets"])
      if (m.is_object()) market_rows[str(m, "ticker")] = &m;
  // Soft refusals (the market moved) are not the desk's fault, but they are not free either: each
  // is an order write. At most one re-priced retry a step, and two soft refusals end the step.
  int soft_refusals = 0;
  bool retried = false;
  auto back_off = [&](const std::string& ticker, long long until) {
    long long& u = refused_until_[ticker];
    u = std::max(u, until);   // never shorten a longer backoff set by the send itself
  };
  for (const auto& row : screen["rows"]) {
    if (soft_refusals >= 2) break;
    if (str(row, "verdict") != "algo") break;   // sorted: every algo row comes first
    if (sent >= integer(s, "max_entries_per_cycle")) break;
    if (open_n >= integer(s, "max_positions")) { break; }
    const std::string ticker = str(row, "ticker"), event = str(row, "event_ticker");
    if (held.count(event)) continue;
    if (auto cd = cooldowns.find(event); cd != cooldowns.end() && cd->second > now) continue;
    auto ru = refused_until_.find(ticker);
    if (ru != refused_until_.end() && ru->second > now) continue;
    double cost_per = number(row, "price") + number(row, "fee");
    if (cost_per <= 0) continue;
    double dollars = dollars_for(row, cost_per);
    double count = count_for(row, cost_per);
    if (count < 1) {
      if (budget_left < cost_per) { log("info", "daily budget used up ($" + fmt2(spent_today()) + " of $" + fmt2(number(s, "daily_budget_dollars")) + ")"); break; }
      continue;
    }
    // The ladder: a maker post that went unfilled comes back one tick better each time, up to
    // ladder_ticks, as long as the improved price still clears the edge floor; reaching the offer
    // turns it into a taker when that clears too. A bid the market walks past for nine minutes
    // is not liquidity provision, it is a queue.
    json send_row = row;
    int improve = 0;
    if (str(row, "style") == "maker") {
      auto it = improve_ticks_.find(ticker);
      if (it != improve_ticks_.end()) improve = std::min<long long>(it->second, integer(s, "ladder_ticks"));
    }
    if (improve > 0) {
      // Every step is priced by the one edge engine, on the decision's own quote and book: the fee,
      // the slippage estimate (a step that reaches the offer is a taker) and the uncertainty
      // deduction are the router's, never a formula of the ladder's own.
      const double tick = number(row, "tick", .01), ask = number(row, "ask");
      EdgeCandidate best;
      bool improved = false;
      for (int k = 1; k <= improve; ++k) {
        double px = floor_to_tick(number(row, "maker_price") + k * tick, tick);
        const bool taker = ask > 0 && px >= ask - 1e-9;
        if (taker) px = ask;
        const EdgeCandidate c = decision_edge_at(row, taker ? "taker" : "maker", px, s);
        if (!c.executable || c.executable_edge + 1e-9 < number(s, "min_edge")) break;   // the ladder stops at the edge floor
        best = c;
        improved = true;
        if (taker) break;
      }
      if (improved && best.entry_price > number(row, "price") + 1e-12) {
        const bool taker = best.style == "taker";
        const double fee = best.entry_fee;
        send_row["price"] = round4(best.entry_price);
        send_row["style"] = best.style;
        send_row["fee"] = round4(fee);
        send_row["edge"] = round4(best.net_edge);
        send_row["raw_edge"] = round4(best.raw_edge);
        send_row["slippage"] = round4(best.estimated_slippage);
        send_row["executable_edge"] = round4(best.executable_edge);
        send_row["kelly"] = round4(best.kelly);
        const double best_price = best.entry_price;
        cost_per = best_price + fee;
        count = std::floor(std::min({dollars, budget_left, number(s, "max_open_dollars") - open_cost}) / cost_per);
        if (taker) count = std::min(count, std::floor(number(row, "ask_size")));
        if (count < 1) continue;
        log("info", ticker + ": ladder step " + std::to_string(improve) + ", " + (taker ? "taking the offer at " : "posting at ") + fmt2(best_price) +
                        " (was " + fmt2(number(row, "price")) + ")");
      }
    }
    std::string why;
    RejectClass refused = RejectClass::Unknown;
    bool ok = live_send_entry(send_row, count, now, &why, &refused);
    if (!ok && reject_is_soft(refused)) ++soft_refusals;
    // Only a post-only cross is re-priced: a closed or paused market, or a short shard, would
    // refuse the same order again, and the fresh book carries no market status to tell.
    if (!ok && !retried && refused == RejectClass::SoftMarketState && is_post_only_cross(why) &&
        yes(state_, "running") && !yes(state_, "halted") && !hooks_.killed()) {
      retried = true;
      // The book moved between the screen and the send: a post-only price that now crosses (the
      // exchange refused it, or the executor saw it before sending). That is the market, not a
      // fault: re-read this one book, run the same screen on it, and send once more only if the
      // row is still the algorithm's - posted at the new bid, or lifting the new offer if that
      // still clears the edge floor.
      json fresh = hooks_.fresh_quote ? hooks_.fresh_quote(ticker) : json();
      auto mit = market_rows.find(ticker);
      // The re-screen and the second send run on the clock as it is now, not the step's: the first
      // attempt took time, and the model estimate must still be inside the limit when it goes.
      const long long t2 = std::max(now, now_s());
      if (fresh.is_object() && !fresh.empty() && mit != market_rows.end()) {
        json m = *mit->second;
        for (const auto& [k, v] : fresh.items()) m[k] = v;
        const long long read_ts = integer(fresh, "quote_ts");
        m["quote_ts"] = read_ts > 0 ? std::min(read_ts, t2) : t2;
        m["quote_stale"] = false;
        json one = {{"markets", json::array({m})}};
        if (board.contains("market_data_max_age_seconds")) one["market_data_max_age_seconds"] = board["market_data_max_age_seconds"];
        json rescreen = systematic_screen(one, s, t2, held, cooldowns, cfg_.risk.max_disagreement);
        const json* again = nullptr;
        for (const auto& r : rescreen["rows"])
          if (str(r, "side") == str(row, "side")) again = &r;
        if (again && str(*again, "verdict") == "algo") {
          double cost2 = number(*again, "price") + number(*again, "fee");
          double count2 = cost2 > 0 ? count_for(*again, cost2) : 0;
          if (count2 >= 1) {
            log("info", ticker + ": the book moved (" + why + "); re-priced from a fresh read: " +
                            (str(*again, "style") == "taker" ? "taking the offer at " : "posting at ") + fmt2(number(*again, "price")));
            ok = live_send_entry(*again, count2, t2, &why, &refused);
            if (ok) { count = count2; cost_per = cost2; }
            else if (reject_is_soft(refused)) ++soft_refusals;
          }
        } else {
          log("info", ticker + ": the book moved; after a fresh read the row is " + (again ? str(*again, "verdict") : std::string("gone")) +
                          ", not sent" + (again ? " (" + (*again)["reasons"].dump().substr(0, 160) + ")" : ""));
        }
      }
    }
    if (!ok && reject_is_soft(refused)) back_off(ticker, now + 30);   // a breather; re-screened from fresh data after it
    if (ok) {
      ++sent;
      ++open_n;
      held.insert(event);
      open_cost += count * cost_per;
      budget_left -= count * cost_per;
    } else if (!yes(state_, "running")) {
      break;   // stopped itself on a rejection
    }
  }
}

// Locked baskets found by the ladder/dominance finders wait in Signals as ladder_arb legs. While
// the algorithm runs with arbitrage on, it sends them itself, every leg, without waiting for the
// AUTO switch: a locked basket is the one thing on this desk that needs no estimate.
void SystematicTrader::live_arbitrage(long long now) {
  const json& s = state_["settings"];
  // manual, research or off: the finders still list them, nothing is sent. The category is the
  // outer permission and ladder_arb's own model route the inner one, as for every other model.
  if (effective_route(route_of(s, "arbitrage"), model_route(s, "ladder_arb", "")) != RouteMode::Auto) return;
  std::map<std::string, std::vector<Proposal>> baskets;
  for (const auto& p : store_.proposals("pending", 200)) {
    if (p.strategy != "ladder_arb") continue;
    auto b = p.note.find("basket ");
    std::string key = b == std::string::npos ? p.event_ticker : p.note.substr(b, p.note.find('|', b) - b);
    baskets[key].push_back(p);
  }
  const json capital=systematic_capital(store_,s,"live");
  double budget_left=std::min({number(s,"daily_budget_dollars")-spent_today(),number(capital,"available"),
                             number(s,"max_open_dollars")-number(capital,"committed")});
  for (auto& [key, legs] : baskets) {
    double cost = 0, edge = 0;
    for (const auto& l : legs) { cost += l.count * (l.price + l.fee_per); edge += l.count * l.net_edge; }
    if (cost > budget_left + 1e-9) { log("info", "basket " + key + " needs $" + fmt2(cost) + ", over the remaining daily budget"); continue; }
    int ok = 0;
    double committed = 0;
    for (auto l : legs) {
      try {
        exec_->submit(l);
        const auto orders=store_.query("SELECT fill_count,remaining_count FROM orders WHERE order_id=?",{l.order_id});
        const double fill=orders.empty()?0:number(orders[0],"fill_count");
        const double remaining=orders.empty()?0:number(orders[0],"remaining_count");
        const double cost=fill*(l.price+l.fee_per);
        json t={{"ticker",l.ticker},{"event_ticker",l.event_ticker},{"series_ticker",l.series_ticker},
          {"title",key},{"side",l.outcome_side},{"mode","live"},{"state",remaining>0?"entering":fill>0?"open":"canceled"},
          {"opened_ts",now},{"filled_ts",fill>0?now:0},{"closed_ts",fill<=0&&remaining<=0?now:0},
          {"entry_order_id",l.order_id},{"entry_proposal_id",l.id},{"entry_style",l.exec_style},
          {"entry_price",l.price},{"count",remaining>0?l.count:fill},{"filled",fill},
          {"entry_fee",fill*l.fee_per},{"entry_cost",cost},{"fair_strategy","ladder_arb"},
          {"day",str(state_,"day")},{"note","Basket leg: held to settlement; combined payout requires all legs to fill"}};
        upsert_trade(t);
        committed+=(fill+remaining)*(l.price+l.fee_per);
        if(fill>0||remaining>0)++ok;
      }
      catch (const std::exception& e) { log("warn", "basket leg " + l.ticker + " " + l.outcome_side + " not sent: " + e.what()); }
    }
    if (ok > 0) { add_spent(committed); budget_left-=committed; hooks_.request_reconcile(); }
    log(ok == static_cast<int>(legs.size()) ? "trade" : "warn",
        "locked basket " + key + ": " + std::to_string(ok) + "/" + std::to_string(legs.size()) + " legs sent, cash $" +
            fmt2(cost) + ", locked $" + fmt2(edge) + (ok == static_cast<int>(legs.size()) ? "" : " - PARTIAL: a directional position may be left"));
  }
  (void)now;
}

// ---- paper -------------------------------------------------------------------------------------

void SystematicTrader::paper_manage(const json& markets, long long now) {
  const json& s = state_["settings"];
  double slip = number(s, "paper_slippage");
  for (auto t : store_.query("SELECT * FROM systematic_trades WHERE mode='paper' AND state IN ('open','settling') ORDER BY id")) {
    const std::string ticker = str(t, "ticker"), side = str(t, "side");
    json m = markets.contains(ticker) ? markets[ticker] : json::object();
    auto stored = store_.get_market(ticker);
    double filled = number(t, "filled");
    if (stored && !stored->result.empty()) {
      bool won = (stored->result == "yes") == (side == "yes");
      double payout = won ? filled : 0.0;
      finalize(t, std::string("settled ") + (won ? "in our favour" : "against us"), payout - number(t, "entry_cost"), payout, 0,
               won ? 1.0 : 0.0, now);
      continue;
    }
    long long close_ts = stored ? parse_iso(stored->close_time) : 0;
    bool trading_closed = stored && (!stored->tradable() || (close_ts > 0 && close_ts <= now));
    double bid = side_price(m, side, false), bid_size = side_depth(m, side, false);
    bool quote_fresh = fresh_manage(m, "quote_ts", "quote_stale", now) && bid > 0 && bid < 1;
    FeeModel fm = fee_model(m);
    double exit_px = std::max(0.01, bid - slip);
    if (quote_fresh) {
      t["mark_price"] = exit_px;
      t["mark_ts"] = integer(m, "quote_ts");
      t["unrealized"] = round4(filled * exit_px - fm.fee(exit_px, filled, true) - number(t, "entry_cost"));
    }
    if (trading_closed) { t["state"] = "settling"; upsert_trade(t); continue; }
    t["state"] = "open";
    if (!quote_fresh) { upsert_trade(t); continue; }
    bool stale = false;
    ExitPlan plan = plan_exit(t, m, bid, bid_size, side_price(m, side, true), now, &stale);
    // Paper fills every exit as a taker into the bid: a resting sell cannot be simulated honestly
    // without queue data, and the pessimistic price keeps the paper book conservative.
    if (plan.reason.empty() || bid_size < filled) { upsert_trade(t); continue; }
    double exit_fee = fm.fee(exit_px, filled, true), proceeds = filled * exit_px;
    finalize(t, plan.reason + " (paper)", proceeds - exit_fee - number(t, "entry_cost"), proceeds, exit_fee, exit_px, now);
  }
}

void SystematicTrader::paper_enter(const json& screen, long long now) {
  const json& s = state_["settings"];
  std::set<std::string> held;
  double open_cost = 0;
  int open_n = 0;
  for (const auto& t : store_.query("SELECT event_ticker, entry_cost FROM systematic_trades WHERE mode='paper' AND state IN ('open','settling')")) {
    ++open_n;
    open_cost += t.value("entry_cost", 0.0);
  }
  held = blocked_events("paper");
  std::map<std::string, long long> cooldowns;
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    cooldowns[it.key()] = it.value().get<long long>();
  int sent = 0;
  double budget_left = std::min(number(s,"daily_budget_dollars")-spent_today(),
                               number(systematic_capital(store_,s,"paper"),"available"));
  double slip = number(s, "paper_slippage");
  for (const auto& row : screen["rows"]) {
    if (str(row, "verdict") != "algo") break;
    if (sent >= integer(s, "max_entries_per_cycle") || open_n >= integer(s, "max_positions")) break;
    const std::string ticker = str(row, "ticker"), event = str(row, "event_ticker");
    if (held.count(event)) continue;
    if (auto cd = cooldowns.find(event); cd != cooldowns.end() && cd->second > now) continue;
    // Paper fills are only believable as a taker against displayed size, with slippage.
    double price = std::min(0.99, number(row, "ask") + slip);
    FeeModel fm = fee_model(row);
    double cost_per = price + fm.fee(price, 1, true);
    double dollars = std::min({number(s,str(row,"category")=="sports"?"sports_per_trade_dollars":"per_trade_dollars"), budget_left, number(s, "max_open_dollars") - open_cost});
    double count = std::floor(std::min(dollars / cost_per, number(row, "ask_size")));
    if (count < 1) continue;
    double fee = fm.fee(price, count, true), cost = count * price + fee;
    json t = {{"ticker", ticker}, {"event_ticker", event}, {"series_ticker", str(row, "series_ticker")},
              {"title", str(row, "title")}, {"side", str(row, "side")}, {"state", "open"}, {"mode", "paper"},
              {"opened_ts", now}, {"filled_ts", now}, {"day", str(state_, "day")}, {"entry_style", "taker"},
              {"entry_price", price}, {"count", count}, {"filled", count}, {"entry_cost", round4(cost)}, {"entry_fee", fee},
              {"realized_pnl", 0.0}, {"exit_proceeds", 0.0}, {"exit_fee", 0.0}, {"p_win", number(row, "p_win")},
              {"edge", number(row, "edge")}, {"kelly", number(row, "kelly")}, {"settings_version", integer(state_, "settings_version")},
              {"note", "paper: " + str(row, "fair_strategy")}, {"mark_price", number(row, "bid")}, {"mark_ts", now},
              {"unrealized", round4(count * (number(row, "bid") - slip) - fm.fee(number(row, "bid") - slip, count, true) - cost)},
              {"exit_attempt_ts", 0}, {"fair_strategy", str(row, "fair_strategy")}, {"exit_reason", ""}, {"exit_order_id", ""},
              {"exit_proposal_id", 0}, {"exit_price", 0.0}, {"closed_ts", 0}, {"entry_proposal_id", 0}, {"entry_order_id", ""}};
    upsert_trade(t);
    // The decision the paper lane acted on, at the price it paper-filled (taker, with slippage).
    if (!str(row, "decision_id").empty())
      record_decision(store_, row, "auto", now, 0, "automatic lane (paper fill)", count, price, "taker");
    add_spent(cost);
    ++sent; ++open_n; held.insert(event); open_cost += cost; budget_left -= cost;
    log("trade", "paper: " + ticker + " " + str(row, "side") + " x" + fmt2(count) + " @ " + fmt2(price) + " (p(win) " +
                     fmt2(number(row, "p_win")) + ", edge " + fmt2(number(row, "edge")) + ")");
  }
}

// ---- cycle -------------------------------------------------------------------------------------

void SystematicTrader::step(const json& board, long long now) {
  json screen;
  {
    std::lock_guard<std::mutex> lk(mu_);
    screen = screen_unlocked(board, now);
  }
  step(board, screen, now);
}

void SystematicTrader::step(const json& board, const json& screen, long long now) {
  std::lock_guard<std::mutex> lk(mu_);
  if (now <= integer(state_, "last_step_ts")) return;
  state_["last_step_ts"] = now;
  roll_day(now);
  json markets = json::object();
  if (board.contains("markets") && board["markets"].is_array())
    for (const auto& m : board["markets"])
      if (m.is_object() && !str(m, "ticker").empty()) markets[str(m, "ticker")] = m;
  const std::string mode = str(state_, "mode");
  try {
    if (mode == "live" && exec_) live_manage(markets, now); else if (mode == "paper") paper_manage(markets, now);
  } catch (const std::exception& e) {
    log("error", std::string("managing positions: ") + e.what());
  }
  // The loss stop reads realized today plus the mark on what is open, for the active mode.
  double today = pnl_today(now);
  double stop = yes(state_["settings"],"daily_loss_stop_enabled") ? number(state_["settings"], "daily_loss_stop_dollars") : 0;
  double base = number(state_, "loss_stop_base");
  if (yes(state_, "running") && !yes(state_, "halted") && stop > 0 && today - base <= -stop) {
    state_["halted"] = true;
    state_["halt_class"] = "";
    state_["halt_reason"] = "daily loss stop: today " + fmt2(today) + (base < 0 ? " (from " + fmt2(base) + ")" : "") +
                            " <= -" + fmt2(stop) + " (no new entries until the day rolls or you clear it; exits keep running)";
    log("warn", state_["halt_reason"].get<std::string>());
  }
  if (yes(state_, "running") && !yes(state_, "halted") && !hooks_.killed()) {
    try {
      if (mode == "live" && exec_ && exec_->armed()) { live_arbitrage(now); live_enter(board, screen, now); }
      else if (mode == "paper") paper_enter(screen, now);
    } catch (const std::exception& e) {
      log("error", std::string("entering: ") + e.what());
    }
  }
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end();)
    if (it.value().get<long long>() < now) it = state_["cooldowns"].erase(it); else ++it;
  persist();
}

json SystematicTrader::close_now(const std::string& ticker) {
  std::lock_guard<std::mutex> lk(mu_);
  long long now = now_s();
  json rows = store_.query("SELECT * FROM systematic_trades WHERE ticker=? AND state IN ('open','exiting','settling') ORDER BY id DESC LIMIT 1", {ticker});
  if (rows.empty()) throw std::runtime_error("the algorithm holds nothing in " + ticker);
  json t = rows[0];
  auto m = store_.get_market(ticker);
  if (!m) throw std::runtime_error("unknown market " + ticker);
  const std::string side = str(t, "side");
  double bid = side == "yes" ? m->yes_bid : (m->no_bid > 0 ? m->no_bid : (m->yes_ask > 0 && m->yes_ask < 1 ? 1.0 - m->yes_ask : 0));
  double bid_size = side == "yes" ? m->yes_bid_size : m->yes_ask_size;
  if (bid <= 0 || bid >= 1) throw std::runtime_error("no bid to sell into in " + ticker);
  if (!m->tradable() || (parse_iso(m->close_time) > 0 && parse_iso(m->close_time) <= now))
    throw std::runtime_error("trading in " + ticker + " has closed; the position settles");
  const std::string reason = "closed by hand from the systematic screen";
  if (str(t, "mode") == "paper") {
    double slip = number(state_["settings"], "paper_slippage");
    json mrow = market_to_json(*m);
    mrow["fee_type"] = "quadratic"; mrow["fee_multiplier"] = 1.0;
    if (auto srs = store_.get_series(m->series_ticker)) { mrow["fee_type"] = srs->fee_type; mrow["fee_multiplier"] = srs->fee_multiplier; }
    FeeModel fm = fee_model(mrow);
    double px = std::max(0.01, bid - slip), n = number(t, "filled");
    double fee = fm.fee(px, n, true), proceeds = n * px;
    finalize(t, reason + " (paper)", proceeds - fee - number(t, "entry_cost"), proceeds, fee, px, now);
    persist();
    return json{{"ok", true}, {"note", "paper position closed at " + fmt2(px)}};
  }
  if (!exec_ || !exec_->armed()) throw std::runtime_error("no API key: cannot send");
  if (hooks_.killed()) throw std::runtime_error("the kill switch is on");
  t["state"] = "exiting";
  bool sent = live_send_exit(t, reason, bid, bid_size, now);
  if (!sent && t["state"] == "exiting") { t["state"] = "open"; upsert_trade(t); }
  persist();
  json out = {{"ok", sent}, {"note", sent ? (t["state"] == "closed" ? "closed at " + fmt2(bid) : "partly closed at " + fmt2(bid))
                                          : "exit did not go; see the algorithm log"}};
  return out;
}

// ---- views and configuration -------------------------------------------------------------------

json SystematicTrader::performance(long long now) {
  json out = json::object();
  for (const std::string mode : {"live", "paper"}) {
    double realized = 0, realized_today = 0, unrealized = 0, fees = 0, open_cost = 0;
    int closed = 0, wins = 0, open_n = 0, entering = 0;
    std::string day = str(state_, "day");
    long long since = day_start_epoch(day, cfg_.risk.day_tz, now);
    for (const auto& r : store_.query("SELECT state, day, closed_ts, realized_pnl, unrealized, entry_fee, exit_fee, entry_cost, count, entry_price "
                                       "FROM systematic_trades WHERE mode=?", {mode})) {
      // NULL-tolerant reads: this runs inside view(), which book capture calls every cycle, and a
      // single NULL column (a hand-written or older row) used to throw out of the books step.
      std::string st = str(r, "state");
      if (st == "closed") {
        ++closed;
        double pnl = number(r, "realized_pnl");
        realized += pnl;
        if (pnl > 0) ++wins;
        if (integer(r, "closed_ts") >= since) realized_today += pnl;
        fees += number(r, "entry_fee") + number(r, "exit_fee");
      } else if (st == "open" || st == "exiting" || st == "settling") {
        ++open_n;
        unrealized += number(r, "unrealized");
        open_cost += number(r, "entry_cost");
        fees += number(r, "entry_fee");
      } else if (st == "entering") {
        ++entering;
        open_cost += number(r, "count") * number(r, "entry_price");
      }
    }
    out[mode] = {{"realized", round4(realized)}, {"realized_today", round4(realized_today)}, {"unrealized", round4(unrealized)},
                 {"today", round4(realized_today + unrealized)}, {"fees", round4(fees)}, {"open_cost", round4(open_cost)},
                 {"closed_trades", closed}, {"wins", wins}, {"win_rate", closed ? json(static_cast<double>(wins) / closed) : json(nullptr)},
                 {"open_positions", open_n}, {"resting_entries", entering}};
  }
  (void)now;
  return out;
}

json SystematicTrader::execution_stats() {
  json out = json::object();
  for (const std::string mode : {"live", "paper"}) {
    int attempted = 0, filled = 0, unfilled = 0, resting = 0;
    double realized = 0, fees = 0;
    json cancel_reasons = json::object();
    for (const auto& t : store_.query("SELECT state,filled_ts,realized_pnl,entry_fee,exit_fee,exit_reason FROM systematic_trades WHERE mode=?", {mode})) {
      ++attempted;
      if (integer(t, "filled_ts") > 0) ++filled;
      if (str(t, "state") == "entering") ++resting;
      if (str(t, "state") == "canceled") {
        ++unfilled;
        std::string reason = str(t, "exit_reason", "unfilled");
        cancel_reasons[reason] = cancel_reasons.value(reason, 0) + 1;
      }
      realized += number(t, "realized_pnl");
      fees += number(t, "entry_fee") + number(t, "exit_fee");
    }
    out[mode] = {{"attempted", attempted}, {"filled", filled}, {"unfilled", unfilled}, {"resting", resting},
                 {"fill_rate", filled + unfilled ? json(static_cast<double>(filled) / (filled + unfilled)) : json(nullptr)},
                 {"realized_net", round4(realized)}, {"fees", round4(fees)},
                 {"cancel_reasons", cancel_reasons},
                 {"notice", "Fill rate counts orders with any fill, not contracts. P&L uses recorded fills when available, otherwise limit-price estimates. Fees include open entries; canceled orders are not losing trades."}};
  }
  return out;
}

json SystematicTrader::view_unlocked() {
  long long now = now_s();
  json out = state_;
  out.erase("cooldowns");
  out["performance"] = performance(now);
  out["execution_stats"] = execution_stats();
  out["capital"] = systematic_capital(store_,state_["settings"],str(state_,"mode"));
  out["spent_today"] = spent_today();
  out["live_possible"] = exec_ != nullptr && exec_->armed();
  out["killed"] = hooks_.killed();
  out["positions"] = store_.query("SELECT * FROM systematic_trades WHERE state IN ('entering','open','exiting','settling') ORDER BY id DESC");
  out["history"] = store_.query("SELECT * FROM systematic_trades WHERE state IN ('closed','canceled','failed','released') ORDER BY id DESC LIMIT 200");
  json cd = json::array();
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    if (it.value().get<long long>() > now) cd.push_back({{"event_ticker", it.key()}, {"until", it.value()}});
  out["cooldowns"] = cd;
  out["notice"] = str(state_, "mode") == "live"
      ? "LIVE: entries and exits are real orders on your Kalshi account. Every order still passes the desk's own limits "
        "(order cap, turnover cap, exposure caps, daily equity stop, kill switch). The algorithm's own budget and loss stop are on top of those."
      : "PAPER: simulated fills only (taker at the ask plus slippage, exits at the bid minus slippage, fees on both). Nothing is sent.";
  return out;
}

json SystematicTrader::view() { std::lock_guard<std::mutex> lk(mu_); return view_unlocked(); }

void SystematicTrader::router_context(RouterContext& ctx) {
  std::lock_guard<std::mutex> lk(mu_);
  const long long now = ctx.now > 0 ? ctx.now : now_s();
  ctx.now = now;
  const std::string mode = str(state_, "mode");
  for (const auto& e : blocked_events(mode)) ctx.held_events.insert(e);
  for (const auto& t : store_.query("SELECT event_ticker FROM systematic_trades WHERE mode=? AND state IN "
                                    "('entering','open','exiting','settling')", {mode}))
    if (t.contains("event_ticker") && t["event_ticker"].is_string()) ctx.algo_events.insert(t["event_ticker"].get<std::string>());
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    ctx.cooldowns[it.key()] = std::max(ctx.cooldowns[it.key()], it.value().get<long long>());
  ctx.hard_gap = cfg_.risk.max_disagreement;
  ctx.desk_horizon_hours = cfg_.risk.max_entry_horizon_hours > 0 ? cfg_.risk.max_entry_horizon_hours : 48.0;
  // ctx.strategy_modes is the caller's to fill: the engine keeps a locked copy, because the modes
  // change at runtime (set_mode) and this also runs on HTTP threads.
  ctx.killed = ctx.killed || hooks_.killed();
  const bool sendable = mode == "paper" || (exec_ && exec_->armed());
  ctx.auto_armed = yes(state_, "running") && !yes(state_, "halted") && !ctx.killed && sendable;
  if (!yes(state_, "running")) ctx.auto_off_reason = "the algorithm is stopped: offered to you instead";
  else if (yes(state_, "halted"))
    ctx.auto_off_reason = "the algorithm is halted (" + str(state_, "halt_reason").substr(0, 80) + "): offered to you instead";
  else if (!sendable) ctx.auto_off_reason = "live mode has no API key: offered to you instead";
  if (mode == "paper" && ctx.auto_armed) ctx.auto_off_reason.clear();
}

json SystematicTrader::screen_unlocked(const json& board, long long now) {
  RouterContext ctx;
  ctx.now = now;
  const std::string mode = str(state_, "mode");
  ctx.held_events = blocked_events(mode);
  for (const auto& t : store_.query("SELECT event_ticker FROM systematic_trades WHERE mode=? AND state IN "
                                    "('entering','open','exiting','settling')", {mode}))
    if (t.contains("event_ticker") && t["event_ticker"].is_string()) ctx.algo_events.insert(t["event_ticker"].get<std::string>());
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    ctx.cooldowns[it.key()] = it.value().get<long long>();
  ctx.hard_gap = cfg_.risk.max_disagreement;
  ctx.desk_horizon_hours = cfg_.risk.max_entry_horizon_hours > 0 ? cfg_.risk.max_entry_horizon_hours : 48.0;
  ctx.strategy_modes = cfg_.strategy_modes;
  ctx.killed = hooks_.killed();
  const bool sendable = mode == "paper" || (exec_ && exec_->armed());
  ctx.auto_armed = yes(state_, "running") && !yes(state_, "halted") && !ctx.killed && sendable;
  return systematic_screen(board, state_["settings"], ctx);
}

json SystematicTrader::screen(const json& board, long long now) {
  std::lock_guard<std::mutex> lk(mu_);
  std::set<std::string> held = blocked_events(str(state_, "mode"));
  std::map<std::string, long long> cooldowns;
  for (auto it = state_["cooldowns"].begin(); it != state_["cooldowns"].end(); ++it)
    cooldowns[it.key()] = it.value().get<long long>();
  return systematic_screen(board, state_["settings"], now, held, cooldowns, cfg_.risk.max_disagreement);
}

json SystematicTrader::configure(const json& body) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!body.is_object()) throw std::invalid_argument("request must be an object");
  for (auto it = body.begin(); it != body.end(); ++it)
    if (it.key() != "settings" && it.key() != "running" && it.key() != "mode" && it.key() != "clear_halt" && it.key() != "release")
      throw std::invalid_argument("unknown field: " + it.key());
  long long now = now_s();
  if (body.contains("settings")) {
    json settings = validate_systematic_settings(body["settings"], state_["settings"]);
    if (settings != state_["settings"]) {
      state_["settings"] = settings;
      state_["settings_version"] = integer(state_, "settings_version") + 1;
      if (number(settings, "cooldown_minutes") <= 0) state_["cooldowns"] = json::object();
      log("info", "settings changed (version " + std::to_string(integer(state_, "settings_version")) + ")");
    }
  }
  if (body.contains("mode")) {
    if (!body["mode"].is_string() || (body["mode"] != "live" && body["mode"] != "paper"))
      throw std::invalid_argument("mode must be live or paper");
    if (body["mode"] == "live" && (!exec_ || !exec_->armed()))
      throw std::invalid_argument("live mode needs an API key on this desk");
    if (body["mode"] != state_["mode"]) {
      state_["mode"] = body["mode"];
      log("warn", std::string("mode -> ") + body["mode"].get<std::string>());
    }
  }
  if (body.contains("clear_halt") && body["clear_halt"].is_boolean() && body["clear_halt"].get<bool>()) {
    if (yes(state_, "halted")) {
      // Clearing a loss stop accepts the loss so far: the stop re-arms from today's P&L as it
      // stands now, so the same loss does not trip it again on the next cycle.
      double today = pnl_today(now);
      state_["loss_stop_base"] = std::min(0.0, today);
      log("warn", "halt cleared by hand: " + str(state_, "halt_reason") +
                      (today < 0 ? " (loss stop re-armed from " + fmt2(today) + ")" : ""));
    }
    state_["halted"] = false;
    state_["halt_reason"] = "";
    state_["halt_class"] = "";
  }
  if (body.contains("running")) {
    if (!body["running"].is_boolean()) throw std::invalid_argument("running must be boolean");
    bool want = body["running"].get<bool>();
    if (want && state_["mode"] == "live" && (!exec_ || !exec_->armed()))
      throw std::invalid_argument("live mode needs an API key on this desk");
    if (want && hooks_.killed()) throw std::invalid_argument("the kill switch is on; re-arm the desk first");
    if (want != yes(state_, "running")) {
      state_["running"] = want;
      if (want) {
        state_["started_ts"] = now;
        roll_day(now);
        log("warn", std::string("STARTED in ") + str(state_, "mode") + " mode: budget $" +
                        fmt2(number(state_["settings"], "daily_budget_dollars")) + "/day, daily loss stop " +
                        (yes(state_["settings"],"daily_loss_stop_enabled") ? "$" + fmt2(number(state_["settings"], "daily_loss_stop_dollars")) : "off") + ", up to " +
                        std::to_string(integer(state_["settings"], "max_positions")) + " positions");
      } else {
        state_["stopped_ts"] = now;
        log("warn", "STOPPED: no new entries; open positions keep their exit rules until closed or released");
      }
    }
  }
  if (body.contains("release") && body["release"].is_string()) {
    std::string ticker = body["release"].get<std::string>();
    for (auto t : store_.query("SELECT * FROM systematic_trades WHERE ticker=? AND state IN ('entering','open','exiting','settling')", {ticker})) {
      if (str(t, "mode") == "live") {
        // Handing back a position also withdraws our outstanding instructions. An unrelated
        // manual order on this ticker is never part of this handoff.
        for (const auto& order : store_.orders("resting"))
          if (order.order_id == str(t, "entry_order_id") || order.order_id == str(t, "exit_order_id")) {
            if (!exec_) throw std::runtime_error("cannot release while strategy orders are working: API unavailable");
            exec_->cancel(order.order_id); // cancellation failure leaves this row managed
            hooks_.request_reconcile();
          }
        if (str(t, "state") == "entering") {
          auto order = store_.query("SELECT fill_count FROM orders WHERE order_id=?", {str(t, "entry_order_id")});
          double filled = order.empty() ? 0.0 : number(order[0], "fill_count");
          FeeModel fm;
          if (auto series = store_.get_series(str(t, "series_ticker"))) fm = FeeModel::from_series(*series);
          double price = number(t, "entry_price"), fee = fm.fee(price, filled, str(t, "entry_style") == "taker");
          double reserved = number(t, "count") * (price + fm.fee(price, 1, str(t, "entry_style") == "taker"));
          actual_fill_cost(store_, str(t, "entry_order_id"), str(t, "side"), filled, &price, &fee);
          t["filled"] = filled; t["entry_cost"] = round4(filled * price + fee); t["entry_fee"] = fee;
          add_spent(number(t, "entry_cost") - reserved);
        } else if (str(t, "state") == "exiting" && str(t, "exit_style") == "maker") {
          json market = json::object();
          if (auto series = store_.get_series(str(t, "series_ticker"))) {
            market["fee_type"] = series->fee_type; market["fee_multiplier"] = series->fee_multiplier;
          }
          live_manage_resting_exit(t, market, number(t, "filled"), now, true);
          if (str(t, "state") == "closed") continue;
        }
      }
      t["state"] = "released";
      t["closed_ts"] = now;
      t["exit_reason"] = "released to manual management";
      t["unrealized"] = 0.0;
      upsert_trade(t);
      log("warn", ticker + " released: the algorithm no longer manages this position; its P&L is not counted here");
    }
  }
  persist();
  return view_unlocked();
}

}  // namespace desk
