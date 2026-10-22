// strat/bball_model: the basketball models of every league in one loader (docs/wcbb_model.md).
// The league table and NBA's clock (four 12-minute quarters, then overtime on its own clock); the
// two features beyond WCBB's first file (ot, logit_p0_x_sqrt_tau) worked by hand; an NBA file whose
// golden rows reproduce loads, with the sha256 a gate file names; a league or regulation_s that is
// not its model's, or a model of another league than the one asked for, is refused; the WCBB
// file deskd-wcbb loads still loads here; the live fair in overtime.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/ref/espn.h"
#include "deskd/strat/bball_model.h"
#include "deskd/strat/lane_gate.h"
#include "deskd/strat/wcbb_model.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
double sigmoid(double z) { return 1 / (1 + std::exp(-z)); }

std::string fixture(const std::string& name) { return std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/" + name; }
std::string read(const std::string& path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

void leagues() {
  const auto* nba = bball_league("nba");
  const auto* wcbb = bball_league("wcbb");
  CHECK(nba && nba->model == "nba_logit" && nba->regulation_s == 2880 && nba->period_s == 720 && nba->periods == 4 &&
        nba->ot_s == 300 && nba->ot_uses_clock);
  CHECK(wcbb && wcbb->model == "wcbb_logit" && wcbb->regulation_s == 2400 && wcbb->period_s == 600 && !wcbb->ot_uses_clock);
  CHECK(!bball_league("nhl") && !bball_league(""));
  CHECK(bball_league_for_ticker("KXNBAGAME-26NOV02LALBOS-BOS") == nba);
  CHECK(bball_league_for_ticker("KXNCAAWBGAME-26NOV03UCONNSC-SC") == wcbb);
  CHECK(!bball_league_for_ticker("KXNBA-26-BOS") && !bball_league_for_ticker("KXNBAGAMES-26NOV02") &&
        !bball_league_for_ticker("KXNCAAMBGAME-26NOV03DUKEUNC-DUKE"));
}

// ESPN's period and clock to the model's: NBA counts down four 12-minute quarters, then an
// overtime reads its own 5-minute clock; WCBB's overtime is 0, whatever its clock says
void game_clock() {
  const auto& nba = *bball_league("nba");
  auto at = [&](int period, double clock_s) { return bball_t_left(nba, period, clock_s); };
  CHECK(at(1, 720) && at(1, 720)->t_left_s == 2880 && !at(1, 720)->ot);   // Q1 12:00
  CHECK(at(3, 252) && at(3, 252)->t_left_s == 972 && !at(3, 252)->ot);    // Q3 4:12
  CHECK(at(4, 35.4) && near(at(4, 35.4)->t_left_s, 35.4) && !at(4, 35.4)->ot);
  CHECK(at(4, 0) && at(4, 0)->t_left_s == 0 && !at(4, 0)->ot);            // the horn
  CHECK(at(5, 180) && at(5, 180)->t_left_s == 180 && at(5, 180)->ot);     // OT1 3:00
  CHECK(at(7, 300) && at(7, 300)->t_left_s == 300 && at(7, 300)->ot);     // a third overtime's tip
  CHECK(!at(5, 301));                                                     // longer than an overtime
  CHECK(!at(2, 780));                                                     // 13:00 in Q2
  CHECK(!at(0, 300) && !at(2, -1) && !at(2, std::nan("")));               // before the tip; the clock unread
  const auto& wcbb = *bball_league("wcbb");
  CHECK(bball_t_left(wcbb, 1, 600)->t_left_s == 2400 && bball_t_left(wcbb, 3, 252)->t_left_s == 852);
  CHECK(bball_t_left(wcbb, 5, 120)->t_left_s == 0 && bball_t_left(wcbb, 5, 120)->ot);
  CHECK(bball_t_left(wcbb, 5, 400) && bball_t_left(wcbb, 5, 400)->t_left_s == 0);   // the clock is not read
  CHECK(!bball_t_left(wcbb, 2, 601) && !bball_t_left(wcbb, 5, 601));
}

void features() {
  const double lp = std::log(0.7 / 0.3);
  BballState s{1440, 4, 1, 0.7, false};   // NBA: tau 0.5
  CHECK(near(*bball_feature("tau", s, 2880), 0.5));
  CHECK(near(*bball_feature("margin_over_sd", s, 2880), 4 / std::sqrt(0.51)));
  CHECK(near(*bball_feature("logit_p0_x_tau", s, 2880), lp * 0.5));
  CHECK(near(*bball_feature("logit_p0_x_sqrt_tau", s, 2880), lp * std::sqrt(0.5)));
  CHECK(near(*bball_feature("possession_over_sd", s, 2880), 1 / std::sqrt(0.51)));
  CHECK(*bball_feature("ot", s, 2880) == 0);
  CHECK(near(*bball_feature("tau", s, 2400), 0.6));   // the same clock is a later point of a shorter game
  s = BballState{180, -2, 0, 0.7, true};              // OT 3:00: tau is the overtime clock over 2880
  CHECK(*bball_feature("ot", s, 2880) == 1 && near(*bball_feature("tau", s, 2880), 0.0625));
  CHECK(near(*bball_feature("margin_over_sd", s, 2880), -2 / std::sqrt(0.0725)));
  CHECK(near(*bball_feature("logit_p0_x_sqrt_tau", s, 2880), lp * 0.25));
  s.t_left_s = -5;   // tau 0
  CHECK(*bball_feature("logit_p0_x_sqrt_tau", s, 2880) == 0 && !bball_feature("elo", s, 2880));
  // the WCBB names read as they always did
  CHECK(near(*wcbb_feature("logit_p0_x_sqrt_tau", WcbbState{1200, 0, 0, 0.7}), lp * std::sqrt(0.5)));
  CHECK(*wcbb_feature("ot", WcbbState{0, 0, 0, 0.7, true}) == 1);
}

// The NBA fixture (a model file in the trainer's format, with an overtime golden row): it loads,
// it is the NBA's, its sha256 is the file's, and every number in it is checked by some golden row
void nba_file() {
  const std::string path = fixture("nba_synthetic.json");
  const std::string text = read(path);
  std::string why;
  const auto m = load_bball_model(path, "nba", &why);
  CHECK(m.has_value());
  if (!m) { std::fprintf(stderr, "  %s\n", why.c_str()); return; }
  CHECK(m->league == "nba" && m->regulation_s == 2880 && m->path == path && m->version >= 1);
  CHECK(m->sha256.size() == 64 && m->sha256 == sha256_hex(text));
  CHECK(load_bball_model(path, "", &why) && load_bball_model(path, "", &why)->league == "nba");
  const auto j = json::parse(text);
  // an overtime golden row, read as overtime: the same row as regulation does not reproduce
  int ot_row = -1;
  for (std::size_t r = 0; r < j["golden"].size(); ++r)
    if (j["golden"][r]["in"].value("ot", false)) ot_row = static_cast<int>(r);
  CHECK(ot_row >= 0 && std::count(m->features.begin(), m->features.end(), "ot") == 1);
  if (ot_row >= 0) {
    auto bad = j;
    bad["golden"][ot_row]["in"]["ot"] = false;
    CHECK(!parse_bball_model(bad.dump(), "nba", &why) && has(why, "golden row " + std::to_string(ot_row)));
    bad["golden"][ot_row]["in"]["ot"] = "yes";
    CHECK(!parse_bball_model(bad.dump(), "nba", &why) && has(why, "malformed"));
  }
  for (std::size_t i = 0; i < m->coef.size(); ++i) {   // every coefficient reaches a golden row
    auto bad = j;
    bad["coef"][i] = bad["coef"][i].get<double>() + 1e-4;
    CHECK(!parse_bball_model(bad.dump(), "nba", &why) && has(why, "golden row"));
  }
  // the gate names the model by this sha256: one more byte in the file and it is another model
  const auto gate = json{{"lane", "N1"}, {"stage", "live"}, {"verdict", "PASS"}, {"model_sha256", m->sha256},
                         {"params", {{"thr", 0.1}}}}.dump();
  CHECK(parse_lane_gate(gate, "N1", m->sha256, {"thr"}, &why));
  const auto other = parse_bball_model(text + "\n", "nba", &why);
  CHECK(other && other->sha256 != m->sha256 && !parse_lane_gate(gate, "N1", other->sha256, {"thr"}, &why));
}

// The league a file says it is must be its model's, and a caller gets only the league it asked for
void refusals() {
  const auto j = json::parse(read(fixture("nba_synthetic.json")));
  std::string why;
  auto bad = j;
  bad["regulation_s"] = 2400;
  CHECK(!parse_bball_model(bad.dump(), "nba", &why) && has(why, "regulation_s 2400 is not nba's 2880 s"));
  CHECK(!parse_bball_model(bad.dump(), "", &why) && has(why, "regulation_s"));
  bad["regulation_s"] = "2880";
  CHECK(!parse_bball_model(bad.dump(), "nba", &why) && has(why, "regulation_s"));
  bad = j;
  bad["league"] = "wcbb";
  CHECK(!parse_bball_model(bad.dump(), "", &why) && has(why, "league \"wcbb\" is not nba_logit's \"nba\""));
  // a file written before the league keys existed takes them from its model name
  bad = j;
  bad.erase("league");
  bad.erase("regulation_s");
  const auto old = parse_bball_model(bad.dump(), "nba", &why);
  CHECK(old && old->league == "nba" && old->regulation_s == 2880);
  // the WCBB loader refuses the NBA's model, and an unknown league or model is refused
  CHECK(!parse_bball_model(j.dump(), "wcbb", &why) && has(why, "model is not \"wcbb_logit\" (it is the nba model)"));
  CHECK(!load_wcbb_model(fixture("nba_synthetic.json"), &why) && has(why, "nba_synthetic.json: model is not"));
  CHECK(!parse_bball_model(j.dump(), "nhl", &why) && has(why, "no league"));
  bad = j;
  bad["model"] = "nhl_logit";
  CHECK(!parse_bball_model(bad.dump(), "", &why) && has(why, "model is not"));
  bad["model"] = 7;
  CHECK(!parse_bball_model(bad.dump(), "", &why) && has(why, "model is not"));
  CHECK(!load_bball_model("/nonexistent/nba.json", "nba", &why) && has(why, "cannot read"));
}

// deskd-wcbb's trainer file, through the league-wide loader: the same model as through the wrappers
void wcbb_file() {
  const std::string path = fixture("wcbb_synthetic.json");
  std::string why;
  const auto b = load_bball_model(path, "wcbb", &why);
  const auto w = load_wcbb_model(path, &why);
  CHECK(b && w && b->league == "wcbb" && b->regulation_s == 2400 && b->features.size() == 7);
  CHECK(load_bball_model(path, "", &why) && load_bball_model(path, "", &why)->league == "wcbb");
  CHECK(!load_bball_model(path, "nba", &why) && has(why, "model is not \"nba_logit\" (it is the wcbb model)"));
  if (!b || !w) return;
  CHECK(b->sha256 == sha256_hex(read(path)) && w->sha256 == b->sha256);
  for (const auto& s : {BballState{2400, 0, 0, 0.5}, BballState{852, -4, 1, 0.62}, BballState{0, 2, -1, 0.3, true}})
    CHECK(bball_p(*b, s) == wcbb_p(*w, s));
}

// The fair while the game is on: NBA overtime reaches the model as overtime, with its clock
void live() {
  BballModel m;   // p = p0 exactly, so what reaches the model is visible
  m.league = "nba"; m.regulation_s = 2880;
  m.features = {"logit_p0"}; m.mean = {0}; m.scale = {1}; m.coef = {1}; m.intercept = 0;
  CHECK(bball_live_p(m, "in", 5, 180, 0, 0.62) && near(*bball_live_p(m, "in", 5, 180, 0, 0.62), 0.62));
  CHECK(!bball_live_p(m, "pre", 0, 720, 0, 0.62) && !bball_live_p(m, "post", 4, 0, 10, 0.62));
  CHECK(!bball_live_p(m, "in", 3, 252, 4, std::nullopt) && !bball_live_p(m, "in", 3, -1, 4, 0.62));
  CHECK(!bball_live_p(m, "in", 2, 780, 4, 0.62) && !bball_live_p(m, "in", 5, 400, 4, 0.62));
  m.features = {"ot"};   // overtime is 1, regulation 0
  CHECK(near(*bball_live_p(m, "in", 5, 180, 0, 0.5), sigmoid(1)) && near(*bball_live_p(m, "in", 4, 30, 0, 0.5), 0.5));
  m.features = {"margin_over_sd"};   // a lead with 3:00 left in overtime counts for more than in Q1
  CHECK(*bball_live_p(m, "in", 5, 180, 3, 0.5) > *bball_live_p(m, "in", 1, 540, 3, 0.5));
  CHECK(near(*bball_live_p(m, "in", 5, 180, 3, 0.5), sigmoid(3 / std::sqrt(0.0725))));
  // from ESPN's row for the market
  EspnRef r;
  r.state = "in"; r.period = 4; r.clock_s = 35.4; r.yes_score = 100; r.opp_score = 98;
  CHECK(bball_live_p(m, r, 0.62) && *bball_live_p(m, r, 0.62) == bball_p(m, BballState{35.4, 2, 0, 0.62, false}));
  r.period = 6; r.clock_s = 12;
  CHECK(*bball_live_p(m, r, 0.62) == bball_p(m, BballState{12, 2, 0, 0.62, true}));
  r.state = "post";
  CHECK(!bball_live_p(m, r, 0.62));
  m.league = "nhl";   // a model of no league has no clock
  CHECK(!bball_live_p(m, "in", 3, 252, 4, 0.62));
  // WCBB through its wrapper: overtime is t_left 0 and ot 1; a model without a league is WCBB's
  WcbbModel w;
  w.features = {"ot"}; w.mean = {0}; w.scale = {1}; w.coef = {1}; w.intercept = 0;
  CHECK(w.league == "wcbb" && w.regulation_s == 2400);
  CHECK(near(*wcbb_live_p(w, "in", 5, 120, 0, 0.5), sigmoid(1)) && near(*wcbb_live_p(w, "in", 4, 120, 0, 0.5), 0.5));
}

}  // namespace

int main() {
  leagues();
  game_clock();
  features();
  nba_file();
  refusals();
  wcbb_file();
  live();
  return finish("deskd-bball");
}
