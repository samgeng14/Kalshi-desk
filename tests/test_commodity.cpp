// The 15-minute commodity pipeline (session 16): one terminal distribution, every contract priced
// off it; the payoff rules as the exchange writes them (docs/commodity-settlement.md); volatility
// from the reference feed's own observations; and the WebSocket client that carries the feed.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>

#include "desk/mathutil.h"
#include "desk/models/terminal_price.h"
#include <optional>

#include "desk/config.h"
#include "desk/engine.h"
#include "desk/executor.h"
#include "desk/log.h"
#include "desk/probability.h"
#include "desk/reference_feed.h"
#include "desk/strategies.h"
#include "desk/systematic.h"
#include "desk/timeutil.h"
#include "desk/ws_client.h"
#include "desk/types.h"

using namespace desk;

static int fails = 0, checks = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++checks;                                                                       \
    if (!(cond)) {                                                                  \
      ++fails;                                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                               \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static Market market(const std::string& strike_type, double floor_strike, double cap_strike = 0) {
  Market m;
  m.ticker = "KXGOLD15M-26SEP111915-15";
  m.event_ticker = "KXGOLD15M-26SEP111915";
  m.series_ticker = "KXGOLD15M";
  m.status = "active";
  m.strike_type = strike_type;
  if (floor_strike > 0) m.floor_strike = floor_strike;
  if (cap_strike > 0) m.cap_strike = cap_strike;
  return m;
}

// ---- 1, 2, 3, 4, 5: the payoff and the probability of it ---------------------------------------
static void test_payoff_parsing() {
  // The real thing: KXGOLD15M is greater_or_equal against the window's opening reference.
  auto p = terminal_payoff(market("greater_or_equal", 4351.61));
  CHECK(p && p->type == TerminalPayoff::Above && p->lower_inclusive);
  CHECK_NEAR(p->lower, 4351.61, 1e-9);
  CHECK(p->pays(4351.61) && p->pays(4351.62) && !p->pays(4351.60));   // the tie pays YES
  // Its exclusive sibling, which a Kalshi ladder uses on other series.
  auto g = terminal_payoff(market("greater", 96.37));
  CHECK(g && g->type == TerminalPayoff::Above && !g->lower_inclusive);
  CHECK(!g->pays(96.37) && g->pays(96.371));
  auto l = terminal_payoff(market("less", 0, 2.94));
  CHECK(l && l->type == TerminalPayoff::Below && !l->upper_inclusive);
  CHECK(l->pays(2.93) && !l->pays(2.94));
  auto le = terminal_payoff(market("less_or_equal", 0, 2.94));
  CHECK(le && le->upper_inclusive && le->pays(2.94));
  auto b = terminal_payoff(market("between", 4340, 4360));
  CHECK(b && b->type == TerminalPayoff::Range && b->lower_inclusive && b->upper_inclusive);
  CHECK(b->pays(4340) && b->pays(4360) && b->pays(4350) && !b->pays(4339.99));
  // Not a terminal-price question, or not enough to make one.
  CHECK(!terminal_payoff(market("structured", 1)));
  CHECK(!terminal_payoff(market("greater", 0)));            // no strike
  CHECK(!terminal_payoff(market("between", 4360, 4340)));   // inverted
  CHECK(!terminal_payoff(market("", 4351.61)));
  // No ticker text is parsed anywhere: two different series with the same strike type agree.
  Market wti = market("greater_or_equal", 96.37);
  wti.ticker = "KXWTI15M-26SEP111915-15";
  wti.series_ticker = "KXWTI15M";
  auto w = terminal_payoff(wti);
  CHECK(w && w->type == TerminalPayoff::Above && w->lower_inclusive);
}

static void test_threshold_and_range_math() {
  TerminalDistribution d;
  d.spot = 100;
  d.sigma = 0.0002;          // per sqrt(second)
  d.tau_seconds = 900;       // a fifteen-minute window
  const double sd = 0.0002 * std::sqrt(900.0);
  CHECK_NEAR(d.sd(), sd, 1e-12);
  // At the money, with no drift, it is a coin flip.
  CHECK_NEAR(d.p_above(100, true), 0.5, 1e-9);
  CHECK_NEAR(d.p_below(100, false), 0.5, 1e-9);
  // One sigma above the strike.
  const double k = 100 * std::exp(-sd);
  CHECK_NEAR(d.z(k), 1.0, 1e-9);
  CHECK_NEAR(d.p_above(k, true), norm_cdf(1.0), 1e-9);
  // YES and NO are complements of the same distribution (test 3).
  for (double strike : {95.0, 99.5, 100.0, 100.5, 105.0}) {
    CHECK_NEAR(d.p_above(strike, true) + d.p_below(strike, false), 1.0, 1e-12);
    CHECK_NEAR(d.p_above(strike, false) + d.p_below(strike, true), 1.0, 1e-12);
  }
  // A range is the difference of two thresholds, and it is symmetric around the spot.
  const double lo = 100 * std::exp(-sd), hi = 100 * std::exp(sd);
  CHECK_NEAR(d.p_between(lo, hi), norm_cdf(1.0) - norm_cdf(-1.0), 1e-9);
  CHECK_NEAR(d.p_between(lo, hi), d.p_above(lo, true) - d.p_above(hi, true), 1e-12);
  CHECK_NEAR(d.p_between(0.0001, 1e9), 1.0, 1e-9);
  // Exhaustive ranges over one distribution sum to one (test 11).
  const std::vector<double> edges = {90, 98, 99.5, 100.5, 101, 110};
  double total = d.p_below(edges.front(), false);
  for (size_t i = 1; i < edges.size(); ++i) total += d.p_between(edges[i - 1], edges[i], false, true);
  total += d.p_above(edges.back(), false);
  CHECK_NEAR(total, 1.0, 1e-9);
  // Every contract on the underlying comes from the one distribution: the model cannot say two
  // things about the same strike (test 10).
  auto above = terminal_payoff("greater_or_equal", 100.5, std::nullopt);
  auto below = terminal_payoff("less", std::nullopt, 100.5);
  CHECK(above && below);
  CHECK_NEAR(d.probability(*above) + d.probability(*below), 1.0, 1e-12);
  auto range = terminal_payoff("between", 99.5, 100.5);
  auto under = terminal_payoff("less", std::nullopt, 99.5);
  CHECK(range && under);
  CHECK_NEAR(d.probability(*under) + d.probability(*range) + d.probability(*above), 1.0, 1e-9);
}

static void test_degenerate_and_tie() {
  // The last instant: no time left, so the answer is the indicator and the tie rule decides it.
  TerminalDistribution d;
  d.spot = 4351.61;
  d.sigma = 0.0002;
  d.tau_seconds = 0;
  CHECK_NEAR(d.p_above(4351.61, true), 1.0, 1e-12);    // >= pays
  CHECK_NEAR(d.p_above(4351.61, false), 0.0, 1e-12);   // > does not
  CHECK_NEAR(d.p_below(4351.61, true), 1.0, 1e-12);
  CHECK_NEAR(d.p_between(4351.61, 4360, true, true), 1.0, 1e-12);
  CHECK_NEAR(d.p_between(4351.61, 4360, false, true), 0.0, 1e-12);
  // Nothing to price on.
  TerminalDistribution bad;
  CHECK(!bad.usable());
  CHECK_NEAR(bad.p_above(100, true), 0.0, 1e-12);
  TerminalDistribution nospot = d;
  nospot.spot = 0;
  CHECK(!nospot.usable());
}

// ---- volatility and the settlement candle ------------------------------------------------------
static std::vector<std::pair<long long, double>> walk(long long from, long long to, int step,
                                                      double start, double per_step_sd, unsigned seed) {
  std::vector<std::pair<long long, double>> out;
  unsigned state = seed;
  double p = start;
  for (long long t = from; t <= to; t += step) {
    state = state * 1664525u + 1013904223u;
    const double u1 = ((state >> 8) & 0xffffff) / 16777216.0;
    state = state * 1664525u + 1013904223u;
    const double u2 = ((state >> 8) & 0xffffff) / 16777216.0;
    const double z = std::sqrt(-2 * std::log(u1 + 1e-12)) * std::cos(2 * 3.14159265358979 * u2);
    p *= std::exp(per_step_sd * z);
    out.emplace_back(t, p);
  }
  return out;
}

static void test_volatility_estimate() {
  const long long now = 1789160000;
  // A one-second walk with a known per-step standard deviation: the estimate should find it.
  const double per_step = 0.00005;   // 5 bp a second
  auto ticks = walk(now - 3600, now, 1, 4350.0, per_step, 7);
  VolSettings s;
  VolEstimate v = estimate_volatility(ticks, now, s);
  CHECK(v.warm && v.problems.empty());
  CHECK_NEAR(v.sigma, per_step, 0.4 * per_step);          // within 40% of the truth
  CHECK(v.rv_1m > 0 && v.rv_5m > 0 && v.rv_15m > 0 && v.rv_60m > 0);
  CHECK(v.samples > 600 && v.span_seconds >= 3500);
  CHECK(v.annualized() > 0);
  CHECK(v.to_json().value("sigma_per_sqrt_second", 0.0) > 0);
  // A burst in the last minutes lifts the estimate: the floor is the recent window, not the hour.
  auto burst = ticks;
  auto tail = walk(now - 240, now, 1, ticks.back().second, per_step * 6, 11);
  burst.erase(std::remove_if(burst.begin(), burst.end(), [&](const auto& t) { return t.first > now - 240; }),
              burst.end());
  burst.insert(burst.end(), tail.begin(), tail.end());
  VolEstimate hot = estimate_volatility(burst, now, s);
  CHECK(hot.sigma > v.sigma * 1.5);
  CHECK(hot.sigma <= s.max_ratio_to_hour * hot.rv_60m + 1e-12);   // and it cannot run away
  // Warm-up: two minutes of history is not enough to stand behind a number.
  auto young = walk(now - 120, now, 1, 4350.0, per_step, 3);
  VolEstimate cold = estimate_volatility(young, now, s);
  CHECK(!cold.warm && !cold.problems.empty());
  VolEstimate none = estimate_volatility({}, now, s);
  CHECK(!none.warm && none.sigma == 0);
  // A silent feed is not a calm market. Two observations an hour apart used to fill a 5-second
  // grid with 720 forward-filled points, all of which counted as samples and spanned an hour, so
  // the estimator called itself warm on two ticks and read the whole hour's move as if it had
  // happened in five seconds. Both halves of that have to fail.
  {
    std::vector<std::pair<long long, double>> sparse{{now - 3600, 4350.0}, {now, 4360.0}};
    VolEstimate q = estimate_volatility(sparse, now, s);
    CHECK(!q.warm);
    CHECK(q.samples < s.min_samples);
    bool said_quiet = false;
    for (const auto& why : q.problems) said_quiet = said_quiet || why.find("carried observations we can difference") != std::string::npos;
    CHECK(said_quiet);
  }
  // A gap in the middle: a quiet hour, twenty minutes of silence, then the price reappears half a
  // percent away. That move took twenty minutes, and the estimator must not price it as if it took
  // five seconds - which is the difference between a plausible sigma and one four times too big.
  {
    auto quiet = walk(now - 3600, now - 1500, 1, 4350.0, per_step, 5);
    const double before = quiet.back().second;
    auto after = walk(now - 300, now, 1, before * 1.005, per_step, 9);
    quiet.insert(quiet.end(), after.begin(), after.end());
    VolEstimate gapped = estimate_volatility(quiet, now, s);
    // The jump across the silence is excluded outright, so what is left is the ordinary walk.
    CHECK(gapped.sigma < v.sigma * 2.5);
    CHECK(gapped.rv_5m > 0);
  }
  // Nothing after `now` is read: the replay depends on it.
  auto future = ticks;
  for (long long t = now + 1; t <= now + 600; ++t) future.emplace_back(t, 9999.0);
  VolEstimate blind = estimate_volatility(future, now, s);
  CHECK_NEAR(blind.sigma, v.sigma, 1e-12);
  CHECK(blind.last_ts == v.last_ts);
}

static void test_minute_close() {
  // "the close price for the 1-minute candlestick at a given time is the price at the end of the
  // immediately preceding one-minute interval" (Kalshi's rules_secondary, verbatim).
  const long long minute_end = 1789160400;   // a minute boundary
  std::vector<std::pair<long long, double>> ticks;
  ticks.emplace_back(minute_end - 90, 4300.0);   // the minute before last
  ticks.emplace_back(minute_end - 59, 4350.0);
  ticks.emplace_back(minute_end - 2, 4352.14);   // the last tick inside the minute: the close
  ticks.emplace_back(minute_end + 1, 4360.0);    // after the boundary: not this candle
  auto c = minute_close(ticks, minute_end);
  CHECK(c && std::fabs(*c - 4352.14) < 1e-9);
  // The minute before it closes on its own last tick.
  auto prev = minute_close(ticks, minute_end - 60);
  CHECK(prev && std::fabs(*prev - 4300.0) < 1e-9);
  // A minute with no observation has no close of its own; the caller decides what to do.
  CHECK(!minute_close(ticks, minute_end - 180));
  CHECK(!minute_close({}, minute_end));
}

// ---- the WebSocket client ----------------------------------------------------------------------
// A mock server on a loopback port: handshake, frames, ping, and a close that forces a reconnect.
// Nothing in this test reaches a network, and the client it exercises is the one that carries the
// live reference feed.
struct MockWs {
  int listen_fd = -1;
  int port = 0;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<int> connections{0}, subscriptions{0}, pongs{0};
  std::vector<std::string> to_send;          // text frames sent right after the handshake
  std::atomic<bool> send_ping{false};        // ping the client once per connection
  std::atomic<bool> close_after_send{false}; // drop the socket after the frames: tests reconnect
  std::atomic<bool> close_immediately{false}; // hang up the instant the handshake is done
  std::atomic<bool> silent{false};           // accept and say nothing: tests stale detection
  std::mutex mu;
  std::vector<std::string> received;         // text frames the client sent us

  MockWs() {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    ::listen(listen_fd, 4);
    thread = std::thread([this] { serve(); });
  }
  ~MockWs() {
    stop = true;
    ::shutdown(listen_fd, SHUT_RDWR);
    ::close(listen_fd);
    if (thread.joinable()) thread.join();
  }
  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port) + "/trade-api/ws/v2"; }

  void serve() {
    while (!stop.load()) {
      fd_set r;
      FD_ZERO(&r);
      FD_SET(listen_fd, &r);
      timeval tv{0, 100 * 1000};
      if (::select(listen_fd + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
      int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) continue;
      ++connections;
      std::string head;
      char buf[4096];
      while (head.find("\r\n\r\n") == std::string::npos) {
        const int n = static_cast<int>(::recv(fd, buf, sizeof buf, 0));
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
      }
      const auto kp = head.find("Sec-WebSocket-Key: ");
      if (kp == std::string::npos) { ::close(fd); continue; }
      const auto ke = head.find("\r\n", kp);
      const std::string key = head.substr(kp + 19, ke - kp - 19);
      const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                               WsClient::handshake_accept(key) + "\r\n\r\n";
      ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
      if (!silent.load()) {
        for (const auto& frame : to_send) {
          const std::string f = WsClient::encode_frame(frame, 0x1, false);   // a server never masks
          ::send(fd, f.data(), f.size(), MSG_NOSIGNAL);
        }
        if (send_ping.load()) {
          const std::string p = WsClient::encode_frame("hb", 0x9, false);
          ::send(fd, p.data(), p.size(), MSG_NOSIGNAL);
        }
      }
      // Read what the client says (subscriptions, pongs) until it goes or we drop it.
      std::string in;
      if (close_immediately.load()) { ::close(fd); continue; }
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(close_after_send.load() ? 1 : 6);
      while (!stop.load() && std::chrono::steady_clock::now() < until) {
        fd_set rr;
        FD_ZERO(&rr);
        FD_SET(fd, &rr);
        timeval t2{0, 100 * 1000};
        if (::select(fd + 1, &rr, nullptr, nullptr, &t2) <= 0) continue;
        const int n = static_cast<int>(::recv(fd, buf, sizeof buf, 0));
        if (n <= 0) break;
        in.append(buf, static_cast<size_t>(n));
        int op = 0;
        bool fin = false;
        std::string payload;
        while (WsClient::decode_frame(in, &op, &payload, &fin)) {
          if (op == 0x1) {
            std::lock_guard<std::mutex> lk(mu);
            received.push_back(payload);
            ++subscriptions;
          } else if (op == 0xA) {
            ++pongs;
          } else if (op == 0x9) {
            const std::string pong = WsClient::encode_frame(payload, 0xA, false);
            ::send(fd, pong.data(), pong.size(), MSG_NOSIGNAL);
          }
        }
      }
      ::close(fd);
    }
  }
};

static void test_ws_codec() {
  // RFC 6455's own handshake example.
  CHECK(WsClient::handshake_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  // Round trips at each length encoding, masked (client) and unmasked (server).
  for (size_t n : {0u, 5u, 125u, 126u, 200u, 65535u, 65536u, 70000u}) {
    const std::string payload(n, 'x');
    for (bool mask : {true, false}) {
      std::string buf = WsClient::encode_frame(payload, 0x1, mask);
      int op = 0;
      bool fin = false;
      std::string out;
      CHECK(WsClient::decode_frame(buf, &op, &out, &fin));
      CHECK(op == 0x1 && fin && out == payload && buf.empty());
    }
  }
  // A masked frame is actually masked: the payload is not on the wire in the clear.
  const std::string masked = WsClient::encode_frame("subscribe-me", 0x1, true);
  CHECK(masked.find("subscribe-me") == std::string::npos);
  CHECK(WsClient::encode_frame("subscribe-me", 0x1, false).find("subscribe-me") != std::string::npos);
  // Partial input is not a frame yet, and two frames in one buffer are two frames.
  std::string two = WsClient::encode_frame("a", 0x1, false) + WsClient::encode_frame("bb", 0x1, false);
  std::string half = two.substr(0, 2);
  int op = 0;
  std::string out;
  CHECK(!WsClient::decode_frame(half, &op, &out, nullptr) || out.empty());
  CHECK(WsClient::decode_frame(two, &op, &out, nullptr) && out == "a");
  CHECK(WsClient::decode_frame(two, &op, &out, nullptr) && out == "bb" && two.empty());
}

static void test_ws_client_live() {
  MockWs server;
  server.to_send = {R"({"type":"subscribed","id":1})",
                    R"({"type":"pyth_value","sid":1,"seq":1,"msg":{"underlying_ticker":"Metal.Index.GOLD/USD","value_usd":"4351.61000000","source_ts_ms":1789160400000}})"};
  server.send_ping = true;
  std::vector<std::string> got;
  std::vector<std::string> events;
  std::mutex m;
  WsClient::Options o;
  o.url = server.url();
  o.name = "test-feed";
  o.ping_interval_seconds = 1;
  o.stale_after_seconds = 30;
  o.backoff_initial_seconds = 0.2;
  o.auth_headers = [] { return std::map<std::string, std::string>{{"KALSHI-ACCESS-KEY", "test-key"}}; };
  o.on_connect = [] { return std::vector<std::string>{R"({"id":1,"cmd":"subscribe","params":{"channels":["pyth_value"],"underlying_tickers":["all"]}})"}; };
  WsClient ws(o, [&](const std::string& text, long long) { std::lock_guard<std::mutex> lk(m); got.push_back(text); },
              [&](const std::string& ev, const std::string&) { std::lock_guard<std::mutex> lk(m); events.push_back(ev); });
  ws.start();
  for (int i = 0; i < 60 && got.size() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    std::lock_guard<std::mutex> lk(m);
    CHECK(got.size() >= 2);
    CHECK(!got.empty() && got[0].find("subscribed") != std::string::npos);
    CHECK(got.size() > 1 && got[1].find("Metal.Index.GOLD/USD") != std::string::npos);
  }
  // The subscription went out on connect, and the auth header was used.
  for (int i = 0; i < 30 && server.subscriptions.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    std::lock_guard<std::mutex> lk(server.mu);
    CHECK(!server.received.empty() && server.received[0].find("pyth_value") != std::string::npos);
  }
  // The server's ping was answered: a socket the peer thinks is dead is no use.
  for (int i = 0; i < 30 && server.pongs.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(server.pongs.load() >= 1);
  json st = ws.status();
  CHECK(st.value("connected", false) && st.value("fresh", false));
  CHECK(st.value("frames", 0) >= 2 && st.value("connects", 0) == 1);
  ws.stop();
  CHECK(!ws.running());
}

static void test_ws_reconnects() {
  MockWs server;
  server.to_send = {R"({"type":"pyth_value","seq":1})"};
  server.close_after_send = true;    // every connection is dropped a second in
  std::atomic<int> frames{0};
  WsClient::Options o;
  o.url = server.url();
  o.name = "test-reconnect";
  o.ping_interval_seconds = 1;
  o.backoff_initial_seconds = 0.2;
  o.backoff_max_seconds = 0.4;
  o.on_connect = [] { return std::vector<std::string>{R"({"cmd":"subscribe","params":{"channels":["pyth_value"]}})"}; };
  WsClient ws(o, [&](const std::string&, long long) { ++frames; });
  ws.start();
  for (int i = 0; i < 100 && (server.connections.load() < 2 || frames.load() < 2); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ws.stop();
  CHECK(server.connections.load() >= 2);          // it came back
  CHECK(frames.load() >= 2);                      // and re-received after the reconnect
  CHECK(server.subscriptions.load() >= 2);        // re-subscribing every time, not once
  json st = ws.status();
  CHECK(st.value("reconnects", 0) >= 1);
}

// A server that accepts the upgrade and immediately hangs up - a stale signature, a rate limit,
// a close frame on subscribe - used to be reconnected to as fast as the machine could open
// sockets, because the backoff was reset the moment a handshake completed and reapplied only when
// a CONNECT failed. Against an authenticated venue endpoint that is an unthrottled loop.
static void test_ws_backs_off_after_a_connection_that_dies_immediately() {
  MockWs server;
  server.close_immediately = true;    // handshake, then hang up at once
  WsClient::Options o;
  o.url = server.url();
  o.name = "test-flap";
  o.ping_interval_seconds = 5;
  o.stale_after_seconds = 30;         // no connection here lives anywhere near this long
  o.backoff_initial_seconds = 0.25;
  o.backoff_max_seconds = 2.0;
  WsClient ws(o, [](const std::string&, long long) {});
  ws.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const int attempts = server.connections.load();
  ws.stop();
  // 0.25s doubling to 2s, with jitter: about five or six attempts in two and a half seconds. The
  // number that matters is that it is not hundreds.
  CHECK(attempts >= 2);               // it does keep trying
  CHECK(attempts <= 15);              // and it is not spinning
  if (attempts > 15) std::fprintf(stderr, "  (saw %d connection attempts in 2.5s)\n", attempts);
}

static void test_ws_stale_socket_is_dead() {
  MockWs server;
  server.silent = true;              // accepts, subscribes, then says nothing at all
  WsClient::Options o;
  o.url = server.url();
  o.name = "test-stale";
  o.ping_interval_seconds = 1;
  o.stale_after_seconds = 1;         // a second of silence is enough for this test
  o.backoff_initial_seconds = 0.2;
  WsClient ws(o, [](const std::string&, long long) {});
  ws.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  for (int i = 0; i < 60 && server.connections.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  json st = ws.status();
  ws.stop();
  CHECK(server.connections.load() >= 2);   // silence was treated as a dead socket
  CHECK(!st.value("fresh", true) || st.value("reconnects", 0) >= 1);
}

// ---- the reference feed -------------------------------------------------------------------------
static std::string pyth_msg(const std::string& symbol, double price, long long source_ms, long long seq) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                R"({"type":"pyth_value","sid":3,"seq":%lld,"msg":{"underlying_ticker":"%s","value_usd":"%.8f","source_ts_ms":%lld,"received_at":%lld}})",
                seq, symbol.c_str(), price, source_ms, source_ms + 40);
  return buf;
}

static void test_reference_feed() {
  Store st(":memory:");
  ReferenceFeedSettings s;
  s.ws_url = "";              // nothing connects in a test
  s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const std::string GOLD = "Metal.Index.GOLD/USD";
  const long long t0 = 1789160400000;   // a minute boundary in milliseconds
  feed.on_message_for_tests(pyth_msg(GOLD, 4350.00, t0 - 120000, 1), t0 - 120000);
  feed.on_message_for_tests(pyth_msg(GOLD, 4351.00, t0 - 61000, 2), t0 - 61000);
  feed.on_message_for_tests(pyth_msg(GOLD, 4352.14, t0 - 2000, 3), t0 - 2000);
  auto last = feed.latest(GOLD);
  CHECK(last && std::fabs(last->price - 4352.14) < 1e-9 && last->sequence == 3);
  CHECK(last && last->source_ts_ms == t0 - 2000);
  // Duplicate and out-of-order source timestamps are ignored, as Kalshi ignores them.
  feed.on_message_for_tests(pyth_msg(GOLD, 9999.0, t0 - 2000, 4), t0 - 1500);
  feed.on_message_for_tests(pyth_msg(GOLD, 8888.0, t0 - 90000, 5), t0 - 1400);
  last = feed.latest(GOLD);
  CHECK(last && std::fabs(last->price - 4352.14) < 1e-9);
  // The settlement candle: the last observation in the minute ending at the boundary.
  auto close = feed.candle_close(GOLD, t0 / 1000);
  CHECK(close && std::fabs(*close - 4352.14) < 1e-9);
  CHECK(!feed.candle_close(GOLD, t0 / 1000 - 600));      // a minute with nothing in it
  CHECK(!feed.candle_close("Metal.Index.SILVER/USD", t0 / 1000));
  // Ticks come back oldest first and windowed.
  auto ticks = feed.ticks(GOLD, t0 / 1000 - 70);
  CHECK(ticks.size() == 2 && ticks.front().first <= ticks.back().first);
  // Rows are queued for the cycle, not written from the socket thread, and the cycle writes them.
  json before = st.query("SELECT COUNT(*) AS n FROM commodity_reference");
  CHECK(!before.empty() && before[0].value("n", -1) == 0);
  const int wrote = feed.flush(t0 / 1000);
  CHECK(wrote == 3);
  json rows = st.query("SELECT symbol, price, ts_exchange, sequence, source, raw_json FROM commodity_reference ORDER BY id");
  CHECK(rows.size() == 3);
  CHECK(rows[2].value("symbol", "") == GOLD);
  CHECK(std::fabs(rows[2].value("price", 0.0) - 4352.14) < 1e-9);
  CHECK(rows[2].value("ts_exchange", 0LL) == t0 - 2000);
  CHECK(rows[2].value("source", "") == "kalshi_ws:pyth_value");
  CHECK(rows[2].value("raw_json", "").find("underlying_ticker") != std::string::npos);
  CHECK(feed.flush(t0 / 1000) == 0);   // nothing left pending
  // Whatever Kalshi calls the underlying this month, the desk finds it by pattern, and an exact
  // configured name wins.
  CHECK(feed.symbol_for({"gold"}) == GOLD);
  CHECK(feed.symbol_for({"Metal.Index.GOLD/USD"}) == GOLD);
  CHECK(feed.symbol_for({"xau", "gold"}) == GOLD);
  CHECK(feed.symbol_for({"natgas"}).empty());
  // Nonsense on the wire is counted, never priced.
  feed.on_message_for_tests("{not json", t0);
  feed.on_message_for_tests(R"({"type":"pyth_value","msg":{"underlying_ticker":"X","value_usd":"0"}})", t0);
  feed.on_message_for_tests(R"({"type":"subscribed","id":1})", t0);
  CHECK(!feed.latest("X"));
  json status = feed.status();
  CHECK(status.value("unparsed", 0) == 2);
  CHECK(status["symbols"].contains(GOLD));
  CHECK(status["symbols"][GOLD].value("duplicates", 0) == 1);
  CHECK(status["symbols"][GOLD].value("out_of_order", 0) == 1);
  CHECK(status.value("recorded", 0) == 3);
  // The minute's CLOSE is the value a contract settles on, and which value that is is not known
  // until the next minute starts. Sampling alone would keep whichever tick happened to fall on
  // the sampling clock, which is not the same value.
  {
    Store st3(":memory:");
    ReferenceFeedSettings s3;
    s3.ws_url = "";
    s3.record_min_interval_ms = 4000;      // sample slowly, so most ticks are not sampled
    ReferenceFeed f3(st3, s3, nullptr);
    const long long base = 1789160400000;  // a minute boundary
    double px = 4300.0;
    for (int i = 0; i < 120; ++i) {        // two minutes of one-second ticks
      const long long t = base + i * 1000;
      f3.on_message_for_tests(pyth_msg(GOLD, px + i, t, i + 1), t);
    }
    f3.flush(base / 1000 + 200);
    // The last tick of the first minute is 4300+59 at :59, and it must be in the table even
    // though the 4-second sampler would never have picked it.
    json rows = st3.query("SELECT price FROM commodity_reference WHERE ts_received=?",
                          {(base + 59 * 1000) / 1000});
    CHECK(!rows.empty());
    CHECK(!rows.empty() && std::fabs(rows[0].value("price", 0.0) - (4300.0 + 59)) < 1e-9);
    // And the recorded candle close agrees with the one computed in memory, which is the number
    // the settlement check compares against the exchange's published strike.
    auto in_memory = f3.candle_close(GOLD, base / 1000 + 60);
    CHECK(in_memory && std::fabs(*in_memory - (4300.0 + 59)) < 1e-9);
  }
  // A write that fails keeps its rows instead of dropping them on the floor: this table is the
  // whole replay sample, and a silent hole in it is indistinguishable from a quiet market.
  {
    Store st4(":memory:");
    ReferenceFeedSettings s4; s4.ws_url = ""; s4.record_min_interval_ms = 0;
    ReferenceFeed f4(st4, s4, nullptr);
    const long long t4 = 1789160400000;
    f4.on_message_for_tests(pyth_msg(GOLD, 4400.0, t4, 1), t4);
    f4.on_message_for_tests(pyth_msg(GOLD, 4401.0, t4 + 1000, 2), t4 + 1000);
    st4.exec("DROP TABLE commodity_reference");            // the write will fail
    CHECK(f4.flush(t4 / 1000 + 2) == 0);
    CHECK(f4.status().value("write_failures", 0) == 1);
    CHECK(f4.status().value("pending_rows", 0) == 2);      // still queued, not lost
    st4.exec("CREATE TABLE commodity_reference(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "ts_received INTEGER, ts_exchange INTEGER, symbol TEXT, source TEXT, price REAL,"
             "sequence INTEGER, raw_json TEXT, message_gap_ms INTEGER, unchanged_age_ms INTEGER,"
             "value_changed INTEGER, observed_value_fresh INTEGER)");
    CHECK(f4.flush(t4 / 1000 + 3) == 2);                   // and written once the table is back
  }
  // The settlement check: our candle against Kalshi's published strike.
  feed.note_replication(GOLD, 4352.14, 4352.14, 0.01, t0 / 1000);
  auto rep = feed.replication(GOLD);
  CHECK(rep && rep->matched && std::fabs(rep->error) < 1e-9);
  feed.note_replication(GOLD, 4352.14, 4355.00, 0.01, t0 / 1000);
  rep = feed.replication(GOLD);
  CHECK(rep && !rep->matched);
  CHECK(feed.status()["symbols"][GOLD]["replication"].value("matched", true) == false);
}

// ---- session 17, priority 0: what the wire actually said ---------------------------------------
static void test_raw_capture_and_schema() {
  const std::string GOLD = "Metal.Index.GOLD/USD";
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0; s.raw_frames_to_keep = 3;
  ReferenceFeed feed(st, s, nullptr);
  const long long t0 = 1789160400000;
  for (int i = 0; i < 6; ++i)
    feed.on_message_for_tests(pyth_msg(GOLD, 4300.0 + i, t0 + i * 1000, i + 1), t0 + i * 1000);
  // The first frames of the run are kept byte for byte, and only the first few: this is evidence
  // about the schema, not a second copy of the feed.
  feed.flush(t0 / 1000 + 10);
  json raw = st.query("SELECT reason, frame FROM commodity_reference_raw ORDER BY id");
  CHECK(raw.size() == 3);
  CHECK(!raw.empty() && raw[0].value("reason", "") == "first frames");
  CHECK(!raw.empty() && raw[0].value("frame", "").find("underlying_ticker") != std::string::npos);
  CHECK(!raw.empty() && raw[0].value("frame", "").find("value_usd") != std::string::npos);

  // The observed schema is what arrived, with its JSON types - not what the parser expected.
  json sch = feed.observed_schema();
  CHECK(sch.contains("underlying_ticker") && sch["underlying_ticker"].value("type", "") == "string");
  CHECK(sch.contains("value_usd") && sch["value_usd"].value("type", "") == "string");
  CHECK(sch.contains("source_ts_ms") && sch["source_ts_ms"].value("type", "") == "number");
  CHECK(sch["underlying_ticker"].value("count", 0) == 6);
  CHECK(feed.status().value("schema_surprises", -1) == 0);

  // A field nobody has seen before is logged and kept, and the price still prices: an exchange
  // adding an informational field must not take the settlement feed down.
  feed.on_message_for_tests(
      R"({"type":"pyth_value","seq":9,"msg":{"underlying_ticker":"Metal.Index.GOLD/USD","value_usd":"4306.00000000",)"
      R"("source_ts_ms":1789160410000,"received_at":1789160410040,"confidence":"0.25"}})",
      t0 + 10000);
  CHECK(feed.status().value("schema_surprises", 0) == 1);
  CHECK(feed.observed_schema().contains("confidence"));
  auto v = feed.latest(GOLD);
  CHECK(v && std::fabs(v->price - 4306.0) < 1e-9);          // still priced

  // A KNOWN field changing type is different in kind: value_usd as a number instead of a string
  // silently changes what every price means, so it is counted and the frame is kept.
  feed.on_message_for_tests(
      R"({"type":"pyth_value","seq":10,"msg":{"underlying_ticker":"Metal.Index.GOLD/USD","value_usd":4307.0,)"
      R"("source_ts_ms":1789160411000,"received_at":1789160411040}})",
      t0 + 11000);
  CHECK(feed.status().value("schema_surprises", 0) == 2);
  // A frame with no usable price is rejected outright and kept for inspection.
  feed.on_message_for_tests(
      R"({"type":"pyth_value","seq":11,"msg":{"underlying_ticker":"Metal.Index.GOLD/USD","value_usd":"not a price"}})",
      t0 + 12000);
  CHECK(feed.status().value("unparsed", 0) == 1);
  feed.flush(t0 / 1000 + 20);
  json kept = st.query("SELECT reason FROM commodity_reference_raw WHERE reason != 'first frames'");
  std::set<std::string> reasons;
  for (const auto& r : kept) reasons.insert(r.value("reason", ""));
  CHECK(reasons.count("unknown field") && reasons.count("type changed") && reasons.count("no usable price"));

  // And the venue's own list of underlyings, rather than a pattern match against a guess.
  feed.on_message_for_tests(
      R"({"type":"pyth_value_underlying_list","sid":1,"seq":12,"msg":{"underlying_tickers":)"
      R"(["Metal.Index.GOLD/USD","Commodities.Index.CU/USD"]}})",
      t0 + 13000);
  auto advertised = feed.advertised_underlyings();
  CHECK(advertised.size() == 2);
  CHECK(!advertised.empty() && advertised[0] == "Metal.Index.GOLD/USD");
}

// A silence and an unchanged price are NOT the same observation, and the desk must not record
// them as though they were.
//
// Kalshi documents this channel as deduplicated, which says repeats are removed. It does not say
// whether a message is emitted on every scheduled source update when the value has not changed.
// So a gap has two explanations - the source published nothing, or the value did not move and the
// venue sent nothing - and this desk cannot tell them apart. What it CAN see is when it last
// heard anything, and when the value last changed. Those are recorded separately, and the
// trade gate uses the first, because the desk should not price from a value whose recency it
// cannot verify. Neither field claims the source stopped.
static void test_message_gap_and_unchanged_age_are_different_facts() {
  const std::string GOLD = "Metal.Index.GOLD/USD";
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  s.long_gap_multiple = 8.0; s.long_gap_floor_ms = 2000;
  const long long t0 = 1789160400000;

  // Case 1: the messages keep coming and the price does not move. Thirty seconds of an unchanging
  // market, fully observed.
  ReferenceFeed talking(st, s, nullptr);
  long long t = t0;
  for (int i = 0; i < 60; ++i, t += 200)
    talking.on_message_for_tests(pyth_msg(GOLD, 4300.0 + i * 0.01, t, i + 1), t);
  const long long changed_at = t - 200;
  for (int i = 0; i < 150; ++i, t += 200)                 // 30s of the SAME value, still arriving
    talking.on_message_for_tests(pyth_msg(GOLD, 4300.59, t, 100 + i), t);
  auto v1 = talking.latest(GOLD);
  CHECK(v1 && v1->message_gap_ms == 200);                 // we are hearing from it constantly
  CHECK(v1 && v1->observed_value_fresh);                  // so the value's recency is verified
  CHECK(v1 && !v1->value_changed);                        // and it has not moved
  CHECK(v1 && v1->unchanged_age_ms >= 29000);             // for about thirty seconds
  json st1 = talking.status()["symbols"][GOLD];
  // The measurement that would settle what a silence means: a channel that suppressed unchanged
  // values could never send one, so seeing them proves it does not suppress them.
  CHECK(st1.value("unchanged_value_messages", 0) >= 149);
  CHECK(st1.value("long_message_gaps", -1) == 0);
  CHECK(v1 && v1->value_change_ts_ms >= changed_at);

  // Case 2: the messages stop for thirty seconds, then one arrives with a different price. The
  // value is the same age as case 1 in wall-clock terms, and everything else about it differs.
  Store st2(":memory:");
  ReferenceFeed silent(st2, s, nullptr);
  long long u = t0;
  for (int i = 0; i < 60; ++i, u += 200)
    silent.on_message_for_tests(pyth_msg(GOLD, 4300.0 + i * 0.01, u, i + 1), u);
  const long long last_heard = u - 200;
  u = last_heard + 30000;
  silent.on_message_for_tests(pyth_msg(GOLD, 4302.0, u, 200), u);
  auto v2 = silent.latest(GOLD);
  CHECK(v2 && v2->message_gap_ms == 30000);               // we heard nothing for thirty seconds
  CHECK(v2 && !v2->observed_value_fresh);                 // so we cannot vouch for what we held
  CHECK(v2 && v2->value_changed);                         // and the number that arrived is new
  CHECK(v2 && v2->unchanged_age_ms == 0);                 // it changed just now
  json st2s = silent.status()["symbols"][GOLD];
  CHECK(st2s.value("unchanged_value_messages", -1) == 0); // nothing here proves anything about the channel
  CHECK(st2s.value("long_message_gaps", 0) == 1);
  CHECK(st2s.value("longest_gap_ms", 0) == 30000);

  // The two cases must be distinguishable in the recorded research output. They agree on the
  // thing a naive field would have collapsed them into - "the value is thirty seconds old" - and
  // disagree on everything that matters.
  talking.flush(t / 1000 + 1);
  silent.flush(u / 1000 + 1);
  json a = st.query("SELECT message_gap_ms, unchanged_age_ms, value_changed, observed_value_fresh"
                    " FROM commodity_reference ORDER BY id DESC LIMIT 1");
  json b = st2.query("SELECT message_gap_ms, unchanged_age_ms, value_changed, observed_value_fresh"
                     " FROM commodity_reference ORDER BY id DESC LIMIT 1");
  CHECK(!a.empty() && !b.empty());
  if (!a.empty() && !b.empty()) {
    CHECK(a[0].value("message_gap_ms", -1LL) == 200 && b[0].value("message_gap_ms", -1LL) == 30000);
    CHECK(a[0].value("observed_value_fresh", -1LL) == 1 && b[0].value("observed_value_fresh", -1LL) == 0);
    CHECK(a[0].value("value_changed", -1LL) == 0 && b[0].value("value_changed", -1LL) == 1);
    CHECK(a[0].value("unchanged_age_ms", -1LL) >= 29000 && b[0].value("unchanged_age_ms", -1LL) == 0);
    // The one field that would have made them look identical.
    CHECK(a[0].value("message_gap_ms", 0LL) != b[0].value("message_gap_ms", 0LL));
  }
}

// The volatility estimator acts on the same ambiguity, so its choice is a named setting rather
// than a constant in a loop, and the two positions must actually produce different numbers.
static void test_gap_policy_is_a_choice_not_an_assumption() {
  const long long now = 1789160000;
  // A quiet hour with one twenty-minute silence in the middle, and a move across it.
  std::vector<std::pair<long long, double>> ticks;
  double px = 4350.0;
  unsigned long long rng = 999;
  for (long long t = now - 3600; t <= now - 1800; t += 5) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    px *= std::exp(0.00002 * (static_cast<double>(rng % 2000001) / 1000000.0 - 1.0));
    ticks.emplace_back(t, px);
  }
  const double before = px;
  for (long long t = now - 600; t <= now; t += 5) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    px *= std::exp(0.00002 * (static_cast<double>(rng % 2000001) / 1000000.0 - 1.0));
    ticks.emplace_back(t, t == now - 600 ? before * 1.004 : px);
  }
  VolSettings drop;                       // the default: a move across a long silence is discarded
  VolSettings include = drop;
  include.long_gap_policy = VolSettings::GapPolicy::IncludeAll;
  VolEstimate a = estimate_volatility(ticks, now, drop);
  VolEstimate b = estimate_volatility(ticks, now, include);
  CHECK(a.sigma > 0 && b.sigma > 0);
  CHECK(a.sigma != b.sigma);              // the choice is not cosmetic
  CHECK(a.rv_60m != b.rv_60m);
  // And the estimate says which position produced it, so a recorded sigma can be read later.
  CHECK(a.basis.find("dropped") != std::string::npos);
  CHECK(b.basis.find("every sampled interval") != std::string::npos);
  // Which way the difference goes depends on how big the move across the silence was, so neither
  // policy is uniformly safer. But the case Sam's objection is about has a determinate direction:
  // if a silence really means "the price did not change", then the value on the far side is the
  // SAME, dropping the interval throws away a genuine stretch of calm, and the estimate comes out
  // too high. Here is that case, with the bias it produces.
  {
    std::vector<std::pair<long long, double>> flat;
    double q = 4350.0;
    unsigned long long r2 = 4242;
    for (long long t = now - 3600; t <= now - 1800; t += 5) {
      r2 ^= r2 << 13; r2 ^= r2 >> 7; r2 ^= r2 << 17;
      q *= std::exp(0.00002 * (static_cast<double>(r2 % 2000001) / 1000000.0 - 1.0));
      flat.emplace_back(t, q);
    }
    const double held = q;                     // nothing moved for twenty minutes
    for (long long t = now - 600; t <= now; t += 5) {
      r2 ^= r2 << 13; r2 ^= r2 >> 7; r2 ^= r2 << 17;
      q *= std::exp(0.00002 * (static_cast<double>(r2 % 2000001) / 1000000.0 - 1.0));
      flat.emplace_back(t, t == now - 600 ? held : q);
    }
    VolEstimate f_drop = estimate_volatility(flat, now, drop);
    VolEstimate f_incl = estimate_volatility(flat, now, include);
    CHECK(f_incl.rv_60m < f_drop.rv_60m);      // the calm is counted rather than discarded
    CHECK(f_drop.rv_60m > 0 && f_incl.rv_60m > 0);
  }
}

// Priority 7's prerequisite. The desk's ordinary market data is a REST poll every five seconds,
// which cannot measure a reprice at 50 ms; this is the exchange's own ticker stream, stamped with
// the venue's millisecond clock and with ours, which can.
static void test_market_quote_feed() {
  Store st(":memory:");
  MarketQuoteFeed::Settings s;
  s.ws_url = "";                        // nothing connects in a test
  MarketQuoteFeed q(st, s, nullptr);
  const long long t0 = 1789160400000;

  q.watch({"KXGOLD15M-A", "KXWTI15M-A"});
  CHECK(q.watching().size() == 2);
  q.on_message_for_tests(
      R"({"type":"ticker","sid":1,"msg":{"market_ticker":"KXGOLD15M-A","price_dollars":"0.51",)"
      R"("yes_bid_dollars":"0.50","yes_ask_dollars":"0.52","yes_bid_size_fp":"12500",)"
      R"("yes_ask_size_fp":"8000","volume_fp":"430000","open_interest_fp":"1200000","ts_ms":1789160399950}})",
      t0);
  // Both clocks, because the difference between them is the only honest way to say whether a lag
  // we measure is the exchange's or our own network's.
  CHECK(q.flush(t0 / 1000) == 1);
  json rows = st.query("SELECT * FROM commodity_quotes ORDER BY id");
  CHECK(rows.size() == 1);
  if (!rows.empty()) {
    CHECK(rows[0].value("ticker", "") == "KXGOLD15M-A");
    CHECK(rows[0].value("ts_received_ms", 0LL) == t0);
    CHECK(rows[0].value("ts_exchange_ms", 0LL) == 1789160399950LL);   // 50 ms before we saw it
    CHECK(std::fabs(rows[0].value("yes_bid", 0.0) - 0.50) < 1e-9);
    CHECK(std::fabs(rows[0].value("yes_ask", 0.0) - 0.52) < 1e-9);
    CHECK(std::fabs(rows[0].value("yes_bid_size", 0.0) - 125.0) < 1e-9);   // fixed point, 2 decimals
    CHECK(std::fabs(rows[0].value("volume", 0.0) - 4300.0) < 1e-9);
    CHECK(rows[0].value("raw_json", "").find("market_ticker") != std::string::npos);
  }
  // Windows roll every fifteen minutes: the set is a difference, not a reconnect.
  q.watch({"KXWTI15M-A", "KXGOLD15M-B"});
  auto watching = q.watching();
  CHECK(watching.size() == 2);
  CHECK(std::find(watching.begin(), watching.end(), "KXGOLD15M-B") != watching.end());
  CHECK(std::find(watching.begin(), watching.end(), "KXGOLD15M-A") == watching.end());
  // Anything that is not a ticker update is counted, never priced.
  q.on_message_for_tests(R"({"type":"subscribed","id":1})", t0 + 1000);
  q.on_message_for_tests("{not json", t0 + 1000);
  q.on_message_for_tests(R"({"type":"ticker","msg":{"yes_bid_dollars":"0.5"}})", t0 + 1000);
  CHECK(q.status().value("unparsed", 0) == 2);
  CHECK(q.flush(t0 / 1000 + 1) == 0);
  CHECK(q.status().value("recorded", 0) == 1);

  // The book, kept as the venue sent it. This is the stream that can answer "could I have
  // executed this quantity"; the ticker above can only answer "did the quoted price move", and a
  // fill claim built on ticker data describes a price nobody could have taken.
  q.on_message_for_tests(
      R"({"type":"subscribed","id":2,"msg":{"channel":"orderbook_delta","sid":7}})", t0 + 1500);
  q.on_message_for_tests(
      R"({"type":"orderbook_snapshot","sid":7,"seq":100,"msg":{"market_ticker":"KXWTI15M-A",)"
      R"("ts_ms":1789160400100}})", t0 + 1600);
  q.on_message_for_tests(
      R"({"type":"orderbook_delta","sid":7,"seq":101,"msg":{"market_ticker":"KXWTI15M-A",)"
      R"("price":"0.47","delta":"-25.00","side":"bid","ts_ms":1789160400200}})", t0 + 1700);
  // A missing message is counted rather than interpolated over: a book reconstructed across a
  // gap is a book that never existed.
  q.on_message_for_tests(
      R"({"type":"orderbook_delta","sid":7,"seq":104,"msg":{"market_ticker":"KXWTI15M-A",)"
      R"("price":"0.48","delta":"10.00","side":"ask","ts_ms":1789160400400}})", t0 + 1800);
  CHECK(q.flush(t0 / 1000 + 2) == 0);          // no new ticker rows, book rows are their own stream
  json be = st.query("SELECT kind, sequence, side, price, delta, ts_exchange_ms FROM commodity_book_events ORDER BY id");
  CHECK(be.size() == 3);
  if (be.size() == 3) {
    CHECK(be[0].value("kind", "") == "orderbook_snapshot" && be[0].value("sequence", 0LL) == 100);
    CHECK(be[1].value("side", "") == "bid" && std::fabs(be[1].value("delta", 0.0) + 25.0) < 1e-9);
    CHECK(be[2].value("sequence", 0LL) == 104);
    CHECK(be[1].value("ts_exchange_ms", 0LL) == 1789160400200LL);
  }
  json qs = q.status();
  CHECK(qs.value("book_messages", 0) == 3);
  CHECK(qs.value("book_snapshots", 0) == 1);
  CHECK(qs.value("book_sequence_gaps", 0) == 1);      // 101 -> 104
  CHECK(qs.value("book_recorded", 0) == 3);
  // And the venue's own subscription id is used, not a guessed one.
  CHECK(qs["subscription_ids"].value("orderbook_delta", 0) == 7);

  MarketQuoteFeed::Settings prod = market_quote_settings(json::object(), "prod");
  CHECK(prod.ws_url == "wss://external-api-ws.kalshi.com/trade-api/ws/v2");
  CHECK(prod.enabled);
  CHECK(!market_quote_settings(json{{"quotes", {{"enabled", false}}}}, "prod").enabled);
}

static void test_reference_feed_settings() {
  ReferenceFeedSettings prod = reference_feed_settings(json::object(), "prod");
  CHECK(prod.ws_url == "wss://external-api-ws.kalshi.com/trade-api/ws/v2");
  CHECK(prod.sign_path == "/trade-api/ws/v2");
  CHECK(prod.subscribe.size() == 1 && prod.subscribe[0] == "all");
  CHECK(reference_feed_settings(json::object(), "demo").ws_url.find("demo") != std::string::npos);
  json params{{"reference", {{"enabled", false}, {"record_min_interval_ms", 250},
                             {"subscribe", json::array({"Metal.Index.GOLD/USD"})},
                             {"stale_after_seconds", 5}}}};
  ReferenceFeedSettings s = reference_feed_settings(params, "prod");
  CHECK(!s.enabled && s.record_min_interval_ms == 250 && s.stale_after_seconds == 5);
  CHECK(s.subscribe.size() == 1 && s.subscribe[0] == "Metal.Index.GOLD/USD");
}


// ---- 12, 13, 14: the strategy on top of the feed --------------------------------------------
// A deterministic walk on an exact five-second grid, so that every price the tests assert on is
// reproducible and the candle at the window open is a price we know.
struct FeedWalk {
  double at_open = 0, last = 0;
};
static FeedWalk fill_feed(ReferenceFeed& feed, const std::string& sym, long long now, long long open,
                          double base, double step, int seconds = 3600) {
  FeedWalk w;
  unsigned long long rng = 88172645463325252ULL;
  double p = base;
  for (long long t = ((now - seconds) / 5) * 5; t <= now; t += 5) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    const double u = static_cast<double>(rng % 2000001) / 1000000.0 - 1.0;   // [-1, 1]
    p *= std::exp(step * u);
    feed.on_message_for_tests(pyth_msg(sym, p, t * 1000, t), t * 1000);
    // The candle that has closed by the window open is the one covering [open-60, open): the
    // exchange cannot publish a strike out of a minute that has not finished yet.
    if (t < open) w.at_open = p;
    w.last = p;
  }
  return w;
}

static const std::string GOLD_SYM = "Metal.Index.GOLD/USD";

static CommodityTerminal::Underlying gold_underlying() {
  for (auto& u : CommodityTerminal::default_underlyings())
    if (u.key == "gold") return u;
  return {};
}

// The futures cross-check is off in these tests unless one of them switches it on: it is an HTTP
// call, and a unit test that reaches the internet is a unit test that fails on a train.
static json no_external() { return json{{"external", {{"enabled", false}}}}; }

// The settlement gate wants fifty consecutive windows before it trusts anything. Tests that are
// about something else say so explicitly rather than quietly running under a different rule; the
// gate itself is tested on its own, below.
static json no_external_one_window() {
  json p = no_external();
  p["replication_windows_required"] = 1;
  return p;
}

static Market gold_market(long long open, double strike, bool rules_ok = true) {
  Market m = market("greater_or_equal", strike);
  m.open_time = iso_of(open);
  m.close_time = iso_of(open + 900);
  m.expected_expiration_time = iso_of(open + 900 + 6 * 3600);   // the exchange's own field, hours late
  m.rules_primary = rules_ok ? "If the Pyth Gold price index at the close time is greater than or equal to "
                               "the strike, the market settles to Yes."
                             : "If the London bullion afternoon auction price is above the strike, Yes.";
  return m;
}

static void test_strategy_prices_from_the_reference() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60;
  const FeedWalk w = fill_feed(feed, GOLD_SYM, now, open, 4350.0, 0.00004);
  CHECK(w.at_open > 0 && w.last > 0);

  CommodityTerminal gold(gold_underlying(), &feed, no_external_one_window(), "test");
  Market m = gold_market(open, w.at_open);       // the published strike IS our candle close
  CHECK(gold.covers(m, Series{}));
  gold.prepare({m}, st);
  CHECK(gold.symbol() == GOLD_SYM);              // found by pattern, whatever Kalshi calls it
  auto rep = feed.replication(GOLD_SYM);
  CHECK(rep && rep->matched);
  CHECK(gold.settlement_verified(m).value_or(false));

  auto f = gold.fair(m, Series{}, st);
  CHECK(f.has_value());
  if (!f) return;
  CHECK(!f->research_only);
  CHECK(f->p > 0 && f->p < 1);
  CHECK(f->inputs.value("valid", false));
  CHECK(f->inputs["reasons"].empty());
  CHECK(f->inputs.value("auto_eligible", true) == false);   // never, in this session
  CHECK_NEAR(f->inputs.value("reference_price", 0.0), w.last, 1e-6);
  CHECK_NEAR(f->inputs.value("strike", 0.0), w.at_open, 1e-6);
  // The clock is the window close, not the exchange's expiration field six hours later.
  CHECK(f->inputs.value("seconds_to_close", 0.0) > 500 && f->inputs.value("seconds_to_close", 0.0) <= 900);
  CHECK(f->inputs["settlement"].value("replicated_strike", false));
  CHECK(f->inputs["volatility"].value("warm", false));
  CHECK(f->inputs["terminal_distribution"].value("mu_per_second", 1.0) == 0.0);   // v1 claims no drift
  CHECK(f->inputs.value("underlying_ts", 0LL) > 0);

  // Every strike on the same distribution, and the ladder is coherent: a higher strike is never
  // more likely to be exceeded.
  double previous = 1.1;
  for (double k : {w.last - 3.0, w.last - 1.0, w.last, w.last + 1.0, w.last + 3.0}) {
    auto g = gold.fair(gold_market(open, k), Series{}, st);
    CHECK(g.has_value());
    if (!g) continue;
    CHECK(g->p <= previous + 1e-9);
    previous = g->p;
  }
  // A strike far above the reference with ten minutes to run is nearly hopeless, and far below is
  // nearly certain: the model is pricing a distance, not a direction.
  auto far_above = gold.fair(gold_market(open, w.last * 1.01), Series{}, st);
  auto far_below = gold.fair(gold_market(open, w.last * 0.99), Series{}, st);
  CHECK(far_above && far_above->p < 0.02);
  CHECK(far_below && far_below->p > 0.98);

  // The estimate reaches the router as a quote, diagnostics intact: the router's own checks read
  // inputs_valid, auto_eligible and the reasons out of exactly this object.
  ProbabilityQuote q = quote_from_fair(m, "Commodities", "commodity_terminal_gold", *f, f->research_only,
                                       f->research_reason, now, now);
  CHECK(q.p_yes == f->p);
  CHECK(q.inputs_valid);
  CHECK(!q.auto_eligible);
  CHECK(q.underlying_ts > 0 && q.underlying_ts <= now);
  const json& di = q.diagnostics["inputs"];
  CHECK_NEAR(di.value("reference_price", 0.0), w.last, 1e-6);
  CHECK(di["terminal_distribution"].contains("sigma_per_sqrt_second"));
  CHECK(di["volatility"].contains("rv_5m") && di["volatility"].contains("ewma"));
  CHECK(di["settlement"].value("rules_name_this_feed", false));
}

static void test_strategy_blocks_without_a_good_reference() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0; s.stale_after_seconds = 20;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60;

  // 1. No reference at all: nothing is priced. Not a probability of 0.5, not a guess from the
  //    futures - no estimate, so there is no row for anyone to trade.
  {
    CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");
    gold.set_external_for_tests(4360.0, now);
    Market m = gold_market(open, 4350.0);
    CHECK(!gold.fair(m, Series{}, st).has_value());
  }

  // 2. A stale reference: the estimate exists (it is worth recording) but its own input check
  //    fails, which is what the router blocks on.
  {
    ReferenceFeed old_feed(st, s, nullptr);
    fill_feed(old_feed, GOLD_SYM, now - 120, open, 4350.0, 0.00004);
    CommodityTerminal gold(gold_underlying(), &old_feed, no_external(), "test");
    gold.prepare({gold_market(open, 4350.0)}, st);
    auto f = gold.fair(gold_market(open, 4350.0), Series{}, st);
    CHECK(f.has_value());
    if (f) {
      CHECK(!f->inputs.value("valid", true));
      bool said_so = false;
      for (const auto& r : f->inputs["reasons"])
        said_so = said_so || r.get<std::string>().find("reference observation too old") != std::string::npos;
      CHECK(said_so);
      CHECK(!quote_from_fair(gold_market(open, 4350.0), "Commodities", "commodity_terminal_gold", *f,
                             f->research_only, f->research_reason, now, now).inputs_valid);
    }
  }

  // 3. Rules that name some other settlement source: research, whatever the feed says.
  {
    CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");
    const FeedWalk w = fill_feed(feed, GOLD_SYM, now, open, 4350.0, 0.00004);
    Market m = gold_market(open, w.at_open, /*rules_ok=*/false);
    gold.prepare({m}, st);
    CHECK(gold.settlement_verified(m) == std::optional<bool>(false));
    auto f = gold.fair(m, Series{}, st);
    CHECK(f && f->research_only);
    CHECK(f && f->inputs["settlement"].value("rules_name_this_feed", true) == false);
  }

  // 4. A published strike our own recording of the feed cannot reproduce. That is the check that
  //    says we are reading the number this contract settles on, so failing it is research, not a
  //    trade - the probability would otherwise be a confident statement about the wrong series.
  {
    Store st2(":memory:");
    ReferenceFeed f2(st2, s, nullptr);
    const long long open2 = ((now_s() - 300) / 60) * 60;
    fill_feed(f2, GOLD_SYM, now_s(), open2, 4350.0, 0.00004);
    CommodityTerminal gold(gold_underlying(), &f2, no_external(), "test");
    Market m = gold_market(open2, 4111.11);     // nowhere near our candle
    gold.prepare({m}, st2);
    auto rep = f2.replication(GOLD_SYM);
    CHECK(rep && !rep->matched);
    auto f = gold.fair(m, Series{}, st2);
    CHECK(f && f->research_only);
    CHECK(f && f->inputs["settlement"].value("replicated_strike", true) == false);
  }
}

// The settlement check has to check the window we are pricing. The store hands markets over in
// ticker order and keeps a 15-minute window listed as open until a reconcile catches up, so the
// list normally starts with a window that has already closed. Checking that one and calling it a
// day left the live window unverified while the desk said "settlement confirmed" - the worst
// shape a check can take, because it is indistinguishable from a check that passed.
static void test_replication_checks_the_window_being_priced() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60, previous_open = open - 900;
  // One walk covering both windows, so both candle closes are real numbers from the same feed.
  FeedWalk w;
  {
    unsigned long long rng = 88172645463325252ULL;
    double px = 4350.0;
    double at_previous = 0;
    for (long long t = ((now - 3600) / 5) * 5; t <= now; t += 5) {
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      px *= std::exp(0.00004 * (static_cast<double>(rng % 2000001) / 1000000.0 - 1.0));
      feed.on_message_for_tests(pyth_msg(GOLD_SYM, px, t * 1000, t), t * 1000);
      if (t < previous_open) at_previous = px;
      if (t < open) w.at_open = px;
      w.last = px;
    }
    w.at_open = w.at_open;
    // The closed window's own strike, which does match: this is the row that used to satisfy the
    // check and then stop it.
    Market done = gold_market(previous_open, at_previous);
    Market live = gold_market(open, w.at_open + 50.0);      // the live window does NOT match
    CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");
    gold.prepare({done, live}, st);
    auto rep = feed.replication(GOLD_SYM);
    CHECK(rep && rep->window_open == open);          // the window we are pricing, not the old one
    CHECK(rep && !rep->matched);
    CHECK(gold.settlement_verified(live) == std::optional<bool>(false));
    auto f = gold.fair(live, Series{}, st);
    CHECK(f && f->research_only);
    // Research, not blocked: the estimate is worth recording against settlement even while it is
    // worth showing to nobody, and that is the sample COM-1 is built from.
    CHECK(f && f->inputs.value("valid", false));
    CHECK(f && f->inputs["settlement"].value("replicated_strike", true) == false);
    ProbabilityQuote q = quote_from_fair(live, "Commodities", "commodity_terminal_gold", *f,
                                         f->research_only, f->research_reason, now, now);
    CHECK(q.inputs_valid && q.research_only);
  }
  // And when the live window does match, it is confirmed however many stale windows precede it.
  {
    Store st2(":memory:");
    ReferenceFeed f2(st2, s, nullptr);
    const long long now2 = now_s(), open2 = ((now2 - 300) / 60) * 60;
    const FeedWalk w2 = fill_feed(f2, GOLD_SYM, now2, open2, 4350.0, 0.00004);
    Market done = gold_market(open2 - 900, 1.0);     // a closed window with a nonsense strike
    Market live = gold_market(open2, w2.at_open);
    CommodityTerminal gold(gold_underlying(), &f2, no_external_one_window(), "test");
    gold.prepare({done, live}, st2);
    auto rep = f2.replication(GOLD_SYM);
    CHECK(rep && rep->matched && rep->window_open == open2);
    CHECK(gold.settlement_verified(live).value_or(false));
  }
}

// The gate itself: fifty consecutive windows, and one mismatch puts it back to zero. Twelve hours
// of recording per underlying before a probability from it is trusted, which is the price of the
// claim "this feed is the feed the contract settles on".
static void test_fifty_consecutive_windows_before_trust() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60;
  const FeedWalk w = fill_feed(feed, GOLD_SYM, now, open, 4350.0, 0.00004);
  CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");   // the real 50
  Market m = gold_market(open, w.at_open);
  gold.set_symbol_for_tests(GOLD_SYM);

  // Forty-nine perfect windows is not fifty.
  for (int i = 0; i < 49; ++i)
    feed.note_replication(GOLD_SYM, 4350.0, 4350.0, 0.01, now, open - (49 - i) * 900);
  auto rep = feed.replication(GOLD_SYM);
  CHECK(rep && rep->consecutive_matches == 49);
  CHECK(gold.settlement_verified(m) == std::optional<bool>(false));
  auto f = gold.fair(m, Series{}, st);
  CHECK(f && f->research_only);
  bool counted = false;
  if (f) counted = f->research_reason.find("49 consecutive windows of 50") != std::string::npos;
  CHECK(counted);
  // The estimate is still computed and still valid data - it is the trust that is missing, not
  // the number - so it accumulates the sample the gate is waiting for.
  CHECK(f && f->inputs.value("valid", false));
  CHECK(f && f->p > 0 && f->p < 1);

  // The fiftieth passes.
  feed.note_replication(GOLD_SYM, 4350.0, 4350.0, 0.01, now, open);
  CHECK(gold.settlement_verified(m).value_or(false));
  CHECK(!gold.fair(m, Series{}, st)->research_only);

  // And one mismatch throws the run away rather than denting it: the claim is "every time".
  feed.note_replication(GOLD_SYM, 4350.0, 4399.0, 0.01, now, open + 900);
  rep = feed.replication(GOLD_SYM);
  CHECK(rep && rep->consecutive_matches == 0);
  CHECK(rep && rep->windows_checked == 51 && rep->windows_matched == 50);
  CHECK(gold.settlement_verified(m) == std::optional<bool>(false));
}

// "The settlement value is rounded to the nearest N decimal places" is the exchange telling us
// what a tick is on this underlying. The desk reads it instead of keeping its own table.
static void test_tolerance_comes_from_the_contract() {
  auto with_rules = [](const std::string& secondary) {
    Market m = market("greater_or_equal", 4350.0);
    m.rules_secondary = secondary;
    return m;
  };
  CHECK(CommodityTerminal::stated_tolerance(
            with_rules("The settlement value is rounded to the nearest 2 decimal places.")) ==
        std::optional<double>(0.01));
  CHECK(CommodityTerminal::stated_tolerance(
            with_rules("The settlement value is rounded to the nearest 5 decimal places. When confirming...")) ==
        std::optional<double>(0.00001));
  CHECK(CommodityTerminal::stated_tolerance(
            with_rules("The settlement value is rounded to the nearest three decimal places.")) ==
        std::optional<double>(0.001));
  CHECK(!CommodityTerminal::stated_tolerance(with_rules("")).has_value());
  CHECK(!CommodityTerminal::stated_tolerance(with_rules("Settles on the London afternoon auction.")).has_value());
  // Falls back to the underlying's own constant when the contract says nothing, and prefers the
  // contract when it does.
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = "";
  ReferenceFeed feed(st, s, nullptr);
  CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");
  CHECK(std::fabs(gold.tolerance_for(with_rules("")) - 0.02) < 1e-12);          // the fallback
  CHECK(std::fabs(gold.tolerance_for(with_rules("rounded to the nearest 2 decimal places")) - 0.01) < 1e-12);
  // Copper rounds to five places, so its tick is four orders of magnitude finer than gold's -
  // exactly the case a single hand-written constant gets wrong.
  auto copper = [] {
    for (auto& u : CommodityTerminal::default_underlyings())
      if (u.key == "copper") return u;
    return CommodityTerminal::Underlying{};
  }();
  CHECK(copper.key == "copper");
  CHECK(copper.series.size() == 1 && copper.series[0] == "KXCOPPER15M");
  CHECK(!copper.symbol_patterns.empty() && copper.symbol_patterns[0] == "Commodities.Index.CU/USD");
  CommodityTerminal cu(copper, &feed, no_external(), "test");
  CHECK(std::fabs(cu.tolerance_for(with_rules("rounded to the nearest 5 decimal places")) - 0.00001) < 1e-15);
}

// Priority 3: the probability at fixed moments before the close, written as the window runs down.
// None of this can be reconstructed afterwards, so the test is about what actually lands in the
// table - including at ten seconds to close, where the model would refuse to offer a trade and the
// probability is at its most interesting.
static void test_calibration_checkpoints() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60;
  const FeedWalk w = fill_feed(feed, GOLD_SYM, now, open, 4350.0, 0.00004);
  CommodityTerminal gold(gold_underlying(), &feed, no_external_one_window(), "test");

  // A window with fourteen and a half minutes to run: nothing has been crossed yet.
  Market m = gold_market(open, w.at_open);
  m.close_time = iso_of(now + 870);
  m.yes_bid = 0.48; m.yes_ask = 0.52; m.quote_ts = now;
  gold.prepare({m}, st);
  CHECK(st.query("SELECT COUNT(*) AS n FROM commodity_calibration")[0].value("n", -1) == 0);

  // Walk the window down. Each checkpoint is written once, at the first cycle at or past it.
  const std::vector<long long> lefts{839, 700, 599, 400, 299, 121, 119, 61, 59, 31, 29, 11, 9, 3};
  for (long long left : lefts) {
    Market step = m;
    step.close_time = iso_of(now_s() + left);
    gold.prepare({step}, st);
  }
  json rows = st.query(
      "SELECT checkpoint_seconds, seconds_to_close, p_model, spot, strike, distance_sigma, vol_sigma,"
      " yes_ask, reference_observed_fresh, settlement_verified, model, tau_seconds"
      " FROM commodity_calibration ORDER BY checkpoint_seconds DESC");
  CHECK(rows.size() == 7);                     // 14m, 10m, 5m, 2m, 1m, 30s, 10s - each exactly once
  std::vector<double> want{840, 600, 300, 120, 60, 30, 10};
  for (size_t i = 0; i < want.size() && i < rows.size(); ++i) {
    CHECK(std::fabs(rows[i].value("checkpoint_seconds", 0.0) - want[i]) < 1e-9);
    // Recorded at or just inside the checkpoint, never before it.
    CHECK(rows[i].value("seconds_to_close", 1e9) <= want[i] + 1);
    CHECK(rows[i].value("p_model", -1.0) >= 0 && rows[i].value("p_model", 2.0) <= 1);
    CHECK(std::fabs(rows[i].value("spot", 0.0) - w.last) < 1e-6);
    CHECK(std::fabs(rows[i].value("strike", 0.0) - w.at_open) < 1e-6);
    CHECK(rows[i].value("vol_sigma", 0.0) > 0);
    CHECK(rows[i].value("model", "") == "commodity_terminal_gold");
    CHECK(rows[i].value("reference_observed_fresh", -1) == 1);
    // The market it was priced against, so the edge can be computed from the row alone.
    CHECK(std::fabs(rows[i].value("yes_ask", 0.0) - 0.52) < 1e-9);
  }
  // The ten-second row exists even though fair() refuses to price inside twenty seconds: the
  // checkpoint is a research record, not an offer, and that gate is about trading.
  const json& last = rows[rows.size() - 1];
  CHECK(last.value("checkpoint_seconds", 0.0) == 10);
  CHECK(last.value("tau_seconds", 0.0) > 0);
  {
    Market late = m;
    late.close_time = iso_of(now_s() + 9);
    CHECK(!gold.fair(late, Series{}, st).has_value());     // not tradable
  }
  // Running the same window again writes nothing more.
  Market again = m;
  again.close_time = iso_of(now_s() + 5);
  gold.prepare({again}, st);
  CHECK(st.query("SELECT COUNT(*) AS n FROM commodity_calibration")[0].value("n", -1) == 7);
  // And a window that has gone away stops being tracked, so the bookkeeping does not grow for the
  // life of the process.
  gold.prepare({}, st);
  Market fresh_window = gold_market(open + 900, w.at_open);
  fresh_window.close_time = iso_of(now_s() + 100);
  fresh_window.ticker = "KXGOLD15M-26SEP111930-15";
  gold.prepare({fresh_window}, st);
  json counts = st.query("SELECT COUNT(DISTINCT ticker) AS n FROM commodity_calibration");
  CHECK(counts[0].value("n", 0) == 2);
}

static void test_external_cross_check_never_prices() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = ""; s.record_min_interval_ms = 0;
  ReferenceFeed feed(st, s, nullptr);
  const long long now = now_s(), open = ((now - 300) / 60) * 60;
  const FeedWalk w = fill_feed(feed, GOLD_SYM, now, open, 4350.0, 0.00004);
  CommodityTerminal gold(gold_underlying(), &feed, no_external(), "test");
  Market m = gold_market(open, w.at_open);
  gold.prepare({m}, st);

  auto plain = gold.fair(m, Series{}, st);
  CHECK(plain.has_value());
  // A futures print a hundred dollars away from the reference. It is recorded as a basis and it
  // changes nothing: the probability is the same number to the last bit.
  gold.set_external_for_tests(w.last + 100.0, now);
  auto with_ext = gold.fair(m, Series{}, st);
  CHECK(with_ext.has_value());
  if (plain && with_ext) {
    CHECK(plain->p == with_ext->p);
    CHECK_NEAR(with_ext->inputs["external"].value("basis", 0.0), 100.0, 1e-6);
    CHECK_NEAR(with_ext->inputs.value("reference_price", 0.0), w.last, 1e-6);
  }
  // And a futures print cannot stand in for a missing reference (checked above with no feed at
  // all); here the same strategy with an unknown symbol prices nothing either.
  CommodityTerminal lonely(gold_underlying(), &feed, no_external(), "test");
  lonely.set_symbol_for_tests("Metal.Index.NOTHING/USD");
  lonely.set_external_for_tests(w.last, now);
  CHECK(!lonely.fair(m, Series{}, st).has_value());
}


// ---- 15: the wiring. Which models exist, and who owns the socket ------------------------------
// Priority 10: where a model stands, from what is recorded. The two states that mean "risk money
// on this" are not reachable by computation at all, which is the point of the ladder.
static void test_economic_status_ladder() {
  Store st(":memory:");
  ReferenceFeedSettings s; s.ws_url = "";
  ReferenceFeed feed(st, s, nullptr);      // creates the tables
  const std::string model = "commodity_terminal_gold";

  json status = commodity_model_status(st, model);
  CHECK(status.value("status", "") == "SETTLEMENT_UNVERIFIED");
  CHECK(status.value("settled_windows", -1.0) == 0);

  auto checkpoint = [&](const std::string& ticker, long long consecutive, const char* result) {
    st.exec("INSERT INTO commodity_calibration(ts,ticker,model,model_version,checkpoint_seconds,"
            "p_model,replication_consecutive) VALUES(?,?,?,?,?,?,?)",
            {now_s(), ticker, model, std::string("v1"), 60.0, 0.6, consecutive});
    Market m;
    m.ticker = ticker;
    m.event_ticker = ticker;
    m.series_ticker = "KXGOLD15M";
    m.status = "finalized";
    m.result = result;
    st.upsert_market(m);
  };

  // Replication short of the gate: nothing else matters yet.
  checkpoint("W-1", 10, "yes");
  CHECK(commodity_model_status(st, model).value("status", "") == "SETTLEMENT_UNVERIFIED");

  // Gate passed, sample far too small.
  checkpoint("W-2", 50, "yes");
  json accumulating = commodity_model_status(st, model);
  CHECK(accumulating.value("status", "") == "DATA_ACCUMULATING");
  CHECK(accumulating.value("settled_windows", -1.0) == 2);
  CHECK(accumulating["milestones"].size() == 4);
  CHECK(accumulating["milestones"][0].value("settlements", 0) == 50);
  CHECK(accumulating["milestones"][0].value("reached", true) == false);

  // A small sample with a verdict on it is still DATA_ACCUMULATING: a conclusion from fifty
  // settlements is not a conclusion, whatever the replay says about it.
  st.kv_set("commodity_verdict:" + model,
            json{{"status", "MISPRICING_DETECTED"}, {"reason", "paired Brier -0.004, CI excludes zero"}}.dump());
  CHECK(commodity_model_status(st, model).value("status", "") == "DATA_ACCUMULATING");

  // With the sample reached, the replay's verdict is what the desk reports.
  json tiny = commodity_model_status(st, model, 50, 2);
  CHECK(tiny.value("status", "") == "MISPRICING_DETECTED");
  CHECK(tiny.value("reason", "").find("paired Brier") != std::string::npos);
  // Including the one that says the fees ate it.
  st.kv_set("commodity_verdict:" + model,
            json{{"status", "FAILED_COST"}, {"reason", "net EV negative in every edge bucket"}}.dump());
  CHECK(commodity_model_status(st, model, 50, 2).value("status", "") == "FAILED_COST");

  // A trading state cannot be reached by research. Only a person sets those, and the desk reads
  // that decision rather than making it.
  st.kv_set("commodity_verdict:" + model, json{{"status", "LIVE_CANDIDATE"}, {"reason", "very confident"}}.dump());
  CHECK(commodity_model_status(st, model, 50, 2).value("status", "") == "DATA_ACCUMULATING");
  st.kv_set("commodity_promotion:" + model, "PAPER_CANDIDATE");
  json promoted = commodity_model_status(st, model, 50, 2);
  CHECK(promoted.value("status", "") == "PAPER_CANDIDATE");
  CHECK(promoted.value("reason", "") == "set by hand");
}

static void test_registry_and_engine_wiring() {
  Store st(":memory:");
  ReferenceFeedSettings fs; fs.ws_url = ""; ReferenceFeed feed(st, fs, nullptr);
  Config cfg; cfg.user_agent = "test";

  auto names = [](const std::vector<std::unique_ptr<Strategy>>& v) {
    std::vector<std::string> out;
    for (const auto& s2 : v) out.push_back(s2->name());
    return out;
  };
  // Without a settlement reference the terminal models are not built at all. That is the point:
  // there is no number to price them from, so there must be no model claiming to price them.
  const auto bare = names(build_strategies(cfg, nullptr));
  for (const auto& n : bare) CHECK(n.rfind("commodity_terminal", 0) != 0);

  const auto wired = build_strategies(cfg, &feed);
  const auto with_feed = names(wired);
  int terminals = 0; size_t first_terminal = with_feed.size(), futures = with_feed.size();
  for (size_t i = 0; i < with_feed.size(); ++i) {
    if (with_feed[i].rfind("commodity_terminal", 0) == 0) { ++terminals; first_terminal = std::min(first_terminal, i); }
    if (with_feed[i] == "commodity_futures") futures = i;
  }
  CHECK(terminals == 5);   // gold, silver, wti, natgas, copper
  // The first strategy that covers a market wins, and commodity_futures covers the whole category.
  CHECK(first_terminal < futures);
  for (const char* n : {"commodity_terminal_gold", "commodity_terminal_silver",
                        "commodity_terminal_wti", "commodity_terminal_natgas",
                        "commodity_terminal_copper"}) {
    bool found = false;
    for (const auto& have : with_feed) found = found || have == n;
    CHECK(found);
  }
  // Each one answers for its own series and nobody else's, and every one is v1.
  for (const auto& s2 : wired) {
    if (s2->name().rfind("commodity_terminal", 0) != 0) continue;
    CHECK(s2->model_version() == "v1");
    CHECK(s2->category() == "Commodities");
    Market gold = market("greater_or_equal", 4350.0);      // a KXGOLD15M ticker
    const bool is_gold = s2->name() == "commodity_terminal_gold";
    CHECK(s2->covers(gold, Series{}) == is_gold);
  }

  // The desk's own settings say these are offered, never sent: manual, and nothing graduated.
  const json defaults = systematic_defaults();
  for (const char* key : {"commodity_terminal_gold_v1", "commodity_terminal_silver_v1",
                          "commodity_terminal_wti_v1", "commodity_terminal_natgas_v1",
                          "commodity_terminal_copper_v1"}) {
    CHECK(defaults["model_routes"].value(key, std::string()) == "manual");
    for (const auto& g : defaults["graduated_models"]) CHECK(g.get<std::string>() != key);
  }

  // A model nobody blends. The desk shrinks a young model's number toward the market mid by
  // default, which for this one would answer its own question with the thing it is questioning:
  // the whole point is whether the market's price is wrong about the terminal distribution.
  for (const auto& s2 : wired)
    if (s2->name().rfind("commodity_terminal", 0) == 0) CHECK(s2->default_market_weight() == 0.0);

  // A strategy with no mode is "off", and an off strategy is skipped before prepare(), so a
  // config that does not name these would run the socket and price nothing.
  {
    const std::string path = "/tmp/desk_commodity_cfg.json";
    std::FILE* f = std::fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    if (f) {
      std::fputs(R"({"env":"demo","key_id":"k","pem_path":"/dev/null","watch_series":["KXGOLD15M"]})", f);
      std::fclose(f);
      Config loaded = load_config(path);
      for (const char* n : {"commodity_terminal_gold", "commodity_terminal_silver",
                            "commodity_terminal_wti", "commodity_terminal_natgas",
                            "commodity_terminal_copper"})
        CHECK(loaded.mode_for(n) == "manual");
      std::remove(path.c_str());
    }
  }

  // The engine owns the socket, and only opens one when this desk actually watches one of these
  // series. A desk that trades none of them has no business holding an authenticated feed open.
  KalshiClient client("https://example.invalid/trade-api/v2", nullptr, "t");
  {
    Store s1(":memory:"); Config c1 = cfg; Executor e1(c1, s1, client);
    Engine idle(c1, s1, client, e1);
    CHECK(idle.reference_feed() == nullptr);
  }
  {
    Store s2(":memory:"); Config c2 = cfg; c2.watch_series = {"KXGOLD15M", "KXWTI15M"};
    Executor e2(c2, s2, client);
    Engine watching(c2, s2, client, e2);
    CHECK(watching.reference_feed() != nullptr);
    // No API key in a test, so nothing connects: the feed is owned and idle, and the cycle can
    // still flush it without a socket ever existing.
    CHECK(watching.reference_feed() && !watching.reference_feed()->running());
    CHECK(watching.reference_feed() && watching.reference_feed()->flush(now_s()) == 0);
    // The table it records into exists from construction, so a replay of a run that never
    // connected reads an empty table rather than a missing one.
    json t = s2.query("SELECT COUNT(*) AS n FROM commodity_reference");
    CHECK(!t.empty() && t[0].value("n", -1) == 0);
  }
  {
    Store s3(":memory:"); Config c3 = cfg; c3.watch_series = {"KXGOLD15M"};
    c3.strategy_params["commodity"] = json{{"reference", {{"enabled", false}}}};
    Executor e3(c3, s3, client);
    Engine off(c3, s3, client, e3);
    CHECK(off.reference_feed() == nullptr);   // switched off in the config: no feed, so no models
  }
}

int main() {
  test_payoff_parsing();
  test_threshold_and_range_math();
  test_degenerate_and_tie();
  test_volatility_estimate();
  test_minute_close();
  test_ws_codec();
  test_ws_client_live();
  test_ws_reconnects();
  test_ws_backs_off_after_a_connection_that_dies_immediately();
  test_ws_stale_socket_is_dead();
  test_reference_feed();
  test_market_quote_feed();
  test_reference_feed_settings();
  test_raw_capture_and_schema();
  test_message_gap_and_unchanged_age_are_different_facts();
  test_gap_policy_is_a_choice_not_an_assumption();
  test_strategy_prices_from_the_reference();
  test_strategy_blocks_without_a_good_reference();
  test_replication_checks_the_window_being_priced();
  test_fifty_consecutive_windows_before_trust();
  test_tolerance_comes_from_the_contract();
  test_calibration_checkpoints();
  test_external_cross_check_never_prices();
  test_economic_status_ladder();
  test_registry_and_engine_wiring();
  std::printf("commodity: %d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
