// deskd net/: the connection factory, one request per connection, and the spare pool, against the
// fake Kalshi's TLS listener (a CA made under $TMPDIR at run time; tests/fake_exchange/tls_pki.h)
// and small raw loopback servers.
//
//   - the guard on every connect: a non-live deskd pointed at production opens no socket at all,
//     by itself or through a pool (week-0 review); plain HTTP only to loopback
//   - verification is never off: an untrusted CA, a leaf for another name, an expired leaf, a
//     self-signed one are each refused, and the test CA is trusted for loopback only
//   - one request per connection: a reply, a drop before or after the venue took the order, a
//     body cut with and without close_notify, a stall past the deadline, a reset, a reply the
//     parser refuses; NOT_SENT only when nothing whole was written
//   - the venue's idle close (close_notify or a bare FIN) seen before a write, never after
//   - TLS 1.3 session tickets are not data; resumption works and can be turned off
//   - the pool: pre-opened, replaced at once after a take, retired at idle_replace_ms, dropped when
//     the venue closes it, misses counted, refusals backed off
//   - SO_NOSIGPIPE: a write into a reset connection with SIGPIPE's default action does not kill
//   - reads wait in slices of 250 ms or less
//   - loopback latency: handshake and request, TLS and plain
#include <signal.h>

#include <cstring>

#include "deskd/app/lock.h"
#include "deskd/net/pool.h"
#include "net_fixture.h"

using namespace deskd;
using namespace deskd::net;
using namespace fx;

namespace {

std::string get_status_bytes(const Endpoint& ep, bool keep_alive = false) {
  return request_bytes("GET", ep.host_header(), ep.base_path + "/exchange/status", {{"Accept", "application/json"}}, "",
                       keep_alive);
}

std::string create_bytes(const Endpoint& ep, const std::string& cid, const std::string& price = "0.3000") {
  const std::string body = "{\"ticker\":\"" + T + "\",\"side\":\"bid\",\"count\":\"1.00\",\"price\":\"" + price +
                           "\",\"time_in_force\":\"good_till_canceled\",\"post_only\":true,\"expiration_time\":" +
                           std::to_string(std::time(nullptr) + 60) +
                           ",\"self_trade_prevention_type\":\"maker\",\"exchange_index\":0,\"client_order_id\":\"" +
                           cid + "\"}";
  return request_bytes("POST", ep.host_header(), ep.base_path + "/portfolio/events/orders",
                       {{"KALSHI-ACCESS-KEY", "k"},
                        {"KALSHI-ACCESS-TIMESTAMP", "1"},
                        {"KALSHI-ACCESS-SIGNATURE", "c2ln"},
                        {"Content-Type", "application/json"}},
                       body, false);
}

bool log_has(const fakex::FakeKalshi& f, const std::string& needle) {
  for (const auto& l : f.wire_log())
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

static void test_endpoint() {
  std::string why;
  auto a = Endpoint::parse("https://demo-api.kalshi.co/trade-api/v2", &why);
  CHECK(a && a->tls && a->host == "demo-api.kalshi.co" && a->port == 443 && a->base_path == "/trade-api/v2");
  CHECK(a && a->host_header() == "demo-api.kalshi.co" && a->url() == "https://demo-api.kalshi.co/trade-api/v2");
  auto b = Endpoint::parse("HTTP://127.0.0.1:8080/x/", &why);
  CHECK(b && !b->tls && b->port == 8080 && b->base_path == "/x" && b->host_header() == "127.0.0.1:8080");
  auto c = Endpoint::parse("https://[::1]:9443", &why);
  CHECK(c && c->host == "::1" && c->port == 9443 && c->host_header() == "[::1]:9443" && c->base_path.empty());
  auto d = Endpoint::parse("https://Demo-API.Kalshi.co./trade-api/v2", &why);
  CHECK(d && d->host == "demo-api.kalshi.co");
  CHECK(!Endpoint::parse("ftp://x", &why));
  CHECK(!Endpoint::parse("https://user@demo-api.kalshi.co/", &why));
  CHECK(!Endpoint::parse("https://demo-api.kalshi.co/a?b=1", &why));
  CHECK(!Endpoint::parse("https://demo-api.kalshi.co:99999/", &why));
  CHECK(!Endpoint::parse("https://demo-api.kalshi.co:/", &why));
  CHECK(!Endpoint::parse("https:///path", &why));
}

// The week-0 review's test: a non-live deskd configured for production opens no connection.
static void test_guard_on_every_connect() {
  ConnFactory f(demo_guard(), trust_test_ca());
  CHECK(f.ok());
  CHECK(f.trust_roots() > 50);   // the system store loaded (187 roots in Homebrew's bundle on 2026-10-07)
  std::string why;
  for (const char* url : {"https://api.elections.kalshi.com/trade-api/v2", "https://external-api.kalshi.com/trade-api/v2",
                          "https://gateway.polymarket.us", "https://api.elections.kalshi.com:443/trade-api/v2"}) {
    auto c = f.open(*Endpoint::parse(url, nullptr), 1000, &why);
    CHECK(!c && why.find("--live") != std::string::npos);
  }
  CHECK(!f.open(*Endpoint::parse("https://kalshi.com.example.net", nullptr), 1000, &why) &&
        why.find("not a known venue host") != std::string::npos);
  // plain HTTP: loopback only, even to an allowed venue host
  CHECK(!f.open(*Endpoint::parse("http://demo-api.kalshi.co/trade-api/v2", nullptr), 1000, &why) &&
        why.find("only loopback") != std::string::npos);
  CHECK(f.stats().sockets == 0 && f.stats().refused == 6);

  // --live without the production account's lock is refused too
  TempDir d;
  std::string err;
  auto demo_lock = AccountLock::acquire(d.path, "demo", "test", false, &err);
  const SendGuard live_demo_lock(true, demo_lock.get());
  ConnFactory g(live_demo_lock, trust_test_ca());
  CHECK(!g.open(*Endpoint::parse("https://api.elections.kalshi.com/trade-api/v2", nullptr), 1000, &why) &&
        why.find("account's lock") != std::string::npos);
  CHECK(g.stats().sockets == 0);

  // a pool pointed at production backs off at the most and never makes a socket
  PoolConfig pc;
  pc.name = "prod";
  pc.endpoint = *Endpoint::parse("https://api.elections.kalshi.com/trade-api/v2", nullptr);
  pc.backoff_max_ms = 200;
  SparePool pool(f, pc);
  pool.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  bool miss = false;
  auto taken = pool.take(500, &miss, &why);
  pool.stop();
  const PoolStats ps = pool.stats();
  CHECK(!taken && miss && why.find("--live") != std::string::npos);
  CHECK(ps.refused >= 2 && ps.refused <= 4 && ps.handshakes == 0 && ps.miss_failed == 1);
  CHECK(f.stats().sockets == 0);
}

static void test_verification() {
  Venue v;
  CHECK(v.ok);
  std::string why;
  // trusted for loopback: TLS 1.3, verified, SNI-less for an address
  ConnFactory f(demo_guard(), trust_test_ca());
  auto c = f.open(v.ep(), 2000, &why);
  CHECK(c != nullptr);
  if (c) {
    CHECK(c->tls() && c->tls_version() == "TLSv1.3" && c->handshake_ms() > 0 && c->peer().find("127.0.0.1:") == 0);
  }
  // by name: SNI and the host name checked against DNS:localhost
  auto byname = f.open(v.ep("localhost"), 2000, &why);
  CHECK(byname != nullptr);
  // without the test CA the same server is refused: the system store does not know it
  ConnFactory plain(demo_guard());
  CHECK(!plain.open(v.ep(), 2000, &why) && why.find("certificate not trusted") != std::string::npos);
  CHECK(plain.stats().handshake_fail == 1 && plain.stats().handshakes == 0);

  // leaves the client must refuse, each behind its own fake
  struct Bad {
    const char* name;
    std::vector<std::string> sans;
    long nb, na;
    bool own_ca;
    const char* expect;
  };
  const Bad bads[] = {
      {"wrong-name", {"DNS:example.com"}, -60, 86400, true, "not trusted"},
      {"expired", {"IP:127.0.0.1", "DNS:localhost"}, -7200, -3600, true, "not trusted"},
      {"not-yet", {"IP:127.0.0.1", "DNS:localhost"}, 3600, 7200, true, "not trusted"},
      {"self-signed", {"IP:127.0.0.1", "DNS:localhost"}, -60, 86400, false, "not trusted"},
  };
  for (const Bad& b : bads) {
    std::string chain, key, err;
    const bool made = fakex::make_leaf(pki().dir.path, b.name, b.own_ca ? pki().p.ca_cert : "",
                                       b.own_ca ? pki().p.ca_key : "", b.sans, b.nb, b.na, &chain, &key, &err);
    CHECK(made);
    Venue bad({}, true, chain, key);
    CHECK(bad.ok);
    auto bc = f.open(bad.ep(), 2000, &why);
    CHECK(!bc && why.find(b.expect) != std::string::npos);
    if (bc) std::fprintf(stderr, "  %s was accepted\n", b.name);
    // "localhost" is not example.com either
    if (std::strcmp(b.name, "wrong-name") == 0) CHECK(!f.open(bad.ep("localhost"), 2000, &why));
  }
  // the test CA through the venue context (an override bundle) still checks the name
  FactoryOptions o;
  o.system_ca_file_override = pki().p.ca_cert;
  ConnFactory over(demo_guard(), o);
  CHECK(over.open(v.ep(), 2000, &why) != nullptr);
  std::string chain, key, err;
  fakex::make_leaf(pki().dir.path, "wrong-name-2", pki().p.ca_cert, pki().p.ca_key, {"DNS:other.test"}, -60, 86400,
                   &chain, &key, &err);
  Venue other({}, true, chain, key);
  CHECK(!over.open(other.ep(), 2000, &why));
}

static void test_one_request_per_connection() {
  Venue v;
  ConnFactory f(demo_guard(), trust_test_ca());
  std::string why;
  // a read
  auto c = f.open(v.ep(), 2000, &why);
  Exchange x = exchange(*c, get_status_bytes(v.ep(), true), deadline_ms(2000));
  CHECK(x.fate == Exchange::Fate::REPLIED && x.res.status == 200 && x.res.body.find("exchange_active") != std::string::npos);
  CHECK(x.reusable && x.t_written_us > 0 && x.t_first_byte_us >= x.t_written_us && x.t_done_us >= x.t_first_byte_us);
  // a write: Connection: close, so the venue closes it after the reply and nobody can reuse it
  auto w = f.open(v.ep(), 2000, &why);
  Exchange cx = exchange(*w, create_bytes(v.ep(), "M-net-1"), deadline_ms(2000));
  CHECK(cx.fate == Exchange::Fate::REPLIED && cx.res.status == 201 && !cx.reusable);
  CHECK(cx.res.body.find("\"order_id\"") != std::string::npos && v.has_cid("M-net-1"));
  // the venue closed it: a second request on it is refused before a byte goes
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Exchange again = exchange(*w, create_bytes(v.ep(), "M-net-2"), deadline_ms(2000));
  CHECK(again.fate == Exchange::Fate::NOT_SENT && again.why.find("closed before the write") != std::string::npos);
  CHECK(!v.has_cid("M-net-2"));
}

static void test_faults_over_tls() {
  using K = fakex::HttpFault::Kind;
  for (bool notify : {true, false}) {
    fakex::FakeConfig cfg;
    cfg.tls_close_notify = notify;
    Venue v(cfg);
    ConnFactory f(demo_guard(), trust_test_ca());
    std::string why;
    auto fault = [&](K k, int stall = 0) {
      fakex::HttpFault h;
      h.kind = k;
      h.stall_ms = stall;
      v.f->add_fault(h);
    };
    auto send = [&](const std::string& cid, int deadline = 2000, const std::function<void()>& idle = nullptr) {
      auto c = f.open(v.ep(), 2000, &why);
      return c ? exchange(*c, create_bytes(v.ep(), cid), deadline_ms(deadline), false, idle) : Exchange{};
    };
    // dropped before the venue read it, and after it took the order: both UNKNOWN, no byte back
    fault(K::DROP_BEFORE_REPLY);
    Exchange a = send("M-drop-before");
    CHECK(a.fate == Exchange::Fate::UNKNOWN && a.cut == ResponseParser::Cut::NO_BYTES && !v.has_cid("M-drop-before"));
    fault(K::DROP_AFTER_ACCEPT);
    Exchange b = send("M-drop-after");
    CHECK(b.fate == Exchange::Fate::UNKNOWN && b.cut == ResponseParser::Cut::NO_BYTES && v.has_cid("M-drop-after"));
    CHECK(b.why.find(notify ? "close_notify" : "EOF without close_notify") != std::string::npos);
    // the body cut mid-way: UNKNOWN, never failed, whether or not close_notify came
    fault(K::CUT_BODY);
    Exchange cut = send("M-cut");
    CHECK(cut.fate == Exchange::Fate::UNKNOWN && cut.cut == ResponseParser::Cut::IN_BODY && v.has_cid("M-cut"));
    CHECK(cut.why.find("body cut at") != std::string::npos);
    CHECK(cut.why.find(notify ? "(close_notify)" : "EOF without close_notify") != std::string::npos);
    // a 5xx after the venue took it is a whole reply here; gw/kalshi_wire calls it UNKNOWN
    fault(K::STATUS_AFTER_ACCEPT);
    Exchange s5 = send("M-500");
    CHECK(s5.fate == Exchange::Fate::REPLIED && s5.res.status == 500 && v.has_cid("M-500"));
    // chunked, the same reply
    fault(K::CHUNKED);
    Exchange ch = send("M-chunked");
    CHECK(ch.fate == Exchange::Fate::REPLIED && ch.res.status == 201 && ch.res.chunked &&
          ch.res.body.find("M-chunked") != std::string::npos);
    // a stall past the deadline: UNKNOWN, and the waiting came back at least every 250 ms
    fault(K::STALL_AFTER, 1200);
    std::vector<std::int64_t> idles;
    const std::int64_t t0 = mono_us();
    Exchange st = send("M-stall", 700, [&] { idles.push_back(mono_us()); });
    CHECK(st.fate == Exchange::Fate::UNKNOWN && st.why.find("no whole reply") != std::string::npos && v.has_cid("M-stall"));
    CHECK(idles.size() >= 2);
    std::int64_t prev = t0, gap = 0;
    for (auto t : idles) {
      gap = std::max(gap, t - prev);
      prev = t;
    }
    CHECK(gap <= 300 * 1000);
    // 429: a reply; nothing processed
    fault(K::RATE_LIMIT);
    Exchange rl = send("M-429");
    CHECK(rl.fate == Exchange::Fate::REPLIED && rl.res.status == 429 && !v.has_cid("M-429"));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));   // the stalled reply drains
  }
}

static void test_raw_replies() {
  // a reply the parser refuses after the write: UNKNOWN
  {
    RawServer s;
    s.reply = [](int) { return std::string("HTTP/1.1 200 OK\nContent-Length: 2\n\nok"); };
    s.start();
    ConnFactory f(demo_guard());
    std::string why;
    auto c = f.open(s.ep(), 1000, &why);
    Exchange x = exchange(*c, request_bytes("POST", "127.0.0.1", "/o", {}, "{}", false), deadline_ms(1000));
    CHECK(x.fate == Exchange::Fate::UNKNOWN && x.why.find("refuses") != std::string::npos);
  }
  // a reset after the request: UNKNOWN
  {
    RawServer s;
    s.reset = true;
    s.start();
    ConnFactory f(demo_guard());
    std::string why;
    auto c = f.open(s.ep(), 1000, &why);
    Exchange x = exchange(*c, request_bytes("POST", "127.0.0.1", "/o", {}, "{}", false), deadline_ms(1000));
    CHECK(x.fate == Exchange::Fate::UNKNOWN && x.cut == ResponseParser::Cut::NO_BYTES);
  }
  // a server that never reads: the write cannot complete, so nothing whole reached it: NOT_SENT
  {
    RawServer s;   // listening, never accepting: the kernel's buffers fill and the write stalls
    ConnFactory f(demo_guard());
    std::string why;
    auto c = f.open(s.ep(), 1000, &why);
    CHECK(c != nullptr);
    const std::string big(16 * 1024 * 1024, 'x');
    int idles = 0;
    Exchange x = exchange(*c, request_bytes("POST", "127.0.0.1", "/o", {}, big, false), deadline_ms(400), false,
                          [&] { ++idles; });
    CHECK(x.fate == Exchange::Fate::NOT_SENT && x.why.find("timed out") != std::string::npos && idles >= 1);
  }
  // a close-framed body read to the close
  {
    RawServer s;
    s.reply = [](int) { return std::string("HTTP/1.1 200 OK\r\n\r\nto the end"); };
    s.start();
    ConnFactory f(demo_guard());
    std::string why;
    auto c = f.open(s.ep(), 1000, &why);
    Exchange x = exchange(*c, request_bytes("GET", "127.0.0.1", "/", {}, "", true), deadline_ms(1000));
    CHECK(x.fate == Exchange::Fate::REPLIED && x.res.body == "to the end" && !x.reusable);
  }
}

// The venue's idle close, seen before a write: close_notify, or a bare FIN.
static void test_idle_close() {
  for (bool notify : {true, false}) {
    fakex::FakeConfig cfg;
    cfg.idle_close_ms = 150;
    cfg.tls_close_notify = notify;
    Venue v(cfg);
    ConnFactory f(demo_guard(), trust_test_ca());
    std::string why;
    auto c = f.open(v.ep(), 2000, &why);
    CHECK(c && c->usable(&why));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(log_has(*v.f, "http idle-close") && v.f->idle_closes() == 1);
    CHECK(!c->usable(&why));
    CHECK(why.find(notify ? "CLOSED close_notify" : "CLOSED_UNCLEAN EOF without close_notify") != std::string::npos);
    const auto before = v.f->http_requests();
    Exchange x = exchange(*c, create_bytes(v.ep(), "M-idle"), deadline_ms(1000));
    CHECK(x.fate == Exchange::Fate::NOT_SENT && !v.has_cid("M-idle") && v.f->http_requests() == before);
  }
  // the venue dropping every kept-open connection at once (a network blip)
  Venue v;
  ConnFactory f(demo_guard(), trust_test_ca());
  std::string why;
  auto c = f.open(v.ep(), 2000, &why);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  v.f->drop_http_connections();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(!c->usable(&why));
}

static void test_tickets_and_resumption() {
  Venue v;
  std::string why;
  ConnFactory f(demo_guard(), trust_test_ca());
  auto first = f.open(v.ep(), 2000, &why);
  CHECK(first && !first->resumed());
  // TLS 1.3 tickets arrive after the handshake: readable, but not data
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(first->usable(&why));
  auto second = f.open(v.ep(), 2000, &why);
  CHECK(second && second->resumed());
  // the server finishes its side a moment after the client (it reads the client's Finished)
  for (int i = 0; i < 100 && v.f->tls_resumed() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(f.stats().resumed >= 1 && v.f->tls_resumed() >= 1);
  Exchange x = exchange(*second, get_status_bytes(v.ep()), deadline_ms(2000));
  CHECK(x.fate == Exchange::Fate::REPLIED && x.res.status == 200);
  FactoryOptions o = trust_test_ca();
  o.resume_sessions = false;
  ConnFactory nf(demo_guard(), o);
  auto n1 = nf.open(v.ep(), 2000, &why);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto n2 = nf.open(v.ep(), 2000, &why);
  CHECK(n1 && n2 && !n1->resumed() && !n2->resumed() && nf.stats().resumed == 0);
}

static void test_pool() {
  Venue v;
  ConnFactory f(demo_guard(), trust_test_ca());
  PoolConfig pc;
  pc.name = "k1";
  pc.endpoint = v.ep();
  pc.spares = 2;
  {
    SparePool p(f, pc);
    p.start();
    CHECK(p.wait_ready(2, 3000));
    CHECK(p.stats().handshakes == 2 && p.stats().opened_background == 2);
    // a take is a hit, and its replacement opens at once
    bool miss = true;
    std::string why;
    auto c = p.take(1000, &miss, &why);
    CHECK(c && !miss);
    CHECK(p.wait_ready(2, 3000) && p.stats().handshakes == 3);
    Exchange x = exchange(*c, create_bytes(v.ep(), "M-pool-1"), deadline_ms(2000));
    CHECK(x.fate == Exchange::Fate::REPLIED && x.res.status == 201);
    // 20 clicks 30 ms apart: every one a hit, every connection used once
    const auto accepted0 = v.f->connections_accepted();
    int fails = 0, misses = 0;
    for (int i = 0; i < 20; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      bool m = false;
      auto k = p.take(1000, &m, &why);
      if (m) ++misses;
      if (!k) {
        ++fails;
        continue;
      }
      Exchange e = exchange(*k, create_bytes(v.ep(), "M-pool-s" + std::to_string(i)), deadline_ms(2000));
      if (e.fate != Exchange::Fate::REPLIED || e.res.status != 201) ++fails;
    }
    CHECK(fails == 0 && misses == 0);
    // a burst of 8 back to back drains the two spares: the rest are misses, counted, and still sent
    int burst_fail = 0;
    for (int i = 0; i < 8; ++i) {
      auto k = p.take(1000, nullptr, &why);
      Exchange e = k ? exchange(*k, create_bytes(v.ep(), "M-pool-b" + std::to_string(i)), deadline_ms(2000)) : Exchange{};
      if (e.fate != Exchange::Fate::REPLIED) ++burst_fail;
    }
    const PoolStats s = p.stats();
    CHECK(burst_fail == 0 && s.misses >= 1 && s.misses <= 7 && s.opened_inline == s.misses);
    CHECK(s.takes == 29 && s.hits + s.misses == s.takes);
    CHECK(p.wait_ready(2, 3000));
    // one connection per request: what the venue accepted equals what the pool opened
    CHECK(v.f->connections_accepted() - accepted0 + 3 == p.stats().handshakes);
    CHECK(s.handshake_ms.size() == s.handshakes);
    p.stop();
    CHECK(p.ready() == 0);
  }
  // retired at idle_replace_ms, the replacement opened lead_ms before: never empty, the venue
  // never closes one first
  {
    PoolConfig q = pc;
    q.spares = 1;
    q.idle_replace_ms = 300;
    q.lead_ms = 100;
    SparePool p(f, q);
    p.start();
    CHECK(p.wait_ready(1, 2000));
    int empty = 0;
    for (int i = 0; i < 100; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (p.ready() == 0) ++empty;
    }
    const PoolStats s = p.stats();
    CHECK(s.idle_retired >= 2 && s.closed_by_venue == 0 && empty == 0);
    CHECK(s.handshakes >= 3 && s.handshakes <= 8);
  }
  // the venue closes ready ones first (its idle close shorter than ours): each found within
  // check_ms and replaced
  {
    fakex::FakeConfig cfg;
    cfg.idle_close_ms = 150;
    Venue w(cfg);
    PoolConfig q = pc;
    q.endpoint = w.ep();
    q.spares = 1;
    q.check_ms = 50;
    SparePool p(f, q);
    p.start();
    CHECK(p.wait_ready(1, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(p.stats().closed_by_venue >= 2 && w.f->idle_closes() >= 2);
  }
  // the venue drops every kept-open connection at once: found, replaced, and the next take is a
  // hit on a live one
  {
    Venue w;
    PoolConfig q = pc;
    q.endpoint = w.ep();
    q.spares = 2;
    q.check_ms = 50;
    SparePool p(f, q);
    p.start();
    CHECK(p.wait_ready(2, 2000));
    w.f->drop_http_connections();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(p.stats().closed_by_venue == 2);
    if (p.stats().closed_by_venue != 2)
      std::fprintf(stderr, "  closed_by_venue %llu, stale %llu, ready %zu, last %s\n",
                   static_cast<unsigned long long>(p.stats().closed_by_venue),
                   static_cast<unsigned long long>(p.stats().stale_on_take), p.stats().ready, p.stats().last_error.c_str());
    CHECK(p.wait_ready(2, 2000));
    bool miss = true;
    std::string why;
    auto c = p.take(1000, &miss, &why);
    Exchange x = c ? exchange(*c, get_status_bytes(w.ep()), deadline_ms(2000)) : Exchange{};
    CHECK(x.fate == Exchange::Fate::REPLIED && !miss && p.stats().stale_on_take == 0);
  }
  // with no periodic look the take finds the venue's close itself, drops it, and opens one inline
  {
    fakex::FakeConfig cfg;
    cfg.idle_close_ms = 150;
    Venue w(cfg);
    PoolConfig q = pc;
    q.endpoint = w.ep();
    q.spares = 1;
    q.check_ms = 60000;
    SparePool p(f, q);
    p.start();
    CHECK(p.wait_ready(1, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    bool miss = false;
    std::string why;
    auto c = p.take(1000, &miss, &why);
    Exchange x = c ? exchange(*c, get_status_bytes(w.ep()), deadline_ms(2000)) : Exchange{};
    CHECK(x.fate == Exchange::Fate::REPLIED && miss && p.stats().stale_on_take == 1);
  }
  // a venue that is down: failures back off, take() reports it
  {
    RawServer gone;   // bound, then closed: nothing listens on the port
    const Endpoint dead = gone.ep();
    ::close(gone.fd);
    gone.fd = -1;
    PoolConfig q = pc;
    q.endpoint = dead;
    q.spares = 1;
    q.backoff_min_ms = 50;
    q.backoff_max_ms = 400;
    SparePool p(f, q);
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const PoolStats s = p.stats();
    CHECK(s.open_failures >= 3 && s.open_failures <= 7 && s.handshakes == 0);   // 50, 100, 200, 400 ms apart
    std::string why;
    CHECK(!p.take(300, nullptr, &why) && !why.empty());
  }
}

// The old desk died of SIGPIPE on a TLS reconnect: with SIGPIPE's default action back, a write
// into a connection the peer reset must fail, not kill.
static void test_no_sigpipe() {
#ifdef SO_NOSIGPIPE
  ::signal(SIGPIPE, SIG_DFL);
#else
  // Linux has no per-socket SO_NOSIGPIPE; deskd relies on runtime's process-wide SIG_IGN there,
  // so only the write-fails-cleanly half of this test applies.
  ::signal(SIGPIPE, SIG_IGN);
#endif
  {
    RawServer s;
    s.read_request = false;
    s.reset = true;
    s.start();
    ConnFactory f(demo_guard());
    std::string why;
    auto c = f.open(s.ep(), 1000, &why);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string data(256 * 1024, 'x');
    IoResult r1 = c->write_all(data.data(), data.size(), deadline_ms(500));
    IoResult r2 = c->write_all(data.data(), data.size(), deadline_ms(500));
    CHECK(r1.status != IoStatus::OK || r2.status != IoStatus::OK);
  }
  {
    fakex::FakeConfig cfg;
    cfg.idle_close_ms = 100;
    cfg.tls_close_notify = false;
    Venue v(cfg);
    ConnFactory f(demo_guard(), trust_test_ca());
    std::string why;
    auto c = f.open(v.ep(), 2000, &why);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::string data(256 * 1024, 'x');
    IoResult r1 = c->write_all(data.data(), data.size(), deadline_ms(500));
    IoResult r2 = c->write_all(data.data(), data.size(), deadline_ms(500));
    CHECK(r1.status != IoStatus::OK || r2.status != IoStatus::OK);
    c->close(true);   // close_notify into a dead socket: still no signal
  }
  ::signal(SIGPIPE, SIG_IGN);
  CHECK(true);   // reaching here is the test
}

static void test_loopback_latency() {
  Venue v;
  ConnFactory f(demo_guard(), trust_test_ca());
  std::string why;
  std::vector<double> hs, req, plain_req;
  for (int i = 0; i < 100; ++i) {
    auto c = f.open(v.ep(), 2000, &why);
    if (!c) continue;
    hs.push_back(c->connect_ms() + c->handshake_ms());
    Exchange x = exchange(*c, get_status_bytes(v.ep()), deadline_ms(2000));
    if (x.fate == Exchange::Fate::REPLIED) req.push_back(x.write_to_done_ms());
  }
  Venue pv({}, false);
  for (int i = 0; i < 100; ++i) {
    auto c = f.open(pv.ep(), 2000, &why);
    if (!c) continue;
    Exchange x = exchange(*c, get_status_bytes(pv.ep()), deadline_ms(2000));
    if (x.fate == Exchange::Fate::REPLIED) plain_req.push_back(x.write_to_done_ms());
  }
  CHECK(hs.size() == 100 && req.size() == 100 && plain_req.size() == 100);
  std::printf("loopback: TLS connect+handshake p50 %.2f p90 %.2f ms (resumed %llu of 100); request p50 %.3f p90 %.3f ms;"
              " plain request p50 %.3f p90 %.3f ms\n",
              pct(hs, 0.5), pct(hs, 0.9), static_cast<unsigned long long>(f.stats().resumed), pct(req, 0.5),
              pct(req, 0.9), pct(plain_req, 0.5), pct(plain_req, 0.9));
  CHECK(pct(req, 0.9) < 50);   // loopback; a generous bound for a loaded machine
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  CHECK(pki().ok);
  test_endpoint();
  test_guard_on_every_connect();
  test_verification();
  test_one_request_per_connection();
  test_faults_over_tls();
  test_raw_replies();
  test_idle_close();
  test_tickets_and_resumption();
  test_pool();
  test_no_sigpipe();
  test_loopback_latency();
  return finish("deskd-net");
}
