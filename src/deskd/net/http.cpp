#include "deskd/net/http.h"

#include <chrono>
#include <cstring>

#include "deskd/net/conn.h"

namespace deskd::net {

namespace {

bool is_tchar(unsigned char c) {
  // RFC 9110 token: "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
  // / DIGIT / ALPHA
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != '\0';
}
bool is_ows(char c) { return c == ' ' || c == '\t'; }
// field-vchar, SP and HTAB; obs-text (0x80-0xFF) allowed, every other control byte refused
bool is_field_char(unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); }

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
std::string trim_ows(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && is_ows(s[a])) ++a;
  while (b > a && is_ows(s[b - 1])) --b;
  return s.substr(a, b - a);
}
// a comma-separated list's members, trimmed, empty ones dropped (RFC 9110 5.6.1)
std::vector<std::string> list_members(const std::string& v) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= v.size()) {
    std::size_t comma = v.find(',', pos);
    if (comma == std::string::npos) comma = v.size();
    std::string m = trim_ows(v.substr(pos, comma - pos));
    if (!m.empty()) out.push_back(std::move(m));
    pos = comma + 1;
  }
  return out;
}
int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "name: value" -> lower-cased name and trimmed value; "" on success, else why not
std::string parse_field(const std::string& line, std::string* name, std::string* value) {
  if (!line.empty() && is_ows(line[0])) return "obsolete line folding";
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) return "a header line without a colon";
  if (colon == 0) return "an empty header name";
  for (std::size_t i = 0; i < colon; ++i)
    if (!is_tchar(static_cast<unsigned char>(line[i])))
      return is_ows(line[i]) ? "whitespace before a header's colon" : "a bad character in a header name";
  for (std::size_t i = colon + 1; i < line.size(); ++i)
    if (!is_field_char(static_cast<unsigned char>(line[i]))) return "a control character in a header value";
  *name = lower(line.substr(0, colon));
  *value = trim_ows(line.substr(colon + 1));
  return "";
}

}  // namespace

std::int64_t mono_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string HttpResponse::header(const std::string& name) const {
  std::string out;
  bool any = false;
  for (const auto& [k, v] : headers) {
    if (k != name) continue;
    if (any) out += ", ";
    out += v;
    any = true;
  }
  return out;
}
bool HttpResponse::has_header(const std::string& name) const {
  for (const auto& kv : headers)
    if (kv.first == name) return true;
  return false;
}

ResponseParser::ResponseParser(Limits limits, bool head_request) : lim_(limits), head_request_(head_request) {}

ResponseParser::Status ResponseParser::fail(const std::string& why) {
  state_ = State::ERROR;
  status_ = Status::ERROR;
  if (error_.empty()) error_ = why;
  return status_;
}

bool ResponseParser::take_line(std::string* line, bool* bad) {
  *bad = false;
  const std::size_t lf = buf_.find('\n');
  if (lf == std::string::npos) {
    if (buf_.size() > lim_.max_line) {
      *bad = true;
      fail("a line over " + std::to_string(lim_.max_line) + " bytes");
    }
    return false;
  }
  if (lf == 0 || buf_[lf - 1] != '\r') {
    *bad = true;
    fail("a bare LF (lines end in CRLF)");
    return false;
  }
  if (lf - 1 > lim_.max_line) {
    *bad = true;
    fail("a line over " + std::to_string(lim_.max_line) + " bytes");
    return false;
  }
  if (std::memchr(buf_.data(), '\r', lf - 1) != nullptr) {
    *bad = true;
    fail("a bare CR inside a line");
    return false;
  }
  line->assign(buf_, 0, lf - 1);
  buf_.erase(0, lf + 1);
  if (state_ == State::STATUS || state_ == State::HEADERS) {
    head_bytes_ += lf + 1;
    if (head_bytes_ > lim_.max_head) {
      *bad = true;
      fail("a head over " + std::to_string(lim_.max_head) + " bytes");
      return false;
    }
  }
  return true;
}

ResponseParser::Status ResponseParser::feed(const char* data, std::size_t n) {
  if (state_ == State::ERROR) return status_;
  seen_ += n;
  buf_.append(data, n);
  if (state_ == State::DONE) return status_;  // bytes after the response stay in leftover()
  return run();
}

ResponseParser::Status ResponseParser::on_head_done() {
  // Connection: close (or HTTP/1.0 without keep-alive) ends the connection after this reply
  bool close = false, keep = false;
  for (const auto& m : list_members(lower(res_.header("connection")))) {
    if (m == "close") close = true;
    if (m == "keep-alive") keep = true;
  }
  res_.keep_alive = res_.minor == 1 ? !close : (keep && !close);

  if (interim_) {
    // 100 Continue and the like: their own head, no body, then the real reply
    interim_ = false;
    res_ = HttpResponse{};
    head_bytes_ = 0;
    state_ = State::STATUS;
    return Status::NEED_MORE;
  }
  const bool has_te = res_.has_header("transfer-encoding"), has_cl = res_.has_header("content-length");
  const std::string ce = lower(res_.header("content-encoding"));
  for (const auto& m : list_members(ce))
    if (m != "identity") return fail("Content-Encoding " + m + " (none was asked for)");

  const bool no_body = head_request_ || res_.status == 204 || res_.status == 304;
  if (no_body) {
    state_ = State::DONE;
    status_ = Status::DONE;
    return status_;
  }
  if (has_te && has_cl) return fail("both Transfer-Encoding and Content-Length");
  if (has_te) {
    if (res_.minor == 0) return fail("Transfer-Encoding in an HTTP/1.0 reply");
    const auto codings = list_members(lower(res_.header("transfer-encoding")));
    if (codings.size() != 1 || codings[0] != "chunked")
      return fail("Transfer-Encoding " + res_.header("transfer-encoding") + " (only chunked is read)");
    res_.chunked = true;
    state_ = State::CHUNK_SIZE;
    return Status::NEED_MORE;
  }
  if (has_cl) {
    // repeats, in one field ("5, 5") or several, must all be the same number
    std::string first;
    for (const auto& [k, v] : res_.headers) {
      if (k != "content-length") continue;
      for (const auto& m : list_members(v)) {
        if (m.size() > 18 || m.find_first_not_of("0123456789") != std::string::npos)
          return fail("a bad Content-Length \"" + m + "\"");
        if (first.empty()) first = m;
        else if (std::stoll(m) != std::stoll(first)) return fail("Content-Length values that differ");
      }
    }
    if (first.empty()) return fail("an empty Content-Length");
    content_length_ = std::stoll(first);
    if (static_cast<unsigned long long>(content_length_) > lim_.max_body)
      return fail("a body of " + first + " bytes, over the " + std::to_string(lim_.max_body) + " limit");
    if (content_length_ == 0) {
      state_ = State::DONE;
      status_ = Status::DONE;
      return status_;
    }
    res_.body.reserve(static_cast<std::size_t>(content_length_));
    state_ = State::BODY_LENGTH;
    return Status::NEED_MORE;
  }
  // neither: the body runs to the close, and the connection cannot carry another reply
  res_.keep_alive = false;
  state_ = State::BODY_CLOSE;
  return Status::NEED_MORE;
}

ResponseParser::Status ResponseParser::run() {
  for (;;) {
    bool bad = false;
    std::string line;
    switch (state_) {
      case State::STATUS: {
        if (!take_line(&line, &bad)) return bad ? status_ : Status::NEED_MORE;
        // HTTP-version SP status-code SP [reason]; a missing SP before an empty reason is let go
        if (line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0 || (line[7] != '0' && line[7] != '1') ||
            line[8] != ' ')
          return fail("a bad status line");
        for (int i = 9; i < 12; ++i)
          if (line[i] < '0' || line[i] > '9') return fail("a bad status code");
        if (line.size() > 12 && line[12] != ' ') return fail("a bad status line");
        res_.minor = line[7] - '0';
        res_.status = std::stoi(line.substr(9, 3));
        if (res_.status < 100 || res_.status > 599) return fail("status " + std::to_string(res_.status) + " out of range");
        if (res_.status == 101) return fail("101 Switching Protocols on a request that asked for none");
        res_.reason = line.size() > 13 ? line.substr(13) : std::string();
        for (unsigned char c : res_.reason)
          if (!is_field_char(c)) return fail("a control character in the reason phrase");
        interim_ = res_.status < 200;
        state_ = State::HEADERS;
        break;
      }
      case State::HEADERS: {
        if (!take_line(&line, &bad)) return bad ? status_ : Status::NEED_MORE;
        if (line.empty()) {
          const Status s = on_head_done();
          if (s != Status::NEED_MORE || state_ == State::DONE) return s;
          break;
        }
        std::string name, value;
        const std::string why = parse_field(line, &name, &value);
        if (!why.empty()) return fail(why);
        if (res_.headers.size() >= lim_.max_headers)
          return fail("more than " + std::to_string(lim_.max_headers) + " header fields");
        res_.headers.emplace_back(std::move(name), std::move(value));
        break;
      }
      case State::BODY_LENGTH: {
        const std::size_t want = static_cast<std::size_t>(content_length_) - res_.body.size();
        const std::size_t n = std::min(want, buf_.size());
        res_.body.append(buf_, 0, n);
        buf_.erase(0, n);
        if (res_.body.size() == static_cast<std::size_t>(content_length_)) {
          state_ = State::DONE;
          status_ = Status::DONE;
          return status_;
        }
        return Status::NEED_MORE;
      }
      case State::BODY_CLOSE: {
        if (res_.body.size() + buf_.size() > lim_.max_body)
          return fail("a body over the " + std::to_string(lim_.max_body) + " byte limit");
        res_.body += buf_;
        buf_.clear();
        return Status::NEED_MORE;
      }
      case State::CHUNK_SIZE: {
        if (!take_line(&line, &bad)) return bad ? status_ : Status::NEED_MORE;
        std::size_t i = 0;
        unsigned long long size = 0;
        while (i < line.size() && hexval(line[i]) >= 0) {
          if (size > (~0ULL >> 4)) return fail("a chunk size that overflows");
          size = size * 16 + static_cast<unsigned>(hexval(line[i]));
          ++i;
        }
        if (i == 0) return fail("a chunk without a hex size");
        // chunk-ext = *( BWS ";" BWS name [ BWS "=" BWS ( token / quoted-string ) ] ), read and dropped
        while (i < line.size()) {
          while (i < line.size() && is_ows(line[i])) ++i;
          if (i == line.size()) break;
          if (line[i] != ';') return fail("a bad chunk extension");
          ++i;
          while (i < line.size() && is_ows(line[i])) ++i;
          const std::size_t name_start = i;
          while (i < line.size() && is_tchar(static_cast<unsigned char>(line[i]))) ++i;
          if (i == name_start) return fail("a chunk extension without a name");
          std::size_t j = i;
          while (j < line.size() && is_ows(line[j])) ++j;
          if (j < line.size() && line[j] == '=') {
            i = j + 1;
            while (i < line.size() && is_ows(line[i])) ++i;
            if (i < line.size() && line[i] == '"') {
              ++i;
              bool closed = false;
              while (i < line.size()) {
                const unsigned char c = static_cast<unsigned char>(line[i]);
                if (c == '\\') {
                  if (i + 1 >= line.size()) return fail("a bad quoted chunk extension");
                  i += 2;
                  continue;
                }
                if (c == '"') {
                  closed = true;
                  ++i;
                  break;
                }
                if (!is_field_char(c)) return fail("a control character in a chunk extension");
                ++i;
              }
              if (!closed) return fail("an unclosed quoted chunk extension");
            } else {
              const std::size_t v0 = i;
              while (i < line.size() && is_tchar(static_cast<unsigned char>(line[i]))) ++i;
              if (i == v0) return fail("a chunk extension without a value after =");
            }
          }
        }
        if (size == 0) {
          state_ = State::TRAILERS;
          break;
        }
        if (res_.body.size() + size > lim_.max_body)
          return fail("a body over the " + std::to_string(lim_.max_body) + " byte limit");
        chunk_left_ = static_cast<std::size_t>(size);
        state_ = State::CHUNK_DATA;
        break;
      }
      case State::CHUNK_DATA: {
        const std::size_t n = std::min(chunk_left_, buf_.size());
        res_.body.append(buf_, 0, n);
        buf_.erase(0, n);
        chunk_left_ -= n;
        if (chunk_left_ > 0) return Status::NEED_MORE;
        state_ = State::CHUNK_CRLF;
        break;
      }
      case State::CHUNK_CRLF: {
        if (buf_.size() < 2) {
          if (!buf_.empty() && buf_[0] != '\r') return fail("chunk data longer than its size");
          return Status::NEED_MORE;
        }
        if (buf_[0] != '\r' || buf_[1] != '\n') return fail("chunk data longer than its size");
        buf_.erase(0, 2);
        state_ = State::CHUNK_SIZE;
        break;
      }
      case State::TRAILERS: {
        if (!take_line(&line, &bad)) return bad ? status_ : Status::NEED_MORE;
        if (line.empty()) {
          state_ = State::DONE;
          status_ = Status::DONE;
          return status_;
        }
        std::string name, value;
        const std::string why = parse_field(line, &name, &value);
        if (!why.empty()) return fail("trailer: " + why);
        if (res_.trailers.size() >= lim_.max_headers)
          return fail("more than " + std::to_string(lim_.max_headers) + " trailer fields");
        res_.trailers.emplace_back(std::move(name), std::move(value));
        break;
      }
      case State::DONE:
      case State::ERROR:
        return status_;
    }
  }
}

ResponseParser::Status ResponseParser::eof() {
  if (state_ == State::DONE || state_ == State::ERROR) return status_;
  if (state_ == State::BODY_CLOSE) {
    state_ = State::DONE;
    status_ = Status::DONE;
    return status_;
  }
  if (seen_ == 0) {
    cut_ = Cut::NO_BYTES;
    return fail("closed before any reply");
  }
  if (state_ == State::STATUS || state_ == State::HEADERS) {
    cut_ = Cut::IN_HEAD;
    return fail("closed inside the reply's head after " + std::to_string(seen_) + " bytes");
  }
  cut_ = Cut::IN_BODY;
  if (content_length_ >= 0)
    return fail("body cut at " + std::to_string(res_.body.size()) + " of " + std::to_string(content_length_) + " bytes");
  return fail("chunked body cut after " + std::to_string(res_.body.size()) + " bytes");
}

std::string request_bytes(const std::string& method, const std::string& host_header, const std::string& target,
                          const HeaderList& headers, const std::string& body, bool keep_alive) {
  auto clean = [](const std::string& s) { return s.find_first_of(std::string("\r\n\0", 3)) == std::string::npos; };
  if (method.empty() || target.empty() || target[0] != '/' || !clean(method) || !clean(target) ||
      target.find(' ') != std::string::npos || !clean(host_header))
    return "";
  std::string out;
  out.reserve(256 + body.size());
  out += method;
  out += ' ';
  out += target;
  out += " HTTP/1.1\r\nHost: ";
  out += host_header;
  out += "\r\n";
  for (const auto& [k, v] : headers) {
    if (k.empty() || !clean(k) || !clean(v)) return "";
    out += k;
    out += ": ";
    out += v;
    out += "\r\n";
  }
  if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }
  out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  out += body;
  return out;
}

const char* fate_name(Exchange::Fate f) {
  switch (f) {
    case Exchange::Fate::REPLIED: return "REPLIED";
    case Exchange::Fate::NOT_SENT: return "NOT_SENT";
    case Exchange::Fate::UNKNOWN: return "UNKNOWN";
  }
  return "?";
}

Exchange exchange(Conn& c, const std::string& request, std::int64_t deadline_us, bool head_request,
                  const std::function<void()>& idle) {
  Exchange x;
  x.t_start_us = mono_us();
  if (request.empty()) {
    x.fate = Exchange::Fate::NOT_SENT;
    x.why = "an empty or malformed request";
    return x;
  }
  // A connection the venue already closed (an idle close, a close_notify) would take the write
  // into its socket buffer and then reset it: an order that may or may not have arrived. Looked
  // at before the first byte goes, it is a refusal that costs nothing.
  std::string why;
  if (!c.usable(&why)) {
    x.fate = Exchange::Fate::NOT_SENT;
    x.why = "the connection was closed before the write: " + why;
    return x;
  }
  const IoResult w = c.write_all(request.data(), request.size(), deadline_us, idle);
  if (w.status != IoStatus::OK) {
    x.fate = Exchange::Fate::NOT_SENT;
    x.why = "the write did not complete: " + w.why;
    return x;
  }
  x.t_written_us = mono_us();
  ResponseParser p(head_request);
  char buf[16384];
  for (;;) {
    const std::int64_t now = mono_us();
    if (now >= deadline_us) {
      x.fate = Exchange::Fate::UNKNOWN;
      x.cut = p.bytes_seen() ? (p.head_done() ? ResponseParser::Cut::IN_BODY : ResponseParser::Cut::IN_HEAD)
                             : ResponseParser::Cut::NO_BYTES;
      x.why = "no whole reply " + std::to_string((now - x.t_written_us) / 1000) + " ms after the write (" +
              std::to_string(p.bytes_seen()) + " bytes)";
      return x;
    }
    const int slice_ms = static_cast<int>(std::min<std::int64_t>(250, (deadline_us - now + 999) / 1000));
    const IoResult r = c.read_some(buf, sizeof buf, slice_ms);
    if (r.status == IoStatus::OK) {
      if (!x.t_first_byte_us) x.t_first_byte_us = mono_us();
      const auto s = p.feed(buf, r.n);
      if (s == ResponseParser::Status::DONE) {
        x.t_done_us = mono_us();
        x.fate = Exchange::Fate::REPLIED;
        x.reusable = p.response().keep_alive && p.leftover().empty();
        x.res = p.take();
        return x;
      }
      if (s == ResponseParser::Status::ERROR) {
        x.fate = Exchange::Fate::UNKNOWN;
        x.why = "a reply this parser refuses: " + p.error();
        return x;
      }
      continue;
    }
    if (r.status == IoStatus::TIMEOUT) {
      if (idle) idle();
      continue;
    }
    // the peer closed (cleanly or not) or reset
    const auto s = p.eof();
    if (s == ResponseParser::Status::DONE) {
      x.t_done_us = mono_us();
      x.fate = Exchange::Fate::REPLIED;
      x.reusable = false;
      x.res = p.take();
      return x;
    }
    x.fate = Exchange::Fate::UNKNOWN;
    x.cut = p.cut();
    x.why = p.error() + " (" + r.why + ")";
    return x;
  }
}

}  // namespace deskd::net
