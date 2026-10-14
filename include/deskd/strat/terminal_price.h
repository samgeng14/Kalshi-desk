#pragma once
// Ported unchanged from the old desk (include/desk/models/terminal_price.h, kept by plan section
// 2 as models/terminal_price.cpp) for plan section 5's LIP-15 anchor: Phi(ln(S/S0)/(sigma sqrt tau)).
//
// One terminal-price distribution, and every contract on that underlying priced from it.
//
// The desk prices a short-duration commodity contract by answering one question -- where can the
// settlement reference be when the window closes -- and then reading every contract off that one
// distribution. A market is never modelled on its own: YES and NO, a threshold and a range, two
// strikes on the same underlying all come from the same `TerminalDistribution`, so they cannot
// disagree with each other.
//
//   reference feed  ->  VolEstimate  ->  TerminalDistribution  ->  probability(TerminalPayoff)
//
// Nothing here does I/O, reads a Store or knows what Kalshi is: it takes observations and a payoff
// and returns a probability, which is what makes it testable and what keeps the settlement rules
// (docs/commodity-settlement.md) in one place -- `terminal_payoff()` -- rather than spread across
// the model.
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>


namespace deskd {

using nlohmann::json;

// ---- what a contract pays on the terminal value ------------------------------------------------
// Inclusivity is not a rounding detail. Kalshi's 15-minute commodity markets are `greater_or_equal`
// against a published strike that has the same granularity as the feed, so S_T == K happens and it
// pays YES. The payoff carries the rule; the model never assumes continuity makes it irrelevant.
struct TerminalPayoff {
  enum Type { Above, Below, Range };
  Type type = Above;
  double lower = 0;          // Above and Range
  double upper = 0;          // Below and Range
  bool lower_inclusive = false;
  bool upper_inclusive = false;
  std::string strike_type;   // what the exchange called it, for the record

  bool pays(double terminal) const;
  json to_json() const;
};

// The payoff of a Kalshi market, from its strike type and strikes. Returns nothing for a market
// this model has no business pricing (structured, functional, a missing strike).
std::optional<TerminalPayoff> terminal_payoff(const std::string& strike_type,
                                              std::optional<double> floor_strike,
                                              std::optional<double> cap_strike);

// ---- the distribution --------------------------------------------------------------------------
// log(S_T / S_t) ~ Normal(mu * tau, sigma^2 * tau). `sigma` is per square root of a SECOND, which
// is the horizon these contracts live on; annualising is a display choice, not a modelling one.
// v1 keeps mu at zero: no drift has been demonstrated on this horizon, and a drift a model cannot
// defend is a directional bet wearing a distribution's clothes.
struct TerminalDistribution {
  double spot = 0;             // S_t, the reference value now
  double sigma = 0;            // per sqrt(second), in log price
  double tau_seconds = 0;      // to the settlement observation
  double mu = 0;               // per second, in log price

  bool usable() const;
  double sd() const;                          // sigma * sqrt(tau)
  // Distance to a strike in sigma units, positive when the reference is above the strike.
  double z(double strike) const;
  double p_below(double strike, bool inclusive = false) const;
  double p_above(double strike, bool inclusive = false) const;
  double p_between(double lo, double hi, bool lo_inclusive = true, bool hi_inclusive = true) const;
  double probability(const TerminalPayoff& payoff) const;
  json to_json() const;
};

// ---- volatility from the reference feed's own observations --------------------------------------
struct VolSettings {
  int sample_seconds = 5;             // grid the ticks are sampled onto before differencing
  double ewma_halflife_seconds = 600;
  double short_floor_multiplier = 1.0;   // sigma is at least this times the 5-minute estimate
  int min_samples = 60;               // ~5 minutes at a 5-second grid
  long long min_span_seconds = 300;
  double band_ratio = 0.25;           // +/- this much on sigma gives the quote's own uncertainty
  double max_ratio_to_hour = 4.0;     // a spike cannot take sigma above this times the hourly estimate
  double min_ratio_to_hour = 0.25;

  // What to do with a return that spans a long silence on the feed. This is a modelling choice
  // forced by an ambiguity the desk cannot presently resolve, so it is a setting with two named
  // positions rather than a constant buried in a loop:
  //
  //   DropUncertain  a return spanning more than `max_return_gap_seconds` of silence is thrown
  //                  away. Correct if a silence means the source stopped publishing, because then
  //                  the move happened at an unknown time and dividing it by the gap is a guess.
  //                  It biases sigma UP if a silence really means "the price did not change",
  //                  since it discards precisely the calm stretches.
  //
  //   IncludeAll     every sampled interval is differenced, however long. Correct if the channel
  //                  only emits on a change, because then the forward-filled path is exact and a
  //                  long flat stretch is a genuine run of zero returns.
  //
  // Which is right is an empirical question about the channel, and `unchanged_value_messages` in
  // the feed's status is the measurement that answers it: a channel that suppresses unchanged
  // values can never send one, so a single occurrence settles it. Until then the conservative
  // position is the default and the research ladder races both.
  enum class GapPolicy { DropUncertain, IncludeAll };
  GapPolicy long_gap_policy = GapPolicy::DropUncertain;
  long long max_return_gap_seconds = 120;
};

struct VolEstimate {
  double sigma = 0;        // per sqrt(second): what the model uses
  double ewma = 0, rv_1m = 0, rv_5m = 0, rv_15m = 0, rv_60m = 0;
  int samples = 0;
  long long span_seconds = 0;
  long long last_ts = 0;
  bool warm = false;       // enough history to stand behind the number
  std::string basis;       // which estimate is binding, in words
  std::vector<std::string> problems;

  double annualized() const;
  json to_json() const;
};

// `ticks` are (unix seconds, price), oldest first. Only observations at or before `now` are read:
// the replay depends on that and so does the live path.
VolEstimate estimate_volatility(const std::vector<std::pair<long long, double>>& ticks, long long now,
                                const VolSettings& s = VolSettings());

// The close of the one-minute candle stamped at `minute_end`: the last observation in
// [minute_end - 60, minute_end). This is what these contracts settle on, so it is what the desk
// recomputes to check that it is reading the same feed (docs/commodity-settlement.md).
std::optional<double> minute_close(const std::vector<std::pair<long long, double>>& ticks,
                                   long long minute_end);

}  // namespace deskd
