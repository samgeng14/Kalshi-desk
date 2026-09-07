#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"

namespace desk {

// Kalshi game series -> ESPN league slug. Override/extend via params.sports_inplay.leagues.
static const std::pair<const char*, const char*> DEFAULT_LEAGUES[] = {
    {"KXEPLGAME", "eng.1"},          {"KXLALIGAGAME", "esp.1"},        {"KXSERIEAGAME", "ita.1"},
    {"KXBUNDESLIGAGAME", "ger.1"},   {"KXLIGUE1GAME", "fra.1"},        {"KXUCLGAME", "uefa.champions"},
    {"KXUELGAME", "uefa.europa"},    {"KXMLSGAME", "usa.1"},           {"KXEFLCHAMPIONSHIPGAME", "eng.2"},
    {"KXEREDIVISIEGAME", "ned.1"},   {"KXLIGAPORTUGALGAME", "por.1"},  {"KXBRASILEIROGAME", "bra.1"},
    {"KXLIGAMXGAME", "mex.1"},       {"KXSCOTTISHPREMGAME", "sco.1"},  {"KXBELGIANPLGAME", "bel.1"},
    {"KXSUPERLIGGAME", "tur.1"},     {"KXARGPREMDIVGAME", "arg.1"},    {"KXBUNDESLIGA2GAME", "ger.2"},
    {"KXLALIGA2GAME", "esp.2"},      {"KXFACUPGAME", "eng.fa"},
};

// ESPN's edge rejects unfamiliar User-Agents from some networks but accepts curl's; the
// strategy therefore identifies as curl unless params.sports_inplay.user_agent says otherwise.
const std::map<std::string, std::string>& SportsInplay::default_leagues() {
  static const std::map<std::string, std::string> m = [] {
    std::map<std::string, std::string> r;
    for (const auto& [k, v] : DEFAULT_LEAGUES) r[k] = v;
    return r;
  }();
  return m;
}

SportsInplay::SportsInplay(const json& params, const std::string& user_agent)
    : http_(params.value("user_agent", std::string("curl/8.5.0")), 6) {
  (void)user_agent;
  for (const auto& [k, v] : DEFAULT_LEAGUES) leagues_[k] = v;
  if (params.contains("leagues") && params["leagues"].is_object())
    for (auto& [k, v] : params["leagues"].items()) leagues_[k] = v.get<std::string>();
  lambda_home_ = params.value("lambda_home", 1.45);
  lambda_away_ = params.value("lambda_away", 1.15);
  red_self_ = params.value("red_self", 0.65);
  red_opp_ = params.value("red_opp", 1.25);
  late_boost_ = params.value("late_boost", 0.2);
  t_eff_ = params.value("t_eff", 95.0);
  rho_ = params.value("rho", -0.13);  // Dixon-Coles low-score correction used in the pre-match fit
  refresh_seconds_ = params.value("refresh_seconds", 15);
  log_seconds_ = params.value("log_seconds", 30);
}

// "If Tie is the result of the Nottingham Forest vs Coventry professional EPL soccer game ..."
// "If Coventry wins the Nottingham Forest vs Coventry professional EPL soccer game ..."
bool SportsInplay::parse_teams(const std::string& rules, std::string& home, std::string& away) {
  size_t vs = rules.find(" vs ");
  if (vs == std::string::npos) return false;
  size_t start = rules.rfind(" the ", vs);
  if (start == std::string::npos) return false;
  start += 5;
  size_t end = rules.find(" professional", vs);
  if (end == std::string::npos) end = rules.find(" game", vs);
  if (end == std::string::npos || end <= vs) return false;
  home = rules.substr(start, vs - start);
  away = rules.substr(vs + 4, end - (vs + 4));
  auto trim = [](std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  };
  trim(home);
  trim(away);
  return !home.empty() && !away.empty();
}

static std::set<std::string> tokens(const std::string& s) {
  static const std::set<std::string> stop = {"fc", "afc", "cf", "sc", "sk", "ac", "club", "de", "the", "and", "&"};
  std::set<std::string> out;
  std::string cur;
  for (char ch : s + " ") {
    if (std::isalnum(static_cast<unsigned char>(ch))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    else if (!cur.empty()) {
      if (!stop.count(cur)) out.insert(cur);
      cur.clear();
    }
  }
  return out;
}

// 1.0 = every token of the shorter name appears in the longer one.
double SportsInplay::name_match(const std::string& a, const std::string& b) {
  auto ta = tokens(a), tb = tokens(b);
  if (ta.empty() || tb.empty()) return 0;
  size_t common = 0;
  for (const auto& t : ta) common += tb.count(t);
  return static_cast<double>(common) / std::min(ta.size(), tb.size());
}

// "67'" -> 67, "90'+7'" -> 97, "45'+2'" in period 1 -> 47; halftime -> 45.
double SportsInplay::parse_clock(const std::string& clock, int period, const std::string& status_name) {
  if (status_name.find("HALFTIME") != std::string::npos) return 45;
  if (status_name.find("FULL_TIME") != std::string::npos || status_name.find("FINAL") != std::string::npos) return 999;
  double base = 0, extra = 0;
  std::string s = clock;
  size_t plus = s.find('+');
  std::string first = plus == std::string::npos ? s : s.substr(0, plus);
  base = std::atof(first.c_str());
  if (plus != std::string::npos) extra = std::atof(s.c_str() + plus + 1);
  if (base <= 0 && period >= 2) base = 45;
  return base + extra;
}

// Which outcome a market is: "tie", "home", "away" or "". Titles changed format over time
// ("Coventry wins" vs "Leeds United vs Brentford Winner?"), so the rules text decides:
// "If Tie is the result of ..." / "If <team> wins the ...".
std::string SportsInplay::market_side(const Market& m, const std::string& home, const std::string& away) {
  const std::string& r = m.rules_primary;
  if (m.yes_sub_title == "Tie" || r.rfind("If Tie is the result", 0) == 0 ||
      (m.ticker.size() > 4 && m.ticker.compare(m.ticker.size() - 4, 4, "-TIE") == 0))
    return "tie";
  if (r.rfind("If ", 0) == 0) {
    size_t w = r.find(" wins the ");
    if (w != std::string::npos) {
      std::string team = r.substr(3, w - 3);
      if (!home.empty() && team == home) return "home";
      if (!away.empty() && team == away) return "away";
      if (!home.empty() && name_match(team, home) >= 0.99) return "home";
      if (!away.empty() && name_match(team, away) >= 0.99) return "away";
    }
  }
  if (!home.empty() && m.title.find(home + " wins") == 0) return "home";
  if (!away.empty() && m.title.find(away + " wins") == 0) return "away";
  if (m.yes_sub_title == home && !home.empty()) return "home";
  if (m.yes_sub_title == away && !away.empty()) return "away";
  return "";
}

bool SportsInplay::covers(const Market& m, const Series& s) const {
  if (s.category != "Sports") return false;
  if (!leagues_.count(m.series_ticker)) return false;
  bool tie_ticker = m.ticker.size() > 4 && m.ticker.compare(m.ticker.size() - 4, 4, "-TIE") == 0;
  return m.yes_sub_title == "Tie" || m.title.find(" wins") != std::string::npos || tie_ticker ||
         m.rules_primary.find(" wins the ") != std::string::npos;
}

void SportsInplay::fetch_league(const std::string& slug, long long now) {
  if (now - espn_ts_[slug] < refresh_seconds_) return;
  espn_ts_[slug] = now;
  try {
    HttpResponse r = http_.get("https://site.api.espn.com/apis/site/v2/sports/soccer/" + slug + "/scoreboard");
    if (!r.ok()) throw std::runtime_error("espn HTTP " + std::to_string(r.status));
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) throw std::runtime_error("espn: not a JSON object");
    espn_cache_[slug] = std::move(j);
    espn_data_ts_[slug] = now;  // age of the data itself; a failed refresh keeps the old stamp
  } catch (const std::exception& e) {
    LOG_WARN("espn %s: %s", slug.c_str(), e.what());
    espn_ts_[slug] = now - refresh_seconds_ + std::min(20, refresh_seconds_);  // retry soon, not next cycle
  }
}

void SportsInplay::prepare(const std::vector<Market>& markets, Store& store) {
  long long now = now_s();
  std::string today = now_iso().substr(0, 10);
  // 1. Group the covered markets by event and parse team names from the rules.
  std::map<std::string, std::vector<const Market*>> by_event;
  for (const auto& m : markets) {
    if (!leagues_.count(m.series_ticker)) continue;
    std::string date = event_date(m.event_ticker);
    if (date.empty()) continue;
    long long dt = parse_iso(date + "T12:00:00Z") - now;
    if (dt > 36 * 3600 || dt < -30 * 3600) continue;  // only games within about a day
    by_event[m.event_ticker].push_back(&m);
  }
  std::set<std::string> slugs;
  for (auto& [ev, ms] : by_event) {
    EventInfo& e = events_[ev];
    e.event_ticker = ev;
    e.series = ms.front()->series_ticker;
    e.date = event_date(ev);
    if (e.home.empty()) {
      for (const Market* m : ms)
        if (parse_teams(m->rules_primary, e.home, e.away)) break;
    }
    for (const Market* m : ms) {
      std::string side = market_side(*m, e.home, e.away);
      if (side == "tie") e.tie_ticker = m->ticker;
      else if (side == "home") e.home_ticker = m->ticker;
      else if (side == "away") e.away_ticker = m->ticker;
    }
    slugs.insert(leagues_[e.series]);
    // 2. Scoring rates fitted from the market. Pre-match this is a clean read of the prices.
    //    Mid-match it is not: prices already contain the goals scored, so fitting the full-match
    //    model to them hands those goals to the rates, and fair() then applies the score a second
    //    time. On 2026-09-06 a restart at half time refitted Bologna-Sassuolo from a 0-1 book and
    //    the model read 0.82 for the away side against a market at 0.535. When the match is
    //    already running, fit the rates GIVEN the score and the time left, which reproduces the
    //    market instead of inventing an edge, and carry the extra uncertainty in fair().
    auto st = states_.find(ev);
    bool started = st != states_.end() && st->second.state != "pre";
    bool have_rates = e.lam_home > 0 && !e.lam_default;
    if ((!started || !have_rates) && !e.tie_ticker.empty() && !e.home_ticker.empty() && !e.away_ticker.empty()) {
      const Market *mt = nullptr, *mh = nullptr, *ma = nullptr;
      for (const Market* m : ms) {
        if (m->ticker == e.tie_ticker) mt = m;
        else if (m->ticker == e.home_ticker) mh = m;
        else if (m->ticker == e.away_ticker) ma = m;
      }
      auto twosided = [](const Market* m) { return m && m->yes_bid > 0 && m->yes_ask > 0 && m->yes_ask < 1; };
      if (twosided(mt) && twosided(mh) && twosided(ma)) {
        double pd = mt->mid(), ph = mh->mid(), pa = ma->mid();
        double s = pd + ph + pa;
        if (s > 0.9 && s < 1.15) {
          std::pair<double, double> fit;
          bool inplay = started && st != states_.end() && st->second.state == "in";
          if (inplay) {
            const MatchState& ms2 = st->second;
            double rem = std::max(0.0, t_eff_ - ms2.minute) / t_eff_ * late_factor(ms2.minute);
            fit = fit_lambdas_inplay(ph / s, pd / s, rem, ms2.home_score - ms2.away_score, rho_);
          } else {
            fit = fit_lambdas(ph / s, pd / s, rho_);
          }
          e.lam_home = fit.first;
          e.lam_away = fit.second;
          e.lam_default = false;
          e.lam_inplay_fit = inplay;
          e.lam_ts = now;
          // Never let a mid-match fit overwrite a clean pre-match one on disk.
          if (!inplay || !store.kv_get("lam:" + ev))
            store.kv_set("lam:" + ev, fmt2(fit.first) + "," + fmt2(fit.second) + (inplay ? ",inplay" : ",pre"));
        }
      }
    }
    if (e.lam_home <= 0) {
      if (auto v = store.kv_get("lam:" + ev)) {
        auto c = v->find(',');
        if (c != std::string::npos) {
          e.lam_home = std::atof(v->substr(0, c).c_str());
          e.lam_away = std::atof(v->substr(c + 1).c_str());
          e.lam_inplay_fit = v->find(",inplay") != std::string::npos;
        }
      }
    }
    if (e.lam_home <= 0) {
      e.lam_home = lambda_home_;
      e.lam_away = lambda_away_;
      e.lam_default = true;
    }
  }
  // 3. ESPN scoreboards for the leagues in play.
  for (const auto& slug : slugs) fetch_league(slug, now);
  // 4. Match Kalshi events to ESPN events by team names and date.
  for (auto& [ev, e] : events_) {
    if (!by_event.count(ev) || e.home.empty()) continue;
    auto cit = espn_cache_.find(leagues_[e.series]);
    if (cit == espn_cache_.end() || !cit->second.contains("events")) continue;
    long long data_ts = espn_data_ts_[leagues_[e.series]];
    try {
    const json* best = nullptr;
    double best_score = 0;
    if (!cit->second.is_object() || !cit->second["events"].is_array()) continue;
    for (const auto& x : cit->second["events"]) {
      if (!x.is_object() || !x.contains("competitions") || !x["competitions"].is_array() || x["competitions"].empty()) continue;
      long long dd = std::llabs(parse_iso(x.value("date", "")) - parse_iso(e.date + "T12:00:00Z"));
      if (dd > 30 * 3600) continue;
      const json& comp = x["competitions"][0];
      if (!comp.is_object() || !comp.contains("competitors") || !comp["competitors"].is_array()) continue;
      std::string hn, an;
      for (const auto& c : comp["competitors"]) {
        if (!c.is_object() || !c.contains("team") || !c["team"].is_object()) continue;
        std::string nm = c["team"].value("displayName", "");
        if (c.value("homeAway", "") == "home") hn = nm;
        else an = nm;
      }
      double sc = std::min(name_match(e.home, hn), name_match(e.away, an));
      if (sc > best_score) { best_score = sc; best = &x; }
    }
    if (!best || best_score < 0.5) continue;
    const json& x = *best;
    const json& comp = x["competitions"][0];
    if (!x.contains("status") || !x["status"].is_object() || !x["status"].contains("type") || !x["status"]["type"].is_object()) continue;
    MatchState& s = states_[ev];
    s.espn_id = x.value("id", "");
    s.state = x["status"]["type"].value("state", "");
    s.status_name = x["status"]["type"].value("name", "");
    s.period = x["status"].value("period", 0);
    s.minute = parse_clock(x["status"].value("displayClock", "0'"), s.period, s.status_name);
    s.kickoff = x.value("date", "");
    s.home = e.home;
    s.away = e.away;
    s.red_home = s.red_away = 0;
    std::string home_id, away_id;
    for (const auto& c : comp["competitors"]) {
      if (!c.is_object() || !c.contains("team") || !c["team"].is_object()) continue;
      bool home = c.value("homeAway", "") == "home";
      std::string id = c["team"].value("id", "");
      int score = std::atoi((c.contains("score") && c["score"].is_string() ? c["score"].get<std::string>() : std::string("0")).c_str());
      double sot = 0, poss = 0;
      for (const auto& st : c.value("statistics", json::array())) {
        if (!st.is_object()) continue;
        std::string nm = st.value("name", "");
        if (nm == "shotsOnTarget") sot = std::atof(st.value("displayValue", "0").c_str());
        if (nm == "possessionPct") poss = std::atof(st.value("displayValue", "0").c_str());
      }
      if (home) { s.home_score = score; s.sot_home = sot; s.poss_home = poss; home_id = id; }
      else { s.away_score = score; s.sot_away = sot; away_id = id; }
    }
    for (const auto& d : comp.value("details", json::array())) {
      if (!d.is_object() || !d.value("redCard", false)) continue;
      std::string tid = d.contains("team") && d["team"].is_object() ? d["team"].value("id", "") : "";
      if (tid == home_id) s.red_home++;
      else if (tid == away_id) s.red_away++;
    }
    s.updated = data_ts;  // the scoreboard's age, not the cycle's: a failed refresh must read stale
    // 5. Research log: every in-play state, every log_seconds_.
    if (s.state == "in" && now - last_log_[ev] >= log_seconds_) {
      last_log_[ev] = now;
      double rem = std::max(0.0, t_eff_ - s.minute) / t_eff_ * late_factor(s.minute);
      double lh = e.lam_home * std::pow(red_self_, s.red_home) * std::pow(red_opp_, s.red_away) * rem;
      double la = e.lam_away * std::pow(red_self_, s.red_away) * std::pow(red_opp_, s.red_home) * rem;
      MatchProbs mp = match_probs(lh, la, s.home_score - s.away_score);
      const std::vector<const Market*>& ev_markets = by_event[ev];  // (a structured binding cannot be captured in C++17)
      auto mid_of = [&ev_markets](const std::string& t) -> double {
        for (const Market* m : ev_markets) if (m->ticker == t) return m->mid();
        return 0;
      };
      store.exec(
          "INSERT INTO ext_sports(ts,event_ticker,espn_id,state,minute,home,away,home_score,away_score,red_home,"
          "red_away,sot_home,sot_away,poss_home,lam_home,lam_away,p_home,p_draw,p_away,mkt_home,mkt_draw,mkt_away) "
          "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {now, ev, s.espn_id, s.state, s.minute, s.home, s.away, static_cast<long long>(s.home_score),
           static_cast<long long>(s.away_score), static_cast<long long>(s.red_home),
           static_cast<long long>(s.red_away), s.sot_home, s.sot_away, s.poss_home, e.lam_home, e.lam_away,
           mp.home, mp.draw, mp.away, mid_of(e.home_ticker), mid_of(e.tie_ticker), mid_of(e.away_ticker)});
    }
    } catch (const std::exception& ex) {  // one malformed ESPN event must not take the league down
      LOG_WARN("sports_inplay %s: %s", ev.c_str(), ex.what());
    }
  }
}

std::optional<Fair> SportsInplay::fair(const Market& m, const Series&, Store&) {
  auto eit = events_.find(m.event_ticker);
  auto sit = states_.find(m.event_ticker);
  if (eit == events_.end() || sit == states_.end()) return std::nullopt;
  const EventInfo& e = eit->second;
  const MatchState& s = sit->second;
  long long now = now_s();
  if (s.state != "in" || now - s.updated > 90) return std::nullopt;
  double remaining = t_eff_ - s.minute;
  if (remaining < 3) return std::nullopt;  // settlement is minutes away; books are unreliable
  double rem = remaining / t_eff_ * late_factor(s.minute);
  double lh0 = e.lam_home * std::pow(red_self_, s.red_home) * std::pow(red_opp_, s.red_away);
  double la0 = e.lam_away * std::pow(red_self_, s.red_away) * std::pow(red_opp_, s.red_home);
  int d = s.home_score - s.away_score;
  auto pick = [&](const MatchProbs& mp) -> double {
    if (m.ticker == e.tie_ticker) return mp.draw;
    if (m.ticker == e.home_ticker) return mp.home;
    if (m.ticker == e.away_ticker) return mp.away;
    return -1;
  };
  double p = pick(match_probs(lh0 * rem, la0 * rem, d));
  if (p < 0) return std::nullopt;
  double p_hi = pick(match_probs(lh0 * rem * 1.25, la0 * rem * 1.25, d));
  double p_lo = pick(match_probs(lh0 * rem * 0.8, la0 * rem * 0.8, d));
  Fair f;
  f.p = p;
  f.sigma = 0.5 * std::fabs(p_hi - p_lo) + (e.lam_default ? 0.06 : (e.lam_inplay_fit ? 0.05 : 0.03));
  char buf[200];
  std::snprintf(buf, sizeof buf, "%.0f' %d-%d lam %.2f/%.2f%s red %d/%d sot %.0f/%.0f", s.minute, s.home_score,
                s.away_score, e.lam_home, e.lam_away,
                e.lam_default ? " (default)" : (e.lam_inplay_fit ? " (fitted mid-match: reproduces the market at the fit, so the edge only appears when the state changes)" : ""),
                s.red_home, s.red_away, s.sot_home, s.sot_away);
  f.note = buf;
  f.cluster = m.event_ticker;
  return f;
}

std::string SportsInplay::describe() const {
  int live = 0, tracked = 0;
  for (const auto& [ev, s] : states_) {
    ++tracked;
    if (s.state == "in") ++live;
  }
  std::ostringstream os;
  os << "Poisson in-play (ESPN live state, lambdas from pre-match prices); tracked " << tracked << ", live " << live;
  for (const auto& [ev, s] : states_)
    if (s.state == "in") os << "; " << s.home << " " << s.home_score << "-" << s.away_score << " " << s.away << " " << s.minute << "'";
  return os.str();
}

}  // namespace desk
