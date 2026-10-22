#pragma once
// One score for each game market, from ESPN's board (ref/espn.h) and NCAA.com's (ref/ncaa.h):
// plan 5(d)'s "a score feed at most 5 s old", by the rule tools/college_gate.py measures (its
// Score class), so the gate measures what the lane trades. GameFair's fair and each TakeEngine's
// score read this board, never a feed's own.
//
// For one market, of the sources that show it:
//   fresh     a source whose last poll (fetched_ms) is at most max_age_ms old
//   both      fresh: they must agree on the state, the scores (from the market's YES team's side)
//             and the period when both read one (0 is unread: NCAA.com's "HALF" or "OT"). Then
//             ESPN's row, which alone has linescores and a win probability, with the earlier of
//             the two first sightings (ESPN's last poll stays its age). When they differ there
//             is no score: a row with an empty state and a why naming both ("ESPN has in 10-0
//             (period 1) and NCAA in 0-0 (period 1)"), so the lanes pull NO_SCORE and the fair
//             says why
//   one       fresh: its row
//   neither   fresh: the fresher one's row (ESPN's on a tie), which the lanes' score age pulls
// A market neither shows has no row. ESPN alone (NCAA.com off, or a series it has no scoreboard
// for) is ESPN's board unchanged.
//
// Threading: step() runs on strat; board() and status() are read from any thread.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include "deskd/ref/espn.h"

namespace deskd {

// The rule above for one market; nullopt when neither source shows it.
std::optional<EspnRef> merge_score(const EspnRef* espn, const EspnRef* ncaa, std::int64_t now_ms, std::int64_t max_age_ms);

// The store's "scores" row (store/store.h) for a game's state as first seen: src, league,
// game_id, state, period, clock, home, away, home_score, away_score, lines ([home, away]),
// first_seen_ms, prev_poll_ms (null when none)
nlohmann::json score_row(const EspnGame& g);

class ScoreMerge {
 public:
  static constexpr std::int64_t kMaxAgeMs = 5000;   // plan 5(d); C1's max_score_age_ms
  struct Env {
    std::function<std::shared_ptr<const EspnBoard>()> espn, ncaa;   // either empty: that source is off
  };
  explicit ScoreMerge(Env env, std::int64_t max_age_ms = kMaxAgeMs);
  void step(std::int64_t now_ms);
  std::shared_ptr<const EspnBoard> board() const;
  nlohmann::json status() const;   // the max age, the rows by source, and each market whose sources disagree

 private:
  Env env_;
  std::int64_t max_age_ms_;
  mutable std::mutex m_;   // board_ and the status fields
  std::shared_ptr<const EspnBoard> board_;
  std::map<std::string, int> rows_;              // by source
  std::map<std::string, std::string> disagree_;  // market -> why
};

}  // namespace deskd
