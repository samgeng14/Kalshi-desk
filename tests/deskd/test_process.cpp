// deskd as processes: the built binaries, started the way the supervisor will start them, each
// with HOME pointed into a temporary directory, so the default lock dir is never the user's.
//   - a second deskd on the same account is refused (exit 3) while the first holds the lock: from
//     the same data dir, and from another working directory with a relative --data-dir (the
//     review's case: until 2026-10-06 the lock sat in the data dir, and both came up)
//   - a missing data dir is refused and not created; "live" in a config file is refused
//   - the heartbeat file appears; deskctl status reads the workers; deskctl kill KILLs through
//     the screen's route with its token
//   - a stream client that vanishes mid-stream does not end deskd (SIGPIPE ignored)
//   - SIGTERM shuts down cleanly (exit 0) and releases the lock
#include <arpa/inet.h>
#include <fcntl.h>
#include <httplib.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/app/lock.h"
#include "deskd/build_paths.h"

extern char** environ;

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// the children's environment: ours with HOME replaced
std::vector<std::string> g_env;
std::vector<char*> g_envp;
void set_child_home(const std::string& home) {
  g_env.clear();
  for (char** e = environ; *e; ++e)
    if (std::string(*e).compare(0, 5, "HOME=") != 0) g_env.push_back(*e);
  g_env.push_back("HOME=" + home);
  g_envp.clear();
  for (auto& e : g_env) g_envp.push_back(const_cast<char*>(e.c_str()));
  g_envp.push_back(nullptr);
}

// runs argv (in cwd, when given) with stdout and stderr in files; returns the pid
pid_t spawn(const std::vector<std::string>& args, const std::string& out, const std::string& err,
            const std::string& cwd = "") {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&fa, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), g_envp.empty() ? environ : g_envp.data()) != 0) pid = -1;
  posix_spawn_file_actions_destroy(&fa);
  return pid;
}

// the exit code, or -1 if it is still running after `limit` (then it is killed)
int wait_exit(pid_t pid, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  int st = 0;
  while (std::chrono::steady_clock::now() < end) {
    const pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    std::this_thread::sleep_for(20ms);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &st, 0);
  return -1;
}

int run(const std::vector<std::string>& args, const TempDir& d, const std::string& tag, std::string* out = nullptr,
        std::string* err = nullptr, const std::string& cwd = "") {
  const pid_t pid = spawn(args, d.file(tag + ".out"), d.file(tag + ".err"), cwd);
  if (pid < 0) return -2;
  const int code = wait_exit(pid, 10s);
  if (out) *out = slurp(d.file(tag + ".out"));
  if (err) *err = slurp(d.file(tag + ".err"));
  return code;
}

int free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  socklen_t len = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  const int port = ntohs(a.sin_port);
  ::close(fd);
  return port;
}

bool healthy(int port) {
  httplib::Client c("127.0.0.1", port);
  c.set_connection_timeout(0, 300000);
  c.set_read_timeout(1, 0);
  auto r = c.Get("/api/health");
  return r && r->status == 200;
}

bool exists(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0;
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);   // this test writes to sockets deskd may close
  TempDir d;
  const int port = free_port();
  const std::string P = std::to_string(port);
  const std::string ui = std::string(DESKD_SOURCE_DIR) + "/ui/desk";
  // HOME for every child: the account lock's default dir is $HOME/Library/Application
  // Support/kalshi-desk/locks, made if missing
  const std::string home = d.file("home");
  ::mkdir(home.c_str(), 0755);
  set_child_home(home);
  const std::string lock_dir = home + "/" + deskd::AccountLock::kDirUnderHome;
  const std::string lock_file = lock_dir + "/kalshi-demo.lock";
  // two working directories, each with its own relative "data"
  for (const char* sub : {"a", "a/data", "b", "b/data"}) ::mkdir(d.file(sub).c_str(), 0755);

  // the first deskd
  const pid_t first = spawn({DESKD_BIN, "--data-dir", d.path, "--ui-dir", ui, "--port", P}, d.file("d1.out"), d.file("d1.err"));
  CHECK(first > 0);
  bool up = false;
  for (int i = 0; i < 100 && !up; ++i) {
    std::this_thread::sleep_for(50ms);
    up = healthy(port);
  }
  CHECK(up);
  CHECK(slurp(lock_file).find("pid " + std::to_string(first) + " deskd DEMO since ") == 0);
  CHECK(!exists(d.file("kalshi-account.lock")));   // nothing in the data dir decides the lock any more

  // a second deskd on the same data dir: refused before it binds anything
  std::string out, err;
  CHECK(run({DESKD_BIN, "--data-dir", d.path, "--ui-dir", ui, "--port", std::to_string(free_port())}, d, "second", &out,
            &err) == 3);
  CHECK(err.find("held by another process") != std::string::npos);
  CHECK(err.find("pid " + std::to_string(first)) != std::string::npos);
  // and from another working directory with its own relative data dir: the same account, the
  // same file, refused (the review started two this way and both came up)
  CHECK(run({DESKD_BIN, "--data-dir", "data", "--ui-dir", ui, "--port", std::to_string(free_port())}, d, "otherdir", &out,
            &err, d.file("b")) == 3);
  CHECK(err.find("held by another process") != std::string::npos);
  CHECK(err.find("kalshi-demo.lock") != std::string::npos);
  // the Python side takes the same file: the old desk's supervisor starts its binary under
  // `account_lock.py hold`, and kalshi_client.py takes it before a write
  if (std::string(DESKD_PYTHON).empty()) {
    std::printf("python3 not found: account_lock.py checks skipped\n");
  } else {
    const std::string py = DESKD_PYTHON, script = std::string(DESKD_SOURCE_DIR) + "/scripts/account_lock.py";
    CHECK(run({py, script, "hold", "demo", "old desk", "--", "/usr/bin/touch", d.file("old-desk-ran")}, d, "pyhold", &out, &err) == 3);
    CHECK(!exists(d.file("old-desk-ran")));
    CHECK(err.find("deskd DEMO") != std::string::npos);
    CHECK(run({py, script, "status", "demo"}, d, "pystatus", &out) == 3);
    CHECK(out.find("held: pid " + std::to_string(first) + " deskd DEMO") != std::string::npos);
    // the prod account is another file, free while a non-live deskd runs (and held by the
    // child it execs for that child's life)
    CHECK(run({py, script, "hold", "prod", "py", "--", py, script, "status", "prod"}, d, "pyprod", &out) == 3);
    CHECK(out.find(" py since ") != std::string::npos);
    CHECK(run({py, script, "status", "prod"}, d, "pyprod2", &out) == 0);
  }

  // a missing data dir: refused, and not created
  CHECK(run({DESKD_BIN, "--data-dir", d.file("absent"), "--port", "0"}, d, "absent", &out, &err) == 3);
  CHECK(!exists(d.file("absent")));
  // live is a flag, never a file setting
  std::ofstream(d.file("live.json")) << R"({"deskd": {"live": true}})";
  CHECK(run({DESKD_BIN, "--config", d.file("live.json"), "--data-dir", d.path}, d, "livecfg", &out, &err) == 2);
  CHECK(err.find("command-line flag only") != std::string::npos);
  CHECK(run({DESKD_BIN, "--bogus"}, d, "bogus") == 2);

  // the heartbeat file, written while gw, md-k and priv-k move
  bool beat = false;
  for (int i = 0; i < 60 && !beat; ++i) {
    std::this_thread::sleep_for(50ms);
    beat = exists(d.file("deskd.heartbeat"));
  }
  CHECK(beat);
  const long long hb = std::atoll(slurp(d.file("deskd.heartbeat")).c_str());
  CHECK(hb > 1700000000 && std::llabs(hb - static_cast<long long>(std::time(nullptr))) <= 3);

  // deskctl status
  CHECK(run({DESKCTL_BIN, "--port", P, "status"}, d, "status", &out) == 0);
  CHECK(out.find("DEMO") != std::string::npos);
  CHECK(out.find("gw") != std::string::npos && out.find("md-k") != std::string::npos && out.find("send-cx") != std::string::npos);
  CHECK(out.find("heartbeat written") != std::string::npos);
  CHECK(out.find("run STOP") != std::string::npos);

  // a stream client that vanishes mid-stream: deskd writes into a dead socket and lives on
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    const std::string req = "GET /api/stream?sid=vanish01 HTTP/1.1\r\nHost: 127.0.0.1:" + P + "\r\n\r\n";
    CHECK(::write(fd, req.data(), req.size()) == static_cast<ssize_t>(req.size()));
    char b[512];
    CHECK(::read(fd, b, sizeof b) > 0);
    linger lg{1, 0};   // RST on close: the next write deskd makes fails at once
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    ::close(fd);
    std::this_thread::sleep_for(2500ms);   // two hbs written into the dead stream
    CHECK(::waitpid(first, nullptr, WNOHANG) == 0);
    CHECK(healthy(port));
  }

  // deskctl kill: through the screen's route, Host and token included
  CHECK(run({DESKCTL_BIN, "--port", P, "kill"}, d, "kill", &out) == 0);
  CHECK(out.find("KILLED") != std::string::npos);
  {
    httplib::Client c("127.0.0.1", port);
    auto r = c.Get("/ops/status");
    CHECK(r && r->status == 200);
    if (r) {
      json s = json::parse(r->body);
      CHECK(s["run"]["state"] == "KILLED");
      CHECK(s["send_cx"]["kills"].get<int>() >= 1);
      // status never carries the token
      auto page = c.Get("/");
      const std::string tag = "name=\"desk-token\" content=\"";
      const std::size_t at = page ? page->body.find(tag) : std::string::npos;
      CHECK(at != std::string::npos);
      if (at != std::string::npos) {
        const std::string token = page->body.substr(at + tag.size(), 32);
        CHECK(token.size() == 32 && r->body.find(token) == std::string::npos);
      }
    }
  }
  CHECK(run({DESKCTL_BIN, "--port", P, "status"}, d, "status2", &out) == 0);
  CHECK(out.find("run KILLED") != std::string::npos);
  CHECK(run({DESKCTL_BIN, "--port", P, "frobnicate"}, d, "badcmd") == 2);

  // SIGTERM: a clean exit, the lock released
  const auto t0 = std::chrono::steady_clock::now();
  ::kill(first, SIGTERM);
  CHECK(wait_exit(first, 5s) == 0);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("SIGTERM to exit: %.2f s\n", secs);
  CHECK(secs < 3.0);
  std::string lerr;
  auto lock = deskd::AccountLock::acquire(lock_dir, "demo", "test", false, &lerr);
  CHECK(lock && lock->held());
  const std::string log = slurp(d.file("d1.err"));
  CHECK(log.find("signal: shutting down") != std::string::npos);
  CHECK(log.find("stopped") != std::string::npos);
  CHECK(log.find("/kalshi-demo.lock (demo account), data ") != std::string::npos);   // the real paths, logged

  // KILLED survives a restart until someone re-arms: the account's, beside its lock, so a restart
  // from another working directory with another data dir is KILLED too
  {
    CHECK(lock);
    lock.reset();   // the next deskd needs it
    CHECK(exists(lock_dir + "/kalshi-demo.killed"));
    const pid_t again = spawn({DESKD_BIN, "--data-dir", "data", "--ui-dir", ui, "--port", P}, d.file("d2.out"),
                              d.file("d2.err"), d.file("a"));
    bool up2 = false;
    for (int i = 0; i < 100 && !up2; ++i) {
      std::this_thread::sleep_for(50ms);
      up2 = healthy(port);
    }
    CHECK(up2);
    CHECK(run({DESKCTL_BIN, "--port", P, "status"}, d, "status3", &out) == 0);
    CHECK(out.find("run KILLED") != std::string::npos);
    ::kill(again, SIGTERM);
    CHECK(wait_exit(again, 5s) == 0);
  }

  // deskctl with nobody listening
  CHECK(run({DESKCTL_BIN, "--port", P, "status"}, d, "nobody") == 1);
  CHECK(run({DESKCTL_BIN, "--port", P, "kill"}, d, "nobody-kill", &out, &err) == 1);
  CHECK(err.find("KILL NOT SENT") != std::string::npos);
  return finish("deskd-process");
}
