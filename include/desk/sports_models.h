#pragma once
#include "desk/strategy.h"
namespace desk {
// P(final total > line), conditional on current scoring and elapsed regulation.
// A half-point line has no push ambiguity. These are uncalibrated research baselines.
double soccer_total_over(double line,int goals,double elapsed,double prior_mean,double prior_sd);
double basketball_total_over(double line,int points,double elapsed,double prior_mean,double prior_sd,
                             double overtime_probability=0,double overtime_mean=0,double overtime_sd=0);
// P(regulation ends level), which is P(overtime), given the margin now and how much regulation is
// left. Overtime happens only when the regulation margin lands exactly on zero, so if the margin
// still to come is roughly N(0, margin_sd * sqrt(fraction_left)) this is that normal's density at
// the current margin times the width of one tie on the sport's scoring lattice. It replaces a
// constant P(overtime), which is wrong in the one situation that matters: measured on 2023-2026
// finals, a game tied entering the last regulation period reached overtime 12.6% of the time in
// the NBA against a 4.8% base rate, and 17.5% against 4.2% in college football. Overtime is also
// worth more than it looks, adding 24.1 points to an NBA total (1.2 regulation standard
// deviations) and 12.3 to a college football one, so a constant understates the upper tail by
// most exactly when the game is close and late.
// Fitted by maximum likelihood over every period boundary of 26,868 games. It calibrates well at
// small margins and understates overtime from large ones (NBA 7-10 points: 5.8% observed against
// 3.3% predicted), because a trailing team plays deliberately higher variance in a way a random
// walk does not capture. Treat the large-margin end as a floor.
double overtime_probability(double margin,double fraction_left,double margin_sd,double tie_width);
json sports_comeback_candidate(double price,double earlier_price,double independent_win_probability,
                              double fee_per,double fraction_left,bool state_fresh);
class SportsTotals : public Strategy {
 public:
  explicit SportsTotals(const json& params):params_(params){}
  std::string name() const override {return "sports_totals";}
  std::string category() const override {return "Sports";}
  bool covers(const Market& m,const Series&) const override;
  std::optional<Fair> fair(const Market& m,const Series&,Store& store) override;
  std::string describe() const override {return "Mapped soccer goals / basketball points totals; research baselines, no live auto promotion";}
 private: json params_;
};
}
