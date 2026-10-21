// deskctl: talks to a running deskd from the terminal.
//
//   deskctl [--port N] status   workers, run state, heartbeat; exit 0 healthy, 2 heartbeat
//                               withheld, 1 no answer
//   deskctl [--port N] kill     KILL through the same route as the screen's button (Host and
//                               X-Desk token included); exit 0 once deskd confirms KILLED
//   deskctl kill --direct --config FILE [--live]
//                               KILL at the exchange without deskd (plan section 2: "deskctl
//                               kill does the same if deskd hangs"): cancel-all, every order
//                               group triggered, then a batch cancel of anything still listed
//                               resting; exit 0 once the cancel-all is confirmed and nothing rests
//
// KILL reads the token the way the page refreshes it: GET / and take <meta name="desk-token">.
// If deskd does not answer, deskctl says so and exits 1.
//
// The direct KILL signs with the desk's own key (its config: key_id, pem_path, key_type, env,
// lock_dir) and asks the same send guard. It cannot take the account lock while a hung deskd
// holds it, so (review, 2026-10-06) it sends to a production host only with an explicit --live,
// a config whose env is "prod", AND a lock whose holder line (app/lock.h) names "deskd LIVE".
// When nobody holds the lock it takes it itself, for the few seconds it runs. --sim URL --sim-ca
// FILE points it at the fake exchange (loopback only, never with --live), for the tests.
#include <httplib.h>

#include <algorithm>
#include <csignal>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "deskd/app/config.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/conn.h"
#include "deskd/net/guard.h"
#include "deskd/net/http.h"
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

// ---- the direct KILL ---------------------------------------------------------------------------

struct Direct {
  deskd::net::Endpoint ep;
  deskd::net::ConnFactory* factory = nullptr;
  deskd::KalshiSigner* signer = nullptr;
  // one signed request on its own connection (one write per connection, as deskd's senders)
  deskd::net::Exchange send(deskd::kalshi::WireRequest r, std::string* why) {
    deskd::net::Exchange x;
    auto conn = factory->open(ep, 3000, why);
    if (!conn) return x;
    const std::string base = deskd::kalshi::kBasePath;
    if (r.path.compare(0, base.size(), base) != 0) { *why = "unexpected path"; return x; }
    r.path.replace(0, base.size(), ep.base_path);
    const auto headers = signer->headers(deskd::now_ms(), r.method, r.path);
    if (headers.size() != 3) { *why = "request signing failed"; return x; }
    x = deskd::net::exchange(*conn, deskd::kalshi::request_bytes(r, ep.host_header(), headers),
                             deskd::net::mono_us() + 5000000LL);
    if (x.fate != deskd::net::Exchange::Fate::REPLIED) *why = x.why.empty() ? "no reply" : x.why;
    return x;
  }
};

struct DirectArgs {
  std::string config, sim_url, sim_ca;
  bool live = false;
};

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

int kill_direct(const DirectArgs& a) {
  using namespace deskd;
  if (a.config.empty()) {
    std::fprintf(stderr, "deskctl: kill --direct needs --config FILE (the desk's config: its key and account)\n");
    return 2;
  }
  DeskdConfig cfg;
  try {
    cfg = load_deskd_config(expand_home(a.config));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "deskctl: %s\n", e.what());
    return 2;
  }
  const bool prod = cfg.env == "prod";
  if (!AccountLock::env_ok(cfg.env)) {
    std::fprintf(stderr, "deskctl: the config's env is \"%s\": \"demo\" (paper) or \"prod\" (live)\n", cfg.env.c_str());
    return 2;
  }
  if (prod != a.live) {
    std::fprintf(stderr, "deskctl: %s\n", prod ? "a production config needs --live" : "--live takes a production config (env \"prod\")");
    return 2;
  }
  std::string url = prod ? kProdRestUrl : RuntimeOptions{}.rest_url;
  net::FactoryOptions fo;
  if (!a.sim_url.empty() || !a.sim_ca.empty()) {
    std::string why;
    const auto ep = net::Endpoint::parse(a.sim_url, &why);
    if (a.live || !ep || (ep->host != "127.0.0.1" && ep->host != "localhost" && ep->host != "::1") || a.sim_ca.empty()) {
      std::fprintf(stderr, "deskctl: --sim takes a loopback https URL and --sim-ca, and never --live\n");
      return 2;
    }
    url = a.sim_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 13 || url.compare(url.size() - 13, 13, "/trade-api/v2") != 0) url += "/trade-api/v2";
    fo.loopback_ca_file = expand_home(a.sim_ca);
  }
  // the account lock: taken when free; else its holder must be deskd, and for production "deskd LIVE"
  std::string err;
  std::string dir = cfg.lock_dir.empty() ? AccountLock::default_dir(&err) : cfg.lock_dir;
  if (dir.empty()) {
    std::fprintf(stderr, "deskctl: no lock directory: %s\n", err.c_str());
    return 2;
  }
  auto lock = AccountLock::acquire(dir, cfg.env, "deskctl kill --direct", false, &err);
  bool holder_live = false;
  if (!lock) {
    const std::string holder = read_file(dir + "/kalshi-" + cfg.env + ".lock");
    holder_live = holder.find("deskd LIVE") != std::string::npos;
    std::printf("deskctl: the %s account's lock is held: %s\n", cfg.env.c_str(), holder.empty() ? err.c_str() : holder.c_str());
    if (holder.find("deskd") == std::string::npos) {
      std::fprintf(stderr, "deskctl: KILL NOT SENT: the lock is not deskd's; whatever holds it trades this account\n");
      return 1;
    }
  }
  const SendGuard guard = lock ? SendGuard(a.live, lock.get()) : SendGuard::direct_kill(a.live, holder_live);
  net::ConnFactory factory(guard, fo);
  std::string why;
  if (!factory.ok(&why)) {
    std::fprintf(stderr, "deskctl: KILL NOT SENT: %s\n", why.c_str());
    return 1;
  }
  auto ep = net::Endpoint::parse(url, &why);
  auto signer = KalshiSigner::load(cfg.key_id, expand_home(cfg.pem_path), cfg.key_type, &why);
  if (!ep || !signer) {
    std::fprintf(stderr, "deskctl: KILL NOT SENT: %s\n", why.c_str());
    return 1;
  }
  Direct d{*ep, &factory, signer.get()};
  // 1. cancel-all: the account's every resting order, and those placed in the next minute
  why.clear();
  auto x = d.send(kalshi::build_cancel_all(), &why);
  const auto all = kalshi::classify_write(WireOp::CANCEL_ALL, x);
  const bool confirmed = all.kind == kalshi::WriteOutcome::Kind::ACK;
  std::printf("cancel-all: %s%s\n", confirmed ? "confirmed" : "NOT CONFIRMED",
              confirmed ? "" : (" (" + (why.empty() ? all.why : why) + ")").c_str());
  // 2. every order group triggered (a triggered group refuses new orders until reset)
  why.clear();
  x = d.send(kalshi::build_get_order_groups(), &why);
  int groups = 0, triggered = 0;
  if (x.fate == net::Exchange::Fate::REPLIED && x.res.status == 200) {
    if (auto rows = kalshi::parse_order_groups(x.res.body, &why)) {
      for (const auto& g : *rows) {
        ++groups;
        kalshi::WireRequest r;
        if (!kalshi::build_order_group_action(g.id, "trigger", g.exchange_index.value_or(0), -1, &r, &why)) continue;
        std::string w;
        const auto t = kalshi::classify_write(WireOp::GROUP, d.send(std::move(r), &w));
        if (t.kind == kalshi::WriteOutcome::Kind::ACK) ++triggered;
        else std::printf("order group %s: not triggered (%s)\n", g.id.c_str(), (w.empty() ? t.why : w).c_str());
      }
    } else std::printf("order groups: unreadable (%s)\n", why.c_str());
  } else std::printf("order groups: not read (%s)\n", why.empty() ? ("HTTP " + std::to_string(x.res.status)).c_str() : why.c_str());
  std::printf("order groups: %d triggered of %d\n", triggered, groups);
  // 3. anything still listed resting: batch-cancelled, 20 at a time per shard
  kalshi::ListQuery q;
  q.status = "resting";
  q.limit = 1000;
  std::map<int, std::vector<kalshi::BatchCancelItem>> by_shard;
  bool listed = false;
  for (int page = 0; page < 10; ++page) {
    why.clear();
    x = d.send(kalshi::build_get_orders(q), &why);
    if (x.fate != net::Exchange::Fate::REPLIED || x.res.status != 200) break;
    auto p = kalshi::parse_orders(x.res.body, &why);
    if (!p) break;
    for (const auto& r : p->rows)
      if (r.status == kalshi::OrderStatus::RESTING && r.remaining > 0)
        by_shard[r.exchange_index.value_or(0)].push_back({r.order_id, r.exchange_index.value_or(0), r.ticker, -1});
    if (p->cursor.empty()) { listed = true; break; }
    q.cursor = p->cursor;
  }
  std::size_t left = 0, cancelled = 0;
  for (const auto& [shard, items] : by_shard)
    for (std::size_t i = 0; i < items.size(); i += kalshi::kBatchCancelMax) {
      const std::vector<kalshi::BatchCancelItem> chunk(items.begin() + static_cast<std::ptrdiff_t>(i),
          items.begin() + static_cast<std::ptrdiff_t>(std::min(items.size(), i + kalshi::kBatchCancelMax)));
      kalshi::WireRequest r;
      left += chunk.size();
      if (!kalshi::build_batch_cancel(chunk, &r, &why)) continue;
      std::string w;
      if (kalshi::classify_write(WireOp::BATCH_CANCEL, d.send(std::move(r), &w)).kind == kalshi::WriteOutcome::Kind::ACK)
        cancelled += chunk.size();
    }
  if (!listed) std::printf("resting orders: NOT READ (%s): check the venue\n", why.c_str());
  else std::printf("resting orders after the cancel-all: %zu, batch-cancelled %zu\n", left, cancelled);
  const bool clean = confirmed && listed && cancelled == left;
  std::printf("%s\n", clean ? "KILLED at the exchange. deskd still runs (or hangs): stop it before trading again."
                             : "KILL INCOMPLETE: run it again, and check the venue for resting orders.");
  return clean ? 0 : 1;
}

void usage() {
  std::fprintf(stderr, "usage: deskctl [--port N] status|kill\n"
                       "       deskctl kill --direct --config FILE [--live] [--sim URL --sim-ca FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
  int port = 8788;
  std::string cmd;
  bool direct = false;
  DirectArgs d;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "--direct") direct = true;
    else if (a == "--live") d.live = true;
    else if (a == "--config" && i + 1 < argc) d.config = argv[++i];
    else if (a == "--sim" && i + 1 < argc) d.sim_url = argv[++i];
    else if (a == "--sim-ca" && i + 1 < argc) d.sim_ca = argv[++i];
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (cmd.empty()) cmd = a;
    else {
      usage();
      return 2;
    }
  }
  if (port <= 0 || port > 65535 || (cmd != "status" && cmd != "kill") || (direct && cmd != "kill") ||
      (!direct && (d.live || !d.config.empty() || !d.sim_url.empty()))) {
    usage();
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);            // deskd closing mid-write is an error, not our death
  if (direct) return kill_direct(d);
  httplib::Client cli("127.0.0.1", port);   // Host: 127.0.0.1:<port>, the only Host deskd answers
  cli.set_connection_timeout(1, 0);
  cli.set_read_timeout(3, 0);
  cli.set_write_timeout(3, 0);
  return cmd == "status" ? status(cli) : kill(cli);
}
