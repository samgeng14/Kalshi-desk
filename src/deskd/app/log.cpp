#include "deskd/app/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace deskd {

namespace {
thread_local std::string t_label = "main";
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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
  // One write(2) per line: stdio's lock is enough to keep lines whole across threads, but a
  // single write also keeps them whole when stderr is a file two processes append to.
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char line[2304];
  const int n = std::snprintf(line, sizeof line, "%s [%s] %s: %s\n", iso_ms(now_ms()).c_str(), level,
                              t_label.c_str(), msg);
  if (n > 0) {
    const std::size_t len = static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1;
    ssize_t w = ::write(STDERR_FILENO, line, len);
    (void)w;
  }
}

}  // namespace deskd
