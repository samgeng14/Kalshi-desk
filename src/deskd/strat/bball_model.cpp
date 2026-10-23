#include "deskd/strat/bball_model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "deskd/ref/espn.h"
#include "deskd/strat/lane_gate.h"

namespace deskd {

namespace {
// tools/train_bball.py's LEAGUES, field for field
const BballLeague kLeagues[] = {
    {"wcbb", "wcbb_logit", "KXNCAAWBGAME", 2400, 600, 300, 4, false},
    {"nba", "nba_logit", "KXNBAGAME", 2880, 720, 300, 4, true},
};

double logit(double p) {
  p = std::clamp(p, 0.01, 0.99);
  return std::log(p / (1 - p));
}
}  // namespace

const BballLeague* bball_league(const std::string& key) {
  for (const auto& l : kLeagues)
    if (l.key == key) return &l;
  return nullptr;
}

const BballLeague* bball_league_for_ticker(const std::string& ticker) {
  for (const auto& l : kLeagues)
    if (ticker.rfind(l.series + "-", 0) == 0) return &l;
  return nullptr;
}

std::optional<double> bball_feature(const std::string& name, const BballState& s, double regulation_s) {
  const double tau = std::max(s.t_left_s, 0.0) / regulation_s;
  const double sd = std::sqrt(tau + 0.01);
  if (name == "logit_p0") return logit(s.p0);
  if (name == "margin") return s.margin;
  if (name == "margin_over_sd") return s.margin / sd;
  if (name == "logit_p0_x_tau") return logit(s.p0) * tau;
  if (name == "logit_p0_x_sqrt_tau") return logit(s.p0) * std::sqrt(tau);
  if (name == "possession") return static_cast<double>(s.possession);
  if (name == "possession_over_sd") return s.possession / sd;
  if (name == "tau") return tau;
  if (name == "ot") return s.ot ? 1.0 : 0.0;
  return std::nullopt;
}

double bball_p(const BballModel& m, const BballState& s) {
  double z = m.intercept;
  for (std::size_t i = 0; i < m.features.size(); ++i)
    z += m.coef[i] * (bball_feature(m.features[i], s, m.regulation_s).value_or(0) - m.mean[i]) / m.scale[i];
  return 1 / (1 + std::exp(-z));
}

std::optional<BballModel> parse_bball_model(const std::string& text, const std::string& league, std::string* why) {
  auto fail = [&](const std::string& w) { if (why) *why = w; return std::optional<BballModel>(); };
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  // the model name says the league; the league and regulation_s keys, when there, must agree
  const std::string name = j.contains("model") && j["model"].is_string() ? j["model"].get<std::string>() : "";
  const BballLeague* lg = nullptr;
  for (const auto& l : kLeagues)
    if (l.model == name) lg = &l;
  if (!league.empty()) {
    const auto* want = bball_league(league);
    if (!want) return fail("no league \"" + league + "\"");
    if (lg != want) return fail("model is not \"" + want->model + "\"" + (lg ? " (it is the " + lg->key + " model)" : ""));
  } else if (!lg) {
    return fail("model is not \"wcbb_logit\" or \"nba_logit\"");
  }
  if (j.contains("league") && j["league"] != lg->key)
    return fail("league " + j["league"].dump() + " is not " + lg->model + "'s \"" + lg->key + "\"");
  if (j.contains("regulation_s") && !(j["regulation_s"].is_number() && j["regulation_s"].get<double>() == lg->regulation_s))
    return fail("regulation_s " + j["regulation_s"].dump() + " is not " + lg->key + "'s " +
                std::to_string(static_cast<int>(lg->regulation_s)) + " s");
  BballModel m;
  m.league = lg->key;
  m.regulation_s = lg->regulation_s;
  try {
    m.version = j.at("version").get<int>();
    m.trained = j.value("trained", "");
    m.features = j.at("features").get<std::vector<std::string>>();
    m.mean = j.at("mean").get<std::vector<double>>();
    m.scale = j.at("scale").get<std::vector<double>>();
    m.coef = j.at("coef").get<std::vector<double>>();
    m.intercept = j.at("intercept").get<double>();
    m.n_games = j.value("n_games", 0);
    if (j.contains("brier") && j["brier"].is_object()) {
      const auto& b = j["brier"];
      if (b.contains("model") && b["model"].is_number()) m.brier_model = b["model"].get<double>();
      if (b.contains("kalshi_mid") && b["kalshi_mid"].is_number()) m.brier_mid = b["kalshi_mid"].get<double>();
    }
  } catch (const std::exception& e) {
    return fail(std::string("malformed: ") + e.what());
  }
  const auto n = m.features.size();
  if (!n || m.mean.size() != n || m.scale.size() != n || m.coef.size() != n) return fail("features, mean, scale and coef differ in length");
  for (std::size_t i = 0; i < n; ++i) {
    if (!bball_feature(m.features[i], BballState{}, m.regulation_s)) return fail("unknown feature \"" + m.features[i] + "\"");
    if (!(std::isfinite(m.scale[i]) && m.scale[i] != 0) || !std::isfinite(m.mean[i]) || !std::isfinite(m.coef[i]))
      return fail("feature " + m.features[i] + ": a zero or non-finite scale, mean or coefficient");
  }
  if (!std::isfinite(m.intercept)) return fail("non-finite intercept");
  const auto& golden = j.contains("golden") ? j["golden"] : nlohmann::json();
  if (!golden.is_array() || golden.size() < 10) return fail("fewer than 10 golden rows");
  for (std::size_t r = 0; r < golden.size(); ++r) {
    const auto& g = golden[r];
    BballState s;
    try {
      const auto& in = g.at("in");
      s.t_left_s = in.at("t_left_s").get<double>();
      s.margin = in.at("margin").get<double>();
      s.possession = in.at("possession").get<int>();
      s.p0 = in.at("p0").get<double>();
      s.ot = in.value("ot", false);   // absent before the NBA's: regulation
      const double want = g.at("p").get<double>(), got = bball_p(m, s);
      if (!(std::fabs(got - want) <= 1e-6)) {
        std::ostringstream o;
        o << "golden row " << r << " gives " << got << ", the file says " << want
          << ": deskd's features are not the trainer's (docs/wcbb_model.md)";
        return fail(o.str());
      }
    } catch (const std::exception& e) {
      return fail("golden row " + std::to_string(r) + " malformed: " + e.what());
    }
  }
  m.sha256 = sha256_hex(text);
  return m;
}

std::optional<GameClock> bball_t_left(const BballLeague& lg, int period, double clock_s) {
  if (period < 1 || !(clock_s >= 0) || clock_s > lg.period_s) return std::nullopt;
  if (period <= lg.periods) return GameClock{(lg.periods - period) * lg.period_s + clock_s, false};
  if (!lg.ot_uses_clock) return GameClock{0, true};
  if (clock_s > lg.ot_s) return std::nullopt;
  return GameClock{clock_s, true};
}

std::optional<double> bball_live_p(const BballModel& m, const std::string& state, int period, double clock_s,
                                   double margin, std::optional<double> p0) {
  const auto* lg = bball_league(m.league);
  if (state != "in" || !p0 || !lg) return std::nullopt;
  const auto c = bball_t_left(*lg, period, clock_s);
  if (!c) return std::nullopt;
  return bball_p(m, BballState{c->t_left_s, margin, 0, *p0, c->ot});
}

std::optional<double> bball_live_p(const BballModel& m, const EspnRef& r, std::optional<double> p0) {
  return bball_live_p(m, r.state, r.period, r.clock_s, r.yes_score - r.opp_score, p0);
}

std::optional<BballModel> load_bball_model(const std::string& path, const std::string& league, std::string* why) {
  std::ifstream f(path);
  if (!f) { if (why) *why = "cannot read " + path; return std::nullopt; }
  std::stringstream s;
  s << f.rdbuf();
  auto m = parse_bball_model(s.str(), league, why);
  if (m) m->path = path;
  else if (why) *why = path + ": " + *why;
  return m;
}

}  // namespace deskd
