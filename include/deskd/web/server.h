#pragma once
// web (plan section 2 and 6): cpp-httplib on 127.0.0.1:8788, serving the screen from --ui-dir
// on disk (a UI edit needs a reload, not a rebuild) and the contract's endpoints.
//
// Guards (contract: REQUEST GUARDS):
// - Host: every request whose Host is not exactly 127.0.0.1:<port> or localhost:<port> is
//   refused 403 FORBIDDEN, GET included. A DNS-rebinding page could otherwise read the page,
//   and with it the token, from a name that resolves to 127.0.0.1.
// - Exactly one Host header (two, the first good, were let in until 2026-10-06).
// - Origin: a request with Sec-Fetch-Site cross-site or same-site, or with an Origin other than
//   http://127.0.0.1:<port> or http://localhost:<port>, is refused 403 FORBIDDEN, GET included.
//   An ordinary page on another site sends Host 127.0.0.1:8788, so the Host rule alone let it
//   hold every stream slot (review, 2026-10-06). deskctl and curl send neither header.
//   One exception, peer_navigation below: the PAPER/LIVE switch's click from the other desk.
// - These run before routing and before httplib reads a body, on every method; a refused POST's
//   connection is closed after the reply, so its unread body is never parsed as a request.
// - Token: every POST carries X-Desk with the per-start token written into the served page
//   (<meta name="desk-token">), else 403 FORBIDDEN. KILL included, and a POST to an unknown path.
// - Then the contract's request schema (web/contract.h check_request): BAD_REQUEST before any
//   handler. KILL and Stop take any body.
// - No CORS headers, ever.
// - A request thread waits on an owner (gw, pub) for at most 4 at a time: 50 ms for gw (Start,
//   Stop: then "queued", the stream shows the outcome), 2 s for pub's snapshot and subscribe,
//   whose answer is the data itself.
// Threads: 16 with a 2 s keep-alive and at most 4 SSE streams. The old desk's 9 threads all sat
// in idle keep-alive connections once and a click waited 4.8 s.
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class Gateway;
class KillSwitch;
class Pub;
struct SenderStats;

bool host_ok(const std::string& host, int port);                  // contract hostOk
bool token_ok(const std::string& got, const std::string& want);   // constant time, never empty
// why a request is cross-site, or "": Sec-Fetch-Site cross-site or same-site, or an Origin
// (origin non-null: the header was sent) other than this desk's own page
std::string cross_site_refusal(const std::string& sec_fetch_site, const std::string* origin, int port);
// The header's PAPER/LIVE switch: a click on the other desk's page (another port on 127.0.0.1, so
// Sec-Fetch-Site same-site) opening this one. Let past cross_site_refusal: a user-activated
// top-level GET of the page, as a document, with no Origin. A navigation reads nothing back to the
// page that started it, and holds no stream; a frame, fetch, script or any other path stays refused.
bool peer_navigation(const std::string& method, const std::string& path, const std::string& site,
                     const std::string& mode, const std::string& dest, const std::string& user, bool has_origin);
// html with the token written into <meta name="desk-token" content="">; false when the page
// has no such empty meta (then the page would boot without a token and every POST would fail)
bool inject_token(const std::string& html, const std::string& token, std::string* out);
// html with value (attribute-escaped) written into <meta name="NAME" content="">; false without it
bool inject_meta(const std::string& html, const std::string& name, const std::string& value, std::string* out);
std::string random_token();   // 32 hex characters from the system's CSPRNG

struct WebDeps {
  std::string ui_dir;
  int threads = 16;
  int keepalive_s = 2;
  std::string token;
  // the header's PAPER/LIVE switch (<meta name="desk-peer">, "desk-venue"): the other desk's page
  // and this desk's venue, "demo", "prod" or "sim" (the fake exchange). "" leaves the meta empty.
  std::string peer_url;
  std::string venue;
  Pub* pub = nullptr;
  Gateway* gw = nullptr;
  KillSwitch* kill = nullptr;
  const SenderStats* cx_stats = nullptr;   // KILL's route checks send-cx took the switch's generation
  std::function<nlohmann::json()> status;   // GET /ops/status (deskctl)
  std::function<nlohmann::json(const nlohmann::json&)> universe;
  // {ok,mkt} or the contract's error body. No network I/O on a web request.
  std::function<nlohmann::json(const nlohmann::json&)> watch;
};

class WebServer {
 public:
  explicit WebServer(WebDeps deps);
  ~WebServer();

  // binds 127.0.0.1:port (0 = any free port); the bound port, or 0 with *err set
  int bind(int port, std::string* err);
  void serve();        // blocks until stop(); the web worker runs it
  void wait_ready();   // returns once serve() is accepting (a stop() before that would be lost)
  void stop();    // ends every stream, stops listening, waits for the handlers

  int port() const;
  std::uint64_t requests() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deskd
