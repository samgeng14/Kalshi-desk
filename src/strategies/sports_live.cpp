#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/strategies.h"
#include "desk/team_names.h"
#include "desk/timeutil.h"

namespace desk {

// Kalshi game series -> ESPN league. sigma_game is the standard deviation of the final margin
// (points) used by the lead/time model; periods x minutes give the clock. Baseball and hockey
// use ESPN's win probability only (runs and goals are too lumpy for a Gaussian margin).
const std::map<std::string, SportsLive::League>& SportsLive::default_leagues() {
  static const std::map<std::string, League> L = {
      {"KXNFLGAME", {"football", "nfl", 13.5, 4, 15, {}, true}},
      {"KXNCAAFGAME", {"football", "college-football", 16.0, 4, 15, {"80", "81"}, true}},
      {"KXNBAGAME", {"basketball", "nba", 12.0, 4, 12, {}, true}},
      {"KXWNBAGAME", {"basketball", "wnba", 11.0, 4, 10, {}, true}},
      {"KXNCAAMBGAME", {"basketball", "mens-college-basketball", 11.0, 2, 20, {"50"}, true}},
      {"KXNCAAWBGAME", {"basketball", "womens-college-basketball", 12.0, 4, 10, {"50"}, true}},
      {"KXMLBGAME", {"baseball", "mlb", 0, 9, 0, {}, false}},
      {"KXNHLGAME", {"hockey", "nhl", 0, 3, 20, {}, false}},
  };
  return L;
}

SportsLive::SportsLive(const json& params, const std::string& user_agent) : http_("curl/8.5.0", 6) {
  (void)user_agent;  // ESPN's CDN rejects most other agents from datacenters; keep curl's.
  leagues_ = default_leagues();
  if (params.contains("leagues") && params["leagues"].is_object()) {
    for (auto& [series, v] : params["leagues"].items()) {
      League lg = leagues_.count(series) ? leagues_[series] : League{};
      lg.sport = v.value("sport", lg.sport);
      lg.league = v.value("league", lg.league);
      lg.sigma_game = v.value("sigma_game", lg.sigma_game);
      lg.periods = v.value("periods", lg.periods);
      lg.minutes_per_period = v.value("minutes_per_period", lg.minutes_per_period);
      lg.leadtime = v.value("leadtime", lg.leadtime);
      if (v.contains("groups") && v["groups"].is_array()) {
        lg.groups.clear();
        for (const auto& g : v["groups"]) lg.groups.push_back(g.get<std::string>());
      }
      leagues_[series] = lg;
    }
  }
  sb_refresh_seconds_ = params.value("scoreboard_refresh_seconds", 20);
  wp_refresh_seconds_ = params.value("wp_refresh_seconds", 30);
  log_seconds_ = params.value("log_seconds", 30);
  sigma_wp_ = params.value("sigma_wp", 0.04);
  sigma_leadtime_ = params.value("sigma_leadtime", 0.07);
  sigma_pregame_ = params.value("sigma_pregame", 0.10);
  // Off by default: the lead/time fallback is an unvalidated approximation and saying so is a
  // fact. Turning it on makes those rows tradable, it does not make the model better.
  allow_approximation_ = params.value("allow_approximation", false);
}

bool SportsLive::covers(const Market& m, const Series& s) const {
  if (s.category != "Sports") return false;
  return leagues_.count(m.series_ticker) > 0 && m.strike_type == "structured";
}

// ---- helpers --------------------------------------------------------------------------------

static std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}


static double token_overlap(const std::string& a, const std::string& b) {
  return team_name_overlap(a, b);   // include/desk/team_names.h: initials, city short forms, A's
}

// The team a market pays on: its YES subtitle ("Michigan"), else the "If <team> wins the" of its
// own rules. Per MARKET, so the name always belongs to the same ticker as its code.
std::string SportsLive::market_team(const Market& m) {
  if (!m.yes_sub_title.empty()) return m.yes_sub_title;
  const std::string& r = m.rules_primary;
  if (r.rfind("If ", 0) == 0) {
    const size_t p = r.find(" wins the ");
    if (p != std::string::npos && p > 3) return r.substr(3, p - 3);
  }
  return "";
}

double SportsLive::team_evidence(const std::string& code, const std::string& name, const std::string& abbr,
                                 const std::string& full_name) {
  return (!code.empty() && lower(code) == lower(abbr) ? 2.0 : 0.0) + token_overlap(name, full_name);
}

std::pair<double, double> SportsLive::orientation_scores(const std::string& code_a, const std::string& code_b,
                                                         const std::string& name_a, const std::string& name_b,
                                                         const std::string& away_abbr, const std::string& home_abbr,
                                                         const std::string& away_name, const std::string& home_name) {
  auto ab = [](const std::string& code, const std::string& abbr) {
    return !code.empty() && lower(code) == lower(abbr) ? 2.0 : 0.0;
  };
  const double s1 = ab(code_a, away_abbr) + ab(code_b, home_abbr) + token_overlap(name_a, away_name) + token_overlap(name_b, home_name);
  const double s2 = ab(code_a, home_abbr) + ab(code_b, away_abbr) + token_overlap(name_a, home_name) + token_overlap(name_b, away_name);
  return {s1, s2};
}

// "If Los Angeles D wins the Cincinnati vs Los Angeles D professional baseball game originally
// scheduled for ..." -> a = "Cincinnati", b = "Los Angeles D" (a is listed first: the away team
// on Kalshi's US-sports rules; we do not rely on that, ESPN decides home/away).
bool SportsLive::parse_us_teams(const std::string& rules, std::string& a, std::string& b) {
  size_t p = rules.find(" wins the ");
  if (p == std::string::npos) return false;
  size_t q = rules.find(" game ", p);
  if (q == std::string::npos) q = rules.find(" game,", p);
  if (q == std::string::npos) return false;
  std::string mid = rules.substr(p + 10, q - (p + 10));
  size_t v = mid.find(" vs ");
  if (v == std::string::npos) v = mid.find(" vs. ");
  if (v == std::string::npos) return false;
  a = mid.substr(0, v);
  b = mid.substr(v + (mid.compare(v, 5, " vs. ") == 0 ? 5 : 4));
  // Strip the sport words Kalshi appends to the second name.
  static const char* SUFFIX[] = {"professional", "pro", "college", "baseball", "basketball", "football", "hockey",
                                 "women's", "men's", "ncaa", "d3", "diii", "fcs"};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const char* w : SUFFIX) {
      std::string sfx = std::string(" ") + w;
      if (b.size() > sfx.size() && lower(b).compare(b.size() - sfx.size(), sfx.size(), sfx) == 0) {
        b = b.substr(0, b.size() - sfx.size());
        changed = true;
      }
    }
  }
  auto trim = [](std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  };
  trim(a);
  trim(b);
  return !a.empty() && !b.empty();
}

double SportsLive::spread_to_prob(double spread_home, double sigma_game) {
  if (sigma_game <= 0) return 0.5;
  return norm_cdf(spread_home / sigma_game);
}

// P(home wins) given the current lead, the pre-game expected margin and the fraction of the game
// left. Margin over the remaining game ~ N(mu*f, sigma^2*f). A tie at the end goes to overtime,
// treated as a coin flip (the normal's mass at exactly zero is negligible).
double SportsLive::leadtime_prob(double lead_home, double mu_home, double f, double sigma_game) {
  f = std::max(0.0, std::min(1.0, f));
  if (sigma_game <= 0) return 0.5;
  if (f < 0.002) return lead_home > 0 ? 1.0 : (lead_home < 0 ? 0.0 : 0.5);
  return norm_cdf((lead_home + mu_home * f) / (sigma_game * std::sqrt(f)));
}

// Fraction of regulation left from ESPN's period + displayClock ("12:34"). Overtime counts as
// a short remainder so the model still leans on the lead.
double SportsLive::frac_left(const League& lg, int period, const std::string& clock, const std::string& state) {
  if (state == "post") return 0.0;
  if (state != "in" || period <= 0) return 1.0;
  double total = lg.periods * lg.minutes_per_period;
  if (total <= 0) return 1.0;
  double mm = 0, ss = 0;
  if (std::sscanf(clock.c_str(), "%lf:%lf", &mm, &ss) < 1) mm = 0;
  double left_in_period = mm + ss / 60.0;
  if (period > lg.periods) return std::min(0.05, left_in_period / total);  // overtime
  double left = (lg.periods - period) * lg.minutes_per_period + left_in_period;
  return std::max(0.0, std::min(1.0, left / total));
}

// ---- ESPN ----------------------------------------------------------------------------------

static std::string us_date(const std::string& iso_utc) {  // ESPN UTC time -> US Eastern date
  long long t = parse_iso(iso_utc);
  if (t == 0) return "";
  return date_of(t, us_eastern_offset(t));
}

// Const JSON access that never asserts on a missing key (nlohmann's const operator[] is UB then).
static const json& jget(const json& j, const char* key) {
  static const json null_json;
  if (!j.is_object()) return null_json;
  auto it = j.find(key);
  return it == j.end() ? null_json : *it;
}
// jstr/jnum (types.h) already tolerate missing keys and numbers-as-strings.
static int jint(const json& j, const char* key, int def = 0) { return static_cast<int>(jnum(j, key, def)); }

void SportsLive::fetch_scoreboard(const std::string& series, const League& lg, const std::string& date, long long now) {
  std::string key = lg.league + "|" + date;
  if (now - sb_ts_[key] < sb_refresh_seconds_) return;
  sb_ts_[key] = now;
  std::vector<std::string> groups = lg.groups.empty() ? std::vector<std::string>{""} : lg.groups;
  std::string ymd = date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2);
  for (const auto& g : groups) {
    std::string url = "https://site.api.espn.com/apis/site/v2/sports/" + lg.sport + "/" + lg.league +
                      "/scoreboard?limit=300&dates=" + ymd + (g.empty() ? "" : "&groups=" + g);
    HttpResponse r;
    try {
      r = http_.get(url);
    } catch (const std::exception& e) {
      LOG_WARN("espn %s: %s", lg.league.c_str(), e.what());
      continue;
    }
    if (!r.ok()) { LOG_WARN("espn %s HTTP %ld", lg.league.c_str(), r.status); continue; }
    json sb = json::parse(r.body, nullptr, false);
    if (!sb.is_object()) continue;
    const json& events = jget(sb, "events");
    if (!events.is_array()) continue;
    for (const auto& e : events) {
      try {
        const json& comps = jget(e, "competitions");
        if (!comps.is_array() || comps.empty() || !comps[0].is_object()) continue;
        const json& c = comps[0];
        std::string id = jstr(e, "id");
        if (id.empty()) continue;
        GameState& g = games_[id];
        g.espn_id = id;
        g.league = lg.league;
        g.start_utc = jstr(e, "date");
        const json& status = jget(c, "status");
        const json& st = jget(status, "type");
        g.state = jstr(st, "state");
        g.detail = jstr(st, "shortDetail");
        g.period = jint(status, "period", 0);
        g.clock = jstr(status, "displayClock");
        g.last_play_id = jstr(jget(jget(c,"situation"),"lastPlay"),"id");
        const json& competitors = jget(c, "competitors");
        if (competitors.is_array())
          for (const auto& comp : competitors) {
            if (!comp.is_object()) continue;
            bool home = jstr(comp, "homeAway") == "home";
            const json& team = jget(comp, "team");
            std::string abbr = jstr(team, "abbreviation"), name = jstr(team, "displayName");
            int score = jint(comp, "score", 0);
            if (home) { g.home_abbr = abbr; g.home_name = name; g.home_score = score; }
            else { g.away_abbr = abbr; g.away_name = name; g.away_score = score; }
          }
        // Pre-game spread (DraftKings via ESPN): "ATL -14.5" with spread relative to the home team.
        const json& odds = jget(c, "odds");
        if (odds.is_array() && !odds.empty() && odds[0].is_object()) {
          const json& sp = jget(odds[0], "spread");
          if (sp.is_number()) {
            // ESPN's "spread" is the home team's line: negative when the home team is favored.
            g.spread_home = -sp.get<double>();
            g.have_spread = true;
          }
        }
        g.updated = now;  // this game was in a scoreboard fetched just now
      } catch (const std::exception& ex) {
        LOG_WARN("espn %s event: %s", lg.league.c_str(), ex.what());
      }
    }
  }
}

std::string SportsLive::state_signature(const GameState& g) {
  return g.state+"|"+std::to_string(g.period)+"|"+std::to_string(g.home_score)+"|"+
         std::to_string(g.away_score)+"|"+g.clock+"|"+g.last_play_id;
}

bool SportsLive::accept_win_probability(const json& j, GameState& g, long long now) {
  const auto& header=jget(j,"header");
  const auto& competitions=jget(header,"competitions");
  const auto& wp=jget(j,"winprobability");
  if (g.state!="in" || g.updated<=0 || now<g.updated || now-g.updated>120 ||
      jstr(header,"id")!=g.espn_id || !competitions.is_array() || competitions.empty() ||
      !wp.is_array() || wp.empty()) return false;
  const auto& comp=competitions[0];const auto& status=jget(comp,"status");
  if (jstr(jget(status,"type"),"state")!=g.state || jint(status,"period",-1)!=g.period ||
      jstr(status,"displayClock")!=g.clock) return false;
  int home=-1,away=-1;
  const auto& teams=jget(comp,"competitors");
  if (!teams.is_array()) return false;
  for(const auto& team:teams) {
    if(jstr(team,"homeAway")=="home")home=jint(team,"score",-1);
    else if(jstr(team,"homeAway")=="away")away=jint(team,"score",-1);
  }
  if(home!=g.home_score || away!=g.away_score) return false;
  const auto& point=wp.back();
  const std::string play=jstr(point,"playId");
  std::string latest=g.last_play_id;
  const auto& plays=jget(j,"plays");
  if(plays.is_array()&&!plays.empty()) {
    const auto id=jstr(plays.back(),"id");
    if(id.empty()||(!latest.empty()&&latest!=id)) return false;
    latest=id;
  }
  // A successful HTTP fetch cannot make an older chart point current. Require an identified
  // latest play plus the exact scoreboard state; ambiguous/missing play linkage stays unused.
  if(play.empty() || latest.empty() || play!=latest) return false;
  const auto& value=jget(point,"homeWinPercentage");const auto& tie=jget(point,"tiePercentage");
  if(!value.is_number()||!std::isfinite(value.get<double>())||value.get<double>()<0||value.get<double>()>1)return false;
  const double tie_p=tie.is_number()?tie.get<double>():0;
  if(!std::isfinite(tie_p)||tie_p<0||value.get<double>()+tie_p>1+1e-9)return false;
  g.wp_home=value.get<double>();g.tie=tie_p;g.wp_ts=now;g.last_sig=state_signature(g);
  return true;
}

void SportsLive::fetch_wp(const League& lg, GameState& g, long long now) {
  // Refresh the win probability when the game state changed (score, period or clock), and at
  // least every 4 refresh intervals regardless; never more often than wp_refresh_seconds_.
  std::string sig = state_signature(g);
  if (g.wp_ts > 0 && sig == g.last_sig && now - g.wp_ts < 4 * wp_refresh_seconds_) return;
  if (now - g.wp_attempt_ts < wp_refresh_seconds_) return;
  g.wp_attempt_ts = now;  // pacing only; wp_ts below is the age of the number itself
  std::string url = "https://site.api.espn.com/apis/site/v2/sports/" + lg.sport + "/" + lg.league + "/summary?event=" + g.espn_id;
  try {
    HttpResponse r = http_.get(url);
    if (!r.ok()) { LOG_WARN("espn summary %s HTTP %ld", g.espn_id.c_str(), r.status); return; }
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return;
    // The pre-game line survives in the summary's pickcenter after kickoff (the scoreboard drops it).
    const json& pc = jget(j, "pickcenter");
    if (!g.have_spread && pc.is_array() && !pc.empty() && pc[0].is_object()) {
      const json& sp = jget(pc[0], "spread");
      if (sp.is_number()) {
        g.spread_home = -sp.get<double>();
        g.have_spread = true;
      }
    }
    if(!accept_win_probability(j,g,now)) {g.wp_home=-1;g.wp_ts=0;g.tie=0;}
  } catch (const std::exception& e) {
    LOG_WARN("espn summary %s: %s", g.espn_id.c_str(), e.what());
  }
}

// ---- prepare: map Kalshi events to ESPN games, refresh live states, log -------------------------

void SportsLive::prepare(const std::vector<Market>& markets, Store& store) {
  long long now = now_s();
  std::string today = date_of(now, us_eastern_offset(now));
  // 1. Kalshi events from the watched markets.
  std::map<std::string, std::vector<const Market*>> by_event;
  for (const auto& m : markets)
    if (leagues_.count(m.series_ticker) && m.strike_type == "structured") by_event[m.event_ticker].push_back(&m);
  std::set<std::string> want;  // league|date to fetch
  for (auto& [ev, ms] : by_event) {
    EventInfo& info = events_[ev];
    if (info.event_ticker.empty()) {
      info.event_ticker = ev;
      info.series = ms.front()->series_ticker;
      info.date = event_date(ev);
      info.start_min = event_start_minutes(ev);
      // Each ticker's name comes from its own market. It used to come from the rules text in the
      // game's order ("Oklahoma vs Michigan") while the codes came in ticker order (MICH, OKLA),
      // so name_a could be the OTHER ticker's team. When the ticker codes did not match ESPN's
      // abbreviations exactly (OKLA vs OU) the two orientations then scored the same, and the tie
      // silently made the first ticker the away team. On 2026-09-06..12 that swapped home and away
      // in 10 college football games: ESPN's number was applied to the wrong team, and every
      // "edge" it produced there was the size of the game.
      for (const Market* m : ms) {
        std::string code = m->ticker.substr(m->ticker.find_last_of('-') + 1);
        if (info.ticker_a.empty()) { info.ticker_a = m->ticker; info.code_a = code; info.name_a = market_team(*m); }
        else if (info.ticker_b.empty() && m->ticker != info.ticker_a) {
          info.ticker_b = m->ticker; info.code_b = code; info.name_b = market_team(*m);
        }
      }
    }
    if (info.date.empty()) continue;
    // Only games within a day of now are worth an ESPN round trip.
    long long t_ev = parse_iso(info.date + "T12:00:00Z");
    if (std::llabs(t_ev - now) > 36 * 3600) continue;
    want.insert(info.series + "|" + info.date);
  }
  // 2. Scoreboards.
  for (const auto& w : want) {
    std::string series = w.substr(0, w.find('|')), date = w.substr(w.find('|') + 1);
    fetch_scoreboard(series, leagues_.at(series), date, now);
  }
  // 3. Match events to games: team abbreviations from the tickers, then names.
  for (auto& [ev, info] : events_) {
    if (!info.espn_id.empty() || info.date.empty()) continue;
    const League& lg = leagues_.at(info.series);
    // Only once the event's own date has been fetched: before that the only candidates are the same
    // teams' games on the neighbouring day, and a match, once made, is never revisited.
    if (!sb_ts_.count(lg.league + "|" + info.date)) continue;
    double best = 0, second = 0;
    std::string best_id;
    bool best_home = false;
    long long t_info = parse_iso(info.date + "T12:00:00Z");
    for (const auto& [id, g] : games_) {
      if (g.home_abbr.empty() || g.league != lg.league) continue;
      double closeness = 0;   // 1 at the ticker's exact start, 0 at 90 minutes off
      // A ticker that carries its start time (MLB) must meet a game that starts then: the two games
      // of a doubleheader have the same teams and date and differ only in the time.
      if (info.start_min >= 0) {
        const long long t = parse_iso(g.start_utc);
        if (t <= 0) continue;
        const int off = us_eastern_offset(t);
        if (date_of(t, off) != info.date) continue;
        const int m = static_cast<int>((((t + off) % 86400) + 86400) % 86400 / 60);
        if (std::abs(m - info.start_min) > 90) continue;
        closeness = 1.0 - std::abs(m - info.start_min) / 90.0;
      }
      // Kalshi's event date and ESPN's kickoff can straddle a date line (late West-coast games,
      // UTC vs Eastern); accept a day either side but let the exact date win ties.
      std::string gd = us_date(g.start_utc);
      if (gd.empty()) continue;
      long long dd = std::llabs(parse_iso(gd + "T12:00:00Z") - t_info);
      if (dd > 24 * 3600) continue;
      double score = 0;
      const auto [s1, s2] = orientation_scores(info.code_a, info.code_b, info.name_a, info.name_b, g.away_abbr,
                                               g.home_abbr, g.away_name, g.home_name);
      // Both teams, in the orientation that wins, need their own evidence (see team_evidence).
      {
        const bool a_home = s2 > s1;
        const double ea = team_evidence(info.code_a, info.name_a, a_home ? g.home_abbr : g.away_abbr,
                                        a_home ? g.home_name : g.away_name);
        const double eb = team_evidence(info.code_b, info.name_b, a_home ? g.away_abbr : g.home_abbr,
                                        a_home ? g.away_name : g.home_name);
        if (std::min(ea, eb) < 0.75) continue;
      }
      bool a_is_home = s2 > s1;
      score = std::max(s1, s2) + (gd == info.date ? 0.5 : 0.0) + closeness;
      // An orientation that barely beats its mirror image is a guess. A wrong side is far worse
      // than no price (it trades the whole game the wrong way), so an ambiguous match is not taken.
      if (std::fabs(s1 - s2) < 1.0) {
        if (score >= 2.0 && ambiguous_logged_.insert(ev + "|" + id).second)
          LOG_WARN("sports_live %s: espn %s (%s vs %s) matches both ways round (%.1f vs %.1f); not priced", ev.c_str(),
                   id.c_str(), g.away_abbr.c_str(), g.home_abbr.c_str(), s1, s2);
        continue;
      }
      if (score < 2.0) continue;
      if (score > best) { second = best; best = score; best_id = id; best_home = a_is_home; }
      else if (score > second) second = score;
    }
    // Two candidates this close are two games we cannot tell apart (the same teams again): no price
    // rather than whichever the map happened to list first.
    if (!best_id.empty() && second >= best - 0.25) {
      if (ambiguous_logged_.insert(ev + "|tie").second)
        LOG_WARN("sports_live %s: two ESPN games match equally well (%.1f, %.1f); not priced", ev.c_str(), best, second);
      best_id.clear();
    }
    if (!best_id.empty()) { info.espn_id = best_id; info.home_is_a = best_home; }
    if (!info.espn_id.empty())
      LOG_INFO("sports_live %s -> espn %s (%s vs %s, score %.1f)", ev.c_str(), info.espn_id.c_str(),
               games_[info.espn_id].away_abbr.c_str(), games_[info.espn_id].home_abbr.c_str(), best);
  }
  // 4. Live games: win probability, pre-game margin, logging.
  for (auto& [ev, info] : events_) {
    if (info.espn_id.empty() || !games_.count(info.espn_id)) continue;
    const League& lg = leagues_.at(info.series);
    GameState& g = games_[info.espn_id];
    auto mit = by_event.find(ev);
    if (mit == by_event.end()) continue;
    // Pre-game expected margin: the spread when ESPN has it, else the Kalshi pre-game price.
    if (g.have_spread) { info.mu_home = g.spread_home; info.mu_from_market = false; }
    else if (g.state == "pre" && !info.mu_from_market) {
      const Market* home_m = nullptr;
      for (const Market* m : mit->second)
        if (m->ticker == (info.home_is_a ? info.ticker_a : info.ticker_b)) home_m = m;
      if (home_m && home_m->yes_bid > 0 && home_m->yes_ask < 1 && lg.sigma_game > 0) {
        double p = home_m->mid();
        p = std::max(0.02, std::min(0.98, p));
        // Phi^-1 via bisection on norm_cdf (no inverse in mathutil).
        double lo = -4, hi = 4;
        for (int i = 0; i < 40; ++i) { double mid = 0.5 * (lo + hi); (norm_cdf(mid) < p ? lo : hi) = mid; }
        info.mu_home = 0.5 * (lo + hi) * lg.sigma_game;
        info.mu_from_market = true;
      }
    }
    if (g.state == "in") fetch_wp(lg, g, now);
    if (g.state == "in" && now - last_log_[ev] >= log_seconds_) {
      last_log_[ev] = now;
      double mkt_home = 0, mkt_away = 0;
      for (const Market* m : mit->second) {
        bool is_home = m->ticker == (info.home_is_a ? info.ticker_a : info.ticker_b);
        (is_home ? mkt_home : mkt_away) = m->mid();
      }
      double f = frac_left(lg, g.period, g.clock, g.state);
      double lt = lg.leadtime ? leadtime_prob(g.home_score - g.away_score, info.mu_home, f, lg.sigma_game) : -1;
      double current_wp = g.last_sig == state_signature(g) && now >= g.wp_ts &&
                          now - g.wp_ts < 4 * wp_refresh_seconds_ ? g.wp_home : -1;
      store.exec(
          "INSERT INTO ext_wp(ts,event_ticker,espn_id,league,state,period,clock,home_score,away_score,wp_home,wp_leadtime,"
          "mu_home,mkt_home,mkt_away,home_ticker,away_ticker) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {now, ev, g.espn_id, lg.league, g.state, static_cast<long long>(g.period), g.clock,
           static_cast<long long>(g.home_score), static_cast<long long>(g.away_score), current_wp, lt, info.mu_home,
           mkt_home, mkt_away, info.home_is_a ? info.ticker_a : info.ticker_b, info.home_is_a ? info.ticker_b : info.ticker_a});
    }
  }
}

std::optional<Fair> SportsLive::fair(const Market& m, const Series&, Store&) {
  auto eit = events_.find(m.event_ticker);
  if (eit == events_.end() || eit->second.espn_id.empty()) return std::nullopt;
  const EventInfo& info = eit->second;
  auto git = games_.find(info.espn_id);
  if (git == games_.end()) return std::nullopt;
  const GameState& g = git->second;
  const League& lg = leagues_.at(info.series);
  bool is_home = m.ticker == (info.home_is_a ? info.ticker_a : info.ticker_b);
  if (m.ticker != info.ticker_a && m.ticker != info.ticker_b) return std::nullopt;  // a third market (tie/other): skip
  long long now = now_s();
  if (now - g.updated > 120) return std::nullopt;  // stale scoreboard: do not price
  double p_home, sigma;
  std::string src;
  Fair f;
  if (g.state == "post") return std::nullopt;
  if (g.state == "in" && g.wp_home >= 0 && g.last_sig==state_signature(g) && now>=g.wp_ts && now - g.wp_ts < 4 * wp_refresh_seconds_) {
    p_home = g.wp_home;
    double fl = frac_left(lg, g.period, g.clock, g.state);
    sigma = std::max(0.02, sigma_wp_ * (0.5 + fl));  // tighter as the game runs down
    src = "espn wp";
  } else if (g.state == "in" && lg.leadtime) {
    double fl = frac_left(lg, g.period, g.clock, g.state);
    p_home = leadtime_prob(g.home_score - g.away_score, info.mu_home, fl, lg.sigma_game);
    sigma = std::max(0.03, sigma_leadtime_ * (0.5 + fl));
    src = "lead/time";
  } else if (g.state == "pre" && g.have_spread && lg.sigma_game > 0) {
    // Pre-game: the sportsbook line is an independent source; the Kalshi-derived prior is not
    // (it would only score the market against itself), so without a line there is no fair.
    p_home = spread_to_prob(g.spread_home, lg.sigma_game);
    sigma = sigma_pregame_;
    src = "spread";
  } else {
    return std::nullopt;
  }
  double tie = src == "espn wp" ? g.tie : 0.0;
  double p = is_home ? p_home : std::max(0.0, 1.0 - p_home - tie);
  f.p = std::max(0.005, std::min(0.995, p));
  f.sigma = sigma;
  char buf[220];
  std::snprintf(buf, sizeof buf, "%s %s-%s %d-%d %s %s | %s p_home %.3f mu %+.1f", lg.league.c_str(), g.away_abbr.c_str(),
                g.home_abbr.c_str(), g.away_score, g.home_score, g.state == "in" ? g.detail.c_str() : g.state.c_str(),
                is_home ? "(home)" : "(away)", src.c_str(), p_home, info.mu_home);
  f.note = buf;
  f.cluster = m.event_ticker;
  f.inputs={{"valid",true},{"source",src},{"source_ts",src=="espn wp"?g.wp_ts:g.updated},
            {"state_signature",state_signature(g)},{"auto_eligible",src=="espn wp"}};
  // When ESPN publishes a win probability the model uses it; otherwise it falls back to its own
  // lead-and-time Gaussian, which has never been validated against outcomes. That fallback was
  // unconditionally research-only, which is why a board full of game rows could not be traded.
  // It is now a setting, because "this estimate is unvalidated" and "you may not look at it" are
  // different statements and only the first one is a fact.
  if(src!="espn wp" && !allow_approximation_) {
    f.research_only=true;
    f.research_reason="Unvalidated sports approximation; manual research only";
  } else if(src!="espn wp") {
    f.note += " | APPROXIMATION: lead/time model, no ESPN win probability, never validated against outcomes";
  }
  return f;
}

std::string SportsLive::describe() const {
  int live = 0, matched = 0;
  for (const auto& [ev, info] : events_) if (!info.espn_id.empty()) ++matched;
  for (const auto& [id, g] : games_) if (g.state == "in") ++live;
  char buf[160];
  std::snprintf(buf, sizeof buf, "ESPN win probability / lead-time model; %zu events, %d matched, %d live", events_.size(),
                matched, live);
  return buf;
}

}  // namespace desk
