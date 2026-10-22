// strat/lane_gate: the one gate-file loader (decisions 13). A missing file, not JSON, another
// lane's file, a FAIL or unknown verdict, an unknown stage, no params or a param the lane needs,
// and a model sha256 other than the loaded model's each give their own reason; a shadow PASS loads
// but does not open the lane, a live PASS opens it; C1's per-series results through tradable_series.
// The gate tools' own files (college_gate_ok.json, x1_gate.json, n1_gate.json, n2_gate.json, each
// kept current by its tool's self-test) load as deskd's runtime loads them, with the params it reads.
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/bus/units.h"
#include "deskd/gw/sys_book.h"
#include "deskd/strat/lane_gate.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

// FIPS 180-2's examples: the sha256 of "abc", of "" and of a two-block message
const std::string kAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const std::string kEmpty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const std::string kTwo = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// N1's live gate as tools/nba_gate.py writes it, measured on the model whose sha256 is kAbc
json n1_gate() {
  return {{"lane", "N1"}, {"stage", "live"}, {"verdict", "PASS"}, {"why", nullptr},
          {"computed", "2026-11-02T04:10:00Z"}, {"tool", "tools/nba_gate.py shadow"}, {"model_sha256", kAbc},
          {"params", {{"thr", 0.10}, {"clip", 2.0}}},
          {"evidence", {{"g3", {{"signals", 61}, {"days", 15}, {"mo60_net", 0.012}}}}},
          {"written_by", "a key deskd does not read"}};
}

// parse as N1 (model kAbc, needs thr); "" when it loads, else why
std::string refusal(const json& j, const std::string& sha = kAbc, const std::vector<std::string>& params = {"thr"}) {
  std::string why;
  return parse_lane_gate(j.dump(), "N1", sha, params, &why) ? "" : why;
}

void write(const std::string& path, const std::string& text) {
  std::ofstream f(path);
  f << text;
}

std::string fixture(const std::string& name) { return std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/" + name; }
std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

void sha() {
  CHECK(sha256_hex("abc") == kAbc);
  CHECK(sha256_hex("") == kEmpty);
  CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == kTwo);
  CHECK(sha256_hex(std::string("a\0b", 3)) != sha256_hex("ab"));   // bytes, not a C string
}

void opens() {
  std::string why = "untouched";
  auto g = parse_lane_gate(n1_gate().dump(), "N1", kAbc, {"thr", "clip"}, &why);
  CHECK(g && why == "untouched");
  CHECK(g->lane == "N1" && g->stage == "live" && g->live() && g->verdict == "PASS" && g->why.empty());
  CHECK(g->computed == "2026-11-02T04:10:00Z" && g->tool == "tools/nba_gate.py shadow" && g->model_sha256 == kAbc);
  CHECK(g->params.value("thr", 0.0) == 0.10 && g->evidence["g3"].value("signals", 0) == 61);
  CHECK(g->series.empty() && g->path.empty());
  CHECK(lane_gate_why(g, "") == "");   // a live PASS opens the lane
  // hashlib writes lower case; either side in upper case is the same sha
  auto j = n1_gate();
  j["model_sha256"] = upper(kAbc);
  CHECK(refusal(j) == "");
  CHECK(refusal(n1_gate(), upper(kAbc)) == "");
  // no params needed: an empty params object is a rule with nothing to set
  j = n1_gate();
  j["params"] = json::object();
  CHECK(refusal(j, kAbc, {}) == "");
  // a lane without a model (X1, N2, W1): the file names none and none is loaded
  json x1 = {{"lane", "X1"}, {"stage", "live"}, {"verdict", "PASS"}, {"params", {{"direction", "fade"}, {"thr", 0.08}}}};
  g = parse_lane_gate(x1.dump(), "X1", "", {"direction", "thr"}, &why);
  CHECK(g && g->model_sha256.empty() && g->params.value("direction", "") == "fade" && g->evidence.is_object());
}

void shadow() {
  auto j = n1_gate();
  j["stage"] = "shadow";
  std::string why;
  const auto g = parse_lane_gate(j.dump(), "N1", kAbc, {"thr"}, &why);
  CHECK(g && !g->live());   // loads: the engine may write shadow signals from its params ...
  const auto closed = lane_gate_why(g, "");
  CHECK(has(closed, "stage shadow") && !has(closed, ": stage"));   // ... but the lane stays gated
  CHECK(lane_gate_why(std::nullopt, "no gate file /g/N1.json") == "no gate file /g/N1.json");
  CHECK(lane_gate_why(std::nullopt, "") == "no gate");   // never "" without a gate
}

void refusals() {
  std::set<std::string> reasons;
  auto refused = [&](const json& j, const std::string& part, const std::string& sha = kAbc) {
    const auto why = refusal(j, sha);
    CHECK(has(why, part));
    if (!has(why, part)) std::fprintf(stderr, "  wanted \"%s\", got \"%s\"\n", part.c_str(), why.c_str());
    reasons.insert(why);
  };
  std::string why;
  CHECK(!parse_lane_gate("{\"lane\": \"N1\",", "N1", kAbc, {}, &why) && why == "not a JSON object");
  reasons.insert(why);
  CHECK(refusal(json::array({n1_gate()})) == "not a JSON object");
  auto j = n1_gate();
  j["lane"] = "N2";
  refused(j, "is lane N2's gate, not N1's");
  j.erase("lane");
  refused(j, "names no lane");
  j = n1_gate();
  j["verdict"] = "FAIL";
  j["why"] = "G1: Brier 0.1712 is not below the mid's 0.1709";
  j["stage"] = "unheard of";   // a FAIL is a FAIL whatever else the file says
  refused(j, "verdict FAIL: G1: Brier 0.1712 is not below the mid's 0.1709");
  j.erase("why");
  CHECK(refusal(j) == "verdict FAIL");
  j["verdict"] = "pass";
  refused(j, "verdict \"pass\" is neither PASS nor FAIL");
  j.erase("verdict");
  CHECK(has(refusal(j), "neither PASS nor FAIL"));
  j = n1_gate();
  j["stage"] = "paper";
  refused(j, "unknown stage \"paper\"");
  j.erase("stage");
  CHECK(has(refusal(j), "unknown stage \"\""));
  j = n1_gate();
  j.erase("params");
  refused(j, "no params object");
  j["params"] = json::array({0.10});
  CHECK(refusal(j) == "no params object");
  j["params"] = {{"clip", 2.0}};
  refused(j, "params has no thr");
  j["params"] = {{"thr", nullptr}};
  CHECK(refusal(j) == "params has no thr");
  // the model: a retrained one closes the gate measured on the old one
  j = n1_gate();
  j["model_sha256"] = kEmpty;
  refused(j, "measured another model (" + kEmpty.substr(0, 12) + ", loaded " + kAbc.substr(0, 12) + ")", kAbc);
  CHECK(has(refusal(n1_gate(), kEmpty), "measured another model"));
  refused(n1_gate(), "and no model is loaded", "");
  j = n1_gate();
  j.erase("model_sha256");
  refused(j, "names no model, and model " + kAbc.substr(0, 12) + " is loaded");
  j["model_sha256"] = "abc123";
  refused(j, "model_sha256 is not 64 hex digits");
  j["model_sha256"] = std::string(63, 'a') + "g";
  CHECK(refusal(j) == "model_sha256 is not 64 hex digits");
  // a field of the wrong type
  j = n1_gate();
  j["lane"] = 1;
  refused(j, "malformed: ");
  j = n1_gate();
  j["series"] = json::array();
  refused(j, "series is not an object");
  j["series"] = {{"KXA", {{"why", "no pass"}}}};
  refused(j, "series KXA has no pass (true or false)");
  j["series"] = {{"KXA", {{"pass", "yes"}}}};
  CHECK(refusal(j) == "series KXA has no pass (true or false)");
  j["series"] = {{"KXA", {{"pass", true}, {"model_sha256", "zz"}}}};
  refused(j, "series KXA: model_sha256 is not 64 hex digits");
  // each reason is its own
  CHECK(reasons.size() == 16);
}

void files() {
  TempDir d;
  const auto path = lane_gate_path(d.path, "X1");
  CHECK(path == d.path + "/X1.json");
  std::string why;
  auto g = load_lane_gate(path, "X1", "", {"direction"}, &why);
  CHECK(!g && why == "no gate file " + path && lane_gate_why(g, why) == why);
  json x1 = {{"lane", "X1"}, {"stage", "shadow"}, {"verdict", "PASS"}, {"params", {{"direction", "follow"}, {"thr", 0.08}}}};
  write(path, x1.dump(2));
  g = load_lane_gate(path, "X1", "", {"direction"}, &why);
  CHECK(g && g->path == path && g->params.value("direction", "") == "follow");
  CHECK(lane_gate_why(g, "") == path + ": stage shadow (shadow signals only until its gate is live)");
  x1["stage"] = "live";
  write(path, x1.dump());
  g = load_lane_gate(path, "X1", "", {"direction"}, &why);
  CHECK(g && lane_gate_why(g, "") == "");
  x1["verdict"] = "FAIL";
  x1["why"] = "both directions negative on the test dates";
  write(path, x1.dump());
  g = load_lane_gate(path, "X1", "", {"direction"}, &why);
  CHECK(!g && why == path + ": verdict FAIL: both directions negative on the test dates");
  CHECK(lane_gate_why(g, why) == why);
  write(path, "");
  CHECK(!load_lane_gate(path, "X1", "", {}, &why) && why == path + ": not a JSON object");
  // the X1 gate read as another lane's: the lane in the file decides
  write(path, json({{"lane", "X1"}, {"stage", "live"}, {"verdict", "PASS"}, {"params", json::object()}}).dump());
  CHECK(!load_lane_gate(path, "N2", "", {}, &why) && why == path + ": is lane X1's gate, not N2's");
}

// C1: one gate, a result per series (decisions 13)
void series() {
  const std::string volley = sha256_hex("volley_markov_v1");
  json c1 = {{"lane", "C1"}, {"stage", "live"}, {"verdict", "PASS"}, {"tool", "tools/college_gate.py"},
             {"params", {{"edge", 0.20}, {"exec_lag_s", 2}, {"max_score_age_s", 5}}},
             {"series", {{"KXWB", {{"pass", true}, {"why", ""}, {"level", nullptr}, {"leader_only", false}, {"model_sha256", kAbc}}},
                         {"KXVB", {{"pass", true}, {"level", "set"}, {"leader_only", true}, {"model_sha256", volley}}},
                         {"KXMB", {{"pass", false}, {"why", "31 cases, fewer than 50"}, {"model_sha256", kAbc}}},
                         {"KXOLD", {{"pass", true}, {"model_sha256", kAbc}}},
                         {"KXRAW", {{"pass", true}}}}}};
  std::string why;
  const auto g = parse_lane_gate(c1.dump(), "C1", "", {"edge"}, &why);
  CHECK(g && g->series.size() == 5);
  if (!g) return;
  const auto& vb = g->series.at("KXVB");
  CHECK(vb.pass && vb.level == "set" && vb.leader_only && vb.model_sha256 == volley && vb.why.empty());
  const auto& wb = g->series.at("KXWB");
  CHECK(wb.pass && wb.level.empty() && !wb.leader_only && wb.model_sha256 == kAbc);
  CHECK(!g->series.at("KXMB").pass && g->series.at("KXRAW").model_sha256.empty());
  const std::map<std::string, std::string> loaded = {
      {"KXWB", kAbc}, {"KXVB", volley}, {"KXMB", kAbc}, {"KXRAW", ""}, {"KXNEW", kAbc}};
  auto t = tradable_series(*g, loaded);
  CHECK(t.size() == 6);
  CHECK(t["KXWB"] == "" && t["KXVB"] == "" && t["KXRAW"] == "");   // a series traded without a model
  CHECK(t["KXMB"] == "did not pass the gate: 31 cases, fewer than 50");
  CHECK(t["KXOLD"] == "no model loaded for it");
  CHECK(t["KXNEW"] == "the gate did not measure it");
  // a retrained volleyball model: its series closes, basketball stays open
  auto other = loaded;
  other["KXVB"] = sha256_hex("volley_markov_v2");
  other["KXRAW"] = kAbc;   // a model where the gate measured none
  other["KXWB"] = upper(kAbc);
  t = tradable_series(*g, other);
  CHECK(t["KXVB"] == "the gate measured another model" && t["KXRAW"] == "the gate measured another model" && t["KXWB"] == "");
  // a failed series with no why
  auto mb = *g;
  mb.series["KXMB"].why = "";
  CHECK(tradable_series(mb, loaded)["KXMB"] == "did not pass the gate");
  // at stage shadow nothing is tradable
  auto sh = *g;
  sh.stage = "shadow";
  t = tradable_series(sh, loaded);
  CHECK(t.size() == 6);
  for (const auto& [s, w] : t) CHECK(w == "the gate is at stage shadow");
  // no series at all: every loaded series is unmeasured
  json bare = c1;
  bare.erase("series");
  const auto b = parse_lane_gate(bare.dump(), "C1", "", {"edge"}, &why);
  CHECK(b && b->series.empty() && tradable_series(*b, {{"KXWB", kAbc}}).at("KXWB") == "the gate did not measure it");
}

// tools/college_gate.py's gate for its synthetic season, as that tool writes it: C1's, live, both
// series passed at point level on their models (wcbb_synthetic.json, and volleyball_golden.json,
// the bytes of models/volley_markov_v1.json), measured at gw's inventory for C1
void college_fixture() {
  const std::string path = fixture("college_gate_ok.json");
  std::string why = "untouched";
  const auto g = load_lane_gate(path, "C1", "", {"edge"}, &why);
  CHECK(g && why == "untouched");
  if (!g) return;
  CHECK(g->lane == "C1" && g->verdict == "PASS" && g->live() && g->model_sha256.empty() && lane_gate_why(g, why) == "");
  CHECK(g->params.value("edge", 0.0) == 0.2 && g->tool == "tools/college_gate.py" && g->evidence.contains("series"));
  const auto& lanes = default_lanes();
  const auto c1 = std::find_if(lanes.begin(), lanes.end(), [](const LaneSpec& l) { return l.id == "C1"; });
  CHECK(c1 != lanes.end() && g->params.value("inventory", 0) * kOneContract == c1->inventory);
  CHECK(g->series.size() == 2 && g->series.count("KXNCAAWBGAME") && g->series.count("KXNCAAWVMATCH"));
  for (const auto& [name, s] : g->series) CHECK(s.pass && s.why.empty() && s.level == "point" && !s.leader_only);
  const std::string wb = sha256_hex(read_text(fixture("wcbb_synthetic.json")));
  const std::string vb = sha256_hex(read_text(fixture("volleyball_golden.json")));
  CHECK(vb == sha256_hex(read_text(std::string(DESKD_SOURCE_DIR) + "/models/volley_markov_v1.json")));
  auto t = tradable_series(*g, {{"KXNCAAWBGAME", wb}, {"KXNCAAWVMATCH", vb}});
  CHECK(t.size() == 2 && t["KXNCAAWBGAME"] == "" && t["KXNCAAWVMATCH"] == "");
  t = tradable_series(*g, {{"KXNCAAWBGAME", kAbc}, {"KXNCAAWVMATCH", vb}});
  CHECK(t["KXNCAAWBGAME"] == "the gate measured another model" && t["KXNCAAWVMATCH"] == "");
}

// tools/x1_backtest.py's gate for its synthetic extract, as that tool writes it (its self-test
// keeps the file current): deskd loads it as X1's, at stage shadow, with the rule's params
void x1_fixture() {
  const std::string path = fixture("x1_gate.json");
  std::string why = "untouched";
  const auto g = load_lane_gate(path, "X1", "", {"direction", "thr"}, &why);
  CHECK(g && why == "untouched");
  if (!g) return;
  CHECK(g->lane == "X1" && g->verdict == "PASS" && g->stage == "shadow" && !g->live() && g->model_sha256.empty());
  CHECK(g->params.value("direction", "") == "fade" && g->params.value("thr", 0.0) == 0.12);
  CHECK(g->tool.rfind("tools/x1_backtest.py", 0) == 0 && g->evidence.contains("db"));
  CHECK(lane_gate_why(g, why) == path + ": stage shadow (shadow signals only until its gate is live)");
}

// tools/nba_gate.py's N1 gate for its synthetic panel, measured on nba_synthetic.json as the
// model: it loads on that model only, at stage shadow
void n1_fixture() {
  const std::string path = fixture("n1_gate.json");
  const std::string sha = sha256_hex(read_text(fixture("nba_synthetic.json")));
  std::string why = "untouched";
  const auto g = load_lane_gate(path, "N1", sha, {"thr"}, &why);
  CHECK(g && why == "untouched");
  if (!g) return;
  CHECK(g->lane == "N1" && g->stage == "shadow" && !g->live() && g->verdict == "PASS");
  CHECK(g->model_sha256 == "e3febb3b41bd15f51e49568f34147917e32f34943894a3a27ce994bb114fd795" && g->model_sha256 == sha);
  CHECK(g->params.value("thr", 0.0) == 0.1);
  CHECK(lane_gate_why(g, "") == path + ": stage shadow (shadow signals only until its gate is live)");
  // another model, or none, closes it
  CHECK(!load_lane_gate(path, "N1", kAbc, {"thr"}, &why) && has(why, "measured another model"));
  CHECK(!load_lane_gate(path, "N1", "", {"thr"}, &why) &&
        why == path + ": measured model e3febb3b41bd, and no model is loaded");
}

// tools/nba_gate.py's N2 gate (pregame favourites, no model): live, with its count and band
void n2_fixture() {
  const std::string path = fixture("n2_gate.json");
  std::string why = "untouched";
  const auto g = load_lane_gate(path, "N2", "", {"count", "fav_lo", "fav_hi"}, &why);
  CHECK(g && why == "untouched");
  if (!g) return;
  CHECK(g->lane == "N2" && g->live() && lane_gate_why(g, "") == "" && g->model_sha256.empty());
  CHECK(g->params.value("count", 0) == 5 && g->params.value("fav_lo", 0.0) == 0.9 && g->params.value("fav_hi", 0.0) == 0.95);
  CHECK(!load_lane_gate(path, "N2", kAbc, {"count", "fav_lo", "fav_hi"}, &why) &&
        why == path + ": names no model, and model ba7816bf8f01 is loaded");
}

}  // namespace

int main() {
  sha();
  opens();
  shadow();
  refusals();
  files();
  x1_fixture();
  college_fixture();
  n1_fixture();
  n2_fixture();
  series();
  return finish("deskd-lane_gate");
}
