#include "deskd/net/ws.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <limits>

#include "deskd/gw/kalshi_sign.h"

namespace deskd::net {
namespace {
bool utf8(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) continue;
    unsigned n = 0, cp = 0, min = 0;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; min = 0x10000; }
    else return false;
    if (s.size() - i < n) return false;
    while (n--) {
      c = static_cast<unsigned char>(s[i++]);
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}
bool token(const std::string& value, const std::string& wanted) {
  std::size_t pos = 0;
  while (pos < value.size()) {
    auto end = value.find(',', pos);
    auto part = value.substr(pos, end == std::string::npos ? end : end - pos);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return std::tolower(c); });
    if (part == wanted) return true;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return false;
}
}  // namespace

bool WsParser::fail(const char* why) { error_ = why; return false; }
bool WsParser::pop(WsEvent* event) {
  if (events_.empty()) return false;
  *event = std::move(events_.front()); events_.pop_front(); return true;
}
bool WsParser::feed(const char* bytes, std::size_t n) {
  if (!error_.empty()) return false;
  buffer_.append(bytes, n);
  while (buffer_.size() >= 2) {
    const auto a = static_cast<unsigned char>(buffer_[0]);
    const auto b = static_cast<unsigned char>(buffer_[1]);
    const bool fin = a & 0x80;
    const auto op = static_cast<WsOp>(a & 15);
    const bool control = (a & 15) >= 8;
    if ((a & 0x70) || (b & 0x80)) return fail("reserved bits or masked server frame");
    if (op != WsOp::CONT && op != WsOp::TEXT && op != WsOp::BINARY && op != WsOp::CLOSE &&
        op != WsOp::PING && op != WsOp::PONG) return fail("invalid frame opcode");
    std::uint64_t len = b & 127;
    std::size_t head = 2;
    if (len == 126) {
      if (buffer_.size() < 4) return true;
      len = static_cast<unsigned char>(buffer_[2]) * 256u + static_cast<unsigned char>(buffer_[3]);
      head = 4;
      if (len < 126) return fail("noncanonical frame length");
    } else if (len == 127) {
      if (buffer_.size() < 10) return true;
      if (static_cast<unsigned char>(buffer_[2]) & 0x80) return fail("invalid 64-bit frame length");
      len = 0;
      for (std::size_t i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buffer_[i]);
      head = 10;
      if (len <= 65535) return fail("noncanonical frame length");
    }
    if (control && (!fin || len > 125)) return fail("invalid control frame");
    if (!control && (len > limit_ || fragments_.size() > limit_ - static_cast<std::size_t>(len)))
      return fail("message exceeds limit");
    if (len > buffer_.size() - head) return true;
    std::string payload = buffer_.substr(head, static_cast<std::size_t>(len));
    buffer_.erase(0, head + static_cast<std::size_t>(len));
    if (control) {
      if (op == WsOp::CLOSE) {
        if (payload.size() == 1) return fail("invalid close body");
        if (payload.size() >= 2) {
          const unsigned code = static_cast<unsigned char>(payload[0]) * 256u + static_cast<unsigned char>(payload[1]);
          if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 ||
              code == 1015 || (code >= 1016 && code <= 2999) || !utf8(payload.substr(2)))
            return fail("invalid close code or reason");
        }
      }
      events_.push_back({op, std::move(payload)});
    } else if (op == WsOp::CONT) {
      if (fragment_op_ == WsOp::CONT) return fail("continuation without first fragment");
      fragments_ += payload;
      if (fin) {
        if (fragment_op_ == WsOp::TEXT && !utf8(fragments_)) return fail("invalid UTF-8 text");
        events_.push_back({fragment_op_, std::move(fragments_)});
        fragments_.clear(); fragment_op_ = WsOp::CONT;
      }
    } else {
      if (fragment_op_ != WsOp::CONT) return fail("new message inside fragmented message");
      if (fin) {
        if (op == WsOp::TEXT && !utf8(payload)) return fail("invalid UTF-8 text");
        events_.push_back({op, std::move(payload)});
      } else { fragment_op_ = op; fragments_ = std::move(payload); }
    }
    if (events_.size() > 16384) return fail("undrained frame queue exceeds limit");
  }
  return true;
}

std::string ws_accept(const std::string& key) {
  const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n = 0;
  if (EVP_Digest(input.data(), input.size(), digest, &n, EVP_sha1(), nullptr) != 1) return {};
  return base64_encode(digest, n);
}
std::string ws_client_frame(WsOp op, const std::string& payload) {
  const auto code = static_cast<unsigned char>(op);
  if ((code >= 8 && payload.size() > 125) || (code != 1 && code != 2 && code != 8 && code != 9 && code != 10)) return {};
  unsigned char mask[4];
  if (RAND_bytes(mask, sizeof mask) != 1) return {};
  std::string out(1, static_cast<char>(0x80 | code));
  if (payload.size() < 126) out.push_back(static_cast<char>(0x80 | payload.size()));
  else if (payload.size() <= 65535) {
    out.push_back(static_cast<char>(0xfe));
    out.push_back(static_cast<char>(payload.size() >> 8)); out.push_back(static_cast<char>(payload.size()));
  } else {
    out.push_back(static_cast<char>(0xff));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> (i * 8)));
  }
  out.append(reinterpret_cast<const char*>(mask), sizeof mask);
  const auto offset = out.size(); out += payload;
  for (std::size_t i = 0; i < payload.size(); ++i) out[offset + i] ^= static_cast<char>(mask[i % 4]);
  return out;
}

bool WsClient::connect(ConnFactory& factory, const Endpoint& endpoint, const HeaderList& auth,
                       int timeout_ms, std::string* why, const std::function<void()>& idle) {
  close(); parser_ = WsParser(limit_);
  auto fail = [&](const std::string& reason) { if (why) *why = reason; close(); return false; };
  const auto deadline = mono_us() + static_cast<std::int64_t>(timeout_ms) * 1000;
  conn_ = factory.open(endpoint, timeout_ms, why);
  if (!conn_) return false;
  unsigned char random[16];
  if (RAND_bytes(random, sizeof random) != 1) return fail("WebSocket random key failed");
  const auto key = base64_encode(random, sizeof random);
  const auto path = endpoint.base_path.empty() ? "/" : endpoint.base_path;
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + endpoint.host_header() +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n";
  for (const auto& h : auth) {
    if (h.first.find_first_of("\r\n:") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos)
      return fail("invalid upgrade header");
    req += h.first + ": " + h.second + "\r\n";
  }
  req += "\r\n";
  const auto sent = conn_->write_all(req.data(), req.size(), deadline, idle);
  if (sent.status != IoStatus::OK) return fail("WebSocket upgrade write failed");
  std::string head; char buf[16384];
  for (;;) {
    const auto boundary = head.find("\r\n\r\n");
    if (boundary != std::string::npos) {
      if (boundary > 65536) return fail("WebSocket upgrade head too large");
      if (head.compare(0, 13, "HTTP/1.1 101 ") != 0) return fail("WebSocket upgrade was not HTTP 101");
      // Reuse the strict HTTP parser's header validation; 204 has the same no-body framing as
      // 101, which REST deliberately refuses. Bytes beyond this header belong to the frames.
      auto parsed_head = head.substr(0, boundary + 4); parsed_head.replace(9, 3, "204");
      ResponseParser parser;
      if (parser.feed(parsed_head) != ResponseParser::Status::DONE) return fail("malformed WebSocket upgrade headers");
      const auto& res = parser.response();
      if (!token(res.header("upgrade"), "websocket") || !token(res.header("connection"), "upgrade") ||
          res.header("sec-websocket-accept") != ws_accept(key) || res.has_header("sec-websocket-extensions") ||
          res.has_header("sec-websocket-protocol")) return fail("WebSocket upgrade verification failed");
      if (!parser_.feed(head.substr(boundary + 4))) return fail(parser_.error());
      return true;
    }
    if (head.size() > 65536) return fail("WebSocket upgrade head too large");
    const auto left = (deadline - mono_us()) / 1000;
    if (left <= 0) return fail("WebSocket upgrade timeout");
    if (idle) idle();
    const auto r = conn_->read_some(buf, sizeof buf, static_cast<int>(std::min<std::int64_t>(250, left)));
    if (r.status == IoStatus::OK) head.append(buf, r.n);
    else if (r.status != IoStatus::TIMEOUT) return fail("WebSocket upgrade disconnected");
  }
}
bool WsClient::send(WsOp op, const std::string& payload, int timeout_ms, std::string* why,
                    const std::function<void()>& idle) {
  if (!connected()) { if (why) *why = "WebSocket is disconnected"; return false; }
  const auto bytes = ws_client_frame(op, payload);
  if (bytes.empty()) { if (why) *why = "invalid WebSocket frame or random mask failed"; return false; }
  const auto r = conn_->write_all(bytes.data(), bytes.size(), mono_us() + static_cast<std::int64_t>(timeout_ms) * 1000, idle);
  if (r.status == IoStatus::OK) return true;
  if (why) *why = "WebSocket write failed";
  close(); return false;
}
WsClient::Read WsClient::read(WsEvent* event, int timeout_ms, std::string* why) {
  if (!connected()) return Read::CLOSED;
  const auto deadline = mono_us() + static_cast<std::int64_t>(std::max(0, timeout_ms)) * 1000;
  bool first = true;
  for (;;) {
    if (parser_.pop(event)) {
      if (event->op == WsOp::PING && !send(WsOp::PONG, event->payload, 250, why)) return Read::ERROR;
      if (event->op == WsOp::CLOSE) { send(WsOp::CLOSE, event->payload, 100, nullptr); close(); return Read::CLOSED; }
      return Read::EVENT;
    }
    const auto left = (deadline - mono_us()) / 1000;
    if (!first && left <= 0) return Read::TIMEOUT;
    first = false;
    char buf[32768];
    const auto r = conn_->read_some(buf, sizeof buf, static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(250, left))));
    if (r.status == IoStatus::TIMEOUT) return Read::TIMEOUT;
    if (r.status != IoStatus::OK) { if (why) *why = "WebSocket disconnected"; close(); return Read::CLOSED; }
    if (!parser_.feed(buf, r.n)) { if (why) *why = parser_.error(); close(); return Read::ERROR; }
  }
}
void WsClient::close() { if (conn_) conn_->close(); conn_.reset(); }

}  // namespace deskd::net
