#include "desk/ws_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>

#include "desk/auth.h"
#include "desk/log.h"

namespace desk {
namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Url {
  bool tls = true;
  std::string host, path = "/";
  int port = 443;
};

bool parse_url(const std::string& url, Url* out) {
  Url u;
  std::string rest;
  if (url.rfind("wss://", 0) == 0) { u.tls = true; u.port = 443; rest = url.substr(6); }
  else if (url.rfind("ws://", 0) == 0) { u.tls = false; u.port = 80; rest = url.substr(5); }
  else return false;
  const auto slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  u.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const auto colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
    u.host = hostport.substr(0, colon);
    u.port = std::atoi(hostport.c_str() + colon + 1);
  } else {
    u.host = hostport;
  }
  if (u.host.empty() || u.port <= 0) return false;
  *out = u;
  return true;
}

}  // namespace

// ---- the two pieces worth testing on their own -------------------------------------------------
std::string WsClient::handshake_accept(const std::string& key_b64) {
  const std::string material = key_b64 + kGuid;
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
  return Signer::base64(digest, sizeof digest);
}

std::string WsClient::encode_frame(const std::string& payload, int opcode, bool mask) {
  std::string out;
  out.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));   // FIN, no extensions
  const size_t n = payload.size();
  const unsigned char mask_bit = mask ? 0x80 : 0x00;
  if (n < 126) {
    out.push_back(static_cast<char>(mask_bit | n));
  } else if (n <= 0xffff) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((n >> 8) & 0xff));
    out.push_back(static_cast<char>(n & 0xff));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<unsigned long long>(n) >> (8 * i)) & 0xff));
  }
  if (!mask) {
    out += payload;
    return out;
  }
  unsigned char key[4];
  if (RAND_bytes(key, 4) != 1) {
    static std::mt19937 rng{std::random_device{}()};
    for (unsigned char& k : key) k = static_cast<unsigned char>(rng() & 0xff);
  }
  out.append(reinterpret_cast<char*>(key), 4);
  for (size_t i = 0; i < n; ++i) out.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
  return out;
}

// No message this desk subscribes to is anywhere near this large. A frame that claims to be
// bigger is a malformed or hostile stream, and the honest response is to drop the connection
// rather than allocate whatever it asked for and wait 45 seconds for the stale timer.
constexpr unsigned long long kMaxFrameBytes = 8ull * 1024 * 1024;

bool WsClient::decode_frame(std::string& buf, int* opcode, std::string* payload, bool* fin) {
  if (buf.size() < 2) return false;
  const unsigned char b0 = static_cast<unsigned char>(buf[0]), b1 = static_cast<unsigned char>(buf[1]);
  const bool masked = (b1 & 0x80) != 0;
  size_t len = b1 & 0x7f, off = 2;
  if (len == 126) {
    if (buf.size() < 4) return false;
    len = (static_cast<unsigned char>(buf[2]) << 8) | static_cast<unsigned char>(buf[3]);
    off = 4;
  } else if (len == 127) {
    if (buf.size() < 10) return false;
    // RFC 6455: the most significant bit of a 64-bit length must be 0. Taking it on trust lets
    // off + mask_len + len wrap around, which makes "have we got the whole frame" true for a
    // frame we have not got, and then erases the wrong number of bytes and desynchronises every
    // frame after it.
    if (static_cast<unsigned char>(buf[2]) & 0x80) { buf.clear(); return false; }
    unsigned long long big = 0;
    for (int i = 0; i < 8; ++i) big = (big << 8) | static_cast<unsigned char>(buf[2 + i]);
    if (big > kMaxFrameBytes) { buf.clear(); return false; }
    len = static_cast<size_t>(big);
    off = 10;
  }
  const size_t mask_len = masked ? 4 : 0;
  if (len > kMaxFrameBytes) { buf.clear(); return false; }
  if (buf.size() < off + mask_len + len) return false;
  std::string out = buf.substr(off + mask_len, len);
  if (masked) {
    const char* key = buf.data() + off;
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<char>(out[i] ^ key[i % 4]);
  }
  if (opcode) *opcode = b0 & 0x0f;
  if (fin) *fin = (b0 & 0x80) != 0;
  if (payload) *payload = std::move(out);
  buf.erase(0, off + mask_len + len);
  return true;
}

// ---- lifecycle ---------------------------------------------------------------------------------
WsClient::WsClient(Options options, OnText on_text, OnEvent on_event)
    : opt_(std::move(options)), on_text_(std::move(on_text)), on_event_(std::move(on_event)) {}

WsClient::~WsClient() { stop(); }

void WsClient::start() {
  if (running_.exchange(true)) return;
  stopping_ = false;
  // run() returns only when stopping. A callback that throws must not end the process (an
  // exception leaving a thread is std::terminate): log it, drop the socket, and go round again.
  thread_ = std::thread([this] {
    while (!stopping_.load()) {
      try {
        run();
      } catch (const std::exception& e) {
        LOG_ERR("%s: feed thread error: %s; reconnecting", opt_.name.c_str(), e.what());
      } catch (...) {
        LOG_ERR("%s: feed thread error (unknown); reconnecting", opt_.name.c_str());
      }
      if (stopping_.load()) break;
      try { close_socket(); } catch (...) {}
      // The connection is gone as surely as after any other failure, and a caller that tracks
      // connections has to hear it before the next one opens.
      try { if (on_event_) on_event_("disconnected", "feed thread error"); } catch (...) {}
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });
}

void WsClient::stop() {
  if (!running_.load()) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  stopping_ = true;
  // Break the read out of select() rather than waiting for its timeout.
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void WsClient::reconnect() {
  // Reads end (the loop sees the connection closed and reconnects); writes still reach the peer,
  // so a pong or a queued frame after this is harmless.
  std::lock_guard<std::mutex> lk(mu_);
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RD);
}

bool WsClient::send(const std::string& text) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!connected_.load()) return false;
  outbox_.push_back(text);
  return true;
}

json WsClient::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  const long long now = now_ms();
  const bool up = connected_.load();
  json j{{"name", opt_.name},
         {"url", opt_.url},
         {"running", running_.load()},
         {"connected", up},
         {"frames", frames_},
         {"bytes", bytes_},
         {"connects", connects_},
         {"reconnects", reconnects_},
         {"last_error", last_error_},
         {"last_message_ts_ms", last_message_ms_},
         {"connected_since_ts_ms", connected_since_ms_}};
  j["last_message_age_seconds"] = last_message_ms_ > 0 ? (now - last_message_ms_) / 1000.0 : -1.0;
  j["stale_after_seconds"] = opt_.stale_after_seconds;
  j["fresh"] = up && last_message_ms_ > 0 && (now - last_message_ms_) / 1000.0 <= opt_.stale_after_seconds;
  return j;
}

// ---- transport ---------------------------------------------------------------------------------
void WsClient::close_socket() {
  std::lock_guard<std::mutex> lk(mu_);
  if (ssl_) {
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
    ssl_ctx_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  connected_ = false;
  outbox_.clear();
}

bool WsClient::write_all(const char* data, size_t n) {
  size_t sent = 0;
  const long long deadline = now_ms() + 1000LL * std::max(2, opt_.connect_timeout_seconds);
  while (sent < n) {
    if (stopping_.load()) return false;
    if (now_ms() > deadline) return false;
    int rc;
    if (ssl_) rc = SSL_write(static_cast<SSL*>(ssl_), data + sent, static_cast<int>(n - sent));
    else rc = static_cast<int>(::send(fd_, data + sent, n - sent, MSG_NOSIGNAL));
    if (rc > 0) {
      sent += static_cast<size_t>(rc);
      continue;
    }
    if (ssl_) {
      const int err = SSL_get_error(static_cast<SSL*>(ssl_), rc);
      if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) continue;
      return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return false;
  }
  return true;
}

int WsClient::read_some(char* data, size_t n) {
  // SSL can hold decrypted bytes that select() cannot see; ask it first.
  if (ssl_ && SSL_pending(static_cast<SSL*>(ssl_)) > 0) {
    const int rc = SSL_read(static_cast<SSL*>(ssl_), data, static_cast<int>(n));
    return rc > 0 ? rc : -1;
  }
  fd_set r;
  FD_ZERO(&r);
  FD_SET(fd_, &r);
  timeval tv{0, 200 * 1000};
  const int ready = ::select(fd_ + 1, &r, nullptr, nullptr, &tv);
  if (ready == 0) return 0;
  if (ready < 0) return errno == EINTR ? 0 : -1;
  int rc;
  if (ssl_) {
    rc = SSL_read(static_cast<SSL*>(ssl_), data, static_cast<int>(n));
    if (rc <= 0) {
      const int err = SSL_get_error(static_cast<SSL*>(ssl_), rc);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
      return -1;
    }
    return rc;
  }
  rc = static_cast<int>(::recv(fd_, data, n, 0));
  if (rc == 0) return -1;
  if (rc < 0) return (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  return rc;
}

bool WsClient::connect_once(std::string* why) {
  Url u;
  if (!parse_url(opt_.url, &u)) {
    *why = "unreadable url " + opt_.url;
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(u.port);
  const int rc = ::getaddrinfo(u.host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0 || !res) {
    *why = "cannot resolve " + u.host + ": " + gai_strerror(rc);
    return false;
  }
  int fd = -1;
  for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
    fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    timeval tv{opt_.connect_timeout_seconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
      ::close(fd);
      fd = -1;
      continue;
    }
  }
  ::freeaddrinfo(res);
  if (fd < 0) {
    *why = "cannot connect to " + u.host + ":" + port;
    return false;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  // A write to a socket the peer (or reconnect()) has closed must fail, not kill the process. The
  // plain path passes MSG_NOSIGNAL, but TLS writes go through OpenSSL's own write(2), and SIGPIPE's
  // default action ends the whole desk: a forced reconnect followed by a pong did, in a probe.
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  {
    std::lock_guard<std::mutex> lk(mu_);
    fd_ = fd;
  }

  if (u.tls) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
      *why = "cannot create a TLS context";
      return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (opt_.verify_tls) {
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
      SSL_CTX_set_default_verify_paths(ctx);
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, u.host.c_str());
    if (opt_.verify_tls) {
      X509_VERIFY_PARAM* p = SSL_get0_param(ssl);
      X509_VERIFY_PARAM_set_hostflags(p, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      X509_VERIFY_PARAM_set1_host(p, u.host.c_str(), 0);
    }
    if (SSL_connect(ssl) != 1) {
      const unsigned long e = ERR_get_error();
      char buf[256];
      ERR_error_string_n(e, buf, sizeof buf);
      *why = std::string("TLS handshake failed: ") + buf;
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    ssl_ctx_ = ctx;
    ssl_ = ssl;
  }

  unsigned char nonce[16];
  if (RAND_bytes(nonce, sizeof nonce) != 1) {
    static std::mt19937 rng{std::random_device{}()};
    for (unsigned char& c : nonce) c = static_cast<unsigned char>(rng() & 0xff);
  }
  const std::string key = Signer::base64(nonce, sizeof nonce);
  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
  if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) req += ":" + port;
  req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
         "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + opt_.user_agent + "\r\n";
  auto add = [&req](const std::map<std::string, std::string>& h) {
    for (const auto& [k, v] : h) req += k + ": " + v + "\r\n";
  };
  add(opt_.headers);
  if (opt_.auth_headers) add(opt_.auth_headers());   // rebuilt per attempt: it is time-stamped
  req += "\r\n";
  if (!write_all(req.data(), req.size())) {
    *why = "handshake write failed";
    return false;
  }

  std::string head;
  const long long deadline = now_ms() + 1000LL * opt_.connect_timeout_seconds;
  char buf[4096];
  while (head.find("\r\n\r\n") == std::string::npos) {
    if (stopping_.load()) return false;
    if (now_ms() > deadline) {
      *why = "handshake timed out";
      return false;
    }
    const int n = read_some(buf, sizeof buf);
    if (n < 0) {
      *why = "connection closed during the handshake";
      return false;
    }
    head.append(buf, static_cast<size_t>(n));
    if (head.size() > 64 * 1024) {
      *why = "handshake response too large";
      return false;
    }
  }
  const size_t end = head.find("\r\n\r\n");
  std::string rest = head.substr(end + 4);
  head.resize(end);
  const std::string first = head.substr(0, head.find("\r\n"));
  if (first.find(" 101") == std::string::npos) {
    *why = "handshake refused: " + first;
    return false;
  }
  // The accept header proves the peer is a WebSocket server and not a proxy that said 101.
  std::string lower = head;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  const auto ap = lower.find("sec-websocket-accept:");
  if (ap == std::string::npos) {
    *why = "handshake had no accept header";
    return false;
  }
  size_t vs = head.find(':', ap) + 1;
  while (vs < head.size() && (head[vs] == ' ' || head[vs] == '\t')) ++vs;
  const size_t ve = head.find("\r\n", vs);
  const std::string accept = head.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
  if (accept != handshake_accept(key)) {
    *why = "handshake accept value did not match";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    connected_ = true;
    const long long t = now_ms();
    connected_since_ms_ = t;
    last_message_ms_ = t;   // a fresh socket is not stale
    ++connects_;
    inbox_ = rest;          // bytes that arrived with the handshake response
  }
  return true;
}

// ---- the loop ----------------------------------------------------------------------------------
void WsClient::run() {
  double backoff = opt_.backoff_initial_seconds;
  std::mt19937 rng{std::random_device{}()};
  while (!stopping_.load()) {
    std::string why;
    {
      std::lock_guard<std::mutex> lk(mu_);
      last_connect_attempt_ms_ = now_ms();
      inbox_.clear();
    }
    const bool ok = connect_once(&why);
    if (!ok) {
      close_socket();
      {
        std::lock_guard<std::mutex> lk(mu_);
        last_error_ = why;
        ++reconnects_;
      }
      if (on_event_) on_event_("error", why);
      LOG_WARN("%s: %s; reconnecting in %.0fs", opt_.name.c_str(), why.c_str(), backoff);
      const double jitter = 0.5 + (rng() % 1000) / 1000.0;   // 0.5x to 1.5x
      const long long wait_until = now_ms() + static_cast<long long>(1000 * backoff * jitter);
      while (!stopping_.load() && now_ms() < wait_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      backoff = std::min(opt_.backoff_max_seconds, backoff * 2);
      continue;
    }
    const long long connected_at = now_ms();
    LOG_INFO("%s: connected to %s", opt_.name.c_str(), opt_.url.c_str());
    if (on_event_) on_event_("connected", opt_.url);
    if (opt_.on_connect)
      for (const auto& frame : opt_.on_connect()) {
        const std::string encoded = encode_frame(frame, 0x1, true);
        if (!write_all(encoded.data(), encoded.size())) break;
      }

    std::string in;
    {
      std::lock_guard<std::mutex> lk(mu_);
      in.swap(inbox_);
    }
    std::string assembled;
    int assembled_op = 0;
    long long next_ping = now_ms() + 1000LL * std::max(1, opt_.ping_interval_seconds);
    std::string disconnect_why = "closed by the peer";
    char buf[65536];
    while (!stopping_.load()) {
      // Anything the caller queued since the last pass.
      std::vector<std::string> out;
      {
        std::lock_guard<std::mutex> lk(mu_);
        out.swap(outbox_);
      }
      bool write_failed = false;
      for (const auto& text : out) {
        const std::string encoded = encode_frame(text, 0x1, true);
        if (!write_all(encoded.data(), encoded.size())) { write_failed = true; break; }
      }
      if (write_failed) { disconnect_why = "write failed"; break; }

      const long long now = now_ms();
      if (now >= next_ping) {
        const std::string ping = encode_frame("", 0x9, true);
        if (!write_all(ping.data(), ping.size())) { disconnect_why = "ping failed"; break; }
        next_ping = now + 1000LL * std::max(1, opt_.ping_interval_seconds);
      }
      long long since;
      {
        std::lock_guard<std::mutex> lk(mu_);
        since = now - last_message_ms_;
      }
      if (opt_.stale_after_seconds > 0 && since > 1000LL * opt_.stale_after_seconds) {
        disconnect_why = "no frame for " + std::to_string(since / 1000) + "s";
        if (on_event_) on_event_("stale", disconnect_why);
        break;
      }

      const int n = read_some(buf, sizeof buf);
      if (n < 0) { disconnect_why = "connection closed"; break; }
      if (n == 0) continue;
      in.append(buf, static_cast<size_t>(n));
      {
        std::lock_guard<std::mutex> lk(mu_);
        bytes_ += n;
      }
      int op = 0;
      bool fin = false;
      std::string payload;
      bool closed = false;
      while (decode_frame(in, &op, &payload, &fin)) {
        if (op == 0x8) { closed = true; disconnect_why = "close frame"; break; }
        // A control frame is the socket telling us it is alive, and the stale timer exists to
        // catch a socket that is not. Counting only data frames tears down a healthy but quiet
        // feed every stale interval - which for a commodity index outside its trading session is
        // every 45 seconds, all night, each teardown leaving a gap in the very observations the
        // volatility estimate is built from.
        if (op == 0x9 || op == 0xA) {
          {
            std::lock_guard<std::mutex> lk(mu_);
            last_message_ms_ = now_ms();
          }
          if (op == 0x9) {   // ping: answer with the same payload
            const std::string pong = encode_frame(payload, 0xA, true);
            if (!write_all(pong.data(), pong.size())) { closed = true; disconnect_why = "pong failed"; }
          }
          continue;
        }
        if (op == 0x0) {           // continuation
          assembled += payload;
        } else {
          assembled = payload;
          assembled_op = op;
        }
        if (!fin) continue;
        const long long recv = now_ms();
        {
          std::lock_guard<std::mutex> lk(mu_);
          last_message_ms_ = recv;
          ++frames_;
        }
        if (assembled_op == 0x1 && on_text_) {
          try {
            on_text_(assembled, recv);
          } catch (const std::exception& e) {
            LOG_WARN("%s: message handler: %s", opt_.name.c_str(), e.what());
          }
        }
        assembled.clear();
      }
      if (closed) break;
    }
    close_socket();
    // A connection that completed the handshake and was then closed is not a success to reset the
    // backoff on. A server that accepts the upgrade and hangs up - a stale signature, a rate
    // limit, a close frame on subscribe - would otherwise be reconnected to as fast as the
    // machine can open sockets, against an authenticated venue endpoint. Only a socket that
    // stayed up long enough to be real clears the backoff.
    const long long lived_ms = now_ms() - connected_at;
    if (lived_ms >= 1000LL * std::max(5, opt_.stale_after_seconds)) backoff = opt_.backoff_initial_seconds;
    if (!stopping_.load()) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        last_error_ = disconnect_why;
        ++reconnects_;
      }
      const double jitter = 0.5 + (rng() % 1000) / 1000.0;
      const long long wait_until = now_ms() + static_cast<long long>(1000 * backoff * jitter);
      LOG_WARN("%s: %s; reconnecting in %.0fs", opt_.name.c_str(), disconnect_why.c_str(), backoff);
      while (!stopping_.load() && now_ms() < wait_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      backoff = std::min(opt_.backoff_max_seconds, backoff * 2);
    }
    if (on_event_) on_event_("disconnected", disconnect_why);
  }
  close_socket();
  running_ = false;
}

}  // namespace desk
