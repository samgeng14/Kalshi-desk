#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <cmath>
#include <iostream>
#include <thread>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/engine.h"
#include "desk/eval.h"
#include "desk/executor.h"
#include "desk/log.h"
#include "desk/research.h"
#include "desk/server.h"
#include "desk/store.h"

using namespace desk;

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static void usage() {
  std::cerr << "kalshi-desk [--config config/desk.json] <command>\n"
               "  serve         run the desk: poll, propose, dashboard on localhost\n"
               "  once          one cycle, then print status\n"
               "  report        print the gate report as JSON\n"
               "  sign-test     sign a sample request with the configured key and verify it\n"
               "  balance       fetch balance/positions once (checks the API key)\n"
               "  research ...  historical studies (backfill, longshot, tied80, wx-settle)\n";
}

int main(int argc, char** argv) {
  std::string cfg_path = "config/desk.json", cmd = "serve";
  std::vector<std::string> rest;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) cfg_path = argv[++i];
    else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) { usage(); return 0; }
    else if (cmd == "serve" && rest.empty() && argv[i][0] != '-') cmd = argv[i];
    else rest.push_back(argv[i]);
  }
  Config cfg;
  try {
    cfg = load_config(cfg_path);
  } catch (const std::exception& e) {
    std::cerr << "config: " << e.what() << "\n";
    usage();
    return 2;
  }

  std::shared_ptr<Signer> signer;
  if (!cfg.key_id.empty() && !cfg.pem_path.empty()) {
    try {
      signer = std::make_shared<Signer>(cfg.key_id, cfg.pem_path);
      LOG_INFO("API key loaded (%s)", cfg.env.c_str());
    } catch (const std::exception& e) {
      LOG_ERR("API key: %s (running unauthenticated: market data only)", e.what());
    }
  } else {
    LOG_WARN("no key_id/pem_path in config: market data only, no orders");
  }

  if (cmd == "sign-test") {
    if (!signer) { std::cerr << "no key configured\n"; return 1; }
    long long ts = now_ms();
    auto h = signer->headers("GET", "/trade-api/v2/portfolio/balance", ts);
    std::string msg = std::to_string(ts) + "GET/trade-api/v2/portfolio/balance";
    bool ok = signer->verify(msg, h["KALSHI-ACCESS-SIGNATURE"]);
    std::cout << "message: " << msg << "\nsignature ok: " << (ok ? "yes" : "NO") << "\n";
    return ok ? 0 : 1;
  }

  // One engine per database: two desks on one SQLite file would double-count exposure.
  // Read-only commands (report, balance) may run alongside a live desk.
  if (cmd == "serve" || cmd == "once") {
    std::string lock_path = cfg.db_path + ".lock";
    int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
      std::cerr << "another kalshi-desk is running on " << cfg.db_path << " (lock " << lock_path << ")\n";
      return 3;
    }
  }
  Store store(cfg.db_path);
  seed_hypotheses(store);
  KalshiClient client(cfg.rest_base(), signer, cfg.user_agent);
  Executor exec(cfg, store, client);

  if (cmd == "research") return run_research(rest, cfg, store, client);

  if (cmd == "balance") {
    try {
      Balance b = client.get_balance();
      std::cout << "balance " << b.balance << " portfolio_value " << b.portfolio_value << "\n";
      for (const auto& p : client.get_positions())
        std::cout << "  " << p.ticker << " pos " << p.position << " exposure " << p.exposure << "\n";
    } catch (const std::exception& e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }
    return 0;
  }

  // Flatten everything. Uses the same close path the dashboard button uses - reduce_only paired
  // with immediate-or-cancel, which is the combination Kalshi actually accepts - and works while
  // the kill switch is on, because going flat only ever reduces risk. Prints the plan and does
  // nothing unless --yes is given.
  if (cmd == "close-all") {
    bool confirmed = false;
    for (const auto& a : rest) if (a == "--yes") confirmed = true;
    try {
      exec.reconcile(true);   // the exchange's view, not yesterday's cache
      auto positions = store.positions();
      int open = 0;
      for (const auto& p : positions) if (p.position != 0) ++open;
      if (open == 0) { std::cout << "nothing open\n"; return 0; }
      if (store.kv_get("kill").value_or("0") == "1")
        std::cout << "note: the kill switch is on; closing anyway, because flattening only "
                     "reduces risk\n";
      for (const auto& p : positions) {
        if (p.position == 0) continue;
        std::cout << (confirmed ? "closing " : "would close ") << p.ticker << "  "
                  << std::fabs(p.position) << (p.position > 0 ? " YES" : " NO")
                  << "  cost $" << p.exposure << "\n";
        if (!confirmed) continue;
        try {
          std::cout << "  -> " << exec.close_position(p.ticker).dump() << "\n";
        } catch (const std::exception& e) {
          // One position that cannot be closed must not stop the others.
          std::cout << "  -> FAILED: " << e.what() << "\n";
        }
      }
      if (!confirmed) std::cout << "\nnothing sent. re-run with --yes to send these orders.\n";
      else { exec.reconcile(true); std::cout << "\ndone; positions re-read from the exchange\n"; }
    } catch (const std::exception& e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }
    return 0;
  }

  Engine engine(cfg, store, client, exec);

  if (cmd == "report") {
    std::cout << build_report(store, cfg).dump(2) << "\n";
    return 0;
  }
  if (cmd == "once") {
    engine.cycle();
    std::cout << engine.status().dump(2) << "\n";
    return 0;
  }
  if (cmd != "serve") {
    usage();
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  HttpServer server(engine, store, client, exec, cfg.http_port);
  server.start();
  LOG_INFO("desk up: env=%s poll=%ds bankroll=%.2f watch=%zu", cfg.env.c_str(), cfg.poll_seconds,
           cfg.bankroll_dollars, engine.watch().size());
  while (!g_stop) {
    long long t0 = now_ms();
    engine.cycle();
    long long spent = now_ms() - t0;
    long long wait = cfg.poll_seconds * 1000LL - spent;
    while (wait > 0 && !g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(wait, 250)));
      wait -= 250;
    }
  }
  LOG_INFO("shutting down");
  server.stop();
  return 0;
}
