#include "deskd/ref/espn.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <thread>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/net/http.h"
#include "deskd/web/contract.h"

namespace deskd {
using json = nlohmann::json;
namespace {
const json& jget(const json& j, const char* key) {
  static const json null_json;
  if (!j.is_object()) return null_json;
  const auto it = j.find(key);
  return it == j.end() ? null_json : *it;
}
std::string jstr(const json& j, const char* key) {
  const auto& v = jget(j, key);
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  return "";
}
int jint(const json& j, const char* key, int def) {
  const auto& v = jget(j, key);
  if (v.is_number()) return static_cast<int>(v.get<double>());
  if (v.is_string()) {
    const auto& s = v.get_ref<const std::string&>();
    char* end = nullptr;
    const long n = std::strtol(s.c_str(), &end, 10);
    if (!s.empty() && end == s.c_str() + s.size()) return static_cast<int>(n);
  }
  return def;
}
std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// ESPN's times: "2026-10-10T23:30Z", with or without seconds and fractions.
std::int64_t iso_ms(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  double sec = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) return 0;
  if (s.size() > 16 && s[16] == ':') sec = std::atof(s.c_str() + 17);
  if (s.find('Z') == std::string::npos) return 0;   // UTC only
  std::tm tm{};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi;
  const auto t = timegm(&tm);
  if (t <= 0) return 0;
  return static_cast<std::int64_t>(t) * 1000 + static_cast<std::int64_t>(sec * 1000);
}
std::string espn_et_date(std::int64_t ms) {
  const std::time_t noon = static_cast<std::time_t>((et_midnight_ms(ms) + 12 * 3600000LL) / 1000);
  std::tm tm{};
  gmtime_r(&noon, &tm);
  char buf[32];   // room for any int the compiler cannot rule out
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}
std::int64_t day_ms(const std::string& ymd) { return iso_ms(ymd + "T12:00Z"); }
std::vector<std::string> team_tokens(const std::string& name) {
  std::string s = lower(name);
  for (size_t p = s.find("a's"); p != std::string::npos; p = s.find("a's", p + 9))
    if ((p == 0 || !std::isalnum(static_cast<unsigned char>(s[p - 1]))) &&
        (p + 3 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[p + 3]))))
      s.replace(p, 3, "athletics");
  static const std::map<std::string, std::vector<std::string>> kCity = {
      {"la", {"los", "angeles"}}, {"ny", {"new", "york"}}, {"sf", {"san", "francisco"}},
      {"kc", {"kansas", "city"}}, {"tb", {"tampa", "bay"}}, {"gb", {"green", "bay"}}, {"lv", {"las", "vegas"}}};
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (cur.empty()) return;
    const auto it = kCity.find(cur);
    if (it != kCity.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    else out.push_back(cur);
    cur.clear();
  };
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
    else flush();
  }
  flush();
  return out;
}
double team_evidence(const std::string& code, const std::string& name, const std::string& abbr, const std::string& full) {
  return (!code.empty() && lower(code) == lower(abbr) ? 2.0 : 0.0) + team_name_overlap(name, full);
}
std::string ordinal(int n) {
  const int m = n % 100;
  const char* s = (m >= 11 && m <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
  return std::to_string(n) + s;
}
}  // namespace

double team_name_overlap(const std::string& a, const std::string& b) {
  const auto ta = team_tokens(a), tb = team_tokens(b);
  if (ta.empty() || tb.empty()) return 0;
  const auto& small = ta.size() <= tb.size() ? ta : tb;
  const auto& big = ta.size() <= tb.size() ? tb : ta;
  std::vector<bool> used(big.size(), false), done(small.size(), false);
  int hit = 0;
  for (size_t i = 0; i < small.size(); ++i) {   // whole words first
    if (small[i].size() < 2) continue;
    for (size_t j = 0; j < big.size(); ++j)
      if (!used[j] && big[j] == small[i]) { used[j] = true; done[i] = true; ++hit; break; }
  }
  for (size_t i = 0; i < small.size(); ++i) {   // then an initial takes a word it starts
    if (done[i] || small[i].size() != 1) continue;
    for (size_t j = 0; j < big.size(); ++j)
      if (!used[j] && big[j][0] == small[i][0]) { used[j] = true; ++hit; break; }
  }
  return static_cast<double>(hit) / static_cast<double>(small.size());
}

const std::vector<EspnLeague>& espn_leagues() {
  static const std::vector<EspnLeague> L = {
      {"KXNFLGAME", "football", "nfl", {}, 4, "Q"},
      {"KXNCAAFGAME", "football", "college-football", {"80", "81"}, 4, "Q"},
      {"KXNBAGAME", "basketball", "nba", {}, 4, "Q"},
      {"KXWNBAGAME", "basketball", "wnba", {}, 4, "Q"},
      {"KXNCAAMBGAME", "basketball", "mens-college-basketball", {"50"}, 2, "H"},
      {"KXNCAAWBGAME", "basketball", "womens-college-basketball", {"50"}, 4, "Q"},
      {"KXMLBGAME", "baseball", "mlb", {}, 9, ""},
      {"KXNHLGAME", "hockey", "nhl", {}, 3, "P"},
  };
  return L;
}

const EspnLeague* espn_league_for(const std::string& ticker) {
  for (const auto& l : espn_leagues())
    if (ticker.size() > l.series.size() && ticker.compare(0, l.series.size(), l.series) == 0 && ticker[l.series.size()] == '-')
      return &l;
  return nullptr;
}

std::string kalshi_event_date(const std::string& ev) {
  const auto p = ev.find('-');
  if (p == std::string::npos || ev.size() < p + 8) return "";
  const std::string d = ev.substr(p + 1, 7);   // 26OCT10
  static const char* kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  if (!std::isdigit(static_cast<unsigned char>(d[0])) || !std::isdigit(static_cast<unsigned char>(d[1])) ||
      !std::isdigit(static_cast<unsigned char>(d[5])) || !std::isdigit(static_cast<unsigned char>(d[6])))
    return "";
  int month = 0;
  for (int i = 0; i < 12; ++i) if (d.compare(2, 3, kMon[i]) == 0) month = i + 1;
  if (!month) return "";
  char buf[16];
  std::snprintf(buf, sizeof buf, "20%s-%02d-%s", d.substr(0, 2).c_str(), month, d.substr(5, 2).c_str());
  return buf;
}

int kalshi_event_start_minutes(const std::string& ev) {
  const auto p = ev.find('-');
  if (p == std::string::npos || ev.size() < p + 1 + 11) return -1;
  const std::string hhmm = ev.substr(p + 8, 4);
  for (char c : hhmm) if (c < '0' || c > '9') return -1;
  const int hh = std::stoi(hhmm.substr(0, 2)), mm = std::stoi(hhmm.substr(2, 2));
  return hh < 24 && mm < 60 ? hh * 60 + mm : -1;
}

std::vector<EspnGame> parse_scoreboard(const json& sb, const std::string& league, std::int64_t now) {
  std::vector<EspnGame> out;
  const auto& events = jget(sb, "events");
  if (!events.is_array()) return out;
  for (const auto& e : events) {
    const auto& comps = jget(e, "competitions");
    if (!comps.is_array() || comps.empty() || !comps[0].is_object()) continue;
    const auto& c = comps[0];
    EspnGame g;
    g.id = jstr(e, "id");
    if (g.id.empty()) continue;
    g.league = league;
    g.start_ms = iso_ms(jstr(e, "date"));
    const auto& status = jget(c, "status");
    const auto& type = jget(status, "type");
    g.state = jstr(type, "state");
    g.detail = jstr(type, "shortDetail");
    g.period = jint(status, "period", 0);
    g.clock = jstr(status, "displayClock");
    g.last_play_id = jstr(jget(jget(c, "situation"), "lastPlay"), "id");
    const auto& teams = jget(c, "competitors");
    if (!teams.is_array()) continue;
    for (const auto& t : teams) {
      const bool home = jstr(t, "homeAway") == "home";
      const auto& team = jget(t, "team");
      (home ? g.home_abbr : g.away_abbr) = jstr(team, "abbreviation");
      (home ? g.home_name : g.away_name) = jstr(team, "displayName");
      (home ? g.home_score : g.away_score) = jint(t, "score", 0);
    }
    if (g.home_abbr.empty() || g.away_abbr.empty() || (g.state != "pre" && g.state != "in" && g.state != "post")) continue;
    g.fetched_ms = now;
    out.push_back(std::move(g));
  }
  return out;
}

std::optional<EspnWp> parse_summary_wp(const json& j, const EspnGame& g) {
  const auto& header = jget(j, "header");
  const auto& comps = jget(header, "competitions");
  const auto& wp = jget(j, "winprobability");
  if (g.state != "in" || jstr(header, "id") != g.id || !comps.is_array() || comps.empty() || !wp.is_array() || wp.empty())
    return std::nullopt;
  const auto& comp = comps[0];
  const auto& status = jget(comp, "status");
  if (jstr(jget(status, "type"), "state") != g.state || jint(status, "period", -1) != g.period ||
      jstr(status, "displayClock") != g.clock)
    return std::nullopt;
  int home = -1, away = -1;
  const auto& teams = jget(comp, "competitors");
  if (!teams.is_array()) return std::nullopt;
  for (const auto& t : teams) {
    if (jstr(t, "homeAway") == "home") home = jint(t, "score", -1);
    else if (jstr(t, "homeAway") == "away") away = jint(t, "score", -1);
  }
  if (home != g.home_score || away != g.away_score) return std::nullopt;
  const auto& point = wp.back();
  const std::string play = jstr(point, "playId");
  std::string latest = g.last_play_id;
  std::int64_t play_ms = 0;
  const auto& plays = jget(j, "plays");
  if (plays.is_array() && !plays.empty()) {
    const auto id = jstr(plays.back(), "id");
    if (id.empty() || (!latest.empty() && latest != id)) return std::nullopt;
    latest = id;
    play_ms = iso_ms(jstr(plays.back(), "wallclock"));
  }
  // A successful fetch cannot make an older chart point current: the chart's last point must
  // be the latest play, identified.
  if (play.empty() || latest.empty() || play != latest) return std::nullopt;
  const auto& value = jget(point, "homeWinPercentage");
  const auto& tie = jget(point, "tiePercentage");
  if (!value.is_number()) return std::nullopt;
  const double h = value.get<double>(), t = tie.is_number() ? tie.get<double>() : 0;
  if (!std::isfinite(h) || h < 0 || h > 1 || !std::isfinite(t) || t < 0 || h + t > 1 + 1e-9) return std::nullopt;
  return EspnWp{h, t, play_ms};
}

std::optional<EspnMatch> match_game(const KalshiGame& k, const std::vector<EspnGame>& games) {
  if (k.date.empty()) return std::nullopt;
  const auto t_info = day_ms(k.date);
  double best = 0, second = 0;
  EspnMatch found;
  for (const auto& g : games) {
    if (g.start_ms <= 0) continue;
    const auto gd = espn_et_date(g.start_ms);
    if (std::llabs(day_ms(gd) - t_info) > 24 * 3600000LL) continue;
    double closeness = 0;
    if (k.start_min >= 0) {   // the two games of a doubleheader differ only in the time
      if (gd != k.date) continue;
      const int m = static_cast<int>((g.start_ms - et_midnight_ms(g.start_ms)) / 60000);
      if (std::abs(m - k.start_min) > 90) continue;
      closeness = 1.0 - std::abs(m - k.start_min) / 90.0;
    }
    auto ab = [](const std::string& code, const std::string& abbr) { return !code.empty() && lower(code) == lower(abbr) ? 2.0 : 0.0; };
    const double s1 = ab(k.code_a, g.away_abbr) + ab(k.code_b, g.home_abbr) + team_name_overlap(k.name_a, g.away_name) +
                      team_name_overlap(k.name_b, g.home_name);
    const double s2 = ab(k.code_a, g.home_abbr) + ab(k.code_b, g.away_abbr) + team_name_overlap(k.name_a, g.home_name) +
                      team_name_overlap(k.name_b, g.away_name);
    const bool a_home = s2 > s1;
    // both teams, in the winning orientation, need their own evidence
    const double ea = team_evidence(k.code_a, k.name_a, a_home ? g.home_abbr : g.away_abbr, a_home ? g.home_name : g.away_name);
    const double eb = team_evidence(k.code_b, k.name_b, a_home ? g.away_abbr : g.home_abbr, a_home ? g.away_name : g.home_name);
    if (std::min(ea, eb) < 0.75) continue;
    // a side that barely beats its mirror image is a guess; a wrong side is worse than none
    if (std::fabs(s1 - s2) < 1.0) continue;
    const double score = std::max(s1, s2) + (gd == k.date ? 0.5 : 0.0) + closeness;
    if (score < 2.0) continue;
    if (score > best) { second = best; best = score; found = EspnMatch{g.id, a_home}; }
    else if (score > second) second = score;
  }
  if (found.id.empty() || second >= best - 0.25) return std::nullopt;   // two games we cannot tell apart
  return found;
}

EspnRef espn_ref(const EspnLeague& lg, const EspnGame& g, bool yes_home, const std::optional<EspnWp>& wp) {
  EspnRef r;
  r.fetched_ms = g.fetched_ms;
  r.score = std::to_string(g.away_score) + "-" + std::to_string(g.home_score);
  if (g.state == "pre") {
    r.per = "PRE";
    const auto m = g.start_ms > 0 ? (g.start_ms - et_midnight_ms(g.start_ms)) / 60000 : 0;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(m / 60), static_cast<int>(m % 60));
    r.clock = g.start_ms > 0 ? buf : "";
  } else if (g.state == "post") {
    r.per = "FINAL";
  } else if (lg.period_label.empty()) {   // baseball: ESPN's own words ("Top 3rd")
    r.per = g.detail.empty() ? ordinal(g.period) : g.detail;
  } else {
    r.per = g.period > lg.periods ? "OT" : lg.period_label + std::to_string(g.period);
    r.clock = g.clock;
  }
  if (wp) {
    const double p = yes_home ? wp->home : std::max(0.0, 1.0 - wp->home - wp->tie);
    r.espn = static_cast<Px>(std::llround(std::min(1.0, std::max(0.0, p)) * kOneDollar));
    r.espn_ts_ms = wp->play_ms;
  }
  return r;
}

EspnFeed::EspnFeed(net::Endpoint ep, MarketsFn markets) : EspnFeed(std::move(ep), std::move(markets), Options{}) {}
EspnFeed::EspnFeed(net::Endpoint ep, MarketsFn markets, Options options)
    : endpoint_(std::move(ep)), markets_(std::move(markets)), options_(options),
      guard_(SendGuard::data_hosts({"site.api.espn.com"})), board_(std::make_shared<const EspnBoard>()) {
  factory_ = std::make_unique<net::ConnFactory>(guard_);
}
EspnFeed::~EspnFeed() = default;

std::shared_ptr<const EspnBoard> EspnFeed::board() const {
  std::lock_guard<std::mutex> lk(m_);
  return board_;
}

json EspnFeed::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json shown = json::object();
  for (const auto& [tk, r] : *board_)
    shown[tk] = {{"per", r.per}, {"clock", r.clock}, {"score", r.score},
                 {"espn", r.espn ? json(price_str(*r.espn)) : json(nullptr)}, {"fetched_ms", r.fetched_ms}};
  return {{"requests", requests_}, {"failures", failures_}, {"error", last_error_}, {"markets", shown}};
}

bool EspnFeed::get(const std::string& target, json* body, std::string* why, Worker* self) {
  {
    std::lock_guard<std::mutex> lk(m_);
    ++requests_;
  }
  auto fail = [&](const std::string& w) {
    std::lock_guard<std::mutex> lk(m_);
    ++failures_;
    last_error_ = w;
    *why = w;
    conn_.reset();
    return false;
  };
  if (!conn_ || !conn_->usable()) {
    conn_ = factory_->open(endpoint_, options_.timeout_ms, why);
    if (!conn_) return fail("connect: " + *why);
  }
  const auto bytes = net::request_bytes("GET", endpoint_.host_header(), endpoint_.base_path + target,
                                        {{"User-Agent", "curl/8.5.0"}, {"Accept", "application/json"}}, "", true);
  const auto x = net::exchange(*conn_, bytes, net::mono_us() + 1000LL * options_.timeout_ms, false,
                               [self] { if (self) self->beat(); });
  if (x.fate != net::Exchange::Fate::REPLIED) return fail(target.substr(0, 60) + ": " + x.why);
  if (!x.reusable) conn_.reset();
  if (x.res.status != 200) return fail(target.substr(0, 60) + ": HTTP " + std::to_string(x.res.status));
  *body = json::parse(x.res.body, nullptr, false);
  if (!body->is_object()) return fail(target.substr(0, 60) + ": not a JSON object");
  return true;
}

void EspnFeed::step(std::int64_t now, Worker* self) {
  // The watched game markets, by event: each market pays on one team (its ticker's last code).
  std::map<std::string, KalshiGame> events;
  std::map<std::string, const EspnLeague*> league_of;
  for (const auto& m : markets_()) {
    const EspnLeague* lg = espn_league_for(m.ticker);
    if (!lg || m.event_ticker.empty()) continue;
    auto& k = events[m.event_ticker];
    if (k.event_ticker.empty()) {
      k.event_ticker = m.event_ticker;
      k.series = lg->series;
      k.date = kalshi_event_date(m.event_ticker);
      k.start_min = kalshi_event_start_minutes(m.event_ticker);
    }
    league_of[m.event_ticker] = lg;
    const auto code = m.ticker.substr(m.ticker.find_last_of('-') + 1);
    if (k.ticker_a.empty() || k.ticker_a == m.ticker) { k.ticker_a = m.ticker; k.code_a = code; k.name_a = m.yes_sub_title; }
    else if (k.ticker_b.empty() || k.ticker_b == m.ticker) { k.ticker_b = m.ticker; k.code_b = code; k.name_b = m.yes_sub_title; }
  }
  // Scoreboards, for each league and event date within a day and a half of now.
  std::set<std::pair<const EspnLeague*, std::string>> want;
  for (const auto& [ev, k] : events)
    if (!k.date.empty() && std::llabs(day_ms(k.date) - now) <= 36 * 3600000LL) want.insert({league_of[ev], k.date});
  for (const auto& [lg, date] : want) {
    const auto key = lg->league + "|" + date;
    if (now - fetched_[key] < options_.scoreboard_ms) continue;
    fetched_[key] = now;
    const auto ymd = date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2);
    for (const auto& grp : lg->groups.empty() ? std::vector<std::string>{""} : lg->groups) {
      json body;
      std::string why;
      if (!get("/apis/site/v2/sports/" + lg->sport + "/" + lg->league + "/scoreboard?limit=300&dates=" + ymd +
                   (grp.empty() ? "" : "&groups=" + grp), &body, &why, self))
        continue;
      for (auto& g : parse_scoreboard(body, lg->league, now)) games_[g.id] = std::move(g);
    }
  }
  // Match each event once, then keep the match.
  for (const auto& [ev, k] : events) {
    if (matched_.count(ev) || k.ticker_b.empty()) continue;
    std::vector<EspnGame> candidates;
    for (const auto& [id, g] : games_) if (g.league == league_of[ev]->league) candidates.push_back(g);
    if (auto m = match_game(k, candidates)) {
      DLOG_INFO("espn: %s is %s (%s at %s)", ev.c_str(), m->id.c_str(), games_[m->id].away_abbr.c_str(),
                games_[m->id].home_abbr.c_str());
      matched_[ev] = *m;
    }
  }
  // Win probability for games in progress.
  for (const auto& [ev, m] : matched_) {
    if (!events.count(ev)) continue;
    auto it = games_.find(m.id);
    if (it == games_.end() || it->second.state != "in") { wp_.erase(m.id); continue; }
    if (now - wp_tried_[m.id] < options_.summary_ms) continue;
    wp_tried_[m.id] = now;
    const auto* lg = league_of[ev];
    json body;
    std::string why;
    if (!get("/apis/site/v2/sports/" + lg->sport + "/" + lg->league + "/summary?event=" + m.id, &body, &why, self)) {
      wp_.erase(m.id);
      continue;
    }
    wp_[m.id] = parse_summary_wp(body, it->second);
  }
  // The board: only from a scoreboard younger than keep_ms.
  auto board = std::make_shared<EspnBoard>();
  for (const auto& [ev, m] : matched_) {
    const auto e = events.find(ev);
    const auto g = games_.find(m.id);
    if (e == events.end() || g == games_.end() || now - g->second.fetched_ms > options_.keep_ms) continue;
    const auto w = wp_.find(m.id);
    const std::optional<EspnWp> wp = w == wp_.end() ? std::nullopt : w->second;
    const auto* lg = league_of[ev];
    (*board)[e->second.ticker_a] = espn_ref(*lg, g->second, m.a_home, wp);
    (*board)[e->second.ticker_b] = espn_ref(*lg, g->second, !m.a_home, wp);
  }
  std::lock_guard<std::mutex> lk(m_);
  board_ = std::move(board);
}

void EspnFeed::run(Worker& self) {
  while (!self.stopping()) {
    self.beat();
    step(now_ms(), &self);
    for (int i = 0; i < 50 && !self.stopping(); ++i) {   // a round every 5 s
      self.beat();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

}  // namespace deskd
