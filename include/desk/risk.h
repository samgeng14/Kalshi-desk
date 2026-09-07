#pragma once
#include "desk/config.h"
#include "desk/store.h"
namespace desk {
// One thing we hold, or would hold, in an event: a YES or NO stake in one market.
struct Holding {
  std::string ticker;
  bool yes = true;
  double count = 0;  // contracts
  double cost = 0;   // dollars paid (price*count + fees), the most this holding can lose
  bool contingent = false;  // resting/proposed: may fill partially or not at all
};
// Conservative payoff-space view: without verified event settlement metadata, markets may
// resolve independently. Only filled opposite sides of the exact same ticker offset.
struct EventRisk {
  double worst_loss = 0;      // max over states of (cost - payout): what the cap measures
  double cost = 0;            // sum of costs (the old, no-netting number)
  std::string worst_state;    // independent ticker=YES/NO scenarios
};
EventRisk event_risk(Store& store, const std::string& event_ticker, const std::vector<Holding>& holdings);

// How the markets of one event settle together. `exclusive` is Kalshi's own mutually_exclusive
// flag (cached in event_meta by the engine): at most one market settles YES. `exhaustive` is
// proven here from the strikes: the brackets tile the whole line, so exactly one settles YES.
// Without both, the desk assumes the markets can settle independently, which is what the
// conservative per-ticker model above does.
struct EventStructure {
  bool exclusive = false, exhaustive = false;
  std::vector<std::string> tickers;   // markets of the event, low strike first
  std::vector<std::string> labels;    // human labels, parallel to tickers
};
EventStructure event_structure(Store& store, const std::string& event_ticker);
// One row per settlement state: what the book loses (negative = gain) if that state happens.
// The last row is the "nothing settles YES" state, present unless the event is exhaustive.
struct StatePayoff {
  std::string ticker;   // market that settles YES ("" = none of them)
  std::string label;
  double loss = 0;      // cost - payout, in dollars
  double prob = -1;     // market-implied probability of the state (-1 = unknown)
};
std::vector<StatePayoff> event_payoffs(Store& store, const std::string& event_ticker,
                                       const std::vector<Holding>& holdings);
// True when `add` loses money in a state that is already a worst state for `existing`: it stacks
// the same bet instead of diversifying or hedging it (LAX B79.5 YES + LAX T80 NO, 2026-09-05).
bool stacks_event(Store& store, const std::string& event_ticker, const std::vector<Holding>& existing, const Holding& add);
struct Exposure {
  double filled = 0, resting = 0;                 // purchase cost, for display (live markets)
  double settling = 0;                            // cost sitting in markets closed for trading, awaiting payout
  std::vector<std::string> settling_tickers;
  std::map<std::string, double> events, clusters; // worst-case loss per event / summed per cluster
  std::map<std::string, std::vector<Holding>> holdings;  // per event: positions + resting orders
  std::map<std::string, std::string> worst_state;        // per event
  double committed = 0;                           // sum of worst-case losses over events
  double total() const { return committed; }
};
std::string risk_cluster(Store& store, const Market& market);
bool weather_source_supported(const Market& market);
Exposure committed_exposure(Store& store);
json daily_equity(Store& store, const Config& cfg);
void validate_entry(Store& store, const Config& cfg, const Proposal& proposal, const Market& market);
json risk_book(Store& store, const Config& cfg);
} // namespace desk
