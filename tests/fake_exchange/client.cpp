#include "client.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

namespace fakex {

Headers kalshi_auth(const std::string& method, const std::string& path, std::int64_t ts_ms, const SignFn& sign) {
  const std::string ts = std::to_string(ts_ms);
  return {{"KALSHI-ACCESS-KEY", "fake-key-id"},
          {"KALSHI-ACCESS-TIMESTAMP", ts},
          {"KALSHI-ACCESS-SIGNATURE", sign ? sign(ts + method + path) : std::string("c2lnbmF0dXJl")}};
}

HttpConn::HttpConn(int port, int timeout_ms) : port_(port), fd_(net::connect_loopback(port, timeout_ms)) {}
HttpConn::~HttpConn() { close(); }

void HttpConn::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  buf_.clear();
}

HttpResult HttpConn::request(const std::string& method, const std::string& target, const std::string& body,
                             const Headers& headers, int timeout_ms) {
  HttpResult out;
  if (fd_ < 0) return out;
  std::string req = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port_) + "\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") {
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n" + body;
  const auto t0 = std::chrono::steady_clock::now();
  if (!net::write_all(fd_, req)) {
    out.rr = net::ReadResult::CLOSED;
    out.dropped = true;
    return out;
  }
  const std::size_t before = buf_.size();
  out.rr = net::read_response(fd_, buf_, out.res, timeout_ms, method == "HEAD");
  out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (out.rr == net::ReadResult::CLOSED) {
    if (out.res.status == 0 && before == 0 && buf_.empty()) out.dropped = true;
    if (out.res.status != 0 && !out.res.complete) out.cut = true;
    if (out.res.status == 0 && !buf_.empty()) out.cut = true;  // a head cut short
  }
  out.j = json::parse(out.res.body, nullptr, false);
  if (out.rr == net::ReadResult::CLOSED || net::lower(out.res.headers["connection"]) == "close") close();
  return out;
}

bool HttpConn::closed_by_server(int wait_ms) {
  if (fd_ < 0) return true;
  pollfd p{fd_, POLLIN, 0};
  if (::poll(&p, 1, wait_ms) <= 0) return false;
  char c;
  return ::recv(fd_, &c, 1, MSG_PEEK) == 0;
}

WsConn::WsConn(int port, const Headers& headers, const std::string& path) {
  fd_ = net::connect_loopback(port, 2000);
  if (fd_ < 0) return;
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";  // RFC 6455's sample nonce
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                    "\r\nSec-WebSocket-Version: 13\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  req += "\r\n";
  if (!net::write_all(fd_, req)) return;
  net::Response res;
  const net::ReadResult rr = net::read_response(fd_, rd_.buf, res, 3000, true);
  status_ = res.status;
  upgraded_ = rr == net::ReadResult::OK && res.status == 101 &&
              res.headers["sec-websocket-accept"] == net::ws_accept_key(key);
}

WsConn::~WsConn() { close(); }

void WsConn::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool WsConn::send_frame(int opcode, const std::string& payload) {
  if (fd_ < 0) return false;
  mask_ = mask_ * 1664525u + 1013904223u;
  return net::write_all(fd_, net::ws_frame(opcode, payload, true, true, mask_));
}
bool WsConn::send_text(const std::string& text) { return send_frame(net::TEXT, text); }

std::optional<json> WsConn::next(int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (fd_ >= 0 && !closed_) {
    const int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (left <= 0) return std::nullopt;
    net::WsMessage m;
    const net::ReadResult r = net::ws_read(fd_, rd_, m, false, left);
    if (r == net::ReadResult::TIMEOUT) return std::nullopt;
    if (r != net::ReadResult::OK) {
      closed_ = true;
      return std::nullopt;
    }
    if (m.opcode == net::PING) {
      ++pings_;
      send_frame(net::PONG, m.payload);
      continue;
    }
    if (m.opcode == net::PONG) {
      ++pongs_;
      continue;
    }
    if (m.opcode == net::CLOSE) {
      ++close_frames_;
      closed_ = true;
      return std::nullopt;
    }
    if (m.opcode != net::TEXT) continue;
    last_ = m;
    return json::parse(m.payload, nullptr, false);
  }
  return std::nullopt;
}

std::optional<json> WsConn::next_where(const std::function<bool(const json&)>& pred, int timeout_ms,
                                       std::vector<json>* skipped) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (left <= 0) return std::nullopt;
    auto m = next(left);
    if (!m) return std::nullopt;
    if (pred(*m)) return m;
    if (skipped) skipped->push_back(std::move(*m));
  }
}

std::vector<json> WsConn::drain(int quiet_ms) {
  std::vector<json> out;
  while (auto m = next(quiet_ms)) out.push_back(std::move(*m));
  return out;
}

}  // namespace fakex
