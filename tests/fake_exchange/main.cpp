// fake-kalshi: the fake exchange on its own, for soak runs and for pointing a deskd at by hand.
//
//   fake-kalshi [--port N] [--tape FILE.csv[.gz] [--series KXNATGAS15M] [--speed X]]
//               [--scenario FILE.jsonl] [--cash SHARD:DOLLARS,...] [--auth off|presence]
//               [--no-rate-limits] [--advanced]
//
// Listens on 127.0.0.1 only and prints its REST and WebSocket addresses. SIGINT or SIGTERM stops
// it. Cash defaults to $100 on shards 0 and 2.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "fake_kalshi.h"

using namespace fakex;

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static int usage() {
  std::fprintf(stderr,
               "usage: fake-kalshi [--port N] [--tape FILE [--series S] [--speed X]] [--scenario FILE]\n"
               "                   [--cash SHARD:DOLLARS,...] [--auth off|presence] [--no-rate-limits]\n"
               "                   [--advanced]\n");
  return 2;
}

int main(int argc, char** argv) {
  FakeConfig cfg;
  std::string tape_path, scenario_path, cash = "0:100,2:100,3:0";
  TapeFilter filter;
  double speed = 1.0;
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
    } else {
      return usage();
    }
  }
  Tape tape;
  std::string err;
  if (!tape_path.empty() && !load_archive_csv(tape_path, tape, &err, filter)) {
    std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
    return 1;
  }
  if (!scenario_path.empty() && !load_scenario(scenario_path, tape, &err)) {
    std::fprintf(stderr, "fake-kalshi: %s\n", err.c_str());
    return 1;
  }
  FakeKalshi fake(cfg);
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
  std::printf("fake-kalshi: REST %s\nfake-kalshi: WS   %s\n", fake.rest_base().c_str(), fake.ws_url().c_str());
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
  std::printf("fake-kalshi: stopped after %llu requests on %llu connections\n",
              static_cast<unsigned long long>(fake.http_requests()),
              static_cast<unsigned long long>(fake.connections_accepted()));
  return 0;
}
