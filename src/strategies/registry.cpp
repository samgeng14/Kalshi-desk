#include "desk/strategies.h"
#include "desk/sports_models.h"

namespace desk {

std::vector<std::unique_ptr<Strategy>> build_strategies(const Config& cfg, ReferenceFeed* feed) {
  std::vector<std::unique_ptr<Strategy>> v;
  const json& P = cfg.strategy_params;
  auto params = [&](const char* name) -> json {
    return P.is_object() && P.contains(name) ? P[name] : json::object();
  };
  // Order matters: the first strategy that covers a market wins.
  v.push_back(std::make_unique<ManualFair>());
  v.push_back(std::make_unique<WeatherNws>(params("weather_nws"), cfg.user_agent));
  v.push_back(std::make_unique<CryptoLognormal>(params("crypto_lognormal"), cfg.user_agent));
  v.push_back(std::make_unique<SportsTotals>(params("sports_totals")));
  v.push_back(std::make_unique<SportsInplay>(params("sports_inplay"), cfg.user_agent));
  // Pregame line before the in-game model: it returns nothing once a game starts, and sports_live
  // (which returns nothing pregame unless ESPN posts a spread) takes the market from there.
  v.push_back(std::make_unique<SportsbookLine>(params("sportsbook_line"), cfg.user_agent));
  v.push_back(std::make_unique<SportsLive>(params("sports_live"), cfg.user_agent));
  // XVenue AFTER the sports models, not before them. It used to sit second, and the first
  // strategy that covers a market wins, so a Polymarket pair took the market from whichever
  // model would otherwise have priced it. That mattered because settlement_verified_for()
  // returns false for xvenue BY DESIGN - payoff parity with Polymarket was read for some
  // leagues, never verified - so every market it claimed became permanently RESEARCH.
  //
  // Measured on 2026-09-12: 604 game markets that sports_live verifies unconditionally were
  // being held by xvenue, 288 of them while xvenue produced no usable fair at all (its
  // Polymarket quote was stale or one-sided). Checked before making the change: all 707
  // structured markets in the sports_live leagues are "<Team> wins" contracts settling on the
  // official game result, which is exactly what that model estimates, with zero exceptions.
  //
  // XVenue still covers everything the sports models do not, and still contributes its
  // Polymarket reference price on those. It simply no longer takes markets that can be verified
  // away from the models that can verify them.
  v.push_back(std::make_unique<XVenue>());
  v.push_back(std::make_unique<EconNowcast>());
  // Before CommodityFutures, which covers the whole category: the first strategy that covers a
  // market wins, and the terminal model is the one with a settlement reference behind it.
  if (feed)
    for (auto& u : CommodityTerminal::default_underlyings())
      v.push_back(std::make_unique<CommodityTerminal>(std::move(u), feed, params("commodity"), cfg.user_agent));
  v.push_back(std::make_unique<CommodityFutures>());
  v.push_back(std::make_unique<IndexClose>());
  v.push_back(std::make_unique<SciTech>());
  return v;
}

}  // namespace desk
