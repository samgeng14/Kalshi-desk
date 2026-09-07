#pragma once
// Fixed-point string helpers. Kalshi V2 sends prices as "*_dollars" strings
// (up to 4 decimals) and quantities as "*_fp" strings (2 decimals).
#include <cmath>
#include <cstdio>
#include <string>

namespace desk {

inline double parse_num(const std::string& s, double dflt = 0.0) {
  if (s.empty()) return dflt;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return dflt;
  return v;
}

// "0.5600"
inline std::string fmt_dollars(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return buf;
}

inline double floor_contracts(double v) { return std::floor(v * 100.0 + 1e-8) / 100.0; }

// "10.00"
inline std::string fmt_fp(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

inline std::string fmt2(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

// Round a price down / up / nearest to the market tick (default 1 cent).
inline double round_to_tick(double p, double tick = 0.01) {
  return std::round(p / tick) * tick;
}
inline double floor_to_tick(double p, double tick = 0.01) {
  return std::floor(p / tick + 1e-9) * tick;
}
inline double ceil_to_tick(double p, double tick = 0.01) {
  return std::ceil(p / tick - 1e-9) * tick;
}
inline double clamp_price(double p, double tick = 0.01) {
  if (p < tick) return tick;
  if (p > 1.0 - tick) return 1.0 - tick;
  return p;
}

}  // namespace desk
