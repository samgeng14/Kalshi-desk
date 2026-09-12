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
  fit_tolerance_ = params.value("fit_tolerance", 0.02);
  inplay_min_progress_ = params.value("inplay_min_progress", 5.0);
  stale_seconds_ = params.value("stale_seconds", 90);
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

// ---- match phase -------------------------------------------------------------------------
// Derived from the provider's status block and never guessed. A missing or unrecognised status
// is Unknown, which is NOT Prematch: that distinction is the entire point. On 2026-09-06 the
// desk restarted mid-match, prepare() reached its rate fit before it had ingested any ESPN
// state, read the empty state map as "before kick-off", and calibrated a full-match model to a
// Marseille-Paris book that already contained a 1-2 scoreline. The model then applied the same
// goals a second time through the score term and printed 0.04 for a home side the market held
// at 0.205. It also overwrote the good pre-match rates on disk, tagged "pre".
const char* phase_name(MatchPhase p) {
  switch (p) {
    case MatchPhase::Prematch: return "prematch";
    case MatchPhase::FirstHalf: return "first_half";
    case MatchPhase::Halftime: return "halftime";
    case MatchPhase::SecondHalf: return "second_half";
    case MatchPhase::Stoppage: return "stoppage";
    case MatchPhase::Finished: return "finished";
    case MatchPhase::Suspended: return "suspended";
    default: return "unknown";
  }
}

MatchPhase phase_parse(const std::string& n) {
  for (MatchPhase p : {MatchPhase::Prematch, MatchPhase::FirstHalf, MatchPhase::Halftime,
                       MatchPhase::SecondHalf, MatchPhase::Stoppage, MatchPhase::Finished,
                       MatchPhase::Suspended})
    if (n == phase_name(p)) return p;
  return MatchPhase::Unknown;
}

bool phase_started(MatchPhase p) {
  return p == MatchPhase::FirstHalf || p == MatchPhase::Halftime || p == MatchPhase::SecondHalf ||
         p == MatchPhase::Stoppage || p == MatchPhase::Finished;
}

bool phase_in_play(MatchPhase p) { return p == MatchPhase::FirstHalf || p == MatchPhase::SecondHalf; }

// ESPN's status block carries a coarse type.state (pre|in|post) and a specific type.name.
// Verified against live fixtures captured 2026-09-06 (tests/fixtures/espn): halftime arrives as
// state="in", name="STATUS_HALFTIME", period=1, displayClock="45'+3'" - so state alone cannot
// tell halftime from open play, and name alone cannot be trusted to exist.
MatchPhase phase_from_espn(const std::string& state, const std::string& status_name, int period) {
  auto has = [&](const char* k) { return status_name.find(k) != std::string::npos; };
  if (has("HALFTIME")) return MatchPhase::Halftime;
  if (has("END_OF_PERIOD")) return period >= 2 ? MatchPhase::Stoppage : MatchPhase::Halftime;
  if (has("FIRST_HALF")) return MatchPhase::FirstHalf;
  if (has("SECOND_HALF")) return MatchPhase::SecondHalf;
  if (has("EXTRA") || has("OVERTIME") || has("PENALT") || has("SHOOTOUT")) return MatchPhase::Stoppage;
  if (has("FULL_TIME") || has("FINAL")) return MatchPhase::Finished;
  if (has("POSTPONED") || has("SUSPENDED") || has("ABANDONED") || has("CANCEL") || has("DELAY") ||
      has("FORFEIT"))
    return MatchPhase::Suspended;
  if (has("SCHEDULED") || has("PRE_")) return MatchPhase::Prematch;
  if (state == "pre") return MatchPhase::Prematch;
  if (state == "post") return MatchPhase::Finished;
  if (state == "in") return period >= 2 ? MatchPhase::SecondHalf : MatchPhase::FirstHalf;
  return MatchPhase::Unknown;
}

// ---- match state across restarts ---------------------------------------------------------
void SportsInplay::load_match_state(Store& store) {
  json rows = store.query(
      "SELECT event_ticker,espn_id,phase,phase_ts,home_score,away_score,minute,period,red_home,"
      "red_away,status_name,state,data_ts FROM match_state");
  for (const auto& r : rows) {
    MatchState s;
    std::string ev = r.value("event_ticker", "");
    if (ev.empty()) continue;
    s.espn_id = r.value("espn_id", "");
    s.phase = phase_parse(r.value("phase", "unknown"));
    s.phase_ts = r.value("phase_ts", 0LL);
    s.home_score = static_cast<int>(r.value("home_score", 0));
    s.away_score = static_cast<int>(r.value("away_score", 0));
    s.minute = r.value("minute", 0.0);
    s.period = static_cast<int>(r.value("period", 0));
    s.red_home = static_cast<int>(r.value("red_home", 0));
    s.red_away = static_cast<int>(r.value("red_away", 0));
    s.status_name = r.value("status_name", "");
    s.state = r.value("state", "");
    s.data_ts = r.value("data_ts", 0LL);
    s.updated = s.data_ts;
    s.from_disk = true;   // known to have started, but not yet re-observed this process
    states_[ev] = s;
  }
  LOG_INFO("sports_inplay: restored %d match states", static_cast<int>(rows.size()));
}

void SportsInplay::save_match_state(Store& store, const std::string& ev, const MatchState& s) {
  store.exec(
      "INSERT INTO match_state(event_ticker,espn_id,phase,phase_ts,home_score,away_score,minute,"
      "period,red_home,red_away,status_name,state,data_ts,updated_ts) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(event_ticker) DO UPDATE SET "
      "espn_id=excluded.espn_id,phase=excluded.phase,phase_ts=excluded.phase_ts,"
      "home_score=excluded.home_score,away_score=excluded.away_score,minute=excluded.minute,"
      "period=excluded.period,red_home=excluded.red_home,red_away=excluded.red_away,"
      "status_name=excluded.status_name,state=excluded.state,data_ts=excluded.data_ts,"
      "updated_ts=excluded.updated_ts",
      {ev, s.espn_id, std::string(phase_name(s.phase)), s.phase_ts,
       static_cast<long long>(s.home_score), static_cast<long long>(s.away_score), s.minute,
       static_cast<long long>(s.period), static_cast<long long>(s.red_home),
       static_cast<long long>(s.red_away), s.status_name, s.state, s.data_ts, now_s()});
}

// ---- rate fitting ------------------------------------------------------------------------
// Only ever called once the phase for this event is settled. Pre-match, the book is a clean read
// of the two teams. In play it is not: the prices already contain the goals, so the fit has to be
// conditioned on the score and the time left, which reproduces the book instead of inventing an
// edge. Reproducing the book is the success condition, not a signal - see fair().
void SportsInplay::fit_rates(const std::string& ev, EventInfo& e, const std::vector<const Market*>& ms,
                             Store& store, long long now) {
  if (e.tie_ticker.empty() || e.home_ticker.empty() || e.away_ticker.empty()) return;
  auto sit = states_.find(ev);
  MatchPhase phase = sit == states_.end() ? MatchPhase::Unknown : sit->second.phase;
  bool have_rates = e.lam_home > 0 && !e.lam_default;

  auto from_disk = [&]() {
    if (e.lam_home > 0) return;
    if (auto v = store.kv_get("lam:" + ev)) {
      auto c = v->find(',');
      if (c != std::string::npos) {
        e.lam_home = std::atof(v->substr(0, c).c_str());
        e.lam_away = std::atof(v->substr(c + 1).c_str());
        e.lam_calibration = v->find(",inplay") != std::string::npos ? "inplay" : "prematch";
        // Rates restored from disk carry no calibration snapshot in this process, so they cannot
        // be checked for convergence and cannot claim an independent edge.
        e.lam_converged = e.lam_calibration == "prematch";
      }
    }
    if (e.lam_home <= 0) { e.lam_home = lambda_home_; e.lam_away = lambda_away_; e.lam_default = true; }
  };

  // Unknown phase: we cannot tell whether the book contains goals. This is the restart case.
  if (phase == MatchPhase::Unknown) { from_disk(); return; }
  if (phase == MatchPhase::Finished || phase == MatchPhase::Suspended) { from_disk(); return; }
  // Every calibration needs fresh, first-hand state. A phase restored from disk proves the match
  // started - which is what keeps us out of the pre-match branch - but it describes the match as
  // it was before the restart, and the match may have moved on during the outage.
  if (sit->second.from_disk || now - sit->second.data_ts > stale_seconds_) { from_disk(); return; }
  bool started = phase_started(phase);
  if (started && have_rates) return;   // never re-fit over rates we already trust

  const Market *mt = nullptr, *mh = nullptr, *ma = nullptr;
  for (const Market* m : ms) {
    if (m->ticker == e.tie_ticker) mt = m;
    else if (m->ticker == e.home_ticker) mh = m;
    else if (m->ticker == e.away_ticker) ma = m;
  }
  auto twosided = [](const Market* m) { return m && m->yes_bid > 0 && m->yes_ask > 0 && m->yes_ask < 1; };
  if (!twosided(mt) || !twosided(mh) || !twosided(ma)) { from_disk(); return; }
  double pd = mt->mid(), ph = mh->mid(), pa = ma->mid(), sum = pd + ph + pa;
  if (!(sum > 0.9 && sum < 1.15)) { from_disk(); return; }
  ph /= sum; pd /= sum; pa /= sum;

  double rem = 1.0, minute = 0;
  int d = 0, hs = 0, as = 0;
  bool inplay = started;
  if (inplay) {
    if (sit == states_.end()) { from_disk(); return; }
    const MatchState& s = sit->second;
    minute = s.minute; hs = s.home_score; as = s.away_score; d = hs - as;
    rem = std::max(0.0, t_eff_ - minute) / t_eff_ * late_factor(minute);
    if (rem <= 0.02) { from_disk(); return; }
  }

  std::pair<double, double> fit =
      inplay ? fit_lambdas_inplay(ph, pd, rem, d, rho_) : fit_lambdas(ph, pd, rho_);

  // Convergence, checked against the same snapshot the fit consumed. A fit that cannot reproduce
  // its own calibration prices did not converge, and every difference it later shows against the
  // book is an artefact of that failure rather than information.
  MatchProbs chk = match_probs(fit.first * rem, fit.second * rem, d, d == 0 ? rho_ : 0.0);
  double residual = std::max(std::fabs(chk.home - ph),
                             std::max(std::fabs(chk.draw - pd), std::fabs(chk.away - pa)));
  bool converged = residual <= fit_tolerance_;

  e.lam_home = fit.first;
  e.lam_away = fit.second;
  e.lam_default = false;
  e.lam_calibration = inplay ? "inplay" : "prematch";
  e.lam_phase = phase;
  e.lam_cal_minute = minute;
  e.lam_cal_home = hs;
  e.lam_cal_away = as;
  e.lam_cal_ph = ph; e.lam_cal_pd = pd; e.lam_cal_pa = pa;
  e.lam_residual = residual;
  e.lam_converged = converged;
  e.lam_ts = now;

  store.exec(
      "INSERT INTO rate_fits(ts,event_ticker,model_version,calibration,phase,minute,home_score,"
      "away_score,p_home,p_draw,p_away,lam_home,lam_away,residual,converged) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {now, ev, std::string(MODEL_VERSION), e.lam_calibration, std::string(phase_name(phase)), minute,
       static_cast<long long>(hs), static_cast<long long>(as), ph, pd, pa, fit.first, fit.second,
       residual, static_cast<long long>(converged ? 1 : 0)});

  // Only a genuine pre-match fit is allowed to become the persisted rates for this match. A
  // mid-match calibration is specific to the moment it was taken and must never be mistaken for
  // one on the next restart.
  if (!started) store.kv_set("lam:" + ev, fmt2(fit.first) + "," + fmt2(fit.second) + ",pre");
  else if (!store.kv_get("lam:" + ev))
    store.kv_set("lam:" + ev, fmt2(fit.first) + "," + fmt2(fit.second) + ",inplay");
  if (!converged)
    LOG_WARN("sports_inplay %s: %s fit did not converge (residual %.3f > %.3f); event will not trade",
             ev.c_str(), e.lam_calibration.c_str(), residual, fit_tolerance_);
}

void SportsInplay::prepare(const std::vector<Market>& markets, Store& store) {
  long long now = now_s();
  if (!state_loaded_) { load_match_state(store); state_loaded_ = true; }
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
  }

  // 2. ESPN scoreboards for the leagues in play. This has to happen BEFORE any rate fitting:
  //    the fit needs to know whether the book it is reading already contains goals.
  for (const auto& slug : slugs) fetch_league(slug, now);

  // 3. Match Kalshi events to ESPN events by team names and date, then settle the phase.
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
      // A duplicate listing of the same fixture must not flip the match we track between cycles.
      if (sc > best_score + 1e-9) { best_score = sc; best = &x; }
    }
    if (!best || best_score < 0.5) continue;
    const json& x = *best;
    const json& comp = x["competitions"][0];
    if (!x.contains("status") || !x["status"].is_object() || !x["status"].contains("type") || !x["status"]["type"].is_object()) continue;
    MatchState& s = states_[ev];
    MatchPhase was = s.phase;
    std::string prev_id = s.espn_id;
    s.espn_id = x.value("id", "");
    s.state = x["status"]["type"].value("state", "");
    s.status_name = x["status"]["type"].value("name", "");
    s.period = x["status"].contains("period") && x["status"]["period"].is_number()
                   ? x["status"]["period"].get<int>()
                   : 0;  // scheduled fixtures carry "period": null
    s.minute = parse_clock(x["status"].value("displayClock", "0'"), s.period, s.status_name);
    s.kickoff = x.value("date", "");
    s.home = e.home;
    s.away = e.away;
    s.red_home = s.red_away = 0;
    MatchPhase now_phase = phase_from_espn(s.state, s.status_name, s.period);
    // A match never un-starts. If the provider briefly reports a pre-match status for a fixture
    // we have already seen in play (a stale row, a duplicate listing, a mid-match identity
    // change), keep the phase we had rather than handing the fit a false kick-off.
    if (now_phase == MatchPhase::Prematch && phase_started(was)) now_phase = was;
    if (!prev_id.empty() && !s.espn_id.empty() && prev_id != s.espn_id && phase_started(was)) {
      LOG_WARN("sports_inplay %s: espn id changed %s -> %s mid-match; holding phase %s",
               ev.c_str(), prev_id.c_str(), s.espn_id.c_str(), phase_name(was));
      now_phase = was;
    }
    if (now_phase != was) s.phase_ts = now;
    s.phase = now_phase;
    s.from_disk = false;
    std::string home_id, away_id;
    for (const auto& c : comp["competitors"]) {
      if (!c.is_object() || !c.contains("team") || !c["team"].is_object()) continue;
      bool home = c.value("homeAway", "") == "home";
      std::string id = c["team"].value("id", "");
      int score = std::atoi((c.contains("score") && c["score"].is_string() ? c["score"].get<std::string>() : std::string("0")).c_str());
      double sot = 0, poss = 0;
      for (const auto& stx : c.value("statistics", json::array())) {
        if (!stx.is_object()) continue;
        std::string nm = stx.value("name", "");
        if (nm == "shotsOnTarget") sot = std::atof(stx.value("displayValue", "0").c_str());
        if (nm == "possessionPct") poss = std::atof(stx.value("displayValue", "0").c_str());
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
    s.data_ts = data_ts;
    save_match_state(store, ev, s);
    } catch (const std::exception& ex) {  // one malformed ESPN event must not take the league down
      LOG_WARN("sports_inplay %s: %s", ev.c_str(), ex.what());
    }
  }

  // 4. Rates, now that every phase is settled.
  for (auto& [ev, ms] : by_event) fit_rates(ev, events_[ev], ms, store, now);

  // 5. Research log: every in-play state, every log_seconds_.
  for (auto& [ev, ms] : by_event) {
    auto sit = states_.find(ev);
    if (sit == states_.end()) continue;
    MatchState& s = sit->second;
    if (!phase_in_play(s.phase) && s.phase != MatchPhase::Halftime) continue;
    if (s.from_disk || now - last_log_[ev] < log_seconds_) continue;
    last_log_[ev] = now;
    const EventInfo& e = events_[ev];
    double rem = std::max(0.0, t_eff_ - s.minute) / t_eff_ * late_factor(s.minute);
    double lh = e.lam_home * std::pow(red_self_, s.red_home) * std::pow(red_opp_, s.red_away) * rem;
    double la = e.lam_away * std::pow(red_self_, s.red_away) * std::pow(red_opp_, s.red_home) * rem;
    MatchProbs mp = match_probs(lh, la, s.home_score - s.away_score);
    const auto* event_markets = &ms;
    auto mid_of = [event_markets](const std::string& t) -> double {
      for (const Market* m : *event_markets) if (m->ticker == t) return m->mid();
      return 0;
    };
    store.exec(
        "INSERT INTO ext_sports(ts,event_ticker,espn_id,state,minute,home,away,home_score,away_score,red_home,"
        "red_away,sot_home,sot_away,poss_home,lam_home,lam_away,p_home,p_draw,p_away,mkt_home,mkt_draw,mkt_away) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {now, ev, s.espn_id, std::string(phase_name(s.phase)), s.minute, s.home, s.away,
         static_cast<long long>(s.home_score), static_cast<long long>(s.away_score),
         static_cast<long long>(s.red_home), static_cast<long long>(s.red_away), s.sot_home, s.sot_away,
         s.poss_home, e.lam_home, e.lam_away, mp.home, mp.draw, mp.away, mid_of(e.home_ticker),
         mid_of(e.tie_ticker), mid_of(e.away_ticker)});
  }
}

std::optional<Fair> SportsInplay::fair(const Market& m, const Series&, Store&) {
  auto eit = events_.find(m.event_ticker);
  auto sit = states_.find(m.event_ticker);
  if (eit == events_.end() || sit == states_.end()) return std::nullopt;
  const EventInfo& e = eit->second;
  const MatchState& s = sit->second;
  long long now = now_s();
  // Only open play is quotable. Halftime has no price discovery, stoppage means extra time or
  // penalties which this model does not cover, and Unknown means we do not know what we are
  // looking at - none of them may produce a number, and a state restored from disk describes a
  // match as it was before the restart.
  if (!phase_in_play(s.phase) || s.from_disk) return std::nullopt;
  if (now - s.updated > stale_seconds_) return std::nullopt;
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
  bool inplay_fit = e.lam_calibration == "inplay";
  f.sigma = 0.5 * std::fabs(p_hi - p_lo) + (e.lam_default ? 0.06 : (inplay_fit ? 0.05 : 0.03));
  char buf[320];
  std::snprintf(buf, sizeof buf, "%.0f' %d-%d lam %.2f/%.2f%s red %d/%d sot %.0f/%.0f", s.minute, s.home_score,
                s.away_score, e.lam_home, e.lam_away,
                e.lam_default ? " (default)" : (inplay_fit ? " (fitted mid-match)" : ""),
                s.red_home, s.red_away, s.sot_home, s.sot_away);
  f.note = buf;
  f.cluster = m.event_ticker;
  // A fit that never reproduced its own calibration prices is not a model, it is noise.
  if (!e.lam_default && !e.lam_converged) {
    f.research_only = true;
    f.research_reason = "rate fit did not converge (residual " + fmt2(e.lam_residual) + " > " +
                        fmt2(fit_tolerance_) + "): logging only";
  } else if (inplay_fit) {
    // The mid-match fit was built to reproduce this book at its calibration point, so there the
    // model and the market agree by construction and any gap is fit error rather than a signal.
    // Independent information only accumulates once the score moves or the clock runs on.
    bool score_moved = (s.home_score - s.away_score) != (e.lam_cal_home - e.lam_cal_away);
    double progress = s.minute - e.lam_cal_minute;
    if (!score_moved && progress < inplay_min_progress_) {
      char why[300];
      std::snprintf(why, sizeof why,
                    "rates were calibrated to this same book at %.0f' %d-%d, %.0f' ago: the model "
                    "reproduces it by construction, so this gap is fit error, not an edge",
                    e.lam_cal_minute, e.lam_cal_home, e.lam_cal_away, progress);
      f.research_only = true;
      f.research_reason = why;
    }
  }
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
