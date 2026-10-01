#include "desk/config.h"
#include <algorithm>
#include "desk/log.h"
#include "desk/universe.h"

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

// config/desk.json -> config/weather_discovered.json, beside it whatever it is called.
static std::string discovered_path(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) +
         "weather_discovered.json";
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
  take(j, "market_data_max_age_seconds", c.market_data_max_age_seconds);
  take(j, "rest_min_interval_ms", c.rest_min_interval_ms);
  take(j, "refresh_threads", c.refresh_threads);
  take(j, "cycle_report_seconds", c.cycle_report_seconds);
  take(j, "max_consecutive_cycle_failures", c.max_consecutive_cycle_failures);
  take(j, "commodity_book_retain_days", c.commodity_book_retain_days);
  take(j, "bankroll_dollars", c.bankroll_dollars);
  if (j.contains("risk")) {
    const json& r = j["risk"];
    take(r, "kelly_frac", c.risk.kelly_frac);
    take(r, "max_event_frac", c.risk.max_event_frac);
    take(r, "max_cluster_frac", c.risk.max_cluster_frac);
    take(r, "max_open_frac", c.risk.max_open_frac);
    take(r, "daily_loss_stop_frac", c.risk.daily_loss_stop_frac);
    take(r, "daily_loss_stop_enabled", c.risk.daily_loss_stop_enabled);
    take(r, "min_edge_maker", c.risk.min_edge_maker);
    take(r, "min_edge_taker", c.risk.min_edge_taker);
    take(r, "sigma_mult", c.risk.sigma_mult);
    take(r, "min_price", c.risk.min_price);
    take(r, "manual_review_seconds", c.risk.manual_review_seconds);
    take(r, "proposal_ttl_seconds", c.risk.proposal_ttl_seconds);
    take(r, "max_order_dollars", c.risk.max_order_dollars);
    take(r, "max_turnover_dollars_per_day", c.risk.max_turnover_dollars_per_day);
    take(r, "max_entry_horizon_hours", c.risk.max_entry_horizon_hours);
    take(r, "add_min_edge", c.risk.add_min_edge);
    take(r, "add_edge_improvement", c.risk.add_edge_improvement);
    take(r, "add_cooldown_seconds", c.risk.add_cooldown_seconds);
    take(r, "auto_additions", c.risk.auto_additions);
    take(r, "kelly_min_samples", c.risk.kelly_min_samples);
    take(r, "uncalibrated_max_order_frac", c.risk.uncalibrated_max_order_frac);
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
    take(r, "ladder_max_baskets_per_cycle", c.risk.ladder_max_baskets_per_cycle);
    take(r, "max_auto_dollars_per_day", c.risk.max_auto_dollars_per_day);
    take(r, "day_tz", c.risk.day_tz);
    if (r.contains("shard_topup") && r["shard_topup"].is_object())
      for (auto& [k, v] : r["shard_topup"].items()) c.risk.shard_topup[std::atoi(k.c_str())] = v.get<double>();
    take(r, "max_orders_per_cycle", c.risk.max_orders_per_cycle);
  }
  take(j, "watch_series", c.watch_series);
  take(j, "basket_series", c.basket_series);
  take(j, "basket_refresh_every", c.basket_refresh_every);
  if (j.contains("strategies") && j["strategies"].is_object()) {
    for (auto& [k, v] : j["strategies"].items()) {
      if (v.is_string()) c.strategy_modes[k] = v.get<std::string>();
      else if (v.is_object()) {
        c.strategy_modes[k] = v.value("mode", "off");
        c.strategy_params[k] = v;
      }
    }
  }
  // The cross-venue fair (Polymarket US book on matched game markets) is on unless the config
  // says otherwise; it covers nothing until Polymarket is configured and a pair is found.
  if (!c.strategy_modes.count("xvenue")) c.strategy_modes["xvenue"] = "manual";
  // Same for the 15-minute commodity models. A strategy with no mode is "off", and an off
  // strategy is skipped before prepare(), so leaving these unlisted would run the settlement
  // socket, record the feed, and price nothing - a feature that looks installed and is not. They
  // are manual here and manual in the router's model_routes: offered, never sent.
  for (const char* m : {"commodity_terminal_gold", "commodity_terminal_silver",
                        "commodity_terminal_wti", "commodity_terminal_natgas",
                        "commodity_terminal_copper"})
    if (!c.strategy_modes.count(m)) c.strategy_modes[m] = "manual";
  if (j.contains("systematic") && j["systematic"].is_object()) c.systematic_settings = j["systematic"];
  if (j.contains("params") && j["params"].is_object()) {
    for (auto& [k, v] : j["params"].items()) c.strategy_params[k] = v;
  }
  // Cities discovered at runtime, in their own file. research/wx_discover.py rewrites it daily;
  // keeping it separate from desk.json means discovery never has to edit a file holding the API
  // key, and a bad discovery run can be deleted rather than unpicked from hand-written config.
  // Absent or unreadable is normal and silent: the desk simply covers what it covered before.
  {
    const std::string dpath = expand_home(discovered_path(path));
    std::ifstream din(dpath);
    if (din) {
      json d = json::parse(din, nullptr, false);
      if (d.is_object()) {
        if (d.contains("settlement_specs")) add_settlement_specs(d["settlement_specs"]);
        if (d.contains("stations") && d["stations"].is_object()) {
          json& wx = c.strategy_params["weather_nws"];
          if (!wx.is_object()) wx = json::object();
          json& st = wx["stations"];
          if (!st.is_object()) st = json::object();
          // A hand-written station entry wins: discovery adds cities, it never re-points one.
          for (auto& [series, v] : d["stations"].items())
            if (!st.contains(series)) st[series] = v;
        }
        // Only series whose settlement mapping actually resolved get watched. A discovered series
        // with no spec would be polled every cycle and refused at the gate every cycle.
        for (const auto& series : d.value("watch", json::array())) {
          if (!series.is_string()) continue;
          const std::string s2 = series.get<std::string>();
          if (!settlement_for(s2)) continue;
          if (std::find(c.watch_series.begin(), c.watch_series.end(), s2) == c.watch_series.end())
            c.watch_series.push_back(s2);
        }
      } else {
        LOG_WARN("discovered weather config at %s is not readable JSON; ignoring", dpath.c_str());
      }
    }
  }
  c.pem_path = expand_home(c.pem_path);
  c.db_path = expand_home(c.db_path);
  if (c.env != "demo" && c.env != "prod") throw std::runtime_error("env must be demo or prod");
  if (c.poll_seconds < 1) c.poll_seconds = 1;
  if (c.market_data_max_age_seconds < 0) c.market_data_max_age_seconds = 0;
  // 50 ms is 20 requests a second, the Basic tier's read ceiling at 10 tokens a request.
  c.rest_min_interval_ms = std::clamp(c.rest_min_interval_ms, 50, 2000);
  c.refresh_threads = std::clamp(c.refresh_threads, 1, 16);
  c.basket_refresh_every = std::clamp(c.basket_refresh_every, 1, 4);
  c.max_consecutive_cycle_failures = std::clamp(c.max_consecutive_cycle_failures, 1, 1000);
  if (c.commodity_book_retain_days < 0) c.commodity_book_retain_days = 0;
  return c;
}

json config_to_json(const Config& c) {
  json r{{"env", c.env},
         {"key_id_set", !c.key_id.empty()},
         {"pem_set", !c.pem_path.empty()},
         {"db_path", c.db_path},
         {"http_port", c.http_port},
         {"poll_seconds", c.poll_seconds},
         {"market_data_max_age_seconds", c.data_max_age()},
         {"bankroll_dollars", c.bankroll_dollars},
         {"watch_series", c.watch_series},
         {"basket_series", c.basket_series},
         {"strategies", c.strategy_modes},
         {"risk",
          {{"kelly_frac", c.risk.kelly_frac},
           {"max_event_frac", c.risk.max_event_frac},
           {"max_cluster_frac", c.risk.max_cluster_frac},
           {"max_open_frac", c.risk.max_open_frac},
           {"daily_loss_stop_frac", c.risk.daily_loss_stop_frac},
           {"daily_loss_stop_enabled", c.risk.daily_loss_stop_enabled},
           {"min_edge_maker", c.risk.min_edge_maker},
           {"min_edge_taker", c.risk.min_edge_taker},
           {"sigma_mult", c.risk.sigma_mult},
           {"min_price", c.risk.min_price},
           {"proposal_ttl_seconds", c.risk.proposal_ttl_seconds},
           {"max_order_dollars", c.risk.max_order_dollars},
           {"max_turnover_dollars_per_day", c.risk.max_turnover_dollars_per_day},
           {"max_entry_horizon_hours", c.risk.max_entry_horizon_hours},
           {"add_min_edge", c.risk.add_min_edge},
           {"add_edge_improvement", c.risk.add_edge_improvement},
           {"add_cooldown_seconds", c.risk.add_cooldown_seconds},
           {"auto_additions", c.risk.auto_additions},
           {"kelly_min_samples", c.risk.kelly_min_samples},
           {"uncalibrated_max_order_frac", c.risk.uncalibrated_max_order_frac},
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
