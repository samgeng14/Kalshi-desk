// deskd ops and guards: the account lock (a second holder is refused; a missing data dir is
// never created), the heartbeat rule and file, config loading, the empty-subscription guard,
// and the send guard that keeps production hosts closed without --live and the lock.
#include <sys/stat.h>

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

#include "check.h"
#include "deskd/app/config.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/ops.h"
#include "deskd/app/worker.h"
#include "deskd/md/feed.h"
#include "deskd/md/subscribe.h"
#include "deskd/net/guard.h"

using namespace deskd;
using namespace std::chrono_literals;

static std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static bool exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

static void test_lock() {
  TempDir d;
  std::string err;
  auto a = AccountLock::acquire(d.path, &err);
  CHECK(a && a->held());
  CHECK(a && a->path() == d.file("kalshi-account.lock"));
  CHECK(slurp(d.file("kalshi-account.lock")).find("pid ") == 0);
  // a second holder is refused, and told who holds it (flock is per open file: a second open in
  // this process is refused the same way another process is)
  auto b = AccountLock::acquire(d.path, &err);
  CHECK(!b);
  CHECK(err.find("held by another process") != std::string::npos);
  CHECK(err.find("pid ") != std::string::npos);
  a.reset();
  auto c = AccountLock::acquire(d.path, &err);
  CHECK(c && c->held());
  // a missing data dir is refused and NOT created: a fresh private lock would protect nothing
  const std::string missing = d.file("no-such-dir");
  CHECK(!AccountLock::acquire(missing, &err));
  CHECK(err.find("does not exist") != std::string::npos);
  CHECK(!exists(missing));
}

static void test_heartbeat_rule() {
  const std::int64_t t0 = 1791306000000;
  HeartbeatRule r(5000);
  r.observe("gw", 1, t0);
  r.observe("md-k", 1, t0);
  r.observe("priv-k", 1, t0);
  CHECK(r.withheld(t0).empty());
  CHECK(r.withheld(t0 + 5000).empty());              // the start-up grace: first sight counts as a move
  r.observe("gw", 9, t0 + 4000);
  r.observe("priv-k", 9, t0 + 4000);
  r.observe("md-k", 1, t0 + 4000);                   // md-k stuck
  CHECK(r.withheld(t0 + 4900).empty());
  CHECK(r.withheld(t0 + 6000) == "md-k silent 6.0 s");
  r.observe("gw", 9, t0 + 9500);                     // gw stuck too
  r.observe("priv-k", 10, t0 + 9500);
  CHECK(r.withheld(t0 + 9500) == "gw silent 5.5 s, md-k silent 9.5 s");
  CHECK(r.silent_names(t0 + 9500) == "gw,md-k");
  CHECK(r.silent_names(t0 + 9900) == "gw,md-k");   // the log line does not change with the seconds
  r.observe("md-k", 2, t0 + 9600);
  r.observe("gw", 10, t0 + 9600);
  CHECK(r.withheld(t0 + 9600).empty());              // both moved: written again

  TempDir d;
  std::string err;
  CHECK(write_heartbeat(d.file("deskd.heartbeat"), 1791306000, &err));
  CHECK(slurp(d.file("deskd.heartbeat")) == "1791306000\n");
  CHECK(!exists(d.file("deskd.heartbeat.tmp")));
  CHECK(!write_heartbeat(d.file("missing/deskd.heartbeat"), 1, &err));
  CHECK(!err.empty());
  CHECK(disk_free_bytes(d.path).has_value());
}

// The real ops thread over real workers: the file is written while gw, md-k and priv-k beat,
// and withheld within the window once one of them stops.
static void test_ops_thread() {
  TempDir d;
  Feeds feeds;
  Worker gw("gw"), md("md-k"), priv("priv-k"), ops_w("ops");
  auto beat = [](Worker& w) {
    while (!w.stopping()) {
      w.beat();
      std::this_thread::sleep_for(50ms);
    }
  };
  gw.start(beat);
  md.start(beat);
  priv.start(beat);
  Ops ops(d.path, 1000, {&gw, &md, &priv}, feeds);
  // week 0's rows: every check but DISK waits for a venue; FEED and PRIV read the feeds
  {
    auto rows = ops.preflight();
    CHECK(rows.size() == 8);
    bool feed_down = false;
    for (const auto& r : rows)
      if (r.id == "FEED") feed_down = !r.ok && r.why == "KX down";
    CHECK(feed_down);
  }
  ops_w.start([&ops](Worker& w) { ops.run(w); });
  bool written = false;
  for (int i = 0; i < 40 && !written; ++i) {
    std::this_thread::sleep_for(50ms);
    written = exists(ops.heartbeat_path());
  }
  CHECK(written);
  CHECK(ops.heartbeat_withheld().empty());
  md.request_stop();   // md-k's loop ends: its counter stops
  md.join();
  bool withheld = false;
  for (int i = 0; i < 60 && !withheld; ++i) {
    std::this_thread::sleep_for(50ms);
    withheld = ops.heartbeat_withheld().find("md-k silent") == 0;
  }
  CHECK(withheld);
  const std::int64_t last = ops.heartbeat_written_ms();
  std::this_thread::sleep_for(1200ms);
  CHECK(ops.heartbeat_written_ms() == last);   // not written while withheld
  CHECK(now_ms() - last >= 1200);
  feeds.KX.set(true, false, 30);
  std::this_thread::sleep_for(1100ms);
  for (const auto& r : ops.preflight())
    if (r.id == "FEED") CHECK(r.ok);
  ops_w.request_stop();
  gw.request_stop();
  priv.request_stop();
}

static void test_config() {
  TempDir d;
  auto write = [&d](const std::string& text) {
    std::ofstream(d.file("c.json")) << text;
    return d.file("c.json");
  };
  // the old desk's format: comments, top-level key fields, and deskd's own section
  DeskdConfig c = load_deskd_config(write(R"({
    // a comment, as config/desk.example.json has
    "env": "prod", "key_id": "kid", "pem_path": "~/k.pem",
    "risk": {"kelly_frac": 0.25},
    "deskd": {"http_port": 9999, "data_dir": "~/deskd-data", "max_streams": 3, "daily_stop_dollars": 25}
  })"));
  CHECK(c.env == "prod");
  CHECK(c.key_id == "kid");
  CHECK(c.pem_path.find("~") == std::string::npos);
  CHECK(c.http_port == 9999);
  CHECK(c.max_streams == 3);
  CHECK(c.daily_stop == 250000);
  CHECK(c.data_dir.find("~") == std::string::npos);
  CHECK(!c.live);   // env "prod" never makes deskd live
  CHECK(c.web_threads == 16 && c.keepalive_s == 2);
  // defaults without a deskd section
  DeskdConfig e = load_deskd_config(write(R"({"env": "demo"})"));
  CHECK(e.http_port == 8788 && e.data_dir == "data" && e.max_streams == 4 && e.daily_stop == 300000);
  auto throws = [&](const std::string& text, const std::string& want) {
    try {
      load_deskd_config(write(text));
    } catch (const std::runtime_error& ex) {
      return std::string(ex.what()).find(want) != std::string::npos;
    }
    return false;
  };
  CHECK(throws(R"({"deskd": {"live": true}})", "command-line flag only"));
  CHECK(throws(R"({"deskd": {"heartbeat_window": 5000}})", "unknown key \"heartbeat_window\""));
  CHECK(throws(R"({"deskd": {"http_port": "8788"}})", "\"http_port\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"max_streams": 15}})", "max_streams"));
  CHECK(throws(R"({"deskd": [1]})", "not an object"));
  CHECK(throws(R"({"env": )", "c.json"));
  bool missing = false;
  try {
    load_deskd_config(d.file("nope.json"));
  } catch (const std::runtime_error&) {
    missing = true;
  }
  CHECK(missing);
}

static void test_subscribe_guard() {
  std::string err;
  // the invariant: an empty list for market data is refused, never sent
  CHECK(kalshi_subscribe(1, {"orderbook_delta"}, {}, &err).empty());
  CHECK(err.find("empty ticker list") != std::string::npos);
  CHECK(kalshi_subscribe(1, {"ticker", "trade"}, {}, &err).empty());
  CHECK(kalshi_subscribe(1, {"market_lifecycle_v2"}, {}, &err).empty());
  const std::string ok = kalshi_subscribe(7, {"orderbook_delta"}, {"KXNATGAS15M-26OCT061930-30"}, &err);
  CHECK(!ok.empty());
  auto j = nlohmann::json::parse(ok);
  CHECK(j["id"] == 7 && j["cmd"] == "subscribe");
  CHECK(j["params"]["market_tickers"].size() == 1);
  // account channels are scoped to the key: no list needed, none sent
  const std::string acct = kalshi_subscribe(2, {"fill", "user_orders"}, {}, &err);
  CHECK(!acct.empty());
  CHECK(!nlohmann::json::parse(acct)["params"].contains("market_tickers"));
  CHECK(kalshi_account_channel("order_group_updates"));
  CHECK(!kalshi_account_channel("orderbook_delta"));
  CHECK(kalshi_subscribe(3, {"fill", "orderbook_delta"}, {"KXA"}, &err).empty());   // mixed
  CHECK(kalshi_subscribe(3, {"orderbook_snapshot"}, {"KXA"}, &err).empty());        // unknown
  CHECK(kalshi_subscribe(3, {}, {"KXA"}, &err).empty());
  CHECK(kalshi_subscribe(3, {"trade"}, {"KXA", "KXA"}, &err).empty());
  CHECK(kalshi_subscribe(3, {"trade"}, {"KX A"}, &err).empty());
  CHECK(kalshi_subscribe(3, {"trade"}, {""}, &err).empty());
}

static void test_send_guard() {
  CHECK(classify_host("api.elections.kalshi.com") == HostClass::PRODUCTION);
  CHECK(classify_host("external-api.kalshi.com") == HostClass::PRODUCTION);
  CHECK(classify_host("external-api-ws.kalshi.com.") == HostClass::PRODUCTION);   // trailing dot
  CHECK(classify_host("API.Elections.KALSHI.com") == HostClass::PRODUCTION);
  CHECK(classify_host("gateway.polymarket.us") == HostClass::PRODUCTION);
  CHECK(classify_host("api.polymarket.us") == HostClass::PRODUCTION);
  CHECK(classify_host("clob.polymarket.com") == HostClass::PRODUCTION);
  CHECK(classify_host("something.kalshi.co") == HostClass::PRODUCTION);   // unknown Kalshi: never assumed demo
  CHECK(classify_host("external-api.demo.kalshi.co") == HostClass::DEMO);
  CHECK(classify_host("external-api-ws.demo.kalshi.co") == HostClass::DEMO);
  CHECK(classify_host("demo-api.kalshi.co") == HostClass::DEMO);
  CHECK(classify_host("127.0.0.1") == HostClass::LOOPBACK);
  CHECK(classify_host("localhost") == HostClass::LOOPBACK);
  CHECK(classify_host("kalshi.com.example.net") == HostClass::UNKNOWN);
  CHECK(classify_host("evilkalshi.com") == HostClass::UNKNOWN);
  CHECK(classify_host("api.elections.kalshi.com:443") == HostClass::UNKNOWN);
  CHECK(classify_host("") == HostClass::UNKNOWN);
  CHECK(classify_host("api.elections.kalshi.com/x") == HostClass::UNKNOWN);

  TempDir d;
  std::string err;
  auto lock = AccountLock::acquire(d.path, &err);
  CHECK(lock && lock->held());
  const SendGuard demo(false, lock.get()), live_no_lock(true, nullptr), live(true, lock.get());
  CHECK(demo.refuse("api.elections.kalshi.com").find("--live") != std::string::npos);
  CHECK(demo.refuse("gateway.polymarket.us").find("--live") != std::string::npos);
  CHECK(live_no_lock.refuse("api.elections.kalshi.com").find("account lock") != std::string::npos);
  CHECK(live.refuse("api.elections.kalshi.com").empty());
  CHECK(demo.refuse("external-api.demo.kalshi.co").empty());
  CHECK(demo.refuse("127.0.0.1").empty());
  CHECK(!demo.refuse("example.com").empty());
  CHECK(!live.refuse("example.com").empty());
}

int main() {
  test_lock();
  test_heartbeat_rule();
  test_ops_thread();
  test_config();
  test_subscribe_guard();
  test_send_guard();
  return finish("deskd-ops");
}
