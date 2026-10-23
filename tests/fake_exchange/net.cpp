#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>

namespace fakex::net {

namespace {
constexpr std::size_t kMaxHead = 64 * 1024;
constexpr std::size_t kMaxBody = 16 * 1024 * 1024;

// >0 bytes appended, 0 the peer closed, -1 nothing within timeout_ms, -2 an error (reset)
int recv_some(int fd, std::string& buf, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  const int r = ::poll(&p, 1, timeout_ms);
  if (r == 0) return -1;
  if (r < 0) return errno == EINTR ? -1 : -2;
  char tmp[16384];
  const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
  if (n == 0) return 0;
  if (n < 0) return (errno == EAGAIN || errno == EINTR) ? -1 : -2;
  buf.append(tmp, static_cast<std::size_t>(n));
  return static_cast<int>(n);
}

// Reads until buf holds `need` bytes. OK, CLOSED (the peer went first) or TIMEOUT.
ReadResult ensure(int fd, std::string& buf, std::size_t need, int timeout_ms) {
  while (buf.size() < need) {
    const int r = recv_some(fd, buf, timeout_ms);
    if (r == -1) return ReadResult::TIMEOUT;
    if (r <= 0) return ReadResult::CLOSED;
  }
  return ReadResult::OK;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// The header block's lines after the first, into lower-cased names.
std::map<std::string, std::string> parse_headers(const std::string& head, std::size_t first_eol) {
  std::map<std::string, std::string> h;
  std::size_t pos = first_eol + 2;
  while (pos < head.size()) {
    std::size_t eol = head.find("\r\n", pos);
    if (eol == std::string::npos) eol = head.size();
    const std::string line = head.substr(pos, eol - pos);
    pos = eol + 2;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    auto it = h.find(name);
    if (it != h.end())
      it->second += ", " + value;
    else
      h.emplace(std::move(name), std::move(value));
  }
  return h;
}

// A chunked body starting at buf[pos]; on OK, *end is the offset just past it.
ReadResult read_chunked(int fd, std::string& buf, std::size_t pos, std::string& body, std::size_t* end,
                        int timeout_ms) {
  for (;;) {
    std::size_t eol;
    while ((eol = buf.find("\r\n", pos)) == std::string::npos) {
      if (buf.size() - pos > 1024) return ReadResult::BAD;
      const ReadResult r = ensure(fd, buf, buf.size() + 1, timeout_ms);
      if (r != ReadResult::OK) return r;
    }
    const std::string size_line = buf.substr(pos, eol - pos);
    char* stop = nullptr;
    const unsigned long long n = std::strtoull(size_line.c_str(), &stop, 16);
    if (stop == size_line.c_str()) return ReadResult::BAD;
    if (body.size() + n > kMaxBody) return ReadResult::BAD;
    pos = eol + 2;
    if (n == 0) {
      // trailers, then the empty line
      for (;;) {
        while ((eol = buf.find("\r\n", pos)) == std::string::npos) {
          const ReadResult r = ensure(fd, buf, buf.size() + 1, timeout_ms);
          if (r != ReadResult::OK) return r;
        }
        const bool empty = eol == pos;
        pos = eol + 2;
        if (empty) break;
      }
      *end = pos;
      return ReadResult::OK;
    }
    const ReadResult r = ensure(fd, buf, pos + n + 2, timeout_ms);
    if (r != ReadResult::OK) {
      // what did arrive stays visible to a caller looking at a cut body
      if (buf.size() > pos) body.append(buf, pos, std::min<std::size_t>(n, buf.size() - pos));
      return r;
    }
    body.append(buf, pos, n);
    pos += n + 2;
  }
}
}  // namespace

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Request::header(const std::string& name) const {
  auto it = headers.find(name);
  return it == headers.end() ? std::string() : it->second;
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::map<std::string, std::string> parse_query(const std::string& q) {
  std::map<std::string, std::string> out;
  std::size_t pos = 0;
  while (pos < q.size()) {
    std::size_t amp = q.find('&', pos);
    if (amp == std::string::npos) amp = q.size();
    const std::string kv = q.substr(pos, amp - pos);
    pos = amp + 1;
    if (kv.empty()) continue;
    const auto eq = kv.find('=');
    out[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
  }
  return out;
}

ReadResult read_request(int fd, std::string& buf, Request& req, int idle_ms, std::string* why) {
  std::size_t head_end;
  while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > kMaxHead) {
      if (why) *why = "header block too large";
      return ReadResult::BAD;
    }
    const int r = recv_some(fd, buf, idle_ms);
    if (r == -1) return ReadResult::TIMEOUT;
    if (r <= 0) return ReadResult::CLOSED;
  }
  const std::string head = buf.substr(0, head_end + 2);
  const std::size_t eol = head.find("\r\n");
  const std::string line = head.substr(0, eol);
  const auto s1 = line.find(' '), s2 = line.rfind(' ');
  if (s1 == std::string::npos || s2 == s1) {
    if (why) *why = "bad request line";
    return ReadResult::BAD;
  }
  req = Request{};
  req.method = line.substr(0, s1);
  req.target = line.substr(s1 + 1, s2 - s1 - 1);
  const std::string version = line.substr(s2 + 1);
  const auto q = req.target.find('?');
  req.path = req.target.substr(0, q);
  if (q != std::string::npos) req.query = req.target.substr(q + 1);
  req.params = parse_query(req.query);
  req.headers = parse_headers(head, eol);
  const std::string conn = lower(req.header("connection"));
  req.keep_alive = version == "HTTP/1.1" ? conn.find("close") == std::string::npos
                                         : conn.find("keep-alive") != std::string::npos;
  std::size_t pos = head_end + 4;
  if (lower(req.header("transfer-encoding")).find("chunked") != std::string::npos) {
    std::size_t end = 0;
    const ReadResult r = read_chunked(fd, buf, pos, req.body, &end, idle_ms);
    if (r != ReadResult::OK) return r == ReadResult::TIMEOUT ? ReadResult::BAD : r;
    buf.erase(0, end);
    return ReadResult::OK;
  }
  const std::string cl = req.header("content-length");
  std::size_t n = 0;
  if (!cl.empty()) {
    char* stop = nullptr;
    n = std::strtoull(cl.c_str(), &stop, 10);
    if (*stop != '\0' || n > kMaxBody) {
      if (why) *why = "bad Content-Length";
      return ReadResult::BAD;
    }
  }
  const ReadResult r = ensure(fd, buf, pos + n, idle_ms);
  if (r != ReadResult::OK) return r == ReadResult::TIMEOUT ? ReadResult::BAD : r;
  req.body = buf.substr(pos, n);
  buf.erase(0, pos + n);
  return ReadResult::OK;
}

ReadResult read_response(int fd, std::string& buf, Response& res, int timeout_ms, bool head_request) {
  res = Response{};
  std::size_t head_end;
  while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > kMaxHead) return ReadResult::BAD;
    const int r = recv_some(fd, buf, timeout_ms);
    if (r == -1) return ReadResult::TIMEOUT;
    if (r <= 0) return ReadResult::CLOSED;  // nothing, or a head cut short
  }
  const std::string head = buf.substr(0, head_end + 2);
  const std::size_t eol = head.find("\r\n");
  const std::string line = head.substr(0, eol);
  const auto s1 = line.find(' ');
  if (line.compare(0, 5, "HTTP/") != 0 || s1 == std::string::npos) return ReadResult::BAD;
  res.status = std::atoi(line.c_str() + s1 + 1);
  res.headers = parse_headers(head, eol);
  res.head_bytes = head_end + 4;
  std::size_t pos = head_end + 4;
  if (head_request || res.status == 204 || res.status == 304 || res.status / 100 == 1) {
    res.complete = true;
    buf.erase(0, pos);
    return ReadResult::OK;
  }
  auto te = res.headers.find("transfer-encoding");
  if (te != res.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
    res.chunked = true;
    std::size_t end = 0;
    const ReadResult r = read_chunked(fd, buf, pos, res.body, &end, timeout_ms);
    if (r != ReadResult::OK) {
      buf.clear();
      return r;
    }
    res.complete = true;
    buf.erase(0, end);
    return ReadResult::OK;
  }
  auto cl = res.headers.find("content-length");
  if (cl == res.headers.end()) {
    // no length: the body runs to the close
    for (;;) {
      const int r = recv_some(fd, buf, timeout_ms);
      if (r == -1) return ReadResult::TIMEOUT;
      if (r <= 0) break;
    }
    res.body = buf.substr(pos);
    res.complete = true;
    buf.clear();
    return ReadResult::CLOSED;
  }
  const std::size_t n = std::strtoull(cl->second.c_str(), nullptr, 10);
  const ReadResult r = ensure(fd, buf, pos + n, timeout_ms);
  if (r != ReadResult::OK) {
    res.body = buf.substr(pos);  // what arrived of a body cut mid-way
    buf.clear();
    return r;
  }
  res.body = buf.substr(pos, n);
  res.complete = true;
  buf.erase(0, pos + n);
  return ReadResult::OK;
}

const char* status_text(int s) {
  switch (s) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
  }
  return "Status";
}

std::string response_head(int status, const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string h = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
  for (const auto& [k, v] : headers) h += k + ": " + v + "\r\n";
  h += "\r\n";
  return h;
}

std::string chunked_body(const std::string& body, std::size_t chunk_bytes) {
  if (chunk_bytes == 0) chunk_bytes = body.size() ? body.size() : 1;
  std::string out;
  char hex[32];
  for (std::size_t pos = 0; pos < body.size(); pos += chunk_bytes) {
    const std::size_t n = std::min(chunk_bytes, body.size() - pos);
    std::snprintf(hex, sizeof hex, "%zx\r\n", n);
    out += hex;
    out.append(body, pos, n);
    out += "\r\n";
  }
  out += "0\r\n\r\n";
  return out;
}

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

bool write_all(int fd, const std::string& data) {
  std::size_t off = 0;
  while (off < data.size()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {
        pollfd p{fd, POLLOUT, 0};
        ::poll(&p, 1, 1000);
        continue;
      }
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

int listen_loopback(int port, int* bound_port, std::string* err) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 128) != 0) {
    if (err) *err = "bind 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  if (bound_port) *bound_port = ntohs(a.sin_port);
  return fd;
}

int connect_loopback(int port, int timeout_ms) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  no_sigpipe(fd);
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// ---- WebSocket ----------------------------------------------------------------------------------

std::string base64(const std::string& raw) {
  std::string out(4 * ((raw.size() + 2) / 3) + 1, '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(raw.data()), static_cast<int>(raw.size()));
  out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
  return out;
}

std::string unbase64(const std::string& in) {
  std::string out(in.size() / 4 * 3 + 3, '\0');
  const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
  if (n < 0) return {};
  std::size_t len = static_cast<std::size_t>(n);
  if (!in.empty() && in.back() == '=') --len;
  if (in.size() > 1 && in[in.size() - 2] == '=') --len;
  out.resize(len);
  return out;
}

std::string ws_accept_key(const std::string& client_key) {
  const std::string in = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(in.data(), in.size(), md, &len, EVP_sha1(), nullptr);
  return base64(std::string(reinterpret_cast<const char*>(md), len));
}

std::string ws_frame(int opcode, const std::string& payload, bool fin, bool masked, std::uint32_t key) {
  std::string f;
  f.push_back(static_cast<char>((fin ? 0x80 : 0) | (opcode & 0x0f)));
  const std::uint64_t n = payload.size();
  const char m = masked ? static_cast<char>(0x80) : 0;
  if (n < 126) {
    f.push_back(static_cast<char>(m | static_cast<char>(n)));
  } else if (n <= 0xffff) {
    f.push_back(static_cast<char>(m | 126));
    f.push_back(static_cast<char>((n >> 8) & 0xff));
    f.push_back(static_cast<char>(n & 0xff));
  } else {
    f.push_back(static_cast<char>(m | 127));
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
  }
  if (!masked) return f + payload;
  const unsigned char k[4] = {static_cast<unsigned char>(key >> 24), static_cast<unsigned char>(key >> 16),
                              static_cast<unsigned char>(key >> 8), static_cast<unsigned char>(key)};
  f.append(reinterpret_cast<const char*>(k), 4);
  for (std::size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ k[i % 4]));
  return f;
}

std::string ws_text(const std::string& payload, std::size_t frame_bytes) {
  if (frame_bytes == 0 || payload.size() <= frame_bytes) return ws_frame(TEXT, payload);
  std::string out;
  for (std::size_t pos = 0; pos < payload.size(); pos += frame_bytes) {
    const bool last = pos + frame_bytes >= payload.size();
    out += ws_frame(pos == 0 ? TEXT : CONT, payload.substr(pos, frame_bytes), last);
  }
  return out;
}

ReadResult ws_read(int fd, WsReader& r, WsMessage& out, bool expect_masked, int timeout_ms) {
  for (;;) {
    // one frame
    ReadResult got = ensure(fd, r.buf, 2, timeout_ms);
    if (got != ReadResult::OK) return got;
    const unsigned char b0 = static_cast<unsigned char>(r.buf[0]), b1 = static_cast<unsigned char>(r.buf[1]);
    const bool fin = b0 & 0x80;
    const int op = b0 & 0x0f;
    const bool masked = b1 & 0x80;
    std::uint64_t n = b1 & 0x7f;
    std::size_t hdr = 2;
    if (n == 126) {
      if ((got = ensure(fd, r.buf, 4, timeout_ms)) != ReadResult::OK) return got;
      n = (static_cast<std::uint64_t>(static_cast<unsigned char>(r.buf[2])) << 8) | static_cast<unsigned char>(r.buf[3]);
      hdr = 4;
    } else if (n == 127) {
      if ((got = ensure(fd, r.buf, 10, timeout_ms)) != ReadResult::OK) return got;
      n = 0;
      for (int i = 0; i < 8; ++i) n = (n << 8) | static_cast<unsigned char>(r.buf[2 + i]);
      hdr = 10;
    }
    if (n > kMaxBody || masked != expect_masked) return ReadResult::BAD;
    if (masked) hdr += 4;
    if ((got = ensure(fd, r.buf, hdr + n, timeout_ms)) != ReadResult::OK) return got;
    std::string payload = r.buf.substr(hdr, n);
    if (masked) {
      const unsigned char* k = reinterpret_cast<const unsigned char*>(r.buf.data() + hdr - 4);
      for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ k[i % 4]);
    }
    r.buf.erase(0, hdr + n);
    if (op & 0x8) {  // control frames come whole and may sit between fragments
      out = WsMessage{op, std::move(payload), 1, n};
      return ReadResult::OK;
    }
    if (op == CONT) {
      if (r.partial_op == 0) return ReadResult::BAD;
      r.partial += payload;
      ++r.partial_frames;
      r.partial_max = std::max<std::size_t>(r.partial_max, n);
      if (!fin) continue;
      out = WsMessage{r.partial_op, std::move(r.partial), r.partial_frames, r.partial_max};
      r.partial.clear();
      r.partial_op = r.partial_frames = 0;
      r.partial_max = 0;
      return ReadResult::OK;
    }
    if (r.partial_op != 0) return ReadResult::BAD;
    if (fin) {
      out = WsMessage{op, std::move(payload), 1, n};
      return ReadResult::OK;
    }
    r.partial_op = op;
    r.partial = std::move(payload);
    r.partial_frames = 1;
    r.partial_max = n;
  }
}

}  // namespace fakex::net
