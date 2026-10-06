// deskd: the event-driven desk (docs: the implementation plan, section 2).
//
//   deskd [--config FILE] [--data-dir DIR] [--ui-dir DIR] [--port N] [--live]
//
// Starts non-live: production venue hosts are refused unless --live is given AND the account
// lock is held (net/guard.h). No config file is read unless --config names one.
//
// Exit codes: 0 clean shutdown (SIGTERM, SIGINT), 2 bad arguments or config, 3 the account lock
// is held elsewhere or the data dir is missing, 4 the web port or ui dir is unusable.
#include <signal.h>

#include <chrono>
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
               "  --data-dir DIR  holds kalshi-account.lock and deskd.heartbeat (default data; must exist)\n"
               "  --ui-dir DIR    the screen, served from disk (default %s/ui/desk)\n"
               "  --port N        web port on 127.0.0.1 (default 8788)\n"
               "  --live          allow the production venue hosts (never without the account lock)\n",
               DESKD_SOURCE_DIR);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace deskd;
  std::string config_path, data_dir, ui_dir;
  int port = -1;
  bool live = false;
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

  // A write to a closed socket is an error to handle, never a reason to end the desk: TLS
  // writes go through write(2), which raises SIGPIPE, and its default action killed the old
  // desk on a reconnect.
  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa {};
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  // The lock before anything that could act on the account.
  std::string err;
  std::unique_ptr<AccountLock> lock = AccountLock::acquire(cfg.data_dir, &err);
  if (!lock) {
    std::fprintf(stderr, "deskd: refusing to start: %s\n", err.c_str());
    return 3;
  }
  if (!cfg.live && cfg.env == "prod")
    DLOG_WARN("%s says env prod, but deskd follows --live: production hosts stay refused", cfg.config_path.c_str());
  if (cfg.live) DLOG_WARN("--live: production venue hosts allowed while %s is held", lock->path().c_str());

  Deskd desk(cfg, std::move(lock));
  if (!desk.start(&err)) {
    std::fprintf(stderr, "deskd: %s\n", err.c_str());
    return 4;
  }
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  DLOG_INFO("signal: shutting down");
  desk.stop();
  return 0;
}
