#pragma once
// A lane's evidence gate (decisions 13: one loader, one layout): gates_dir/<lane>.json, written by
// the lane's gate tool (C1 tools/college_gate.py, X1 tools/x1_backtest.py, N1 and N2
// tools/nba_gate.py, W1 tools/wx_pairs.py) and read once at start, before gw starts. A lane its
// gate does not open carries the reason in LaneSpec::gate_why, and gw refuses to switch it on or
// take its orders. gw never reads the file: a new gate needs a restart.
//
//   {"lane": "N1",
//    "stage": "shadow" | "live",
//    "verdict": "PASS" | "FAIL", "why": "the tool's reason, on a FAIL",
//    "computed": "2026-11-02T04:10:00Z", "tool": "tools/nba_gate.py n1",
//    "model_sha256": "the sha256 of the model file it measured, hex; absent for a lane without one",
//    "params": {"thr": 0.10, ...},         the rule it measured, which the engine then trades
//    "series": {"KXNCAAWBGAME": {"pass": true, "why": "", "level": "point", "leader_only": false,
//                                "model_sha256": "..."}, ...},          per-series results (C1)
//    "evidence": {...}}                    the numbers, for the operator; deskd only keeps them
//
// load_lane_gate refuses a file (nullopt and why) that is missing, is not JSON, names another
// lane, has a verdict other than PASS (a FAIL gives the tool's why), a stage other than shadow or
// live, no params object or one without a name the lane needs, or a model_sha256 that is not the
// loaded model's: a retrained model closes the gate it was not measured on. A model_sha256 with no
// model loaded, or a loaded model the file does not name, is a mismatch too. A PASS at stage
// shadow loads but does not open the lane: its engine may only write shadow signals. Only stage
// live opens it.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace deskd {

struct LaneGate {
  struct Series {
    bool pass = false, leader_only = false;
    std::string why, level, model_sha256;   // level: the score level it measured ("point", "set"; "" = any)
  };
  std::string lane, stage, verdict, why, computed, tool, model_sha256, path;
  nlohmann::json params = nlohmann::json::object(), evidence = nlohmann::json::object();
  std::map<std::string, Series> series;
  bool live() const { return stage == "live"; }
};

// gates_dir/<lane>.json
std::string lane_gate_path(const std::string& gates_dir, const std::string& lane);
// model_sha256: the loaded model's sha256 (sha256_hex of its file), "" when the lane trades none;
// params: the names the lane's engine reads from params
std::optional<LaneGate> parse_lane_gate(const std::string& text, const std::string& lane, const std::string& model_sha256,
                                        const std::vector<std::string>& params, std::string* why);
std::optional<LaneGate> load_lane_gate(const std::string& path, const std::string& lane, const std::string& model_sha256,
                                       const std::vector<std::string>& params, std::string* why);
// The lane's LaneSpec::gate_why from load_lane_gate's result: "" when the gate opens it (stage
// live), else why not (load_lane_gate's why, or the shadow stage)
std::string lane_gate_why(const std::optional<LaneGate>& g, const std::string& why);
// C1: series -> "" (the lane may trade it) or why not. A series is tradable when the gate is live,
// the series passed, and the model loaded for it has the sha256 the gate measured (both "" for a
// series traded without a model). loaded_sha: the series the lane would trade, each with its
// model's sha256. The result covers the series of both maps.
std::map<std::string, std::string> tradable_series(const LaneGate& g, const std::map<std::string, std::string>& loaded_sha);
// lower-case hex SHA-256 of bytes (a model file's text): what the gate tools write (hashlib's hexdigest)
std::string sha256_hex(const std::string& bytes);

}  // namespace deskd
