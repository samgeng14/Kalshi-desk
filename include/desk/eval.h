#pragma once
// The $100 -> $1,000 gate, computed from the store.
#include <vector>

#include "desk/config.h"
#include "desk/store.h"

namespace desk {

struct BootCI {
  double mean = 0, lo = 0, hi = 0;
  int n = 0;
};
BootCI bootstrap_mean(const std::vector<double>& x, int reps = 2000, double alpha = 0.10, unsigned seed = 7);

json build_report(Store& store, const Config& cfg);
// Realized / unrealized / fees, by strategy and category, today and inception.
json build_pnl(Store& store, const std::string& day_tz = "UTC");

}  // namespace desk
