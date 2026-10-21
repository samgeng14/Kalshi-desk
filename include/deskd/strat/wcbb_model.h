#pragma once
// The women's college basketball model (plan section 5(f); docs/wcbb_model.md): P(the market's
// YES team wins) from the game state and the pregame Kalshi price, a logistic regression that
// tools/train_wcbb.py fits and writes as models/wcbb_logit_vN.json.
//
// load_wcbb_model refuses a file unless every one of its golden rows reproduces here to 1e-6: the
// features below must be the trainer's, exactly, or the fair on the ladder would be a different
// model's. It also refuses a feature name outside the shared vocabulary, mismatched array
// lengths, a zero scale, and a file without 10 golden rows.
#include <optional>
#include <string>
#include <vector>

namespace deskd {

struct WcbbState {
  double t_left_s = 0;    // seconds left in regulation (2400 = 40 minutes); 0 in overtime
  double margin = 0;      // the YES team's score minus the opponent's
  int possession = 0;     // +1 the YES team has the ball, -1 the opponent, 0 unknown
  double p0 = 0.5;        // the pregame YES probability (clamped to [0.01, 0.99])
};

struct WcbbModel {
  int version = 0;
  std::string trained, path;
  std::vector<std::string> features;
  std::vector<double> mean, scale, coef;
  double intercept = 0;
  double brier_model = -1, brier_mid = -1;   // held-out Brier scores (-1: not given)
  int n_games = 0;
};

// one feature's value (docs/wcbb_model.md); nullopt for a name outside the vocabulary
std::optional<double> wcbb_feature(const std::string& name, const WcbbState& s);
double wcbb_p(const WcbbModel& m, const WcbbState& s);
std::optional<WcbbModel> load_wcbb_model(const std::string& path, std::string* why);
std::optional<WcbbModel> parse_wcbb_model(const std::string& text, std::string* why);
// Regulation seconds left in a women's college game (4 quarters of 10 minutes) from ESPN's period
// and the period's clock; 0 in overtime; nullopt when the clock was not read
std::optional<double> wcbb_t_left(int period, double clock_s);
// The fair on a live game's ladder: the model's p while ESPN's state is "in", its clock was read
// and the game's pregame price was captured; nullopt before tip, at the final and without p0. The
// scoreboard does not say who has the ball, so possession is 0 (unknown).
std::optional<double> wcbb_live_p(const WcbbModel& m, const std::string& state, int period, double clock_s, double margin,
                                  std::optional<double> p0);

}  // namespace deskd
