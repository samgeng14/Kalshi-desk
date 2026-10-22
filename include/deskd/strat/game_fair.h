#pragma once
// The game markets' pregame price and in-game fair (plan 5(d) and 5(f); decisions 13: one
// GameFair for WCBB, NBA and volleyball), on a series-to-model map: the WCBB and NBA logistic
// models (strat/bball_model.h) and the volleyball Markov model (strat/volley_model.h). The
// Manual ladders show its fair (book.ref.fair) and the game lanes (C1, N1) trade on it.
//
// p0. A game's two markets each pay on one team, so market A's YES has an effective book over
// both: bid max(bid_A, 1 - ask_B), ask min(ask_A, 1 - bid_B), a stale or absent book left out. Its
// mid is A's pregame price, captured while the score says "pre", before the scheduled start and at
// an effective spread of 15c or less, as the models were trained ("the Kalshi mid at the
// scheduled tip"). ESPN says "in" 40-60 s after the real tip, so "pre" alone would let in-play
// mids in and count the opening run twice (in p0 and in the score). The last capture before the
// start is kept and an in-play mid never replaces it; a game first seen after its start has no p0
// and so no fair. B's p0 is 1 - A's. With persist() the captures outlive a restart: the file
// (data_dir/game_p0.json) is written at most once a minute, and a capture over 12 h old is dropped.
//
// The fair, while the game is "in" and has p0: the model's P for the home team's market, from the
// home team's side (the NBA's model is trained so), and 1 - P for the other market, so the two
// always sum to 1. Basketball reads ESPN's period and clock (bball_live_p); an NBA model without
// the ot feature has no fair in overtime (its clock would be a regulation clock to the model);
// WCBB's overtime is t_left 0, as its model was trained. Volleyball reads the sets and points from
// the linescores (volley_state); a set whose points were not read gives a P with the set at 0-0,
// which the board keeps (the lanes decide on it) but the ladder does not show.
//
// The score is ESPN's board, or an NCAA board's (ref-ncaa, C3) for a market ESPN does not show. A
// model is used only on its own sport's series: a basketball model on its league's series, the
// volleyball model on a volleyball series; any other pairing is refused at construction and said
// in status.
//
// Threading: step() runs on strat; board() and status() are read from any thread.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>
#include "deskd/bus/units.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/ref/espn.h"
#include "deskd/strat/bball_model.h"
#include "deskd/strat/volley_model.h"

namespace deskd {
namespace rest { struct Snapshot; }

using GameModel = std::variant<BballModel, VolleyModel>;

struct GameFairRow {
  std::string event, model;          // model: "wcbb_logit", "nba_logit", "volley_markov"
  std::optional<double> p0;          // this market's pregame YES price
  std::optional<double> p;           // P(this market's YES team wins)
  bool points = true;                // volleyball: false when p takes the current set at 0-0 (points unread)
  bool ot = false;                   // basketball in overtime
  std::string state, src;            // the score's state (pre, in, post) and source (ESPN, NCAA)
  std::int64_t first_seen_ms = 0, fetched_ms = 0;   // the score state's first sighting, its last poll
  std::string why;                   // why no p, or why it is not shown
  bool shown() const { return p.has_value() && points; }   // what the ladder shows
};
using GameBoard = std::map<std::string, GameFairRow>;   // by market ticker

// Market a's effective YES book over both of a game's markets (b pays on the other team); stale
// books are left out, and nullopt when no bid or no ask is left
struct EffBook {
  Px bid = 0, ask = 0;
};
std::optional<EffBook> effective_book(const MdBook* a, const MdBook* b);

class GameFair {
 public:
  static constexpr Px kMaxSpread = 1500;                          // 15c
  static constexpr std::int64_t kKeepMs = 12 * 3600000LL;        // a capture's life in the file
  static constexpr std::int64_t kSaveEveryMs = 60000;
  struct Env {
    std::function<std::shared_ptr<const rest::Snapshot>()> snapshot;   // the listing: each market's event
    std::function<std::shared_ptr<const std::map<std::string, MdBook>>()> books;
    std::function<std::shared_ptr<const EspnBoard>()> espn;
    std::function<std::shared_ptr<const EspnBoard>()> ncaa;   // empty: ESPN alone (until C3's NcaaFeed)
  };
  GameFair(Env env, std::map<std::string, GameModel> by_series);
  // Keep the captures in this file (call it before the first step()): read now (absent: none yet;
  // unreadable: said in status and started from none), written by step() at most once a minute
  // after a change.
  void persist(const std::string& path);
  void step(std::int64_t now_ms);
  std::shared_ptr<const GameBoard> board() const;
  nlohmann::json status() const;

 private:
  struct Capture {
    std::string ticker;   // the market whose YES price p0 is
    double p0 = 0;
    std::int64_t ms = 0;  // when it was captured
  };
  GameFairRow row(const std::string& event, const std::string& ticker, const EspnRef& r, const GameModel& m) const;
  void save(std::int64_t now_ms);
  Env env_;
  std::map<std::string, GameModel> models_;    // by series
  std::map<std::string, std::string> refused_; // series -> why its model is not used
  mutable std::mutex m_;                       // board_; captures_ and the file's names for status()
  std::shared_ptr<const GameBoard> board_;
  std::map<std::string, Capture> captures_;    // by event ticker
  std::string path_, path_error_;
  bool dirty_ = false;                         // step()'s thread only
  std::int64_t saved_ms_ = 0;
};

}  // namespace deskd
