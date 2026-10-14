// strat/calendar: data releases in US Eastern wall clock, worked by hand (2026-10-15 is a
// Thursday, EDT, UTC-4; 2026-11-05 a Thursday after the clocks went back, EST, UTC-5).
#include <fstream>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/strat/calendar.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

std::int64_t utc(const char* s) { return *kalshi::parse_time_ms(s); }
const char* NG = "KXNATGAS15M-26OCT151430-30";

void windows() {
  ReleaseCalendar c;
  CHECK(c.parse(json::parse(R"({"releases": [
    {"name": "EIA natural gas storage", "series": ["KXNATGAS"], "weekly": ["Thu 10:30"], "skip": ["2026-11-26"],
     "dates": ["2026-11-25 12:00"]},
    {"name": "CPI", "series": ["*"], "dates": ["2026-10-14 08:30"]}]})")).empty());
  CHECK(c.size() == 2);
  // Thursday 10:30 EDT is 14:30 UTC: 60 s before to 5 minutes after
  CHECK(c.active(NG, utc("2026-10-15T14:28:59Z")).empty());
  CHECK(c.active(NG, utc("2026-10-15T14:29:00Z")) == "EIA natural gas storage");
  CHECK(c.active(NG, utc("2026-10-15T14:35:00Z")) == "EIA natural gas storage");
  CHECK(c.active(NG, utc("2026-10-15T14:35:01Z")).empty());
  CHECK(c.active("KXGOLD15M-26OCT151430-T4000", utc("2026-10-15T14:30:00Z")).empty());   // not its series
  CHECK(c.active(NG, utc("2026-10-16T14:30:00Z")).empty());                              // Friday
  // after the clocks went back, 10:30 EST is 15:30 UTC
  CHECK(c.active(NG, utc("2026-11-05T14:30:00Z")).empty());
  CHECK(c.active(NG, utc("2026-11-05T15:30:00Z")) == "EIA natural gas storage");
  // Thanksgiving: skipped, and moved to Wednesday 12:00 EST
  CHECK(c.active(NG, utc("2026-11-26T15:30:00Z")).empty());
  CHECK(c.active(NG, utc("2026-11-25T17:02:00Z")) == "EIA natural gas storage");
  // "*": every market; 08:30 EDT is 12:30 UTC
  CHECK(c.active("KXHIGHNY-26OCT14-B70", utc("2026-10-14T12:34:00Z")) == "CPI");
  CHECK(c.active(NG, utc("2026-10-14T12:34:00Z")) == "CPI");
  // the next one for a market
  auto n = c.next(NG, utc("2026-10-13T12:00:00Z"));
  CHECK(n && n->first == "CPI" && n->second == utc("2026-10-14T12:30:00Z"));
  n = c.next(NG, utc("2026-10-14T13:00:00Z"));
  CHECK(n && n->first == "EIA natural gas storage" && n->second == utc("2026-10-15T14:30:00Z"));
  CHECK(!c.next("KXHIGHNY-26OCT20-B70", utc("2026-10-15T00:00:00Z")));   // nothing for weather after the CPI
}

void refused() {
  ReleaseCalendar c;
  auto why = [&](const char* text) { return c.parse(json::parse(text)); };
  CHECK(!why(R"({})").empty());
  CHECK(why(R"({"releases": [{"series": ["X"], "weekly": ["Thu 10:30"]}]})").find("without a name") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "weekly": ["Thu 10:30"]}]})").find("no series") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"]}]})").find("neither weekly nor dates") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "weekly": ["Thursday 10:30"]}]})").find("not like") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "weekly": ["Thu 25:00"]}]})").find("not like") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "dates": ["2026-10-14T08:30"]}]})").find("not like") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "dates": ["2026-13-14 08:30"]}]})").find("not like") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "weekly": ["Thu 10:30"], "skip": ["26-11-26"]}]})").find("skip") != std::string::npos);
  CHECK(why(R"({"releases": [{"name": "A", "series": ["X"], "weekly": ["Thu 10:30"], "when": 1}]})").find("unknown key") != std::string::npos);
  CHECK(c.size() == 0);
}

void shipped() {   // the repo's own config/calendar.json loads, and covers natural gas
  ReleaseCalendar c;
  CHECK(c.load(std::string(DESKD_SOURCE_DIR) + "/config/calendar.json").empty());
  CHECK(c.size() >= 1 && c.active(NG, utc("2026-10-15T14:30:00Z")) == "EIA natural gas storage");
  CHECK(!c.load("/nonexistent/calendar.json").empty());
}

}  // namespace

int main() {
  windows();
  refused();
  shipped();
  return finish("deskd-calendar");
}
