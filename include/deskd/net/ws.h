#pragma once
// RFC 6455 on the same guarded, certificate-verified connection factory as REST. No alternate
// sockets, redirects, compression or implicit retries. All public methods belong to one owner.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "deskd/net/conn.h"
#include "deskd/net/http.h"

namespace deskd::net {

enum class WsOp : unsigned char { CONT = 0, TEXT = 1, BINARY = 2, CLOSE = 8, PING = 9, PONG = 10 };
struct WsEvent { WsOp op = WsOp::TEXT; std::string payload; };

// Server frames must be unmasked. One complete text/binary message is emitted after all its
// fragments; control frames may arrive between them. Bound each message, not a read buffer.
class WsParser {
 public:
  explicit WsParser(std::size_t max_message = 16 * 1024 * 1024) : limit_(max_message) {}
  bool feed(const char* bytes, std::size_t n);
  bool feed(const std::string& bytes) { return feed(bytes.data(), bytes.size()); }
  bool pop(WsEvent* event);
  const std::string& error() const { return error_; }
 private:
  bool fail(const char* why);
  std::size_t limit_;
  std::string buffer_, fragments_, error_;
  WsOp fragment_op_ = WsOp::CONT;
  std::deque<WsEvent> events_;
};

// Always uses a cryptographically random mask, including for ping, pong and close.
std::string ws_client_frame(WsOp op, const std::string& payload);
std::string ws_accept(const std::string& key);

class WsClient {
 public:
  enum class Read { EVENT, TIMEOUT, CLOSED, ERROR };
  explicit WsClient(std::size_t max_message = 16 * 1024 * 1024) : limit_(max_message), parser_(max_message) {}
  bool connect(ConnFactory& factory, const Endpoint& endpoint, const HeaderList& auth,
               int timeout_ms, std::string* why, const std::function<void()>& idle = nullptr);
  bool send(WsOp op, const std::string& payload, int timeout_ms, std::string* why,
            const std::function<void()>& idle = nullptr);
  // PING automatically gets a PONG before being returned. A quiet socket's caller can send
  // its own ping and require the matching pong: no market event is needed to prove liveness.
  Read read(WsEvent* event, int timeout_ms, std::string* why);
  void close();
  bool connected() const { return conn_ && !conn_->closed(); }
 private:
  std::size_t limit_;
  WsParser parser_;
  std::unique_ptr<Conn> conn_;
};

}  // namespace deskd::net
