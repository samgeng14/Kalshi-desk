#include "deskd/web/server.h"

#include <httplib.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

#include "deskd/app/log.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kill.h"
#include "deskd/net/send.h"
#include "deskd/web/contract.h"
#include "deskd/web/pub.h"

namespace deskd {

using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// guards
// ---------------------------------------------------------------------------------------------
bool host_ok(const std::string& host, int port) {
  std::string h;
  h.reserve(host.size());
  for (char c : host) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  const std::string p = std::to_string(port);
  return h == "127.0.0.1:" + p || h == "localhost:" + p;
}

std::string cross_site_refusal(const std::string& sec_fetch_site, const std::string* origin, int port) {
  auto lower = [](std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
  };
  const std::string site = lower(sec_fetch_site);
  if (site == "cross-site" || site == "same-site") return "Sec-Fetch-Site " + site + ": the desk answers its own page only";
  if (origin) {
    const std::string o = lower(*origin), p = std::to_string(port);
    if (o != "http://127.0.0.1:" + p && o != "http://localhost:" + p)
      return "Origin " + origin->substr(0, 80) + " is not this desk's page";
  }
  return "";
}

bool token_ok(const std::string& got, const std::string& want) {
  if (want.empty() || got.size() != want.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < want.size(); ++i) diff |= static_cast<unsigned char>(got[i] ^ want[i]);
  return diff == 0;
}

bool peer_navigation(const std::string& method, const std::string& path, const std::string& site,
                     const std::string& mode, const std::string& dest, bool has_origin) {
  auto lower = [](std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
  };
  return method == "GET" && (path == "/" || path == "/index.html") && lower(site) == "same-site" &&
         lower(mode) == "navigate" && lower(dest) == "document" && !has_origin;
}

bool inject_token(const std::string& html, const std::string& token, std::string* out) {
  static const std::string kEmpty = "<meta name=\"desk-token\" content=\"\">";
  const std::size_t at = html.find(kEmpty);
  if (at == std::string::npos) return false;
  *out = html.substr(0, at) + "<meta name=\"desk-token\" content=\"" + token + "\">" + html.substr(at + kEmpty.size());
  return true;
}

bool inject_meta(const std::string& html, const std::string& name, const std::string& value, std::string* out) {
  const std::string empty = "<meta name=\"" + name + "\" content=\"\">";
  const std::size_t at = html.find(empty);
  if (at == std::string::npos) return false;
  std::string v;
  for (char c : value) {
    switch (c) {
      case '&': v += "&amp;"; break;
      case '<': v += "&lt;"; break;
      case '>': v += "&gt;"; break;
      case '"': v += "&quot;"; break;
      case '\'': v += "&#39;"; break;
      default: v.push_back(c);
    }
  }
  *out = html.substr(0, at) + "<meta name=\"" + name + "\" content=\"" + v + "\">" + html.substr(at + empty.size());
  return true;
}

std::string random_token() {
  unsigned char b[16];
#ifdef __APPLE__
  arc4random_buf(b, sizeof b);
#else
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f || std::fread(b, 1, sizeof b, f) != sizeof b) std::abort();   // no token, no desk
  std::fclose(f);
#endif
  static const char* hex = "0123456789abcdef";
  std::string t;
  for (unsigned char c : b) {
    t.push_back(hex[c >> 4]);
    t.push_back(hex[c & 15]);
  }
  return t;
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
namespace {

void reply_json(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_header("Cache-Control", "no-store");
  res.set_content(body.dump(), "application/json");
}
void reply_err(httplib::Response& res, const std::string& code, const std::string& msg) {
  reply_json(res, err_http(code), err_body(code, msg));
}
// The refusal, then the connection closed under it. A request refused before httplib reads its
// body (a POST refused in the pre-routing hook, a GET that carries a body) leaves that body on
// the socket, and on a kept-open connection the next request would be parsed out of it: a
// cross-site form POST can carry a whole second request in its text/plain body. cpp-httplib
// 0.18.3 keeps the connection whatever the reply's headers say (only the request's own
// "Connection: close" ends it), so the reply goes out through a content provider that writes it
// all and then reports failure, the one path on which httplib closes the socket.
void reply_close(httplib::Response& res, const std::string& code, const std::string& msg) {
  auto body = std::make_shared<std::string>(err_body(code, msg).dump());
  res.status = err_http(code);
  res.set_header("Cache-Control", "no-store");
  res.set_header("Connection", "close");   // also tells the error handler the reply is ours
  res.set_content_provider(body->size(), "application/json",
                           [body](std::size_t off, std::size_t len, httplib::DataSink& sink) {
                             sink.write(body->data() + off, std::min(len, body->size() - off));
                             return false;   // all written: "failed" so httplib closes
                           });
}

bool re_chars(const std::string& s, std::size_t lo, std::size_t hi, const char* extra) {
  if (s.size() < lo || s.size() > hi) return false;
  for (char c : s) {
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && !std::strchr(extra, c)) return false;
  }
  return true;
}
// a stream id: the page makes 8 random characters; the contract asks for 8 or more
bool sid_ok(const std::string& s) { return re_chars(s, 8, 64, "_-"); }
bool tk_ok(const std::string& s) { return re_chars(s, 1, 96, "._-"); }   // contract RE_TK

bool is_str(const json& j, const char* k) { return j.contains(k) && j[k].is_string(); }
// a {v, tk} market reference, strictly
bool parse_ref(const json& j, MarketRef* out) {
  if (!j.is_object() || j.size() != 2 || !is_str(j, "v") || !is_str(j, "tk")) return false;
  const std::string v = j["v"], tk = j["tk"];
  if ((v != "K" && v != "P") || !tk_ok(tk)) return false;
  out->v = v == "K" ? Venue::K : Venue::P;
  out->tk = tk;
  return true;
}

bool read_file(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

const char* content_type_for(const std::string& path) {
  auto ends = [&path](const char* ext) {
    const std::size_t n = std::strlen(ext);
    return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
  };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".js")) return "text/javascript; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".json")) return "application/json";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".png")) return "image/png";
  if (ends(".woff2")) return "font/woff2";
  return nullptr;
}

// a page asset path under the ui dir: no "..", no hidden files, a known type
bool asset_path_ok(const std::string& p) {
  if (p.empty() || p[0] == '/' || p.find("..") != std::string::npos || p.find("//") != std::string::npos) return false;
  if (p[0] == '.' || p.find("/.") != std::string::npos) return false;
  return re_chars(p, 1, 200, "._/-") && content_type_for(p) != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// the server
// ---------------------------------------------------------------------------------------------
struct WebServer::Impl {
  explicit Impl(WebDeps deps) : d(std::move(deps)) {}

  WebDeps d;
  httplib::Server svr;
  std::atomic<int> port{0};
  std::atomic<std::uint64_t> requests{0};
  std::atomic<bool> stopping{false};
  std::atomic<int> waiting{0};   // request threads blocked on gw or pub right now
  std::atomic<int> order_waiting{0};   // request threads blocked on a Manual order's venue reply

  using Handler = std::function<void(const httplib::Request&, httplib::Response&, const json&)>;

  // Why the request is refused before anything else, or "": exactly one Host, and it names this
  // port on 127.0.0.1 or localhost (DNS rebinding); no cross-site or same-site Sec-Fetch-Site and
  // no Origin but this desk's own page. The Host rule alone let any page on another site hold the
  // 4 stream slots with <script src> or a no-cors fetch, which carry Host 127.0.0.1:8788 (review,
  // 2026-10-06: four such streams and the page's own got 409, the screen dark while Systematic
  // kept trading). deskctl, curl and the ops probe send neither header and pass.
  std::string refusal(const httplib::Request& req) const {
    const int p = port.load();
    if (req.get_header_value_count("Host") != 1)
      return "exactly one Host header, 127.0.0.1:" + std::to_string(p) + " or localhost:" + std::to_string(p);
    if (!host_ok(req.get_header_value("Host"), p))
      return "Host must be 127.0.0.1:" + std::to_string(p) + " or localhost:" + std::to_string(p);
    if (req.get_header_value_count("Origin") > 1 || req.get_header_value_count("Sec-Fetch-Site") > 1)
      return "one Origin and one Sec-Fetch-Site at most";
    for (const char* h : {"Sec-Fetch-Mode", "Sec-Fetch-Dest"})
      if (req.get_header_value_count(h) > 1) return std::string("one ") + h + " at most";
    if (peer_navigation(req.method, req.path, req.get_header_value("Sec-Fetch-Site"), req.get_header_value("Sec-Fetch-Mode"),
                        req.get_header_value("Sec-Fetch-Dest"), req.has_header("Origin")))
      return "";
    const std::string origin = req.get_header_value("Origin");
    return cross_site_refusal(req.get_header_value("Sec-Fetch-Site"), req.has_header("Origin") ? &origin : nullptr, p);
  }

  // Every POST, after the pre-routing hook passed its Host and origin and httplib read its body:
  // the token, then a JSON object, then the contract's request schema for `key`
  // (check_request), and only then the handler. KILL and Stop take any body (key ""): a safety
  // action is never refused for its shape, only for Host, origin and token.
  httplib::Server::Handler post(const std::string& key, Handler h) {
    return [this, key, h](const httplib::Request& req, httplib::Response& res) {
      if (!token_ok(req.get_header_value("X-Desk"), d.token))
        return reply_err(res, "FORBIDDEN", "X-Desk token missing or not this start's");
      json body = json::object();
      if (!key.empty()) {
        const std::string ct = req.get_header_value("Content-Type");
        if (ct.compare(0, 16, "application/json") != 0)
          return reply_err(res, "BAD_REQUEST", "Content-Type must be application/json");
        try {
          body = json::parse(req.body);
        } catch (const json::exception&) {
          return reply_err(res, "BAD_REQUEST", "body is not JSON");
        }
        const std::vector<std::string> errs = check_request(key, body);
        if (!errs.empty()) {
          std::string msg = errs[0];
          for (std::size_t i = 1; i < errs.size() && i < 3; ++i) msg += "; " + errs[i];
          return reply_err(res, "BAD_REQUEST", msg);
        }
      }
      h(req, res, body);
    };
  }

  // A request thread waits on an owner (gw, pub) only through this, and at most kMaxWaiting of
  // the 16 wait at once: a frozen owner then holds 4 threads, never the pool. The rest answer
  // INTERNAL at once.
  static constexpr int kMaxWaiting = 4;
  struct Waiter {
    std::atomic<int>& n;
    bool ok;
    explicit Waiter(std::atomic<int>& c) : n(c), ok(c.fetch_add(1) < kMaxWaiting) {}
    ~Waiter() { n.fetch_sub(1); }
  };

  // Start and Stop: gw answers in microseconds; the web thread waits at most 50 ms for it (plan
  // section 2), then says the request is queued and the stream's hdr carries the outcome. A Stop
  // stays queued in gw's control lane and is applied when gw turns again.
  void control_reply(httplib::Response& res, const ControlPromise& p, bool posted, const char* what) {
    if (!posted) return reply_err(res, "INTERNAL", std::string("gw inbox full: ") + what + " not queued");
    auto f = p->get_future();
    Waiter w(waiting);
    if (!w.ok)
      return reply_err(res, "INTERNAL", std::string(what) + " queued; " + std::to_string(kMaxWaiting) +
                                            " requests already wait on gw or pub: the stream's hdr shows the outcome");
    if (f.wait_for(std::chrono::milliseconds(kGwWaitMs)) != std::future_status::ready)
      return reply_err(res, "INTERNAL", std::string(what) + " queued; gw has not answered within " +
                                            std::to_string(kGwWaitMs) + " ms: the stream's hdr shows the outcome");
    ControlReply r = f.get();
    if (r.code.empty()) return reply_json(res, 200, json{{"ok", true}, {"run", run_json(r.run)}, {"n", r.n}});
    json body = err_body(r.code, r.msg);
    if (r.code == "PREFLIGHT") body["preflight"] = preflight_json(r.preflight);
    reply_json(res, err_http(r.code), body);
  }
  static constexpr int kGwWaitMs = 50;

  // A new order: gw admits it in microseconds, then the reply waits for the venue (send-k1's
  // connect and reply budgets, 1 s + 1.5 s). At most kMaxOrderWaiting request threads wait on
  // orders, apart from kMaxWaiting. Past the cap or the wait, the answer is a 5xx without ord,
  // which the contract defines: the page keeps the order PEND and the stream carries its state.
  static constexpr int kMaxOrderWaiting = 6;
  static constexpr int kOrderWaitMs = 3000;
  struct OrderWaiter {
    std::atomic<int>& n;
    bool ok;
    explicit OrderWaiter(std::atomic<int>& c) : n(c), ok(c.fetch_add(1) < kMaxOrderWaiting) {}
    ~OrderWaiter() { n.fetch_sub(1); }
  };
  void order_route(httplib::Response& res, const json& b) {
    PlaceIntent p;
    p.cid = b["cid"].get<std::string>();
    p.owner = Owner::MAN;
    if (b["v"] != "K") return reply_err(res, "UNKNOWN_MARKET", "P|" + b["tk"].get<std::string>() + ": Polymarket is not connected");
    p.venue = Venue::K;
    p.ticker = b["tk"].get<std::string>();
    p.side = b["side"] == "BUY" ? Side::BUY : Side::SELL;
    const auto px = parse_price(b["px"].get<std::string>());
    const auto qty = qty_from_number(b["qty"].get<double>());
    if (!px) return reply_err(res, "OFF_GRID", "price is not a contract price");
    if (!qty || *qty <= 0) return reply_err(res, "BAD_QTY", "size must be positive with at most 2 decimals");
    p.price = *px;
    p.count = *qty;
    const std::string tif = b["tif"].get<std::string>();
    p.tif = tif == "POST" ? Tif::POST : tif == "FOK" ? Tif::FOK : Tif::IOC;
    if (b.contains("ttl_s") && !b["ttl_s"].is_null()) p.ttl_s = b["ttl_s"].get<int>();
    if (b.contains("exp_ms") && !b["exp_ms"].is_null()) p.exp_ms = b["exp_ms"].get<std::int64_t>();
    p.reduce_only = b.contains("reduce_only") && b["reduce_only"].is_boolean() && b["reduce_only"].get<bool>();
    p.override_guard = b["override"].get<bool>();
    if (b.contains("t_click_ms") && !b["t_click_ms"].is_null()) p.t_click_ms = b["t_click_ms"].get<std::int64_t>();
    if (b.contains("touch") && b["touch"].is_string()) p.touch = parse_price(b["touch"].get<std::string>());
    p.t_recv_ms = now_ms();
    p.reply = std::make_shared<std::promise<OrderReply>>();
    auto f = p.reply->get_future();
    OrderWaiter w(order_waiting);
    if (!d.gw->post(std::move(p))) return reply_err(res, "RO", "gw inbox full: the order was not queued, nothing was sent");
    if (!w.ok)
      return reply_err(res, "INTERNAL", "order queued; " + std::to_string(kMaxOrderWaiting) +
                                            " orders already wait on the venue: the stream carries its state");
    if (f.wait_for(std::chrono::milliseconds(kOrderWaitMs)) != std::future_status::ready)
      return reply_err(res, "INTERNAL", "order queued; no answer within " + std::to_string(kOrderWaitMs) +
                                            " ms: the stream carries its state");
    const OrderReply r = f.get();
    json ord = r.ord.empty() ? json(nullptr) : json::parse(r.ord, nullptr, false);
    if (ord.is_discarded()) return reply_err(res, "INTERNAL", "gw returned an unreadable order row");
    if (r.code.empty()) {
      if (ord.is_null()) return reply_err(res, "INTERNAL", "gw accepted the order without its row");
      return reply_json(res, 200, json{{"ok", true}, {"ord", ord}});
    }
    json body = err_body(r.code, r.msg);
    if (!ord.is_null()) body["ord"] = ord;
    reply_json(res, err_http(r.code), body);
  }

  // Cancels: gw answers at once with the orders it asked to cancel (each then gets its ord).
  void cancel_route(httplib::Response& res, std::vector<CancelIntent> intents) {
    std::vector<std::future<CancelReply>> futures;
    for (auto& c : intents) {
      c.reply = std::make_shared<std::promise<CancelReply>>();
      futures.push_back(c.reply->get_future());
      if (!d.gw->post(std::move(c))) return reply_err(res, "EXCHANGE", "gw inbox full: cancel not queued; use KILL if it must go now");
    }
    Waiter w(waiting);
    if (!w.ok) return reply_err(res, "EXCHANGE", "cancel queued; gw is busy: the stream shows each order's state");
    json cids = json::array();
    for (auto& f : futures) {
      if (f.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready)
        return reply_err(res, "EXCHANGE", "cancel queued; gw has not answered: the stream shows each order's state");
      const CancelReply r = f.get();
      if (!r.code.empty()) return reply_err(res, r.code, r.msg);
      for (const auto& cid : r.cids) cids.push_back(cid);
    }
    reply_json(res, 200, json{{"ok", true}, {"n", cids.size()}, {"cids", cids}});
  }

  // Snapshot and subscribe: the contract's answer is the data itself, so these wait for pub (up
  // to 2 s), but under the same cap.
  PubReply pub_call(PubMsg m, PubPromise p) {
    auto f = p->get_future();
    Waiter w(waiting);
    if (!w.ok) return PubReply{500, err_body("INTERNAL", std::to_string(kMaxWaiting) + " requests already wait on gw or pub")};
    if (!d.pub->post(std::move(m))) return PubReply{500, err_body("INTERNAL", "pub inbox full")};
    if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
      return PubReply{500, err_body("INTERNAL", "pub did not answer in 2 s")};
    return f.get();
  }

  // true once send-cx has handled kill generation gen (or a later one), polled for up to ms
  bool kill_taken(std::uint64_t gen, int ms) const {
    if (!d.cx_stats) return false;
    const std::int64_t end = mono_ms() + ms;
    while (d.cx_stats->kill_gen_seen.load() < gen) {
      if (mono_ms() >= end) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  void serve_page(httplib::Response& res) {
    std::string html, page;
    if (!read_file(d.ui_dir + "/index.html", &html)) {
      const int e = errno;
      return reply_err(res, "INTERNAL", "cannot read " + d.ui_dir + "/index.html: " + std::strerror(e));
    }
    if (!inject_token(html, d.token, &page))
      return reply_err(res, "INTERNAL", "index.html has no empty desk-token meta to write the token into");
    // the PAPER/LIVE switch's metas: an older page without them still works, the tag just stays put
    for (const auto& kv : {std::make_pair("desk-peer", d.peer_url), std::make_pair("desk-venue", d.venue)}) {
      std::string next;
      if (!kv.second.empty() && inject_meta(page, kv.first, kv.second, &next)) page.swap(next);
    }
    res.set_header("Cache-Control", "no-store");   // the token changes every start
    res.set_content(page, "text/html; charset=utf-8");
  }

  void routes();
};

void WebServer::Impl::routes() {
  svr.new_task_queue = [n = d.threads] { return new httplib::ThreadPool(static_cast<std::size_t>(n)); };
  svr.set_keep_alive_timeout(d.keepalive_s);
  svr.set_keep_alive_max_count(1000);   // the order POST rides a warm connection
  svr.set_read_timeout(5, 0);
  svr.set_write_timeout(5, 0);
  svr.set_tcp_nodelay(true);
  svr.set_payload_max_length(64 * 1024);

  // Every request, every method, before routing and before httplib reads a body: Host and
  // origin first ("FORBIDDEN ... before anything else", contract). A POST refused here has its
  // body unread, so its connection is closed after the reply (reply_close); so is a GET that
  // carries a body, which httplib never reads.
  svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    requests.fetch_add(1, std::memory_order_relaxed);
    const bool has_body = req.method == "POST" || req.method == "PUT" || req.method == "PATCH" || req.method == "DELETE";
    const std::string why = refusal(req);
    if (!why.empty()) {
      if (has_body) reply_close(res, "FORBIDDEN", why);
      else reply_err(res, "FORBIDDEN", why);
      return httplib::Server::HandlerResponse::Handled;
    }
    if (!has_body && (req.get_header_value_u64("Content-Length") > 0 || req.has_header("Transfer-Encoding"))) {
      reply_close(res, "BAD_REQUEST", req.method + " with a body");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  svr.set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
    if (!res.body.empty() || res.has_header("Connection")) return httplib::Server::HandlerResponse::Unhandled;   // ours
    if (res.status == 413) {
      // over 64 KiB: httplib skipped the body without reading it into memory. Host, origin and
      // token still come first.
      const std::string why = refusal(req);
      if (!why.empty()) reply_err(res, "FORBIDDEN", why);
      else if (req.method == "POST" && !token_ok(req.get_header_value("X-Desk"), d.token))
        reply_err(res, "FORBIDDEN", "X-Desk token missing or not this start's");
      else reply_err(res, "BAD_REQUEST", "body over 64 KiB");
      return httplib::Server::HandlerResponse::Handled;
    }
    if (res.status == 404) reply_err(res, "NOT_FOUND", "no such endpoint");
    else if (res.status < 500) reply_err(res, "BAD_REQUEST", "HTTP " + std::to_string(res.status));
    else reply_err(res, "INTERNAL", "HTTP " + std::to_string(res.status));
    return httplib::Server::HandlerResponse::Handled;
  });
  svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
    std::string what = "unknown exception";
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
    }
    DLOG_ERR("%s %s: %s", req.method.c_str(), req.path.c_str(), what.c_str());
    reply_err(res, "INTERNAL", what);
  });

  // ---- the stream --------------------------------------------------------------------------
  svr.Get("/api/stream", [this](const httplib::Request& req, httplib::Response& res) {
    if (stopping.load()) return reply_err(res, "BAD_STATE", "shutting down");
    const std::string sid = req.get_param_value("sid");
    if (!sid_ok(sid)) return reply_err(res, "BAD_REQUEST", "sid: 8 to 64 of A-Z a-z 0-9 _ -");
    const std::int64_t now = now_ms();
    auto s = std::make_shared<Stream>(sid, d.pub->stream_cap(), mono_ms());
    // seq 1 is an hb, queued before pub can see the stream. The page ignores Last-Event-ID's
    // resume option: every open starts at 1 and the page resyncs.
    s->q.offer("hb", now, d.pub->hb_data(sid, now));
    std::string why;
    if (!d.pub->streams().open(s, &why)) return reply_err(res, "BAD_STATE", "stream refused: " + why);
    res.set_header("Cache-Control", "no-store");
    auto last_write = std::make_shared<std::int64_t>(0);
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, s, last_write](std::size_t, httplib::DataSink& sink) {
          std::string out;
          if (*last_write == 0) out = "retry: 2000\n\n";   // EventSource reconnects 2 s after a drop
          if (s->q.closed() || stopping.load()) {
            sink.done();
            return true;
          }
          out += s->q.drain_for(std::chrono::milliseconds(*last_write == 0 ? 0 : 250));
          const std::int64_t t = mono_ms();
          if (out.empty() && t - *last_write >= 15000) out = ": ka\n\n";   // 15 s quiet: a comment for proxies
          if (out.empty()) return true;
          *last_write = t;
          return sink.write(out.data(), out.size());
        },
        [this, s](bool) { d.pub->streams().closed(s, mono_ms()); });
  });

  svr.Get("/api/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
    const std::string sid = req.get_param_value("sid");
    if (!sid_ok(sid)) return reply_err(res, "BAD_STATE", "no open stream for that sid");
    auto p = std::make_shared<std::promise<PubReply>>();
    PubReply r = pub_call(PubSnapshot{sid, p}, p);
    reply_json(res, r.status, r.body);
  });

  svr.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, json{{"ok", true}, {"epoch", d.pub->epoch()}, {"srv_ms", now_ms()}});
  });

  // THE UNIVERSE, paged (contract 1.5). Until gw has a universe the reply is an empty page for
  // the day asked, after the same query check contract.js makes.
  svr.Get("/api/universe", [this](const httplib::Request& req, httplib::Response& res) {
    json q = json::object();
    for (const auto& kv : req.params) {   // contract.js parseQuery: the last value of a key wins
      const std::string& v = kv.second;
      const bool digits = !v.empty() && v.size() < 16 && v.find_first_not_of("0123456789") == std::string::npos;
      q[kv.first] = (kv.first == "offset" || kv.first == "limit") && digits ? json(std::stoll(v)) : json(v);
    }
    const std::vector<std::string> errs = check_universe_query(q);
    if (!errs.empty()) return reply_err(res, "BAD_REQUEST", errs[0]);
    reply_json(res, 200, d.universe ? d.universe(q) : universe_json(q, now_ms()));
  });

  // How likely a resting order is to fill (contract 1.6): the ticket's FILL line and P80 price
  svr.Get("/api/fill", [this](const httplib::Request& req, httplib::Response& res) {
    json q = json::object();
    for (const auto& kv : req.params) {   // contract.js parseQuery: ttl a count, qty a size, the rest strings
      const std::string& v = kv.second;
      const bool digits = !v.empty() && v.size() < 10 && v.find_first_not_of("0123456789") == std::string::npos;
      const auto dot = v.find('.');
      const bool decimal = !v.empty() && v.size() < 14 && v.find_first_not_of("0123456789.") == std::string::npos &&
                           dot != 0 && v.find('.', dot == std::string::npos ? v.size() : dot + 1) == std::string::npos &&
                           (dot == std::string::npos || (v.size() - dot - 1 >= 1 && v.size() - dot - 1 <= 2));
      if (kv.first == "ttl" && digits) q[kv.first] = std::stoll(v);
      else if (kv.first == "qty" && decimal) q[kv.first] = std::stod(v);
      else q[kv.first] = v;
    }
    const std::vector<std::string> errs = check_fill_query(q);
    if (!errs.empty()) return reply_err(res, "BAD_REQUEST", errs[0]);
    if (!d.fill) return reply_err(res, "UNKNOWN_MARKET", "no venue connected: nothing to estimate");
    const json out = d.fill(q);
    reply_json(res, out.value("ok", false) ? 200 : err_http(out.at("err").at("code")), out);
  });

  // deskctl status. Outside /api/: not part of the screen's contract, and never the token.
  svr.Get("/ops/status", [this](const httplib::Request&, httplib::Response& res) {
    json s = d.status ? d.status() : json::object();
    s["web"] = {{"port", port.load()},
                {"requests", requests.load()},
                {"streams", d.pub->streams().open_count()},
                {"waiting", waiting.load()},
                {"threads", d.threads}};
    reply_json(res, 200, s);
  });

  // ---- POSTs -------------------------------------------------------------------------------
  svr.Post("/api/subscribe", post("POST /api/subscribe", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    Subs subs;
    for (const auto& l : b["ladders"]) {
      MarketRef r;
      if (!parse_ref(l, &r)) return reply_err(res, "BAD_REQUEST", "ladders: [{v, tk}]");
      subs.ladders.push_back(r);
    }
    if (subs.ladders.size() > 4) return reply_err(res, "BAD_REQUEST", "at most 4 ladders");
    if (b["tops"].is_string()) {
      subs.tops_all = true;   // "ALL", the only string the schema lets through
    } else {
      for (const auto& t : b["tops"]) {
        MarketRef r;
        if (!parse_ref(t, &r)) return reply_err(res, "BAD_REQUEST", "tops: \"ALL\" or [{v, tk}]");
        subs.tops.push_back(r);
      }
    }
    auto p = std::make_shared<std::promise<PubReply>>();
    PubReply r = pub_call(PubSubscribe{b["sid"].get<std::string>(), std::move(subs), p}, p);
    reply_json(res, r.status, r.body);
  }));

  // KILL: its own path. The trip sets the flag and wakes send-cx and gw, which both watch the
  // switch (gw/kill.h): no queue slot, so nothing can drop it, and nothing here waits for gw, so
  // KILL answers while gw is frozen. send-cx is expected to take the new generation at once; if
  // it has not within 100 ms (dead, or stuck in a write), the screen hears it as CRIT rather than
  // seeing KILLED alone. A repeated KILL sends cancel-all again: idempotent and never harmful.
  svr.Post("/api/kill", post("", [this](const httplib::Request&, httplib::Response& res, const json&) {
    const bool first = d.kill->trip(now_ms(), mono_ms(), "operator");
    const std::uint64_t gen = d.kill->generation();
    if (first) d.pub->alert("CRIT", "KILL", "KILL by operator: new orders refused until re-armed (60 s)");
    if (!kill_taken(gen, 100)) {
      DLOG_ERR("KILL: send-cx has not taken generation %llu within 100 ms", static_cast<unsigned long long>(gen));
      d.pub->alert("CRIT", "KILL", "send-cx has not taken the KILL within 100 ms: check the venues for resting orders");
    }
    const RunView r{RunState::KILLED, d.kill->since_ms(), d.kill->rearm_ms(), d.kill->why()};
    reply_json(res, 200, json{{"ok", true}, {"run", run_json(r)}, {"n", 0}});
  }));

  // gw applies the re-arm itself, on its own thread, before it looks at any later Start
  svr.Post("/api/rearm", post("POST /api/rearm", [this](const httplib::Request&, httplib::Response& res, const json&) {
    const std::int64_t now = now_ms();
    const std::string why = d.kill->rearm(now, mono_ms());
    if (!why.empty()) return reply_err(res, "BAD_STATE", why);
    d.pub->alert("WARN", "KILL", "re-armed: Systematic needs a Start");
    reply_json(res, 200, json{{"ok", true}, {"run", run_json(RunView{RunState::STOP, now, 0, ""})}, {"n", 0}});
  }));

  svr.Post("/api/start", post("POST /api/start", [this](const httplib::Request&, httplib::Response& res, const json&) {
    auto p = std::make_shared<std::promise<ControlReply>>();
    const bool posted = d.gw->post(Start{p});
    control_reply(res, p, posted, "start");
  }));

  // Stop is a safety action like KILL: never refused for its body
  svr.Post("/api/stop", post("", [this](const httplib::Request&, httplib::Response& res, const json&) {
    auto p = std::make_shared<std::promise<ControlReply>>();
    const bool posted = d.gw->post(Stop{"operator", p});
    control_reply(res, p, posted, "stop");
  }));

  // Week 0 lists no markets and holds no orders, lanes or balances. These are the answers the
  // gateway would give an empty universe, so the page's buttons get contract replies, not 404s;
  // a body that breaks the contract never gets this far (post: BAD_REQUEST).
  // Manual orders go to gw, which refuses them RO until the Manual path is attached.
  svr.Post("/api/order", post("POST /api/order", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    order_route(res, b);
  }));
  svr.Post("/api/cancel", post("POST /api/cancel", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    std::vector<CancelIntent> intents;
    if (b.contains("cids")) {   // unknown or finished cids are skipped by gw
      for (const auto& cid : b["cids"]) {
        CancelIntent c; c.scope = CancelIntent::Scope::CID; c.cid = cid.get<std::string>();
        intents.push_back(std::move(c));
      }
      if (intents.empty()) return reply_json(res, 200, json{{"ok", true}, {"n", 0}, {"cids", json::array()}});
    } else {
      CancelIntent c;
      c.scope = CancelIntent::Scope::MARKET;
      c.venue = b["v"] == "K" ? Venue::K : Venue::P;
      c.ticker = b["tk"].get<std::string>();
      const std::string own = b["own"].get<std::string>();
      c.own = own == "MAN" ? CancelIntent::Own::MAN : own == "SYS" ? CancelIntent::Own::SYS : CancelIntent::Own::ANY;
      if (b.contains("side") && !b["side"].is_null()) { c.has_side = true; c.side = b["side"] == "BUY" ? Side::BUY : Side::SELL; }
      if (b.contains("px") && !b["px"].is_null()) c.px = parse_price(b["px"].get<std::string>()).value_or(0);
      intents.push_back(std::move(c));
    }
    cancel_route(res, std::move(intents));
  }));
  svr.Post("/api/cancel_all", post("POST /api/cancel_all", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    if (b.contains("v") && b["v"] == "P") return reply_json(res, 200, json{{"ok", true}, {"n", 0}, {"cids", json::array()}});
    CancelIntent c;
    c.scope = CancelIntent::Scope::ALL;
    const std::string own = b["own"].get<std::string>();
    c.own = own == "MAN" ? CancelIntent::Own::MAN : own == "SYS" ? CancelIntent::Own::SYS : CancelIntent::Own::ANY;
    cancel_route(res, {c});
  }));
  svr.Post("/api/amend", post("POST /api/amend", [](const httplib::Request&, httplib::Response& res, const json&) {
    reply_err(res, "NOT_FOUND", "no such order");
  }));
  svr.Post(R"(/api/lane/([A-Za-z0-9_-]+))", post("POST /api/lane/{id}", [this](const httplib::Request& req, httplib::Response& res, const json& b) {
    auto p = std::make_shared<std::promise<LaneReply>>();
    const bool posted = d.gw->post(LaneSet{std::string(req.matches[1]), b.at("on").get<bool>(), p});
    if (!posted) return reply_err(res, "INTERNAL", "gw inbox full: lane switch not queued");
    auto f = p->get_future();
    Waiter w(waiting);
    if (!w.ok) return reply_err(res, "INTERNAL", "lane switch queued; the stream's lane row shows the outcome");
    if (f.wait_for(std::chrono::milliseconds(kGwWaitMs)) != std::future_status::ready)
      return reply_err(res, "INTERNAL", "lane switch queued; gw has not answered within 50 ms: the stream's lane row shows the outcome");
    const LaneReply r = f.get();
    if (!r.code.empty()) return reply_err(res, r.code, r.msg);
    reply_json(res, 200, json{{"ok", true}, {"lane", json::parse(r.lane)}});
  }));
  svr.Post("/api/release", post("POST /api/release", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    const std::string tk = b.at("tk").get<std::string>();
    if (b.at("v") != "K") return reply_err(res, "UNKNOWN_MARKET", tk + " is not held by Manual");
    auto p = std::make_shared<std::promise<bool>>();
    if (!d.gw->post(ReleaseMarket{tk, p})) return reply_err(res, "INTERNAL", "gw inbox full: release not queued");
    auto f = p->get_future();
    Waiter w(waiting);
    if (!w.ok) return reply_err(res, "INTERNAL", "release queued; the stream's mkt row shows the outcome");
    if (f.wait_for(std::chrono::milliseconds(kGwWaitMs)) != std::future_status::ready)
      return reply_err(res, "INTERNAL", "release queued; gw has not answered within 50 ms: the stream's mkt row shows the outcome");
    if (!f.get()) return reply_err(res, "UNKNOWN_MARKET", tk + " is not held by Manual");
    reply_json(res, 200, json{{"ok", true}});
  }));
  svr.Post("/api/watch", post("POST /api/watch", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!d.watch) return reply_err(res, "UNKNOWN_MARKET", "no markets are listed in this stage");
    const json answer = d.watch(b);
    reply_json(res, answer.value("ok", false) ? 200 : err_http(answer.at("err").at("code")), answer);
  }));
  svr.Post("/api/transfer", post("POST /api/transfer", [this](const httplib::Request&, httplib::Response& res, const json& b) {
    // the ticket's MOVE: gw checks the source's free cash, send-k1 writes it, the answer waits for the venue's
    auto shard = [](const std::string& id) {
      if (id.size() < 2 || id.size() > 3 || id[0] != 'S' || !std::all_of(id.begin() + 1, id.end(), ::isdigit)) return -1;
      return std::stoi(id.substr(1));
    };
    TransferIntent t;
    t.from = shard(b.at("from").get<std::string>());
    t.to = shard(b.at("to").get<std::string>());
    t.amount = parse_money(b.at("amt").get<std::string>()).value_or(0);
    if (t.from < 0 || t.to < 0) return reply_err(res, "BAD_REQUEST", "from and to are Kalshi shards (S0, S1, ...)");
    t.reply = std::make_shared<std::promise<TransferReply>>();
    auto f = t.reply->get_future();
    if (!d.gw->post(std::move(t))) return reply_err(res, "INTERNAL", "gw inbox full: transfer not queued");
    OrderWaiter w(order_waiting);
    if (!w.ok) return reply_err(res, "INTERNAL", "transfer queued; the header's shard cash shows the outcome");
    if (f.wait_for(std::chrono::milliseconds(kOrderWaitMs)) != std::future_status::ready)
      return reply_err(res, "EXCHANGE", "transfer queued; no answer within 3000 ms: the header's shard cash shows the outcome");
    const TransferReply r = f.get();
    if (!r.code.empty()) return reply_err(res, r.code, r.msg);
    reply_json(res, 200, json{{"ok", true}, {"shards", json::parse(r.shards)}});
  }));
  // Any other POST: the token first, like every POST, then 404 (until 2026-10-06 an unknown
  // path answered 404 before the token was looked at)
  svr.Post(R"(/.*)", post("", [](const httplib::Request&, httplib::Response& res, const json&) {
    reply_err(res, "NOT_FOUND", "no such endpoint");
  }));

  // ---- the page ----------------------------------------------------------------------------
  svr.Get("/", [this](const httplib::Request&, httplib::Response& res) { serve_page(res); });
  svr.Get("/index.html", [this](const httplib::Request&, httplib::Response& res) { serve_page(res); });
  svr.Get(R"(/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
    const std::string p = req.matches[1];
    if (p.compare(0, 4, "api/") == 0 || p.compare(0, 4, "ops/") == 0) return reply_err(res, "NOT_FOUND", "no such endpoint");
    std::string body;
    if (!asset_path_ok(p) || !read_file(d.ui_dir + "/" + p, &body)) return reply_err(res, "NOT_FOUND", "no such file");
    res.set_header("Cache-Control", "no-cache");   // revalidate: an edit on disk shows on reload
    res.set_content(body, content_type_for(p));
  });
}

WebServer::WebServer(WebDeps deps) : impl_(new Impl(std::move(deps))) { impl_->routes(); }

WebServer::~WebServer() { stop(); }

int WebServer::bind(int port, std::string* err) {
  // SO_REUSEADDR alone, never httplib's default SO_REUSEPORT: with it a second deskd on the same
  // port bound without an error and the kernel shared the page's connections between the two
  // (a paper and a live desk on one port: each click to either). Now the second one is refused.
  impl_->svr.set_socket_options([](socket_t s) {
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  });
  int bound = 0;
  if (port == 0) {
    bound = impl_->svr.bind_to_any_port("127.0.0.1");
  } else if (impl_->svr.bind_to_port("127.0.0.1", port)) {
    bound = port;
  }
  if (bound <= 0) {
    if (err) *err = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (in use?)";
    return 0;
  }
  impl_->port.store(bound);
  return bound;
}

void WebServer::serve() { impl_->svr.listen_after_bind(); }

void WebServer::wait_ready() { impl_->svr.wait_until_ready(); }

void WebServer::stop() {
  if (impl_->stopping.exchange(true)) return;
  // Every stream's provider sees `stopping` within 250 ms and ends its response with the closing
  // chunk. svr.stop() at once cut them off mid-response instead (httplib's chunked loop leaves on
  // is_shutting_down() before the provider's sink.done()): the browser logged
  // ERR_INCOMPLETE_CHUNKED_ENCODING on every SIGTERM. Wait up to 300 ms for them.
  if (impl_->d.pub) {
    impl_->d.pub->streams().close_all();
    const std::int64_t end = mono_ms() + 300;
    while (impl_->d.pub->streams().open_count() > 0 && mono_ms() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  impl_->svr.stop();
}

int WebServer::port() const { return impl_->port.load(); }
std::uint64_t WebServer::requests() const { return impl_->requests.load(); }

}  // namespace deskd
