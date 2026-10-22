#pragma once
// The fake venues' loopback listener, shared by the fake Kalshi and the fake Polymarket US: it
// accepts on 127.0.0.1 only, runs each connection's serve(fd) on a thread of its own, and keeps what
// a drop of the kept-open connections and a stop need. Each fake writes its own serve(): its routes,
// auth, limits and faults.
//
// TLS (week-0 review issue 10(a), added 2026-10-07): with tls_cert_file and tls_key_file set the
// listener speaks TLS 1.2+ (https:// and wss://), so a client's real TLS path runs against the
// fake: the handshake and its verification (tests/fake_exchange/tls_pki.h makes a CA and a leaf at
// run time), TLS 1.3's session tickets arriving after the handshake, resumption, close_notify, a
// bare FIN without one (tls_close_notify false: a load balancer that just drops), and the idle
// close of a kept-open connection. Each TLS connection runs a pump thread between the TLS socket
// and a local socketpair whose other end is served exactly like a plain connection, so every
// fault, the wire log and the idle close behave the same in both modes. One difference: the order
// between two connections' bytes (EVENTS_FIRST, REPLY_FIRST) holds for what the fake hands its
// pumps; the two pumps then forward in parallel, microseconds apart.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fakex {

struct ListenerConfig {
  int port = 0;                      // 0: any free port
  // TLS: both set, the listener speaks TLS with this chain (leaf first) and key, PEM files.
  std::string tls_cert_file, tls_key_file;
  bool tls_close_notify = true;      // false: every close is a bare TCP FIN, no close_notify
  int tls_handshake_ms = 5000;       // a client that does not finish its handshake is dropped
};

class Listener {
 public:
  // Serves one connection: returns when it is done with fd, which the listener then closes.
  using Serve = std::function<void(int fd)>;
  using Log = std::function<void(const std::string& line)>;

  explicit Listener(ListenerConfig cfg);  // loads the TLS chain and key, when set
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Listens and accepts until stop(); a chain or key that would not load fails it. `log` takes
  // the listener's own wire-log lines ("tls handshake-failed").
  bool start(Serve serve, Log log, std::string* err);
  // Stops accepting, shuts every connection's socket under its serve() and waits for them.
  void stop();
  int port() const { return port_; }
  bool tls() const { return ctx_ != nullptr; }

  // Every kept-open REST connection closed under its client (a load balancer's idle close, a
  // network drop): the next request on it gets nothing back. A TLS connection still ending its
  // handshake is dropped as soon as it is served.
  void drop_http();
  // fd carries a WebSocket now: drop_http leaves it alone.
  void upgraded(int fd);

  std::uint64_t accepted() const { return accepted_.load(); }
  std::uint64_t tls_handshakes() const { return tls_ok_.load(); }        // completed
  std::uint64_t tls_handshake_failures() const { return tls_failed_.load(); }
  std::uint64_t tls_resumed() const { return tls_resumed_.load(); }

 private:
  void accept_loop();
  void serve_tls(int tcp_fd);   // the pump; serve_() on a socketpair

  ListenerConfig cfg_;
  Serve serve_;
  Log log_;
  void* ctx_ = nullptr;  // SSL_CTX*, when TLS
  std::string tls_err_;
  int listen_fd_ = -1, port_ = 0;
  std::atomic<bool> running_{false}, stopping_{false};
  std::thread acceptor_;
  struct Conn {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex conn_mu_;
  std::vector<Conn> conns_;
  // fd -> 1 (REST) or 2 (upgraded) while its thread owns it; a TLS socket ending its handshake
  // is 3, or 4 once a drop reached it
  std::map<int, int> conn_fds_;
  std::atomic<std::uint64_t> accepted_{0}, tls_ok_{0}, tls_failed_{0}, tls_resumed_{0};
};

}  // namespace fakex
