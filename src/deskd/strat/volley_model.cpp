#include "deskd/strat/volley_model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "deskd/ref/espn.h"
#include "deskd/strat/lane_gate.h"

namespace deskd {

namespace {
// tools/volleyball.py's check_rules
bool rules_ok(const VolleyRules& r) {
  return r.win_by == 2 && r.best_of >= 1 && r.best_of % 2 == 1 && r.set_to >= 1 && r.last_set_to >= 1;
}
int sets_to_win(const VolleyRules& r) { return (r.best_of + 1) / 2; }
// the target of set number k (1-based): set_target
int set_target(const VolleyRules& r, int k) { return k == r.best_of ? r.last_set_to : r.set_to; }
// the set ended at w-l, won by w: the target reached two clear, and no later than that
bool won_at(int w, int l, int target, int win_by) { return w >= target && w - l >= win_by && (w == target || w - l == win_by); }
bool past(int w, int l, int target, int win_by) { return w >= target && w - l >= win_by; }
std::string score(int a, int b) { return std::to_string(a) + "-" + std::to_string(b); }
}  // namespace

double volley_set_p(double q, int a, int b, int target, int win_by) {
  if (win_by != 2 || !(q >= 0 && q <= 1) || a < 0 || b < 0 || target < 1) return std::nan("");
  const double d = q * q / (q * q + (1 - q) * (1 - q));
  // set_p's p(x, y), in its order: the deuce zone, then a side at the target, then one rally
  auto end = [&](int x, int y, double* v) {
    if (x >= target - 1 && y >= target - 1) {
      const int lead = x - y;
      *v = lead >= 2 ? 1 : lead <= -2 ? 0 : lead == 0 ? d : lead == 1 ? q + (1 - q) * d : q * d;
      return true;
    }
    if (x >= target) { *v = 1; return true; }
    if (y >= target) { *v = 0; return true; }
    return false;
  };
  double v;
  if (end(a, b, &v)) return v;
  // below both targets: f(i, j) from (target, target) down to (a, b)
  const int n = target + 1;
  std::vector<double> f(static_cast<std::size_t>(n) * n);
  for (int i = target; i >= a; --i)
    for (int j = target; j >= b; --j)
      if (!end(i, j, &f[i * n + j])) f[i * n + j] = q * f[(i + 1) * n + j] + (1 - q) * f[i * n + j + 1];
  return f[a * n + b];
}

double volley_match_p(double q, const VolleyState& s, const VolleyRules& r) {
  if (!rules_ok(r)) return std::nan("");
  const int need = sets_to_win(r);
  if (s.sets_yes >= need) return 1;
  if (s.sets_opp >= need) return 0;
  // m(i, j): P(win) from i-j in sets with the next set at 0-0
  const double first = volley_set_p(q, 0, 0, r.set_to, r.win_by), last = volley_set_p(q, 0, 0, r.last_set_to, r.win_by);
  const int n = need + 1;
  std::vector<double> m(static_cast<std::size_t>(n) * n);
  for (int i = need; i >= 0; --i)
    for (int j = need; j >= 0; --j) {
      if (i >= need || j >= need) { m[i * n + j] = i >= need ? 1 : 0; continue; }
      const double sp = i + j + 1 == r.best_of ? last : first;
      m[i * n + j] = sp * m[(i + 1) * n + j] + (1 - sp) * m[i * n + j + 1];
    }
  const double cur = volley_set_p(q, s.pts_yes, s.pts_opp, set_target(r, s.sets_yes + s.sets_opp + 1), r.win_by);
  return cur * m[(s.sets_yes + 1) * n + s.sets_opp] + (1 - cur) * m[s.sets_yes * n + s.sets_opp + 1];
}

std::optional<double> volley_q_for_p0(double p0, const VolleyRules& r) {
  if (!std::isfinite(p0) || !rules_ok(r)) return std::nullopt;
  const double p = std::clamp(p0, 0.02, 0.98);
  double lo = 0.05, hi = 0.95;
  if (!(volley_match_p(lo, VolleyState{}, r) <= p && p <= volley_match_p(hi, VolleyState{}, r))) return std::nullopt;
  while (true) {   // until no double lies strictly between lo and hi
    const double mid = (lo + hi) / 2;
    if (mid == lo || mid == hi) break;
    if (volley_match_p(mid, VolleyState{}, r) < p) lo = mid;
    else hi = mid;
  }
  return (lo + hi) / 2;
}

std::optional<double> volley_live_p(const VolleyModel& m, const VolleyState& s, double p0) {
  if (s.sets_yes < 0 || s.sets_opp < 0 || s.pts_yes < 0 || s.pts_opp < 0) return std::nullopt;
  const auto q = volley_q_for_p0(p0, m.rules);
  if (!q) return std::nullopt;
  return volley_match_p(*q, s, m.rules);
}

std::optional<VolleyModel> parse_volley_model(const std::string& text, std::string* why) {
  using json = nlohmann::json;
  auto fail = [&](const std::string& w) { if (why) *why = w; return std::optional<VolleyModel>(); };
  // a whole number, as Python's int (never a bool or 2.0)
  auto whole = [](const json& o, const char* k, int* v) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    *v = o[k].get<int>();
    return true;
  };
  const auto j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  if (!j.contains("model") || j["model"] != "volley_markov") return fail("model is not \"volley_markov\"");
  if (!j.contains("version") || j["version"] != 1)
    return fail("version " + (j.contains("version") ? j["version"].dump() : std::string("(none)")) + " is not 1");
  VolleyModel m;
  const auto& rules = j.contains("rules") ? j["rules"] : json();
  auto& r = m.rules;
  if (!rules.is_object() || !whole(rules, "best_of", &r.best_of) || !whole(rules, "set_to", &r.set_to) ||
      !whole(rules, "last_set_to", &r.last_set_to) || !whole(rules, "win_by", &r.win_by))
    return fail("rules are not {best_of, set_to, last_set_to, win_by} as whole numbers");
  if (r.win_by != 2) return fail("win_by " + std::to_string(r.win_by) + " is not 2 (the deuce's closed form is for 2)");
  if (!rules_ok(r)) return fail("rules " + rules.dump() + ": best_of odd, set_to and last_set_to whole and positive");
  const auto& golden = j.contains("golden") ? j["golden"] : json();
  if (!golden.is_array() || golden.size() < 10) return fail("fewer than 10 golden rows");
  for (std::size_t i = 0; i < golden.size(); ++i) {
    const auto& g = golden[i];
    const auto& in = g.is_object() && g.contains("in") ? g["in"] : json();
    VolleyState s;
    if (!in.is_object() || !whole(in, "sets_yes", &s.sets_yes) || !whole(in, "sets_opp", &s.sets_opp) ||
        !whole(in, "pts_yes", &s.pts_yes) || !whole(in, "pts_opp", &s.pts_opp) || !in.contains("p0") ||
        !in["p0"].is_number() || !g.contains("p") || !g["p"].is_number())
      return fail("golden row " + std::to_string(i) + " is not {\"in\": {sets_yes, sets_opp, pts_yes, pts_opp, p0}, \"p\"}");
    const double want = g["p"].get<double>();
    const auto got = volley_live_p(m, s, in["p0"].get<double>());
    std::ostringstream o;
    if (!got) o << "golden row " << i << " has no fair here";
    else if (!(std::fabs(*got - want) <= 1e-9)) o << "golden row " << i << " gives " << *got << ", the file says " << want;
    if (!o.str().empty()) return fail(o.str() + ": this is not tools/volleyball.py's model");
  }
  m.sha256 = sha256_hex(text);
  return m;
}

std::optional<VolleyModel> load_volley_model(const std::string& path, std::string* why) {
  std::ifstream f(path);
  if (!f) { if (why) *why = "cannot read " + path; return std::nullopt; }
  std::stringstream s;
  s << f.rdbuf();
  auto m = parse_volley_model(s.str(), why);
  if (m) m->path = path;
  else if (why) *why = path + ": " + *why;
  return m;
}

std::optional<VolleyState> volley_state(const EspnRef& r, const VolleyRules& rules, std::string* why) {
  auto fail = [&](const std::string& w) { if (why) *why = w; return std::optional<VolleyState>(); };
  if (!rules_ok(rules)) return fail("rules the model does not cover");
  const int n = r.period, need = sets_to_win(rules);
  if (n < 1 || n > rules.best_of) return fail("set " + std::to_string(n) + " is not a set of a best of " + std::to_string(rules.best_of));
  const auto& y = r.yes_lines;
  const auto& o = r.opp_lines;
  if (y.size() != o.size()) return fail("the two teams' linescores differ in length");
  if (static_cast<int>(y.size()) < n - 1)
    return fail("set " + std::to_string(n) + " with linescores for " + std::to_string(y.size()) + " sets");
  VolleyState s;
  for (int k = 0; k < n - 1; ++k) {   // the sets played
    const int a = y[k], b = o[k], t = set_target(rules, k + 1);
    if (a < 0 || b < 0) return fail("set " + std::to_string(k + 1) + "'s points were not read");
    if (won_at(a, b, t, rules.win_by)) ++s.sets_yes;
    else if (won_at(b, a, t, rules.win_by)) ++s.sets_opp;
    else return fail("set " + std::to_string(k + 1) + " has no winner at " + score(a, b));
  }
  if (s.sets_yes >= need || s.sets_opp >= need) return fail("the match was decided before set " + std::to_string(n));
  const int before_yes = s.sets_yes, before_opp = s.sets_opp;
  bool finished = false;
  if (static_cast<int>(y.size()) >= n && y[n - 1] >= 0 && o[n - 1] >= 0) {   // the set being played
    const int a = y[n - 1], b = o[n - 1], t = set_target(rules, n);
    s.points_known = true;
    if (past(a, b, t, rules.win_by) || past(b, a, t, rules.win_by)) {
      if (!won_at(a, b, t, rules.win_by) && !won_at(b, a, t, rules.win_by))
        return fail("set " + std::to_string(n) + "'s " + score(a, b) + " runs past its end");
      ++(a > b ? s.sets_yes : s.sets_opp);   // between sets: the next one at 0-0
      finished = true;
    } else {
      s.pts_yes = a;
      s.pts_opp = b;
    }
  }
  for (std::size_t k = static_cast<std::size_t>(n); k < y.size(); ++k)
    if (y[k] > 0 || o[k] > 0) return fail("linescores past set " + std::to_string(n) + " have points");
  // the score field is sets won: the sets before this one, or those and this one once it is won
  if (!(r.yes_score == before_yes && r.opp_score == before_opp) &&
      !(finished && r.yes_score == s.sets_yes && r.opp_score == s.sets_opp))
    return fail("the linescores give " + score(s.sets_yes, s.sets_opp) + " in sets, the score " +
                score(r.yes_score, r.opp_score));
  return s;
}

}  // namespace deskd
