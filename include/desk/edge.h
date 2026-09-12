#pragma once
// One edge calculation for the whole desk.
//
// A ProbabilityQuote is priced against the book here, for YES and for NO, and the result feeds
// both the manual board and the algorithm: the same EdgeCandidate a person sees in the order
// dialog is the one the automatic lane executes. Nothing else on the desk computes an entry edge.
//
// All YES/NO price arithmetic goes through normalize_prices(). The NO book is the YES book seen
// from the other side (NO ask = 1 - YES bid, NO bid = 1 - YES ask, and the size at the NO ask is
// the size on the YES bid); that identity lives in exactly one function.
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "desk/fees.h"
#include "desk/probability.h"
#include "desk/types.h"

namespace desk {

enum class Side { Yes, No };
inline const char* side_name(Side s) { return s == Side::Yes ? "yes" : "no"; }
inline Side side_from(const std::string& s) { return s == "no" ? Side::No : Side::Yes; }

// Prices are dollars in (0, 1); 0 means "no price on that side". Sizes are contracts displayed at
// the touch.
struct MarketSidePrices {
  double yes_bid = 0, yes_ask = 0, no_bid = 0, no_ask = 0;
  double yes_bid_size = 0, yes_ask_size = 0, no_bid_size = 0, no_ask_size = 0;
  bool crossed = false;   // YES bid above YES ask: the book cannot be traded as read

  double bid(Side s) const { return s == Side::Yes ? yes_bid : no_bid; }
  double ask(Side s) const { return s == Side::Yes ? yes_ask : no_ask; }
  double bid_size(Side s) const { return s == Side::Yes ? yes_bid_size : no_bid_size; }
  double ask_size(Side s) const { return s == Side::Yes ? yes_ask_size : no_ask_size; }
  // Both sides quoted and not crossed. A locked book (bid == ask) still counts, as it always has.
  bool two_sided(Side s) const;
  // The YES mid of a two-sided book; 0 otherwise.
  double yes_mid() const;
};

// The one normalization. Explicit NO prices win when they are real prices; a missing one is
// derived from the YES book (and a missing YES price from the NO book). Anything outside (0, 1)
// is treated as missing.
MarketSidePrices normalize_prices(double yes_bid, double yes_ask, double no_bid = 0, double no_ask = 0,
                                  double yes_bid_size = 0, double yes_ask_size = 0, double no_bid_size = 0,
                                  double no_ask_size = 0);
MarketSidePrices normalize_prices(const Market& m);
MarketSidePrices normalize_prices_from_row(const nlohmann::json& row);   // a board row

struct EdgeSettings {
  double uncertainty_multiplier = 0.5;   // x sigma, deducted from the executable edge
  double taker_slippage = 0.0;           // per contract, a taker's expected cost beyond the touch
};

struct EdgeCandidate {
  ProbabilityQuote fair;
  Side side = Side::Yes;
  std::string style = "taker";   // taker: lift the actual ask; maker: post at the maker price

  double fair_probability = NAN;      // p_yes for YES, 1 - p_yes for NO
  double raw_fair_probability = NAN;  // the same from the model before any market blend
  double entry_price = 0;

  double entry_fee = 0;               // per contract, at entry_price for this style
  double estimated_slippage = 0;
  double uncertainty_buffer = 0;      // uncertainty_multiplier * sigma

  double raw_edge = 0;                // fair_probability - entry_price
  double net_edge = 0;                // raw_edge - entry_fee
  double executable_edge = 0;         // net_edge - estimated_slippage - uncertainty_buffer
  double ev_per_contract = 0;         // expected dollars per contract after fee and slippage
  double kelly = 0;                   // (fair - cost - buffer) / (1 - cost)

  double bid = 0, ask = 0, spread = 0;
  double bid_size = 0, ask_size = 0;
  int displayed_size = 0;             // contracts at the price this candidate trades against
  bool executable = false;            // there is a price to trade at

  nlohmann::json diagnostics = nlohmann::json::object();
};

// The candidate for buying `side` as a taker at the side's actual ask. Never the mid.
EdgeCandidate taker_candidate(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const FeeModel& fm,
                              const EdgeSettings& es);
// The maker candidate: one tick inside the spread when there is room, else joining the bid, never
// at or through the ask (the desk's maker rule, unchanged).
EdgeCandidate maker_candidate(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const FeeModel& fm,
                              double tick, const EdgeSettings& es);
// The same arithmetic at an arbitrary price and style: a re-priced ladder step, or the price a
// person typed into the order dialog.
EdgeCandidate candidate_at(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const std::string& style,
                           double price, const FeeModel& fm, const EdgeSettings& es);

nlohmann::json candidate_to_json(const EdgeCandidate& c);

}  // namespace desk
