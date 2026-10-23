#include "deskd/ref/scores.h"

#include <algorithm>
#include <set>

#include "deskd/ref/ncaa.h"

namespace deskd {
using json = nlohmann::json;
namespace {
// The same score state, as both sources can show it: the state, the scores (from the market's YES
// team's side) and the period when both read one
bool agree(const EspnRef& a, const EspnRef& b) {
  return a.state == b.state && a.yes_score == b.yes_score && a.opp_score == b.opp_score &&
         (!a.period || !b.period || a.period == b.period);
}
}  // namespace

std::optional<EspnRef> merge_score(const EspnRef* espn, const EspnRef* ncaa, std::int64_t now, std::int64_t max_age_ms) {
  if (!espn && !ncaa) return std::nullopt;
  for (const EspnRef* r : {espn, ncaa})   // over, whichever source still lags
    if (r && r->state == "post") return *r;
  auto fresh = [&](const EspnRef* r) { return r && now - r->fetched_ms <= max_age_ms; };
  const EspnRef* pick;
  if (fresh(espn) && fresh(ncaa)) {
    const auto& e = *espn;
    const auto& n = *ncaa;
    if (!agree(e, n)) {
      EspnRef none;
      none.src = "";
      none.why = "ESPN has " + e.state + " " + std::to_string(e.yes_score) + "-" + std::to_string(e.opp_score) + " (period " +
                 std::to_string(e.period) + ") and NCAA " + n.state + " " + std::to_string(n.yes_score) + "-" +
                 std::to_string(n.opp_score) + " (period " + std::to_string(n.period) + ")";
      return none;
    }
    pick = espn;
  } else if (fresh(espn) || fresh(ncaa)) {
    pick = fresh(espn) ? espn : ncaa;
  } else {
    pick = !ncaa ? espn : !espn ? ncaa : ncaa->fetched_ms > espn->fetched_ms ? ncaa : espn;
  }
  EspnRef r = *pick;
  // the state's first sighting, and the poll before it: the other source's when that row, fresh
  // or not, shows the same state and saw it first
  const EspnRef* other = pick == espn ? ncaa : espn;
  if (other && agree(*other, r) && other->first_seen_ms < r.first_seen_ms) {
    r.first_seen_ms = other->first_seen_ms;
    r.prev_poll_ms = other->prev_poll_ms;
  }
  return r;
}

json score_row(const EspnGame& g) {
  return {{"src", g.src}, {"league", g.league}, {"game_id", g.id}, {"state", g.state}, {"period", g.period},
          {"clock", g.clock}, {"home", g.home_abbr}, {"away", g.away_abbr}, {"home_score", g.home_score},
          {"away_score", g.away_score}, {"lines", json::array({g.home_lines, g.away_lines})},
          {"first_seen_ms", g.first_seen_ms}, {"prev_poll_ms", g.prev_poll_ms ? json(g.prev_poll_ms) : json(nullptr)}};
}

ScoreMerge::ScoreMerge(Env env, std::int64_t max_age_ms)
    : env_(std::move(env)), max_age_ms_(max_age_ms), board_(std::make_shared<const EspnBoard>()) {}

std::shared_ptr<const EspnBoard> ScoreMerge::board() const {
  std::lock_guard<std::mutex> lk(m_);
  return board_;
}

void ScoreMerge::step(std::int64_t now) {
  const auto espn = env_.espn ? env_.espn() : nullptr;
  const auto ncaa = env_.ncaa ? env_.ncaa() : nullptr;
  std::set<std::string> markets;
  for (const auto* b : {espn.get(), ncaa.get()})
    if (b) for (const auto& [tk, r] : *b) markets.insert(tk);
  auto find = [](const EspnBoard* b, const std::string& tk) -> const EspnRef* {
    if (!b) return nullptr;
    const auto it = b->find(tk);
    return it == b->end() ? nullptr : &it->second;
  };
  auto board = std::make_shared<EspnBoard>();
  std::map<std::string, int> rows;
  std::map<std::string, std::string> disagree;
  for (const auto& tk : markets) {
    auto r = merge_score(find(espn.get(), tk), find(ncaa.get(), tk), now, max_age_ms_);
    if (!r) continue;
    if (ncaa && ncaa_sport_for(tk) && !r->state.empty()) keep_first(tk, &*r);
    if (r->why.empty()) ++rows[r->src];
    else disagree[tk] = r->why;
    (*board)[tk] = std::move(*r);
  }
  for (auto it = held_.begin(); it != held_.end();) it = ncaa && markets.count(it->first) ? std::next(it) : held_.erase(it);
  std::lock_guard<std::mutex> lk(m_);
  board_ = std::move(board);
  rows_ = std::move(rows);
  disagree_ = std::move(disagree);
}

// The same state as the one held (a period, or linescores, compared only where both rows read
// them): the row takes the held first sighting, and the held state what the row reads that it did
// not. Another state is held from this row on.
void ScoreMerge::keep_first(const std::string& tk, EspnRef* r) {
  auto [it, fresh] = held_.try_emplace(tk);
  Held& h = it->second;
  const bool same = !fresh && h.state == r->state && h.yes == r->yes_score && h.opp == r->opp_score &&
                    (!h.period || !r->period || h.period == r->period) &&
                    (h.yes_lines.empty() || r->yes_lines.empty() || (h.yes_lines == r->yes_lines && h.opp_lines == r->opp_lines));
  if (!same) {
    h = Held{r->state, r->yes_score, r->opp_score, r->period, r->yes_lines, r->opp_lines, r->first_seen_ms, r->prev_poll_ms};
    return;
  }
  if (r->period) h.period = r->period;
  if (!r->yes_lines.empty()) {
    h.yes_lines = r->yes_lines;
    h.opp_lines = r->opp_lines;
  }
  r->first_seen_ms = h.first_seen_ms;
  r->prev_poll_ms = h.prev_poll_ms;
}

json ScoreMerge::status() const {
  std::lock_guard<std::mutex> lk(m_);
  return {{"max_age_ms", max_age_ms_}, {"rows", rows_}, {"disagree", disagree_}};
}

}  // namespace deskd
