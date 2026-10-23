#pragma once
// The basketball win-probability models (plan section 5(f); docs/wcbb_model.md), one loader for
// every league: P(the market's YES team wins) from the game state and the pregame Kalshi price, a
// logistic regression that tools/train_bball.py fits (tools/train_wcbb.py is it with --league
// wcbb) and writes as models/{wcbb,nba}_logit_vN.json. strat/wcbb_model.h is this with the
// women's college league.
//
// The leagues (bball_league):
//
//   key   model       series        regulation        in overtime
//   wcbb  wcbb_logit  KXNCAAWBGAME  4 x 600 s = 2400  t_left_s 0, as its model was trained
//   nba   nba_logit   KXNBAGAME     4 x 720 s = 2880  t_left_s the overtime clock (at most 300 s)
//
// load_bball_model refuses a file unless every one of its golden rows reproduces here to 1e-6: the
// features below must be the trainer's, exactly, or the fair would be a different model's. It also
// refuses a model name that is not a league's (or not the league asked for), a league or
// regulation_s that is not its model's league's, a feature name outside the shared vocabulary,
// mismatched array lengths, a zero scale, and a file without 10 golden rows. A file without the
// league and regulation_s keys (one written before the NBA's) takes them from its model name.
#include <optional>
#include <string>
#include <vector>

namespace deskd {

struct EspnRef;

struct BballLeague {
  std::string key, model, series;   // "nba", "nba_logit", "KXNBAGAME"
  double regulation_s = 0, period_s = 0, ot_s = 0;
  int periods = 0;
  bool ot_uses_clock = false;       // overtime's t_left_s: its clock (NBA), or 0 (WCBB)
};
// "wcbb" or "nba"; nullptr for any other key
const BballLeague* bball_league(const std::string& key);
// the league of a game market (KXNCAAWBGAME-..., KXNBAGAME-...); nullptr for any other
const BballLeague* bball_league_for_ticker(const std::string& ticker);

struct BballState {
  double t_left_s = 0;    // seconds left in regulation; in overtime, the league's (0, or the overtime clock)
  double margin = 0;      // the YES team's score minus the opponent's
  int possession = 0;     // +1 the YES team has the ball, -1 the opponent, 0 unknown
  double p0 = 0.5;        // the pregame YES probability (clamped to [0.01, 0.99])
  bool ot = false;        // the game is in overtime
};

struct BballModel {
  std::string league = "wcbb";   // a model without one is women's college (the only league before the NBA's)
  double regulation_s = 2400;    // the league's: features read tau = t_left_s / regulation_s
  int version = 0;
  std::string trained, path;
  std::string sha256;            // sha256_hex of the file's text (strat/lane_gate.h): what a gate file names
  std::vector<std::string> features;
  std::vector<double> mean, scale, coef;
  double intercept = 0;
  double brier_model = -1, brier_mid = -1;   // held-out Brier scores (-1: not given)
  int n_games = 0;
};

// one feature's value (docs/wcbb_model.md); nullopt for a name outside the vocabulary
std::optional<double> bball_feature(const std::string& name, const BballState& s, double regulation_s);
double bball_p(const BballModel& m, const BballState& s);
// league: the league the caller trades ("" takes any league's file)
std::optional<BballModel> parse_bball_model(const std::string& text, const std::string& league, std::string* why);
std::optional<BballModel> load_bball_model(const std::string& path, const std::string& league, std::string* why);

// The model's clock from ESPN's period and the period's clock in seconds: in regulation (periods 1
// to 4, a clock of at most one period) the seconds left in it; after it, overtime, with t_left_s
// the overtime clock (NBA, at most 300 s) or 0 (WCBB, any clock of at most one period). nullopt
// when the clock was not read (-1), the period is 0, or the clock is longer than its period.
struct GameClock {
  double t_left_s = 0;
  bool ot = false;
};
std::optional<GameClock> bball_t_left(const BballLeague& lg, int period, double clock_s);
// The fair on a live game's ladder: the model's p while ESPN's state is "in", its clock was read
// and the game's pregame price was captured; nullopt before tip, at the final and without p0. The
// scoreboard does not say who has the ball, so possession is 0 (unknown).
std::optional<double> bball_live_p(const BballModel& m, const std::string& state, int period, double clock_s,
                                   double margin, std::optional<double> p0);
std::optional<double> bball_live_p(const BballModel& m, const EspnRef& r, std::optional<double> p0);

}  // namespace deskd
