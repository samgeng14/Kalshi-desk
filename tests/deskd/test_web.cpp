// deskd web: a whole deskd in this process on a free port, driven over HTTP.
//   - the page carries this start's token; Host (exactly one), origin and X-Desk guards on GET
//     and POST, before routing, before a body is read, before a 413
//   - cross-site requests refused: they can no longer hold the stream slots
//   - every POST body checked against the contract: deskd's verdict on each probe body is
//     contract.js's own (validateRequest), and so is its verdict on universe queries
//   - SSE framing: "retry: 2000", then event/id/data with seq 1, 2, 3 ... (hb first, 1/s)
//   - the stream lifecycle: takeover by sid, at most 4 streams, snapshot and subscribe only
//     for an open sid, a full per-stream queue drops and leaves a visible gap; a stop ends every
//     stream with its closing chunk
//   - every snapshot, event and reply checked by contract.js itself (node; skipped, and ctest
//     says so, without node)
//   - 10 idle connections and a POST still answers at once (the old desk's 4.8 s stall)
//   - KILL answers and reaches send-cx while gw is frozen, and while send-cx's inbox is full;
//     Start and Stop answer "queued" within 50 ms while gw is frozen
//   - a client that resets mid-stream does not end the process: Deskd::start ignores SIGPIPE
//     for whoever embeds it (this main deliberately does not)
#include <arpa/inet.h>
#include <httplib.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/app/config.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/build_paths.h"
#include "deskd/web/contract.h"
#include "deskd/web/server.h"
#include "deskd/web/sse.h"

using namespace deskd;
using json = nlohmann::json;
using namespace std::chrono_literals;

// everything served, for contract.js at the end
static json g_capture = {{"snapshots", json::array()}, {"events", json::array()}, {"responses", json::array()},
                         {"requests", json::array()},  {"queries", json::array()}};
static void capture(const std::string& key, const httplib::Result& r) {
  if (!r) return;
  json j = json::parse(r->body, nullptr, false);
  if (!j.is_discarded()) g_capture["responses"].push_back({{"key", key}, {"status", r->status}, {"json", j}});
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
struct Desk {
  TempDir dir;
  std::unique_ptr<Deskd> d;
  explicit Desk(int max_streams = 4) {
    DeskdConfig c;
    c.data_dir = dir.path;
    c.ui_dir = std::string(DESKD_SOURCE_DIR) + "/ui/desk";
    c.http_port = 0;
    c.max_streams = max_streams;
    std::string err;
    auto lock = AccountLock::acquire(dir.path, "demo", "deskd-test-web", false, &err);
    d = std::make_unique<Deskd>(c, std::move(lock));
    if (!d->start(&err)) {
      std::fprintf(stderr, "deskd did not start: %s\n", err.c_str());
      std::exit(2);
    }
  }
  int port() const { return d->port(); }
  std::string host() const { return "127.0.0.1:" + std::to_string(port()); }
};

static std::unique_ptr<httplib::Client> client(int port) {
  auto c = std::make_unique<httplib::Client>("127.0.0.1", port);
  c->set_connection_timeout(2, 0);
  c->set_read_timeout(5, 0);
  return c;
}
static httplib::Result post(int port, const std::string& path, const std::string& body, const std::string& token,
                            const std::string& host = "", const std::string& ctype = "application/json",
                            const httplib::Headers& extra = {}) {
  httplib::Headers h = extra;
  if (!token.empty()) h.emplace("X-Desk", token);
  if (!host.empty()) h.emplace("Host", host);
  return client(port)->Post(path, h, body, ctype);
}
static httplib::Result get(int port, const std::string& path, const std::string& host = "",
                           const httplib::Headers& extra = {}) {
  httplib::Headers h = extra;
  if (!host.empty()) h.emplace("Host", host);
  return client(port)->Get(path, h);
}
static std::string err_code(const httplib::Result& r) {
  if (!r) return "NO ANSWER";
  json j = json::parse(r->body, nullptr, false);
  if (j.is_discarded() || !j.contains("err")) return "";
  return j["err"].value("code", "");
}
static bool eventually(const std::function<bool()>& f, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (f()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return f();
}

// One EventSource, on its own thread.
struct SseReader {
  std::thread th;
  std::mutex m;
  std::string buf;
  std::atomic<bool> stop{false}, done{false}, complete{false};   // complete: ended with its closing chunk
  std::atomic<int> status{0};
  SseReader(int port, const std::string& sid, const std::string& host = "", const httplib::Headers& extra = {}) {
    th = std::thread([this, port, sid, host, extra] {
      httplib::Client cli("127.0.0.1", port);
      cli.set_read_timeout(10, 0);
      httplib::Headers h = extra;
      if (!host.empty()) h.emplace("Host", host);
      auto r = cli.Get(
          "/api/stream?sid=" + sid, h,
          [this](const httplib::Response& res) {
            status.store(res.status);
            return true;
          },
          [this](const char* data, std::size_t len) {
            std::lock_guard<std::mutex> lk(m);
            buf.append(data, len);
            return !stop.load();
          });
      if (r) status.store(r->status);
      complete.store(static_cast<bool>(r));
      done.store(true);
    });
  }
  ~SseReader() { close(); }
  std::string text() {
    std::lock_guard<std::mutex> lk(m);
    return buf;
  }
  // the SSE messages so far: {event, id, data} per block (the retry block has neither)
  std::vector<json> messages() {
    std::vector<json> out;
    const std::string t = text();
    std::size_t at = 0;
    while (true) {
      const std::size_t end = t.find("\n\n", at);
      if (end == std::string::npos) break;
      const std::string block = t.substr(at, end - at);
      at = end + 2;
      json msg = json::object();
      std::size_t ls = 0;
      while (ls <= block.size()) {
        std::size_t le = block.find('\n', ls);
        if (le == std::string::npos) le = block.size();
        const std::string line = block.substr(ls, le - ls);
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos) msg[line.substr(0, colon)] = line.substr(colon + 2);
        else if (!line.empty()) msg[line] = "";
        ls = le + 1;
      }
      out.push_back(msg);
    }
    return out;
  }
  std::vector<json> envelopes() {
    std::vector<json> out;
    for (const auto& m : messages())
      if (m.contains("data")) out.push_back(json::parse(m["data"].get<std::string>()));
    return out;
  }
  bool wait_events(std::size_t n, std::chrono::milliseconds limit) {
    return eventually([&] { return envelopes().size() >= n; }, limit);
  }
  void close() {
    stop.store(true);
    if (th.joinable()) th.join();
  }
};

static int raw_connect(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
static std::string raw_request(int port, const std::string& req) {
  const int fd = raw_connect(port);
  if (fd < 0) return "";
  timeval tv{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ssize_t w = ::write(fd, req.data(), req.size());
  (void)w;
  std::string out;
  char b[4096];
  ssize_t n;
  while ((n = ::read(fd, b, sizeof b)) > 0) out.append(b, static_cast<std::size_t>(n));
  ::close(fd);
  return out;
}

// ---------------------------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------------------------
static void test_guards_and_page(Desk& k) {
  const int port = k.port();
  const std::string token = k.d->token();
  CHECK(token.size() == 32);
  CHECK(host_ok("127.0.0.1:" + std::to_string(port), port));
  CHECK(host_ok("LOCALHOST:" + std::to_string(port), port));
  CHECK(!host_ok("127.0.0.1", port));
  CHECK(!host_ok("127.0.0.1:" + std::to_string(port) + ".evil.example", port));
  CHECK(token_ok(token, token));
  CHECK(!token_ok("", ""));
  CHECK(!token_ok(token.substr(1), token));
  std::string out;
  CHECK(!inject_token("<html></html>", "t", &out));

  // the page carries this start's token; the file on disk does not
  auto page = get(port, "/");
  CHECK(page && page->status == 200);
  CHECK(page && page->body.find("<meta name=\"desk-token\" content=\"" + token + "\">") != std::string::npos);
  CHECK(page && page->get_header_value("Cache-Control") == "no-store");
  auto page2 = get(port, "/index.html", "localhost:" + std::to_string(port));
  CHECK(page2 && page2->status == 200);
  auto js = get(port, "/js/contract.js");
  CHECK(js && js->status == 200 && js->get_header_value("Content-Type").find("text/javascript") == 0);
  auto css = get(port, "/desk.css");
  CHECK(css && css->status == 200);

  // Host: refused on every request whose Host is not 127.0.0.1:<port> or localhost:<port>
  for (const std::string& bad : {std::string("evil.example:") + std::to_string(port), std::string("127.0.0.1:1"),
                                 std::string("127.0.0.1"), std::string("desk.localhost:") + std::to_string(port)}) {
    auto r = get(port, "/", bad);
    CHECK(r && r->status == 403 && err_code(r) == "FORBIDDEN");
    CHECK(r && r->body.find(token) == std::string::npos);   // never the page, never the token
    auto h = get(port, "/api/health", bad);
    CHECK(h && h->status == 403);
    capture("GET /api/health", h);
    auto p = post(port, "/api/kill", "{}", token, bad);
    CHECK(p && p->status == 403 && err_code(p) == "FORBIDDEN");
  }
  CHECK(!k.d->kill_switch().active());   // no refused KILL got through
  const std::string nohost = raw_request(port, "GET /api/health HTTP/1.1\r\nConnection: close\r\n\r\n");
  CHECK(nohost.find("HTTP/1.1 403") == 0);

  // assets stay inside the ui dir
  for (const std::string& p : {std::string("/../CMakeLists.txt"), std::string("/js/../../CMakeLists.txt"),
                               std::string("/%2e%2e/CMakeLists.txt"), std::string("/.git/config")}) {
    const std::string r = raw_request(port, "GET " + p + " HTTP/1.1\r\nHost: " + k.host() + "\r\nConnection: close\r\n\r\n");
    CHECK(r.find("HTTP/1.1 200") != 0);
    CHECK(r.find("cmake_minimum_required") == std::string::npos);
  }
  auto none = get(port, "/api/nope");
  CHECK(none && none->status == 404 && err_code(none) == "NOT_FOUND");

  // token: every POST, KILL included
  auto t0 = post(port, "/api/kill", "{}", "");
  CHECK(t0 && t0->status == 403 && err_code(t0) == "FORBIDDEN");
  capture("POST /api/kill", t0);
  auto t1 = post(port, "/api/kill", "{}", std::string(32, 'a'));
  CHECK(t1 && t1->status == 403);
  auto t2 = post(port, "/api/stop", "{}", token.substr(0, 31));
  CHECK(t2 && t2->status == 403);
  CHECK(!k.d->kill_switch().active());
  auto ct = post(port, "/api/start", "{}", token, "", "text/plain");
  CHECK(ct && ct->status == 400 && err_code(ct) == "BAD_REQUEST");
  auto nj = post(port, "/api/start", "{nope", token);
  CHECK(nj && nj->status == 400);
  auto unknown = post(port, "/api/nope", "{}", token);
  CHECK(unknown && unknown->status == 404 && err_code(unknown) == "NOT_FOUND");
  CHECK(unknown && unknown->get_header_value("Access-Control-Allow-Origin").empty());   // no CORS, ever
  // an unknown path is no way round the guards: the token first (until 2026-10-06 it got 404)
  auto unknown_nt = post(port, "/api/nope", "{}", "");
  CHECK(unknown_nt && unknown_nt->status == 403 && err_code(unknown_nt) == "FORBIDDEN");
  auto unknown_bh = post(port, "/nope", "{}", token, "evil.example:" + std::to_string(port));
  CHECK(unknown_bh && unknown_bh->status == 403);

  // exactly one Host: a second one, the first good, is refused (it was let in until 2026-10-06)
  const std::string two_hosts = raw_request(port, "GET /api/health HTTP/1.1\r\nHost: " + k.host() +
                                                      "\r\nHost: evil:1\r\nConnection: close\r\n\r\n");
  CHECK(two_hosts.find("HTTP/1.1 403") == 0);
  const std::string two_hosts_post = raw_request(port, "POST /api/kill HTTP/1.1\r\nHost: " + k.host() + "\r\nHost: " +
                                                           k.host() + "\r\nX-Desk: " + token +
                                                           "\r\nContent-Length: 2\r\n\r\n{}");
  CHECK(two_hosts_post.find("HTTP/1.1 403") == 0);
  CHECK(!k.d->kill_switch().active());

  // A refused POST's body is never read as a request: the connection is closed after the
  // refusal. The body here is a whole request of its own, with a good Host (what a cross-site
  // text/plain form can carry); were it parsed, a second response would follow the 403.
  {
    const std::string inner = "GET /api/health HTTP/1.1\r\nHost: " + k.host() + "\r\n\r\n";
    const std::string r = raw_request(port, "POST /api/kill HTTP/1.1\r\nHost: " + k.host() +
                                                "\r\nOrigin: https://evil.example\r\nContent-Type: text/plain\r\nContent-Length: " +
                                                std::to_string(inner.size()) + "\r\n\r\n" + inner);
    CHECK(r.find("HTTP/1.1 403") == 0);
    CHECK(r.find("HTTP/1.1 200") == std::string::npos);
    CHECK(r.find("Connection: close") != std::string::npos);
  }
  // a GET with a body (httplib never reads it): refused and closed the same way
  {
    const std::string inner = "GET /api/health HTTP/1.1\r\nHost: " + k.host() + "\r\n\r\n";
    const std::string r = raw_request(port, "GET /api/health HTTP/1.1\r\nHost: " + k.host() + "\r\nContent-Length: " +
                                                std::to_string(inner.size()) + "\r\n\r\n" + inner);
    CHECK(r.find("HTTP/1.1 400") == 0);
    CHECK(r.find("HTTP/1.1 200") == std::string::npos);
  }

  // cross-site: refused on every method, KILL included; the page's own origin and requests with
  // neither header (deskctl, curl) pass
  const std::string P = std::to_string(port);
  for (const auto& hdr : std::vector<httplib::Headers>{{{"Origin", "https://evil.example"}},
                                                       {{"Sec-Fetch-Site", "cross-site"}},
                                                       {{"Sec-Fetch-Site", "same-site"}},
                                                       {{"Origin", "null"}},
                                                       {{"Origin", "http://127.0.0.1:1"}},
                                                       {{"Origin", "http://127.0.0.1:" + P}, {"Origin", "https://evil.example"}}}) {
    auto g = get(port, "/api/stream?sid=evilstream1", "", hdr);
    CHECK(g && g->status == 403 && err_code(g) == "FORBIDDEN");
    auto pg = get(port, "/", "", hdr);
    CHECK(pg && pg->status == 403 && pg->body.find(token) == std::string::npos);
    auto kr = post(port, "/api/kill", "{}", token, "", "application/json", hdr);
    CHECK(kr && kr->status == 403 && err_code(kr) == "FORBIDDEN");
    capture("POST /api/kill", kr);
  }
  CHECK(!k.d->kill_switch().active());
  CHECK(k.d->pub().streams().open_count() == 0);
  for (const auto& hdr : std::vector<httplib::Headers>{{{"Origin", "http://127.0.0.1:" + P}, {"Sec-Fetch-Site", "same-origin"}},
                                                       {{"Origin", "http://LOCALHOST:" + P}},
                                                       {{"Sec-Fetch-Site", "none"}}}) {
    auto h = get(port, "/api/health", "", hdr);
    CHECK(h && h->status == 200);
    auto st = post(port, "/api/cancel_all", R"({"own":"MAN","v":null})", token, "", "application/json", hdr);
    CHECK(st && st->status == 200);
  }
  CHECK(cross_site_refusal("Cross-Site", nullptr, 8788).find("cross-site") != std::string::npos);
  CHECK(cross_site_refusal("same-origin", nullptr, 8788).empty());
  // the PAPER/LIVE switch: the other desk's page (same-site) may open this page by a click, nothing else
  {
    const httplib::Headers nav{{"Sec-Fetch-Site", "same-site"}, {"Sec-Fetch-Mode", "navigate"},
                               {"Sec-Fetch-Dest", "document"}, {"Sec-Fetch-User", "?1"}};
    auto page = get(port, "/", "", nav);
    CHECK(page && page->status == 200 && page->body.find("desk-token\" content=\"" + token) != std::string::npos);
    auto api = get(port, "/api/health", "", nav);
    CHECK(api && api->status == 403);
    auto frame = get(port, "/", "", {{"Sec-Fetch-Site", "same-site"}, {"Sec-Fetch-Mode", "navigate"},
                                     {"Sec-Fetch-Dest", "iframe"}, {"Sec-Fetch-User", "?1"}});
    CHECK(frame && frame->status == 403);
    auto unasked = get(port, "/", "", {{"Sec-Fetch-Site", "same-site"}, {"Sec-Fetch-Mode", "navigate"}, {"Sec-Fetch-Dest", "document"}});
    CHECK(unasked && unasked->status == 403);
    auto far = get(port, "/", "", {{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Mode", "navigate"},
                                   {"Sec-Fetch-Dest", "document"}, {"Sec-Fetch-User", "?1"}});
    CHECK(far && far->status == 403);
    CHECK(peer_navigation("GET", "/index.html", "Same-Site", "navigate", "document", "?1", false));
    CHECK(!peer_navigation("GET", "/", "same-site", "navigate", "document", "?1", true));
    CHECK(!peer_navigation("POST", "/", "same-site", "navigate", "document", "?1", false));
    CHECK(!peer_navigation("GET", "/js/main.js", "same-site", "navigate", "document", "?1", false));
  }

  // over 64 KiB: Host, origin and token still come first; a good one is BAD_REQUEST
  {
    const std::string big(70 * 1024, ' ');
    auto bh = post(port, "/api/order", big, token, "evil.example:" + P);
    CHECK(bh && bh->status == 403);
    auto bt = post(port, "/api/order", big, "");
    CHECK(bt && bt->status == 403 && err_code(bt) == "FORBIDDEN");
    auto ok = post(port, "/api/order", big, token);
    CHECK(ok && ok->status == 400 && err_code(ok) == "BAD_REQUEST");
    capture("POST /api/order", ok);
  }

  // the contract's replies for an empty universe
  auto health = get(port, "/api/health");
  CHECK(health && health->status == 200);
  capture("GET /api/health", health);
  CHECK(json::parse(health->body)["epoch"] == k.d->epoch());
  auto start = post(port, "/api/start", "{}", token);
  CHECK(start && start->status == 409 && err_code(start) == "PREFLIGHT");
  CHECK(start && json::parse(start->body)["preflight"].size() == 8);
  capture("POST /api/start", start);
  auto stop = post(port, "/api/stop", "{}", token);
  CHECK(stop && stop->status == 200 && json::parse(stop->body)["run"]["state"] == "STOP");
  capture("POST /api/stop", stop);
  auto rearm = post(port, "/api/rearm", "{}", token);
  CHECK(rearm && rearm->status == 409 && err_code(rearm) == "BAD_STATE");
  capture("POST /api/rearm", rearm);
  const std::string ord = R"({"cid":"Mk3f9x2-17","v":"K","tk":"KXNATGAS15M-26OCT061930-30","side":"BUY","px":"0.6300",)"
                          R"("qty":10,"tif":"POST","ttl_s":15,"override":false,"touch":"0.6300","t_click_ms":1791307199950})";
  auto o = post(port, "/api/order", ord, token);
  // gw answers: the Manual path is not attached in this runtime yet, so read-only, nothing sent
  CHECK(o && o->status == 409 && err_code(o) == "RO" && !json::parse(o->body).contains("ord"));
  CHECK(k.d->gw().stats().place_refused >= 1);
  capture("POST /api/order", o);
  auto cx = post(port, "/api/cancel", R"({"cids":["Mk3f9x2-17"]})", token);
  CHECK(cx && cx->status == 200);
  capture("POST /api/cancel", cx);
  auto cxm = post(port, "/api/cancel", R"({"v":"K","tk":"KXA","own":"MAN","side":null,"px":null})", token);
  CHECK(cxm && cxm->status == 404);
  capture("POST /api/cancel", cxm);
  auto ca = post(port, "/api/cancel_all", R"({"own":"MAN","v":null})", token);
  CHECK(ca && ca->status == 200);
  capture("POST /api/cancel_all", ca);
  auto am = post(port, "/api/amend", R"({"cid":"Mk3f9x2-17","px":"0.6200","qty":null,"ttl_s":null})", token);
  CHECK(am && am->status == 404);
  capture("POST /api/amend", am);
  auto ln = post(port, "/api/lane/M1", R"({"on":false})", token);
  CHECK(ln && ln->status == 404);
  capture("POST /api/lane/{id}", ln);
  auto rl = post(port, "/api/release", R"({"v":"K","tk":"KXA"})", token);
  CHECK(rl && rl->status == 404);
  capture("POST /api/release", rl);
  auto tr = post(port, "/api/transfer", R"({"from":"S1","to":"S3","amt":"12.0000"})", token);
  CHECK(tr && tr->status == 409 && err_code(tr) == "NO_CASH");
  capture("POST /api/transfer", tr);
}

static void test_stream(Desk& k) {
  const int port = k.port();
  const std::string token = k.d->token();
  // a snapshot or a subscribe for a sid with no open stream: 409 BAD_STATE
  auto s0 = get(port, "/api/snapshot?sid=nostream1");
  CHECK(s0 && s0->status == 409 && err_code(s0) == "BAD_STATE");
  capture("GET /api/snapshot", s0);
  auto sub0 = post(port, "/api/subscribe", R"({"sid":"nostream1","ladders":[],"tops":"ALL"})", token);
  CHECK(sub0 && sub0->status == 409);
  capture("POST /api/subscribe", sub0);
  auto badsid = get(port, "/api/stream?sid=x");
  CHECK(badsid && badsid->status == 400);

  SseReader r(port, "streamtest1");
  CHECK(r.wait_events(3, 4000ms));   // hb at open, then one a second
  CHECK(r.status.load() == 200);
  const auto msgs = r.messages();
  CHECK(!msgs.empty() && msgs[0].contains("retry") && msgs[0]["retry"] == "2000" && !msgs[0].contains("data"));
  std::uint64_t want = 1;
  for (const auto& m : msgs) {
    if (!m.contains("data")) continue;
    const json env = json::parse(m["data"].get<std::string>());
    CHECK(m["event"] == env["type"]);
    CHECK(m["id"] == std::to_string(env["seq"].get<std::uint64_t>()));
    CHECK(env["seq"] == want);   // 1, 2, 3 ... every message of any type
    ++want;
    CHECK(env["ts_ms"].is_number_integer());
    if (env["type"] == "hb") {
      CHECK(env["data"]["sid"] == "streamtest1");
      CHECK(env["data"]["epoch"] == k.d->epoch());
      CHECK(env["data"]["feeds"]["KX"]["up"] == false);   // no venue connection yet: honest
    }
  }
  CHECK(r.envelopes().front()["type"] == "hb");

  // subscribe, then a snapshot that carries it, taken against the stream's counter
  auto sub = post(port, "/api/subscribe",
                  R"({"sid":"streamtest1","ladders":[{"v":"K","tk":"KXNATGAS15M-26OCT061930-30"}],"tops":"ALL"})", token);
  CHECK(sub && sub->status == 200);
  capture("POST /api/subscribe", sub);
  auto subbad = post(port, "/api/subscribe", R"({"sid":"streamtest1","ladders":[{"v":"X","tk":"A"}],"tops":"ALL"})", token);
  CHECK(subbad && subbad->status == 400);
  const std::uint64_t before = r.envelopes().back()["seq"].get<std::uint64_t>();
  auto snap = get(port, "/api/snapshot?sid=streamtest1");
  CHECK(snap && snap->status == 200);
  json sj = snap ? json::parse(snap->body) : json();
  if (snap) g_capture["snapshots"].push_back(sj);
  capture("GET /api/snapshot", snap);
  CHECK(sj["seq"].get<std::uint64_t>() >= before);
  CHECK(sj["subs"]["tops"] == "ALL" && sj["subs"]["ladders"].size() == 1);
  CHECK(sj["hdr"]["mode"] == "DEMO");
  CHECK(sj["hdr"]["cash"]["K"]["ro"] == true);
  CHECK(sj["hdr"]["run"]["state"] == "STOP");
  CHECK(sj["alerts"].size() >= 1 && sj["alerts"][0]["src"] == "OPS");
  CHECK(r.wait_events(static_cast<std::size_t>(sj["seq"].get<std::uint64_t>()) + 1, 3000ms));
  CHECK(r.envelopes().back()["seq"].get<std::uint64_t>() > sj["seq"].get<std::uint64_t>());
  // hb is 1/s: about 3 over 2.5 s more, never a burst
  const std::size_t n0 = r.envelopes().size();
  std::this_thread::sleep_for(2500ms);
  const std::size_t hbs = r.envelopes().size() - n0;
  CHECK(hbs >= 2 && hbs <= 4);

  // takeover: a second stream with the same sid ends the first and starts again at seq 1
  SseReader r2(port, "streamtest1");
  CHECK(r2.wait_events(1, 3000ms));
  CHECK(eventually([&] { return r.done.load(); }, 2000ms));
  CHECK(r2.envelopes()[0]["seq"] == 1);
  CHECK(k.d->pub().streams().open_count() == 1);
  for (const auto& e : r.envelopes()) g_capture["events"].push_back(e);
  r2.close();
  // the sid's subscription survives the disconnect (60 s), but its snapshot needs an open stream
  CHECK(eventually([&] { return k.d->pub().streams().open_count() == 0; }, 3000ms));
  CHECK(k.d->pub().streams().subs("streamtest1").tops_all);
  auto s1 = get(port, "/api/snapshot?sid=streamtest1");
  CHECK(s1 && s1->status == 409);
}

static void test_stream_limit(Desk& k) {
  const int port = k.port();
  std::vector<std::unique_ptr<SseReader>> rs;
  for (int i = 0; i < 4; ++i) rs.push_back(std::make_unique<SseReader>(port, "limit00" + std::to_string(i)));
  for (auto& r : rs) CHECK(r->wait_events(1, 3000ms));
  CHECK(k.d->pub().streams().open_count() == 4);
  SseReader fifth(port, "limit004");
  CHECK(eventually([&] { return fifth.done.load(); }, 3000ms));
  CHECK(fifth.status.load() == 409);
  CHECK(fifth.text().find("BAD_STATE") != std::string::npos);
  // the takeover of an open sid is not a fifth stream
  SseReader again(port, "limit000");
  CHECK(again.wait_events(1, 3000ms));
  CHECK(again.status.load() == 200);
  // a closed stream frees its slot as soon as the server sees the socket go
  rs[1]->close();
  CHECK(eventually([&] { return k.d->pub().streams().open_count() == 3; }, 3000ms));
  SseReader sixth(port, "limit005");
  CHECK(sixth.wait_events(1, 3000ms));
  CHECK(sixth.status.load() == 200);
}

// The plan's web test: 10 idle connections hold 10 of the 16 threads, and a POST still answers
// at once (the old desk's 9 threads all sat in idle keep-alives once: a click waited 4.8 s).
static void test_idle_connections(Desk& k) {
  const int port = k.port();
  std::vector<int> fds;
  for (int i = 0; i < 10; ++i) fds.push_back(raw_connect(port));
  for (int fd : fds) CHECK(fd >= 0);
  std::this_thread::sleep_for(100ms);
  auto c = client(port);
  httplib::Headers h = {{"X-Desk", k.d->token()}};
  const auto t0 = std::chrono::steady_clock::now();
  auto r = c->Post("/api/cancel_all", h, R"({"own":"MAN","v":null})", "application/json");
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r && r->status == 200);
  std::printf("POST beside 10 idle connections: %.2f ms\n", ms);
  CHECK(ms < 50);   // the plan's target is 5 ms; 50 leaves room for a loaded test machine
  for (int fd : fds) ::close(fd);
}

static void test_stream_queue_gap() {
  StreamQueue q(2);
  CHECK(q.offer("hb", 1791306000000, "{}") == 1);
  CHECK(q.offer("hb", 1791306000001, "{}") == 2);
  CHECK(q.offer("hb", 1791306000002, "{}") == 3);   // full: dropped, but seq 3 is spent
  CHECK(q.dropped() == 1);
  const std::string a = q.drain_for(0ms);
  CHECK(a.find("id: 1\n") != std::string::npos && a.find("id: 2\n") != std::string::npos);
  CHECK(a.find("id: 3\n") == std::string::npos);
  CHECK(q.offer("hb", 1791306000003, "{}") == 4);
  const std::string b = q.drain_for(0ms);
  CHECK(b.find("id: 4\n") != std::string::npos);    // the page sees 2 then 4: a gap, so it resyncs
  CHECK(q.last_seq() == 4);
  CHECK(sse_frame("mo", 1042, 1791307213000, R"({"fid":"K-F-1"})") ==
        "event: mo\nid: 1042\ndata: {\"type\":\"mo\",\"seq\":1042,\"ts_ms\":1791307213000,\"data\":{\"fid\":\"K-F-1\"}}\n\n");
  q.close();
  CHECK(q.drain_for(1000ms).empty());
}

// KILL's own path: it answers, and send-cx gets it, while gw is stuck inside a handler.
static void test_kill_with_gw_frozen() {
  Desk k;
  const int port = k.port();
  SseReader r(port, "killtest01");
  CHECK(r.wait_events(1, 3000ms));
  std::promise<void> entered, release;
  std::shared_future<void> rel = release.get_future().share();
  k.d->gw().post(Probe{[&entered, rel] {
    entered.set_value();
    rel.wait();
  }});
  entered.get_future().wait();   // gw is frozen from here

  const auto t0 = std::chrono::steady_clock::now();
  auto kr = post(port, "/api/kill", "{}", k.d->token());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("KILL with gw frozen: %.2f ms\n", ms);
  CHECK(kr && kr->status == 200);
  CHECK(ms < 250);
  capture("POST /api/kill", kr);
  json kj = kr ? json::parse(kr->body) : json();
  CHECK(kj["run"]["state"] == "KILLED");
  CHECK(kj["run"]["rearm_ms"].get<std::int64_t>() - kj["run"]["since_ms"].get<std::int64_t>() == 60000);
  CHECK(eventually([&] { return k.d->cx_stats().kills.load() >= 1; }, 1000ms));   // send-cx, not gw
  CHECK(k.d->gw().run_view().state == RunState::KILLED);
  // the screen hears it while gw is still frozen: hdr with KILLED and the KILL alerts
  CHECK(eventually([&] {
    for (const auto& e : r.envelopes())
      if (e["type"] == "hdr" && e["data"]["run"]["state"] == "KILLED") return true;
    return false;
  }, 2000ms));
  auto again = post(port, "/api/kill", "{}", k.d->token());   // idempotent: same times
  CHECK(again && json::parse(again->body)["run"]["since_ms"] == kj["run"]["since_ms"]);
  release.set_value();

  auto start = post(port, "/api/start", "{}", k.d->token());
  CHECK(start && start->status == 409 && err_code(start) == "KILLED");
  capture("POST /api/start", start);
  auto rearm = post(port, "/api/rearm", "{}", k.d->token());
  CHECK(rearm && rearm->status == 409 && err_code(rearm) == "BAD_STATE");
  CHECK(eventually([&] {
    int alerts = 0;
    for (const auto& e : r.envelopes())
      if (e["type"] == "alert" && e["data"]["src"] == "KILL") ++alerts;
    return alerts >= 2;   // the route's and send-cx's
  }, 2000ms));
  auto snap = get(port, "/api/snapshot?sid=killtest01");
  CHECK(snap && snap->status == 200);
  if (snap) {
    json sj = json::parse(snap->body);
    CHECK(sj["hdr"]["run"]["state"] == "KILLED");
    g_capture["snapshots"].push_back(sj);
  }
  for (const auto& e : r.envelopes()) g_capture["events"].push_back(e);
}

// The strict request checker: each body goes to deskd and its verdict is kept for contract.js,
// which must agree (contract_check.cjs). The issue's probes are among them, and each is 400.
static void test_request_checker(Desk& k) {
  const int port = k.port();
  const std::string token = k.d->token();
  struct Probe {
    const char* path;
    const char* key;
    std::string body;
  };
  const std::string ok_ord = R"("cid":"Mk3f9x2-17","v":"K","tk":"KXNATGAS15M-26OCT061930-30","side":"BUY",)";
  const std::vector<Probe> probes = {
      // POST /api/order
      {"/api/order", "POST /api/order", R"({"cid":"x","v":"K","tk":"A"})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"1.5","qty":-1,"tif":"POST","ttl_s":7,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"POST","ttl_s":15,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"POST","ttl_s":null,"exp_ms":1791307200000,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"POST","ttl_s":15,"exp_ms":1791307200000,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"POST","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"POST","ttl_s":null,"exp_ms":1791307200500,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"IOC","ttl_s":15,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"reduce_only":true,"override":false,"touch":"0.6300","t_click_ms":1791307199950})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"FOK","ttl_s":null,"reduce_only":true,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"FOK","ttl_s":null,"reduce_only":false,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.0000","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"1.0000","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":0,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":0.005,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":2.5,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"override":"no","touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null,"extra":1})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":0.63,"qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"GTC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "{" + ok_ord + R"("px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":17913071999})"},
      {"/api/order", "POST /api/order", R"({"cid":"Mk3f9x2-17","v":"X","tk":"A","side":"BUY","px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", R"({"cid":"M k3f9x2","v":"K","tk":"A","side":"BUY","px":"0.6300","qty":10,"tif":"IOC","ttl_s":null,"override":false,"touch":null,"t_click_ms":null})"},
      {"/api/order", "POST /api/order", "[]"},
      // POST /api/cancel
      {"/api/cancel", "POST /api/cancel", R"({"cids":["!!"]})"},
      {"/api/cancel", "POST /api/cancel", R"({"cids":[]})"},
      {"/api/cancel", "POST /api/cancel", R"({"cids":["Mk3f9x2-17"],"v":"K"})"},
      {"/api/cancel", "POST /api/cancel", R"({"v":"K","tk":"KXA","own":"MAN","side":"BUY","px":"0.6300"})"},
      {"/api/cancel", "POST /api/cancel", R"({"v":"K","tk":"KXA","own":"EXT","side":null,"px":null})"},
      {"/api/cancel", "POST /api/cancel", R"({"v":"K","tk":"KXA","own":"MAN"})"},
      // POST /api/cancel_all
      {"/api/cancel_all", "POST /api/cancel_all", R"({"own":"BOGUS","v":null})"},
      {"/api/cancel_all", "POST /api/cancel_all", R"({"own":"MAN"})"},
      {"/api/cancel_all", "POST /api/cancel_all", R"({"own":"ANY","v":"P"})"},
      // POST /api/amend
      {"/api/amend", "POST /api/amend", R"({"cid":"Mk3f9x2-17","px":null,"qty":null,"ttl_s":null})"},
      {"/api/amend", "POST /api/amend", R"({"cid":"Mk3f9x2-17","px":null,"qty":null,"ttl_s":60})"},
      {"/api/amend", "POST /api/amend", R"({"cid":"Mk3f9x2-17","px":"1.0000","qty":null,"ttl_s":null})"},
      {"/api/amend", "POST /api/amend", R"({"cid":"Mk3f9x2-17","px":null,"qty":0,"ttl_s":null})"},
      {"/api/amend", "POST /api/amend", R"({"cid":"Mk3f9x2-17","px":null,"qty":4,"ttl_s":61})"},
      // POST /api/transfer
      {"/api/transfer", "POST /api/transfer", R"({"from":"S1","to":"S3","amt":"-1.0000"})"},
      {"/api/transfer", "POST /api/transfer", R"({"from":"S1","to":"S3","amt":"0.0000"})"},
      {"/api/transfer", "POST /api/transfer", R"({"from":"S1","to":"S3","amt":"12.00"})"},
      {"/api/transfer", "POST /api/transfer", R"({"from":"S1","to":"S3","amt":"12.0000"})"},
      // POST /api/release, /api/lane/{id}, /api/watch, /api/start, /api/rearm, /api/subscribe
      {"/api/release", "POST /api/release", R"({"v":"X","tk":""})"},
      {"/api/release", "POST /api/release", R"({"v":"K","tk":"KXA"})"},
      {"/api/lane/M1", "POST /api/lane/{id}", R"({"on":true,"x":1})"},
      {"/api/lane/M1", "POST /api/lane/{id}", R"({"on":1})"},
      {"/api/lane/M1", "POST /api/lane/{id}", R"({"on":true})"},
      {"/api/watch", "POST /api/watch", R"({"v":"K","tk":"KXHIGHDEN-26OCT06-B71.5","on":true})"},
      {"/api/watch", "POST /api/watch", R"({"v":"K","tk":"KXHIGHDEN-26OCT06-B71.5"})"},
      {"/api/start", "POST /api/start", R"({"now":true})"},
      {"/api/rearm", "POST /api/rearm", R"({"x":1})"},
      {"/api/subscribe", "POST /api/subscribe", R"({"sid":"nostream1","ladders":[],"tops":"SOME"})"},
      {"/api/subscribe", "POST /api/subscribe", R"({"sid":"nostream1","ladders":[{"v":"K"}],"tops":[]})"},
      {"/api/subscribe", "POST /api/subscribe", R"({"sid":"nostream1","ladders":[],"tops":[{"v":"P","tk":"x"}]})"},
  };
  int refused = 0;
  for (const auto& pr : probes) {
    auto r = post(port, pr.path, pr.body, token);
    CHECK(r && r->status != 500);
    if (!r) continue;
    const bool bad = r->status == 400 && err_code(r) == "BAD_REQUEST";
    refused += bad;
    g_capture["requests"].push_back({{"key", pr.key}, {"body", json::parse(pr.body)}, {"accepted", !bad}});
    capture(pr.key, r);
  }
  std::printf("request checker: %zu probe bodies, %d refused BAD_REQUEST\n", probes.size(), refused);
  // the review's probes, each now 400 rather than a handler's 404, 409 or 200
  auto code_of = [&](const char* path, const std::string& body) { return err_code(post(port, path, body, token)); };
  CHECK(code_of("/api/order", R"({"cid":"x","v":"K","tk":"A"})") == "BAD_REQUEST");
  CHECK(code_of("/api/cancel", R"({"cids":["!!"]})") == "BAD_REQUEST");
  CHECK(code_of("/api/cancel_all", R"({"own":"BOGUS","v":null})") == "BAD_REQUEST");
  CHECK(code_of("/api/cancel_all", R"({"own":"MAN"})") == "BAD_REQUEST");
  CHECK(code_of("/api/amend", R"({"cid":"Mk3f9x2-17","px":null,"qty":null,"ttl_s":null})") == "BAD_REQUEST");
  CHECK(code_of("/api/transfer", R"({"from":"S1","to":"S3","amt":"-1.0000"})") == "BAD_REQUEST");
  CHECK(code_of("/api/release", R"({"v":"X","tk":""})") == "BAD_REQUEST");
  CHECK(code_of("/api/lane/M1", R"({"on":true,"x":1})") == "BAD_REQUEST");
  // KILL and Stop are never refused for their body
  auto st = post(port, "/api/stop", R"({"why":"anything"})", token, "", "text/plain");
  CHECK(st && st->status == 200);

  // THE UNIVERSE (contract 1.5): an empty page for each day, its ET date, its grev (groups null
  // only when the query sends that grev back); bad queries refused
  const std::int64_t now = now_ms();
  for (const std::string& q : {std::string("day=T"), std::string("day=T1&cat=WEATHER&v=K&q=den&sort=vol&offset=0&limit=100"),
                               std::string("day=T&cat=&v=&limit=200&offset=400"), std::string("day=T1&sort=edge"),
                               std::string("day=T&grev=0"), std::string("day=T&grev=1x9f2a.1")}) {
    auto r = get(port, "/api/universe?" + q);
    CHECK(r && r->status == 200);
    capture("GET /api/universe", r);
    g_capture["queries"].push_back({{"key", "GET /api/universe"}, {"path", "/api/universe?" + q}, {"accepted", r && r->status == 200}});
    if (r && r->status == 200) {
      const json j = json::parse(r->body);
      CHECK(j["total"] == 0 && j["rows"].empty() && j["cats"]["SPORTS"] == 0 && j["cats"].size() == 5);
      CHECK(j["grev"] == kEmptyUniverseGrev);
      if (q.find("grev=0") != std::string::npos) CHECK(j["groups"].is_null());
      else CHECK(j["groups"].is_array() && j["groups"].empty());
      const std::string today = et_date(now);
      CHECK(j["date"] == (q.compare(0, 6, "day=T1") == 0 ? et_date_after(today) : today) || j["date"] == et_date(now_ms()));
    }
  }
  for (const std::string& q : {std::string(""), std::string("day=T2"), std::string("day=T&limit=0"), std::string("day=T&limit=201"),
                               std::string("day=T&offset=-1"), std::string("day=T&sort=price"), std::string("day=T&cat=CRYPTO"),
                               std::string("day=T&v=X"), std::string("day=T&page=2")}) {
    auto r = get(port, "/api/universe?" + q);
    CHECK(r && r->status == 400 && err_code(r) == "BAD_REQUEST");
    capture("GET /api/universe", r);
    g_capture["queries"].push_back({{"key", "GET /api/universe"}, {"path", "/api/universe?" + q}, {"accepted", r && r->status != 400}});
  }
  auto w = post(port, "/api/watch", R"({"v":"K","tk":"KXHIGHDEN-26OCT06-B71.5","on":true})", token);
  CHECK(w && w->status == 404 && err_code(w) == "UNKNOWN_MARKET");
  capture("POST /api/watch", w);

  // ET dates (checked against Python's zoneinfo): EDT and EST, and the nights where the wrong
  // offset would give the wrong day
  CHECK(et_date(1791300000000) == "2026-10-06");    // 15:20Z, EDT
  CHECK(et_date(1791345599000) == "2026-10-06");    // 2026-10-07T03:59:59Z = 23:59:59 EDT
  CHECK(et_date(1791345600000) == "2026-10-07");
  CHECK(et_date(1793505599000) == "2026-10-31");    // 2026-11-01T03:59:59Z, still EDT
  CHECK(et_date(1793505600000) == "2026-11-01");
  CHECK(et_date(1793593800000) == "2026-11-01");    // 2026-11-02T04:30Z: 23:30 EST (EDT would say the 2nd)
  CHECK(et_date(1793595600000) == "2026-11-02");
  CHECK(et_date(1772944200000) == "2026-03-07");    // 2026-03-08T04:30Z: 23:30 EST, before the change
  CHECK(et_date(1773028799000) == "2026-03-08");    // 2026-03-09T03:59:59Z: 23:59:59 EDT
  CHECK(et_date(1773030600000) == "2026-03-09");    // 2026-03-09T04:30Z: 00:30 EDT (EST would say the 8th)
  CHECK(et_date_after("2026-10-31") == "2026-11-01");
  CHECK(et_date_after("2026-12-31") == "2027-01-01");
  CHECK(et_date_after("2028-02-28") == "2028-02-29");
}

// Cross-site pages cannot take the stream slots: four streams with another site's headers are
// refused, and the page's own stream opens (review, 2026-10-06: they took all 4 and the page got
// 409).
static void test_cross_site_streams(Desk& k) {
  const int port = k.port();
  std::vector<std::unique_ptr<SseReader>> evil;
  for (int i = 0; i < 4; ++i)
    evil.push_back(std::make_unique<SseReader>(port, "evilslot" + std::to_string(i), "",
                                               httplib::Headers{{"Origin", "https://evil.example"}, {"Sec-Fetch-Site", "cross-site"}}));
  for (auto& e : evil) CHECK(eventually([&] { return e->done.load(); }, 3000ms));
  for (auto& e : evil) CHECK(e->status.load() == 403);
  SseReader mine(port, "pagestream1", "", httplib::Headers{{"Sec-Fetch-Site", "same-origin"}});
  CHECK(mine.wait_events(1, 3000ms));
  CHECK(mine.status.load() == 200);
}

// Start and Stop wait at most 50 ms for gw, then answer "queued"; the Stop is applied when gw
// turns again. Neither holds a request thread for the 1 s it used to.
static void test_control_waits() {
  Desk k;
  const int port = k.port();
  std::promise<void> entered, release;
  std::shared_future<void> rel = release.get_future().share();
  k.d->gw().post(Probe{[&entered, rel] {
    entered.set_value();
    rel.wait();
  }});
  entered.get_future().wait();
  const auto t0 = std::chrono::steady_clock::now();
  auto st = post(port, "/api/stop", "{}", k.d->token());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("Stop with gw frozen: %.1f ms\n", ms);
  CHECK(st && st->status == 500 && err_code(st) == "INTERNAL");
  CHECK(st && st->body.find("queued") != std::string::npos);
  CHECK(ms < 500);
  capture("POST /api/stop", st);
  auto sa = post(port, "/api/start", "{}", k.d->token());
  CHECK(sa && sa->status == 500 && sa->body.find("queued") != std::string::npos);
  capture("POST /api/start", sa);
  CHECK(k.d->gw().inbox().size(Lane::CONTROL) == 1);
  release.set_value();
  CHECK(eventually([&] { return k.d->gw().stats().erased_by_stop.load() == 1; }, 2000ms));   // the Stop took the Start
  CHECK(eventually([&] { return k.d->gw().run_view().why == "operator"; }, 2000ms));
}

// A KILL with send-cx's inbox full: it takes no slot, so it still reaches send-cx at once.
static void test_kill_with_cx_full() {
  Desk k;
  BoundedQueue<SendJob>& q = k.d->cx_queue();
  std::atomic<bool> stop{false};
  std::thread flood([&] {   // keeps the inbox full while send-cx drains it
    int i = 0;
    while (!stop.load()) {
      ++i;
      q.push(SendJob{SendJob::Kind::WRITE, now_ms(), "Mflood-" + std::to_string(i)}, (i & 1) ? Lane::PRIORITY : Lane::NORMAL);
      if ((i & 255) == 0) std::this_thread::yield();
    }
  });
  CHECK(eventually([&] { return q.dropped() > 0; }, 2000ms));
  auto kr = post(k.port(), "/api/kill", "{}", k.d->token());
  CHECK(kr && kr->status == 200);
  CHECK(eventually([&] { return k.d->cx_stats().kill_gen_seen.load() == k.d->kill_switch().generation(); }, 1000ms));
  CHECK(k.d->cx_stats().kills.load() == 1);
  stop.store(true);
  flood.join();
  std::printf("KILL with send-cx's inbox full: %llu jobs dropped meanwhile\n", static_cast<unsigned long long>(q.dropped()));
}

// Deskd::start ignores SIGPIPE for the whole process: a client that resets a stream mid-write
// cannot end whoever embeds deskd (this test's main sets nothing itself). httplib 0.18.3 sends
// with flags 0 and never sets SO_NOSIGPIPE.
static void test_client_reset_in_process(Desk& k) {
  struct sigaction cur {};
  CHECK(::sigaction(SIGPIPE, nullptr, &cur) == 0);
  CHECK(cur.sa_handler == SIG_IGN);
  const int port = k.port();
  const int fd = raw_connect(port);
  CHECK(fd >= 0);
  const std::string req = "GET /api/stream?sid=resetme01 HTTP/1.1\r\nHost: " + k.host() + "\r\n\r\n";
  CHECK(::write(fd, req.data(), req.size()) == static_cast<ssize_t>(req.size()));
  char b[512];
  CHECK(::read(fd, b, sizeof b) > 0);
  linger lg{1, 0};   // RST on close: deskd's next write into it fails at once
  ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  ::close(fd);
  std::this_thread::sleep_for(2500ms);   // two hbs written into the dead stream
  auto h = get(port, "/api/health");
  CHECK(h && h->status == 200);   // and this process is still here to ask
  CHECK(eventually([&] { return !k.d->pub().streams().stream_for("resetme01"); }, 3000ms));
}

// A stop ends each open stream with its closing chunk (svr.stop() used to cut them off: the
// browser logged ERR_INCOMPLETE_CHUNKED_ENCODING on every SIGTERM).
static void test_stop_ends_streams() {
  Desk k;
  SseReader a(k.port(), "stopend001"), b(k.port(), "stopend002");
  CHECK(a.wait_events(1, 3000ms) && b.wait_events(1, 3000ms));
  k.d->stop();
  CHECK(eventually([&] { return a.done.load() && b.done.load(); }, 3000ms));
  CHECK(a.complete.load() && b.complete.load());
}

// A second desk on a port already in use is refused, never let in beside the first (httplib's
// default SO_REUSEPORT bound it and split the page's connections between the two)
static void test_port_taken() {
  Desk k;
  WebDeps deps;
  deps.ui_dir = std::string(DESKD_SOURCE_DIR) + "/ui/desk";
  WebServer second(deps);
  std::string err;
  CHECK(second.bind(k.port(), &err) == 0);
  CHECK(err.find("in use") != std::string::npos);
  auto h = get(k.port(), "/api/health");
  CHECK(h && h->status == 200 && json::parse(h->body)["epoch"] == k.d->epoch());
}

// false when node is missing: the conformance check did not run, and ctest must say so (77,
// SKIP_RETURN_CODE in CMakeLists.txt) rather than report a pass
static bool test_contract() {
  if (std::string(DESKD_NODE).empty()) {
    std::printf("node not found: contract.js checks SKIPPED\n");
    return false;
  }
  TempDir d;
  const std::string file = d.file("capture.json");
  std::ofstream(file) << g_capture.dump();
  CHECK(g_capture["snapshots"].size() >= 2);
  CHECK(g_capture["events"].size() >= 8);
  CHECK(g_capture["responses"].size() >= 20);
  CHECK(g_capture["requests"].size() >= 40);
  CHECK(g_capture["queries"].size() >= 10);
  const std::string cmd = std::string("\"") + DESKD_NODE + "\" \"" + DESKD_SOURCE_DIR + "/tests/deskd/contract_check.cjs\" \"" + file + "\"";
  CHECK(std::system(cmd.c_str()) == 0);
  return true;
}

int main() {
  // No signal(SIGPIPE) here, unlike the other test mains: Deskd::start must do it for any
  // process that embeds deskd, and test_client_reset_in_process checks that it did.
  {
    Desk k;
    test_guards_and_page(k);
    test_stream(k);
    test_stream_limit(k);
    test_idle_connections(k);
    test_request_checker(k);
    test_cross_site_streams(k);
    test_client_reset_in_process(k);
  }
  test_stream_queue_gap();
  test_kill_with_gw_frozen();
  test_kill_with_cx_full();
  test_control_waits();
  test_stop_ends_streams();
  test_port_taken();
  const bool node_ran = test_contract();
  const int rc = finish("deskd-web");
  return rc == 0 && !node_ran ? 77 : rc;
}
