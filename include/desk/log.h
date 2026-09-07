#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace desk {

inline std::string now_iso() {
  using namespace std::chrono;
  auto t = system_clock::now();
  std::time_t tt = system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

inline long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
inline long long now_s() { return now_ms() / 1000; }

inline std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

template <typename... Args>
void logf(const char* level, const char* fmt, Args... args) {
  std::lock_guard<std::mutex> lk(log_mutex());
  std::fprintf(stderr, "%s [%s] ", now_iso().c_str(), level);
  if constexpr (sizeof...(Args) == 0) std::fprintf(stderr, "%s", fmt);
  else std::fprintf(stderr, fmt, args...);
  std::fprintf(stderr, "\n");
}
#define LOG_INFO(...) ::desk::logf("info", __VA_ARGS__)
#define LOG_WARN(...) ::desk::logf("warn", __VA_ARGS__)
#define LOG_ERR(...) ::desk::logf("error", __VA_ARGS__)

}  // namespace desk
