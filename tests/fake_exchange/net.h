#pragma once
// Sockets for the fake Kalshi: just enough HTTP/1.1 and WebSocket (RFC 6455) to speak to deskd
// over loopback, written by hand so a fault can control every byte. cpp-httplib cannot drop a
// connection before its reply, cut a body after its headers, or send the same response chunked,
// and those are exactly the cases deskd's sender must get right (plan section 8: "a body cut off
// mid-way is UNKNOWN, never failed").
//
// Plain TCP on 127.0.0.1, no TLS: deskd's send guard (include/deskd/net/guard.h) allows loopback,
// and nothing here is ever reachable from another machine. Both ends use these functions: the
// server unmasked, the tests' client (client.h) masked, as the RFC requires of a client.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fakex::net {

struct Request {
  std::string method, target, path, query;     // path without the query string
  std::map<std::string, std::string> headers;  // names lower-cased
  std::map<std::string, std::string> params;   // the query, decoded
  std::string body;
  bool keep_alive = true;
  std::string header(const std::string& lower_name) const;
};

enum class ReadResult { OK, CLOSED, TIMEOUT, BAD };

// One request from fd. `buf` carries bytes already read (pipelining) between calls. Handles
// Content-Length and chunked request bodies. idle_ms bounds the wait for each read.
ReadResult read_request(int fd, std::string& buf, Request& req, int idle_ms, std::string* why);

struct Response {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-cased
  std::string body;
  bool chunked = false;    // the body came in chunks
  bool complete = false;   // every promised byte arrived
  std::size_t head_bytes = 0;
};
// One response (the tests' client). CLOSED with complete=false and some bytes read is a body cut
// mid-way; CLOSED with nothing read is a connection dropped before any reply.
ReadResult read_response(int fd, std::string& buf, Response& res, int timeout_ms, bool head_request = false);

const char* status_text(int status);
// "HTTP/1.1 <status> <text>\r\n" + headers + "\r\n". Content-Length is the caller's business.
std::string response_head(int status, const std::vector<std::pair<std::string, std::string>>& headers);
// The body as chunks of chunk_bytes (the last one shorter), then the terminating zero chunk.
std::string chunked_body(const std::string& body, std::size_t chunk_bytes);

bool write_all(int fd, const std::string& data);
void no_sigpipe(int fd);  // macOS has no MSG_NOSIGNAL
int listen_loopback(int port, int* bound_port, std::string* err);
int connect_loopback(int port, int timeout_ms);
std::string url_decode(const std::string& s);
std::map<std::string, std::string> parse_query(const std::string& q);
std::string lower(std::string s);

// ---- WebSocket ----------------------------------------------------------------------------------
enum Opcode : int { CONT = 0x0, TEXT = 0x1, BINARY = 0x2, CLOSE = 0x8, PING = 0x9, PONG = 0xA };

std::string ws_accept_key(const std::string& client_key);  // base64(SHA-1(key + RFC 6455 GUID))
std::string base64(const std::string& raw);
std::string unbase64(const std::string& b64);  // "" when it is not base64
// One frame. A client masks with mask_key; the server never masks.
std::string ws_frame(int opcode, const std::string& payload, bool fin = true, bool masked = false,
                     std::uint32_t mask_key = 0);
// A text message split into frames of at most frame_bytes payload each (0: one frame).
std::string ws_text(const std::string& payload, std::size_t frame_bytes);

struct WsMessage {
  int opcode = 0;
  std::string payload;
  int frames = 0;           // how many frames carried it
  std::size_t max_frame = 0;
};
struct WsReader {
  std::string buf;          // bytes read and not yet framed
  std::string partial;      // a fragmented message so far
  int partial_op = 0;
  int partial_frames = 0;
  std::size_t partial_max = 0;
};
// The next whole message: data messages reassembled from their continuation frames, control
// frames (ping, pong, close) as they come. BAD for a protocol error (an unmasked client frame
// when expect_masked, a stray continuation, a frame over 16 MB).
ReadResult ws_read(int fd, WsReader& r, WsMessage& out, bool expect_masked, int timeout_ms);

}  // namespace fakex::net
