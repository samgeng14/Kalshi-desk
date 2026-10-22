#pragma once
// The women's college basketball model (plan section 5(f); docs/wcbb_model.md): P(the market's
// YES team wins) from the game state and the pregame Kalshi price, a logistic regression that
// tools/train_wcbb.py fits and writes as models/wcbb_logit_vN.json.
//
// strat/bball_model.h with the league fixed to wcbb: the same loader, features and checks (golden
// rows to 1e-6), and a file that is not a wcbb_logit model is refused. In overtime t_left_s is 0.
#include <optional>
#include <string>

#include "deskd/strat/bball_model.h"

namespace deskd {

using WcbbState = BballState;
using WcbbModel = BballModel;

// one feature's value (docs/wcbb_model.md); nullopt for a name outside the vocabulary
inline std::optional<double> wcbb_feature(const std::string& name, const WcbbState& s) {
  return bball_feature(name, s, bball_league("wcbb")->regulation_s);
}
inline double wcbb_p(const WcbbModel& m, const WcbbState& s) { return bball_p(m, s); }
inline std::optional<WcbbModel> load_wcbb_model(const std::string& path, std::string* why) {
  return load_bball_model(path, "wcbb", why);
}
inline std::optional<WcbbModel> parse_wcbb_model(const std::string& text, std::string* why) {
  return parse_bball_model(text, "wcbb", why);
}
// Regulation seconds left in a women's college game (4 quarters of 10 minutes) from ESPN's period
// and the period's clock; 0 in overtime; nullopt when the clock was not read
inline std::optional<double> wcbb_t_left(int period, double clock_s) {
  const auto c = bball_t_left(*bball_league("wcbb"), period, clock_s);
  return c ? std::optional<double>(c->t_left_s) : std::nullopt;
}
// The fair on a live game's ladder: bball_live_p on the wcbb model
inline std::optional<double> wcbb_live_p(const WcbbModel& m, const std::string& state, int period, double clock_s,
                                         double margin, std::optional<double> p0) {
  return bball_live_p(m, state, period, clock_s, margin, p0);
}

}  // namespace deskd
