#include "deskd/web/contract.h"

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

json hb_json(const std::string& epoch, const std::string& sid, std::int64_t srv_ms, const json& feeds) {
  return json{{"epoch", epoch}, {"sid", sid}, {"srv_ms", srv_ms}, {"feeds", feeds}};
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
