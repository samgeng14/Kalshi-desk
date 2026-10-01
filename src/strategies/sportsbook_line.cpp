// Pregame US team sports against the de-vigged sportsbook moneyline (M5). See strategies.h.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "desk/log.h"
#include "desk/strategies.h"
#include "desk/team_names.h"
#include "desk/timeutil.h"

namespace desk {

namespace {

double overlap(const std::string& a, const std::string& b) { return team_name_overlap(a, b); }

std::string expand_home_path(const std::string& p) {
  if (p.rfind("~/", 0) == 0) {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) + p.substr(1) : p;
  }
  return p;
}

std::string read_key(const json& params) {
  std::string key;
  const std::string path = params.value("api_key_path", std::string());
  if (!path.empty()) {
    std::ifstream f(expand_home_path(path));
    if (f) std::getline(f, key);
  }
  const std::string env = params.value("api_key_env", std::string("ODDS_API_KEY"));
  if (key.empty() && !env.empty())
    if (const char* v = std::getenv(env.c_str())) key = v;
  while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
  return key;
}

}  // namespace

SportsbookLine::SportsbookLine(const json& params, const std::string& user_agent) : http_(user_agent, 15) {
  key_ = read_key(params);
  base_url_ = params.value("base_url", std::string("https://api.the-odds-api.com"));
  regions_ = params.value("regions", std::string("us,eu"));
  refresh_seconds_ = std::max(60, static_cast<int>(params.value("refresh_minutes", 60.0) * 60));
  min_books_ = std::max(1, params.value("min_books", 2));
  sigma_ = params.value("sigma", 0.03);
  nfl_tie_ = params.value("nfl_tie", 0.003);
  sharp_ = {"pinnacle", "circasports", "betonlineag", "lowvig"};
  if (params.contains("sharp_books") && params["sharp_books"].is_array()) {
    sharp_.clear();
    for (const auto& b : params["sharp_books"]) if (b.is_string()) sharp_.push_back(b.get<std::string>());
  }
  leagues_ = {{"KXNFLGAME", "americanfootball_nfl"}, {"KXNCAAFGAME", "americanfootball_ncaaf"},
              {"KXMLBGAME", "baseball_mlb"},         {"KXNBAGAME", "basketball_nba"},
              {"KXWNBAGAME", "basketball_wnba"},     {"KXNHLGAME", "icehockey_nhl"},
              {"KXNCAAMBGAME", "basketball_ncaab"}};
  if (params.contains("leagues") && params["leagues"].is_object()) {
    leagues_.clear();
    for (auto it = params["leagues"].begin(); it != params["leagues"].end(); ++it)
      if (it.value().is_string()) leagues_[it.key()] = it.value().get<std::string>();
  }
}

bool SportsbookLine::covers(const Market& m, const Series& s) const {
  return s.category == "Sports" && leagues_.count(m.series_ticker) && m.strike_type == "structured";
}

std::string SportsbookLine::describe() const {
  return key_.empty() ? "sportsbook moneyline, de-vigged: NO API KEY (params.sportsbook_line.api_key_path)"
                      : "sportsbook moneyline, de-vigged; sharp book first, else the median of " +
                            std::to_string(min_books_) + "+ books";
}

std::string SportsbookLine::redact(const std::string& text, const std::string& secret) {
  if (secret.empty()) return text;
  std::string out = text;
  for (size_t p = out.find(secret); p != std::string::npos; p = out.find(secret, p + 3)) out.replace(p, secret.size(), "***");
  return out;
}

std::vector<double> SportsbookLine::devig(const std::vector<double>& odds) {
  std::vector<double> p;
  double sum = 0;
  for (double d : odds) {
    if (!(d > 1.0)) return {};
    p.push_back(1.0 / d);
    sum += 1.0 / d;
  }
  if (sum <= 0) return {};
  for (auto& x : p) x /= sum;
  return p;
}

std::optional<SportsbookLine::Line> SportsbookLine::consensus(const json& ev, const std::vector<std::string>& sharp,
                                                              int min_books, long long fetched_ts) {
  Line L;
  L.id = ev.value("id", std::string());
  L.home = ev.value("home_team", std::string());
  L.away = ev.value("away_team", std::string());
  L.commence_ts = parse_iso(ev.value("commence_time", std::string()));
  L.fetched_ts = fetched_ts;
  if (L.home.empty() || L.away.empty() || L.commence_ts <= 0) return std::nullopt;
  std::map<std::string, std::pair<double, double>> by_book;   // book -> (p_home, p_away)
  for (const auto& b : ev.value("bookmakers", json::array())) {
    for (const auto& mk : b.value("markets", json::array())) {
      if (mk.value("key", std::string()) != "h2h") continue;
      double dh = 0, da = 0;
      int n = 0;
      for (const auto& o : mk.value("outcomes", json::array())) {
        ++n;
        const std::string name = o.value("name", std::string());
        const double price = o.value("price", 0.0);
        if (name == L.home) dh = price;
        else if (name == L.away) da = price;
      }
      // Two-way only: a book quoting a draw is pricing a different contract.
      if (n != 2 || dh <= 1 || da <= 1) continue;
      auto p = devig({dh, da});
      if (p.size() == 2) by_book[b.value("key", std::string())] = {p[0], p[1]};
    }
  }
  for (const auto& s : sharp) {
    auto it = by_book.find(s);
    if (it == by_book.end()) continue;
    L.p_home = it->second.first;
    L.p_away = it->second.second;
    L.source = s;
    L.books = static_cast<int>(by_book.size());
    return L;
  }
  if (static_cast<int>(by_book.size()) < min_books) return std::nullopt;
  std::vector<double> ph;
  for (const auto& [k, v] : by_book) ph.push_back(v.first);
  std::sort(ph.begin(), ph.end());
  const size_t n = ph.size();
  L.p_home = n % 2 ? ph[n / 2] : 0.5 * (ph[n / 2 - 1] + ph[n / 2]);
  L.p_away = 1.0 - L.p_home;
  L.books = static_cast<int>(n);
  L.source = "median of " + std::to_string(n) + " books";
  return L;
}

void SportsbookLine::fetch(const std::string& sport, long long now) {
  fetched_ts_[sport] = now;   // a failure waits a full interval too: the quota is the scarce thing
  const std::string url = base_url_ + "/v4/sports/" + sport + "/odds?apiKey=" + url_encode(key_) +
                          "&regions=" + url_encode(regions_) + "&markets=h2h&oddsFormat=decimal&dateFormat=iso";
  HttpResponse r;
  try {
    r = http_.get(url);
  } catch (const std::exception& e) {
    LOG_WARN("sportsbook_line %s: %s", sport.c_str(), redact(e.what(), key_).c_str());
    return;
  }
  if (!r.ok()) {
    LOG_WARN("sportsbook_line %s: HTTP %ld %s", sport.c_str(), r.status, redact(r.body.substr(0, 200), key_).c_str());
    return;
  }
  json j = json::parse(r.body, nullptr, false);
  if (!j.is_array()) {
    LOG_WARN("sportsbook_line %s: response is not a list of events", sport.c_str());
    return;
  }
  std::vector<Line> lines;
  int malformed = 0;
  for (const auto& ev : j) {
    // One event with an unexpected shape (a price that is a string, a missing object) is skipped,
    // not allowed to throw away the whole league's refresh.
    try {
      if (auto L = consensus(ev, sharp_, min_books_, now)) lines.push_back(*L);
    } catch (const std::exception&) {
      ++malformed;
    }
  }
  if (malformed) LOG_WARN("sportsbook_line %s: %d malformed event(s) skipped", sport.c_str(), malformed);
  lines_[sport] = std::move(lines);
  LOG_INFO("sportsbook_line %s: %zu games priced from %zu listed", sport.c_str(), lines_[sport].size(), j.size());
}

void SportsbookLine::prepare(const std::vector<Market>& markets, Store&) {
  if (key_.empty()) {
    if (!said_no_key_) LOG_WARN("sportsbook_line: no API key (params.sportsbook_line.api_key_path or $ODDS_API_KEY); inert");
    said_no_key_ = true;
    return;
  }
  const long long now = now_s();
  const std::string today = date_of(now, us_eastern_offset(now));
  const long long t_today = parse_iso(today + "T12:00:00Z");
  std::set<std::string> want;
  for (const auto& m : markets) {
    auto it = leagues_.find(m.series_ticker);
    if (it == leagues_.end() || m.strike_type != "structured") continue;
    const std::string d = event_date(m.event_ticker);
    if (d.empty()) continue;
    const long long t = parse_iso(d + "T12:00:00Z");
    if (t < t_today - 86400 || t > t_today + 2 * 86400) continue;   // games in the next day or so
    want.insert(it->second);
  }
  for (const auto& sport : want)
    if (now - fetched_ts_[sport] >= refresh_seconds_) fetch(sport, now);
}

const SportsbookLine::Line* SportsbookLine::match(const Market& m, const std::string& sport, bool* is_home) const {
  auto lit = lines_.find(sport);
  if (lit == lines_.end()) return nullptr;
  std::string a, b;
  if (!SportsLive::parse_us_teams(m.rules_primary, a, b)) return nullptr;
  const std::string team = SportsLive::market_team(m);
  if (team.empty()) return nullptr;
  const std::string d = event_date(m.event_ticker);
  const long long t_kalshi = d.empty() ? 0 : parse_iso(d + "T12:00:00Z");
  const int start_min = event_start_minutes(m.event_ticker);
  const Line* best = nullptr;
  double best_score = 0, second = 0;
  for (const auto& L : lit->second) {
    double closeness = 0;   // 1 at the ticker's exact start, 0 at 90 minutes off
    const int off = us_eastern_offset(L.commence_ts);
    const std::string gd = date_of(L.commence_ts, off);
    if (t_kalshi > 0 && std::llabs(parse_iso(gd + "T12:00:00Z") - t_kalshi) > 86400) continue;
    // A ticker with a start time (MLB) must meet a game starting then: doubleheaders and series
    // games between the same teams differ only in time or date.
    if (start_min >= 0) {
      if (gd != d) continue;
      const int m_start = static_cast<int>((((L.commence_ts + off) % 86400) + 86400) % 86400 / 60);
      if (std::abs(m_start - start_min) > 90) continue;
      closeness = 1.0 - std::abs(m_start - start_min) / 90.0;
    }
    // Both teams, both ways round; the orientation must win clearly (the Oklahoma/Michigan lesson).
    const double s1 = overlap(a, L.away) + overlap(b, L.home), s2 = overlap(a, L.home) + overlap(b, L.away);
    const double score = std::max(s1, s2);
    if (score < 1.5 || std::fabs(s1 - s2) < 1.0) continue;
    // Each team on its own, not just the sum: one team's full name must not carry a game.
    const bool a_home = s2 > s1;
    if (std::min(overlap(a, a_home ? L.home : L.away), overlap(b, a_home ? L.away : L.home)) < 0.75) continue;
    const double s = score + (gd == d ? 0.5 : 0.0) + closeness;   // exact date, then nearest start
    if (s > best_score) { second = best_score; best_score = s; best = &L; }
    else if (s > second) second = s;
  }
  if (!best) return nullptr;
  if (second >= best_score - 0.25) return nullptr;   // two games we cannot tell apart: no price
  const double h = overlap(team, best->home), w = overlap(team, best->away);
  if (std::fabs(h - w) < 0.5) return nullptr;   // this market's team is not clearly one side
  *is_home = h > w;
  return best;
}

std::optional<Fair> SportsbookLine::fair(const Market& m, const Series&, Store&) {
  if (key_.empty()) return std::nullopt;
  auto sit = leagues_.find(m.series_ticker);
  if (sit == leagues_.end()) return std::nullopt;
  bool is_home = false;
  const Line* L = match(m, sit->second, &is_home);
  if (!L) return std::nullopt;
  const long long now = now_s();
  if (now >= L->commence_ts - 60) return std::nullopt;                  // pregame only
  if (now - L->fetched_ts > 2LL * refresh_seconds_) return std::nullopt;   // a stale line is not a price
  double p = is_home ? L->p_home : L->p_away;
  // A two-way moneyline excludes the tie; Kalshi's "<team> wins" pays NO on both teams in one.
  if (sit->second == "americanfootball_nfl") p *= (1.0 - nfl_tie_);
  Fair f;
  f.p = std::max(0.01, std::min(0.99, p));
  f.sigma = sigma_;
  f.cluster = m.event_ticker;
  char buf[200];
  std::snprintf(buf, sizeof buf, "line %s: %s %.3f (%s) | kickoff in %.1fh", L->source.c_str(),
                is_home ? L->home.c_str() : L->away.c_str(), p, is_home ? "home" : "away",
                (L->commence_ts - now) / 3600.0);
  f.note = buf;
  f.inputs = {{"valid", true}, {"source", L->source}, {"books", L->books}, {"source_ts", L->fetched_ts},
              {"auto_eligible", true}};
  return f;
}

}  // namespace desk
