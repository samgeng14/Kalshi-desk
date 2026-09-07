#include "desk/fees.h"

#include <algorithm>
#include <cmath>

namespace desk {

static double ceil_centicent(double x) { return std::ceil(x * 10000.0 - 1e-9) / 10000.0; }

double FeeModel::fee(double price, double count, bool taker) const {
  if (count <= 0 || price <= 0 || price >= 1) return 0.0;
  double rate = taker ? 0.07 : maker_rate();
  double raw = multiplier * rate * count * price * (1.0 - price);
  return ceil_centicent(raw);
}

double kelly_fraction(double p_win, double cost) {
  if (cost <= 0 || cost >= 1 || p_win <= cost) return 0.0;
  return (p_win - cost) / (1.0 - cost);
}

double size_contracts(double bankroll, double p_win, double cost, double kelly_frac, double cap_frac) {
  double f = std::min(kelly_frac * kelly_fraction(p_win, cost), cap_frac);
  if (f <= 0 || cost <= 0) return 0;
  return std::floor(bankroll * f / cost);
}

}  // namespace desk
