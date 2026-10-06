#pragma once
// web (plan section 2 and 6): cpp-httplib on 127.0.0.1:8788, serving the screen from --ui-dir
// on disk (a UI edit needs a reload, not a rebuild) and the contract's endpoints.
//
// Guards (contract: REQUEST GUARDS):
// - Host: every request whose Host is not exactly 127.0.0.1:<port> or localhost:<port> is
//   refused 403 FORBIDDEN, GET included. A DNS-rebinding page could otherwise read the page,
//   and with it the token, from a name that resolves to 127.0.0.1.
// - Token: every POST carries X-Desk with the per-start token written into the served page
//   (<meta name="desk-token">), else 403 FORBIDDEN. KILL included.
// - No CORS headers, ever.
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
struct SendJob;

bool host_ok(const std::string& host, int port);                  // contract hostOk
bool token_ok(const std::string& got, const std::string& want);   // constant time, never empty
// html with the token written into <meta name="desk-token" content="">; false when the page
// has no such empty meta (then the page would boot without a token and every POST would fail)
bool inject_token(const std::string& html, const std::string& token, std::string* out);
std::string random_token();   // 32 hex characters from the system's CSPRNG

struct WebDeps {
  std::string ui_dir;
  int threads = 16;
  int keepalive_s = 2;
  std::string token;
  Pub* pub = nullptr;
  Gateway* gw = nullptr;
  KillSwitch* kill = nullptr;
  BoundedQueue<SendJob>* send_cx = nullptr;
  std::function<nlohmann::json()> status;   // GET /ops/status (deskctl)
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
