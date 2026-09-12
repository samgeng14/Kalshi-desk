#pragma once
// The one place the desk decides who, if anyone, acts on a priced candidate.
//
//   DATA -> PROBABILITY MODEL (probability.h) -> EDGE ENGINE (edge.h) -> DECISION ROUTER (here)
//        -> MANUAL board / AUTO lane / RESEARCH record -> shared risk -> shared executor
//
// The router owns execution eligibility. Models do not: a model reports a probability and what it
// knows about it; the router combines that with the book, the edge, the operator's routes and the
// desk's risk state, and gives every candidate exactly one route:
//
//   BLOCKED   a data or risk gate (closed, stale, invalid probability or book, settlement unknown or
//             outside the desk's horizon, ownership, disagreement, kill switch), or the thresholds
//             are not met (shown as "weak": valid data, nothing worth anyone's attention);
//   RESEARCH  a valid probability nobody should act on yet (research-only estimate, settlement not
//             verified, model routed to research or not routed at all);
//   MANUAL    tradable and worth a look, held back from unattended execution by a named reason;
//   AUTO      everything above passes, the model is routed auto AND graduated, and the master
//             switch is armed.
//
// systematic_screen() (systematic.h) is this router rendered as the screen's JSON rows, so the
// manual board, the Systematic tab and the automatic lane all read one decision.
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "desk/edge.h"
#include "desk/probability.h"
#include "desk/route.h"

namespace desk {

enum class DecisionRoute { Blocked, Research, Manual, Auto };
const char* decision_route_name(DecisionRoute r);

struct TradeDecision {
  DecisionRoute route = DecisionRoute::Blocked;
  EdgeCandidate candidate;              // the one EdgeCandidate both lanes act on
  std::vector<std::string> reasons;     // every gate or policy that shaped the route, in priority order
  std::vector<std::string> warnings;
  std::string verdict;                  // display: algo | manual | weak | research | blocked
  std::string decision_id;              // ticker, side, model version, model time, book time
  long long book_ts = 0;
  long long generated_ts = 0;
  EdgeCandidate alternate;              // the other execution style, for the order dialog
  nlohmann::json market = nlohmann::json::object();   // the board row it was decided on
};

struct RouterContext {
  long long now = 0;
  std::set<std::string> held_events;              // positions, working orders, the algorithm's trades
  // Why an event is held, where it matters: the algorithm's own trades, and working orders or queued
  // manual requests. An event held ONLY by your own position can be offered a qualified same-side
  // addition (manual, never automatic); anything else in a held event is blocked.
  std::set<std::string> algo_events;
  std::set<std::string> working_events;
  std::map<std::string, long long> cooldowns;     // event -> no new entry before this
  double hard_gap = 0.30;                         // the desk's model/market disagreement gate
  double desk_horizon_hours = 48.0;               // the desk-wide T+2 entry limit
  bool auto_armed = true;                         // the AUTO master switch; policy views pass true
  std::string auto_off_reason = "the algorithm is not running: offered to you instead";
  bool killed = false;                            // global risk refusal
  long long max_age = 120;                        // entry freshness limit; decide_board takes the board's
  // Each strategy's mode from config/desk.json (off | research | manual | auto | bot). Still honored,
  // as a cap: a model you set to research there is never offered, whatever its model route says.
  std::map<std::string, std::string> strategy_modes;
};

// Route of a model: model_routes[name_version], capped by model_routes[name] when that is set (a
// bare-name route restricts every version and grants none); a model with no versioned route is
// research until the operator gives it one. Something unversioned (version "", a locked basket) is
// routed by its name. `known` says whether any route was found. Graduation is per name_version too.
RouteMode model_route(const nlohmann::json& settings, const std::string& model_name, const std::string& model_version,
                      bool* known = nullptr);
bool model_graduated(const nlohmann::json& settings, const std::string& model_name, const std::string& model_version);
// The category route is the outer permission: the effective route is the lower of the two
// (off < research < manual < auto), so a category set to auto never promotes a model that is not.
RouteMode effective_route(RouteMode category, RouteMode model);

std::string decision_id_for(const std::string& ticker, Side side, const std::string& model_key, long long model_ts,
                            long long book_ts);

// `settings` must already be validated (validate_systematic_settings).
TradeDecision decide(const nlohmann::json& market_row, Side side, const nlohmann::json& settings,
                     const RouterContext& ctx);
std::vector<TradeDecision> decide_board(const nlohmann::json& board, const nlohmann::json& settings,
                                        const RouterContext& ctx);

// The screen's row for one decision: every field the screen has always carried, plus the decision's.
nlohmann::json decision_to_row(const TradeDecision& d);
// The screen: settings, generated_ts, rows (sorted algo, manual, weak, research, blocked), counts.
nlohmann::json decisions_to_screen(const std::vector<TradeDecision>& decisions, const nlohmann::json& settings,
                                   long long now, int markets);

// A displayed decision row priced again: the book it was decided on (its side's bid and ask, through
// normalize_prices) and the probability it was decided on (its quote), at the price and style a
// person chose in the order dialog. The same candidate_at() the router used; nothing is re-derived.
// The "too near close" floor for one market, in minutes. Normally the settings' flat
// `min_minutes_to_close`; for a contract whose whole trading life is shorter than that, the
// smaller `min_life_fraction_to_close` share of its own life, because a floor above a contract's
// life would block every quote it ever has. Needs `open_ts` and `close_ts` on the row; without
// them the flat floor stands.
double min_minutes_to_close_for(const nlohmann::json& market_row, const nlohmann::json& settings);

MarketSidePrices decision_prices(const nlohmann::json& decision_row);
EdgeCandidate decision_edge_at(const nlohmann::json& decision_row, const std::string& style, double price,
                               const nlohmann::json& settings);

}  // namespace desk
