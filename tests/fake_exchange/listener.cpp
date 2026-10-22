#include "listener.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

#include "net.h"

namespace fakex {

namespace {
std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

Listener::Listener(ListenerConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.tls_cert_file.empty() && cfg_.tls_key_file.empty()) return;
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (ctx && SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) == 1 &&
      SSL_CTX_use_certificate_chain_file(ctx, cfg_.tls_cert_file.c_str()) == 1 &&
      SSL_CTX_use_PrivateKey_file(ctx, cfg_.tls_key_file.c_str(), SSL_FILETYPE_PEM) == 1 &&
      SSL_CTX_check_private_key(ctx) == 1) {
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    ctx_ = ctx;
  } else {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
    tls_err_ = "TLS listener: cannot load " + cfg_.tls_cert_file + " / " + cfg_.tls_key_file + ": " + buf;
    ERR_clear_error();
    if (ctx) SSL_CTX_free(ctx);
  }
}

Listener::~Listener() {
  stop();
  if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
}

bool Listener::start(Serve serve, Log log, std::string* err) {
  if (running_) return true;
  if (!tls_err_.empty()) {
    if (err) *err = tls_err_;
    return false;
  }
  listen_fd_ = net::listen_loopback(cfg_.port, &port_, err);
  if (listen_fd_ < 0) return false;
  serve_ = std::move(serve);
  log_ = std::move(log);
  stopping_ = false;
  running_ = true;
  acceptor_ = std::thread([this] { accept_loop(); });
  return true;
}

void Listener::stop() {
  if (!running_) return;
  stopping_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    for (const auto& [fd, owned] : conn_fds_) {
      (void)owned;
      ::shutdown(fd, SHUT_RDWR);
    }
  }
  std::vector<Conn> conns;
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    conns.swap(conns_);
  }
  for (auto& c : conns)
    if (c.th.joinable()) c.th.join();
  ::close(listen_fd_);
  listen_fd_ = -1;
  running_ = false;
}

void Listener::accept_loop() {
  while (!stopping_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    ++accepted_;
    net::no_sigpipe(fd);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lk(conn_mu_);
    // reap the connections that have finished: a sender that writes once per connection opens
    // thousands an hour
    for (auto it = conns_.begin(); it != conns_.end();) {
      if (it->done->load()) {
        it->th.join();
        it = conns_.erase(it);
      } else {
        ++it;
      }
    }
    if (ctx_) {
      // the TLS socket belongs to its pump; the pump's socketpair end is what serve() reads. Until
      // the server side of the handshake ends the TCP socket is listed (kind 3), so a drop or a
      // stop landing between the client's Finished and the server's still reaches it.
      conn_fds_[fd] = 3;
      conns_.push_back(Conn{std::thread([this, fd, done] {
                              serve_tls(fd);
                              ::close(fd);
                              done->store(true);
                            }),
                            done});
      continue;
    }
    conn_fds_[fd] = 1;
    conns_.push_back(Conn{std::thread([this, fd, done] {
                            serve_(fd);
                            {
                              std::lock_guard<std::mutex> g(conn_mu_);
                              conn_fds_.erase(fd);  // before the close, so stop() never shuts a reused number
                            }
                            ::close(fd);
                            done->store(true);
                          }),
                          done});
  }
}

void Listener::drop_http() {
  // Under conn_mu_: a connection's thread takes its fd out of conn_fds_ before closing it, so a
  // number listed here is still that connection's.
  std::lock_guard<std::mutex> lk(conn_mu_);
  for (auto& [fd, kind] : conn_fds_) {
    if (kind == 1) ::shutdown(fd, SHUT_RDWR);
    if (kind == 3) kind = 4;   // a TLS handshake still ending: dropped as soon as it is served
  }
}

void Listener::upgraded(int fd) {
  std::lock_guard<std::mutex> lk(conn_mu_);
  conn_fds_[fd] = 2;
}

// One TLS connection: the handshake, then a pump between the TLS socket and one end of a
// socketpair while serve() reads the other end as it would a plain connection. Both sockets are
// non-blocking and the pump is the only thread that touches the SSL object.
void Listener::serve_tls(int tcp) {
  ::fcntl(tcp, F_SETFL, ::fcntl(tcp, F_GETFL, 0) | O_NONBLOCK);
  SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx_));
  if (!ssl) return;
  SSL_set_fd(ssl, tcp);
  const std::int64_t hs_deadline = steady_ms() + cfg_.tls_handshake_ms;
  for (;;) {
    ERR_clear_error();
    const int r = SSL_accept(ssl);
    if (r == 1) break;
    const int e = SSL_get_error(ssl, r);
    const short ev = e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
    const std::int64_t left = hs_deadline - steady_ms();
    pollfd p{tcp, ev, 0};
    if (ev == 0 || left <= 0 || stopping_ || ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(left, 50))) < 0) {
      ++tls_failed_;
      log_("tls handshake-failed");
      ERR_clear_error();
      SSL_free(ssl);
      std::lock_guard<std::mutex> lk(conn_mu_);
      conn_fds_.erase(tcp);   // before the caller's close, so stop() never shuts a reused number
      return;
    }
  }
  ++tls_ok_;
  if (SSL_session_reused(ssl)) ++tls_resumed_;
  int sp[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
    SSL_free(ssl);
    std::lock_guard<std::mutex> lk(conn_mu_);
    conn_fds_.erase(tcp);
    return;
  }
  const int inner = sp[0], srv_fd = sp[1];
  net::no_sigpipe(inner);
  net::no_sigpipe(srv_fd);
  ::fcntl(inner, F_SETFL, ::fcntl(inner, F_GETFL, 0) | O_NONBLOCK);
  bool dropped = false;   // a drop that reached the TCP socket during the handshake
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    dropped = conn_fds_.count(tcp) && conn_fds_[tcp] == 4;
    conn_fds_.erase(tcp);
    conn_fds_[srv_fd] = 1;
  }
  if (dropped) ::shutdown(srv_fd, SHUT_RDWR);
  std::thread srv([this, srv_fd] {
    serve_(srv_fd);
    {
      std::lock_guard<std::mutex> g(conn_mu_);
      conn_fds_.erase(srv_fd);
    }
    ::close(srv_fd);
  });

  std::string to_inner, to_tls;
  bool client_done = false;   // close_notify or EOF from the client: nothing more will come
  bool tls_dead = false;      // a reset or a TLS error: nothing more can go out either
  bool inner_eof = false;     // serve() closed its end: the reply is all in to_tls
  bool inner_wr_shut = false;
  char buf[16384];
  while (!stopping_) {
    while (!client_done) {
      ERR_clear_error();
      const int r = SSL_read(ssl, buf, sizeof buf);
      if (r > 0) {
        to_inner.append(buf, static_cast<std::size_t>(r));
        continue;
      }
      const int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
      client_done = true;
      if (e != SSL_ERROR_ZERO_RETURN) tls_dead = true;
      ERR_clear_error();
    }
    while (!to_inner.empty()) {
      const ssize_t n = ::send(inner, to_inner.data(), to_inner.size(), 0);
      if (n > 0) {
        to_inner.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      to_inner.clear();   // serve() is gone
      break;
    }
    if (client_done && to_inner.empty() && !inner_wr_shut) {
      ::shutdown(inner, SHUT_WR);   // serve() sees the client's close
      inner_wr_shut = true;
    }
    while (!inner_eof && to_tls.size() < (1u << 20)) {
      const ssize_t n = ::recv(inner, buf, sizeof buf, 0);
      if (n > 0) {
        to_tls.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      inner_eof = true;
    }
    while (!to_tls.empty() && !tls_dead) {
      ERR_clear_error();
      const int r = SSL_write(ssl, to_tls.data(), static_cast<int>(to_tls.size()));
      if (r > 0) {
        to_tls.erase(0, static_cast<std::size_t>(r));
        continue;
      }
      const int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) break;
      tls_dead = true;
      ERR_clear_error();
    }
    if (tls_dead) to_tls.clear();
    if (inner_eof && to_tls.empty()) break;   // the reply is out: close the TLS side
    if (tls_dead && client_done) break;
    pollfd p[2] = {{tcp, 0, 0}, {inner, 0, 0}};
    if (!client_done) p[0].events |= POLLIN;
    if (!to_tls.empty()) p[0].events |= POLLOUT;
    if (!inner_eof && to_tls.size() < (1u << 20)) p[1].events |= POLLIN;
    if (!to_inner.empty()) p[1].events |= POLLOUT;
    ::poll(p, 2, 50);
  }
  if (!tls_dead && cfg_.tls_close_notify) {
    ERR_clear_error();
    SSL_shutdown(ssl);   // close_notify; the peer's is not waited for
  }
  ERR_clear_error();
  ::shutdown(tcp, SHUT_RDWR);
  ::shutdown(inner, SHUT_RDWR);   // serve() ends if it has not
  srv.join();
  ::close(inner);
  SSL_free(ssl);
}

}  // namespace fakex
