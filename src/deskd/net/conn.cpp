#include "deskd/net/conn.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "deskd/net/guard.h"
#include "deskd/net/http.h"

namespace deskd::net {

struct SessionCache {
  std::mutex mu;
  std::map<std::string, SSL_SESSION*> last;   // owned
  ~SessionCache() {
    for (auto& kv : last) SSL_SESSION_free(kv.second);
  }
  void put(const std::string& key, SSL_SESSION* s) {   // takes s's reference
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = last[key];
    if (slot) SSL_SESSION_free(slot);
    slot = s;
  }
  SSL_SESSION* get(const std::string& key) {           // a new reference, or nullptr
    std::lock_guard<std::mutex> lk(mu);
    auto it = last.find(key);
    if (it == last.end() || !it->second || !SSL_SESSION_is_resumable(it->second)) return nullptr;
    SSL_SESSION_up_ref(it->second);
    return it->second;
  }
};

namespace {

constexpr std::int64_t kDnsTtlUs = 60LL * 1000 * 1000;
constexpr int kMaxSliceMs = 250;

int conn_index() {
  static const int idx = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return idx;
}

std::string ssl_errors() {
  std::string out;
  unsigned long e;
  char buf[256];
  while ((e = ERR_get_error()) != 0) {
    ERR_error_string_n(e, buf, sizeof buf);
    if (!out.empty()) out += "; ";
    out += buf;
  }
  return out;
}

bool is_ip_literal(const std::string& h) {
  in6_addr a6;
  in_addr a4;
  return inet_pton(AF_INET, h.c_str(), &a4) == 1 || inet_pton(AF_INET6, h.c_str(), &a6) == 1;
}

bool is_loopback_addr(const sockaddr* sa) {
  if (sa->sa_family == AF_INET) {
    const auto* s4 = reinterpret_cast<const sockaddr_in*>(sa);
    return (ntohl(s4->sin_addr.s_addr) >> 24) == 127;
  }
  if (sa->sa_family == AF_INET6) {
    const auto* s6 = reinterpret_cast<const sockaddr_in6*>(sa);
    return IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr);
  }
  return false;
}

std::string addr_str(const sockaddr* sa) {
  char host[INET6_ADDRSTRLEN] = {0};
  int port = 0;
  if (sa->sa_family == AF_INET) {
    const auto* s4 = reinterpret_cast<const sockaddr_in*>(sa);
    inet_ntop(AF_INET, &s4->sin_addr, host, sizeof host);
    port = ntohs(s4->sin_port);
    return std::string(host) + ":" + std::to_string(port);
  }
  const auto* s6 = reinterpret_cast<const sockaddr_in6*>(sa);
  inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof host);
  port = ntohs(s6->sin6_port);
  return "[" + std::string(host) + "]:" + std::to_string(port);
}

// poll one fd for `events` up to ms; >0 ready, 0 timeout, <0 error
int wait_fd(int fd, short events, int ms) {
  pollfd p{fd, events, 0};
  for (;;) {
    const int r = ::poll(&p, 1, ms);
    if (r < 0 && errno == EINTR) continue;
    if (r > 0 && (p.revents & POLLNVAL)) return -1;
    return r;
  }
}

int remaining_ms(std::int64_t deadline_us) {
  const std::int64_t left = deadline_us - mono_us();
  if (left <= 0) return 0;
  return static_cast<int>(std::min<std::int64_t>((left + 999) / 1000, 1 << 30));
}

std::size_t count_roots(SSL_CTX* ctx) {
  X509_STORE* st = SSL_CTX_get_cert_store(ctx);
  if (!st) return 0;
#if OPENSSL_VERSION_NUMBER >= 0x30300000L
  STACK_OF(X509_OBJECT)* objs = X509_STORE_get1_objects(st);
  if (!objs) return 0;
  std::size_t n = 0;
  for (int i = 0; i < sk_X509_OBJECT_num(objs); ++i)
    if (X509_OBJECT_get_type(sk_X509_OBJECT_value(objs, i)) == X509_LU_X509) ++n;
  sk_X509_OBJECT_pop_free(objs, X509_OBJECT_free);
  return n;
#else
  // OpenSSL before 3.3 (Linux CI images) has only the borrowed view; the store is private to
  // this context and not yet shared with any thread while the factory counts its roots.
  STACK_OF(X509_OBJECT)* objs = X509_STORE_get0_objects(st);
  if (!objs) return 0;
  std::size_t n = 0;
  for (int i = 0; i < sk_X509_OBJECT_num(objs); ++i)
    if (X509_OBJECT_get_type(sk_X509_OBJECT_value(objs, i)) == X509_LU_X509) ++n;
  return n;
#endif
}

}  // namespace

// ---- Endpoint ---------------------------------------------------------------------------------

std::optional<Endpoint> Endpoint::parse(const std::string& url, std::string* why) {
  auto bad = [&](const std::string& w) -> std::optional<Endpoint> {
    if (why) *why = w;
    return std::nullopt;
  };
  Endpoint e;
  std::size_t pos;
  std::string lower_url = url;
  for (char& c : lower_url)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (lower_url.compare(0, 8, "https://") == 0) {
    e.tls = true;
    e.port = 443;
    pos = 8;
  } else if (lower_url.compare(0, 7, "http://") == 0) {
    e.tls = false;
    e.port = 80;
    pos = 7;
  } else {
    return bad("not an http(s) URL: " + url);
  }
  const std::size_t slash = url.find('/', pos);
  std::string authority = url.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
  std::string path = slash == std::string::npos ? std::string() : url.substr(slash);
  if (authority.empty()) return bad("no host in " + url);
  if (authority.find('@') != std::string::npos) return bad("user info in " + url);
  if (path.find_first_of("?#") != std::string::npos) return bad("a query or fragment in a base URL: " + url);
  std::string host = authority;
  if (authority[0] == '[') {
    const std::size_t rb = authority.find(']');
    if (rb == std::string::npos) return bad("a bad IPv6 literal in " + url);
    host = authority.substr(1, rb - 1);
    const std::string rest = authority.substr(rb + 1);
    if (!rest.empty()) {
      if (rest[0] != ':' || rest.size() < 2) return bad("a bad port in " + url);
      authority = rest;   // ":port"
    } else {
      authority.clear();
    }
    if (!authority.empty()) {
      const std::string p = authority.substr(1);
      if (p.find_first_not_of("0123456789") != std::string::npos || p.size() > 5) return bad("a bad port in " + url);
      e.port = std::stoi(p);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      const std::string p = authority.substr(colon + 1);
      if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos || p.size() > 5)
        return bad("a bad port in " + url);
      e.port = std::stoi(p);
    }
  }
  if (e.port < 1 || e.port > 65535) return bad("a port out of range in " + url);
  for (char& c : host)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (!host.empty() && host.back() == '.') host.pop_back();
  if (host.empty()) return bad("no host in " + url);
  e.host = host;
  while (!path.empty() && path.back() == '/') path.pop_back();
  e.base_path = path;
  return e;
}

std::string Endpoint::host_header() const {
  const bool v6 = host.find(':') != std::string::npos;
  const std::string h = v6 ? "[" + host + "]" : host;
  if ((tls && port == 443) || (!tls && port == 80)) return h;
  return h + ":" + std::to_string(port);
}

std::string Endpoint::url() const { return std::string(tls ? "https://" : "http://") + host_header() + base_path; }

const char* io_name(IoStatus s) {
  switch (s) {
    case IoStatus::OK: return "OK";
    case IoStatus::TIMEOUT: return "TIMEOUT";
    case IoStatus::CLOSED: return "CLOSED";
    case IoStatus::CLOSED_UNCLEAN: return "CLOSED_UNCLEAN";
    case IoStatus::ERROR: return "ERROR";
  }
  return "?";
}

// ---- Conn -------------------------------------------------------------------------------------

Conn::~Conn() { close(true); }

void Conn::close(bool notify) {
  if (ssl_) {
    // SSL_shutdown after a fatal error is forbidden (SSL_shutdown(3)), and means nothing before
    // the handshake ends; otherwise one try at close_notify, without waiting for the peer's
    if (notify && !broken_ && SSL_is_init_finished(ssl_)) {
      ERR_clear_error();
      SSL_shutdown(ssl_);
    }
    SSL_set_ex_data(ssl_, conn_index(), nullptr);
    SSL_free(ssl_);
    ssl_ = nullptr;
    ERR_clear_error();
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

IoResult Conn::write_all(const char* data, std::size_t n, std::int64_t deadline_us, const std::function<void()>& idle) {
  IoResult res;
  if (fd_ < 0) {
    res.why = "closed";
    return res;
  }
  std::size_t off = 0;
  while (off < n) {
    short wait_for = 0;
    if (ssl_) {
      ERR_clear_error();
      const int r = SSL_write(ssl_, data + off, static_cast<int>(std::min<std::size_t>(n - off, 1 << 30)));
      if (r > 0) {
        off += static_cast<std::size_t>(r);
        continue;
      }
      const int e = SSL_get_error(ssl_, r);
      if (e == SSL_ERROR_WANT_WRITE) {
        wait_for = POLLOUT;
      } else if (e == SSL_ERROR_WANT_READ) {
        wait_for = POLLIN;
      } else {
        broken_ = e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL;
        const int saved = errno;
        res.status = e == SSL_ERROR_ZERO_RETURN ? IoStatus::CLOSED : IoStatus::ERROR;
        const std::string tls = ssl_errors();
        res.why = "TLS write failed after " + std::to_string(off) + " of " + std::to_string(n) + " bytes: " +
                  (tls.empty() ? std::string(std::strerror(saved)) : tls);
        res.n = off;
        return res;
      }
    } else {
      const ssize_t r = ::send(fd_, data + off, n - off, 0);
      if (r > 0) {
        off += static_cast<std::size_t>(r);
        continue;
      }
      if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
        wait_for = POLLOUT;
      } else {
        res.status = (errno == EPIPE) ? IoStatus::CLOSED : IoStatus::ERROR;
        res.why = "write failed after " + std::to_string(off) + " of " + std::to_string(n) + " bytes: " +
                  std::strerror(errno);
        res.n = off;
        return res;
      }
    }
    const int left = remaining_ms(deadline_us);
    if (left <= 0) {
      res.status = IoStatus::TIMEOUT;
      res.why = "write timed out after " + std::to_string(off) + " of " + std::to_string(n) + " bytes";
      res.n = off;
      return res;
    }
    const int w = wait_fd(fd_, wait_for, std::min(left, kMaxSliceMs));
    if (w == 0 && idle) idle();
    if (w < 0) {
      res.why = std::string("poll: ") + std::strerror(errno);
      res.n = off;
      return res;
    }
  }
  res.status = IoStatus::OK;
  res.n = n;
  return res;
}

IoResult Conn::read_some(char* out, std::size_t cap, int timeout_ms) {
  IoResult res;
  if (fd_ < 0) {
    res.status = IoStatus::ERROR;
    res.why = "closed";
    return res;
  }
  const std::int64_t deadline = mono_us() + static_cast<std::int64_t>(std::max(0, timeout_ms)) * 1000;
  for (;;) {
    short wait_for = POLLIN;
    if (ssl_) {
      ERR_clear_error();
      const int r = SSL_read(ssl_, out, static_cast<int>(std::min<std::size_t>(cap, 1 << 30)));
      if (r > 0) {
        res.status = IoStatus::OK;
        res.n = static_cast<std::size_t>(r);
        return res;
      }
      const int e = SSL_get_error(ssl_, r);
      if (e == SSL_ERROR_ZERO_RETURN) {
        res.status = IoStatus::CLOSED;
        res.why = "close_notify";
        return res;
      }
      if (e == SSL_ERROR_WANT_WRITE) {
        wait_for = POLLOUT;
      } else if (e != SSL_ERROR_WANT_READ) {
        const unsigned long top = ERR_peek_error();
        const int saved = errno;
        broken_ = true;
        // OpenSSL 3 reports a FIN without close_notify as SSL_R_UNEXPECTED_EOF_WHILE_READING;
        // a SYSCALL error with errno 0 is the same thing from older code paths
        if ((e == SSL_ERROR_SSL && ERR_GET_REASON(top) == SSL_R_UNEXPECTED_EOF_WHILE_READING) ||
            (e == SSL_ERROR_SYSCALL && saved == 0)) {
          ERR_clear_error();
          res.status = IoStatus::CLOSED_UNCLEAN;
          res.why = "EOF without close_notify";
          return res;
        }
        const std::string tls = ssl_errors();
        res.status = IoStatus::ERROR;
        res.why = "TLS read failed: " + (tls.empty() ? std::string(std::strerror(saved)) : tls);
        return res;
      }
    } else {
      const ssize_t r = ::recv(fd_, out, cap, 0);
      if (r > 0) {
        res.status = IoStatus::OK;
        res.n = static_cast<std::size_t>(r);
        return res;
      }
      if (r == 0) {
        res.status = IoStatus::CLOSED;
        res.why = "EOF";
        return res;
      }
      if (errno != EAGAIN && errno != EINTR) {
        res.status = IoStatus::ERROR;
        res.why = std::string("read failed: ") + std::strerror(errno);
        return res;
      }
    }
    const int left = remaining_ms(deadline);
    if (left <= 0) {
      res.status = IoStatus::TIMEOUT;
      return res;
    }
    const int w = wait_fd(fd_, wait_for, left);
    if (w == 0) {
      res.status = IoStatus::TIMEOUT;
      return res;
    }
    if (w < 0) {
      res.status = IoStatus::ERROR;
      res.why = std::string("poll: ") + std::strerror(errno);
      return res;
    }
  }
}

bool Conn::usable(std::string* why) {
  auto no = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (fd_ < 0) return no("closed");
  char b[1];
  if (ssl_) {
    // reads what is there without waiting: session tickets are taken in, data or an EOF is not
    const IoResult r = read_some(b, 1, 0);
    if (r.status == IoStatus::OK) return no("bytes arrived unasked");
    if (r.status != IoStatus::TIMEOUT) return no(std::string(io_name(r.status)) + (r.why.empty() ? "" : " " + r.why));
  } else {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, 0) > 0) {
      if (p.revents & (POLLERR | POLLNVAL)) return no("socket error");
      const ssize_t r = ::recv(fd_, b, 1, MSG_PEEK);
      if (r == 0) return no("EOF");
      if (r > 0) return no("bytes arrived unasked");
      if (errno != EAGAIN && errno != EINTR) return no(std::string("read failed: ") + std::strerror(errno));
    }
  }
  int err = 0;
  socklen_t len = sizeof err;
  if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) return no(std::strerror(err));
  return true;
}

// ---- ConnFactory ------------------------------------------------------------------------------

SSL_CTX* ConnFactory::make_ctx(const std::string& ca_file, bool system_store, std::string* err) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    *err = "SSL_CTX_new: " + ssl_errors();
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
  static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn);   // 0 on success; failure only costs ALPN
  if (opt_.resume_sessions) {
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(ctx, &ConnFactory::on_new_session);
  } else {
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
  }
  if (!ca_file.empty()) {
    if (SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) != 1) {
      *err = "cannot load CA file " + ca_file + ": " + ssl_errors();
      SSL_CTX_free(ctx);
      return nullptr;
    }
  } else if (system_store) {
    SSL_CTX_set_default_verify_paths(ctx);
    ERR_clear_error();   // a missing default directory is not an error by itself
    if (count_roots(ctx) == 0) SSL_CTX_load_verify_locations(ctx, "/etc/ssl/cert.pem", nullptr);
    ERR_clear_error();
  }
  return ctx;
}

ConnFactory::ConnFactory(const SendGuard& guard, FactoryOptions opt)
    : guard_(guard), opt_(std::move(opt)), cache_(std::make_shared<SessionCache>()) {
  OPENSSL_init_ssl(0, nullptr);
  conn_index();
  venue_ctx_ = make_ctx(opt_.system_ca_file_override, true, &err_);
  if (venue_ctx_) {
    roots_ = count_roots(venue_ctx_);
    if (roots_ == 0) err_ = "no trust roots: neither OpenSSL's default store nor /etc/ssl/cert.pem loaded";
  }
  if (err_.empty() && !opt_.loopback_ca_file.empty()) loopback_ctx_ = make_ctx(opt_.loopback_ca_file, false, &err_);
}

ConnFactory::~ConnFactory() {
  // A connection holds its own reference to its SSL_CTX (SSL_new), so it may outlive this.
  if (venue_ctx_) SSL_CTX_free(venue_ctx_);
  if (loopback_ctx_) SSL_CTX_free(loopback_ctx_);
}

bool ConnFactory::ok(std::string* why) const {
  if (why) *why = err_;
  return err_.empty() && venue_ctx_ != nullptr;
}

FactoryStats ConnFactory::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

std::unique_ptr<Conn> ConnFactory::open(const Endpoint& ep, int timeout_ms, std::string* why) {
  auto refuse = [&](const std::string& w) -> std::unique_ptr<Conn> {
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++stats_.refused;
    }
    if (why) *why = w;
    return nullptr;
  };
  auto fail = [&](const std::string& w, bool handshake) -> std::unique_ptr<Conn> {
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++(handshake ? stats_.handshake_fail : stats_.connect_fail);
    }
    if (why) *why = w;
    return nullptr;
  };
  if (!ok()) return fail("no TLS context: " + err_, false);
  // the guard, on every connect: nothing below runs for a refused host, not even DNS
  const std::string no = guard_.refuse(ep.host);
  if (!no.empty()) return refuse(no);
  const HostClass cls = classify_host(ep.host);
  if (!ep.tls && cls != HostClass::LOOPBACK) return refuse("plain HTTP to " + ep.host + ": only loopback may skip TLS");

  const std::int64_t t0 = mono_us();
  const std::int64_t deadline = t0 + static_cast<std::int64_t>(std::max(1, timeout_ms)) * 1000;
  const std::string key = ep.host + ":" + std::to_string(ep.port);

  // addresses: cached for a minute, else getaddrinfo (which can block on a dead resolver; the
  // pool resolves in its own thread, so only a pool miss with a cold cache waits on it)
  std::vector<std::vector<unsigned char>> addrs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = dns_.find(key);
    if (it != dns_.end() && t0 - it->second.at_us < kDnsTtlUs) addrs = it->second.addrs;
  }
  if (addrs.empty()) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const int g = ::getaddrinfo(ep.host.c_str(), std::to_string(ep.port).c_str(), &hints, &res);
    if (g != 0) return fail("resolve " + ep.host + ": " + gai_strerror(g), false);
    for (addrinfo* a = res; a; a = a->ai_next) {
      // a loopback name must stay on loopback: an /etc/hosts line cannot send "localhost" out
      if (cls == HostClass::LOOPBACK && !is_loopback_addr(a->ai_addr)) continue;
      const auto* p = reinterpret_cast<const unsigned char*>(a->ai_addr);
      addrs.emplace_back(p, p + a->ai_addrlen);
    }
    ::freeaddrinfo(res);
    if (addrs.empty()) return refuse(ep.host + " resolved to no loopback address");
    std::lock_guard<std::mutex> lk(mu_);
    dns_[key] = Resolved{t0, addrs};
  }

  auto c = std::unique_ptr<Conn>(new Conn());
  c->ep_ = ep;
  std::string last_err = "no address";
  for (const auto& a : addrs) {
    const auto* sa = reinterpret_cast<const sockaddr*>(a.data());
    const int fd = ::socket(sa->sa_family, SOCK_STREAM, 0);
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++stats_.sockets;
    }
    if (fd < 0) {
      last_err = std::string("socket: ") + std::strerror(errno);
      continue;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = ::connect(fd, sa, static_cast<socklen_t>(a.size()));
    if (r != 0 && errno == EINPROGRESS) {
      const int w = wait_fd(fd, POLLOUT, std::max(1, remaining_ms(deadline)));
      int err = 0;
      socklen_t len = sizeof err;
      if (w <= 0) {
        err = w == 0 ? ETIMEDOUT : errno;
      } else {
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      }
      r = err == 0 ? 0 : -1;
      errno = err;
    }
    if (r != 0) {
      last_err = "connect " + addr_str(sa) + ": " + std::strerror(errno);
      ::close(fd);
      if (remaining_ms(deadline) <= 0) break;
      continue;
    }
    c->fd_ = fd;
    c->peer_ = addr_str(sa);
    break;
  }
  if (c->fd_ < 0) {
    std::lock_guard<std::mutex> lk(mu_);
    dns_.erase(key);   // the next open looks the name up again
    ++stats_.connect_fail;
    if (why) *why = last_err;
    return nullptr;
  }
  const std::int64_t t_tcp = mono_us();
  c->connect_ms_ = (t_tcp - t0) / 1000.0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.connects;
  }
  if (!ep.tls) {
    c->opened_us_ = t_tcp;
    return c;
  }

  SSL_CTX* ctx = (cls == HostClass::LOOPBACK && loopback_ctx_) ? loopback_ctx_ : venue_ctx_;
  SSL* ssl = SSL_new(ctx);
  if (!ssl) return fail("SSL_new: " + ssl_errors(), true);
  c->ssl_ = ssl;
  c->cache_ = cache_;
  c->cache_key_ = key;
  SSL_set_ex_data(ssl, conn_index(), c.get());
  SSL_set_fd(ssl, c->fd_);
  X509_VERIFY_PARAM* vp = SSL_get0_param(ssl);
  if (is_ip_literal(ep.host)) {
    // no SNI for an address (RFC 6066 3); the certificate must name the address itself
    X509_VERIFY_PARAM_set1_ip_asc(vp, ep.host.c_str());
  } else {
    SSL_set_tlsext_host_name(ssl, ep.host.c_str());
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(ssl, ep.host.c_str()) != 1) return fail("SSL_set1_host: " + ssl_errors(), true);
  }
  if (opt_.resume_sessions) {
    if (SSL_SESSION* s = cache_->get(key)) {
      SSL_set_session(ssl, s);
      SSL_SESSION_free(s);
    }
  }
  for (;;) {
    ERR_clear_error();
    const int r = SSL_connect(ssl);
    if (r == 1) break;
    const int e = SSL_get_error(ssl, r);
    short ev = 0;
    if (e == SSL_ERROR_WANT_READ) ev = POLLIN;
    else if (e == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
    if (ev == 0) {
      c->broken_ = true;
      const long vr = SSL_get_verify_result(ssl);
      std::string w = "TLS handshake with " + ep.host + " failed";
      if (vr != X509_V_OK) w += ": certificate not trusted (" + std::string(X509_verify_cert_error_string(vr)) + ")";
      const std::string tls = ssl_errors();
      if (!tls.empty()) w += ": " + tls;
      else if (e == SSL_ERROR_SYSCALL) w += std::string(": ") + (errno ? std::strerror(errno) : "EOF");
      return fail(w, true);
    }
    const int left = remaining_ms(deadline);
    if (left <= 0 || wait_fd(c->fd_, ev, left) <= 0) {
      c->broken_ = true;
      return fail("TLS handshake with " + ep.host + " timed out", true);
    }
  }
  // SSL_VERIFY_PEER already failed the handshake on a bad chain or name; this is belt and braces
  if (SSL_get_verify_result(ssl) != X509_V_OK || SSL_get0_peer_certificate(ssl) == nullptr) {
    c->broken_ = true;
    return fail("TLS peer of " + ep.host + " not verified", true);
  }
  const std::int64_t t_tls = mono_us();
  c->handshake_ms_ = (t_tls - t_tcp) / 1000.0;
  c->opened_us_ = t_tls;
  c->tls_version_ = SSL_get_version(ssl);
  c->cipher_ = SSL_get_cipher_name(ssl);
  c->resumed_ = SSL_session_reused(ssl) == 1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.handshakes;
    if (c->resumed_) ++stats_.resumed;
  }
  // TLS 1.2 hands its session over at the end of the handshake; TLS 1.3's tickets come later,
  // through on_new_session
  if (opt_.resume_sessions && SSL_version(ssl) < TLS1_3_VERSION) {
    if (SSL_SESSION* s = SSL_get1_session(ssl)) cache_->put(key, s);
  }
  return c;
}

// TLS 1.3 sends its session tickets after the handshake; OpenSSL hands each one here from
// inside a later SSL_read (a pooled connection's usable() check, or the reply's read), on
// whichever thread reads. Returning 1 keeps the reference.
int ConnFactory::on_new_session(SSL* ssl, SSL_SESSION* sess) {
  auto* c = static_cast<Conn*>(SSL_get_ex_data(ssl, conn_index()));
  if (!c || !c->cache_) return 0;
  c->cache_->put(c->cache_key_, sess);
  return 1;
}

}  // namespace deskd::net
