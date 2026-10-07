// deskd net/http.h: the strict HTTP/1.1 response parser, without sockets.
//
// Every well-formed reply is fed whole, in every two-piece split, byte by byte and in 300 random
// partitions, and must come out the same each time. Every refusal the header lists is shown to be
// one, and every early end is a cut, never a reply: closed with no byte (NO_BYTES), inside the
// head (IN_HEAD), or with the body cut mid-way (IN_BODY), at every byte of a Content-Length reply
// and of a chunked one. After an order write each cut is UNKNOWN (plan section 3).
#include <signal.h>

#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "deskd/net/http.h"

using namespace deskd::net;
using St = ResponseParser::Status;
using Cut = ResponseParser::Cut;

namespace {

struct Fed {
  St st = St::NEED_MORE;
  HttpResponse res;
  std::string err;
  Cut cut = Cut::NONE;
  std::string leftover;
};

// feeds the pieces, then the close when eof
Fed run(const std::vector<std::string>& pieces, bool eof = false, bool head = false,
        ResponseParser::Limits lim = ResponseParser::Limits{}) {
  ResponseParser p(lim, head);
  Fed f;
  for (const auto& piece : pieces) {
    f.st = p.feed(piece);
    if (f.st != St::NEED_MORE) break;
  }
  if (eof && f.st == St::NEED_MORE) f.st = p.eof();
  f.err = p.error();
  f.cut = p.cut();
  f.leftover = p.leftover();
  f.res = p.response();
  return f;
}
Fed run1(const std::string& s, bool eof = false, bool head = false) { return run({s}, eof, head); }

bool same(const HttpResponse& a, const HttpResponse& b) {
  return a.status == b.status && a.minor == b.minor && a.reason == b.reason && a.headers == b.headers &&
         a.trailers == b.trailers && a.body == b.body && a.chunked == b.chunked && a.keep_alive == b.keep_alive;
}

// The reply whole, then split at every byte, byte by byte, and in random partitions: the same
// response each time.
void every_split(const std::string& wire, bool eof_needed, const char* what) {
  const Fed whole = run1(wire, eof_needed);
  CHECK(whole.st == St::DONE);
  if (whole.st != St::DONE) std::fprintf(stderr, "  %s: %s\n", what, whole.err.c_str());
  int bad = 0;
  for (std::size_t i = 0; i <= wire.size(); ++i) {
    const Fed f = run({wire.substr(0, i), wire.substr(i)}, eof_needed);
    if (f.st != St::DONE || !same(f.res, whole.res)) ++bad;
  }
  std::vector<std::string> bytes;
  for (char c : wire) bytes.emplace_back(1, c);
  const Fed by_byte = run(bytes, eof_needed);
  if (by_byte.st != St::DONE || !same(by_byte.res, whole.res)) ++bad;
  std::mt19937 rng(7);
  for (int k = 0; k < 300; ++k) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < wire.size()) {
      const std::size_t n = 1 + rng() % 23;
      parts.push_back(wire.substr(pos, n));
      pos += n;
    }
    const Fed f = run(parts, eof_needed);
    if (f.st != St::DONE || !same(f.res, whole.res)) ++bad;
  }
  CHECK(bad == 0);
  if (bad) std::fprintf(stderr, "  %s: %d splits differ\n", what, bad);
}

bool refused(const std::string& wire, const char* fragment, bool eof = false, bool head = false,
             ResponseParser::Limits lim = ResponseParser::Limits{}) {
  const Fed f = run({wire}, eof, head, lim);
  const bool ok = f.st == St::ERROR && f.err.find(fragment) != std::string::npos;
  if (!ok) std::fprintf(stderr, "  expected a refusal with \"%s\", got status %d: %s\n", fragment, static_cast<int>(f.st),
                        f.err.c_str());
  return ok;
}

std::string padded_body() {
  std::string b = "{\"order_id\":\"9f1c\",\"client_order_id\":\"M-1\",\"fill_count\":\"10.00\",\"remaining_count\":\"0.00\",\"ts_ms\":1}";
  b.resize(109, ' ');
  return b;
}

const std::string kCreated =
    "HTTP/1.1 201 Created\r\n"
    "Date: Wed, 07 Oct 2026 02:10:00 GMT\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 109\r\n"
    "Connection: keep-alive\r\n"
    "\r\n" +
    padded_body();

const std::string kChunked =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Content-Type: application/json\r\n"
    "\r\n"
    "7;name=value\r\n{\"a\":1,\r\n"
    "0A ; quoted=\"x;\\\"y\" ; flag\r\n\"b\":[1,2],\r\n"
    "0004\r\n\"c\":\r\n"
    "2\r\n3}\r\n"
    "0;last=yes\r\n"
    "X-Trailer-One: 1\r\n"
    "x-trailer-two:  two \r\n"
    "\r\n";

}  // namespace

static void test_content_length() {
  const Fed f = run1(kCreated);
  CHECK(f.st == St::DONE);
  CHECK(f.res.status == 201 && f.res.minor == 1 && f.res.reason == "Created");
  CHECK(f.res.body.size() == 109 && f.res.body.compare(0, 13, "{\"order_id\":\"") == 0);
  CHECK(f.res.header("content-type") == "application/json");
  CHECK(f.res.header("content-length") == "109");
  CHECK(f.res.keep_alive && !f.res.chunked && f.leftover.empty());
  every_split(kCreated, false, "Content-Length reply");
  // header names come out lower-cased, values trimmed of spaces and tabs
  const Fed g = run1("HTTP/1.1 200 OK\r\nX-Odd-Name:\t  v a l  \t\r\nContent-Length: 2\r\n\r\nok");
  CHECK(g.st == St::DONE && g.res.header("x-odd-name") == "v a l" && g.res.body == "ok");
  // a repeated field joins with ", " (RFC 9110 5.3)
  const Fed h = run1("HTTP/1.1 200 OK\r\nVia: a\r\nVia: b\r\nContent-Length: 0\r\n\r\n");
  CHECK(h.st == St::DONE && h.res.header("via") == "a, b" && h.res.body.empty());
  // the same length repeated is one length (RFC 9112 6.3: "5, 5")
  CHECK(run1("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\nContent-Length: 5\r\n\r\nhello").st == St::DONE);
  // a reason phrase may be empty, with or without its space
  CHECK(run1("HTTP/1.1 204 \r\n\r\n").st == St::DONE);
  CHECK(run1("HTTP/1.1 204\r\n\r\n").st == St::DONE);
  CHECK(run1("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n").res.body.empty());
}

static void test_chunked() {
  const Fed f = run1(kChunked);
  CHECK(f.st == St::DONE);
  CHECK(f.res.chunked && f.res.body == "{\"a\":1,\"b\":[1,2],\"c\":3}");
  CHECK(f.res.trailers.size() == 2);
  CHECK(f.res.trailers.size() == 2 && f.res.trailers[0].first == "x-trailer-one" && f.res.trailers[1].second == "two");
  CHECK(!f.res.has_header("x-trailer-one"));   // trailers are kept apart from the head
  every_split(kChunked, false, "chunked reply");
  // upper-case hex, one chunk per byte, no trailers
  std::string one = "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n";
  const std::string text = "0123456789ABCDEF";
  for (char c : text) one += std::string("1\r\n") + c + "\r\n";
  one += "1A\r\n" + std::string(26, 'z') + "\r\n0\r\n\r\n";
  const Fed g = run1(one);
  CHECK(g.st == St::DONE && g.res.body == text + std::string(26, 'z'));
  every_split(one, false, "one byte per chunk");
}

static void test_no_body_statuses() {
  // 204 and 304 carry no body whatever their head says; a HEAD's reply neither
  const Fed a = run1("HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\nNEXT");
  CHECK(a.st == St::DONE && a.res.body.empty() && a.leftover == "NEXT");
  CHECK(run1("HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n").st == St::DONE);
  const Fed h = run1("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n", false, true);
  CHECK(h.st == St::DONE && h.res.body.empty());
  // 100 Continue (and 103) are read and skipped; the real reply follows on the same bytes
  const std::string interim = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n" + kCreated;
  const Fed c = run1(interim);
  CHECK(c.st == St::DONE && c.res.status == 201 && !c.res.has_header("link") && c.res.body.size() == 109);
  every_split(interim, false, "1xx then 201");
}

static void test_close_framed_and_keep_alive() {
  // neither length nor chunking: the body runs to the close, and the connection is spent
  const std::string w = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nall of it";
  const Fed open = run1(w);
  CHECK(open.st == St::NEED_MORE);
  const Fed f = run1(w, true);
  CHECK(f.st == St::DONE && f.res.body == "all of it" && !f.res.keep_alive);
  every_split(w, true, "close-framed body");
  CHECK(!run1("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n").res.keep_alive);
  CHECK(!run1("HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\nContent-Length: 0\r\n\r\n").res.keep_alive);
  CHECK(!run1("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n").res.keep_alive);
  CHECK(run1("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n").res.keep_alive);
  CHECK(run1("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n").res.keep_alive);
  // bytes after the reply stay unread (a second reply nobody asked for)
  const Fed l = run1("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\n");
  CHECK(l.st == St::DONE && l.res.body == "ok" && l.leftover == "HTTP/1.1 200 OK\r\n");
}

static void test_refusals() {
  CHECK(refused("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n", "bare LF"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: 0\n\r\n", "bare LF"));
  CHECK(refused("HTTP/1.1 200 OK\r\nX: a\rb\r\nContent-Length: 0\r\n\r\n", "bare CR"));
  CHECK(refused("HTTP/1.1 200 OK\r\nX: a\r\n  folded\r\nContent-Length: 0\r\n\r\n", "folding"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n", "whitespace before"));
  CHECK(refused("HTTP/1.1 200 OK\r\n: v\r\n\r\n", "empty header name"));
  CHECK(refused("HTTP/1.1 200 OK\r\nX(y): v\r\n\r\n", "bad character in a header name"));
  CHECK(refused("HTTP/1.1 200 OK\r\nNoColon\r\n\r\n", "without a colon"));
  CHECK(refused("HTTP/1.1 200 OK\r\nX: a\x01" "b\r\n\r\n", "control character"));
  CHECK(refused("HTTP/2 200 OK\r\n\r\n", "bad status line"));
  CHECK(refused("HTTP/1.2 200 OK\r\n\r\n", "bad status line"));
  CHECK(refused("http/1.1 200 OK\r\n\r\n", "bad status line"));
  CHECK(refused("HTTP/1.1 20 OK\r\n\r\n", "bad status"));
  CHECK(refused("HTTP/1.1 200OK\r\n\r\n", "bad status line"));
  CHECK(refused("HTTP/1.1  200 OK\r\n\r\n", "bad status"));
  CHECK(refused("HTTP/1.1 099 Low\r\n\r\n", "out of range"));
  CHECK(refused("HTTP/1.1 600 High\r\n\r\n", "out of range"));
  CHECK(refused("HTTP/1.1 101 Switching Protocols\r\n\r\n", "101"));
  CHECK(refused("HTTP/1.1 200 O\x7fK\r\n\r\n", "reason phrase"));
  CHECK(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n", "both"));
  CHECK(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", "only chunked"));
  CHECK(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", "only chunked"));
  CHECK(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", "only chunked"));
  CHECK(refused("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", "HTTP/1.0"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0\r\n\r\n", "Content-Encoding"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", "differ"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\nhello!", "differ"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", "bad Content-Length"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", "bad Content-Length"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\n", "bad Content-Length"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", "bad Content-Length"));
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n", "empty Content-Length"));
  // chunk framing
  const std::string te = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  CHECK(refused(te + "zz\r\nab\r\n0\r\n\r\n", "hex size"));
  CHECK(refused(te + ";ext\r\n\r\n", "hex size"));
  CHECK(refused(te + "2 x\r\nab\r\n0\r\n\r\n", "bad chunk extension"));
  CHECK(refused(te + "2;\r\nab\r\n0\r\n\r\n", "without a name"));
  CHECK(refused(te + "2;a=\r\nab\r\n0\r\n\r\n", "without a value"));
  CHECK(refused(te + "2;a=\"open\r\nab\r\n0\r\n\r\n", "unclosed"));
  CHECK(refused(te + "2\r\nabc\r\n0\r\n\r\n", "longer than its size"));
  CHECK(refused(te + "2\r\nab\n0\r\n\r\n", "longer than its size"));
  CHECK(refused(te + "FFFFFFFFFFFFFFFFF\r\n", "overflows"));
  CHECK(refused(te + "0\r\nBad Trailer\r\n\r\n", "trailer"));
  // limits
  ResponseParser::Limits small;
  small.max_body = 100;
  CHECK(refused("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n", "over the 100 limit", false, false, small));
  CHECK(refused(te + "40\r\n" + std::string(64, 'a') + "\r\n40\r\n", "byte limit", false, false, small));
  CHECK(refused("HTTP/1.1 200 OK\r\n\r\n" + std::string(101, 'x'), "byte limit", false, false, small));
  ResponseParser::Limits lines;
  lines.max_line = 64;
  CHECK(refused("HTTP/1.1 200 OK\r\nX: " + std::string(80, 'v') + "\r\n\r\n", "line over 64", false, false, lines));
  CHECK(refused("HTTP/1.1 200 OK\r\nX: " + std::string(80, 'v'), "line over 64", false, false, lines));   // no CRLF yet
  ResponseParser::Limits head;
  head.max_head = 200;
  std::string many = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 20; ++i) many += "X-" + std::to_string(i) + ": value\r\n";
  CHECK(refused(many + "\r\n", "head over 200", false, false, head));
  ResponseParser::Limits count;
  count.max_headers = 5;
  CHECK(refused(many + "\r\n", "more than 5 header", false, false, count));
  // a refusal is final: more bytes change nothing
  ResponseParser p;
  CHECK(p.feed("HTTP/9.9 200 OK\r\n\r\n") == St::ERROR);
  CHECK(p.feed(kCreated) == St::ERROR && p.eof() == St::ERROR);
}

// The connection closes at every byte of a reply: nothing, inside the head, inside the body. Only
// a whole reply is a reply; every shorter one is a cut, and says where.
static void test_cuts() {
  int wrong = 0;
  const std::size_t head_end = kCreated.find("\r\n\r\n") + 4;
  for (std::size_t i = 0; i < kCreated.size(); ++i) {
    const Fed f = run1(kCreated.substr(0, i), true);
    const Cut want = i == 0 ? Cut::NO_BYTES : (i < head_end ? Cut::IN_HEAD : Cut::IN_BODY);
    if (f.st != St::ERROR || f.cut != want) ++wrong;
  }
  CHECK(wrong == 0);
  CHECK(run1(kCreated, true).st == St::DONE);   // whole before the close: a reply
  const Fed mid = run1(kCreated.substr(0, head_end + 50), true);
  CHECK(mid.err == "body cut at 50 of 109 bytes");
  CHECK(run1("", true).err == "closed before any reply");

  wrong = 0;
  const std::size_t chead = kChunked.find("\r\n\r\n") + 4;
  for (std::size_t i = 0; i < kChunked.size(); ++i) {
    const Fed f = run1(kChunked.substr(0, i), true);
    const Cut want = i == 0 ? Cut::NO_BYTES : (i < chead ? Cut::IN_HEAD : Cut::IN_BODY);
    if (f.st != St::ERROR || f.cut != want) ++wrong;
  }
  CHECK(wrong == 0);
  // cut between chunks, after the last chunk but before the trailers end
  const Fed between = run1(kChunked.substr(0, kChunked.find("0004")), true);
  CHECK(between.cut == Cut::IN_BODY && between.err.find("chunked body cut after 17 bytes") != std::string::npos);
  const Fed in_trailers = run1(kChunked.substr(0, kChunked.size() - 2), true);
  CHECK(in_trailers.cut == Cut::IN_BODY);
  // a 1xx alone, then the close: still no reply
  CHECK(run1("HTTP/1.1 100 Continue\r\n\r\n", true).cut == Cut::IN_HEAD);
}

static void test_request_bytes() {
  const std::string r = request_bytes("POST", "demo-api.kalshi.co", "/trade-api/v2/portfolio/events/orders",
                                      {{"Content-Type", "application/json"}}, "{\"a\":1}", false);
  CHECK(r ==
        "POST /trade-api/v2/portfolio/events/orders HTTP/1.1\r\nHost: demo-api.kalshi.co\r\n"
        "Content-Type: application/json\r\nContent-Length: 7\r\nConnection: close\r\n\r\n{\"a\":1}");
  const std::string g = request_bytes("GET", "127.0.0.1:9000", "/x?a=1", {}, "", true);
  CHECK(g == "GET /x?a=1 HTTP/1.1\r\nHost: 127.0.0.1:9000\r\nConnection: keep-alive\r\n\r\n");
  // a POST with no body still says Content-Length: 0
  CHECK(request_bytes("POST", "h", "/p", {}, "", false).find("Content-Length: 0\r\n") != std::string::npos);
  // no header injection: CR, LF or NUL anywhere, a target without '/' or with a space
  CHECK(request_bytes("GET", "h", "/p", {{"X", "a\r\nEvil: 1"}}, "", true).empty());
  CHECK(request_bytes("GET", "h\r\nX: y", "/p", {}, "", true).empty());
  CHECK(request_bytes("GET", "h", "/p HTTP/1.0\r\n", {}, "", true).empty());
  CHECK(request_bytes("GET", "h", "p", {}, "", true).empty());
  CHECK(request_bytes("GET", "h", "/a b", {}, "", true).empty());
  CHECK(request_bytes("GET", "h", "/p", {{std::string("X\0", 2), "v"}}, "", true).empty());
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_content_length();
  test_chunked();
  test_no_body_statuses();
  test_close_framed_and_keep_alive();
  test_refusals();
  test_cuts();
  test_request_bytes();
  return finish("deskd-http");
}
