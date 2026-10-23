#include "deskd/strat/calendar.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "deskd/gw/kalshi_wire.h"
#include "deskd/web/contract.h"

namespace deskd {

namespace {

using json = nlohmann::json;

const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// "HH:MM" -> minute of the day
std::optional<int> minute_of(const std::string& s) {
  int h = -1, m = -1;
  char extra = 0;
  if (std::sscanf(s.c_str(), "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return std::nullopt;
  return h * 60 + m;
}
bool is_date(const std::string& d) {
  return d.size() == 10 && d[4] == '-' && d[7] == '-' && kalshi::parse_time_ms(d + "T12:00:00Z").has_value();
}
// the ET midnight of an ET date: noon UTC of that date is 7 or 8 am ET on it
std::int64_t midnight_of(const std::string& date) { return et_midnight_ms(*kalshi::parse_time_ms(date + "T12:00:00Z")); }
int weekday_of(const std::string& date) {   // 0 Sunday
  const std::int64_t days = *kalshi::parse_time_ms(date + "T12:00:00Z") / 86400000;
  return static_cast<int>(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
}

}  // namespace

std::string ReleaseCalendar::parse(const json& j) {
  releases_.clear();
  if (!j.is_object() || !j.contains("releases") || !j["releases"].is_array()) return "wants {\"releases\": [...]}";
  for (const auto& r : j["releases"]) {
    Release out;
    if (!r.is_object() || !r.contains("name") || !r["name"].is_string() || r["name"].get<std::string>().empty())
      return "a release without a name";
    out.name = r["name"].get<std::string>();
    const std::string where = "release \"" + out.name + "\": ";
    if (!r.contains("series") || !r["series"].is_array() || r["series"].empty()) return where + "no series";
    for (const auto& s : r["series"]) {
      if (!s.is_string() || s.get<std::string>().empty()) return where + "a series is not a ticker prefix";
      out.series.push_back(s.get<std::string>());
    }
    for (const auto& w : r.value("weekly", json::array())) {
      const std::string text = w.is_string() ? w.get<std::string>() : "";
      const auto sp = text.find(' ');
      const auto day = std::find(std::begin(kDays), std::end(kDays), text.substr(0, sp == std::string::npos ? 0 : sp));
      const auto minute = sp == std::string::npos ? std::nullopt : minute_of(text.substr(sp + 1));
      if (day == std::end(kDays) || !minute) return where + "weekly \"" + text + "\" is not like \"Thu 10:30\"";
      out.weekly.push_back({static_cast<int>(day - std::begin(kDays)), *minute});
    }
    for (const auto& s : r.value("skip", json::array())) {
      if (!s.is_string() || !is_date(s.get<std::string>())) return where + "skip wants dates like \"2026-11-26\"";
      out.skip.insert(s.get<std::string>());
    }
    for (const auto& d : r.value("dates", json::array())) {
      const std::string text = d.is_string() ? d.get<std::string>() : "";
      const auto minute = text.size() == 16 ? minute_of(text.substr(11)) : std::nullopt;
      if (!minute || !is_date(text.substr(0, 10)) || text[10] != ' ')
        return where + "date \"" + text + "\" is not like \"2026-11-12 08:30\"";
      out.dates_ms.push_back(midnight_of(text.substr(0, 10)) + *minute * 60000LL);
    }
    if (out.weekly.empty() && out.dates_ms.empty()) return where + "neither weekly nor dates";
    for (const auto& k : r.items())
      if (k.key() != "name" && k.key() != "series" && k.key() != "weekly" && k.key() != "skip" && k.key() != "dates" &&
          k.key() != "note")
        return where + "unknown key \"" + k.key() + "\"";
    releases_.push_back(std::move(out));
  }
  return "";
}

std::string ReleaseCalendar::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "cannot read " + path;
  std::stringstream text;
  text << f.rdbuf();
  const auto j = json::parse(text.str(), nullptr, false);
  if (j.is_discarded()) return path + ": not JSON";
  const auto why = parse(j);
  return why.empty() ? "" : path + ": " + why;
}

bool ReleaseCalendar::covers(const Release& r, const std::string& ticker) const {
  return std::any_of(r.series.begin(), r.series.end(), [&](const std::string& s) {
    return s == "*" || ticker.compare(0, s.size(), s) == 0;
  });
}

std::vector<std::int64_t> ReleaseCalendar::times(const Release& r, const std::string& from_date, int days) const {
  std::vector<std::int64_t> out;
  std::string date = from_date;
  for (int i = 0; i < days; ++i, date = et_date_after(date)) {
    if (r.skip.count(date)) continue;
    const int wd = weekday_of(date);
    for (const auto& [day, minute] : r.weekly)
      if (day == wd) out.push_back(midnight_of(date) + minute * 60000LL);
  }
  return out;
}

std::string ReleaseCalendar::active(const std::string& ticker, std::int64_t now) const {
  // a window reaches back 5 minutes and forward 1: yesterday's ET day can still hold one
  const std::string from = et_date(now - kAfterMs);
  for (const auto& r : releases_) {
    if (!covers(r, ticker)) continue;
    auto at = times(r, from, 2);
    at.insert(at.end(), r.dates_ms.begin(), r.dates_ms.end());
    for (const auto t : at)
      if (now >= t - kBeforeMs && now <= t + kAfterMs) return r.name;
  }
  return "";
}

std::optional<std::pair<std::string, std::int64_t>> ReleaseCalendar::next(const std::string& ticker, std::int64_t now,
                                                                           int days) const {
  std::optional<std::pair<std::string, std::int64_t>> best;
  for (const auto& r : releases_) {
    if (!covers(r, ticker)) continue;
    auto at = times(r, et_date(now), days);
    at.insert(at.end(), r.dates_ms.begin(), r.dates_ms.end());
    for (const auto t : at)
      if (t >= now && t <= now + days * 86400000LL && (!best || t < best->second)) best = std::make_pair(r.name, t);
  }
  return best;
}

}  // namespace deskd
