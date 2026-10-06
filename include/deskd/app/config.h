#pragma once
// deskd's settings. The file is the old desk's format (config/desk.example.json: JSON with
// comments, ~ expanded), so one config can serve both while they overlap: deskd reads the
// top-level key_id, pem_path and env the old desk reads, and its own "deskd" section.
//
// Live trading is a command-line flag only (--live). A file can never turn it on, so a copied
// or edited config cannot make a test run send to the production hosts.
#include <cstdint>
#include <string>

#include "deskd/bus/units.h"

namespace deskd {

struct DeskdConfig {
  std::string config_path;           // "" = defaults only
  // top level, shared with the old desk
  std::string env;                   // as written; deskd's hosts follow --live, never this
  std::string key_id;
  std::string pem_path;
  // "deskd": { ... }
  std::string data_dir = "data";     // holds kalshi-account.lock and deskd.heartbeat
  std::string ui_dir;                // "" = the repo's ui/desk (served from disk, no rebuild)
  int http_port = 8788;              // not the old desk's 8787: both may run while M0-M5 overlap
  int web_threads = 16;              // 9 threads once all sat in idle keep-alives: a 4.8 s stall
  int keepalive_s = 2;
  int max_streams = 4;               // SSE streams; leaves 12 of the 16 threads for requests
  int heartbeat_window_ms = 5000;    // gw, md-k and priv-k must each move within this
  Money daily_stop = 30 * 10000;     // u; decision 1, the recommended $30
  // from the command line only
  bool live = false;
};

// Reads path over the defaults. Throws std::runtime_error naming the file and field on a
// missing file, bad JSON, a wrong type, an unknown "deskd" key, or "live" in the file.
DeskdConfig load_deskd_config(const std::string& path);

std::string expand_home(const std::string& path);  // "~/x" -> "$HOME/x", as src/config.cpp

}  // namespace deskd
