#include "deskd/md/subscribe.h"

#include <nlohmann/json.hpp>
#include <set>

namespace deskd {

namespace {
const std::set<std::string>& account_channels() {
  static const std::set<std::string> s = {"fill", "user_orders", "order_group_updates", "market_positions"};
  return s;
}
const std::set<std::string>& market_channels() {
  // market_lifecycle_v2 counts as market data: unscoped it is every listing on the exchange.
  static const std::set<std::string> s = {"orderbook_delta", "ticker", "trade", "market_lifecycle_v2"};
  return s;
}
bool ticker_ok(const std::string& t) {
  if (t.empty() || t.size() > 96) return false;   // contract RE_TK
  for (char c : t)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
          c == '-'))
      return false;
  return true;
}
}  // namespace

bool kalshi_account_channel(const std::string& channel) { return account_channels().count(channel) > 0; }

std::string kalshi_subscribe(int id, const std::vector<std::string>& channels,
                             const std::vector<std::string>& tickers, std::string* err) {
  auto refuse = [err](const std::string& why) {
    if (err) *err = why;
    return std::string();
  };
  if (channels.empty()) return refuse("no channels");
  int account = 0, market = 0;
  for (const auto& c : channels) {
    if (account_channels().count(c)) ++account;
    else if (market_channels().count(c)) ++market;
    else return refuse("unknown channel " + c);
  }
  if (account && market) return refuse("market data and account channels in one command");
  if (market && tickers.empty())
    return refuse("empty ticker list: Kalshi would send every market on the exchange");
  std::set<std::string> seen;
  for (const auto& t : tickers) {
    if (!ticker_ok(t)) return refuse("bad ticker \"" + t + "\"");
    if (!seen.insert(t).second) return refuse("ticker " + t + " twice");
  }
  nlohmann::json params = {{"channels", channels}};
  if (!tickers.empty()) params["market_tickers"] = tickers;
  return nlohmann::json{{"id", id}, {"cmd", "subscribe"}, {"params", params}}.dump();
}

}  // namespace deskd
