#include "deskd/app/config.h"
#include <algorithm>
#include <cctype>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace deskd {

using json = nlohmann::json;

std::string expand_home(const std::string& path) {
  if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + path.substr(1);
  }
  return path;
}

namespace {
// src/config.cpp's take(), with the field named when the type is wrong instead of a bare
// nlohmann type_error.
template <typename T>
void take(const json& j, const char* key, T& out, const std::string& where) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  try {
    out = it->get<T>();
  } catch (const json::exception&) {
    throw std::runtime_error(where + ": \"" + key + "\" has the wrong type (" + it->type_name() + ")");
  }
}
// a Kalshi series ticker: upper-case letters and digits
bool series_ok(const std::string& s) {
  return !s.empty() && s.size() <= 40 &&
         std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isupper(ch) || std::isdigit(ch); });
}
// an amount written as a number of dollars, within [lo, hi] (out keeps its default when absent)
void take_dollars(const json& j, const char* key, Money& out, Money lo, Money hi, const std::string& where) {
  if (j.contains(key) && !j.at(key).is_null()) {
    double v = 0;
    take(j, key, v, where);
    out = static_cast<Money>(std::llround(v * 10000.0));
  }
  if (out < lo || out > hi)
    throw std::runtime_error(where + ": " + key + " is $" + money_str(lo) + " to $" + money_str(hi) + ", not $" + money_str(out));
}
// "college": C1's section, every key known and every amount inside its range
void take_college(const json& j, DeskdConfig::College& c, const std::string& where) {
  if (!j.is_object()) throw std::runtime_error(where + ": not an object");
  static const std::set<std::string> known = {
      "enabled",
      "series",
      "volley_model",
      "clip_dollars",
      "game_dollars",
      "lane_dollars",
      "max_score_age_s",
  };
  for (auto kv = j.begin(); kv != j.end(); ++kv)
    if (!known.count(kv.key())) throw std::runtime_error(where + ": unknown key \"" + kv.key() + "\"");
  take(j, "enabled", c.enabled, where);
  take(j, "series", c.series, where);
  take(j, "volley_model", c.volley_model, where);
  take(j, "max_score_age_s", c.max_score_age_s, where);
  c.volley_model = expand_home(c.volley_model);
  if (c.series.empty()) throw std::runtime_error(where + ": series is empty");
  for (const auto& s : c.series)
    if (!series_ok(s)) throw std::runtime_error(where + ": series has \"" + s.substr(0, 40) + "\", not a series ticker");
  take_dollars(j, "clip_dollars", c.clip, 1 * 10000, 5 * 10000, where);
  take_dollars(j, "game_dollars", c.game, c.clip, 15 * 10000, where);
  take_dollars(j, "lane_dollars", c.budget, c.game, 40 * 10000, where);
  if (c.max_score_age_s < 1 || c.max_score_age_s > 5)
    throw std::runtime_error(where + ": max_score_age_s is 1 to 5, not " + std::to_string(c.max_score_age_s));
}
// S1's keys (top level, as the NBA lanes' are): each inside its range, and trade mode only with
// what it needs; after systematic_orders
void take_arb(const json& d, DeskdConfig& c, const std::string& where) {
  take(d, "arb_mode", c.arb_mode, where);
  if (c.arb_mode != "off" && c.arb_mode != "scan" && c.arb_mode != "trade")
    throw std::runtime_error(where + ": arb_mode is \"off\", \"scan\" or \"trade\", not \"" + c.arb_mode.substr(0, 40) + "\"");
  take(d, "event_risk", c.event_risk, where);
  if (c.arb_mode == "trade" && !c.systematic_orders) throw std::runtime_error(where + ": arb_mode \"trade\" needs systematic_orders");
  if (c.arb_mode == "trade" && !c.event_risk)
    throw std::runtime_error(where + ": arb_mode \"trade\" needs event_risk (a basket counted by its settlement states)");
  if (d.contains("arb_series") && !d.at("arb_series").is_null()) {
    take(d, "arb_series", c.arb_series, where);
    if (c.arb_series.empty()) throw std::runtime_error(where + ": arb_series is empty");
  }
  for (const auto& s : c.arb_series)
    if (!series_ok(s)) throw std::runtime_error(where + ": arb_series has \"" + s.substr(0, 40) + "\", not a series ticker");
  double qty = qty_to_number(c.arb_max_qty);
  take(d, "arb_max_qty", qty, where);
  if (!(qty >= 1 && qty <= 20) || qty != std::floor(qty))
    throw std::runtime_error(where + ": arb_max_qty is 1 to 20 whole contracts, not " + d.at("arb_max_qty").dump());
  c.arb_max_qty = static_cast<Qty>(qty) * kOneContract;
  take(d, "arb_max_legs", c.arb_max_legs, where);
  if (c.arb_max_legs < 2 || c.arb_max_legs > 6)
    throw std::runtime_error(where + ": arb_max_legs is 2 to 6, not " + std::to_string(c.arb_max_legs));
  take_dollars(d, "arb_thr", c.arb_thr, 50, 1000, where);
  take_dollars(d, "arb_legging_pause", c.arb_legging_pause, 1, 5 * 10000, where);
  take(d, "arb_lattice", c.arb_lattice, where);   // as given: it replaces the default
  for (const auto& [prefix, step] : c.arb_lattice)
    if (!series_ok(prefix) || !std::isfinite(step) || step <= 0)
      throw std::runtime_error(where + ": arb_lattice maps a series prefix to a step over 0, not \"" + prefix.substr(0, 40) +
                               "\": " + d.at("arb_lattice").at(prefix).dump());
  double precision = 0.01;
  take(d, "fee_precision", precision, where);
  const Money fp = static_cast<Money>(std::llround(precision * 10000.0));
  if ((fp != 100 && fp != 1) || std::fabs(precision * 10000.0 - static_cast<double>(fp)) > 1e-6)
    throw std::runtime_error(where + ": fee_precision is 0.01 or 0.0001 (dollars), not " + d.at("fee_precision").dump());
  c.fee_precision = fp;
}
}  // namespace

DeskdConfig load_deskd_config(const std::string& path) {
  DeskdConfig c;
  c.config_path = path;
  std::ifstream in(expand_home(path));
  if (!in) throw std::runtime_error("cannot open config: " + path);
  json j;
  try {
    j = json::parse(in, nullptr, true, true);  // comments allowed, as the old desk's
  } catch (const json::exception& e) {
    throw std::runtime_error(path + ": " + e.what());
  }
  if (!j.is_object()) throw std::runtime_error(path + ": not a JSON object");
  take(j, "env", c.env, path);
  take(j, "key_id", c.key_id, path);
  take(j, "pem_path", c.pem_path, path);
  c.pem_path = expand_home(c.pem_path);
  take(j, "key_type", c.key_type, path);
  if (c.key_type != "rsa" && c.key_type != "ed25519")
    throw std::runtime_error(path + ": \"key_type\" must be \"rsa\" or \"ed25519\", not \"" + c.key_type + "\"");

  auto it = j.find("deskd");
  if (it == j.end() || it->is_null()) return c;
  const json& d = *it;
  const std::string where = path + " deskd";
  if (!d.is_object()) throw std::runtime_error(where + ": not an object");
  // A misspelt key is an error, not a silently ignored default: "heartbeat_window" for
  // "heartbeat_window_ms" would otherwise leave the supervisor's rule unchanged. One key a line,
  // so a new setting is a line added here, not a line rewritten.
  static const std::set<std::string> known = {
      "data_dir",
      "lock_dir",
      "ui_dir",
      "http_port",
      "web_threads",
      "keepalive_s",
      "max_streams",
      "heartbeat_window_ms",
      "daily_stop_dollars",
      "equity_floor_dollars",
      "manual_orders",
      "systematic_orders",
      "lip_series",
      "band_series",
      "paper_lane",
      "peer_url",
      "write_tier",
      "calendar",
      "wcbb_model",
      "gates_dir",
      "college",
      "nba_lanes",
      "nba_series",
      "nba_model",
      "lane_shadow",
      "arb_mode",
      "arb_series",
      "arb_max_qty",
      "arb_max_legs",
      "arb_thr",
      "arb_legging_pause",
      "arb_lattice",
      "event_risk",
      "fee_precision",
  };
  for (auto kv = d.begin(); kv != d.end(); ++kv) {
    if (kv.key() == "live")
      throw std::runtime_error(where + ": \"live\" is a command-line flag only (--live), never a file setting");
    if (!known.count(kv.key())) throw std::runtime_error(where + ": unknown key \"" + kv.key() + "\"");
  }
  take(d, "data_dir", c.data_dir, where);
  take(d, "lock_dir", c.lock_dir, where);
  take(d, "ui_dir", c.ui_dir, where);
  take(d, "http_port", c.http_port, where);
  take(d, "web_threads", c.web_threads, where);
  take(d, "keepalive_s", c.keepalive_s, where);
  take(d, "max_streams", c.max_streams, where);
  take(d, "heartbeat_window_ms", c.heartbeat_window_ms, where);
  take(d, "manual_orders", c.manual_orders, where);
  take(d, "systematic_orders", c.systematic_orders, where);
  take(d, "lip_series", c.lip_series, where);
  take(d, "band_series", c.band_series, where);
  take(d, "paper_lane", c.paper_lane, where);
  take(d, "peer_url", c.peer_url, where);
  take(d, "write_tier", c.write_tier, where);
  take(d, "calendar", c.calendar, where);
  take(d, "wcbb_model", c.wcbb_model, where);
  take(d, "gates_dir", c.gates_dir, where);
  if (d.contains("college") && !d.at("college").is_null()) take_college(d.at("college"), c.college, where + " college");
  take(d, "nba_lanes", c.nba_lanes, where);
  take(d, "nba_series", c.nba_series, where);
  take(d, "nba_model", c.nba_model, where);
  take(d, "lane_shadow", c.lane_shadow, where);
  if (c.nba_series.empty()) throw std::runtime_error(where + ": nba_series is empty");
  c.wcbb_model = expand_home(c.wcbb_model);
  c.nba_model = expand_home(c.nba_model);
  c.gates_dir = expand_home(c.gates_dir);
  c.calendar = expand_home(c.calendar);
  if (c.write_tier != "basic" && c.write_tier != "advanced")
    throw std::runtime_error(where + ": write_tier is \"basic\" or \"advanced\", not \"" + c.write_tier.substr(0, 40) + "\"");
  if (!c.peer_url.empty() && !peer_url_ok(c.peer_url))
    throw std::runtime_error(where + ": peer_url must be http://127.0.0.1:PORT/ or http://localhost:PORT/, not \"" +
                             c.peer_url.substr(0, 80) + "\"");
  for (const auto* list : {&c.lip_series, &c.band_series, &c.nba_series})
    for (const auto& s : *list)
      if (!series_ok(s))
        throw std::runtime_error(where + ": " + (list == &c.lip_series ? "lip_series" : list == &c.band_series ? "band_series" : "nba_series") +
                                 " has \"" + s + "\", not a series ticker");
  take_arb(d, c, where);
  double stop = -1;
  take(d, "daily_stop_dollars", stop, where);
  if (stop >= 0) c.daily_stop = static_cast<Money>(std::llround(stop * 10000.0));
  double floor = -1;
  take(d, "equity_floor_dollars", floor, where);
  if (floor >= 0) c.equity_floor = static_cast<Money>(std::llround(floor * 10000.0));
  c.data_dir = expand_home(c.data_dir);
  c.lock_dir = expand_home(c.lock_dir);
  // A relative lock dir would be a different file from every other working directory: the bug
  // the fixed path exists to stop (app/lock.h).
  if (!c.lock_dir.empty() && c.lock_dir[0] != '/') throw std::runtime_error(where + ": lock_dir must be an absolute path");
  c.ui_dir = expand_home(c.ui_dir);
  if (c.http_port < 0 || c.http_port > 65535) throw std::runtime_error(where + ": http_port out of range");
  if (c.web_threads < 4) throw std::runtime_error(where + ": web_threads below 4");
  if (c.max_streams < 1 || c.max_streams > c.web_threads - 2)
    throw std::runtime_error(where + ": max_streams must leave at least 2 web threads for requests");
  if (c.keepalive_s < 1) throw std::runtime_error(where + ": keepalive_s below 1");
  if (c.heartbeat_window_ms < 1000) throw std::runtime_error(where + ": heartbeat_window_ms below 1000");
  return c;
}

bool peer_url_ok(const std::string& url) {
  std::string rest;
  for (const char* prefix : {"http://127.0.0.1:", "http://localhost:"})
    if (url.compare(0, std::strlen(prefix), prefix) == 0) rest = url.substr(std::strlen(prefix));
  if (rest.empty()) return false;
  if (rest.back() == '/') rest.pop_back();
  if (rest.empty() || rest.size() > 5 || !std::all_of(rest.begin(), rest.end(), [](unsigned char ch) { return std::isdigit(ch); }))
    return false;
  const int port = std::stoi(rest);
  return port >= 1 && port <= 65535;
}

std::string peer_url_for(const DeskdConfig& cfg) {
  if (!cfg.peer_url.empty()) return cfg.peer_url.back() == '/' ? cfg.peer_url : cfg.peer_url + "/";
  return cfg.live ? "http://127.0.0.1:8788/" : "http://127.0.0.1:8789/";
}

double arb_lattice_step(const DeskdConfig& cfg, const std::string& series) {
  std::size_t best = 0;
  double step = 0;
  for (const auto& [prefix, s] : cfg.arb_lattice)
    if (prefix.size() > best && series.compare(0, prefix.size(), prefix) == 0) {
      best = prefix.size();
      step = s;
    }
  return step;
}

}  // namespace deskd
