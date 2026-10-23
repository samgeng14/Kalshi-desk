#include "deskd/ref/ncaa.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <tuple>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/net/http.h"

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
std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
// A whole number, as NCAA.com writes one: a JSON integer or a string of digits ("34"); else def
long long whole(const json& v, long long def) {
  if (v.is_number_integer()) return v.get<long long>();
  if (!v.is_string()) return def;
  const std::string s = trim(v.get<std::string>());
  if (s.empty() || s.size() > 18 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return def;
  return std::stoll(s);
}
std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// currentPeriod: "1st", "2nd", "3", the number; "HALF", "OT", "2OT", "FINAL" or "" are 0 (unread),
// so an overtime is never read as a regulation period
int period_of(const std::string& text) {
  const std::string s = lower(trim(text));
  std::size_t i = 0;
  while (i < s.size() && i < 3 && s[i] >= '0' && s[i] <= '9') ++i;
  if (i == 0) return 0;
  const std::string rest = s.substr(i);
  if (!rest.empty() && rest != "st" && rest != "nd" && rest != "rd" && rest != "th") return 0;
  return std::stoi(s.substr(0, i));
}
// noon UTC of a YYYY-MM-DD date (espn.cpp's day_ms)
std::int64_t day_ms(const std::string& ymd) {
  int y = 0, mo = 0, d = 0;
  if (std::sscanf(ymd.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
  std::tm tm{};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = 12;
  return static_cast<std::int64_t>(timegm(&tm)) * 1000;
}
// names.char6, else short, else seo: the abbreviation; names.short, else char6, else seo: the name
std::pair<std::string, std::string> names_of(const json& team) {
  const auto& n = jget(team, "names");
  const std::string char6 = jstr(n, "char6"), shrt = jstr(n, "short"), seo = jstr(n, "seo");
  return {!char6.empty() ? char6 : !shrt.empty() ? shrt : seo, !shrt.empty() ? shrt : !char6.empty() ? char6 : seo};
}
constexpr std::int64_t kLateTipMs = 3 * 3600000LL;   // a 'pre' game this far past its start is not about to tip
}  // namespace

const std::vector<NcaaSport>& ncaa_sports() {
  static const std::vector<NcaaSport> S = {
      {"KXNCAAWBGAME", "basketball-women", "d1"},
      {"KXNCAAWVMATCH", "volleyball-women", "d1"},
      {"KXNCAAMBGAME", "basketball-men", "d1"},   // the men's control (tools/college_gate.py), report only
  };
  return S;
}

const NcaaSport* ncaa_sport_for(const std::string& ticker) {
  for (const auto& s : ncaa_sports())
    if (ticker.size() > s.series.size() && ticker.compare(0, s.series.size(), s.series) == 0 && ticker[s.series.size()] == '-')
      return &s;
  return nullptr;
}

std::vector<EspnGame> parse_ncaa_scoreboard(const json& sb, const std::string& league, std::int64_t now) {
  std::vector<EspnGame> out;
  const auto& games = jget(sb, "games");
  if (!games.is_array()) return out;
  for (const auto& item : games) {
    const auto& game = jget(item, "game");
    const auto& home = jget(game, "home");
    const auto& away = jget(game, "away");
    EspnGame g;
    g.src = "NCAA";
    g.league = league;
    g.id = jstr(game, "gameID");
    if (g.id.empty()) g.id = jstr(game, "url");
    if (g.id.empty() || !home.is_object() || !away.is_object()) continue;
    const std::string state = jstr(game, "gameState");
    const std::string ls = lower(state);
    g.state = ls == "pre" ? "pre" : ls == "live" ? "in" : ls == "final" ? "post" : state;   // another state as written
    g.detail = jstr(game, "currentPeriod");
    g.period = period_of(g.detail);
    g.clock = jstr(game, "contestClock");
    const auto start = whole(jget(game, "startTimeEpoch"), 0);
    g.start_ms = start > 0 ? start * 1000 : 0;
    std::tie(g.home_abbr, g.home_name) = names_of(home);
    std::tie(g.away_abbr, g.away_name) = names_of(away);
    g.home_score = static_cast<int>(whole(jget(home, "score"), 0));
    g.away_score = static_cast<int>(whole(jget(away, "score"), 0));
    if (g.state.empty() || g.home_abbr.empty() || g.away_abbr.empty()) continue;
    g.fetched_ms = g.first_seen_ms = now;
    out.push_back(std::move(g));
  }
  return out;
}

NcaaFeed::NcaaFeed(net::Endpoint ep, EspnFeed::MarketsFn markets) : NcaaFeed(std::move(ep), std::move(markets), Options{}) {}
NcaaFeed::NcaaFeed(net::Endpoint ep, EspnFeed::MarketsFn markets, Options options)
    : endpoint_(std::move(ep)), markets_(std::move(markets)), options_(options),
      guard_(SendGuard::data_hosts({"data.ncaa.com"})), board_(std::make_shared<const EspnBoard>()) {
  factory_ = std::make_unique<net::ConnFactory>(guard_);
}
NcaaFeed::~NcaaFeed() = default;

void NcaaFeed::set_on_change(EspnFeed::ScoreChangeFn fn) { on_change_ = std::move(fn); }

// On, or about to start: from hot_pre_ms before the start until the tip, a late one included.
bool NcaaFeed::hot(const EspnGame& g, std::int64_t now) const {
  if (g.state == "in") return true;
  return g.state == "pre" && g.start_ms > 0 && now >= g.start_ms - options_.hot_pre_ms && now <= g.start_ms + kLateTipMs;
}

std::shared_ptr<const EspnBoard> NcaaFeed::board() const {
  std::lock_guard<std::mutex> lk(m_);
  return board_;
}

json NcaaFeed::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json shown = json::object();
  for (const auto& [tk, r] : *board_)
    shown[tk] = {{"per", r.per}, {"clock", r.clock}, {"score", r.score}, {"fetched_ms", r.fetched_ms},
                 {"first_seen_ms", r.first_seen_ms}};
  return {{"requests", requests_}, {"failures", failures_}, {"no_games", no_games_}, {"error", last_error_},
          {"markets", shown}, {"fast_series", options_.fast_series}, {"hot", hot_}};
}

// A 404 is a date without games: an empty scoreboard, not a failure (recorder/scores.py's rule).
bool NcaaFeed::get(const std::string& target, json* body, std::string* why, Worker* self) {
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
  if (x.fate != net::Exchange::Fate::REPLIED) return fail(target.substr(0, 80) + ": " + x.why);
  if (!x.reusable) conn_.reset();
  if (x.res.status == 404) {
    std::lock_guard<std::mutex> lk(m_);
    ++no_games_;
    *body = json::object();
    return true;
  }
  if (x.res.status != 200) return fail(target.substr(0, 80) + ": HTTP " + std::to_string(x.res.status));
  *body = json::parse(x.res.body, nullptr, false);
  if (!body->is_object()) return fail(target.substr(0, 80) + ": not a JSON object");
  return true;
}

void NcaaFeed::step(std::int64_t now, Worker* self) {
  // The watched college game markets, by event: each market pays on one team (its ticker's last code).
  std::map<std::string, KalshiGame> events;
  std::map<std::string, const NcaaSport*> sport_of;
  for (const auto& m : markets_()) {
    const NcaaSport* sp = ncaa_sport_for(m.ticker);
    if (!sp || m.event_ticker.empty()) continue;
    auto& k = events[m.event_ticker];
    if (k.event_ticker.empty()) {
      k.event_ticker = m.event_ticker;
      k.series = sp->series;
      k.date = kalshi_event_date(m.event_ticker);
      k.start_min = kalshi_event_start_minutes(m.event_ticker);
    }
    sport_of[m.event_ticker] = sp;
    const auto code = m.ticker.substr(m.ticker.find_last_of('-') + 1);
    if (k.ticker_a.empty() || k.ticker_a == m.ticker) { k.ticker_a = m.ticker; k.code_a = code; k.name_a = m.yes_sub_title; }
    else if (k.ticker_b.empty() || k.ticker_b == m.ticker) { k.ticker_b = m.ticker; k.code_b = code; k.name_b = m.yes_sub_title; }
  }
  // The scoreboards read hot: a fast series' sport-date while one of its matched games is on or
  // about to start, as the last scoreboard said.
  std::set<std::string> hot_keys;
  for (const auto& [ev, m] : matched_) {
    const auto e = events.find(ev);
    if (e == events.end()) continue;
    const std::string key = sport_of[ev]->league() + "|" + m.id;
    const auto g = games_.find(key);
    const auto from = from_.find(key);
    if (g != games_.end() && from != from_.end() && options_.fast_series.count(e->second.series) && hot(g->second, now))
      hot_keys.insert(from->second);
  }
  // Scoreboards, for each sport and event date within a day and a half of now. A game keeps its
  // first sighting while its state is the same.
  std::set<std::pair<const NcaaSport*, std::string>> want;
  for (const auto& [ev, k] : events)
    if (!k.date.empty() && std::llabs(day_ms(k.date) - now) <= 36 * 3600000LL) want.insert({sport_of[ev], k.date});
  for (const auto& [sp, date] : want) {
    const std::string league = sp->league(), key = league + "|" + date;
    if (now - fetched_[key] < (hot_keys.count(key) ? options_.hot_scoreboard_ms : options_.scoreboard_ms)) continue;
    fetched_[key] = now;
    json body;
    std::string why;
    if (!get("/casablanca/scoreboard/" + league + "/" + date.substr(0, 4) + "/" + date.substr(5, 2) + "/" + date.substr(8, 2) +
                 "/scoreboard.json", &body, &why, self))
      continue;
    ++parsed_[league];
    for (auto& g : parse_ncaa_scoreboard(body, league, now)) {
      const std::string gk = league + "|" + g.id;
      const auto old = games_.find(gk);
      if (old != games_.end()) {
        const bool same = same_score_state(old->second, g);
        g.first_seen_ms = same ? old->second.first_seen_ms : now;
        g.prev_poll_ms = same ? old->second.prev_poll_ms : old->second.fetched_ms;
      }
      from_[gk] = key;
      games_[gk] = std::move(g);
    }
  }
  // Match each event once, then keep the match; one that failed waits for its sport's next
  // scoreboard.
  for (const auto& [ev, k] : events) {
    if (matched_.count(ev) || k.ticker_b.empty()) continue;
    const std::string league = sport_of[ev]->league();
    const auto news = parsed_[league];
    const auto tried = match_tried_.find(ev);
    if (tried != match_tried_.end() && tried->second == news) continue;
    match_tried_[ev] = news;
    std::vector<EspnGame> candidates;
    for (const auto& [gk, g] : games_) if (g.league == league) candidates.push_back(g);
    if (auto m = match_game(k, candidates)) {
      const auto& g = games_[league + "|" + m->id];
      DLOG_INFO("ncaa: %s is %s (%s at %s)", ev.c_str(), m->id.c_str(), g.away_abbr.c_str(), g.home_abbr.c_str());
      matched_[ev] = *m;
      match_tried_.erase(ev);
    }
  }
  // The board: only from a scoreboard younger than keep_ms. Each new state of a watched game is
  // told once.
  auto board = std::make_shared<EspnBoard>();
  std::vector<const EspnGame*> changed;
  for (const auto& [ev, m] : matched_) {
    const auto e = events.find(ev);
    if (e == events.end()) continue;
    const std::string gk = sport_of[ev]->league() + "|" + m.id;
    const auto g = games_.find(gk);
    const auto* lg = espn_league_for(e->second.ticker_a);   // the series' period labels
    if (g == games_.end() || !lg) continue;
    if (on_change_) {
      auto& told = reported_[gk];
      if (told != g->second.first_seen_ms) { told = g->second.first_seen_ms; changed.push_back(&g->second); }
    }
    if (now - g->second.fetched_ms > options_.keep_ms) continue;
    for (const bool a : {true, false}) {
      EspnRef r = espn_ref(*lg, g->second, a ? m.a_home : !m.a_home, std::nullopt);
      if (r.state == "in" && g->second.period == 0) r.per = g->second.detail;   // "HALF", "OT": no number read
      (*board)[a ? e->second.ticker_a : e->second.ticker_b] = std::move(r);
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    board_ = std::move(board);
    hot_ = std::move(hot_keys);
  }
  for (const auto* g : changed) on_change_(*g);
}

void NcaaFeed::run(Worker& self) {
  while (!self.stopping()) {
    self.beat();
    const auto due = mono_ms() + options_.round_ms;   // a round every round_ms, from its start
    step(now_ms(), &self);
    for (auto left = due - mono_ms(); left > 0 && !self.stopping(); left = due - mono_ms()) {
      self.beat();
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(left, 100)));
    }
  }
}

}  // namespace deskd
