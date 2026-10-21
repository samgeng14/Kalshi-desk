#include "deskd/strat/wcbb_model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace deskd {

namespace {
double logit(double p) {
  p = std::clamp(p, 0.01, 0.99);
  return std::log(p / (1 - p));
}
}  // namespace

std::optional<double> wcbb_feature(const std::string& name, const WcbbState& s) {
  const double tau = std::max(s.t_left_s, 0.0) / 2400.0;
  const double sd = std::sqrt(tau + 0.01);
  if (name == "logit_p0") return logit(s.p0);
  if (name == "margin") return s.margin;
  if (name == "margin_over_sd") return s.margin / sd;
  if (name == "logit_p0_x_tau") return logit(s.p0) * tau;
  if (name == "possession") return static_cast<double>(s.possession);
  if (name == "possession_over_sd") return s.possession / sd;
  if (name == "tau") return tau;
  return std::nullopt;
}

double wcbb_p(const WcbbModel& m, const WcbbState& s) {
  double z = m.intercept;
  for (std::size_t i = 0; i < m.features.size(); ++i)
    z += m.coef[i] * (wcbb_feature(m.features[i], s).value_or(0) - m.mean[i]) / m.scale[i];
  return 1 / (1 + std::exp(-z));
}

std::optional<WcbbModel> parse_wcbb_model(const std::string& text, std::string* why) {
  auto fail = [&](const std::string& w) { if (why) *why = w; return std::optional<WcbbModel>(); };
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  if (j.value("model", "") != "wcbb_logit") return fail("model is not \"wcbb_logit\"");
  WcbbModel m;
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
    if (!wcbb_feature(m.features[i], WcbbState{})) return fail("unknown feature \"" + m.features[i] + "\"");
    if (!(std::isfinite(m.scale[i]) && m.scale[i] != 0) || !std::isfinite(m.mean[i]) || !std::isfinite(m.coef[i]))
      return fail("feature " + m.features[i] + ": a zero or non-finite scale, mean or coefficient");
  }
  if (!std::isfinite(m.intercept)) return fail("non-finite intercept");
  const auto& golden = j.contains("golden") ? j["golden"] : nlohmann::json();
  if (!golden.is_array() || golden.size() < 10) return fail("fewer than 10 golden rows");
  for (std::size_t r = 0; r < golden.size(); ++r) {
    const auto& g = golden[r];
    WcbbState s;
    try {
      const auto& in = g.at("in");
      s.t_left_s = in.at("t_left_s").get<double>();
      s.margin = in.at("margin").get<double>();
      s.possession = in.at("possession").get<int>();
      s.p0 = in.at("p0").get<double>();
      const double want = g.at("p").get<double>(), got = wcbb_p(m, s);
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
  return m;
}

std::optional<double> wcbb_t_left(int period, double clock_s) {
  if (period < 1 || clock_s < 0 || clock_s > 600) return std::nullopt;
  if (period > 4) return 0.0;
  return (4 - period) * 600.0 + clock_s;
}

std::optional<double> wcbb_live_p(const WcbbModel& m, const std::string& state, int period, double clock_s, double margin,
                                  std::optional<double> p0) {
  if (state != "in" || !p0) return std::nullopt;
  const auto t = wcbb_t_left(period, clock_s);
  if (!t) return std::nullopt;
  return wcbb_p(m, WcbbState{*t, margin, 0, *p0});
}

std::optional<WcbbModel> load_wcbb_model(const std::string& path, std::string* why) {
  std::ifstream f(path);
  if (!f) { if (why) *why = "cannot read " + path; return std::nullopt; }
  std::stringstream s;
  s << f.rdbuf();
  auto m = parse_wcbb_model(s.str(), why);
  if (m) m->path = path;
  else if (why) *why = path + ": " + *why;
  return m;
}

}  // namespace deskd
