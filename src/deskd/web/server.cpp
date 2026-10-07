#include "deskd/web/server.h"

#include <httplib.h>

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

bool token_ok(const std::string& got, const std::string& want) {
  if (want.empty() || got.size() != want.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < want.size(); ++i) diff |= static_cast<unsigned char>(got[i] ^ want[i]);
  return diff == 0;
}

bool inject_token(const std::string& html, const std::string& token, std::string* out) {
  static const std::string kEmpty = "<meta name=\"desk-token\" content=\"\">";
  const std::size_t at = html.find(kEmpty);
  if (at == std::string::npos) return false;
  *out = html.substr(0, at) + "<meta name=\"desk-token\" content=\"" + token + "\">" + html.substr(at + kEmpty.size());
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
bool only_keys(const json& j, std::initializer_list<const char*> keys) {
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool known = false;
    for (const char* k : keys) known = known || it.key() == k;
    if (!known) return false;
  }
  return true;
}
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

  using Handler = std::function<void(const httplib::Request&, httplib::Response&, const json&)>;

  // Every POST: Host, then the token, then a JSON object body. These run in the handler, after
  // httplib has read the body: refused in the pre-routing hook, an unread body would be left on
  // a keep-alive connection and read as the next request. KILL takes any body: a KILL is never
  // refused for its shape, only for Host and token.
  httplib::Server::Handler post(Handler h, bool any_body = false) {
    return [this, h, any_body](const httplib::Request& req, httplib::Response& res) {
      if (!host_ok(req.get_header_value("Host"), port.load()))
        return reply_err(res, "FORBIDDEN", "Host must be 127.0.0.1:" + std::to_string(port.load()) + " or localhost:" +
                                               std::to_string(port.load()));
      if (!token_ok(req.get_header_value("X-Desk"), d.token))
        return reply_err(res, "FORBIDDEN", "X-Desk token missing or not this start's");
      json body = json::object();
      if (!any_body) {
        const std::string ct = req.get_header_value("Content-Type");
        if (ct.compare(0, 16, "application/json") != 0)
          return reply_err(res, "BAD_REQUEST", "Content-Type must be application/json");
        try {
          body = json::parse(req.body);
        } catch (const json::exception&) {
          return reply_err(res, "BAD_REQUEST", "body is not JSON");
        }
        if (!body.is_object()) return reply_err(res, "BAD_REQUEST", "body is not a JSON object");
      }
      h(req, res, body);
    };
  }

  void control_reply(httplib::Response& res, const ControlPromise& p, bool posted, const char* what) {
    if (!posted) return reply_err(res, "INTERNAL", std::string("gw inbox full: ") + what + " not queued");
    auto f = p->get_future();
    if (f.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
      return reply_err(res, "INTERNAL", std::string("gw did not answer ") + what + " in 1 s");
    ControlReply r = f.get();
    if (r.code.empty()) return reply_json(res, 200, json{{"ok", true}, {"run", run_json(r.run)}, {"n", r.n}});
    json body = err_body(r.code, r.msg);
    if (r.code == "PREFLIGHT") body["preflight"] = preflight_json(r.preflight);
    reply_json(res, err_http(r.code), body);
  }

  PubReply pub_call(PubMsg m, PubPromise p) {
    auto f = p->get_future();
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
    if (!read_file(d.ui_dir + "/index.html", &html)) return reply_err(res, "INTERNAL", "cannot read the ui dir's index.html");
    if (!inject_token(html, d.token, &page))
      return reply_err(res, "INTERNAL", "index.html has no empty desk-token meta to write the token into");
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

  // GET (and anything else without a body): Host first, before routing
  svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    requests.fetch_add(1, std::memory_order_relaxed);
    if (req.method == "POST") return httplib::Server::HandlerResponse::Unhandled;   // see post()
    if (!host_ok(req.get_header_value("Host"), port.load())) {
      reply_err(res, "FORBIDDEN", "Host must be 127.0.0.1:" + std::to_string(port.load()) + " or localhost:" +
                                      std::to_string(port.load()));
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;   // ours already
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

  // deskctl status. Outside /api/: not part of the screen's contract, and never the token.
  svr.Get("/ops/status", [this](const httplib::Request&, httplib::Response& res) {
    json s = d.status ? d.status() : json::object();
    s["web"] = {{"port", port.load()},
                {"requests", requests.load()},
                {"streams", d.pub->streams().open_count()},
                {"threads", d.threads}};
    reply_json(res, 200, s);
  });

  // ---- POSTs -------------------------------------------------------------------------------
  svr.Post("/api/subscribe", post([this](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!only_keys(b, {"sid", "ladders", "tops"}) || !is_str(b, "sid") || !b.contains("ladders") ||
        !b["ladders"].is_array() || !b.contains("tops"))
      return reply_err(res, "BAD_REQUEST", "subscribe takes {sid, ladders, tops}");
    Subs subs;
    for (const auto& l : b["ladders"]) {
      MarketRef r;
      if (!parse_ref(l, &r)) return reply_err(res, "BAD_REQUEST", "ladders: [{v, tk}]");
      subs.ladders.push_back(r);
    }
    if (subs.ladders.size() > 4) return reply_err(res, "BAD_REQUEST", "at most 4 ladders");
    if (b["tops"].is_string() && b["tops"] == "ALL") {
      subs.tops_all = true;
    } else if (b["tops"].is_array()) {
      for (const auto& t : b["tops"]) {
        MarketRef r;
        if (!parse_ref(t, &r)) return reply_err(res, "BAD_REQUEST", "tops: \"ALL\" or [{v, tk}]");
        subs.tops.push_back(r);
      }
    } else {
      return reply_err(res, "BAD_REQUEST", "tops: \"ALL\" or [{v, tk}]");
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
  svr.Post("/api/kill", post([this](const httplib::Request&, httplib::Response& res, const json&) {
    const bool first = d.kill->trip(now_ms(), mono_ms(), "operator");
    const std::uint64_t gen = d.kill->generation();
    if (first) d.pub->alert("CRIT", "KILL", "KILL by operator: new orders refused until re-armed (60 s)");
    if (!kill_taken(gen, 100)) {
      DLOG_ERR("KILL: send-cx has not taken generation %llu within 100 ms", static_cast<unsigned long long>(gen));
      d.pub->alert("CRIT", "KILL", "send-cx has not taken the KILL within 100 ms: check the venues for resting orders");
    }
    const RunView r{RunState::KILLED, d.kill->since_ms(), d.kill->rearm_ms(), d.kill->why()};
    reply_json(res, 200, json{{"ok", true}, {"run", run_json(r)}, {"n", 0}});
  }, true));

  // gw applies the re-arm itself, on its own thread, before it looks at any later Start
  svr.Post("/api/rearm", post([this](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!b.empty()) return reply_err(res, "BAD_REQUEST", "rearm takes {}");
    const std::int64_t now = now_ms();
    const std::string why = d.kill->rearm(now, mono_ms());
    if (!why.empty()) return reply_err(res, "BAD_STATE", why);
    d.pub->alert("WARN", "KILL", "re-armed: Systematic needs a Start");
    reply_json(res, 200, json{{"ok", true}, {"run", run_json(RunView{RunState::STOP, now, 0, ""})}, {"n", 0}});
  }));

  svr.Post("/api/start", post([this](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!b.empty()) return reply_err(res, "BAD_REQUEST", "start takes {}");
    auto p = std::make_shared<std::promise<ControlReply>>();
    const bool posted = d.gw->post(Start{p});
    control_reply(res, p, posted, "start");
  }));

  // Stop is a safety action like KILL: never refused for its body
  svr.Post("/api/stop", post([this](const httplib::Request&, httplib::Response& res, const json&) {
    auto p = std::make_shared<std::promise<ControlReply>>();
    const bool posted = d.gw->post(Stop{"operator", p});
    control_reply(res, p, posted, "stop");
  }, true));

  // Week 0 lists no markets and holds no orders, lanes or balances. These are the answers the
  // gateway would give an empty universe, so the page's buttons get contract replies, not 404s.
  svr.Post("/api/order", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!is_str(b, "cid") || !is_str(b, "v") || !is_str(b, "tk")) return reply_err(res, "BAD_REQUEST", "order needs cid, v, tk");
    reply_err(res, "UNKNOWN_MARKET", b["v"].get<std::string>().substr(0, 1) + "|" + b["tk"].get<std::string>().substr(0, 96) +
                                         ": no markets are listed in this stage");
  }));
  svr.Post("/api/cancel", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (b.contains("cids") && b.size() == 1 && b["cids"].is_array())
      return reply_json(res, 200, json{{"ok", true}, {"n", 0}, {"cids", json::array()}});   // unknown cids are skipped
    if (is_str(b, "v") && is_str(b, "tk")) return reply_err(res, "NOT_FOUND", "no such market");
    reply_err(res, "BAD_REQUEST", "cancel takes {cids} or {v, tk, own, side, px}");
  }));
  svr.Post("/api/cancel_all", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!is_str(b, "own")) return reply_err(res, "BAD_REQUEST", "cancel_all takes {own, v}");
    reply_json(res, 200, json{{"ok", true}, {"n", 0}, {"cids", json::array()}});
  }));
  svr.Post("/api/amend", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!is_str(b, "cid")) return reply_err(res, "BAD_REQUEST", "amend needs cid");
    reply_err(res, "NOT_FOUND", "no such order");
  }));
  svr.Post(R"(/api/lane/([A-Za-z0-9_-]+))", post([](const httplib::Request& req, httplib::Response& res, const json& b) {
    if (!b.contains("on") || !b["on"].is_boolean()) return reply_err(res, "BAD_REQUEST", "lane takes {on}");
    reply_err(res, "NOT_FOUND", "no lane " + std::string(req.matches[1]));
  }));
  svr.Post("/api/release", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!is_str(b, "v") || !is_str(b, "tk")) return reply_err(res, "BAD_REQUEST", "release takes {v, tk}");
    reply_err(res, "UNKNOWN_MARKET", "no markets are listed in this stage");
  }));
  svr.Post("/api/transfer", post([](const httplib::Request&, httplib::Response& res, const json& b) {
    if (!is_str(b, "from") || !is_str(b, "to") || !is_str(b, "amt"))
      return reply_err(res, "BAD_REQUEST", "transfer takes {from, to, amt}");
    reply_err(res, "NO_CASH", "no shard balances in this stage");
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
  impl_->d.pub->streams().close_all();   // every stream's provider ends within 250 ms
  impl_->svr.stop();
}

int WebServer::port() const { return impl_->port.load(); }
std::uint64_t WebServer::requests() const { return impl_->requests.load(); }

}  // namespace deskd
