#pragma once
// ESPN's public scoreboards for Kalshi's game markets (plan section 5: X1, the college lanes and
// the Manual overlay all read it): the game state (period, clock, score) and ESPN's own win
// probability for the market's YES team, for book.ref.game / espn / espn_ts_ms.
//
//   scoreboard  https://site.api.espn.com/apis/site/v2/sports/{sport}/{league}/scoreboard
//               ?limit=300&dates=YYYYMMDD[&groups=G]       every 20 s per league and date
//   summary     .../{sport}/{league}/summary?event={id}    every 30 s per game in progress
//
// A fast series (Options::fast_series: the series a lane's engine trades, none by default) is read
// hot while one of its matched games is in progress or starts within 15 minutes: its scoreboard
// every 3 s and its summaries every 10 s (plan section 2's "poll reads scores every 3-5 s with
// first-seen times"). Every game carries the time this process first saw its state (state,
// period, scores and linescores; never the clock) and the last poll that showed it, both on our
// own clock, which is the only honest one for "what was knowable then" (plan 5(e)).
//
// Ported from the old desk's SportsLive (src/strategies/sports_live.cpp): its league table, its
// team matching (both teams need their own evidence, ticker codes against ESPN's abbreviations
// and names token by token, so "Chicago C" is the Cubs and never the White Sox), and its rule
// for ESPN's win probability: only when the summary's game state is exactly the scoreboard's
// and its chart's last point is the latest play. ESPN runs about 41.5 s behind the venue (plan
// section 5), so espn_ts_ms is the play's own wall clock and the page shows the age.
//
// Nothing is shown from a scoreboard older than 2 minutes. The connection is a public-data one
// (net/guard.h SendGuard::data_hosts): it can reach site.api.espn.com and nothing else.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/units.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/conn.h"
#include "deskd/net/guard.h"

namespace deskd {
class Worker;

struct EspnLeague {
  std::string series, sport, league;
  std::vector<std::string> groups;   // ESPN's group filter (college: all divisions)
  int periods = 4;
  std::string period_label = "Q";    // Q1..Q4, H1/H2, P1..P3, S1..S5; baseball uses ESPN's own detail
};
const std::vector<EspnLeague>& espn_leagues();
const EspnLeague* espn_league_for(const std::string& ticker);

struct EspnWp {
  double home = 0, tie = 0;
  std::int64_t play_ms = 0;   // the latest play's wall clock; 0 when ESPN gave none
  std::string play = "";      // that play's id
  std::int64_t seen_ms = 0;   // the poll that first showed this play's probability (the feed sets it)
};

struct EspnGame {
  std::string id, league, state, detail, clock, last_play_id;   // state: pre, in, post
  std::string src = "ESPN";                                     // the feed it came from
  // fetched_ms is the last poll that showed the game; first_seen_ms the poll that first showed
  // its present state (same_score_state), prev_poll_ms the last poll before that (0: none), so
  // the change happened between the two. A parse sees everything for the first time; the feed
  // carries first_seen_ms and prev_poll_ms across polls.
  std::int64_t start_ms = 0, fetched_ms = 0, first_seen_ms = 0, prev_poll_ms = 0;
  int period = 0, home_score = 0, away_score = 0;
  std::string home_abbr, home_name, away_abbr, away_name;
  std::vector<int> home_lines, away_lines;   // points by period (volleyball: by set); -1 unread
  int possession = 0;                        // +1 home has the ball, -1 away, 0 not given
  std::optional<EspnWp> sb_wp;               // the scoreboard's own last-play probability (TO CONFIRM)
};
std::vector<EspnGame> parse_scoreboard(const nlohmann::json& body, const std::string& league, std::int64_t now_ms);
// The same game state: state, period, scores and linescores. The clock alone is no change.
bool same_score_state(const EspnGame& a, const EspnGame& b);

// ESPN's win probability, only if the summary describes exactly this game state.
std::optional<EspnWp> parse_summary_wp(const nlohmann::json& body, const EspnGame& game);

// One Kalshi game: its two markets, each paying on one team.
struct KalshiGame {
  std::string event_ticker, series, date;   // date: YYYY-MM-DD (ET), from the event ticker
  int start_min = -1;                       // ET minutes after midnight when the ticker says
  std::string ticker_a, code_a, name_a, ticker_b, code_b, name_b;
};
std::string kalshi_event_date(const std::string& event_ticker);   // "" when not a dated event
int kalshi_event_start_minutes(const std::string& event_ticker);  // -1 when none
double team_name_overlap(const std::string& a, const std::string& b);
// The ESPN game this Kalshi game is, and whether its market a is the home team; nullopt when no
// game has evidence for both teams, or two games are equally good.
struct EspnMatch { std::string id; bool a_home = false; };
std::optional<EspnMatch> match_game(const KalshiGame& k, const std::vector<EspnGame>& games);

// What the ladder shows for one market.
struct EspnRef {
  std::string per, clock, score;   // contract book.ref.game
  std::optional<Px> espn;          // ESPN's win probability for this market's YES team
  std::int64_t espn_ts_ms = 0;     // 0: ESPN gave no play time
  std::int64_t fetched_ms = 0;
  // the game state as numbers, for a model's fair (strat/wcbb_model.h): ESPN's state (pre, in,
  // post), the period, the clock in seconds (-1 unread), and the scores from the YES team's side
  std::string state;
  int period = 0, yes_score = 0, opp_score = 0;
  double clock_s = -1;
  std::int64_t start_ms = 0;   // ESPN's scheduled tip (0: not given)
  // From the YES team's side as well: its linescores, who has the ball (+1 the YES team, -1 the
  // other, 0 not given) and the scoreboard's last-play probability.
  bool yes_home = false;
  std::vector<int> yes_lines, opp_lines;
  int possession = 0;
  std::optional<Px> sb_wp;
  // our own clock: the poll that first showed this state (EspnGame::first_seen_ms), the last poll
  // before it (EspnGame::prev_poll_ms; 0: none, the feed had only begun to read the game) and the
  // poll that first showed espn's play (0: no espn); fetched_ms is the last poll
  std::int64_t first_seen_ms = 0, prev_poll_ms = 0, wp_seen_ms = 0;
  std::string src = "ESPN";
  std::string why;   // ref/scores.h's merged board: why there is no score (the state is then "")
};
using EspnBoard = std::map<std::string, EspnRef>;   // by market ticker
// The contract's per and clock for a game ("PRE" with the ET start, "Q3", "OT", "FINAL").
EspnRef espn_ref(const EspnLeague& league, const EspnGame& g, bool yes_home, const std::optional<EspnWp>& wp);

class EspnFeed {
 public:
  struct Options {
    int scoreboard_ms = 20000, summary_ms = 30000, keep_ms = 120000, timeout_ms = 8000;
    // the hot cadence, for fast_series only: from hot_pre_ms before a matched game's start (or
    // while its tip is late) until it ends
    std::set<std::string> fast_series;   // Kalshi series ("KXNBAGAME"); empty: none is hot
    int hot_scoreboard_ms = 3000, hot_summary_ms = 10000;
    std::int64_t hot_pre_ms = 900000;
    int round_ms = 500;                  // run()'s round; step() paces each read itself
  };
  // markets: the game markets to cover (the watched ones), read on espn's thread.
  using MarketsFn = std::function<std::vector<kalshi::MarketRow>()>;
  // Called on espn's thread once for each new state of a game matched to a watched market (its
  // first sighting included), with first_seen_ms and prev_poll_ms set. It must not block.
  using ScoreChangeFn = std::function<void(const EspnGame&)>;
  EspnFeed(net::Endpoint endpoint, MarketsFn markets);
  EspnFeed(net::Endpoint endpoint, MarketsFn markets, Options options);
  ~EspnFeed();
  void set_on_change(ScoreChangeFn fn);   // before run()
  void run(Worker& self);
  void step(std::int64_t now_ms, Worker* self = nullptr);   // one round (tests call it directly)
  std::shared_ptr<const EspnBoard> board() const;
  nlohmann::json status() const;
 private:
  bool get(const std::string& target, nlohmann::json* body, std::string* why, Worker* self);
  bool hot(const EspnGame& g, std::int64_t now) const;
  net::Endpoint endpoint_;
  MarketsFn markets_;
  Options options_;
  ScoreChangeFn on_change_;
  SendGuard guard_;
  std::unique_ptr<net::ConnFactory> factory_;
  std::unique_ptr<net::Conn> conn_;
  std::map<std::string, std::int64_t> fetched_;    // league|date -> last scoreboard attempt
  std::map<std::string, EspnGame> games_;          // by ESPN id
  std::map<std::string, std::string> from_;        // ESPN id -> the league|date that last listed it
  std::map<std::string, std::int64_t> parsed_;     // league -> scoreboards parsed, so a failed match waits for news
  std::map<std::string, std::int64_t> match_tried_;   // event ticker -> its league's parsed_ at its last try
  std::map<std::string, std::int64_t> wp_tried_;   // ESPN id -> last summary attempt
  std::map<std::string, std::optional<EspnWp>> wp_;
  std::map<std::string, EspnWp> wp_play_;          // ESPN id -> the latest play seen, kept through misses
  std::map<std::string, EspnMatch> matched_;       // event ticker -> game
  std::map<std::string, std::int64_t> reported_;   // ESPN id -> the first_seen_ms on_change last had
  mutable std::mutex m_;                           // board_ and the status fields
  std::shared_ptr<const EspnBoard> board_;
  std::int64_t requests_ = 0, failures_ = 0;
  std::string last_error_;
  std::set<std::string> hot_;                      // league|date read hot at the last round
  std::vector<std::int64_t> lag_;                  // the latest plays' seen_ms - play_ms, oldest first
};

}  // namespace deskd
