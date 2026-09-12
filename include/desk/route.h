#pragma once
// Where a priced candidate goes: nowhere (off), into the research record, in front of a person
// (manual), or to the algorithm (auto). A route is a string in the settings and on the wire, and
// it must never be read as a boolean. A non-empty string is truthy in JavaScript and makes
// nlohmann::json::value<bool>() throw, and both happened: after the category switches became
// routes, book capture threw on every cycle (2026-09-11 13:05 UTC onwards) and the dashboard
// described every category as algorithm-traded.
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace desk {

enum class RouteMode { Off, Research, Manual, Auto };

inline const char* route_name(RouteMode r) {
  switch (r) {
    case RouteMode::Off: return "off";
    case RouteMode::Research: return "research";
    case RouteMode::Manual: return "manual";
    case RouteMode::Auto: return "auto";
  }
  return "off";
}

// Strict: exactly one of the four names. Anything else throws std::invalid_argument, so a typo in
// a settings patch is refused instead of quietly meaning something.
inline RouteMode parse_route(std::string_view s) {
  if (s == "off") return RouteMode::Off;
  if (s == "research") return RouteMode::Research;
  if (s == "manual") return RouteMode::Manual;
  if (s == "auto") return RouteMode::Auto;
  throw std::invalid_argument("route must be auto, manual, research or off, not \"" + std::string(s) + "\"");
}

// Tolerant read of one route from a settings object, for code that must not throw mid-cycle.
// A boolean is the shape this setting had before routes existed and keeps the meaning it had:
// true was the algorithm's list (auto), false already put the row in front of a person (manual).
// A missing key gives `missing`; an unreadable value is off, never silently auto.
inline RouteMode route_setting(const nlohmann::json& settings, const std::string& key,
                               RouteMode missing = RouteMode::Off) {
  if (!settings.is_object()) return missing;
  auto it = settings.find(key);
  if (it == settings.end() || it->is_null()) return missing;
  if (it->is_boolean()) return it->get<bool>() ? RouteMode::Auto : RouteMode::Manual;
  if (it->is_string()) {
    try { return parse_route(it->get<std::string>()); } catch (const std::invalid_argument&) { return RouteMode::Off; }
  }
  return RouteMode::Off;
}

// Data follows the route, execution does not. A manual or research candidate needs the same live
// book, fee and liquidity as an automatic one, so everything but "off" is captured; only "auto"
// may be sent without a person.
inline bool route_captures_data(RouteMode r) { return r != RouteMode::Off; }
inline bool route_sends_unattended(RouteMode r) { return r == RouteMode::Auto; }

}  // namespace desk
