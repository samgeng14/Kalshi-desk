// strat/wcbb_model: the women's college basketball model file (docs/wcbb_model.md). Features
// worked by hand; a file whose golden rows reproduce loads, one whose rows do not (a trainer with
// other features), an unknown feature, mismatched lengths or too few golden rows are refused.
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/strat/wcbb_model.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

json model_json() {
  json j = {{"model", "wcbb_logit"}, {"version", 1}, {"trained", "2026-11-01"}, {"n_games", 1200},
            {"brier", {{"model", 0.12}, {"kalshi_mid", 0.125}, {"stern", 0.13}, {"held_out", "2025-26"}}},
            {"features", {"logit_p0", "margin_over_sd", "logit_p0_x_tau", "possession"}},
            {"mean", {0.1, 0.0, 0.05, 0.0}}, {"scale", {1.2, 8.0, 0.6, 0.9}}, {"coef", {1.1, 1.7, -0.4, 0.08}},
            {"intercept", 0.02}, {"golden", json::array()}};
  return j;
}

// golden rows as a trainer with these exact features would write them
void with_golden(json& j) {
  WcbbModel m;
  std::string why;
  json bare = j;
  bare["golden"] = json::array();
  for (int i = 0; i < 10; ++i)
    bare["golden"].push_back({{"in", {{"t_left_s", 0}, {"margin", 0}, {"possession", 0}, {"p0", 0.5}}}, {"p", 0}});
  // build the model without checking, from the same numbers
  m.features = j["features"].get<std::vector<std::string>>();
  m.mean = j["mean"].get<std::vector<double>>();
  m.scale = j["scale"].get<std::vector<double>>();
  m.coef = j["coef"].get<std::vector<double>>();
  m.intercept = j["intercept"].get<double>();
  const double t[] = {2400, 1800, 1200, 600, 60, 0, 2400, 900, 300, 30};
  const double mg[] = {0, 3, -5, 8, -2, 1, 12, -9, 4, -1};
  const int po[] = {0, 1, -1, 1, 0, -1, 1, 0, -1, 1};
  const double p0[] = {0.5, 0.62, 0.3, 0.8, 0.45, 0.55, 0.9, 0.15, 0.7, 0.5};
  j["golden"] = json::array();
  for (int i = 0; i < 10; ++i) {
    WcbbState s{t[i], mg[i], po[i], p0[i]};
    j["golden"].push_back({{"in", {{"t_left_s", t[i]}, {"margin", mg[i]}, {"possession", po[i]}, {"p0", p0[i]}}},
                           {"p", wcbb_p(m, s)}});
  }
}

void features() {
  WcbbState s{1200, 4, 1, 0.7};   // tau 0.5
  CHECK(near(*wcbb_feature("logit_p0", s), std::log(0.7 / 0.3)));
  CHECK(near(*wcbb_feature("margin_over_sd", s), 4 / std::sqrt(0.51)));
  CHECK(near(*wcbb_feature("logit_p0_x_tau", s), std::log(0.7 / 0.3) * 0.5));
  CHECK(near(*wcbb_feature("possession_over_sd", s), 1 / std::sqrt(0.51)));
  CHECK(near(*wcbb_feature("tau", s), 0.5) && near(*wcbb_feature("margin", s), 4));
  s.p0 = 0.999;   // clamped to 0.99
  CHECK(near(*wcbb_feature("logit_p0", s), std::log(0.99 / 0.01)));
  s.t_left_s = -5;   // overtime: tau 0
  CHECK(near(*wcbb_feature("tau", s), 0) && !wcbb_feature("elo", s));
  // p by hand: one feature, logit_p0, coefficient 1 on the raw value gives back p0
  WcbbModel m;
  m.features = {"logit_p0"}; m.mean = {0}; m.scale = {1}; m.coef = {1}; m.intercept = 0;
  CHECK(std::fabs(wcbb_p(m, WcbbState{2400, 0, 0, 0.7}) - 0.7) < 1e-12);
}

void files() {
  std::string why;
  auto j = model_json();
  with_golden(j);
  auto m = parse_wcbb_model(j.dump(), &why);
  CHECK(m && m->version == 1 && m->features.size() == 4 && m->brier_model == 0.12 && m->brier_mid == 0.125);
  // a trainer whose features differ: the golden rows say so
  auto bad = j;
  bad["golden"][3]["p"] = bad["golden"][3]["p"].get<double>() + 2e-6;
  CHECK(!parse_wcbb_model(bad.dump(), &why) && why.find("golden row 3") != std::string::npos);
  bad = j; bad["features"][1] = "elo";
  CHECK(!parse_wcbb_model(bad.dump(), &why) && why.find("unknown feature") != std::string::npos);
  bad = j; bad["coef"].erase(0);
  CHECK(!parse_wcbb_model(bad.dump(), &why));
  bad = j; bad["scale"][0] = 0;
  CHECK(!parse_wcbb_model(bad.dump(), &why));
  bad = j; bad["golden"].erase(9);
  CHECK(!parse_wcbb_model(bad.dump(), &why) && why.find("10 golden") != std::string::npos);
  bad = j; bad["model"] = "other";
  CHECK(!parse_wcbb_model(bad.dump(), &why));
  CHECK(!load_wcbb_model("/nonexistent/wcbb.json", &why) && why.find("cannot read") != std::string::npos);
}

// A file the Python trainer wrote (tools/train_wcbb.py on a generated panel, every feature): the
// C++ features reproduce its 10 golden rows to 1e-6, and the same file with one coefficient moved
// by 1e-4 is refused.
void trainer() {
  const std::string path = std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/wcbb_synthetic.json";
  std::string why;
  const auto m = load_wcbb_model(path, &why);
  CHECK(m.has_value());
  if (!m) { std::fprintf(stderr, "  %s\n", why.c_str()); return; }
  CHECK(m->features.size() == 7 && m->path == path && m->n_games == 2400 && m->brier_model > 0);
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  auto j = json::parse(s.str());
  j["coef"][2] = j["coef"][2].get<double>() + 1e-4;
  CHECK(!parse_wcbb_model(j.dump(), &why) && why.find("golden row") != std::string::npos);
}

// ESPN's game state to the model's: four 10-minute quarters, then overtime at 0
void live() {
  CHECK(wcbb_t_left(1, 600) == 2400.0 && wcbb_t_left(3, 252) == 852.0 && wcbb_t_left(4, 0) == 0.0);
  CHECK(wcbb_t_left(5, 120) == 0.0);
  CHECK(!wcbb_t_left(0, 300) && !wcbb_t_left(2, -1) && !wcbb_t_left(2, 601));
  WcbbModel m;   // p = p0 exactly, so what reaches the model is visible
  m.features = {"logit_p0"}; m.mean = {0}; m.scale = {1}; m.coef = {1}; m.intercept = 0;
  CHECK(wcbb_live_p(m, "in", 3, 252, 4, 0.62) && std::fabs(*wcbb_live_p(m, "in", 3, 252, 4, 0.62) - 0.62) < 1e-12);
  CHECK(!wcbb_live_p(m, "pre", 0, 600, 0, 0.62));    // before tip
  CHECK(!wcbb_live_p(m, "post", 4, 0, 10, 0.62));    // at the final
  CHECK(!wcbb_live_p(m, "in", 3, 252, 4, std::nullopt));   // no pregame price was captured
  CHECK(!wcbb_live_p(m, "in", 3, -1, 4, 0.62));      // the clock was not read
  // the margin and the time left reach the model: a lead late counts for more than early
  m.features = {"margin_over_sd"};
  CHECK(*wcbb_live_p(m, "in", 4, 60, 3, 0.5) > *wcbb_live_p(m, "in", 1, 540, 3, 0.5));
  CHECK(*wcbb_live_p(m, "in", 2, 300, -3, 0.5) < 0.5);
}

}  // namespace

int main() {
  features();
  files();
  live();
  trainer();
  return finish("deskd-wcbb");
}
