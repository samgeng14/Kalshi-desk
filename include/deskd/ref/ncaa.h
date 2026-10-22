#pragma once
// NCAA.com's public scoreboards for the college game markets (plan 5(d): "a score feed at most 5 s
// old"; plan section 7 records the same files): a second source beside ESPN's (ref/espn.h), so a
// college lane can tell ESPN being slow or wrong from the game standing still. ref/scores.h
// merges the two for each market.
//
//   scoreboard  https://data.ncaa.com/casablanca/scoreboard/{sport}/{division}/YYYY/MM/DD/scoreboard.json
//               every 30 s per sport and date; a fast series' every 3 s while one of its matched
//               games is in progress or starts within 15 minutes (as EspnFeed's)
//
// The file is NCAA.com's own and undocumented (the one henrygd/ncaa-api reads; the recorder's
// fixture, tests/deskd/fixtures/ncaa_scoreboard.json, is hand-built in its shape: TO CONFIRM on
// the VM's recorded ncaa_scores rows). A date without games has no file: a 404 is no games, not a
// failure. Its games are read as ESPN's are (parse_ncaa_scoreboard, as recorder/scores.py
// ncaa_games and tools/college_gate.py read them): gameState pre / live / final is ESPN's pre /
// in / post (another state is kept as written); currentPeriod's number ("2nd" is 2; "HALF", "OT",
// "2OT" and anything else not an ordinal is 0, unread, the cases deskd-ncaa's periods() pins and
// college_gate.py's period_of must give too); contestClock as ESPN's clock; names.char6 as
// the abbreviation and names.short as the name, for match_game; no linescores, so no volleyball
// points. Each game carries its first-seen and last-poll times on our own clock, as ESPN's do.
//
// The connection is a public-data one (net/guard.h SendGuard::data_hosts): it can reach
// data.ncaa.com and nothing else, and has its own thread (ref-ncaa), so a source that hangs
// delays only itself. Nothing is shown from a scoreboard older than 2 minutes.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/net/conn.h"
#include "deskd/net/guard.h"
#include "deskd/ref/espn.h"

namespace deskd {
class Worker;

// The NCAA.com scoreboard of each Kalshi college series (recorder/config.py NCAA_SPORTS)
struct NcaaSport {
  std::string series, sport, division;   // "KXNCAAWBGAME", "basketball-women", "d1"
  std::string league() const { return sport + "/" + division; }   // the games' league: "basketball-women/d1"
};
const std::vector<NcaaSport>& ncaa_sports();
const NcaaSport* ncaa_sport_for(const std::string& ticker);

// A scoreboard.json's games ({"games": [{"game": {...}}]}) as EspnGames with src "NCAA"; a game
// without an id, a state or both teams is left out.
std::vector<EspnGame> parse_ncaa_scoreboard(const nlohmann::json& body, const std::string& league, std::int64_t now_ms);

class NcaaFeed {
 public:
  struct Options {
    int scoreboard_ms = 30000, keep_ms = 120000, timeout_ms = 8000;
    // the hot cadence, for fast_series only (EspnFeed::Options' rule)
    std::set<std::string> fast_series;   // Kalshi series; empty: none is hot
    int hot_scoreboard_ms = 3000;
    std::int64_t hot_pre_ms = 900000;
    int round_ms = 500;
  };
  // markets: the game markets to cover (the watched ones), read on ref-ncaa's thread; on_change as
  // EspnFeed's, with src "NCAA"
  NcaaFeed(net::Endpoint endpoint, EspnFeed::MarketsFn markets);
  NcaaFeed(net::Endpoint endpoint, EspnFeed::MarketsFn markets, Options options);
  ~NcaaFeed();
  void set_on_change(EspnFeed::ScoreChangeFn fn);   // before run()
  void run(Worker& self);
  void step(std::int64_t now_ms, Worker* self = nullptr);   // one round (tests call it directly)
  std::shared_ptr<const EspnBoard> board() const;           // by market ticker, src "NCAA"
  nlohmann::json status() const;
 private:
  bool get(const std::string& target, nlohmann::json* body, std::string* why, Worker* self);
  bool hot(const EspnGame& g, std::int64_t now) const;
  net::Endpoint endpoint_;
  EspnFeed::MarketsFn markets_;
  Options options_;
  EspnFeed::ScoreChangeFn on_change_;
  SendGuard guard_;
  std::unique_ptr<net::ConnFactory> factory_;
  std::unique_ptr<net::Conn> conn_;
  std::map<std::string, std::int64_t> fetched_;       // league|date -> last scoreboard attempt
  std::map<std::string, EspnGame> games_;             // by league|id
  std::map<std::string, std::string> from_;           // league|id -> the league|date that last listed it
  std::map<std::string, std::int64_t> parsed_;        // league -> scoreboards read, so a failed match waits for news
  std::map<std::string, std::int64_t> match_tried_;   // event ticker -> its league's parsed_ at its last try
  std::map<std::string, EspnMatch> matched_;          // event ticker -> game (its id within the series' league)
  std::map<std::string, std::int64_t> reported_;      // league|id -> the first_seen_ms on_change last had
  mutable std::mutex m_;                              // board_ and the status fields
  std::shared_ptr<const EspnBoard> board_;
  std::int64_t requests_ = 0, failures_ = 0, no_games_ = 0;   // no_games_: a date's 404
  std::string last_error_;
  std::set<std::string> hot_;                         // league|date read hot at the last round
};

}  // namespace deskd
