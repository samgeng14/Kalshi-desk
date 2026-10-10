#include "deskd/app/config.h"

#include <cmath>
#include <cstdlib>
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
  // "heartbeat_window_ms" would otherwise leave the supervisor's rule unchanged.
  static const std::set<std::string> known = {"data_dir",    "lock_dir",    "ui_dir",
                                              "http_port",   "web_threads", "keepalive_s",
                                              "max_streams", "heartbeat_window_ms", "daily_stop_dollars",
                                              "manual_orders"};
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
  double stop = -1;
  take(d, "daily_stop_dollars", stop, where);
  if (stop >= 0) c.daily_stop = static_cast<Money>(std::llround(stop * 10000.0));
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

}  // namespace deskd
