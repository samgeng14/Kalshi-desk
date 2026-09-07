#include "desk/config.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace desk {
using json = nlohmann::json;

std::string expand_home(const std::string& path) {
  if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + path.substr(1);
  }
  return path;
}

template <typename T>
static void take(const json& j, const char* key, T& out) {
  auto it = j.find(key);
  if (it != j.end() && !it->is_null()) out = it->get<T>();
}

Config load_config(const std::string& path) {
  std::ifstream in(expand_home(path));
  if (!in) throw std::runtime_error("cannot open config: " + path);
  json j = json::parse(in, nullptr, true, true);  // allow comments
  Config c;
  take(j, "env", c.env);
  take(j, "key_id", c.key_id);
  take(j, "pem_path", c.pem_path);
  take(j, "db_path", c.db_path);
  take(j, "user_agent", c.user_agent);
  take(j, "http_port", c.http_port);
  take(j, "poll_seconds", c.poll_seconds);
  take(j, "reconcile_every", c.reconcile_every);
  take(j, "series_refresh_minutes", c.series_refresh_minutes);
  take(j, "bankroll_dollars", c.bankroll_dollars);
  if (j.contains("risk")) {
    const json& r = j["risk"];
    take(r, "kelly_frac", c.risk.kelly_frac);
    take(r, "max_event_frac", c.risk.max_event_frac);
    take(r, "max_cluster_frac", c.risk.max_cluster_frac);
    take(r, "max_open_frac", c.risk.max_open_frac);
    take(r, "daily_loss_stop_frac", c.risk.daily_loss_stop_frac);
    take(r, "min_edge_maker", c.risk.min_edge_maker);
    take(r, "min_edge_taker", c.risk.min_edge_taker);
    take(r, "sigma_mult", c.risk.sigma_mult);
    take(r, "min_price", c.risk.min_price);
    take(r, "proposal_ttl_seconds", c.risk.proposal_ttl_seconds);
    take(r, "max_disagreement", c.risk.max_disagreement);
    take(r, "ladder_min_edge", c.risk.ladder_min_edge);
    take(r, "instant_edge", c.risk.instant_edge);
    take(r, "instant_max_cost", c.risk.instant_max_cost);
    take(r, "shard_topup_on_demand", c.risk.shard_topup_on_demand);
    take(r, "shard_topup_max", c.risk.shard_topup_max);
    take(r, "hedge_auto_fraction", c.risk.hedge_auto_fraction);
    take(r, "hedge_trigger_dollars", c.risk.hedge_trigger_dollars);
    take(r, "hedge_price_limit", c.risk.hedge_price_limit);
    if (r.contains("auto_budget_category") && r["auto_budget_category"].is_object())
      for (auto& [k, v] : r["auto_budget_category"].items())
        if (v.is_number()) c.risk.auto_budget_category[k] = v.get<double>();
    if (r.contains("auto_budget_shard") && r["auto_budget_shard"].is_object())
      for (auto& [k, v] : r["auto_budget_shard"].items())
        if (v.is_number()) c.risk.auto_budget_shard[std::atoi(k.c_str())] = v.get<double>();
    take(r, "ladder_max_cost", c.risk.ladder_max_cost);
    take(r, "max_auto_dollars_per_day", c.risk.max_auto_dollars_per_day);
    take(r, "day_tz", c.risk.day_tz);
    if (r.contains("shard_topup") && r["shard_topup"].is_object())
      for (auto& [k, v] : r["shard_topup"].items()) c.risk.shard_topup[std::atoi(k.c_str())] = v.get<double>();
    take(r, "max_orders_per_cycle", c.risk.max_orders_per_cycle);
  }
  take(j, "watch_series", c.watch_series);
  if (j.contains("strategies") && j["strategies"].is_object()) {
    for (auto& [k, v] : j["strategies"].items()) {
      if (v.is_string()) c.strategy_modes[k] = v.get<std::string>();
      else if (v.is_object()) {
        c.strategy_modes[k] = v.value("mode", "off");
        c.strategy_params[k] = v;
      }
    }
  }
  if (j.contains("params") && j["params"].is_object()) {
    for (auto& [k, v] : j["params"].items()) c.strategy_params[k] = v;
  }
  c.pem_path = expand_home(c.pem_path);
  c.db_path = expand_home(c.db_path);
  if (c.env != "demo" && c.env != "prod") throw std::runtime_error("env must be demo or prod");
  if (c.poll_seconds < 1) c.poll_seconds = 1;
  return c;
}

json config_to_json(const Config& c) {
  json r{{"env", c.env},
         {"key_id_set", !c.key_id.empty()},
         {"pem_set", !c.pem_path.empty()},
         {"db_path", c.db_path},
         {"http_port", c.http_port},
         {"poll_seconds", c.poll_seconds},
         {"bankroll_dollars", c.bankroll_dollars},
         {"watch_series", c.watch_series},
         {"strategies", c.strategy_modes},
         {"risk",
          {{"kelly_frac", c.risk.kelly_frac},
           {"max_event_frac", c.risk.max_event_frac},
           {"max_cluster_frac", c.risk.max_cluster_frac},
           {"max_open_frac", c.risk.max_open_frac},
           {"daily_loss_stop_frac", c.risk.daily_loss_stop_frac},
           {"min_edge_maker", c.risk.min_edge_maker},
           {"min_edge_taker", c.risk.min_edge_taker},
           {"sigma_mult", c.risk.sigma_mult},
           {"min_price", c.risk.min_price},
           {"proposal_ttl_seconds", c.risk.proposal_ttl_seconds},
           {"max_disagreement", c.risk.max_disagreement},
           {"ladder_min_edge", c.risk.ladder_min_edge},
           {"instant_edge", c.risk.instant_edge},
           {"instant_max_cost", c.risk.instant_max_cost},
           {"shard_topup_on_demand", c.risk.shard_topup_on_demand},
           {"shard_topup_max", c.risk.shard_topup_max},
           {"hedge_auto_fraction", c.risk.hedge_auto_fraction},
           {"hedge_trigger_dollars", c.risk.hedge_trigger_dollars},
           {"hedge_price_limit", c.risk.hedge_price_limit},
           {"ladder_max_cost", c.risk.ladder_max_cost},
           {"max_auto_dollars_per_day", c.risk.max_auto_dollars_per_day},
           {"day_tz", c.risk.day_tz},
           {"shard_topup", [&] { json o = json::object(); for (auto& [k, v] : c.risk.shard_topup) o[std::to_string(k)] = v; return o; }()},
           {"max_orders_per_cycle", c.risk.max_orders_per_cycle}}}};
  return r;
}

}  // namespace desk
