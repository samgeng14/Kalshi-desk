// deskd web: a whole deskd in this process on a free port, driven over HTTP.
//   - the page carries this start's token; Host and X-Desk guards on GET and POST
//   - SSE framing: "retry: 2000", then event/id/data with seq 1, 2, 3 ... (hb first, 1/s)
//   - the stream lifecycle: takeover by sid, at most 4 streams, snapshot and subscribe only
//     for an open sid, a full per-stream queue drops and leaves a visible gap
//   - every snapshot, event and reply checked by contract.js itself (node)
//   - 10 idle connections and a POST still answers at once (the old desk's 4.8 s stall)
//   - KILL answers and reaches send-cx while gw is frozen
#include <arpa/inet.h>
#include <httplib.h>
#include <netinet/in.h>
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
#include "deskd/web/server.h"
#include "deskd/web/sse.h"

using namespace deskd;
using json = nlohmann::json;
using namespace std::chrono_literals;

// everything served, for contract.js at the end
static json g_capture = {{"snapshots", json::array()}, {"events", json::array()}, {"responses", json::array()}};
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
    auto lock = AccountLock::acquire(dir.path, &err);
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
                            const std::string& host = "", const std::string& ctype = "application/json") {
  httplib::Headers h;
  if (!token.empty()) h.emplace("X-Desk", token);
  if (!host.empty()) h.emplace("Host", host);
  return client(port)->Post(path, h, body, ctype);
}
static httplib::Result get(int port, const std::string& path, const std::string& host = "") {
  httplib::Headers h;
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
  std::atomic<bool> stop{false}, done{false};
  std::atomic<int> status{0};
  SseReader(int port, const std::string& sid, const std::string& host = "") {
    th = std::thread([this, port, sid, host] {
      httplib::Client cli("127.0.0.1", port);
      cli.set_read_timeout(10, 0);
      httplib::Headers h;
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
  CHECK(o && o->status == 404 && err_code(o) == "UNKNOWN_MARKET");
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

static void test_contract() {
  if (std::string(DESKD_NODE).empty()) {
    std::printf("node not found: contract.js checks skipped\n");
    return;
  }
  TempDir d;
  const std::string file = d.file("capture.json");
  std::ofstream(file) << g_capture.dump();
  CHECK(g_capture["snapshots"].size() >= 2);
  CHECK(g_capture["events"].size() >= 8);
  CHECK(g_capture["responses"].size() >= 20);
  const std::string cmd = std::string("\"") + DESKD_NODE + "\" \"" + DESKD_SOURCE_DIR + "/tests/deskd/contract_check.cjs\" \"" + file + "\"";
  CHECK(std::system(cmd.c_str()) == 0);
}

int main() {
  {
    Desk k;
    test_guards_and_page(k);
    test_stream(k);
    test_stream_limit(k);
    test_idle_connections(k);
  }
  test_stream_queue_gap();
  test_kill_with_gw_frozen();
  test_contract();
  return finish("deskd-web");
}
