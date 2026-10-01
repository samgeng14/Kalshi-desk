#pragma once
// Lanes (M2, 2026-09-23): each model is a lane with its own stage and its own stake, and the
// stake is set by the lane's own live results rather than by a calibration proof.
//
//   stage          what the algorithm does with the model's AUTO decisions
//   research       nothing: priced, recorded, offered to you, never sent by the algorithm
//   paper          the same as research for execution; a model with no settled evidence at all
//   live_candidate sent, at the base stake ($5), while the first window of results accrues
//   live           sent, at a stake the windows have moved off the base
//
// The live-sizing rule, per lane, over its settled (closed) live trades in order:
//   every `lane_window_trades` (30) of them is one window. For each new complete window:
//     mean > 0 and (mean - z * sd / sqrt(n)) > -(one fee per trade)  -> stake doubles, to the cap
//     mean < 0                                                        -> stake halves
//     otherwise                                                       -> stake holds
//   `lane_negative_windows_to_research` (3) negative windows in a row -> back to research.
//   The lane's daily loss stop (a fraction of bankroll) -> research, or halt for the day
//   (`lane_loss_stop_action`).
// No Brier gate anywhere in this path: Brier is a diagnostic.
//
// Everything here is pure (JSON in, JSON out) so it can be tested without a desk.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace desk {

using json = nlohmann::json;

bool lane_managed(const json& settings, const std::string& model);
// "" when the model is not a lane.
std::string lane_stage(const json& settings, const std::string& model);
// The lane is at a stage the algorithm sends from (live_candidate or live) and not halted today.
bool lane_trades_live(const json& settings, const std::string& model, const std::string& today = "");
double lane_stake(const json& settings, const std::string& model);
// The cheap-side filter applies to this lane (settings.asymmetric_filter and the lane's own flag).
bool lane_asymmetric(const json& settings, const std::string& model);
bool valid_lane_stage(const std::string& stage);
// A complete lane object: missing fields filled from the defaults.
json lane_object(const json& partial, double base_stake);

struct LaneWindow {
  int n = 0;
  double mean = 0, sd = 0, lower = 0, fee = 0;
  std::string verdict;   // "double" | "halve" | "hold"
};
// One window of settled trades: realized P&L per trade and the fees each paid (entry + exit).
LaneWindow evaluate_lane_window(const std::vector<double>& pnl, const std::vector<double>& fees, double z);

// Applies every complete window not yet evaluated to `lane` (in order) and returns the updated
// lane. `pnl` and `fees` are ALL the lane's settled live trades since its since_ts, oldest first.
// Each change is described in `notes`.
json apply_lane_windows(json lane, const std::string& model, const std::vector<double>& pnl,
                        const std::vector<double>& fees, const json& settings, std::vector<std::string>* notes);

}  // namespace desk
