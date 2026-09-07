#pragma once
// Hedging. A binary event's risk is a small, enumerable set of states (see risk.h); once the
// exchange has told us the event is mutually exclusive, "what protects this position" stops
// being a judgement call and becomes a search over the other markets in the same event.
#include <optional>
#include <string>
#include <vector>

#include "desk/config.h"
#include "desk/risk.h"
#include "desk/store.h"

namespace desk {

// One protective order: buy `count` of `side` in `ticker` at `price`.
struct HedgeLeg {
  std::string ticker, label, side, style = "taker", note;
  double count = 0, price = 0, fee_per = 0, cost = 0;
  double worst_before = 0, worst_after = 0;   // event worst-case loss, dollars
  double ev_before = 0, ev_after = 0;         // market-implied expected P&L, dollars
  bool have_ev = false;
  std::vector<StatePayoff> after;             // the payoff table once this leg is on
};

struct HedgePlan {
  bool have_structure = false;      // the event is verified mutually exclusive
  bool exhaustive = false;
  double worst_before = 0, ev_before = 0;
  bool have_ev = false;
  std::vector<StatePayoff> before;
  std::vector<HedgeLeg> options;    // best first (at most a handful)
  std::string note;                 // why the list is empty, when it is
};

// `add` is a trade you are about to make (nullopt = hedge what you already hold).
// `loss_limit` is the worst-case loss you are willing to carry in this event; a leg that brings
// the worst case under it is preferred, and among those the one that costs the least in expected
// value. With no such leg, the best worst-case reduction per dollar is offered instead.
HedgePlan plan_hedge(Store& store, const Config& cfg, const std::string& event_ticker,
                     const std::vector<Holding>& existing, const std::optional<Holding>& add,
                     double loss_limit);

// Holdings the desk already has in one event: filled positions plus resting orders.
std::vector<Holding> event_holdings(Store& store, const std::string& event_ticker);

}  // namespace desk
