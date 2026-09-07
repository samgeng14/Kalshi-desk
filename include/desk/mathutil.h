#pragma once
#include <cmath>
#include <initializer_list>
#include <utility>

namespace desk {

inline double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// P(lo <= X <= hi) for X ~ N(mu, sd). Use -inf / +inf for open ends.
inline double norm_prob_between(double lo, double hi, double mu, double sd) {
  if (sd <= 0) return (mu >= lo && mu <= hi) ? 1.0 : 0.0;
  double a = std::isinf(lo) ? 0.0 : norm_cdf((lo - mu) / sd);
  double b = std::isinf(hi) ? 1.0 : norm_cdf((hi - mu) / sd);
  return std::max(0.0, b - a);
}

// P(lo <= S_T <= hi) for lognormal S_T with S_0 = spot, zero drift, annual vol `vol`,
// time `tau_years`.
inline double lognormal_prob_between(double lo, double hi, double spot, double vol, double tau_years) {
  if (tau_years <= 0 || vol <= 0) return (spot >= lo && spot <= hi) ? 1.0 : 0.0;
  double sd = vol * std::sqrt(tau_years);
  double mu = std::log(spot) - 0.5 * sd * sd;
  double a = lo <= 0 ? 0.0 : norm_cdf((std::log(lo) - mu) / sd);
  double b = std::isinf(hi) ? 1.0 : norm_cdf((std::log(hi) - mu) / sd);
  return std::max(0.0, b - a);
}

}  // namespace desk

// ---- soccer: independent Poisson goals ------------------------------------------------
namespace desk {

inline double poisson_pmf(int k, double lam) {
  if (lam <= 0) return k == 0 ? 1.0 : 0.0;
  double p = std::exp(-lam);
  for (int i = 1; i <= k; ++i) p *= lam / i;
  return p;
}

struct MatchProbs {
  double home = 0, draw = 0, away = 0;
};

// Remaining goals X~Pois(lh), Y~Pois(la); current score difference d = home - away.
// Final: home wins if X - Y > -d, draw if X - Y == -d, away wins if X - Y < -d.
// rho: Dixon-Coles low-score correction for FULL-match scorelines (use with d = 0 only;
// typical value -0.13). Zero = independent Poisson.
inline MatchProbs match_probs(double lh, double la, int d = 0, double rho = 0.0, int max_goals = 14) {
  MatchProbs r;
  for (int x = 0; x <= max_goals; ++x) {
    double px = poisson_pmf(x, lh);
    for (int y = 0; y <= max_goals; ++y) {
      double p = px * poisson_pmf(y, la);
      if (rho != 0.0 && d == 0) {
        if (x == 0 && y == 0) p *= 1.0 - lh * la * rho;
        else if (x == 0 && y == 1) p *= 1.0 + lh * rho;
        else if (x == 1 && y == 0) p *= 1.0 + la * rho;
        else if (x == 1 && y == 1) p *= 1.0 - rho;
      }
      int diff = x - y + d;
      if (diff > 0) r.home += p;
      else if (diff == 0) r.draw += p;
      else r.away += p;
    }
  }
  double s = r.home + r.draw + r.away;
  if (s > 0) { r.home /= s; r.draw /= s; r.away /= s; }
  return r;
}

// Finds (lh, la) whose probabilities match (p_home, p_draw) for a match ALREADY IN PLAY: the
// score difference `d` and the remaining fraction `rem` are part of the question. Fitting a
// live market with the full-match version instead attributes the goals already scored to the
// scoring rates, and then the model applies those same goals again.
inline std::pair<double, double> fit_lambdas_inplay(double p_home, double p_draw, double rem, int d,
                                                    double rho = 0.0) {
  double best_h = 1.4, best_a = 1.1, best_err = 1e9;
  rem = std::max(0.02, rem);
  auto err = [&](double lh, double la) {
    MatchProbs m = match_probs(lh * rem, la * rem, d, d == 0 ? rho : 0.0);
    return (m.home - p_home) * (m.home - p_home) + (m.draw - p_draw) * (m.draw - p_draw);
  };
  for (double lh = 0.2; lh <= 5.0; lh += 0.1)
    for (double la = 0.2; la <= 5.0; la += 0.1) {
      double e = err(lh, la);
      if (e < best_err) { best_err = e; best_h = lh; best_a = la; }
    }
  for (double lh = std::max(0.05, best_h - 0.1); lh <= best_h + 0.1; lh += 0.01)
    for (double la = std::max(0.05, best_a - 0.1); la <= best_a + 0.1; la += 0.01) {
      double e = err(lh, la);
      if (e < best_err) { best_err = e; best_h = lh; best_a = la; }
    }
  return {best_h, best_a};
}

// Finds (lh, la) whose full-match Poisson probabilities match (p_home, p_draw).
// Coarse grid then local refinement; good to ~1e-3 in probability.
inline std::pair<double, double> fit_lambdas(double p_home, double p_draw, double rho = 0.0) {
  double best_h = 1.4, best_a = 1.1, best_err = 1e9;
  auto err = [&](double lh, double la) {
    MatchProbs m = match_probs(lh, la, 0, rho);
    return (m.home - p_home) * (m.home - p_home) + (m.draw - p_draw) * (m.draw - p_draw);
  };
  for (double lh = 0.3; lh <= 4.0; lh += 0.1)
    for (double la = 0.3; la <= 4.0; la += 0.1) {
      double e = err(lh, la);
      if (e < best_err) { best_err = e; best_h = lh; best_a = la; }
    }
  double step = 0.05;
  for (int it = 0; it < 6; ++it) {
    bool improved = false;
    for (double dh : {-step, 0.0, step})
      for (double da : {-step, 0.0, step}) {
        double lh = best_h + dh, la = best_a + da;
        if (lh < 0.05 || la < 0.05) continue;
        double e = err(lh, la);
        if (e < best_err) { best_err = e; best_h = lh; best_a = la; improved = true; }
      }
    if (!improved) step /= 2;
  }
  return {best_h, best_a};
}

}  // namespace desk
