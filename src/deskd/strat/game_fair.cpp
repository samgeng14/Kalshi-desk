#include "deskd/strat/game_fair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "deskd/app/log.h"
#include "deskd/rest/state.h"

namespace deskd {
using json = nlohmann::json;

namespace {
std::string series_of(const std::string& ticker) { return ticker.substr(0, ticker.find('-')); }
Px to_px(double p) { return static_cast<Px>(std::llround(std::min(1.0, std::max(0.0, p)) * kOneDollar)); }
// The same game from the home team's side: the fields a model reads, mirrored when YES is away
EspnRef home_side(const EspnRef& r) {
  if (r.yes_home) return r;
  EspnRef h = r;
  h.yes_home = true;
  std::swap(h.yes_score, h.opp_score);
  std::swap(h.yes_lines, h.opp_lines);
  h.possession = -r.possession;
  return h;
}
bool has_feature(const BballModel& m, const std::string& name) {
  return std::find(m.features.begin(), m.features.end(), name) != m.features.end();
}
std::string model_name(const GameModel& gm) {
  if (const auto* b = std::get_if<BballModel>(&gm)) return bball_league(b->league)->model;
  return "volley_markov";
}
}  // namespace

std::optional<EffBook> effective_book(const MdBook* a, const MdBook* b) {
  std::optional<Px> bid, ask;
  auto raise = [](std::optional<Px>& v, Px x) { if (!v || x > *v) v = x; };
  auto lower = [](std::optional<Px>& v, Px x) { if (!v || x < *v) v = x; };
  if (a && !a->stale) {
    if (!a->bids.empty()) raise(bid, a->bids.front().px);
    if (!a->asks.empty()) lower(ask, a->asks.front().px);
  }
  if (b && !b->stale) {   // B's ask is a YES bid on A's team, B's bid an ask
    if (!b->asks.empty()) raise(bid, kOneDollar - b->asks.front().px);
    if (!b->bids.empty()) lower(ask, kOneDollar - b->bids.front().px);
  }
  if (!bid || !ask) return std::nullopt;
  return EffBook{*bid, *ask};
}

GameFair::GameFair(Env env, std::map<std::string, GameModel> by_series)
    : env_(std::move(env)), board_(std::make_shared<const GameBoard>()) {
  for (auto& [series, gm] : by_series) {
    std::string why;
    if (const auto* b = std::get_if<BballModel>(&gm)) {
      const auto* lg = bball_league(b->league);
      if (!lg || lg->series != series) why = "the " + b->league + " model is " + (lg ? lg->series + "'s" : "no league's");
    } else {
      const auto* lg = espn_league_for(series + "-");
      if (!lg || lg->sport != "volleyball") why = "the volleyball model is only for a volleyball series";
    }
    if (why.empty()) {
      models_.emplace(series, std::move(gm));
    } else {
      refused_[series] = why;
      DLOG_ERR("game fair: no fair on %s: %s", series.c_str(), why.c_str());
    }
  }
}

std::shared_ptr<const GameBoard> GameFair::board() const {
  std::lock_guard<std::mutex> lk(m_);
  return board_;
}

void GameFair::persist(const std::string& path) {
  std::lock_guard<std::mutex> lk(m_);
  path_ = path;
  std::ifstream in(path);
  if (!in) return;   // none yet
  const auto j = json::parse(in, nullptr, false);
  try {
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1) throw std::runtime_error("not a version 1 file");
    for (const auto& [ev, c] : j.at("p0").items()) {
      Capture x;
      x.ticker = c.at("ticker").get<std::string>();
      x.p0 = c.at("p0").get<double>();
      x.ms = c.at("ms").get<std::int64_t>();
      if (x.ticker.empty() || !(x.p0 > 0 && x.p0 < 1)) throw std::runtime_error(ev + ": no market, or a p0 outside (0, 1)");
      captures_[ev] = x;
    }
  } catch (const std::exception& e) {
    // starting from none never shows a fair early: a game already on gets none
    captures_.clear();
    path_error_ = path + " cannot be read (" + e.what() + "): the pregame prices start again from none";
    DLOG_WARN("%s", path_error_.c_str());
  }
}

// step()'s thread: captures_ is only read here, and the file written with no lock held
void GameFair::save(std::int64_t now) {
  saved_ms_ = now;
  json p0 = json::object();
  for (const auto& [ev, c] : captures_) p0[ev] = {{"ticker", c.ticker}, {"p0", c.p0}, {"ms", c.ms}};
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json{{"v", 1}, {"p0", p0}}.dump();
    if (!out) { DLOG_WARN("cannot write %s", tmp.c_str()); return; }
  }
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) { DLOG_WARN("cannot replace %s", path_.c_str()); return; }
  dirty_ = false;
}

GameFairRow GameFair::row(const std::string& ev, const std::string& tk, const EspnRef& r, const GameModel& gm) const {
  GameFairRow w;
  w.event = ev;
  w.model = model_name(gm);
  w.state = r.state;
  w.src = r.src;
  w.first_seen_ms = r.first_seen_ms;
  w.fetched_ms = r.fetched_ms;
  const auto c = captures_.find(ev);
  if (c != captures_.end()) w.p0 = c->second.ticker == tk ? c->second.p0 : 1 - c->second.p0;
  if (r.state != "in") {
    w.why = r.state == "pre" ? "not started" : r.state == "post" ? "final" : "state \"" + r.state + "\"";
    return w;
  }
  if (!w.p0) { w.why = "no pregame price: none was captured before the start"; return w; }
  // the home team's P; the away market's is 1 - it
  const EspnRef h = home_side(r);
  const double p0 = r.yes_home ? *w.p0 : 1 - *w.p0;
  std::optional<double> p;
  if (const auto* b = std::get_if<BballModel>(&gm)) {
    const auto* lg = bball_league(b->league);
    const auto clock = bball_t_left(*lg, h.period, h.clock_s);
    if (!clock) { w.why = "no game clock in period " + std::to_string(h.period); return w; }
    w.ot = clock->ot;
    if (clock->ot && lg->ot_uses_clock && !has_feature(*b, "ot")) { w.why = "overtime: the model has no ot feature"; return w; }
    p = bball_live_p(*b, h, p0);
  } else {
    const auto& v = std::get<VolleyModel>(gm);
    std::string why;
    const auto s = volley_state(h, v.rules, &why);
    if (!s) { w.why = why; return w; }
    w.points = s->points_known;
    if (!s->points_known) w.why = "set " + std::to_string(h.period) + "'s points were not read: the fair takes them 0-0";
    p = volley_live_p(v, *s, p0);
  }
  if (!p || !std::isfinite(*p)) { w.why = "the model gives no fair here"; return w; }
  w.p = r.yes_home ? *p : 1 - *p;
  return w;
}

void GameFair::step(std::int64_t now) {
  const auto s = env_.snapshot ? env_.snapshot() : nullptr;
  const auto books = env_.books ? env_.books() : nullptr;
  // the priced markets: on a score board (ESPN's first), of a series with a model, by event
  std::map<std::string, const EspnRef*> score;
  std::shared_ptr<const EspnBoard> boards[] = {env_.espn ? env_.espn() : nullptr, env_.ncaa ? env_.ncaa() : nullptr};
  for (const auto& b : boards)
    if (b)
      for (const auto& [tk, r] : *b)
        if (models_.count(series_of(tk))) score.emplace(tk, &r);
  auto board = std::make_shared<GameBoard>();
  std::map<std::string, std::vector<std::string>> events;
  for (const auto& [tk, r] : score) {
    const kalshi::MarketRow* m = nullptr;
    if (s) {
      const auto it = s->markets.find(tk);
      if (it != s->markets.end()) m = &it->second;
    }
    if (!m || m->event_ticker.empty()) {
      auto& w = (*board)[tk];
      w.model = model_name(models_.at(series_of(tk)));
      w.state = r->state;
      w.src = r->src;
      w.why = "not in the listing";
      continue;
    }
    events[m->event_ticker].push_back(tk);
  }
  auto book = [&](const std::string& tk) -> const MdBook* {
    if (!books) return nullptr;
    const auto it = books->find(tk);
    return it == books->end() ? nullptr : &it->second;
  };
  // The pregame prices: the last effective mid before the start, at 15c or less. captures_ is
  // written only here (and by persist(), before the first step), under m_ for status(); this
  // thread reads it without.
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = captures_.begin(); it != captures_.end();) {
      if (now - it->second.ms > kKeepMs) { it = captures_.erase(it); dirty_ = true; }
      else ++it;
    }
    for (const auto& [ev, tks] : events) {
      const EspnRef& r = *score.at(tks.front());
      if (tks.size() > 2 || r.state != "pre" || r.start_ms <= 0 || now >= r.start_ms) continue;
      const auto e = effective_book(book(tks[0]), tks.size() > 1 ? book(tks[1]) : nullptr);
      if (e && e->ask - e->bid <= kMaxSpread) {
        captures_[ev] = Capture{tks[0], (e->bid + e->ask) / 2.0 / kOneDollar, now};
        dirty_ = true;
      }
    }
  }
  for (const auto& [ev, tks] : events) {
    const auto& gm = models_.at(series_of(tks.front()));
    for (const auto& tk : tks) {
      auto& w = (*board)[tk];
      w = row(ev, tk, *score.at(tk), gm);
      if (tks.size() > 2) {   // a game's event has a market for each team, and no other
        w.p.reset();
        w.why = std::to_string(tks.size()) + " markets in the event: a game has two";
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    board_ = std::move(board);
  }
  if (dirty_ && !path_.empty() && now - saved_ms_ >= kSaveEveryMs) save(now);
}

json GameFair::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json models = json::object();
  for (const auto& [series, gm] : models_) {
    if (const auto* b = std::get_if<BballModel>(&gm))
      models[series] = {{"model", model_name(gm)}, {"version", b->version}, {"path", b->path}, {"sha256", b->sha256}};
    else if (const auto* v = std::get_if<VolleyModel>(&gm))
      models[series] = {{"model", model_name(gm)}, {"version", v->version}, {"path", v->path}, {"sha256", v->sha256}};
  }
  json p0 = json::object();
  for (const auto& [ev, c] : captures_) p0[ev] = {{"ticker", c.ticker}, {"p0", c.p0}, {"ms", c.ms}};
  json markets = json::object();
  for (const auto& [tk, w] : *board_)
    markets[tk] = {{"event", w.event}, {"model", w.model}, {"p0", w.p0 ? json(*w.p0) : json(nullptr)},
                   {"fair", w.p ? json(price_str(to_px(*w.p))) : json(nullptr)}, {"shown", w.shown()}, {"ot", w.ot},
                   {"state", w.state}, {"src", w.src}, {"why", w.why}};
  return {{"path", path_}, {"error", path_error_}, {"models", models}, {"refused", refused_}, {"p0", p0}, {"markets", markets}};
}

}  // namespace deskd
