#pragma once
// Ladder arbitrage: the one edge in a mutually exclusive event that needs no forecast.
//
// If at most one market in an event can settle YES, then selling YES (buying NO) across a set S
// of its brackets pays |S| - 1 in every state where one of them wins, and |S| when none does.
// So the payout is at least |S| - 1 whatever happens, and the trade is locked when
//
//     sum of the YES bids over S  >  1 + fees.
//
// That needs Kalshi's own mutually_exclusive flag (risk.h: event_structure) and nothing else:
// no model, no volatility, no forecast. The mirror trade - buying YES across every bracket for
// less than $1 - additionally needs the brackets to cover every outcome.
#include <optional>
#include <string>
#include <vector>

#include "desk/config.h"
#include "desk/store.h"

namespace desk {

struct LadderLeg {
  std::string ticker, label, side;   // side: "no" (sell the bracket) or "yes" (buy it)
  double price = 0, fee_per = 0, yes_bid = 0, yes_ask = 0, size_available = 0;
};

struct LadderArb {
  std::string event_ticker, kind;    // kind: "sell" (bids sum over 1) or "buy" (asks sum under 1)
  std::vector<LadderLeg> legs;
  double sum_prices = 0;             // sum of bids (sell) or asks (buy) over the legs
  double edge_per_contract = 0;      // locked dollars per contract, after taker fees
  double cost_per_contract = 0;      // cash needed per contract for the whole basket
  double count = 0;                  // contracts the displayed sizes and the caps allow
  double total_edge = 0, total_cost = 0;
  std::string note;
};

// Dominance: when one contract's condition implies another's ("84 or above" implies "80 or
// above" on the same day), the harder one can never be worth more than the easier one. If the
// book bids more for the harder contract than it asks for the easier one, selling the harder and
// buying the easier pays at least $1 in every state for less than $1. This needs no exclusivity
// flag - the implication is read off the strikes - so it covers the events the ladder scan cannot.
std::optional<LadderArb> find_dominance_arb(Store& store, const Config& cfg, const std::string& event_ticker,
                                            const std::vector<const Market*>& markets, double max_cost);

// Best locked basket in one event, or nothing. `markets` is the event's contracts as the engine
// already has them in memory. `max_cost` caps the cash the basket may use.
std::optional<LadderArb> find_ladder_arb(Store& store, const Config& cfg, const std::string& event_ticker,
                                         const std::vector<const Market*>& markets, double max_cost);

}  // namespace desk
