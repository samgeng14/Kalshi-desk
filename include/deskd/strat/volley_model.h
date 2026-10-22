#pragma once
// The women's college volleyball model (plan 5(d): "a Markov point model (volleyball)"): P(the
// market's YES team wins the match) from the sets and points and the pregame Kalshi price, a
// rally-point Markov chain with one parameter. tools/volleyball.py is the same model in Python
// and says it exactly; tests/deskd/fixtures/volleyball_golden.json is its golden file
// (tools/volleyball.py --write), and this reproduces every row of it to 1e-9.
//
// The YES team wins every rally with the same probability q, whoever serves. A set goes to set_to
// points (the deciding set, the best_of-th, to last_set_to) and must be won by two: from a tie at
// target - 1 all or later it is a deuce, which the YES team wins with
//
//   D = q^2 / (q^2 + (1 - q)^2)
//
// (from one point up, q + (1 - q) D; from one point down, q D). Below the deuce every rally is
// q x P(one more point) + (1 - q) x P(one fewer). The match is best_of sets, so P(win) is the
// current set's P from its points, then each later set's P from 0-0. q is solved from the
// pregame price: p0 clamped to [0.02, 0.98], then halving [0.05, 0.95] (mid = (lo + hi) / 2;
// below p0 the mid becomes lo, else hi) until no double lies strictly between lo and hi; q is the
// last (lo + hi) / 2. The in-game fair is the match P from the score at that q.
//
// A model file (models/volley_markov_v1.json, the golden file's format):
//
//   {"model": "volley_markov", "version": 1,
//    "rules": {"best_of": 5, "set_to": 25, "last_set_to": 15, "win_by": 2},
//    "golden": [{"in": {"sets_yes": 1, "sets_opp": 0, "pts_yes": 20, "pts_opp": 22, "p0": 0.62},
//                "p": 0.674...}, ...]}
//
// load_volley_model refuses a file unless it is model volley_markov version 1, its rules are the
// Python's (whole numbers of at least 1, best_of odd, win_by 2: the only one the closed-form
// deuce covers) and every one of its at least 10 golden rows reproduces here to 1e-9. It keeps
// the sha256 of the file's text, which a gate file names.
#include <optional>
#include <string>

namespace deskd {

struct EspnRef;

struct VolleyRules {
  int best_of = 5, set_to = 25, last_set_to = 15, win_by = 2;
};

// From the YES team's side: sets won, and the points of the set being played. points_known is
// false when the set's points were not read (its sets are right, its points are taken as 0-0).
struct VolleyState {
  int sets_yes = 0, sets_opp = 0, pts_yes = 0, pts_opp = 0;
  bool points_known = false;
};

// P(the YES team wins the set from a-b to target) when it wins each rally with q; NaN for win_by
// other than 2, a negative score, a target under 1 or a q outside [0, 1]
double volley_set_p(double q, int a, int b, int target, int win_by = 2);
// P(the YES team wins the match from s): 1 with best_of / 2 + 1 sets won, 0 with that many lost;
// NaN for rules the model does not cover
double volley_match_p(double q, const VolleyState& s, const VolleyRules& r = {});
// the rally probability whose match P from 0-0 is p0 (clamped to [0.02, 0.98]); nullopt for a p0
// that is not a number, or one outside the match P of [0.05, 0.95] (never, for these rules)
std::optional<double> volley_q_for_p0(double p0, const VolleyRules& r = {});

struct VolleyModel {
  int version = 1;
  VolleyRules rules;
  std::string path;
  std::string sha256;   // sha256_hex of the file's text (strat/lane_gate.h): what a gate file names
};
std::optional<VolleyModel> parse_volley_model(const std::string& text, std::string* why);
std::optional<VolleyModel> load_volley_model(const std::string& path, std::string* why);

// The fair: the match P from s at the q of p0; nullopt for a negative state or no q
std::optional<double> volley_live_p(const VolleyModel& m, const VolleyState& s, double p0);

// The match state from ESPN's scoreboard (ref/espn.h, the YES team's side): its period is the set
// being played and its scores are sets won (TO CONFIRM on a capture, as the linescores' live
// points are). The sets before the current one come from the linescores and must each have been
// won (to the set's target, by two, and ended there: 25-23 or 26-24, never 27-24); the score must
// count them, or them and a current set already won. A current set that is already won counts,
// and the next set is 0-0 (between sets). The current set's points come from its linescore entry;
// without one (or with -1, unread) points_known is false. Entries after the current set must be 0
// or unread. Anything else is no state: nullopt and why.
std::optional<VolleyState> volley_state(const EspnRef& r, const VolleyRules& rules, std::string* why);

}  // namespace deskd
