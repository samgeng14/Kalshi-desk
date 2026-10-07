// deskctl: talks to a running deskd from the terminal.
//
//   deskctl [--port N] status   workers, run state, heartbeat; exit 0 healthy, 2 heartbeat
//                               withheld, 1 no answer
//   deskctl [--port N] kill     KILL through the same route as the screen's button (Host and
//                               X-Desk token included); exit 0 once deskd confirms KILLED
//
// KILL reads the token the way the page refreshes it: GET / and take <meta name="desk-token">.
// If deskd does not answer, deskctl says so and exits 1. Cancelling at the exchange without
// deskd (plan section 2, "if deskd hangs") arrives with the senders and order groups in week 2,
// and must land before the first real order (review, 2026-10-06). Its rule, decided now: it
// cannot take the account lock (a hung deskd holds it), so it sends to a production host only
// with an explicit --live AND when the lock file's holder line (app/lock.h) or /ops/status says
// the holder is "deskd LIVE"; it signs with the same key and asks the same send guard.
#include <httplib.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>

namespace {

using json = nlohmann::json;

std::string ago(long long ms) {
  char b[32];
  if (ms < 0) return "never";
  if (ms < 60000) std::snprintf(b, sizeof b, "%.1f s", ms / 1000.0);
  else if (ms < 3600000) std::snprintf(b, sizeof b, "%lldm%02llds", ms / 60000, (ms / 1000) % 60);
  else std::snprintf(b, sizeof b, "%lldh%02lldm", ms / 3600000, (ms / 60000) % 60);
  return b;
}

int status(httplib::Client& cli) {
  auto r = cli.Get("/ops/status");
  if (!r) {
    std::fprintf(stderr, "deskctl: no answer from deskd (%s)\n", httplib::to_string(r.error()).c_str());
    return 1;
  }
  if (r->status != 200) {
    std::fprintf(stderr, "deskctl: HTTP %d: %s\n", r->status, r->body.c_str());
    return 1;
  }
  json s = json::parse(r->body, nullptr, false);
  if (s.is_discarded()) {
    std::fprintf(stderr, "deskctl: status is not JSON\n");
    return 1;
  }
  const long long now = s.value("srv_ms", 0LL);
  std::printf("deskd pid %d  %s  epoch %s  up %s\n", s.value("pid", 0), s.value("mode", "?").c_str(),
              s.value("epoch", "?").c_str(), ago(now - s.value("started_ms", now)).c_str());
  const json run = s.value("run", json::object());
  const json rearm = run.contains("rearm_ms") ? run.at("rearm_ms") : json();
  const std::string why = run.value("why", "");
  std::printf("run %s%s%s\n", run.value("state", "?").c_str(), why.empty() ? "" : (" (" + why + ")").c_str(),
              rearm.is_number() ? (", re-arm in " + ago(rearm.get<long long>() - now)).c_str() : "");
  const json hb = s.value("heartbeat", json::object());
  const std::string withheld = hb.value("withheld", "");
  const long long written = hb.value("written_ms", 0LL);
  if (withheld.empty())
    std::printf("heartbeat written %s ago (%s)\n", ago(written ? now - written : -1).c_str(), hb.value("path", "").c_str());
  else
    std::printf("heartbeat WITHHELD: %s (last written %s ago)\n", withheld.c_str(), ago(written ? now - written : -1).c_str());
  std::printf("%-8s %10s %9s\n", "worker", "progress", "quiet");
  for (const auto& w : s.value("workers", json::array())) {
    const std::string name = w.value("name", "?");
    if (name == "web") continue;   // it blocks in accept(); its progress is the request count below
    std::printf("%-8s %10llu %9s%s\n", name.c_str(), w.value("progress", 0ULL), ago(w.value("quiet_ms", -1LL)).c_str(),
                w.value("running", false) ? "" : "  NOT RUNNING");
  }
  const json web = s.value("web", json::object());
  std::printf("web      port %d, %d of %d threads in streams, %llu requests\n", web.value("port", 0),
              web.value("streams", 0), web.value("threads", 0), web.value("requests", 0ULL));
  return withheld.empty() ? 0 : 2;
}

int kill(httplib::Client& cli) {
  auto page = cli.Get("/");
  if (!page || page->status != 200) {
    std::fprintf(stderr, "deskctl: KILL NOT SENT: deskd did not serve its page (%s). Check the venues for resting orders.\n",
                 page ? ("HTTP " + std::to_string(page->status)).c_str() : httplib::to_string(page.error()).c_str());
    return 1;
  }
  std::smatch m;
  static const std::regex meta(R"re(<meta\s+name=["']desk-token["']\s+content=["']([^"']*)["'])re", std::regex::icase);
  if (!std::regex_search(page->body, m, meta) || m[1].str().empty()) {
    std::fprintf(stderr, "deskctl: KILL NOT SENT: the page carries no token\n");
    return 1;
  }
  httplib::Headers h = {{"X-Desk", m[1].str()}};
  auto r = cli.Post("/api/kill", h, "{}", "application/json");
  if (!r) {
    std::fprintf(stderr, "deskctl: KILL NOT CONFIRMED: no answer (%s). Check the venues for resting orders.\n",
                 httplib::to_string(r.error()).c_str());
    return 1;
  }
  json j = json::parse(r->body, nullptr, false);
  if (r->status != 200 || j.is_discarded() || !j.value("ok", false)) {
    std::fprintf(stderr, "deskctl: KILL NOT CONFIRMED: HTTP %d %s\n", r->status, r->body.c_str());
    return 1;
  }
  const json run = j.value("run", json::object());
  const json rearm = run.contains("rearm_ms") ? run.at("rearm_ms") : json();
  std::printf("KILLED: run %s, %d orders asked to cancel, re-arm possible from %lld (epoch ms)\n",
              run.value("state", "?").c_str(), j.value("n", 0), rearm.is_number() ? rearm.get<long long>() : 0LL);
  return 0;
}

void usage() {
  std::fprintf(stderr, "usage: deskctl [--port N] status|kill\n");
}

}  // namespace

int main(int argc, char** argv) {
  int port = 8788;
  std::string cmd;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (cmd.empty()) cmd = a;
    else {
      usage();
      return 2;
    }
  }
  if (port <= 0 || port > 65535 || (cmd != "status" && cmd != "kill")) {
    usage();
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);            // deskd closing mid-write is an error, not our death
  httplib::Client cli("127.0.0.1", port);   // Host: 127.0.0.1:<port>, the only Host deskd answers
  cli.set_connection_timeout(1, 0);
  cli.set_read_timeout(3, 0);
  cli.set_write_timeout(3, 0);
  return cmd == "status" ? status(cli) : kill(cli);
}
