#include "deskd/strat/terminal_price.h"

#include <algorithm>
#include <cmath>
#include <limits>


namespace deskd {
namespace {

constexpr double kYearSeconds = 365.25 * 86400.0;
double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
double round6(double x) { return std::round(x * 1e6) / 1e6; }

// One point of the sampled grid. `fresh` says an observation actually landed in this point's own
// interval; everything else is the previous price carried forward. The difference is the whole
// estimator: a forward-filled point is not a new observation, and treating it as one reads a
// silent feed as a market that has stopped moving.
struct GridPoint {
  long long ts = 0;
  double price = 0;
  bool fresh = false;
};

// The sampled grid: the last observation at or before each grid point, oldest first. Sampling
// rather than differencing raw ticks keeps the estimate from reading the feed's own update rate
// as volatility.
std::vector<GridPoint> grid(const std::vector<std::pair<long long, double>>& ticks, long long now,
                            long long from, int step) {
  std::vector<GridPoint> out;
  if (step <= 0 || ticks.empty()) return out;
  size_t i = 0;
  double last = 0;
  bool have = false;
  for (long long t = from - (from % step); t <= now; t += step) {
    bool fresh = false;
    while (i < ticks.size() && ticks[i].first <= t) {
      if (ticks[i].second > 0) { last = ticks[i].second; have = true; fresh = true; }
      ++i;
    }
    if (have && t >= from) out.push_back(GridPoint{t, last, fresh});
  }
  return out;
}

// Realized volatility per sqrt(second) over one window of the sampled grid. Returns are taken
// between observations that really happened, over the time that really elapsed between them.
//
// `max_gap` is the longest silence a return may span before it is discarded, and it exists
// because a move observed across a long silence has an unknown duration: we know the value at two
// instants and not when it changed. See VolSettings::GapPolicy for what that choice costs in
// either direction; zero means include every interval.
double realized(const std::vector<GridPoint>& g, long long now, long long window, long long max_gap,
                int* n_out = nullptr) {
  double sum2 = 0;
  long long span = 0;
  int n = 0;
  long long prev_ts = 0;
  double prev_px = 0;
  for (size_t i = 0; i < g.size(); ++i) {
    if (!g[i].fresh) continue;
    const long long ts = g[i].ts;
    const double px = g[i].price;
    if (prev_ts > 0 && ts >= now - window && px > 0 && prev_px > 0) {
      const long long dt = ts - prev_ts;
      if (dt > 0 && (max_gap <= 0 || dt <= max_gap)) {
        const double r = std::log(px / prev_px);
        sum2 += r * r;
        span += dt;
        ++n;
      }
    }
    prev_ts = ts;
    prev_px = px;
  }
  if (n_out) *n_out = n;
  if (n < 2 || span <= 0) return 0;
  return std::sqrt(sum2 / static_cast<double>(span));
}

}  // namespace

// ---- payoff --------------------------------------------------------------------------------
bool TerminalPayoff::pays(double terminal) const {
  switch (type) {
    case Above: return lower_inclusive ? terminal >= lower : terminal > lower;
    case Below: return upper_inclusive ? terminal <= upper : terminal < upper;
    case Range: {
      const bool ok_lo = lower_inclusive ? terminal >= lower : terminal > lower;
      const bool ok_hi = upper_inclusive ? terminal <= upper : terminal < upper;
      return ok_lo && ok_hi;
    }
  }
  return false;
}

json TerminalPayoff::to_json() const {
  json j{{"type", type == Above ? "above" : type == Below ? "below" : "range"},
         {"strike_type", strike_type},
         {"lower_inclusive", lower_inclusive},
         {"upper_inclusive", upper_inclusive}};
  if (type != Below) j["lower"] = lower;
  if (type != Above) j["upper"] = upper;
  return j;
}

std::optional<TerminalPayoff> terminal_payoff(const std::string& strike_type,
                                              std::optional<double> floor_strike,
                                              std::optional<double> cap_strike) {
  TerminalPayoff p;
  p.strike_type = strike_type;
  auto real = [](const std::optional<double>& v) { return v && std::isfinite(*v); };
  // A threshold at or below zero on a commodity is not a contract anyone wrote, it is a strike
  // field that arrived empty. The distribution would answer it correctly - everything is above
  // zero - and that confident 1.0 is exactly the number you do not want on a bad row, so this
  // model declines to price it and something else can have a go.
  auto priced = [&](const std::optional<double>& v) { return real(v) && *v > 0; };
  // The exchange's own strike types. Anything else (structured, functional, custom) is not a
  // terminal-price question and this model does not answer it.
  if (strike_type == "greater" || strike_type == "greater_or_equal") {
    if (!priced(floor_strike)) return std::nullopt;
    p.type = TerminalPayoff::Above;
    p.lower = *floor_strike;
    p.lower_inclusive = strike_type == "greater_or_equal";
    return p;
  }
  if (strike_type == "less" || strike_type == "less_or_equal") {
    if (!priced(cap_strike)) return std::nullopt;
    p.type = TerminalPayoff::Below;
    p.upper = *cap_strike;
    p.upper_inclusive = strike_type == "less_or_equal";
    return p;
  }
  if (strike_type == "between") {
    // The bottom bracket of a ladder legitimately has a floor of zero: it is "at or below the
    // cap" and p_between reads it that way. The cap still has to be a real price.
    if (!real(floor_strike) || !priced(cap_strike) || *cap_strike < *floor_strike) return std::nullopt;
    p.type = TerminalPayoff::Range;
    p.lower = *floor_strike;
    p.upper = *cap_strike;
    p.lower_inclusive = true;   // Kalshi's "between X and Y" includes both ends
    p.upper_inclusive = true;
    return p;
  }
  return std::nullopt;
}


// ---- distribution --------------------------------------------------------------------------
bool TerminalDistribution::usable() const {
  return spot > 0 && std::isfinite(spot) && sigma >= 0 && std::isfinite(sigma) && tau_seconds >= 0 &&
         std::isfinite(tau_seconds);
}
double TerminalDistribution::sd() const { return usable() ? sigma * std::sqrt(tau_seconds) : 0.0; }

double TerminalDistribution::z(double strike) const {
  const double s = sd();
  if (!(strike > 0) || !usable() || s <= 0) return 0.0;
  return (std::log(spot / strike) + mu * tau_seconds) / s;
}

double TerminalDistribution::p_below(double strike, bool inclusive) const {
  // A price can never be at or below zero, so a non-positive bound is not a near-certainty, it is
  // an impossibility - and the other way round in p_above. Saying 0.0 for both, which is what a
  // shared guard would do, prices a bracket with an open lower end at half its true probability.
  if (!usable() || !std::isfinite(strike)) return 0.0;
  if (strike <= 0) return 0.0;
  const double s = sd();
  // No time and no uncertainty left: the answer is the indicator, and the tie rule decides it.
  if (s <= 0) return (inclusive ? spot <= strike : spot < strike) ? 1.0 : 0.0;
  return norm_cdf(-z(strike));
}

double TerminalDistribution::p_above(double strike, bool inclusive) const {
  if (!usable() || !std::isfinite(strike)) return 0.0;
  if (strike <= 0) return 1.0;   // everything is above a non-positive bound
  const double s = sd();
  if (s <= 0) return (inclusive ? spot >= strike : spot > strike) ? 1.0 : 0.0;
  return norm_cdf(z(strike));
}

double TerminalDistribution::p_between(double lo, double hi, bool lo_inclusive, bool hi_inclusive) const {
  if (!usable() || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) return 0.0;
  // The bottom bracket of a ladder is written with a floor of zero. It has no lower end: every
  // price is above it, so the bracket is exactly "at or below the cap".
  if (lo <= 0) return p_below(hi, hi_inclusive);
  const double s = sd();
  if (s <= 0) {
    const bool ok_lo = lo_inclusive ? spot >= lo : spot > lo;
    const bool ok_hi = hi_inclusive ? spot <= hi : spot < hi;
    return ok_lo && ok_hi ? 1.0 : 0.0;
  }
  const double p = norm_cdf(-z(hi)) - norm_cdf(-z(lo));
  return std::max(0.0, std::min(1.0, p));
}

double TerminalDistribution::probability(const TerminalPayoff& payoff) const {
  switch (payoff.type) {
    case TerminalPayoff::Above: return p_above(payoff.lower, payoff.lower_inclusive);
    case TerminalPayoff::Below: return p_below(payoff.upper, payoff.upper_inclusive);
    case TerminalPayoff::Range:
      return p_between(payoff.lower, payoff.upper, payoff.lower_inclusive, payoff.upper_inclusive);
  }
  return 0.0;
}

json TerminalDistribution::to_json() const {
  return json{{"spot", spot},
              {"sigma_per_sqrt_second", sigma},
              {"sigma_annualized", round6(sigma * std::sqrt(kYearSeconds))},
              {"tau_seconds", tau_seconds},
              {"sd_log", round6(sd())},
              {"mu_per_second", mu}};
}

// ---- volatility ----------------------------------------------------------------------------
double VolEstimate::annualized() const { return sigma * std::sqrt(kYearSeconds); }

json VolEstimate::to_json() const {
  json problems_json = json::array();
  for (const auto& p : problems) problems_json.push_back(p);
  return json{{"sigma_per_sqrt_second", sigma},
              {"sigma_annualized", round6(annualized())},
              {"ewma", ewma},
              {"rv_1m", rv_1m},
              {"rv_5m", rv_5m},
              {"rv_15m", rv_15m},
              {"rv_60m", rv_60m},
              {"samples", samples},
              {"span_seconds", span_seconds},
              {"last_observation_ts", last_ts},
              {"warm", warm},
              {"basis", basis},
              {"problems", problems_json}};
}

VolEstimate estimate_volatility(const std::vector<std::pair<long long, double>>& ticks, long long now,
                                const VolSettings& s) {
  VolEstimate v;
  if (ticks.empty()) {
    v.problems.push_back("no reference observations");
    return v;
  }
  for (const auto& t : ticks)
    if (t.first <= now && t.second > 0) v.last_ts = std::max(v.last_ts, t.first);
  const int step = std::max(1, s.sample_seconds);
  const auto g = grid(ticks, now, now - 3600, step);
  if (g.size() < 2) {
    v.problems.push_back("fewer than two sampled observations in the last hour");
    return v;
  }
  int n1 = 0, n5 = 0, n15 = 0, n60 = 0;
  const long long max_gap =
      s.long_gap_policy == VolSettings::GapPolicy::IncludeAll ? 0 : s.max_return_gap_seconds;
  v.rv_1m = realized(g, now, 60, max_gap, &n1);
  v.rv_5m = realized(g, now, 300, max_gap, &n5);
  v.rv_15m = realized(g, now, 900, max_gap, &n15);
  v.rv_60m = realized(g, now, 3600, max_gap, &n60);
  v.samples = n60;   // real observations differenced in the hour, not grid points
  v.span_seconds = g.back().ts - g.front().ts;
  // How much of that hour the feed was actually speaking for. A grid that spans an hour because
  // one tick arrived at each end is not an hour of observation.
  long long observed = 0, prev_obs = 0;
  for (const auto& pt : g) {
    if (!pt.fresh) continue;
    if (prev_obs > 0 && (max_gap <= 0 || pt.ts - prev_obs <= max_gap)) observed += pt.ts - prev_obs;
    prev_obs = pt.ts;
  }

  // EWMA of the squared return per second, halflife in seconds: responsive to a regime change
  // without a single tick setting the whole estimate.
  const double lambda = std::exp(-std::log(2.0) * step / std::max(1.0, s.ewma_halflife_seconds));
  double var = 0;
  bool seeded = false;
  long long prev_ts = 0;
  double prev_px = 0;
  for (const auto& pt : g) {
    if (!pt.fresh || !(pt.price > 0)) continue;
    if (prev_ts > 0) {
      const long long dt = pt.ts - prev_ts;
      if (dt > 0 && (max_gap <= 0 || dt <= max_gap) && prev_px > 0) {
        const double r = std::log(pt.price / prev_px);
        const double per_sec = (r * r) / static_cast<double>(dt);
        var = seeded ? lambda * var + (1 - lambda) * per_sec : per_sec;
        seeded = true;
      }
    }
    prev_ts = pt.ts;
    prev_px = pt.price;
  }
  v.ewma = seeded && var > 0 ? std::sqrt(var) : 0.0;

  // Conservative combination: the slower estimate, floored by the recent one. A quiet hour does
  // not license pricing a contract through a burst that is happening right now.
  double sigma = std::max(v.ewma, s.short_floor_multiplier * v.rv_5m);
  v.basis = sigma == v.ewma ? "ewma" : "5-minute realized";
  v.basis += s.long_gap_policy == VolSettings::GapPolicy::IncludeAll
                 ? ", every sampled interval differenced"
                 : ", returns spanning over " + std::to_string(s.max_return_gap_seconds) + "s of silence dropped";
  if (v.rv_60m > 0) {
    const double hi = s.max_ratio_to_hour * v.rv_60m, lo = s.min_ratio_to_hour * v.rv_60m;
    if (sigma > hi) { sigma = hi; v.basis += ", capped at " + std::to_string(s.max_ratio_to_hour) + "x the hourly estimate"; }
    if (sigma < lo) { sigma = lo; v.basis += ", floored at " + std::to_string(s.min_ratio_to_hour) + "x the hourly estimate"; }
  }
  v.sigma = sigma;

  if (v.samples < s.min_samples)
    v.problems.push_back("volatility warm-up: " + std::to_string(v.samples) + "/" +
                         std::to_string(s.min_samples) + " observed returns");
  if (observed < s.min_span_seconds)
    v.problems.push_back("only " + std::to_string(observed) + "s of the " +
                         std::to_string(v.span_seconds) + "s window carried observations we can "
                         "difference, under the " + std::to_string(s.min_span_seconds) + "s minimum");
  if (!(v.sigma > 0)) v.problems.push_back("no volatility estimate yet");
  v.warm = v.problems.empty();
  return v;
}

std::optional<double> minute_close(const std::vector<std::pair<long long, double>>& ticks,
                                   long long minute_end) {
  std::optional<double> out;
  for (const auto& [ts, price] : ticks) {
    if (price <= 0) continue;
    if (ts >= minute_end) break;          // ticks are oldest first
    if (ts >= minute_end - 60) out = price;
  }
  return out;
}

}  // namespace deskd
