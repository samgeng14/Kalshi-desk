#pragma once
// Shared by deskd-test-net and deskd-test-wire: the fake Kalshi behind its TLS listener with a CA
// made under $TMPDIR at run time, deskd's connection factory trusting that CA for loopback only,
// and a small raw loopback server for replies the fake cannot be made to send.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/net/conn.h"
#include "deskd/net/guard.h"
#include "deskd/net/http.h"
#include "fake_kalshi.h"
#include "tls_pki.h"

namespace fx {

using deskd::SendGuard;
using deskd::net::ConnFactory;
using deskd::net::Endpoint;
using deskd::net::FactoryOptions;

constexpr fakex::Micros D = fakex::kMicrosPerDollar;
const std::string T = "KXTEST-26OCT06-A";            // shard 0, 1c grid
const std::string G = "KXNATGAS15M-26OCT061145-45";  // shard 2, tapered grid

inline fakex::MarketSpec spec(const std::string& ticker, int shard) {
  fakex::MarketSpec s;
  s.ticker = ticker;
  s.exchange_index = shard;
  if (shard == 2) {
    s.ticks = fakex::tapered_deci_cent();
    s.price_level_structure = "tapered_deci_cent";
  }
  return s;
}

// The PKI for one test binary: made once, removed with its TempDir.
struct Pki {
  TempDir dir;
  fakex::TestPki p;
  bool ok = false;
  Pki() {
    std::string err;
    ok = fakex::make_test_pki(dir.path, &p, &err);
    if (!ok) std::fprintf(stderr, "test PKI: %s\n", err.c_str());
  }
};
inline Pki& pki() {
  static Pki p;
  return p;
}

// The fake over TLS (or plain), with two markets, $1,000 on shards 0 and 2, and a street book on
// T: YES 0.40 x5 bid, 0.42 x3 ask.
struct Venue {
  std::unique_ptr<fakex::FakeKalshi> f;
  bool ok = false;
  explicit Venue(fakex::FakeConfig cfg = {}, bool tls = true, const std::string& cert = "", const std::string& key = "") {
    cfg.rate_limits = false;
    if (tls) {
      cfg.tls_cert_file = cert.empty() ? pki().p.server_chain : cert;
      cfg.tls_key_file = key.empty() ? pki().p.server_key : key;
    }
    f = std::make_unique<fakex::FakeKalshi>(std::move(cfg));
    f->with_engine([](fakex::Engine& e, std::int64_t now) {
      e.add_market(spec(T, 0), now);
      e.add_market(spec(G, 2), now);
      e.set_cash(0, 0, 1000 * D, now);
      e.set_cash(0, 2, 1000 * D, now);
      e.street_add(T, false, 4000, 500, now);
      e.street_add(T, true, 5800, 300, now);
    });
    std::string err;
    ok = f->start(&err);
    if (!ok) std::fprintf(stderr, "fake start: %s\n", err.c_str());
  }
  int port() const { return f->port(); }
  Endpoint ep(const std::string& host = "127.0.0.1") const {
    std::string why;
    auto e = Endpoint::parse(std::string(f->tls() ? "https" : "http") + "://" + host + ":" + std::to_string(port()) +
                                 "/trade-api/v2",
                             &why);
    return *e;
  }
  std::size_t orders() {
    return f->with_engine([](fakex::Engine& e, std::int64_t) { return e.user_orders().size(); });
  }
  bool has_cid(const std::string& cid) {
    return f->with_engine([&](fakex::Engine& e, std::int64_t) {
      for (const fakex::Order* o : e.user_orders())
        if (o->client_order_id == cid) return true;
      return false;
    });
  }
};

inline const SendGuard& demo_guard() {
  static const SendGuard g(false, nullptr);
  return g;
}
inline FactoryOptions trust_test_ca() {
  FactoryOptions o;
  o.loopback_ca_file = pki().p.ca_cert;
  return o;
}

inline std::int64_t deadline_ms(int ms) { return deskd::net::mono_us() + static_cast<std::int64_t>(ms) * 1000; }

// A one-shot raw server on 127.0.0.1: accepts connections, reads one request head (and its
// Content-Length body), then answers with reply() for that connection's number, and closes when
// close_after (with SO_LINGER 0 when reset, so the client sees a reset).
struct RawServer {
  int fd = -1, port = 0;
  std::thread th;
  std::atomic<bool> stop{false};
  std::atomic<int> served{0};
  std::function<std::string(int)> reply;
  bool close_after = true, reset = false, read_request = true;
  int stall_ms = 0;
  RawServer() {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(fd, 16);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
  }
  void start() {
    th = std::thread([this] {
      while (!stop) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 20) <= 0) continue;
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        const int n = served++;
        if (read_request) {
          std::string buf;
          char b[4096];
          for (;;) {
            const auto he = buf.find("\r\n\r\n");
            if (he != std::string::npos) {
              std::size_t cl = 0;
              const auto p2 = buf.find("Content-Length: ");
              if (p2 != std::string::npos && p2 < he) cl = std::stoul(buf.substr(p2 + 16));
              if (buf.size() >= he + 4 + cl) break;
            }
            pollfd q{c, POLLIN, 0};
            if (::poll(&q, 1, 2000) <= 0) break;
            const ssize_t r = ::recv(c, b, sizeof b, 0);
            if (r <= 0) break;
            buf.append(b, static_cast<std::size_t>(r));
          }
        }
        if (stall_ms) std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        const std::string out = reply ? reply(n) : std::string();
        std::size_t off = 0;
        while (off < out.size()) {
          const ssize_t w = ::send(c, out.data() + off, out.size() - off, 0);
          if (w <= 0) break;
          off += static_cast<std::size_t>(w);
        }
        if (reset) {
          linger l{1, 0};
          ::setsockopt(c, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        }
        if (close_after || reset) ::close(c);
      }
    });
  }
  ~RawServer() {
    stop = true;
    if (th.joinable()) th.join();
    if (fd >= 0) ::close(fd);
  }
  Endpoint ep() const { return *Endpoint::parse("http://127.0.0.1:" + std::to_string(port), nullptr); }
};

inline double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * (v.size() - 1) + 0.5));
  return v[i];
}

}  // namespace fx
