#pragma once
// ESPN's public scoreboards for Kalshi's game markets (plan section 5: X1, the college lanes and
// the Manual overlay all read it): the game state (period, clock, score) and ESPN's own win
// probability for the market's YES team, for book.ref.game / espn / espn_ts_ms.
//
//   scoreboard  https://site.api.espn.com/apis/site/v2/sports/{sport}/{league}/scoreboard
//               ?limit=300&dates=YYYYMMDD[&groups=G]       every 20 s per league and date
//   summary     .../{sport}/{league}/summary?event={id}    every 30 s per game in progress
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
  std::string period_label = "Q";    // Q1..Q4, H1/H2, P1..P3; baseball uses ESPN's own detail
};
const std::vector<EspnLeague>& espn_leagues();
const EspnLeague* espn_league_for(const std::string& ticker);

struct EspnGame {
  std::string id, league, state, detail, clock, last_play_id;   // state: pre, in, post
  std::int64_t start_ms = 0, fetched_ms = 0;
  int period = 0, home_score = 0, away_score = 0;
  std::string home_abbr, home_name, away_abbr, away_name;
};
std::vector<EspnGame> parse_scoreboard(const nlohmann::json& body, const std::string& league, std::int64_t now_ms);

struct EspnWp {
  double home = 0, tie = 0;
  std::int64_t play_ms = 0;   // the latest play's wall clock; 0 when ESPN gave none
};
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
};
using EspnBoard = std::map<std::string, EspnRef>;   // by market ticker
// The contract's per and clock for a game ("PRE" with the ET start, "Q3", "OT", "FINAL").
EspnRef espn_ref(const EspnLeague& league, const EspnGame& g, bool yes_home, const std::optional<EspnWp>& wp);

class EspnFeed {
 public:
  struct Options {
    int scoreboard_ms = 20000, summary_ms = 30000, keep_ms = 120000, timeout_ms = 8000;
  };
  // markets: the game markets to cover (the watched ones), read on espn's thread.
  using MarketsFn = std::function<std::vector<kalshi::MarketRow>()>;
  EspnFeed(net::Endpoint endpoint, MarketsFn markets);
  EspnFeed(net::Endpoint endpoint, MarketsFn markets, Options options);
  ~EspnFeed();
  void run(Worker& self);
  void step(std::int64_t now_ms, Worker* self = nullptr);   // one round (tests call it directly)
  std::shared_ptr<const EspnBoard> board() const;
  nlohmann::json status() const;
 private:
  bool get(const std::string& target, nlohmann::json* body, std::string* why, Worker* self);
  net::Endpoint endpoint_;
  MarketsFn markets_;
  Options options_;
  SendGuard guard_;
  std::unique_ptr<net::ConnFactory> factory_;
  std::unique_ptr<net::Conn> conn_;
  std::map<std::string, std::int64_t> fetched_;    // league|date -> last scoreboard attempt
  std::map<std::string, EspnGame> games_;          // by ESPN id
  std::map<std::string, std::int64_t> wp_tried_;   // ESPN id -> last summary attempt
  std::map<std::string, std::optional<EspnWp>> wp_;
  std::map<std::string, EspnMatch> matched_;       // event ticker -> game
  mutable std::mutex m_;                           // board_ and the status counters
  std::shared_ptr<const EspnBoard> board_;
  std::int64_t requests_ = 0, failures_ = 0;
  std::string last_error_;
};

}  // namespace deskd
