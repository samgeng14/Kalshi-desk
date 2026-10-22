#include "deskd/ref/scores.h"

#include <algorithm>
#include <set>

namespace deskd {
using json = nlohmann::json;

std::optional<EspnRef> merge_score(const EspnRef* espn, const EspnRef* ncaa, std::int64_t now, std::int64_t max_age_ms) {
  if (!espn && !ncaa) return std::nullopt;
  auto fresh = [&](const EspnRef* r) { return r && now - r->fetched_ms <= max_age_ms; };
  if (fresh(espn) && fresh(ncaa)) {
    const auto& e = *espn;
    const auto& n = *ncaa;
    if (e.state != n.state || e.yes_score != n.yes_score || e.opp_score != n.opp_score ||
        (e.period && n.period && e.period != n.period)) {
      EspnRef none;
      none.src = "";
      none.why = "ESPN has " + e.state + " " + std::to_string(e.yes_score) + "-" + std::to_string(e.opp_score) + " (period " +
                 std::to_string(e.period) + ") and NCAA " + n.state + " " + std::to_string(n.yes_score) + "-" +
                 std::to_string(n.opp_score) + " (period " + std::to_string(n.period) + ")";
      return none;
    }
    EspnRef r = e;
    r.first_seen_ms = std::min(e.first_seen_ms, n.first_seen_ms);
    return r;
  }
  if (fresh(espn)) return *espn;
  if (fresh(ncaa)) return *ncaa;
  if (!ncaa) return *espn;
  if (!espn) return *ncaa;
  return ncaa->fetched_ms > espn->fetched_ms ? *ncaa : *espn;
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
    if (r->why.empty()) ++rows[r->src];
    else disagree[tk] = r->why;
    (*board)[tk] = std::move(*r);
  }
  std::lock_guard<std::mutex> lk(m_);
  board_ = std::move(board);
  rows_ = std::move(rows);
  disagree_ = std::move(disagree);
}

json ScoreMerge::status() const {
  std::lock_guard<std::mutex> lk(m_);
  return {{"max_age_ms", max_age_ms_}, {"rows", rows_}, {"disagree", disagree_}};
}

}  // namespace deskd
