#pragma once
#include "desk/config.h"
#include "desk/fees.h"
#include "desk/store.h"
namespace desk {
// Cached shard-specific cash less resting and queued reservations. Missing/stale data is unknown.
json shard_cash_view(Store& store,const Market& market,long long now);
// Conservative ownership from strategy order/lot ledgers, never inferred from net holdings.
double algorithm_position_quantity(Store& store, const std::string& ticker, bool yes_side);
double manual_position_quantity(Store& store, const Position& position);
json account_stop_policy(Store& store, const Config& cfg);
json set_account_stop_policy(Store& store, const Config& cfg, const json& changes);
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
// What adding one leg does to the worst case of an event, which is the only question that
// distinguishes a hedge from the same bet twice. Two differently named contracts are not opposite
// sides of anything: on 2026-09-06 this desk held Paris YES and Marseille NO in the same match,
// which pay together in two of the three states and lose together in the third.
struct StackCheck {
  double worst_before = 0, worst_after = 0;
  double added_cost = 0;      // the most the new leg can itself lose
  double protection = 0;      // added_cost minus the rise in the worst case: what it buys back
  bool stacks = false;
  std::string reason;
};
StackCheck stack_check(Store& store, const std::string& event_ticker,
                       const std::vector<Holding>& existing, const Holding& add);
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
// Every dollar the cash balance moved, and whether the desk can say why. A binary pair nets on
// Kalshi: buying YES while holding NO on the same ticker closes the position and releases $1 of
// collateral per contract at once, so a close is not a purchase. Counting closes as purchases is
// what made this desk unable to explain its own balance.
struct CashReconciliation {
  double paid = 0;          // cash paid for every fill, offsetting ones included
  double opened = 0;        // of that, the part that opened new risk (reporting only)
  double released = 0;      // collateral returned by offsetting fills, $1 per matched contract
  double fees = 0;
  double settlements = 0;   // payouts received
  double flows = 0;         // deposits, withdrawals, transfers to another exchange instance
  double predicted = 0;     // what the cash balance should have done
  double actual = 0;        // what it did
  double residual = 0;      // actual - predicted; anything but zero is unexplained money
  bool explained = false;
};
CashReconciliation reconcile_cash(Store& store, long long from_ts, long long to_ts);

// Has a strategy shown its probabilities are worth sizing on? Scored against the market's own
// mid on the same settled markets, which is the only opponent that matters: a model can be well
// calibrated in absolute terms and still be strictly worse than the price it is trading against.
struct Calibration {
  // `n` is SETTLED MARKETS, not logged fairs. The difference decides whether this gate means
  // anything: a market the desk quoted every cycle for a day contributes a hundred rows that all
  // share one outcome, so counting rows made 107 weather markets look like 11,390 independent
  // samples and let a strategy clear a 200-sample bar on a fraction of the evidence.
  int n = 0;               // settled markets compared
  int rows = 0;            // fairs behind them, kept only to show the ratio
  double brier_model = 0, brier_market = 0;
  // Paired per-market difference (model minus market; negative means the model is better) and a
  // 95% interval on it. "No worse than the market" on a point estimate is not a finding.
  double diff = 0, diff_lo = 0, diff_hi = 0;
  bool demonstrated = false;   // enough markets AND the whole interval below zero
  std::string note;
};
Calibration strategy_calibration(Store& store, const std::string& strategy, int min_n);

// Money the desk knows has left the balance it reads and cannot see afterwards: transfers to
// another exchange instance. Equity that ignores this is short by exactly this much.
double untracked_transferred(Store& store);

// Position-aware admission for every new entry. Holding any contract in an event moves the event
// out of discovery and into management. Only an exact same-ticker, same-side addition can return,
// and then only after a cooldown and at a materially higher edge. Hedges are admitted solely when
// they reduce the event's worst case. This is recomputed immediately before every send.
struct EntryContext {
  std::string kind = "new";  // new | addition | hedge | managed
  bool allowed = true;
  bool approval_required = false;
  bool has_event_position = false;
  bool has_working_order = false;
  double required_edge = 0;
  long long cooldown_remaining = 0;
  long long settlement_ts = 0;
  std::string reason;
  json to_json() const;
};
long long expected_settlement_ts(const Market& market);
EntryContext entry_context(Store& store, const Config& cfg, const Proposal& proposal,
                           const Market& market, long long now = 0);

json daily_equity(Store& store, const Config& cfg);
void validate_entry(Store& store, const Config& cfg, const Proposal& proposal, const Market& market);
// Whether to keep a position, reduce it, or close it - and why, in words, for every position.
//
// The comparison is between two numbers and only two. A $1 binary settles at $1 if the side held
// wins, so its expected settlement value is simply the probability of that, times the size. What
// selling would return is the executable bid, times the size, minus the fee the sale itself
// costs. Holding is worth doing when the first exceeds the second.
//
// What is NOT in that comparison is what the position cost. The entry price decides the profit
// that gets reported; it says nothing about whether holding from here is attractive. A bid of
// $0.72 is a gift when fair value is $0.66 and robbery when fair value is $0.85, and in neither
// case does it matter whether the contracts were bought at $0.20 or $0.90.
struct ExitDecision {
  std::string action;        // "hold" | "reduce" | "close" | "no_bid" | "stale" | "unpriced"
  double hold_value = 0;     // p(held side wins) * count, in dollars
  double sell_now = 0;       // executable proceeds after the fee to sell
  double advantage = 0;      // hold_value - sell_now: what holding is worth over selling
  double per_contract = 0;   // the same advantage per contract, which is the comparable number
  double suggest_count = 0;  // how many to sell if action is reduce or close
  std::string why;
};

struct ExitPolicy {
  double close_below = -0.02;   // holding worth this much less per contract than selling: get out
  double reduce_below = 0.0;    // between the two: take some off
  double reduce_fraction = 0.5;
  double min_bid = 0.02;        // never sell into a bid this thin; it is not an exit, it is a gift
};

// `held_p` is the probability the side actually held settles yes. `stale` says the model number
// behind it is too old to act on, which stops additions without forcing a liquidation.
ExitDecision exit_decision(double count, double held_p, double bid, const FeeModel& fees,
                           bool stale, bool have_model, const ExitPolicy& policy);

json risk_book(Store& store, const Config& cfg);
} // namespace desk
