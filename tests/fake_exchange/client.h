#pragma once
// A raw client for the fake Kalshi's own tests: it sees what a socket sees (a reply cut short, a
// connection dropped before any byte, a chunked body, a fragmented frame), which a library client
// would hide. deskd's own sender and feeds are tested against the fake with their own code.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"
#include "net.h"

namespace fakex {

using Headers = std::vector<std::pair<std::string, std::string>>;
using SignFn = std::function<std::string(const std::string& text)>;

// KALSHI-ACCESS-KEY, -TIMESTAMP and -SIGNATURE for method + path (no query), signed by `sign` or
// carrying a placeholder signature (enough for AuthMode::PRESENCE).
Headers kalshi_auth(const std::string& method, const std::string& path, std::int64_t ts_ms, const SignFn& sign = nullptr);

struct HttpResult {
  net::ReadResult rr = net::ReadResult::BAD;
  net::Response res;
  json j;                 // the body parsed; discarded when it is not JSON
  bool dropped = false;   // the connection closed before any byte of a reply
  bool cut = false;       // a reply began and the connection closed before its end
  double ms = 0;          // request written to reply read
  int status() const { return res.status; }
};

class HttpConn {
 public:
  explicit HttpConn(int port, int timeout_ms = 2000);
  ~HttpConn();
  HttpConn(const HttpConn&) = delete;
  HttpConn& operator=(const HttpConn&) = delete;
  bool ok() const { return fd_ >= 0; }
  // One request on this connection (kept open between calls until the server closes it).
  HttpResult request(const std::string& method, const std::string& target, const std::string& body = "",
                     const Headers& headers = {}, int timeout_ms = 5000);
  bool closed_by_server(int wait_ms = 200);  // the server closed it (EOF within wait_ms)
  void close();

 private:
  int port_;
  int fd_ = -1;
  std::string buf_;
};

class WsConn {
 public:
  WsConn(int port, const Headers& headers, const std::string& path = "/trade-api/ws/v2");
  ~WsConn();
  WsConn(const WsConn&) = delete;
  WsConn& operator=(const WsConn&) = delete;
  bool ok() const { return fd_ >= 0 && upgraded_; }
  int handshake_status() const { return status_; }
  bool send_text(const std::string& text);
  bool send_json(const json& j) { return send_text(j.dump()); }
  bool send_frame(int opcode, const std::string& payload);
  // The next text message, parsed; nullopt on timeout or close. Pings are answered on the way.
  std::optional<json> next(int timeout_ms = 2000);
  // The next message for which pred holds; the ones before it go to *skipped.
  std::optional<json> next_where(const std::function<bool(const json&)>& pred, int timeout_ms = 2000,
                                 std::vector<json>* skipped = nullptr);
  // Everything until nothing arrives for quiet_ms.
  std::vector<json> drain(int quiet_ms = 150);
  bool closed() const { return closed_; }
  const net::WsMessage& last() const { return last_; }  // frames and largest frame of the last one
  int pings_seen() const { return pings_; }
  int pongs_seen() const { return pongs_; }
  // A close frame came before the end: a socket dropped under us shows closed() without one.
  bool close_frame_seen() const { return close_frames_ > 0; }
  void close();

 private:
  int fd_ = -1;
  bool upgraded_ = false, closed_ = false;
  int status_ = 0, pings_ = 0, pongs_ = 0, close_frames_ = 0;
  std::uint32_t mask_ = 0x5a17c3e1;
  net::WsReader rd_;
  net::WsMessage last_;
};

}  // namespace fakex
