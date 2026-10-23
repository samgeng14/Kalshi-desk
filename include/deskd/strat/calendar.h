#pragma once
// Data releases that pull the makers' quotes (plan section 5: both sides, from 60 s before a
// release to 5 minutes after). config/calendar.json:
//
//   {"releases": [
//     {"name": "EIA natural gas storage", "series": ["KXNATGAS"], "weekly": ["Thu 10:30"],
//      "skip": ["2026-11-26"], "dates": ["2026-11-25 12:00"]},
//     {"name": "CPI", "series": ["*"], "dates": ["2026-11-12 08:30"]}]}
//
// Times are US Eastern wall clock, the releases' own. "weekly" repeats on that weekday, except on
// a "skip" date (a holiday week moves it: name the moved one in "dates"). A series matches a
// ticker by prefix ("KXNATGAS" matches KXNATGAS15M-...); "*" is every market. The ET time of day
// is counted from that day's ET midnight, so on the two Sundays a year the clock changes it is an
// hour off after 2 am: no release here falls on a Sunday.
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace deskd {

class ReleaseCalendar {
 public:
  static constexpr std::int64_t kBeforeMs = 60000, kAfterMs = 300000;
  // "" on success; an empty calendar for a missing file is the caller's choice
  std::string parse(const nlohmann::json& j);
  std::string load(const std::string& path);   // the file's JSON; "" or why not
  // the release whose window holds now for this ticker, or ""
  std::string active(const std::string& ticker, std::int64_t now_ms) const;
  // the next release (its name and time) at or after now for this ticker, within `days` days
  std::optional<std::pair<std::string, std::int64_t>> next(const std::string& ticker, std::int64_t now_ms, int days = 8) const;
  std::size_t size() const { return releases_.size(); }

 private:
  struct Release {
    std::string name;
    std::vector<std::string> series;
    std::vector<std::pair<int, int>> weekly;   // weekday (0 Sunday), minute of the ET day
    std::set<std::string> skip;                // ET dates
    std::vector<std::int64_t> dates_ms;
  };
  bool covers(const Release& r, const std::string& ticker) const;
  // every time of `r` on the ET days from `from_date`, `days` of them
  std::vector<std::int64_t> times(const Release& r, const std::string& from_date, int days) const;
  std::vector<Release> releases_;
};

}  // namespace deskd
