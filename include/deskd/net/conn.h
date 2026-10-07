#pragma once
// deskd's venue connections: the one factory every REST and WebSocket socket comes from (review,
// 2026-10-06; net/guard.h's week-1 rule).
//
// open() asks the send guard first, every time (a reconnect is a connect): a production host
// without --live and the production account's lock, or any host off the allow-list, gets no
// socket at all, and the factory counts the sockets it made so a test can prove none was. There
// is no redirect: a 3xx is a reply like any other and nobody follows it. Plain HTTP is for
// loopback only (the fake exchange), and a "localhost" that resolves anywhere but 127/8 or ::1 is
// refused.
//
// Every venue host gets TLS 1.2 or later with the certificate chain and the host name verified
// against the system trust store, SNI set, and no way to turn either off. On macOS OpenSSL does
// not read the Keychain: Homebrew's openssl@3 reads /opt/homebrew/etc/openssl@3/cert.pem (the
// ca-certificates bundle, 187 roots on 2026-10-07), and if that store comes up empty the factory
// loads /etc/ssl/cert.pem (the system's bundle, 128 roots) instead of trusting nothing. A test
// CA is trusted only for loopback endpoints, through its own context that never sees a venue
// host (FactoryOptions::loopback_ca_file): tests make one under $TMPDIR at run time.
//
// Every socket gets TCP_NODELAY (a 100-byte order must not wait for Nagle) and SO_NOSIGPIPE
// before OpenSSL sees it (the old desk died of SIGPIPE on a TLS reconnect), and is non-blocking:
// each read and write waits in poll() for at most the caller's slice (250 ms or less in deskd's
// threads, so a quiet socket still comes back to beat()).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_session_st SSL_SESSION;

namespace deskd {
class SendGuard;
}

namespace deskd::net {

struct SessionCache;   // the last TLS session per host:port, shared by a factory and its connections

// "https://demo-api.kalshi.co/trade-api/v2" -> tls, host, 443, "/trade-api/v2"
struct Endpoint {
  bool tls = true;
  std::string host;        // lower case, no brackets
  int port = 443;
  std::string base_path;   // "" or "/..." without a trailing slash
  // nullopt with *why on anything but http(s)://host[:port][/path] (no user info, query or fragment)
  static std::optional<Endpoint> parse(const std::string& url, std::string* why);
  std::string host_header() const;   // "host" on the scheme's own port, else "host:port"
  std::string url() const;
};

enum class IoStatus {
  OK,               // bytes moved
  TIMEOUT,          // nothing within the slice
  CLOSED,           // the peer closed: TLS close_notify, or a FIN on plain TCP
  CLOSED_UNCLEAN,   // a FIN on TLS without close_notify (a load balancer that just drops)
  ERROR             // a reset or any other failure
};
const char* io_name(IoStatus s);

struct IoResult {
  IoStatus status = IoStatus::ERROR;
  std::size_t n = 0;
  std::string why;
};

class Conn {
 public:
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  // Writes every byte or fails. Waits in slices of at most 250 ms, calling idle() between them,
  // until deadline_us (steady clock, mono_us()).
  IoResult write_all(const char* data, std::size_t n, std::int64_t deadline_us,
                     const std::function<void()>& idle = nullptr);
  // Up to cap bytes, waiting at most timeout_ms (0: do not wait).
  IoResult read_some(char* out, std::size_t cap, int timeout_ms);
  // Still fit for a request: open, nothing unread, no close_notify or FIN. Never waits; it reads
  // what TLS 1.3 sends after the handshake (session tickets) so those never count as data.
  bool usable(std::string* why = nullptr);
  // Sends close_notify (TLS, when notify) and closes. Idempotent.
  void close(bool notify = true);
  bool closed() const { return fd_ < 0; }

  bool tls() const { return ssl_ != nullptr; }
  const Endpoint& endpoint() const { return ep_; }
  int fd() const { return fd_; }
  std::int64_t opened_us() const { return opened_us_; }   // when it became ready (mono_us)
  double connect_ms() const { return connect_ms_; }       // DNS and TCP
  double handshake_ms() const { return handshake_ms_; }   // TLS on top, 0 on plain
  const std::string& peer() const { return peer_; }       // "ip:port"
  const std::string& tls_version() const { return tls_version_; }
  const std::string& cipher() const { return cipher_; }
  bool resumed() const { return resumed_; }               // a TLS session resumed

 private:
  friend class ConnFactory;
  Conn() = default;
  Endpoint ep_;
  int fd_ = -1;
  SSL* ssl_ = nullptr;
  std::int64_t opened_us_ = 0;
  double connect_ms_ = 0, handshake_ms_ = 0;
  std::string peer_, tls_version_, cipher_;
  bool resumed_ = false;
  bool broken_ = false;                    // a fatal TLS error: no close_notify may follow it
  std::shared_ptr<SessionCache> cache_;    // where this connection's new session tickets go
  std::string cache_key_;
};

struct FactoryOptions {
  // A CA certificate (PEM) trusted for loopback endpoints only, for tests; "" = none.
  std::string loopback_ca_file;
  // A bundle to use instead of the system store (tests that prove a venue-shaped host is
  // verified against a given root); "" = the system store.
  std::string system_ca_file_override;
  bool resume_sessions = true;   // offer the last session ticket per host:port
};

struct FactoryStats {
  std::uint64_t sockets = 0;          // socket(2) calls: what the guard test counts
  std::uint64_t refused = 0;          // opens the guard (or the loopback check) refused
  std::uint64_t connects = 0, connect_fail = 0;
  std::uint64_t handshakes = 0, handshake_fail = 0, resumed = 0;
};

class ConnFactory {
 public:
  // The guard must outlive the factory. *err is set and ok() false when no trust store loads.
  ConnFactory(const SendGuard& guard, FactoryOptions opt = {});
  ~ConnFactory();
  ConnFactory(const ConnFactory&) = delete;
  ConnFactory& operator=(const ConnFactory&) = delete;
  bool ok(std::string* why = nullptr) const;
  std::size_t trust_roots() const { return roots_; }

  // A connected (and for TLS, handshaken and verified) connection, or nullptr with *why. Never
  // waits past timeout_ms in total. Thread-safe.
  std::unique_ptr<Conn> open(const Endpoint& ep, int timeout_ms, std::string* why);
  FactoryStats stats() const;

 private:
  SSL_CTX* make_ctx(const std::string& ca_file, bool system_store, std::string* err);
  static int on_new_session(SSL* ssl, SSL_SESSION* sess);

  const SendGuard& guard_;
  FactoryOptions opt_;
  SSL_CTX* venue_ctx_ = nullptr;      // the system store
  SSL_CTX* loopback_ctx_ = nullptr;   // the test CA only; loopback endpoints only
  std::size_t roots_ = 0;
  std::string err_;
  mutable std::mutex mu_;             // stats_ only, never held across I/O
  FactoryStats stats_;
  std::shared_ptr<SessionCache> cache_;
  // resolved addresses per host:port for 60 s, so a pool miss pays no DNS lookup
  struct Resolved {
    std::int64_t at_us = 0;
    std::vector<std::vector<unsigned char>> addrs;   // sockaddr bytes
  };
  std::map<std::string, Resolved> dns_;
};

}  // namespace deskd::net
