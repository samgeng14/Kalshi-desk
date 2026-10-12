// deskd: the event-driven desk (docs: the implementation plan, section 2).
//
//   deskd [--config FILE] [--data-dir DIR] [--ui-dir DIR] [--port N] [--live]
//         [--sim https://127.0.0.1:PORT --sim-ca FILE]
//
// --sim points every venue connection at the fake exchange (tests/fake_exchange, fake-kalshi)
// on a loopback address, its test CA trusted for loopback only: a paper-trading sandbox with
// no network and the browser tests' venue. Never with --live; a non-loopback URL is refused.
//
// Starts non-live: production venue hosts are refused unless --live is given AND the production
// account's lock is held (net/guard.h). No config file is read unless --config names one.
//
// The account lock is <lock dir>/kalshi-<prod|demo>.lock, one fixed absolute file (app/lock.h):
// wherever deskd is started from, a second deskd on the same account is refused (exit 3). The
// data dir may be given relative to the working directory; it is resolved to its real path and
// logged, and never decides which lock is taken.
//
// Exit codes: 0 clean shutdown (SIGTERM, SIGINT), 2 bad arguments or config, 3 the account lock
// is held elsewhere (or cannot be taken) or the data dir is missing, 4 the web port or ui dir is
// unusable.
#include <signal.h>
#include <sys/stat.h>

#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include "deskd/app/config.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/build_paths.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_stop_signal(int) { g_stop = 1; }

void usage() {
  std::fprintf(stderr,
               "usage: deskd [--config FILE] [--data-dir DIR] [--ui-dir DIR] [--port N] [--live]\n"
               "  --config FILE   JSON with comments: top-level key_id, pem_path, env and a \"deskd\" section\n"
               "  --data-dir DIR  holds deskd.heartbeat (default data; must exist)\n"
               "  --ui-dir DIR    the screen, served from disk (default %s/ui/desk)\n"
               "  --port N        web port on 127.0.0.1 (default 8788)\n"
               "  --live          allow the production venue hosts (never without the account lock)\n"
               "  --sim URL       the fake exchange on loopback (https://127.0.0.1:PORT, as fake-kalshi prints it)\n"
               "  --sim-ca FILE   its test CA (fake-kalshi --tls DIR writes DIR/ca.pem), trusted for loopback only\n"
               "The account lock is <lock dir>/kalshi-prod.lock with --live, else kalshi-demo.lock;\n"
               "lock dir: the config's deskd.lock_dir, else $HOME/%s\n",
               DESKD_SOURCE_DIR, deskd::AccountLock::kDirUnderHome);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace deskd;
  std::string config_path, data_dir, ui_dir;
  int port = -1;
  bool live = false;
  std::string sim_url, sim_ca;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "deskd: %s needs a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") config_path = value("--config");
    else if (a == "--data-dir") data_dir = value("--data-dir");
    else if (a == "--ui-dir") ui_dir = value("--ui-dir");
    else if (a == "--port") {
      const std::string v = value("--port");
      char* end = nullptr;
      const long p = std::strtol(v.c_str(), &end, 10);
      if (!end || *end || p < 0 || p > 65535) {
        std::fprintf(stderr, "deskd: bad --port %s\n", v.c_str());
        return 2;
      }
      port = static_cast<int>(p);
    } else if (a == "--live") live = true;
    else if (a == "--sim") sim_url = value("--sim");
    else if (a == "--sim-ca") sim_ca = value("--sim-ca");
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "deskd: unknown argument %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  DeskdConfig cfg;
  try {
    if (!config_path.empty()) cfg = load_deskd_config(config_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "deskd: %s\n", e.what());
    return 2;
  }
  if (!data_dir.empty()) cfg.data_dir = expand_home(data_dir);
  if (!ui_dir.empty()) cfg.ui_dir = expand_home(ui_dir);
  if (cfg.ui_dir.empty()) cfg.ui_dir = std::string(DESKD_SOURCE_DIR) + "/ui/desk";
  if (port >= 0) cfg.http_port = port;
  cfg.live = live;
  RuntimeOptions options;
  if (!sim_url.empty() || !sim_ca.empty()) {
    // the fake exchange: loopback only, its CA for loopback only, and never with --live
    std::string why;
    const auto ep = net::Endpoint::parse(sim_url, &why);
    if (live || !ep || (ep->host != "127.0.0.1" && ep->host != "localhost" && ep->host != "::1") || sim_ca.empty()) {
      std::fprintf(stderr, "deskd: --sim takes a loopback https URL and --sim-ca, and never --live\n");
      return 2;
    }
    std::string base = sim_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string tail = "/trade-api/v2";
    if (base.size() >= tail.size() && base.compare(base.size() - tail.size(), tail.size(), tail) == 0)
      base.erase(base.size() - tail.size());
    options.rest_url = base + "/trade-api/v2";
    options.ws_url = base + "/trade-api/ws/v2";
    options.factory.loopback_ca_file = expand_home(sim_ca);
    options.espn_url = "";   // no network in the sandbox
    DLOG_WARN("--sim: every venue connection goes to the fake exchange at %s", base.c_str());
  }

  // A write to a closed socket is an error to handle, never a reason to end the desk: TLS
  // writes go through write(2), which raises SIGPIPE, and its default action killed the old
  // desk on a reconnect. (Deskd::start does it again for every embedder.)
  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa {};
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  // The data dir as a real path: what the log and deskctl status show is where the files are.
  {
    char real[PATH_MAX];
    struct stat st {};
    if (::stat(cfg.data_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !::realpath(cfg.data_dir.c_str(), real)) {
      std::fprintf(stderr, "deskd: refusing to start: data dir %s does not exist (deskd never creates it)\n",
                   cfg.data_dir.c_str());
      return 3;
    }
    cfg.data_dir = real;
  }

  // The lock before anything that could act on the account: one fixed file per account.
  std::string err;
  std::string lock_dir = cfg.lock_dir;
  const bool default_dir = lock_dir.empty();
  if (default_dir) lock_dir = AccountLock::default_dir(&err);
  std::unique_ptr<AccountLock> lock;
  if (!lock_dir.empty())
    lock = AccountLock::acquire(lock_dir, cfg.live ? "prod" : "demo", cfg.live ? "deskd LIVE" : "deskd DEMO",
                                default_dir, &err);
  if (!lock) {
    std::fprintf(stderr, "deskd: refusing to start: %s\n", err.c_str());
    return 3;
  }
  DLOG_INFO("account lock %s (%s account), data %s", lock->path().c_str(), lock->env().c_str(), cfg.data_dir.c_str());
  if (!cfg.live && cfg.env == "prod")
    DLOG_WARN("%s says env prod, but deskd follows --live: production hosts stay refused", cfg.config_path.c_str());
  if (cfg.live) DLOG_WARN("--live: production venue hosts allowed while %s is held", lock->path().c_str());

  Deskd desk(cfg, std::move(lock), options);
  if (!desk.start(&err)) {
    std::fprintf(stderr, "deskd: %s\n", err.c_str());
    return 4;
  }
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  DLOG_INFO("signal: shutting down");
  desk.stop();
  return 0;
}
