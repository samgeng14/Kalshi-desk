#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace desk {

struct RiskConfig {
  double kelly_frac = 0.25;        // fraction of full Kelly
  double max_event_frac = 0.10;    // max bankroll fraction per event (all brackets)
  double max_cluster_frac = 0.25;  // max bankroll fraction per correlated cluster
  double max_open_frac = 0.60;     // max bankroll fraction in open exposure
  double daily_loss_stop_frac = 0.10;
  double min_edge_maker = 0.03;    // dollars per contract, net of fee
  double min_edge_taker = 0.05;
  double sigma_mult = 1.0;         // require edge >= sigma_mult * sigma_model
  double min_price = 0.05;         // do not trade outside [min_price, 1-min_price]
  int proposal_ttl_seconds = 300;
  int max_orders_per_cycle = 5;
  // A simple model that disagrees with a liquid market by more than this is treated as the
  // one that is wrong: the fair is logged for research, nothing is proposed. All five of the
  // 2026-09-05 weather trades had |raw - mid| >= 0.34 and the market was right every time.
  double max_disagreement = 0.30;
  // Ladder arbitrage (see ladder.h): the smallest locked profit per contract worth legging into,
  // and the most cash one basket may use.
  double ladder_min_edge = 0.02;
  double ladder_max_cost = 20.0;
  // The fast lane. A proposal whose net edge reaches this goes in the cycle it appears, without
  // waiting for a click, whatever mode its strategy is in (off and research still never send).
  // 0 disables it. Every other guard still applies: AUTO must be armed, the kill switch, the
  // daily stop, the caps, the per-bucket budgets and instant_max_cost per order.
  double instant_edge = 0.0;
  double instant_max_cost = 5.0;
  // Unattended sends (auto/bot entries) may commit at most this much purchase cost per day.
  double max_auto_dollars_per_day = 20.0;
  // Same budget, split. An empty map means the bucket is not limited beyond the total; a bucket
  // present with 0 is switched off for unattended sends. Categories are Kalshi's own
  // ("Crypto", "Climate and Weather", "Sports", ...); shards are the exchange indexes.
  std::map<std::string, double> auto_budget_category;
  std::map<int, double> auto_budget_shard;
  // Automatic hedging. An event's worst case is left alone up to the trigger (that is the risk
  // you are being paid to take); above it, the desk covers this share of the excess by itself.
  double hedge_auto_fraction = 0.5;
  double hedge_trigger_dollars = 0;   // 0 = the per-event cap (max_event_frac * bankroll)
  // The most you will pay for protection, in dollars of premium per dollar of worst case
  // removed. The manual tab calls the risk left after applying this rule the optimal remainder.
  double hedge_price_limit = 0.35;
  // Local zone whose midnight resets "P&L today", the daily stop baseline and the auto budget.
  std::string day_tz = "America/Chicago";
  // Keep at least this much cash on an exchange shard (index -> dollars), moved from shard 0
  // through Kalshi's intra-account transfer when the balance falls short. {} = never transfer.
  std::map<int, double> shard_topup;
  // An order refused for want of collateral on a shard moves the shortfall from shard 0 by
  // itself (up to this much per attempt) instead of only telling you to go and do it.
  bool shard_topup_on_demand = true;
  double shard_topup_max = 25.0;
};

struct Config {
  std::string env = "demo";  // demo | prod
  std::string key_id;
  std::string pem_path;
  std::string db_path = "data/desk.sqlite";
  std::string user_agent = "kalshi-desk/0.1";
  int http_port = 8787;
  int poll_seconds = 5;
  int reconcile_every = 6;       // cycles
  int series_refresh_minutes = 60;
  double bankroll_dollars = 100.0;
  RiskConfig risk;
  std::vector<std::string> watch_series;
  std::map<std::string, std::string> strategy_modes;  // name -> off | research | manual | auto | bot
  nlohmann::json strategy_params;                      // free-form per-strategy params

  std::string rest_base() const {
    return env == "prod" ? "https://external-api.kalshi.com/trade-api/v2"
                         : "https://external-api.demo.kalshi.co/trade-api/v2";
  }
  std::string mode_for(const std::string& strategy) const {
    auto it = strategy_modes.find(strategy);
    return it == strategy_modes.end() ? "off" : it->second;
  }
};

Config load_config(const std::string& path);
nlohmann::json config_to_json(const Config& c);
std::string expand_home(const std::string& path);

}  // namespace desk
