#pragma once
// deskd's settings. The file is the old desk's format (config/desk.example.json: JSON with
// comments, ~ expanded), so one config can serve both while they overlap: deskd reads the
// top-level key_id, pem_path and env the old desk reads, and its own "deskd" section.
//
// Live trading is a command-line flag only (--live). A file can never turn it on, so a copied
// or edited config cannot make a test run send to the production hosts.
#include <cstdint>
#include <string>
#include <vector>

#include "deskd/bus/units.h"

namespace deskd {

struct DeskdConfig {
  std::string config_path;           // "" = defaults only
  // top level, shared with the old desk
  std::string env;                   // as written; deskd's hosts follow --live, never this
  std::string key_id;
  std::string pem_path;
  // "rsa" (the old desk's keys; the default, so its configs still load) or "ed25519" (the demo
  // key of 2026-10-06; Kalshi allows them since 2026-10-01). gw/kalshi_sign.h checks it against
  // the PEM at load: a mismatch is refused, never signed with.
  std::string key_type = "rsa";
  // "deskd": { ... }
  std::string data_dir = "data";     // holds deskd.heartbeat; resolved to an absolute path at start
  // The account lock's directory (app/lock.h): "" = $HOME/Library/Application Support/
  // kalshi-desk/locks, the one every order-capable program uses. Absolute only; set it in tests.
  std::string lock_dir;
  std::string ui_dir;                // "" = the repo's ui/desk (served from disk, no rebuild)
  int http_port = 8788;              // not the old desk's 8787: both may run while M0-M5 overlap
  int web_threads = 16;              // 9 threads once all sat in idle keep-alives: a 4.8 s stall
  int keepalive_s = 2;
  int max_streams = 4;               // SSE streams; leaves 12 of the 16 threads for requests
  int heartbeat_window_ms = 5000;    // gw, md-k and priv-k must each move within this
  Money daily_stop = 30 * 10000;     // u; decision 1, the recommended $30
  Money equity_floor = 150 * 10000;  // u; plan section 4 limit 5: the account under it halts Systematic
  // The account's write rate limit tier (gw/write_budget.h): "basic" (100 tokens a second) until
  // the free Advanced upgrade is run, then "advanced" (300)
  std::string write_tier = "basic";
  // The data releases that pull the makers' quotes (strat/calendar.h); "" = none here, and deskd's
  // command line fills in the repo's config/calendar.json
  std::string calendar;
  // Manual orders to the (demo) venue. Off by default: gw still admits and answers, but every
  // order is refused RO and the header stays read-only until the operator turns it on.
  bool manual_orders = false;
  // Systematic orders to the (demo) venue (send-k2, order groups, lanes). Off by default: the
  // lanes show, Start stays refused (GROUPS) and no group is created until the operator turns
  // it on. Independent of manual_orders.
  bool systematic_orders = false;
  // The 15-minute series lane M1 (LIP-15) quotes; the plan starts with natural gas alone. A demo
  // account may list other series, or none: then M1 has nothing to quote.
  std::vector<std::string> lip_series = {"KXNATGAS15M"};
  // The series lane M2 (BAND 96-99) quotes: the plan's five weather cities first.
  std::vector<std::string> band_series = {"KXHIGHNY", "KXHIGHCHI", "KXHIGHMIA", "KXHIGHAUS", "KXHIGHLAX"};
  // The other desk the header's PAPER/LIVE tag switches to: the paper desk (demo account) and
  // the live one (production) are two deskd processes, each with its own account lock, data and
  // port. http://127.0.0.1:PORT/ or http://localhost:PORT/ only; "" = the other default port
  // (8788 paper, 8789 live; tools/desk.py starts them there).
  std::string peer_url;
  // from the command line only
  bool live = false;
};

// Reads path over the defaults. Throws std::runtime_error naming the file and field on a
// missing file, bad JSON, a wrong type, an unknown "deskd" key, or "live" in the file.
DeskdConfig load_deskd_config(const std::string& path);

std::string expand_home(const std::string& path);

// http://127.0.0.1:PORT/ or http://localhost:PORT/ (PORT 1-65535, the slash optional, nothing after)
bool peer_url_ok(const std::string& url);
// cfg.peer_url, else the other desk's default on 127.0.0.1: 8789 (live) from paper, 8788 from live
std::string peer_url_for(const DeskdConfig& cfg);  // "~/x" -> "$HOME/x", as src/config.cpp

}  // namespace deskd
