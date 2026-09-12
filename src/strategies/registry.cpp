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
  v.push_back(std::make_unique<XVenue>());
  v.push_back(std::make_unique<WeatherNws>(params("weather_nws"), cfg.user_agent));
  v.push_back(std::make_unique<CryptoLognormal>(params("crypto_lognormal"), cfg.user_agent));
  v.push_back(std::make_unique<SportsTotals>(params("sports_totals")));
  v.push_back(std::make_unique<SportsInplay>(params("sports_inplay"), cfg.user_agent));
  v.push_back(std::make_unique<SportsLive>(params("sports_live"), cfg.user_agent));
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
