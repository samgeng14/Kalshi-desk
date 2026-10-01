#pragma once
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace desk {

// Parses "YYYY-MM-DDTHH:MM:SS(.fff)?(Z|±HH:MM)" into epoch seconds (UTC). Returns 0 on failure.
// If offset_out is given it receives the UTC offset in seconds encoded in the string.
inline long long parse_iso(const std::string& s, int* offset_out = nullptr) {
  int Y, M, D, h, m;
  double sec = 0;
  if (s.size() < 16) return 0;  // "2026-09-05T16:30Z" (ESPN) has no seconds
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &sec) < 5) return 0;
  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = static_cast<int>(sec);
  long long t = static_cast<long long>(timegm(&tm));
  // Offset suffix.
  int off = 0;
  size_t p = s.find_last_of("+-");
  if (p != std::string::npos && p >= 16) {
    int oh = 0, om = 0;
    if (std::sscanf(s.c_str() + p + 1, "%d:%d", &oh, &om) >= 1) {
      off = oh * 3600 + om * 60;
      if (s[p] == '-') off = -off;
    }
  }
  if (offset_out) *offset_out = off;
  return t - off;
}

// "26SEP06" (Kalshi event date) -> "2026-09-06". Returns "" if it does not match.
inline std::string kalshi_date(const std::string& code) {
  static const char* MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  if (code.size() < 7) return "";
  int yy = std::atoi(code.substr(0, 2).c_str());
  std::string mon = code.substr(2, 3);
  int dd = std::atoi(code.substr(5, 2).c_str());
  int mi = -1;
  for (int i = 0; i < 12; ++i)
    if (mon == MON[i]) mi = i;
  if (mi < 0 || dd < 1 || dd > 31) return "";
  char buf[16];
  std::snprintf(buf, sizeof buf, "20%02d-%02d-%02d", yy, mi + 1, dd);
  return buf;
}

// The date part of an event ticker like KXHIGHNY-26SEP06 -> "2026-09-06".
// The scheduled start, in minutes after Eastern midnight, when the event ticker carries one:
// KXMLBGAME-26SEP101310HOUPHI -> 13:10 -> 790. -1 when it does not (NFL, NHL, college tickers).
// Two games between the same teams on one day (an MLB doubleheader) differ only here.
inline int event_start_minutes(const std::string& event_ticker) {
  auto p = event_ticker.find('-');
  if (p == std::string::npos || event_ticker.size() < p + 1 + 11) return -1;
  const std::string hhmm = event_ticker.substr(p + 1 + 7, 4);
  for (char c : hhmm) if (c < '0' || c > '9') return -1;
  const int hh = std::stoi(hhmm.substr(0, 2)), mm = std::stoi(hhmm.substr(2, 2));
  return hh < 24 && mm < 60 ? hh * 60 + mm : -1;
}

inline std::string event_date(const std::string& event_ticker) {
  auto p = event_ticker.find('-');
  if (p == std::string::npos) return "";
  return kalshi_date(event_ticker.substr(p + 1, 7));
}

constexpr long long kUnknownMinutes = -1000000;
// Minutes until a market stops trading: the earlier of close_time and expected_expiration_time.
// Negative = already closed; kUnknownMinutes = no timestamps.
inline long long minutes_to_close(const std::string& close_time, const std::string& expected_expiration, long long now) {
  long long a = parse_iso(close_time), b = parse_iso(expected_expiration);
  long long t = 0;
  if (a > 0 && b > 0) t = a < b ? a : b;
  else t = a > 0 ? a : b;
  return t > 0 ? (t - now) / 60 : kUnknownMinutes;
}

// UTC offset (seconds) of US Eastern time on a given UTC epoch: EDT from the second Sunday of
// March to the first Sunday of November (2007 rule), EST otherwise.
inline int us_eastern_offset(long long epoch_utc) {
  std::time_t t = static_cast<std::time_t>(epoch_utc);
  std::tm tm{};
  gmtime_r(&t, &tm);
  int Y = tm.tm_year + 1900;
  auto dow = [](int y, int m, int d) {  // 0 = Sunday
    static const int tt[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7;
  };
  int mar2 = 8 + (7 - dow(Y, 3, 8)) % 7, nov1 = 1 + (7 - dow(Y, 11, 1)) % 7;
  auto at = [&](int m, int d, int hour_utc) {
    std::tm x{};
    x.tm_year = Y - 1900; x.tm_mon = m - 1; x.tm_mday = d; x.tm_hour = hour_utc;
    return static_cast<long long>(timegm(&x));
  };
  // DST starts 2:00 EST (07:00Z) on the second Sunday of March, ends 2:00 EDT (06:00Z) on the first Sunday of November.
  bool dst = epoch_utc >= at(3, mar2, 7) && epoch_utc < at(11, nov1, 6);
  return dst ? -4 * 3600 : -5 * 3600;
}

// UTC offset (seconds) for a handful of IANA zone names the desk cares about, with the US DST
// rule where it applies. Unknown names fall back to UTC.
inline int tz_offset_seconds(const std::string& tz, long long epoch_utc) {
  int std_hours = 0;
  bool dst_rule = false;
  if (tz == "America/New_York") { std_hours = -5; dst_rule = true; }
  else if (tz == "America/Chicago") { std_hours = -6; dst_rule = true; }
  else if (tz == "America/Denver") { std_hours = -7; dst_rule = true; }
  else if (tz == "America/Phoenix") { std_hours = -7; dst_rule = false; }
  else if (tz == "America/Los_Angeles") { std_hours = -8; dst_rule = true; }
  else if (tz == "America/Anchorage") { std_hours = -9; dst_rule = true; }
  else if (tz == "Pacific/Honolulu") { std_hours = -10; dst_rule = false; }
  else return 0;
  if (!dst_rule) return std_hours * 3600;
  // The Eastern rule's boundaries are 07:00Z / 06:00Z; other zones shift by their standard offset difference.
  int shift = (-5 - std_hours) * 3600;
  return us_eastern_offset(epoch_utc - shift) == -4 * 3600 ? (std_hours + 1) * 3600 : std_hours * 3600;
}

// The desk's trading day: the local calendar date in `tz` ("P&L today", the daily stop and the
// auto budget all reset at local midnight).
inline std::string trading_day(const std::string& tz, long long epoch_utc) {
  std::time_t t = static_cast<std::time_t>(epoch_utc + tz_offset_seconds(tz, epoch_utc));
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

// "YYYY-MM-DDTHH:MM:SSZ" for an epoch (UTC).
inline std::string iso_of(long long epoch_utc) {
  std::time_t t = static_cast<std::time_t>(epoch_utc);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

inline std::string date_of(long long epoch_utc, int offset_seconds) {
  std::time_t t = static_cast<std::time_t>(epoch_utc + offset_seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

inline int hour_of(long long epoch_utc, int offset_seconds) {
  std::time_t t = static_cast<std::time_t>(epoch_utc + offset_seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  return tm.tm_hour;
}

}  // namespace desk
