#pragma once
// Kalshi fee schedule (effective July 7, 2026):
//   taker: round_up(M * 0.07   * C * P * (1-P))
//   maker: round_up(M * 0.0175 * C * P * (1-P))   (only on *_with_maker_fees series)
// Rounded up to a centicent ($0.0001). M = series fee_multiplier (0, 0.5, 1, ...).
// The series object from the API carries fee_type + fee_multiplier, so nothing is hardcoded.
#include <string>

#include "desk/types.h"

namespace desk {

struct FeeModel {
  std::string fee_type = "quadratic";  // quadratic | quadratic_with_maker_fees | quadratic_with_combo_maker_fees
  double multiplier = 1.0;

  static FeeModel from_series(const Series& s) { return FeeModel{s.fee_type, s.fee_multiplier}; }

  bool maker_pays() const { return fee_type != "quadratic"; }
  double maker_rate() const {
    if (fee_type == "quadratic_with_combo_maker_fees") return 0.035;
    if (fee_type == "quadratic_with_maker_fees") return 0.0175;
    return 0.0;
  }
  // Total fee in dollars for `count` contracts at `price` (price of the side bought).
  double fee(double price, double count, bool taker) const;
  double fee_per_contract(double price, bool taker) const { return fee(price, 1.0, taker); }
};

// Full-Kelly fraction of bankroll for a $1-payout contract bought at all-in cost `cost`.
double kelly_fraction(double p_win, double cost);

// Contracts to buy given bankroll, win prob, all-in cost, Kelly fraction and a cap on the
// fraction of bankroll committed.
double size_contracts(double bankroll, double p_win, double cost, double kelly_frac, double cap_frac);

}  // namespace desk
