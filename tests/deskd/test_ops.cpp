// deskd ops and guards: the account lock (one fixed absolute file per account; a second holder
// is refused; a relative or missing configured dir is refused, never created), the heartbeat rule
// and file (web's probe and every watched owner), the logger that never blocks a thread, config
// loading, the empty-subscription guard, and the send guard that keeps production hosts closed
// without --live and the production account's lock.
#include <cctype>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdlib>
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
  auto a = AccountLock::acquire(d.path, "demo", "deskd DEMO", false, &err);
  CHECK(a && a->held());
  char real[PATH_MAX];
  CHECK(::realpath(d.path.c_str(), real) != nullptr);
  CHECK(a && a->path() == std::string(real) + "/kalshi-demo.lock");   // the real path, logged as such
  CHECK(a && a->env() == "demo" && a->sibling(".killed") == std::string(real) + "/kalshi-demo.killed");
  CHECK(slurp(d.file("kalshi-demo.lock")).find("pid " + std::to_string(::getpid()) + " deskd DEMO since ") == 0);
  // a second holder is refused, and told who holds it (flock is per open file: a second open in
  // this process is refused the same way another process is), whatever path names the dir
  auto b = AccountLock::acquire(d.path, "demo", "other", false, &err);
  CHECK(!b);
  CHECK(err.find("held by another process") != std::string::npos);
  CHECK(err.find("deskd DEMO") != std::string::npos);
  auto b2 = AccountLock::acquire(d.path + "/./", "demo", "other", false, &err);
  CHECK(!b2);
  // the other account is another file: a non-live deskd never blocks (or holds) the live one
  auto p = AccountLock::acquire(d.path, "prod", "deskd LIVE", false, &err);
  CHECK(p && p->held() && p->env() == "prod");
  a.reset();
  auto c = AccountLock::acquire(d.path, "demo", "deskd DEMO", false, &err);
  CHECK(c && c->held());
  // a relative dir is refused: from another working directory it would be another file, the bug
  // of 2026-10-06 (two deskds with --data-dir data in two checkouts both came up)
  CHECK(!AccountLock::acquire("data", "demo", "x", false, &err));
  CHECK(err.find("not an absolute path") != std::string::npos);
  CHECK(!AccountLock::acquire("", "demo", "x", true, &err));
  CHECK(!AccountLock::acquire(d.path, "staging", "x", false, &err));
  CHECK(err.find("neither prod nor demo") != std::string::npos);
  // a missing configured dir is refused and NOT created: a fresh private lock would protect nothing
  const std::string missing = d.file("no-such-dir");
  CHECK(!AccountLock::acquire(missing, "demo", "x", false, &err));
  CHECK(err.find("does not exist") != std::string::npos);
  CHECK(!exists(missing));
  // the default dir is made if missing (create_dir), and lives under $HOME
  const std::string made = d.file("home/Library/Application Support/kalshi-desk/locks");
  auto m = AccountLock::acquire(made, "demo", "x", true, &err);
  CHECK(m && m->held() && exists(made + "/kalshi-demo.lock"));
  const char* home = std::getenv("HOME");
  const std::string saved = home ? home : "";
  ::setenv("HOME", d.file("home").c_str(), 1);
  CHECK(AccountLock::default_dir(&err) == made);
  ::unsetenv("HOME");
  CHECK(AccountLock::default_dir(&err).empty());
  CHECK(err.find("HOME") != std::string::npos);
  if (!saved.empty()) ::setenv("HOME", saved.c_str(), 1);
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

// The real ops thread over real workers: the file is written while gw, md-k and priv-k beat
// (their placeholder loops come round every 250 ms with no traffic at all: a quiet feed keeps
// the heartbeat), and withheld within the window once one of them stops.
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

// web has no loop of its own to beat: ops asks GET /api/health each second, and a web pool that
// stops answering withholds the heartbeat like a stuck owner, with a CRIT on the screen.
static void test_web_probe() {
  TempDir d;
  Feeds feeds;
  Worker gw("gw"), ops_w("ops");
  gw.start([](Worker& w) {
    while (!w.stopping()) {
      w.beat();
      std::this_thread::sleep_for(50ms);
    }
  });
  std::atomic<bool> answers{true};
  std::atomic<int> crit{0};
  Ops ops(d.path, 1000, {&gw}, feeds, [&answers] { return answers.load(); },
          [&crit](const std::string& lvl, const std::string& msg) {
            if (lvl == "CRIT" && msg.find("web silent") != std::string::npos) ++crit;
          });
  ops_w.start([&ops](Worker& w) { ops.run(w); });
  bool written = false;
  for (int i = 0; i < 40 && !written; ++i) {
    std::this_thread::sleep_for(50ms);
    written = ops.heartbeat_withheld().empty() && ops.heartbeat_written_ms() > 0;
  }
  CHECK(written);
  answers.store(false);
  bool withheld = false;
  for (int i = 0; i < 80 && !withheld; ++i) {
    std::this_thread::sleep_for(50ms);
    withheld = ops.heartbeat_withheld().find("web silent") == 0;
  }
  CHECK(withheld);
  CHECK(crit.load() == 1);
  CHECK(ops.web_probes_failed() >= 1);
  answers.store(true);
  bool back = false;
  for (int i = 0; i < 60 && !back; ++i) {
    std::this_thread::sleep_for(50ms);
    back = ops.heartbeat_withheld().empty();
  }
  CHECK(back);
  ops_w.request_stop();
  gw.request_stop();
  ops_w.join();
  gw.join();
}

// A thread that logs never blocks on stderr: with stderr a pipe nobody reads, 20,000 lines take
// well under a second, the ones past the queue are dropped and counted, and the logger says so
// once the pipe drains. Until 2026-10-06 every log line was a write(2) on the calling thread, so
// gw (it logs each refused order) and send-cx (each KILL) would have stopped on a full pipe.
static void test_logger_never_blocks() {
  int p[2];
  CHECK(::pipe(p) == 0);
  const int saved = ::dup(STDERR_FILENO);
  ::dup2(p[1], STDERR_FILENO);
  start_logger();
  start_logger();   // idempotent
  const std::uint64_t dropped0 = log_dropped();
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20000; ++i) DLOG_INFO("line %d of a flood nobody reads", i);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const std::uint64_t dropped = log_dropped() - dropped0;
  CHECK(ms < 1000);
  CHECK(dropped > 10000);   // the pipe holds 64 KiB, the queue 4,096 lines
  // now drain the pipe: the logger writes the rest, then one line with the drop count
  ::fcntl(p[0], F_SETFL, O_NONBLOCK);
  char buf[65536];
  std::string got;
  bool flushed = false;
  for (int i = 0; i < 500 && !flushed; ++i) {
    ssize_t n;
    while ((n = ::read(p[0], buf, sizeof buf)) > 0) got.append(buf, static_cast<std::size_t>(n));
    flushed = flush_logger(5);
  }
  ssize_t n;
  while ((n = ::read(p[0], buf, sizeof buf)) > 0) got.append(buf, static_cast<std::size_t>(n));
  ::dup2(saved, STDERR_FILENO);
  ::close(saved);
  std::printf("20,000 log lines into a full pipe: %.1f ms, %llu dropped\n", ms, static_cast<unsigned long long>(dropped));
  CHECK(flushed);
  CHECK(got.find("] main: line 0 of a flood nobody reads\n") != std::string::npos);   // whole lines
  // every drop is reported: in one line, or several when the logger woke during the flood (it
  // reports what was dropped so far each time it wakes; under load that splits the count)
  std::uint64_t reported = 0;
  for (std::size_t at = got.find(" log lines dropped"); at != std::string::npos; at = got.find(" log lines dropped", at + 1)) {
    std::size_t b = at;
    while (b > 0 && std::isdigit(static_cast<unsigned char>(got[b - 1]))) --b;
    if (b < at) reported += std::stoull(got.substr(b, at - b));
  }
  if (reported != dropped) std::fprintf(stderr, "  reported %llu of %llu dropped\n", static_cast<unsigned long long>(reported),
                                        static_cast<unsigned long long>(dropped));
  CHECK(reported == dropped);
  ::close(p[0]);
  ::close(p[1]);
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
  CHECK(c.key_type == "rsa");   // the old desk's configs name no key type: its keys are RSA
  // defaults without a deskd section
  DeskdConfig e = load_deskd_config(write(R"({"env": "demo"})"));
  CHECK(e.http_port == 8788 && e.data_dir == "data" && e.max_streams == 4 && e.daily_stop == 300000);
  CHECK(e.lock_dir.empty());   // the fixed default under $HOME
  // the demo key's type (~/.kalshi/deskd-demo.json); anything but rsa or ed25519 is refused
  DeskdConfig k = load_deskd_config(write(R"({"env": "demo", "key_id": "kid", "pem_path": "~/.kalshi/k.pem", "key_type": "ed25519"})"));
  CHECK(k.key_type == "ed25519");
  DeskdConfig l = load_deskd_config(write(R"({"deskd": {"lock_dir": "/tmp/locks"}})"));
  CHECK(l.lock_dir == "/tmp/locks");
  // Systematic: off by default; LIP-15's series, natural gas unless set
  CHECK(!e.systematic_orders && !e.manual_orders && e.lip_series == std::vector<std::string>{"KXNATGAS15M"});
  DeskdConfig sys = load_deskd_config(write(R"({"deskd": {"systematic_orders": true, "lip_series": ["KXGOLD15M", "KXNATGAS15M"]}})"));
  CHECK(sys.systematic_orders && sys.lip_series == (std::vector<std::string>{"KXGOLD15M", "KXNATGAS15M"}));
  CHECK(e.band_series.size() == 5 && e.band_series.front() == "KXHIGHNY");
  CHECK(load_deskd_config(write(R"({"deskd": {"band_series": ["KXHIGHDEN"]}})")).band_series == std::vector<std::string>{"KXHIGHDEN"});
  auto throws = [&](const std::string& text, const std::string& want) {
    try {
      load_deskd_config(write(text));
    } catch (const std::runtime_error& ex) {
      return std::string(ex.what()).find(want) != std::string::npos;
    }
    return false;
  };
  CHECK(throws(R"({"deskd": {"live": true}})", "command-line flag only"));
  // the PAPER/LIVE switch's other desk: loopback http only
  CHECK(e.peer_url.empty() && peer_url_for(e) == "http://127.0.0.1:8789/");
  DeskdConfig live_cfg = e;
  live_cfg.live = true;
  CHECK(peer_url_for(live_cfg) == "http://127.0.0.1:8788/");
  CHECK(peer_url_for(load_deskd_config(write(R"({"deskd": {"peer_url": "http://localhost:9001"}})"))) == "http://localhost:9001/");
  CHECK(peer_url_ok("http://127.0.0.1:8789/") && peer_url_ok("http://localhost:1"));
  for (const char* bad : {"https://127.0.0.1:8789/", "http://127.0.0.1:8789/x", "http://127.0.0.1:0/", "http://127.0.0.1:70000/",
                          "http://evil.example:8789/", "http://127.0.0.1:/", "http://127.0.0.1:8789\"><script>"})
    CHECK(!peer_url_ok(bad));
  CHECK(throws(R"({"deskd": {"peer_url": "http://kalshi.com/"}})", "peer_url must be"));
  // the write rate limit tier (gw/write_budget.h): Basic until the upgrade
  CHECK(e.write_tier == "basic" && load_deskd_config(write(R"({"deskd": {"write_tier": "advanced"}})")).write_tier == "advanced");
  CHECK(throws(R"({"deskd": {"write_tier": "premier"}})", "write_tier is"));
  // the data-release calendar's path (deskd's command line fills in the repo's)
  CHECK(e.calendar.empty() && load_deskd_config(write(R"({"deskd": {"calendar": "/tmp/cal.json"}})")).calendar == "/tmp/cal.json");
  // C1 (plan 5(d)): off, its two series, $2 a take, $10 a game, $40, scores at most 5 s old; the
  // gates in <data_dir>/gates. Every key is known and every amount inside its range.
  CHECK(!e.college.enabled && e.college.series == (std::vector<std::string>{"KXNCAAWBGAME", "KXNCAAWVMATCH"}) &&
        e.college.clip == 20000 && e.college.game == 100000 && e.college.budget == 400000 && e.college.max_score_age_s == 5 &&
        e.college.volley_model.empty() && e.gates_dir.empty());
  DeskdConfig col = load_deskd_config(write(R"({"deskd": {"gates_dir": "~/gates", "college": {"enabled": true,
    "series": ["KXNCAAWVMATCH"], "volley_model": "/m/v.json", "clip_dollars": 5, "game_dollars": 15, "lane_dollars": 30,
    "max_score_age_s": 3}}})"));
  CHECK(col.college.enabled && col.college.series == std::vector<std::string>{"KXNCAAWVMATCH"} && col.college.clip == 50000 &&
        col.college.game == 150000 && col.college.budget == 300000 && col.college.max_score_age_s == 3 &&
        col.college.volley_model == "/m/v.json" && !col.gates_dir.empty() && col.gates_dir.find('~') == std::string::npos);
  CHECK(throws(R"({"deskd": {"college": {"leader_only": true}}})", "deskd college: unknown key \"leader_only\""));
  CHECK(throws(R"({"deskd": {"college": {"clip_dollars": 6}}})", "college: clip_dollars is $1.0000 to $5.0000, not $6.0000"));
  CHECK(throws(R"({"deskd": {"college": {"clip_dollars": 0.5}}})", "clip_dollars is $1.0000 to $5.0000, not $0.5000"));
  CHECK(throws(R"({"deskd": {"college": {"game_dollars": 20}}})", "game_dollars is $2.0000 to $15.0000, not $20.0000"));
  CHECK(throws(R"({"deskd": {"college": {"clip_dollars": 5, "game_dollars": 4}}})", "game_dollars is $5.0000 to $15.0000"));
  CHECK(throws(R"({"deskd": {"college": {"lane_dollars": 50}}})", "lane_dollars is $10.0000 to $40.0000, not $50.0000"));
  CHECK(throws(R"({"deskd": {"college": {"lane_dollars": -1}}})", "lane_dollars is $10.0000 to $40.0000, not $-1.0000"));
  CHECK(throws(R"({"deskd": {"college": {"max_score_age_s": 10}}})", "max_score_age_s is 1 to 5, not 10"));
  CHECK(throws(R"({"deskd": {"college": {"max_score_age_s": 0}}})", "max_score_age_s is 1 to 5, not 0"));
  CHECK(throws(R"({"deskd": {"college": {"series": ["kxncaawbgame"]}}})", "college: series has \"kxncaawbgame\", not a series ticker"));
  CHECK(throws(R"({"deskd": {"college": {"series": []}}})", "college: series is empty"));
  CHECK(throws(R"({"deskd": {"college": {"enabled": "yes"}}})", "\"enabled\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"college": [1]}})", "deskd college: not an object"));
  // X1 and N1: off, on NBA's series, the model in the source tree (deskd's command line), shadow
  // signals recorded
  CHECK(!e.nba_lanes && e.nba_series == std::vector<std::string>{"KXNBAGAME"} && e.nba_model.empty() && e.lane_shadow);
  DeskdConfig nba = load_deskd_config(write(R"({"deskd": {"nba_lanes": true, "nba_series": ["KXNBAGAME", "KXWNBAGAME"],
    "nba_model": "~/m/nba.json", "lane_shadow": false}})"));
  CHECK(nba.nba_lanes && nba.nba_series == (std::vector<std::string>{"KXNBAGAME", "KXWNBAGAME"}) && !nba.lane_shadow &&
        nba.nba_model.size() > 11 && nba.nba_model.find('~') == std::string::npos &&
        nba.nba_model.compare(nba.nba_model.size() - 11, 11, "/m/nba.json") == 0);
  CHECK(throws(R"({"deskd": {"nba_series": []}})", "deskd: nba_series is empty"));
  CHECK(throws(R"({"deskd": {"nba_series": ["KXNBAGAME", "nba"]}})", "nba_series has \"nba\", not a series ticker"));
  CHECK(throws(R"({"deskd": {"nba_lanes": 1}})", "\"nba_lanes\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"nba_shadow": true}})", "unknown key \"nba_shadow\""));
  // S1 (plan 5(c)): off, scanning band_series once on, 10 contracts and 6 legs a basket, 1c a
  // contract after fees, paused at -$5, KXHIGH and KXLOW on whole degrees; gw counts every market
  // alone, and fees round to the cent (decisions 5 and 7)
  CHECK(e.arb_mode == "off" && e.arb_series.empty() && e.arb_max_qty == 1000 && e.arb_max_legs == 6 && e.arb_thr == 100 &&
        e.arb_legging_pause == 50000 && !e.event_risk && e.fee_precision == 100);
  CHECK(arb_lattice_step(e, "KXHIGHNY") == 1 && arb_lattice_step(e, "KXLOWTCHI") == 1 && arb_lattice_step(e, "KXNATGAS15M") == 0);
  DeskdConfig arb = load_deskd_config(write(R"({"deskd": {"systematic_orders": true, "event_risk": true, "arb_mode": "trade",
    "arb_series": ["KXHIGHDEN"], "arb_max_qty": 20, "arb_max_legs": 4, "arb_thr": 0.005, "arb_legging_pause": 2.5,
    "arb_lattice": {"KXHIGH": 1, "KXHIGHDEN": 2}, "fee_precision": 0.0001}})"));
  CHECK(arb.arb_mode == "trade" && arb.arb_series == std::vector<std::string>{"KXHIGHDEN"} && arb.arb_max_qty == 2000 &&
        arb.arb_max_legs == 4 && arb.arb_thr == 50 && arb.arb_legging_pause == 25000 && arb.event_risk && arb.fee_precision == 1);
  CHECK(arb_lattice_step(arb, "KXHIGHDEN") == 2 && arb_lattice_step(arb, "KXHIGHNY") == 1 && arb_lattice_step(arb, "KXLOWTNY") == 0);
  CHECK(load_deskd_config(write(R"({"deskd": {"arb_mode": "scan"}})")).arb_mode == "scan");   // scan needs neither switch
  CHECK(throws(R"({"deskd": {"arb_mode": "trade", "event_risk": true}})", "deskd: arb_mode \"trade\" needs systematic_orders"));
  CHECK(throws(R"({"deskd": {"arb_mode": "trade", "systematic_orders": true}})", "deskd: arb_mode \"trade\" needs event_risk"));
  CHECK(throws(R"({"deskd": {"arb_mode": "on"}})", "arb_mode is \"off\", \"scan\" or \"trade\", not \"on\""));
  CHECK(throws(R"({"deskd": {"arb_series": []}})", "deskd: arb_series is empty"));
  CHECK(throws(R"({"deskd": {"arb_series": ["kxhighny"]}})", "arb_series has \"kxhighny\", not a series ticker"));
  CHECK(throws(R"({"deskd": {"arb_max_qty": 21}})", "arb_max_qty is 1 to 20 whole contracts, not 21"));
  CHECK(throws(R"({"deskd": {"arb_max_qty": 2.5}})", "arb_max_qty is 1 to 20 whole contracts, not 2.5"));
  CHECK(throws(R"({"deskd": {"arb_max_legs": 7}})", "arb_max_legs is 2 to 6, not 7"));
  CHECK(throws(R"({"deskd": {"arb_thr": 0.001}})", "arb_thr is $0.0050 to $0.1000, not $0.0010"));
  CHECK(throws(R"({"deskd": {"arb_legging_pause": 6}})", "arb_legging_pause is $0.0001 to $5.0000, not $6.0000"));
  CHECK(throws(R"({"deskd": {"arb_legging_pause": 0}})", "arb_legging_pause is $0.0001 to $5.0000, not $0.0000"));
  CHECK(throws(R"({"deskd": {"arb_lattice": {"KXHIGH": 0}}})", "arb_lattice maps a series prefix to a step over 0, not \"KXHIGH\": 0"));
  CHECK(throws(R"({"deskd": {"arb_lattice": [1]}})", "\"arb_lattice\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"fee_precision": 0.001}})", "fee_precision is 0.01 or 0.0001 (dollars), not 0.001"));
  CHECK(throws(R"({"deskd": {"event_risk": "on"}})", "\"event_risk\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"lip_series": ["kxgold15m"]}})", "not a series ticker"));
  CHECK(throws(R"({"deskd": {"lip_series": "KXGOLD15M"}})", "wrong type"));
  CHECK(throws(R"({"deskd": {"band_series": ["KX HIGH"]}})", "band_series has"));
  CHECK(throws(R"({"key_type": "ecdsa"})", "\"key_type\" must be \"rsa\" or \"ed25519\""));
  CHECK(throws(R"({"deskd": {"heartbeat_window": 5000}})", "unknown key \"heartbeat_window\""));
  CHECK(throws(R"({"deskd": {"http_port": "8788"}})", "\"http_port\" has the wrong type"));
  CHECK(throws(R"({"deskd": {"max_streams": 15}})", "max_streams"));
  CHECK(throws(R"({"deskd": [1]})", "not an object"));
  CHECK(throws(R"({"deskd": {"lock_dir": "data/locks"}})", "lock_dir must be an absolute path"));
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
  auto lock = AccountLock::acquire(d.path, "prod", "test", false, &err);
  auto demo_lock = AccountLock::acquire(d.path, "demo", "test", false, &err);
  CHECK(lock && lock->held() && demo_lock && demo_lock->held());
  const SendGuard demo(false, demo_lock.get()), live_no_lock(true, nullptr), live(true, lock.get()),
      live_demo_lock(true, demo_lock.get());
  CHECK(demo.refuse("api.elections.kalshi.com").find("--live") != std::string::npos);
  CHECK(demo.refuse("gateway.polymarket.us").find("--live") != std::string::npos);
  CHECK(live_no_lock.refuse("api.elections.kalshi.com").find("account's lock") != std::string::npos);
  // --live with the demo account's lock is still refused: the two conditions are independent
  CHECK(live_demo_lock.refuse("api.elections.kalshi.com").find("account's lock") != std::string::npos);
  CHECK(live.refuse("api.elections.kalshi.com").empty());
  CHECK(demo.refuse("external-api.demo.kalshi.co").empty());
  CHECK(demo.refuse("127.0.0.1").empty());
  CHECK(!demo.refuse("example.com").empty());
  CHECK(!live.refuse("example.com").empty());
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);   // every deskd test main: an embedder never dies of a peer's close
  test_lock();
  test_heartbeat_rule();
  test_ops_thread();
  test_web_probe();
  test_config();
  test_subscribe_guard();
  test_send_guard();
  test_logger_never_blocks();   // last: the logger thread stays on for the rest of the process
  return finish("deskd-ops");
}
