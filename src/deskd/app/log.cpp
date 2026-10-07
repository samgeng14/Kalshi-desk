#include "deskd/app/log.h"

#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

namespace deskd {

namespace {
thread_local std::string t_label = "main";

constexpr std::size_t kQueueCap = 4096;

// Never destroyed: a worker may still log while the process exits.
struct Logger {
  std::mutex m;
  std::condition_variable work, idle;
  std::deque<std::string> q;
  bool started = false, writing = false;
  std::uint64_t dropped = 0, reported = 0;
};
Logger& logger() {
  static Logger* l = new Logger;
  return *l;
}

void write_all(const char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(STDERR_FILENO, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return;   // stderr closed or broken: nothing to tell
    p += w;
    n -= static_cast<std::size_t>(w);
  }
}

std::string format_line(const char* level, const char* msg) {
  char line[2304];
  const int n = std::snprintf(line, sizeof line, "%s [%s] %s: %s\n", iso_ms(now_ms()).c_str(), level,
                              t_label.c_str(), msg);
  if (n <= 0) return std::string();
  return std::string(line, static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1);
}

void logger_loop() {
#ifdef __APPLE__
  pthread_setname_np("log");
#endif
  t_label = "log";
  Logger& l = logger();
  std::unique_lock<std::mutex> lk(l.m);
  for (;;) {
    l.work.wait(lk, [&l] { return !l.q.empty() || l.dropped != l.reported; });
    std::string batch;
    if (l.dropped != l.reported) {
      char msg[96];
      std::snprintf(msg, sizeof msg, "%llu log lines dropped: stderr was not being read",
                    static_cast<unsigned long long>(l.dropped - l.reported));
      l.reported = l.dropped;
      batch = format_line("warn", msg);
    }
    // whole lines, many per write(2): one write keeps them whole in a file other processes append to
    while (!l.q.empty() && batch.size() < 65536) {
      batch += l.q.front();
      l.q.pop_front();
    }
    l.writing = true;
    lk.unlock();
    write_all(batch.data(), batch.size());
    lk.lock();
    l.writing = false;
    l.idle.notify_all();
  }
}
}  // namespace

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t mono_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string iso_ms(std::int64_t ms) {
  std::time_t s = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_r(&s, &tm);
  char buf[40];
  const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + n, sizeof buf - n, ".%03dZ", static_cast<int>(ms % 1000));
  return buf;
}

void set_thread_label(const std::string& name) { t_label = name; }

void log_line(const char* level, const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::string line = format_line(level, msg);
  if (line.empty()) return;
  Logger& l = logger();
  bool queued = false;
  {
    std::lock_guard<std::mutex> lk(l.m);
    if (l.started) {
      if (l.q.size() >= kQueueCap) ++l.dropped;
      else l.q.push_back(std::move(line));
      queued = true;
    }
  }
  if (queued) l.work.notify_one();
  else write_all(line.data(), line.size());   // no logger thread: written at once
}

void start_logger() {
  Logger& l = logger();
  std::lock_guard<std::mutex> lk(l.m);
  if (l.started) return;
  l.started = true;
  std::thread(logger_loop).detach();
}

bool flush_logger(int wait_ms) {
  Logger& l = logger();
  std::unique_lock<std::mutex> lk(l.m);
  if (!l.started) return true;
  return l.idle.wait_for(lk, std::chrono::milliseconds(wait_ms),
                         [&l] { return l.q.empty() && !l.writing && l.dropped == l.reported; });
}

std::uint64_t log_dropped() {
  Logger& l = logger();
  std::lock_guard<std::mutex> lk(l.m);
  return l.dropped;
}

}  // namespace deskd
