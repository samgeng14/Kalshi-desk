#include "deskd/bus/units.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace deskd {

namespace {
// "d+.dddd" with an optional leading minus; digits before the point bounded by max_int_digits.
std::optional<std::int64_t> parse_fixed4(const std::string& s, bool allow_sign, std::size_t max_int_digits) {
  std::size_t i = 0;
  bool neg = false;
  if (allow_sign && i < s.size() && s[i] == '-') { neg = true; ++i; }
  const std::size_t int_start = i;
  std::int64_t whole = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') whole = whole * 10 + (s[i++] - '0');
  const std::size_t int_digits = i - int_start;
  if (int_digits == 0 || int_digits > max_int_digits) return std::nullopt;
  if (i >= s.size() || s[i] != '.') return std::nullopt;
  ++i;
  if (s.size() - i != 4) return std::nullopt;
  std::int64_t frac = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    frac = frac * 10 + (s[i] - '0');
  }
  const std::int64_t v = whole * 10000 + frac;
  return neg ? -v : v;
}
}  // namespace

std::optional<Px> parse_price(const std::string& s) {
  auto v = parse_fixed4(s, false, 1);
  if (!v || *v > kOneDollar) return std::nullopt;
  return v;
}

std::string price_str(Px p) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%lld.%04lld", static_cast<long long>(p / 10000),
                static_cast<long long>(p % 10000));
  return buf;
}

std::optional<Money> parse_money(const std::string& s) {
  auto v = parse_fixed4(s, true, 9);
  // the contract refuses "-0.0000": a zero has one spelling
  if (v && *v == 0 && !s.empty() && s[0] == '-') return std::nullopt;
  return v;
}

std::string money_str(Money m) {
  const unsigned long long a = m < 0 ? static_cast<unsigned long long>(-(m + 1)) + 1 : static_cast<unsigned long long>(m);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%s%llu.%04llu", m < 0 ? "-" : "", a / 10000, a % 10000);
  return buf;
}

std::optional<Qty> qty_from_number(double v, bool allow_negative) {
  if (!std::isfinite(v)) return std::nullopt;
  if (v < 0 && !allow_negative) return std::nullopt;
  const double hundredths = v * 100.0;
  const double r = std::round(hundredths);
  // the page's own test (contract.js twoDp): within 1e-6 of a whole hundredth
  if (std::fabs(hundredths - r) >= 1e-6) return std::nullopt;
  if (std::fabs(r) > 9.0e15) return std::nullopt;
  return static_cast<Qty>(r);
}

double qty_to_number(Qty q) { return static_cast<double>(q) / 100.0; }

}  // namespace deskd
