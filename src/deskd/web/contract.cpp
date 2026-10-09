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
  json result{
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
  if (in.cash_k.is_object()) result["cash"]["K"] = in.cash_k;
  if (in.kx_lag_ms) result["lat"]["kx_lag_ms"] = *in.kx_lag_ms;
  if (in.clk_off_ms) result["lat"]["clk_off_ms"] = *in.clk_off_ms;
  if (in.gw_ack_ms) result["lat"]["gw_ack_ms"] = *in.gw_ack_ms;
  return result;
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

json universe_json(const json& q, std::int64_t wall_ms) {
  const std::string today = et_date(wall_ms);
  const std::string day = q.value("day", std::string("T"));
  json cats = json::object();
  for (const char* c : {"SPORTS", "WEATHER", "FIN", "ECON", "OTHER"}) cats[c] = 0;
  // grev names the result's shape (its groups and their counts; contract 1.5). The empty listing
  // has one shape, kEmptyUniverseGrev; a page that holds it already sends it back and gets
  // groups null, as it will when the shape is thousands of groups on a real day.
  const bool same_shape = q.contains("grev") && q["grev"].is_string() && q["grev"] == kEmptyUniverseGrev;
  return json{{"ok", true},
              {"day", day},
              {"date", day == "T1" ? et_date_after(today) : today},
              {"total", 0},
              {"offset", q.contains("offset") && q["offset"].is_number_integer() ? q["offset"] : json(0)},
              {"rows", json::array()},
              {"groups", same_shape ? json(nullptr) : json(json::array())},
              {"grev", kEmptyUniverseGrev},
              {"cats", cats},
              {"ts_ms", wall_ms}};
}

// ---- the strict request checker ---------------------------------------------------------------
// The schema nodes are contract.js's own notation, in JSON, so a line here can be compared with
// the line there by eye:
//   "type" / "type?"            a leaf type ("?" also allows null)
//   {field: node}               a strict object; a key ending in "?" may be absent
//   ["enum", a, b] / ["enum?", ...], ["arr", node], ["or", n1, n2]
namespace {

bool all_of(const std::string& s, std::size_t lo, std::size_t hi, const char* extra) {
  if (s.size() < lo || s.size() > hi) return false;
  for (char c : s) {
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    bool ok = alnum;
    for (const char* e = extra; !ok && *e; ++e) ok = c == *e;
    if (!ok) return false;
  }
  return true;
}
bool digits(const std::string& s, std::size_t at, std::size_t n) {
  if (s.size() < at + n) return false;
  for (std::size_t i = at; i < at + n; ++i)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
// RE_PRICE ^[01]\.\d{4}$ and at most 1
bool price_ok(const json& v) {
  if (!v.is_string()) return false;
  const std::string s = v.get<std::string>();
  return s.size() == 6 && (s[0] == '0' || s[0] == '1') && s[1] == '.' && digits(s, 2, 4) && (s[0] == '0' || s == "1.0000");
}
// RE_MONEY ^-?\d{1,9}\.\d{4}$, never "-0.0000"
bool money_ok(const json& v) {
  if (!v.is_string()) return false;
  const std::string s = v.get<std::string>();
  const std::size_t neg = !s.empty() && s[0] == '-' ? 1 : 0;
  const std::size_t dot = s.find('.');
  if (dot == std::string::npos || dot <= neg || dot - neg > 9 || !digits(s, neg, dot - neg)) return false;
  return s.size() == dot + 5 && digits(s, dot + 1, 4) && s != "-0.0000";
}
bool is_int(const json& v) {
  if (v.is_number_integer()) return true;
  if (!v.is_number_float()) return false;
  const double d = v.get<double>();
  return std::isfinite(d) && std::floor(d) == d;
}
double num(const json& v) { return v.is_number() ? v.get<double>() : NAN; }
bool two_dp(double v) { return std::fabs(v * 100 - std::round(v * 100)) < 1e-6; }
// JavaScript's string length counts UTF-16 units; for the 400 cap the count of code points is
// close enough (an astral character counts 1 here, 2 there)
std::size_t js_length(const std::string& s) {
  std::size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

bool leaf_ok(const std::string& t, const json& v) {
  if (t == "price") return price_ok(v);
  if (t == "money") return money_ok(v);
  if (t == "size") return v.is_number() && std::isfinite(num(v)) && num(v) >= 0 && two_dp(num(v));
  if (t == "ssize") return v.is_number() && std::isfinite(num(v)) && two_dp(num(v));
  if (t == "count") return is_int(v) && num(v) >= 0;
  if (t == "int") return is_int(v);
  if (t == "ms") return is_int(v) && num(v) >= 1e12 && num(v) < 1e14;
  if (t == "num") return v.is_number() && std::isfinite(num(v));
  if (t == "str") return v.is_string() && js_length(v.get<std::string>()) <= 400;
  if (t == "bool") return v.is_boolean();
  if (t == "cid") return v.is_string() && all_of(v.get<std::string>(), 6, 40, "_-");
  if (t == "tk") return v.is_string() && all_of(v.get<std::string>(), 1, 96, "._-");
  if (t == "id") return v.is_string() && all_of(v.get<std::string>(), 1, 64, "._:|-");
  if (t == "date") {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    return s.size() == 10 && digits(s, 0, 4) && s[4] == '-' && digits(s, 5, 2) && s[7] == '-' && digits(s, 8, 2);
  }
  return false;
}

std::string brief(const json& v) {
  std::string s = v.dump();
  return s.size() > 60 ? s.substr(0, 57) + "..." : s;
}

void validate(const json& node, const json& v, const std::string& path, std::vector<std::string>& errs) {
  if (node.is_string()) {
    std::string t = node.get<std::string>();
    const bool nullable = !t.empty() && t.back() == '?';
    if (nullable) t.pop_back();
    if (nullable && v.is_null()) return;
    if (!leaf_ok(t, v)) errs.push_back(path + ": expected " + t + (nullable ? "|null" : "") + ", got " + brief(v));
    return;
  }
  if (node.is_array()) {
    const std::string op = node[0].get<std::string>();
    if (op == "enum" || op == "enum?") {
      if (op == "enum?" && v.is_null()) return;
      for (std::size_t i = 1; i < node.size(); ++i)
        if (node[i] == v && node[i].is_boolean() == v.is_boolean()) return;
      errs.push_back(path + ": expected one of " + json(std::vector<json>(node.begin() + 1, node.end())).dump() +
                     ", got " + brief(v));
      return;
    }
    if (op == "arr") {
      if (!v.is_array()) {
        errs.push_back(path + ": expected array, got " + brief(v));
        return;
      }
      for (std::size_t i = 0; i < v.size(); ++i) validate(node[1], v[i], path + "[" + std::to_string(i) + "]", errs);
      return;
    }
    if (op == "or") {
      std::vector<std::string> best;
      bool have = false;
      for (std::size_t a = 1; a < node.size(); ++a) {
        std::vector<std::string> e;
        validate(node[a], v, path, e);
        if (e.empty()) return;
        if (!have || e.size() < best.size()) best = std::move(e);
        have = true;
      }
      errs.insert(errs.end(), best.begin(), best.end());
      return;
    }
    errs.push_back(path + ": schema bug, unknown op " + op);
    return;
  }
  // a strict object
  if (!v.is_object()) {
    errs.push_back(path + ": expected object, got " + brief(v));
    return;
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    std::string k = it.key();
    const bool opt = !k.empty() && k.back() == '?';
    if (opt) k.pop_back();
    if (!v.contains(k)) {
      if (!opt) errs.push_back(path + "." + k + ": missing");
      continue;
    }
    validate(it.value(), v[k], path + "." + k, errs);
  }
  for (auto it = v.begin(); it != v.end(); ++it)
    if (!node.contains(it.key()) && !node.contains(it.key() + "?")) errs.push_back(path + "." + it.key() + ": not in the contract");
}

// contract.js's endpoints[...].request, version 1.5.0 (no request body changed from 1.4)
const json& request_schemas() {
  static const json venue = json::array({"enum", "K", "P"});
  static const json ref = {{"v", venue}, {"tk", "tk"}};
  static const json ttl = json::array({"enum?", 5, 15, 60, 300, 1800});
  static const json own = json::array({"enum", "MAN", "SYS", "ANY"});
  static const json s = {
      {"POST /api/subscribe",
       {{"sid", "str"}, {"ladders", json::array({"arr", ref})}, {"tops", json::array({"or", json::array({"enum", "ALL"}), json::array({"arr", ref})})}}},
      {"POST /api/order",
       {{"cid", "cid"},
        {"v", venue},
        {"tk", "tk"},
        {"side", json::array({"enum", "BUY", "SELL"})},
        {"px", "price"},
        {"qty", "size"},
        {"tif", json::array({"enum", "IOC", "FOK", "POST"})},
        {"ttl_s", ttl},
        {"exp_ms?", "ms?"},
        {"reduce_only?", "bool"},
        {"override", "bool"},
        {"touch", "price?"},
        {"t_click_ms", "ms?"}}},
      {"POST /api/cancel",
       json::array({"or",
                    {{"cids", json::array({"arr", "cid"})}},
                    {{"v", venue}, {"tk", "tk"}, {"own", own}, {"side", json::array({"enum?", "BUY", "SELL"})}, {"px", "price?"}}})},
      {"POST /api/cancel_all", {{"own", own}, {"v", json::array({"enum?", "K", "P"})}}},
      {"POST /api/amend", {{"cid", "cid"}, {"px", "price?"}, {"qty", "size?"}, {"ttl_s", ttl}}},
      {"POST /api/lane/{id}", {{"on", "bool"}}},
      {"POST /api/start", json::object()},
      {"POST /api/stop", json::object()},
      {"POST /api/kill", json::object()},
      {"POST /api/rearm", json::object()},
      {"POST /api/release", ref},
      {"POST /api/watch", {{"v", venue}, {"tk", "tk"}, {"on", "bool"}}},
      {"POST /api/transfer", {{"from", "id"}, {"to", "id"}, {"amt", "money"}}}};
  return s;
}

bool inside(const json& p) { return price_ok(p) && std::stod(p.get<std::string>()) > 0 && std::stod(p.get<std::string>()) < 1; }
bool present(const json& b, const char* k) { return b.contains(k) && !b[k].is_null(); }

}  // namespace

std::vector<std::string> check_request(const std::string& key, const json& body) {
  std::vector<std::string> errs;
  const json& schemas = request_schemas();
  if (!schemas.contains(key)) return {"no request schema for " + key};
  validate(schemas[key], body, "$", errs);
  if (!errs.empty()) return errs;
  // validateRequest's rules, in its order
  if (key == "POST /api/order") {
    const bool has_ttl = present(body, "ttl_s"), has_exp = present(body, "exp_ms");
    const std::string tif = body["tif"].get<std::string>();
    if (tif == "POST" && has_ttl == has_exp) errs.push_back("$: a POST needs exactly one of ttl_s and exp_ms");
    if (tif != "POST" && (has_ttl || has_exp)) errs.push_back("$: " + tif + " carries neither ttl_s nor exp_ms");
    if (body.contains("reduce_only") && body["reduce_only"] != false && tif != "IOC") errs.push_back("$.reduce_only: IOC only");
    if (!(num(body["qty"]) > 0)) errs.push_back("$.qty: must be > 0");
    if (!inside(body["px"])) errs.push_back("$.px: an order price is strictly between 0 and 1");
    if (has_exp && std::fmod(num(body["exp_ms"]), 1000.0) != 0) errs.push_back("$.exp_ms: a whole second");
  }
  if (key == "POST /api/amend") {
    if (!body["px"].is_null() && !inside(body["px"])) errs.push_back("$.px: an order price is strictly between 0 and 1");
    if (!body["qty"].is_null() && !(num(body["qty"]) > 0)) errs.push_back("$.qty: must be > 0");
    if (body["px"].is_null() && body["qty"].is_null() && body["ttl_s"].is_null()) errs.push_back("$: nothing to amend");
  }
  if (key == "POST /api/transfer" && !(std::stod(body["amt"].get<std::string>()) > 0)) errs.push_back("$.amt: must be > 0");
  return errs;
}

std::vector<std::string> check_universe_query(const json& q) {
  static const json schema = {
      {"day", json::array({"enum", "T", "T1"})},
      {"cat?", json::array({"or", json::array({"enum", ""}), json::array({"enum", "SPORTS", "WEATHER", "FIN", "ECON", "OTHER"})})},
      {"v?", json::array({"enum", "", "K", "P"})},
      {"q?", "str"},
      {"sort?", json::array({"enum", "vol", "sprd", "close", "edge"})},
      {"offset?", "count"},
      {"limit?", "count"},
      {"grev?", "str"}};
  std::vector<std::string> errs;
  validate(schema, q, "$", errs);
  if (q.is_object() && q.contains("limit") && q["limit"].is_number() && (num(q["limit"]) < 1 || num(q["limit"]) > 200))
    errs.push_back("$.limit: 1 to 200");
  return errs;
}

}  // namespace deskd
