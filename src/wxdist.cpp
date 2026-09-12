#include "desk/wxdist.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>

namespace desk {

double MaxDistribution::mean() const {
  double m = 0;
  for (size_t i = 0; i < pmf.size(); ++i) m += (lo + static_cast<int>(i)) * pmf[i];
  return m;
}

double MaxDistribution::sd() const {
  double m = mean(), v = 0;
  for (size_t i = 0; i < pmf.size(); ++i) {
    double d = (lo + static_cast<int>(i)) - m;
    v += d * d * pmf[i];
  }
  return std::sqrt(std::max(0.0, v));
}

double MaxDistribution::p_at_or_below(int t) const {
  double s = 0;
  for (size_t i = 0; i < pmf.size(); ++i)
    if (lo + static_cast<int>(i) <= t) s += pmf[i];
  return s;
}

double MaxDistribution::p_between(int a, int b) const {
  double s = 0;
  for (size_t i = 0; i < pmf.size(); ++i) {
    int v = lo + static_cast<int>(i);
    if (v >= a && v <= b) s += pmf[i];
  }
  return s;
}

double MaxDistribution::p_greater(int t) const { return 1.0 - p_at_or_below(t); }
double MaxDistribution::p_less(int t) const { return p_at_or_below(t - 1); }

MaxDistribution simulate_extreme(const TrajectoryInputs& in, int draws, unsigned seed) {
  MaxDistribution out;
  bool have_obs = in.running_max > -900;
  if (in.hourly.empty() && !have_obs) return out;

  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  const double sign = in.low ? -1.0 : 1.0;   // a minimum is the maximum of the negated series
  std::map<int, double> counts;

  // The shift is a mixture, not a constant: drawing from it inside the loop keeps its uncertainty
  // in the answer instead of smearing the whole distribution by a single average.
  double shift_total = 0;
  for (const auto& s : in.settle_shift) shift_total += s.second;

  for (int d = 0; d < draws; ++d) {
    // One bias for the whole day. This is the dominant term - a forecast that is two degrees warm
    // at noon is usually warm at three as well - and treating each hour as independent is what
    // makes a simulated day's range far too narrow.
    double bias = in.sigma_day * z(rng);
    double e = 0;
    bool first = true;
    double extreme = have_obs ? sign * in.running_max : -1e18;
    for (double f : in.hourly) {
      // AR(1) wiggle around the day's bias.
      e = first ? in.sigma_hour * z(rng)
                : in.rho * e + std::sqrt(std::max(0.0, 1 - in.rho * in.rho)) * in.sigma_hour * z(rng);
      first = false;
      extreme = std::max(extreme, sign * (f + bias) + e);
    }
    if (extreme < -1e17) continue;
    double settled = sign * extreme;      // back to real degrees

    // Apply the station's settlement shift, drawn from its own mixture.
    if (shift_total > 0) {
      double u = std::uniform_real_distribution<double>(0.0, shift_total)(rng), acc = 0;
      for (const auto& s : in.settle_shift) {
        acc += s.second;
        if (u <= acc) { settled += sign * s.first; break; }
      }
    }
    counts[static_cast<int>(std::lround(settled))] += 1.0;
  }
  if (counts.empty()) return out;

  out.lo = counts.begin()->first;
  int hi = counts.rbegin()->first;
  out.pmf.assign(static_cast<size_t>(hi - out.lo + 1), 0.0);
  double total = 0;
  for (const auto& [v, n] : counts) total += n;
  for (const auto& [v, n] : counts) out.pmf[static_cast<size_t>(v - out.lo)] = n / total;
  return out;
}

}  // namespace desk
