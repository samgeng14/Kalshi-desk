// strat/volley_model: the volleyball fair (tools/volleyball.py in C++). The set and match
// probabilities worked by hand and against a second computation (rally by rally, no closed form);
// q from the pregame price; the golden file tools/volleyball.py wrote reproduces to 1e-9 and loads
// with the sha256 a gate file names, and each kind of bad file is refused; the match state from
// ESPN's linescores, between sets included, and each inconsistent one refused with its reason.
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/ref/espn.h"
#include "deskd/strat/lane_gate.h"
#include "deskd/strat/volley_model.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
double deuce(double q) { return q * q / (q * q + (1 - q) * (1 - q)); }
const std::string kGolden = std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/volleyball_golden.json";
std::string read(const std::string& path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// set_p again: the score's probabilities carried forward rally by rally (tools/volleyball.py's
// set_p_forward), the deuce cut after 600 rallies
double set_p_forward(double q, int a, int b, int target) {
  auto won = [&](int x, int y) { return x >= target && x - y >= 2; };
  if (won(a, b)) return 1;
  if (won(b, a)) return 0;
  std::map<std::pair<int, int>, double> dist{{{a, b}, 1.0}};
  double p = 0;
  for (int r = 0; r < 600 && !dist.empty(); ++r) {
    std::map<std::pair<int, int>, double> next;
    for (const auto& [s, w] : dist) {
      for (const auto& [n, pw] : {std::make_pair(std::make_pair(s.first + 1, s.second), q),
                                  std::make_pair(std::make_pair(s.first, s.second + 1), 1 - q)}) {
        if (won(n.first, n.second)) p += w * pw;
        else if (!won(n.second, n.first)) next[n] += w * pw;
      }
    }
    dist = std::move(next);
  }
  return p;
}

void sets() {
  const double qs[] = {0.3, 0.45, 0.5, 0.52, 0.55, 0.62, 0.8};
  for (int t : {15, 25})
    for (int a : {0, 7, 13, 14, 24, 30}) CHECK(std::fabs(volley_set_p(0.5, a, a, t) - 0.5) < 1e-15);
  for (double q : qs) {
    const double d = deuce(q);
    CHECK(volley_set_p(q, 24, 24, 25) == d && volley_set_p(q, 31, 31, 25) == d && volley_set_p(q, 14, 14, 15) == d);
    CHECK(volley_set_p(q, 26, 25, 25) == q + (1 - q) * d && volley_set_p(q, 25, 26, 25) == q * d);   // an advantage
    CHECK(volley_set_p(q, 25, 23, 25) == 1 && volley_set_p(q, 23, 25, 25) == 0 && volley_set_p(q, 27, 29, 25) == 0);
    CHECK(volley_set_p(q, 25, 0, 25) == 1 && volley_set_p(q, 3, 25, 25) == 0);
    // YES loses from 24-0 only by 24 straight rallies and then the deuce
    CHECK(std::fabs(volley_set_p(q, 24, 0, 25) - (1 - std::pow(1 - q, 24) * (1 - d))) < 1e-15);
    CHECK(std::fabs(volley_set_p(q, 0, 0, 2) - d) < 1e-15);   // a set to 2 by 2 is the deuce itself
  }
  double worst = 0;
  for (double q : qs)
    for (int t : {15, 25})
      for (const auto& [a, b] : std::vector<std::pair<int, int>>{{0, 0}, {3, 9}, {12, 12}, {20, 23}, {24, 23}, {14, 13}, {26, 26}})
        worst = std::max(worst, std::fabs(volley_set_p(q, a, b, t) - set_p_forward(q, a, b, t)));
  CHECK(worst < 1e-12);
  // only the 2-point win, a score and a q that are some set's
  CHECK(std::isnan(volley_set_p(0.5, 0, 0, 25, 3)) && std::isnan(volley_set_p(0.5, -1, 0, 25)) &&
        std::isnan(volley_set_p(1.5, 0, 0, 25)) && std::isnan(volley_set_p(0.5, 0, 0, 0)));
}

void matches() {
  const double qs[] = {0.3, 0.45, 0.5, 0.52, 0.55, 0.62, 0.8};
  auto st = [](int sy, int so, int a, int b) { return VolleyState{sy, so, a, b, true}; };
  for (double q : qs) {
    CHECK(std::fabs(volley_match_p(q, st(0, 0, 0, 0)) + volley_match_p(1 - q, st(0, 0, 0, 0)) - 1) < 1e-14);
    CHECK(std::fabs(volley_match_p(q, st(2, 1, 20, 17)) + volley_match_p(1 - q, st(1, 2, 17, 20)) - 1) < 1e-14);
    // the 5th set plays to 15: at 2-2 the match is that set
    for (const auto& [a, b] : std::vector<std::pair<int, int>>{{0, 0}, {13, 14}, {16, 15}}) CHECK(volley_match_p(q, st(2, 2, a, b)) == volley_set_p(q, a, b, 15));
    // from 0-0: one set's chances combined over a best of five, by hand
    const double s = volley_set_p(q, 0, 0, 25), f = volley_set_p(q, 0, 0, 15);
    const double three = s * s * s, four = 3 * s * s * s * (1 - s), five = 6 * s * s * (1 - s) * (1 - s) * f;
    CHECK(std::fabs(volley_match_p(q, st(0, 0, 0, 0)) - (three + four + five)) < 1e-14);
  }
  CHECK(volley_match_p(0.4, st(3, 1, 0, 0)) == 1 && volley_match_p(0.6, st(2, 3, 5, 0)) == 0);   // decided
  double prev = 0;   // monotone in q
  for (int i = 0; i <= 40; ++i) {
    const double p = volley_match_p(0.40 + 0.005 * i, st(0, 0, 0, 0));
    CHECK(p > prev);
    prev = p;
  }
  CHECK(volley_match_p(0.5, st(1, 1, 20, 10)) > volley_match_p(0.5, st(1, 1, 10, 10)) &&
        volley_match_p(0.5, st(1, 1, 10, 20)) < volley_match_p(0.5, st(1, 1, 10, 10)));
  CHECK(std::isnan(volley_match_p(0.5, st(0, 0, 0, 0), VolleyRules{4, 25, 15, 2})) &&
        std::isnan(volley_match_p(0.5, st(0, 0, 0, 0), VolleyRules{5, 25, 15, 3})));
}

void prices() {
  CHECK(volley_q_for_p0(0.5) == 0.5);
  for (double q : {0.44, 0.47, 0.5, 0.505, 0.53, 0.56})
    CHECK(std::fabs(*volley_q_for_p0(volley_match_p(q, VolleyState{})) - q) < 1e-12);
  CHECK(volley_q_for_p0(0.999) == volley_q_for_p0(0.98) && volley_q_for_p0(0.0) == volley_q_for_p0(0.02));
  CHECK(!volley_q_for_p0(std::nan("")) && !volley_q_for_p0(INFINITY));
  VolleyModel m;
  // before the first rally the fair is p0; YES at p0 and the other team at 1 - p0 sum to 1
  for (double p0 : {0.1, 0.35, 0.5, 0.8}) CHECK(std::fabs(*volley_live_p(m, VolleyState{}, p0) - p0) < 1e-12);
  for (double p0 : {0.2, 0.5, 0.7})
    CHECK(std::fabs(*volley_live_p(m, VolleyState{1, 0, 20, 22, true}, p0) + *volley_live_p(m, VolleyState{0, 1, 22, 20, true}, 1 - p0) - 1) <
          1e-12);
  CHECK(!volley_live_p(m, VolleyState{-1, 0, 0, 0, true}, 0.5) && !volley_live_p(m, VolleyState{}, std::nan("")));
}

// tools/volleyball.py --write's file: every row's inputs and P, which this reproduces to 1e-9
void golden() {
  const std::string text = read(kGolden);
  std::string why;
  const auto m = parse_volley_model(text, &why);
  CHECK(m.has_value());
  if (!m) { std::fprintf(stderr, "  %s\n", why.c_str()); return; }
  CHECK(m->sha256 == sha256_hex(text) && m->sha256.size() == 64);
  CHECK(m->rules.best_of == 5 && m->rules.set_to == 25 && m->rules.last_set_to == 15 && m->rules.win_by == 2);
  const auto j = json::parse(text);
  CHECK(j["golden"].size() == 10);
  double worst = 0;
  for (const auto& g : j["golden"]) {
    const auto& in = g["in"];
    const auto p = volley_live_p(*m, VolleyState{in["sets_yes"], in["sets_opp"], in["pts_yes"], in["pts_opp"], true}, in["p0"]);
    CHECK(p.has_value());
    if (p) worst = std::max(worst, std::fabs(*p - g["p"].get<double>()));
  }
  CHECK(worst <= 1e-9);
  const auto loaded = load_volley_model(kGolden, &why);
  CHECK(loaded && loaded->path == kGolden && loaded->sha256 == m->sha256);
}

void refusals() {
  const auto j = json::parse(read(kGolden));
  std::string why;
  auto refused = [&](const json& bad, const std::string& part) {
    const bool ok = !parse_volley_model(bad.dump(), &why) && has(why, part);
    if (!ok) std::fprintf(stderr, "  wanted a refusal naming \"%s\", got \"%s\"\n", part.c_str(), why.c_str());
    return ok;
  };
  auto bad = j;
  bad["golden"][6]["p"] = bad["golden"][6]["p"].get<double>() + 2e-9;
  CHECK(refused(bad, "golden row 6"));
  bad = j; bad["golden"][2]["p"] = bad["golden"][2]["p"].get<double>() + 5e-10;   // inside 1e-9: still the model
  CHECK(parse_volley_model(bad.dump(), &why).has_value());
  bad = j; bad["model"] = "volley_markov2";
  CHECK(refused(bad, "model is not"));
  bad = j; bad["version"] = 2;
  CHECK(refused(bad, "version 2"));
  bad = j; bad["rules"]["win_by"] = 3;
  CHECK(refused(bad, "win_by 3"));
  bad = j; bad["rules"]["best_of"] = 4;
  CHECK(refused(bad, "best_of odd"));
  bad = j; bad["rules"]["set_to"] = 25.0;
  CHECK(refused(bad, "whole numbers"));
  bad = j; bad.erase("rules");
  CHECK(refused(bad, "rules"));
  bad = j; bad["rules"]["set_to"] = 21;   // other rules: the rows are not theirs
  CHECK(refused(bad, "golden row"));
  bad = j; bad["golden"].erase(9);
  CHECK(refused(bad, "10 golden"));
  bad = j; bad["golden"][4]["in"]["pts_yes"] = 1.5;
  CHECK(refused(bad, "golden row 4 is not"));
  bad = j; bad["golden"][4]["in"]["sets_yes"] = -1;
  CHECK(refused(bad, "golden row 4 has no fair"));
  bad = j; bad["golden"][0].erase("p");
  CHECK(refused(bad, "golden row 0 is not"));
  CHECK(!parse_volley_model("[1, 2]", &why) && has(why, "not a JSON object"));
  CHECK(!load_volley_model("/nonexistent/volley.json", &why) && has(why, "cannot read"));
}

// ESPN's YES-side ref: the period is the set being played, the scores sets won, the linescores
// each set's points
EspnRef vb(int period, std::vector<int> yes, std::vector<int> opp, int yes_sets, int opp_sets) {
  EspnRef r;
  r.state = "in";
  r.period = period;
  r.yes_lines = std::move(yes);
  r.opp_lines = std::move(opp);
  r.yes_score = yes_sets;
  r.opp_score = opp_sets;
  return r;
}
bool is(const std::optional<VolleyState>& s, int sy, int so, int a, int b, bool known) {
  return s && s->sets_yes == sy && s->sets_opp == so && s->pts_yes == a && s->pts_opp == b && s->points_known == known;
}

void states() {
  const VolleyRules r;
  std::string why;
  // set 3 at 12-15, YES won the first two (25-20, 27-25)
  CHECK(is(volley_state(vb(3, {25, 27, 12}, {20, 25, 15}, 2, 0), r, &why), 2, 0, 12, 15, true));
  // the first set's opening rally; ESPN without the current set's entry yet: points not known
  CHECK(is(volley_state(vb(1, {0}, {0}, 0, 0), r, &why), 0, 0, 0, 0, true));
  CHECK(is(volley_state(vb(2, {23}, {25}, 0, 1), r, &why), 0, 1, 0, 0, false));
  CHECK(is(volley_state(vb(2, {23, -1}, {25, -1}, 0, 1), r, &why), 0, 1, 0, 0, false));
  // between sets: the current set is won, the score field before or after it, the next set 0-0
  CHECK(is(volley_state(vb(2, {23, 25}, {25, 21}, 0, 1), r, &why), 1, 1, 0, 0, true));
  CHECK(is(volley_state(vb(2, {23, 25}, {25, 21}, 1, 1), r, &why), 1, 1, 0, 0, true));
  // the 5th set goes to 15, and a deuce runs on; later entries at 0 are allowed
  CHECK(is(volley_state(vb(5, {25, 20, 25, 18, 16}, {22, 25, 19, 25, 16}, 2, 2), r, &why), 2, 2, 16, 16, true));
  CHECK(is(volley_state(vb(4, {25, 20, 25, 18, 0}, {22, 25, 19, 25, 0}, 2, 1), r, &why), 2, 2, 0, 0, true));
  // a match won in its last set is decided
  const auto won = volley_state(vb(3, {25, 25, 25}, {20, 20, 23}, 2, 0), r, &why);
  CHECK(is(won, 3, 0, 0, 0, true) && volley_match_p(0.3, *won, r) == 1);
  // inconsistent lines: no state, and why
  CHECK(!volley_state(vb(3, {25, 24, 3}, {20, 25, 1}, 1, 1), r, &why) && has(why, "set 2 has no winner at 24-25"));
  CHECK(!volley_state(vb(3, {25, 27, 3}, {20, 24, 1}, 2, 0), r, &why) && has(why, "set 2 has no winner at 27-24"));
  CHECK(!volley_state(vb(3, {25, 25, 3}, {20, 18, 1}, 1, 1), r, &why) && has(why, "the score 1-1"));
  CHECK(!volley_state(vb(2, {25, 28}, {20, 25}, 1, 0), r, &why) && has(why, "runs past its end"));
  CHECK(!volley_state(vb(2, {25, 3, 4}, {20, 1, 0}, 1, 0), r, &why) && has(why, "past set 2 have points"));
  CHECK(!volley_state(vb(3, {25}, {20}, 1, 0), r, &why) && has(why, "linescores for 1 sets"));
  CHECK(!volley_state(vb(2, {25, 3}, {20}, 1, 0), r, &why) && has(why, "differ in length"));
  CHECK(!volley_state(vb(3, {-1, 25, 0}, {-1, 20, 0}, 1, 0), r, &why) && has(why, "set 1's points were not read"));
  CHECK(!volley_state(vb(0, {}, {}, 0, 0), r, &why) && !volley_state(vb(6, {}, {}, 0, 0), r, &why));
  CHECK(!volley_state(vb(4, {25, 25, 25, 3}, {20, 20, 20, 1}, 3, 0), r, &why) && has(why, "decided before set 4"));
}

}  // namespace

int main() {
  sets();
  matches();
  prices();
  golden();
  refusals();
  states();
  return finish("deskd-volley");
}
