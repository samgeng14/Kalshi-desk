#include "deskd/web/contract.h"

#include <cmath>
#include <cstdio>
#include <map>

namespace deskd {

using json = nlohmann::json;

json err_body(const std::string& code, const std::string& msg) {
  // contract 'str' is at most 400 characters
  return json{{"ok", false}, {"err", {{"code", code}, {"msg", msg.size() > 400 ? msg.substr(0, 397) + "..." : msg}}}};
}

int err_http(const std::string& code) {
  static const std::map<std::string, int> http = {
      {"BAD_REQUEST", 400}, {"OFF_GRID", 400},      {"BAD_QTY", 400},    {"UNKNOWN_MARKET", 404},
      {"NOT_FOUND", 404},   {"DUP_CID", 409},       {"MARKET_CLOSED", 409}, {"PRICE_BAND", 409},
      {"REDUCE", 409},      {"FORBIDDEN", 403},     {"KILLED", 409},     {"STALE", 409},
      {"RO", 409},          {"NO_CASH", 409},       {"SHARD_CASH", 409}, {"SYS_CEILING", 409},
      {"HALTED", 409},      {"PREFLIGHT", 409},     {"BAD_STATE", 409},  {"RATE", 429},
      {"EXCHANGE", 502},    {"INTERNAL", 500}};
  auto it = http.find(code);
  return it == http.end() ? 500 : it->second;
}

namespace {
json ms_or_null(std::int64_t ms) { return ms > 0 ? json(ms) : json(nullptr); }
json ref_json(const MarketRef& r) { return json{{"v", venue_code(r.v)}, {"tk", r.tk}}; }
}  // namespace

json run_json(const RunView& r) {
  return json{{"state", run_code(r.state)},
              {"since_ms", ms_or_null(r.since_ms)},
              {"rearm_ms", r.state == RunState::KILLED ? ms_or_null(r.rearm_ms) : json(nullptr)},
              {"why", r.why}};
}

json preflight_json(const std::vector<PreflightRow>& rows) {
  json a = json::array();
  for (const auto& r : rows) a.push_back(json{{"id", r.id}, {"ok", r.ok}, {"why", r.why}});
  return a;
}

json feed_json(const FeedView& f) {
  return json{{"up", f.up}, {"gap", f.gap}, {"lag_ms", f.lag_ms ? json(*f.lag_ms) : json(nullptr)}};
}

json feeds_json(const Feeds& f) {
  return json{{"KX", feed_json(f.KX.view())}, {"KXP", feed_json(f.KXP.view())}, {"PM", feed_json(f.PM.view())}};
}

json subs_json(const Subs& s) {
  json ladders = json::array(), tops = json::array();
  for (const auto& r : s.ladders) ladders.push_back(ref_json(r));
  for (const auto& r : s.tops) tops.push_back(ref_json(r));
  return json{{"ladders", ladders}, {"tops", s.tops_all ? json("ALL") : tops}};
}

// ---- ET dates ---------------------------------------------------------------------------------
namespace {
// days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms)
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
void civil_from_days(std::int64_t z, std::int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = static_cast<std::int64_t>(yoe) + era * 400 + (*m <= 2);
}
std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
// the n-th Sunday (1-based) of month m in year y, as days since the epoch
std::int64_t nth_sunday(std::int64_t y, unsigned m, int n) {
  const std::int64_t first = days_from_civil(y, m, 1);
  const std::int64_t wd = ((first % 7) + 7 + 4) % 7;   // 1970-01-01 was a Thursday (4); 0 = Sunday
  return first + (7 - wd) % 7 + 7 * (n - 1);
}
std::string ymd(std::int64_t days) {
  std::int64_t y;
  unsigned m, d;
  civil_from_days(days, &y, &m, &d);
  char b[16];
  std::snprintf(b, sizeof b, "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
  return b;
}
}  // namespace

std::string et_date(std::int64_t wall_ms) {
  const std::int64_t utc_s = floor_div(wall_ms, 1000);
  std::int64_t y;
  unsigned m, d;
  civil_from_days(floor_div(utc_s, 86400), &y, &m, &d);
  // EDT from 02:00 EST (07:00 UTC) on the second Sunday of March to 02:00 EDT (06:00 UTC) on the
  // first Sunday of November
  const std::int64_t dst_on = nth_sunday(y, 3, 2) * 86400 + 7 * 3600;
  const std::int64_t dst_off = nth_sunday(y, 11, 1) * 86400 + 6 * 3600;
  const std::int64_t offset = (utc_s >= dst_on && utc_s < dst_off) ? -4 * 3600 : -5 * 3600;
  return ymd(floor_div(utc_s + offset, 86400));
}

std::string et_date_after(const std::string& date) {
  int y = 0;
  unsigned m = 0, d = 0;
  if (std::sscanf(date.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return date;
  return ymd(days_from_civil(y, m, d) + 1);
}

json hb_json(const std::string& epoch, const std::string& sid, std::int64_t srv_ms, const json& feeds) {
  return json{{"epoch", epoch}, {"sid", sid}, {"srv_ms", srv_ms}, {"date", et_date(srv_ms)}, {"feeds", feeds}};
}

json hdr_json(const HdrInputs& in) {
  const std::string zero = money_str(0);
  const json venue_sys = {{"used", zero}, {"ceiling", zero}, {"eff", zero}};
  return json{
      {"mode", in.live ? "LIVE" : "DEMO"},
      {"cash",
       {{"K", {{"total", zero}, {"free", zero}, {"ro", true}, {"shards", json::array()}}},
        {"P", {{"total", zero}, {"free", zero}, {"ro", true}}}}},
      {"day", {{"total", zero}, {"sys", zero}, {"man", zero}, {"lip", zero}}},
      {"risk", zero},
      // left = limit + min(0, day.sys): the whole stop while nothing has traded
      {"stop", {{"left", money_str(in.daily_stop)}, {"limit", money_str(in.daily_stop)}, {"hit", false}}},
      {"sys", {{"K", venue_sys}, {"P", venue_sys}}},
      {"cost", {{"used", zero}, {"budget", zero}}},
      {"run", run_json(in.run)},
      {"preflight", preflight_json(in.preflight)},
      {"lat", {{"kx_lag_ms", nullptr}, {"pm_lag_ms", nullptr}, {"gw_ack_ms", nullptr}, {"clk_off_ms", nullptr}}}};
}

json alert_json(const std::string& id, const std::string& lvl, const std::string& src, const std::string& msg,
                std::int64_t ts_ms) {
  return json{{"id", id}, {"lvl", lvl}, {"src", src}, {"msg", msg.size() > 400 ? msg.substr(0, 397) + "..." : msg},
              {"ts_ms", ts_ms}};
}

json snapshot_json(const std::string& epoch, const std::string& sid, std::uint64_t seq, std::int64_t srv_ms,
                   const json& hdr, const json& alerts, const json& feeds, const Subs& subs) {
  return json{{"epoch", epoch},
              {"sid", sid},
              {"seq", seq},
              {"srv_ms", srv_ms},
              {"hdr", hdr},
              {"date", et_date(srv_ms)},
              {"markets", json::array()},
              {"books", json::array()},
              {"orders", json::array()},
              {"fills", json::array()},
              {"mos", json::array()},
              {"pos", json::array()},
              {"lanes", json::array()},
              {"risk", {{"groups", json::array()}, {"shards", json::array()}}},
              {"alerts", alerts},
              {"feeds", feeds},
              {"subs", subs_json(subs)}};
}

}  // namespace deskd
