// fake-kalshi: the fake exchange on its own, for soak runs and for pointing a deskd at by hand.
//
//   fake-kalshi [--port N] [--tape FILE.csv[.gz] [--series KXNATGAS15M] [--speed X]]
//               [--scenario FILE.jsonl] [--cash SHARD:DOLLARS,...] [--auth off|presence]
//               [--no-rate-limits] [--advanced] [--tls DIR [--no-close-notify]] [--idle-close-ms N]
//               [--poly [--poly-port N] [--poly-cash DOLLARS]]
//
// Listens on 127.0.0.1 only and prints its REST and WebSocket addresses. SIGINT or SIGTERM stops
// it. Cash defaults to $100 on shards 0 and 2. --tls makes a test CA and a 127.0.0.1 certificate
// in DIR (an existing directory; tls_pki.h) and serves https:// and wss://; a client trusts
// DIR/ca.pem for loopback only.
//
// --poly also starts the fake Polymarket US (fake_poly.h) on a port of its own, with the same
// --auth, --tls, --no-close-notify and --idle-close-ms, and prints its base ("POLY"), the one
// address of both its hosts. Its account starts with a deposit of --poly-cash (default $100); it
// lists no market.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "fake_kalshi.h"
#include "fake_poly.h"
#include "tls_pki.h"

using namespace fakex;

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static int usage() {
  std::fprintf(stderr,
               "usage: fake-kalshi [--port N] [--tape FILE [--series S] [--speed X]] [--scenario FILE]\n"
               "                   [--cash SHARD:DOLLARS,...] [--auth off|presence] [--no-rate-limits]\n"
               "                   [--advanced] [--tls DIR [--no-close-notify]] [--idle-close-ms N]\n"
               "                   [--poly [--poly-port N] [--poly-cash DOLLARS]]\n");
  return 2;
}

int main(int argc, char** argv) {
  FakeConfig cfg;
  std::string tape_path, scenario_path, cash = "0:100,2:100,3:0";
  TapeFilter filter;
  double speed = 1.0;
  std::string tls_dir;
  bool poly = false;
  FakePolyConfig pcfg;
  std::string poly_cash = "100";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--port" && (v = value())) {
      cfg.port = std::atoi(v);
    } else if (a == "--tape" && (v = value())) {
      tape_path = v;
    } else if (a == "--series" && (v = value())) {
      filter.series = v;
    } else if (a == "--speed" && (v = value())) {
      speed = std::atof(v);
    } else if (a == "--scenario" && (v = value())) {
      scenario_path = v;
    } else if (a == "--cash" && (v = value())) {
      cash = v;
    } else if (a == "--auth" && (v = value())) {
      if (std::strcmp(v, "off") == 0)
        cfg.auth = AuthMode::OFF;
      else if (std::strcmp(v, "presence") == 0)
        cfg.auth = AuthMode::PRESENCE;
      else
        return usage();
    } else if (a == "--no-rate-limits") {
      cfg.rate_limits = false;
    } else if (a == "--advanced") {
      cfg.tier = advanced_tier();
    } else if (a == "--tls" && (v = value())) {
      tls_dir = v;
    } else if (a == "--no-close-notify") {
      cfg.tls_close_notify = false;
    } else if (a == "--idle-close-ms" && (v = value())) {
      cfg.idle_close_ms = std::atoi(v);
    } else if (a == "--poly") {
      poly = true;
    } else if (a == "--poly-port" && (v = value())) {
      pcfg.port = std::atoi(v);
    } else if (a == "--poly-cash" && (v = value())) {
      poly_cash = v;
    } else {
      return usage();
    }
  }
  Tape tape;
  std::string err;
  if (!tls_dir.empty()) {
    TestPki pki;
    if (!make_test_pki(tls_dir, &pki, &err)) {
      std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
      return 1;
    }
    cfg.tls_cert_file = pcfg.tls_cert_file = pki.server_chain;
    cfg.tls_key_file = pcfg.tls_key_file = pki.server_key;
    std::printf("fake-kalshi: TLS, trust %s for 127.0.0.1 only\n", pki.ca_cert.c_str());
  }
  if (!tape_path.empty() && !load_archive_csv(tape_path, tape, &err, filter)) {
    std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
    return 1;
  }
  if (!scenario_path.empty() && !load_scenario(scenario_path, tape, &err)) {
    std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
    return 1;
  }
  pcfg.auth = cfg.auth;
  pcfg.idle_close_ms = cfg.idle_close_ms;
  pcfg.tls_close_notify = cfg.tls_close_notify;
  const auto pm_cash = parse_fixed(poly_cash, 6, false);
  if (!pm_cash) return usage();
  FakeKalshi fake(cfg);
  FakePoly pm(pcfg);
  if (poly) pm.deposit(*pm_cash);
  // --cash 0:100,2:50.25 -> $100 on shard 0, $50.25 on shard 2 (the primary subaccount)
  for (std::size_t pos = 0; pos < cash.size();) {
    std::size_t comma = cash.find(',', pos);
    if (comma == std::string::npos) comma = cash.size();
    const std::string item = cash.substr(pos, comma - pos);
    pos = comma + 1;
    const auto colon = item.find(':');
    if (colon == std::string::npos) return usage();
    const int shard = std::atoi(item.substr(0, colon).c_str());
    const auto dollars = parse_fixed(item.substr(colon + 1), 6, false);
    if (!dollars) return usage();
    fake.with_engine([&](Engine& e, std::int64_t now) { e.set_cash(0, shard, *dollars, now); });
  }
  if (!fake.start(&err)) {
    std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
    return 1;
  }
  if (poly && !pm.start(&err)) {
    std::fprintf(stderr, "fake-kalshi: Polymarket: %s\n", err.c_str());
    return 1;
  }
  std::printf("fake-kalshi: REST %s\nfake-kalshi: WS   %s\n", fake.rest_base().c_str(), fake.ws_url().c_str());
  if (poly) std::printf("fake-kalshi: POLY %s\n", pm.base().c_str());
  if (!tape.events.empty())
    std::printf("fake-kalshi: %zu tape events over %.1f min at %.1fx\n", tape.events.size(),
                static_cast<double>(tape.t1() - tape.t0()) / 60000.0, speed);
  std::fflush(stdout);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  if (!tape.events.empty()) fake.play(std::move(tape), speed);
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fake.stop();
  pm.stop();
  std::printf("fake-kalshi: stopped after %llu requests on %llu connections\n",
              static_cast<unsigned long long>(fake.http_requests()),
              static_cast<unsigned long long>(fake.connections_accepted()));
  if (poly)
    std::printf("fake-kalshi: POLY stopped after %llu requests on %llu connections\n",
                static_cast<unsigned long long>(pm.http_requests()),
                static_cast<unsigned long long>(pm.connections_accepted()));
  return 0;
}
