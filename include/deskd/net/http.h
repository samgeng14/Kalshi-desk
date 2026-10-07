#pragma once
// HTTP/1.1 for deskd's venue connections: a strict response parser, the request bytes, and one
// request on one connection (plan sections 2-3).
//
// The parser is deskd's own: llhttp is not on this machine and nothing new is vendored
// (2026-10-07), and cpp-httplib's client hides exactly the cases the sender must see. It takes
// bytes as they arrive, in any split, and stops at the first thing RFC 9112 does not allow
// rather than guessing:
//   - the status line is "HTTP/1.1 ddd reason" (or HTTP/1.0); lines end in CRLF, never a bare LF;
//     no obsolete line folding; a header name is an RFC 9110 token right before its colon
//   - the body is framed by Transfer-Encoding: chunked (extensions skipped, trailers read and
//     kept apart from the headers), by one Content-Length (repeats must agree), or, with neither,
//     by the close; 1xx (100 Continue, skipped), 204, 304 and a HEAD's reply have none
//   - both Transfer-Encoding and Content-Length, a transfer coding other than chunked, a
//     Content-Encoding other than identity (deskd never sends Accept-Encoding), and 101 are errors
//   - limits on the head (64 KiB), header count (100), one line (8 KiB) and the body (16 MiB)
// A response that stops early is never "failed": the connection closed with no byte, inside the
// head, or with the body cut mid-way is Cut::NO_BYTES, IN_HEAD or IN_BODY, and after an order
// write each one is UNKNOWN, because the venue may have taken the order before it went quiet
// (executor.cpp:225-251's rule, plan section 3).
//
// exchange(): one request on one connection, the venue sender's only path. It reports whether
// the request can have reached the venue whole:
//   NOT_SENT  nothing complete was written (the connection was already closed, or the write
//             failed or timed out part-way: a server never processes a request whose body it
//             did not get whole, and a TLS record cut short cannot even be decrypted)
//   UNKNOWN   the request was written, then no reply, a reset, a timeout, a cut reply or one this
//             parser refuses
//   REPLIED   a whole, well-formed reply; its status is the caller's to read (a 5xx after an
//             order write is UNKNOWN to gw: gw/kalshi_wire.h decides that, not this layer)
// It never retries anything, and a connection is never handed a second write (net/pool.h).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace deskd::net {

class Conn;

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
  int status = 0;
  int minor = 1;              // HTTP/1.<minor>
  std::string reason;
  HeaderList headers;         // names lower-cased, values trimmed, in arrival order
  HeaderList trailers;        // a chunked body's trailer fields, apart from the headers
  std::string body;
  bool chunked = false;
  bool keep_alive = true;     // false: Connection: close, HTTP/1.0 without keep-alive, or a body to the close
  // the value of a header (lower-case name); repeated fields joined with ", "; "" when absent
  std::string header(const std::string& lower_name) const;
  bool has_header(const std::string& lower_name) const;
};

class ResponseParser {
 public:
  struct Limits {
    std::size_t max_head = 64 * 1024;       // status line and headers together
    std::size_t max_line = 8 * 1024;        // one header line, one chunk-size line, one trailer
    std::size_t max_headers = 100;          // header fields, and trailer fields
    std::size_t max_body = 16 * 1024 * 1024;
  };
  enum class Status { NEED_MORE, DONE, ERROR };
  enum class Cut { NONE, NO_BYTES, IN_HEAD, IN_BODY };

  explicit ResponseParser(bool head_request = false) : ResponseParser(Limits{}, head_request) {}
  ResponseParser(Limits limits, bool head_request);

  // Takes the next bytes. DONE once the response is whole (bytes after it are kept, unread, in
  // leftover()); ERROR with error() set on anything the rules above refuse; NEED_MORE otherwise.
  // After DONE or ERROR it takes nothing more and returns the same status.
  Status feed(const char* data, std::size_t n);
  Status feed(const std::string& s) { return feed(s.data(), s.size()); }
  // The peer closed. DONE when the body runs to the close (no length, no chunking); otherwise
  // the response is cut (cut() says where) and the status is ERROR.
  Status eof();

  Status status() const { return status_; }
  Cut cut() const { return cut_; }
  const std::string& error() const { return error_; }
  const HttpResponse& response() const { return res_; }
  HttpResponse take() { return std::move(res_); }
  std::size_t bytes_seen() const { return seen_; }
  bool head_done() const { return state_ > State::HEADERS; }
  // the body's length when the head promised one (-1: chunked or to the close); its bytes so far
  long long promised_body() const { return content_length_; }
  std::size_t body_bytes() const { return res_.body.size(); }
  const std::string& leftover() const { return buf_; }

 private:
  enum class State { STATUS, HEADERS, BODY_LENGTH, BODY_CLOSE, CHUNK_SIZE, CHUNK_DATA, CHUNK_CRLF, TRAILERS, DONE, ERROR };
  Status fail(const std::string& why);
  Status run();
  bool take_line(std::string* line, bool* bad);  // a CRLF-terminated line from buf_ (without CRLF)
  Status on_head_done();

  Limits lim_;
  bool head_request_;
  State state_ = State::STATUS;
  Status status_ = Status::NEED_MORE;
  Cut cut_ = Cut::NONE;
  std::string error_;
  std::string buf_;             // bytes taken and not yet consumed
  std::size_t seen_ = 0;
  std::size_t head_bytes_ = 0;
  long long content_length_ = -1;
  std::size_t chunk_left_ = 0;
  bool interim_ = false;        // reading a 1xx head, skipped
  HttpResponse res_;
};

// "GET /trade-api/v2/portfolio/balance HTTP/1.1" plus Host, the caller's headers, Content-Length
// when there is a body (or the method is POST, PUT or DELETE-with-body), and Connection. No
// Accept-Encoding: replies come uncompressed. target is the path with its query.
std::string request_bytes(const std::string& method, const std::string& host_header, const std::string& target,
                          const HeaderList& headers, const std::string& body, bool keep_alive);

struct Exchange {
  enum class Fate { REPLIED, NOT_SENT, UNKNOWN };
  Fate fate = Fate::NOT_SENT;
  HttpResponse res;           // REPLIED only
  std::string why;            // NOT_SENT and UNKNOWN: what happened, for the log and the UNKNOWN row
  ResponseParser::Cut cut = ResponseParser::Cut::NONE;
  bool reusable = false;      // REPLIED with keep-alive and nothing left over (reads may reuse; writes never)
  // steady-clock microseconds: write start, write done, first reply byte, reply whole (0: never)
  std::int64_t t_start_us = 0, t_written_us = 0, t_first_byte_us = 0, t_done_us = 0;
  double write_to_done_ms() const { return t_done_us && t_written_us ? (t_done_us - t_written_us) / 1000.0 : 0; }
  double total_ms() const { return t_done_us && t_start_us ? (t_done_us - t_start_us) / 1000.0 : 0; }
};
const char* fate_name(Exchange::Fate f);

// One request on c, waiting until deadline_us (steady clock, microseconds) for the whole reply.
// idle() runs at least every 250 ms while it waits, so a sender keeps beating (app/ops.h).
Exchange exchange(Conn& c, const std::string& request, std::int64_t deadline_us, bool head_request = false,
                  const std::function<void()>& idle = nullptr);

std::int64_t mono_us();   // the steady clock in microseconds, the clock every net/ deadline uses

}  // namespace deskd::net
