#include "wire.h"

#include <cstdio>
#include <ctime>

namespace fakex {

namespace {
// digits [. digits] with between min_dp and max_dp decimals; a value of at most 12 integer digits
std::optional<std::int64_t> parse_dp(const std::string& s, int min_dp, int max_dp, int scale_dp, bool allow_negative) {
  std::size_t i = 0;
  bool neg = false;
  if (allow_negative && i < s.size() && s[i] == '-') {
    neg = true;
    ++i;
  }
  const std::size_t int_start = i;
  std::int64_t whole = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    whole = whole * 10 + (s[i++] - '0');
    if (i - int_start > 12) return std::nullopt;
  }
  if (i == int_start) return std::nullopt;
  int dp = 0;
  std::int64_t frac = 0;
  if (i < s.size()) {
    if (s[i] != '.') return std::nullopt;
    ++i;
    while (i < s.size()) {
      if (s[i] < '0' || s[i] > '9') return std::nullopt;
      frac = frac * 10 + (s[i++] - '0');
      ++dp;
    }
    if (dp == 0) return std::nullopt;  // "10." is not a number the venue documents
  }
  if (dp < min_dp || dp > max_dp || dp > scale_dp) return std::nullopt;
  std::int64_t scale = 1;
  for (int k = 0; k < scale_dp; ++k) scale *= 10;
  for (int k = dp; k < scale_dp; ++k) frac *= 10;
  const std::int64_t v = whole * scale + frac;
  return neg ? -v : v;
}

std::string fixed(std::int64_t v, int dp) {
  const bool neg = v < 0;
  const unsigned long long a = neg ? static_cast<unsigned long long>(-(v + 1)) + 1 : static_cast<unsigned long long>(v);
  unsigned long long scale = 1;
  for (int k = 0; k < dp; ++k) scale *= 10;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%s%llu.%0*llu", neg ? "-" : "", a / scale, dp, a % scale);
  return buf;
}
}  // namespace

std::optional<Px> parse_price_req(const std::string& s) { return parse_dp(s, 2, 4, 4, false); }
std::optional<Qty> parse_count_req(const std::string& s) { return parse_dp(s, 0, 2, 2, false); }
std::optional<std::int64_t> parse_fixed(const std::string& s, int dp, bool allow_negative) {
  return parse_dp(s, 0, dp, dp, allow_negative);
}

std::string px_str(Px p) { return fixed(p, 4); }
std::string qty_str(Qty q) { return fixed(q, 2); }
std::string usd6(Micros m) { return fixed(m, 6); }
std::string usd4(Micros m) { return m % 100 == 0 ? fixed(m / 100, 4) : fixed(m, 6); }

std::string rfc3339(std::int64_t ms) {
  const std::time_t s = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_r(&s, &tm);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms % 1000) * 1000);
  return buf;
}

std::string uuid4(std::mt19937_64& rng) {
  const std::uint64_t a = rng(), b = rng();
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08llx-%04llx-4%03llx-%04llx-%012llx", static_cast<unsigned long long>(a >> 32),
                static_cast<unsigned long long>((a >> 16) & 0xffff), static_cast<unsigned long long>(a & 0xfff),
                static_cast<unsigned long long>(0x8000 | ((b >> 48) & 0x3fff)),
                static_cast<unsigned long long>(b & 0xffffffffffffULL));
  return buf;
}

}  // namespace fakex
