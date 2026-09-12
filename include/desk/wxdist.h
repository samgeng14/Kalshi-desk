#pragma once
// A distribution over the value Kalshi will settle on: the day's maximum temperature, as whole
// degrees Fahrenheit, after the sensor-to-climate-report shift.
//
// The old model put a normal on the day's maximum with its centre at the largest hourly point
// forecast. That understates the peak. The maximum of the expected hourly values is not the
// expected maximum: whenever several hours sit near the top of the day, whichever of them happens
// to run warm becomes the day's high, so E[max] exceeds max(E[hour]) and the gap widens with
// forecast uncertainty. Simulating the remaining trajectory and taking its maximum gets this
// right by construction, and yields a ladder whose probabilities sum to one because they are read
// off one distribution rather than computed bracket by bracket.
#include <utility>
#include <vector>

namespace desk {

struct MaxDistribution {
  int lo = 0;                 // the lowest integer carrying mass
  std::vector<double> pmf;    // pmf[i] = P(settled value == lo + i)

  bool empty() const { return pmf.empty(); }
  double mean() const;
  double sd() const;
  double p_at_or_below(int t) const;
  double p_between(int a, int b) const;   // inclusive, as Kalshi's bracket wording reads
  double p_greater(int t) const;          // strictly greater
  double p_less(int t) const;             // strictly less
};

struct TrajectoryInputs {
  double running_max = -999;              // highest observed so far today; -999 = nothing observed
  std::vector<double> hourly;             // point forecast for each remaining hour, in F
  double sigma_day = 2.5;                 // day-level forecast bias: shared by every hour
  double sigma_hour = 1.0;                // hour-to-hour wiggle around that
  double rho = 0.7;                       // persistence of the wiggle between adjacent hours
  // Settled value minus the maximum of the observation series, as measured per station in
  // docs/settlement.md. Applied as a mixture, so its uncertainty stays in the answer.
  std::vector<std::pair<int, double>> settle_shift;
  bool low = false;                       // a daily-minimum market: the extreme runs the other way
};

// Deterministic for a given seed, so a fair recomputed on the same inputs is the same number.
MaxDistribution simulate_extreme(const TrajectoryInputs& in, int draws = 20000, unsigned seed = 7);

}  // namespace desk
