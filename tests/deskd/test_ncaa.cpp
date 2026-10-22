// NCAA.com for the college game markets (ref/ncaa.h): its scoreboard file read as ESPN's games are
// (the recorder's fixture), the team match, and the feed against a local stand-in for data.ncaa.com:
// its pacing (a fast series hot at 3 s, else 30 s), each state's first sighting, a date without
// games (404), a failure (503) and the data-only guard. Then ScoreMerge (ref/scores.h): ESPN's and
// NCAA's score for a market by tools/college_gate.py's rule (its Score self-test's cases), and the
// store's scores row.
#include <httplib.h>
#include <signal.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/build_paths.h"
#include "deskd/ref/ncaa.h"
#include "deskd/ref/scores.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
json fixture() {
  std::ifstream f(std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/ncaa_scoreboard.json");
  std::stringstream s;
  s << f.rdbuf();
  return json::parse(s.str());
}
kalshi::MarketRow market(const std::string& ticker, const std::string& event, const std::string& name) {
  kalshi::MarketRow m; m.ticker = ticker; m.event_ticker = event; m.yes_sub_title = name; m.status = "active"; return m;
}
json game(const std::string& id, const std::string& state, const std::string& period, const std::string& clock,
          const std::string& home, int home_score, const std::string& away, int away_score) {
  return {{"game", {{"gameID", id}, {"gameState", state}, {"currentPeriod", period}, {"contestClock", clock},
                    {"startTimeEpoch", "1793919600"},
                    {"home", {{"score", std::to_string(home_score)}, {"names", {{"char6", home}, {"short", home}}}}},
                    {"away", {{"score", std::to_string(away_score)}, {"names", {{"char6", away}, {"short", away}}}}}}}};
}
const std::int64_t kStart = 1793919600000LL;   // the fixture's game in progress: 2026-11-05T23:00Z, 6:00 PM ET
const std::string EV = "KXNCAAWBGAME-26NOV05SCUCONN", SC = EV + "-SC", UCONN = EV + "-UCONN";

void parsing() {
  CHECK(ncaa_sport_for(SC)->league() == "basketball-women/d1");
  CHECK(ncaa_sport_for("KXNCAAWVMATCH-26NOV05UCLAUSC-USC")->league() == "volleyball-women/d1");
  CHECK(ncaa_sport_for("KXNCAAMBGAME-26NOV05DUKEUNC-UNC")->league() == "basketball-men/d1");
  CHECK(!ncaa_sport_for("KXNBAGAME-26NOV05LALBOS-LAL") && !ncaa_sport_for("KXNCAAWBGAMEX-1"));
  const std::int64_t now = kStart + 2700000;
  const auto games = parse_ncaa_scoreboard(fixture(), "basketball-women/d1", now);
  CHECK(games.size() == 3);   // the fourth has no away team
  if (games.size() != 3) return;
  const auto& g = games[0];
  CHECK(g.id == "6305901" && g.src == "NCAA" && g.league == "basketball-women/d1" && g.state == "in" && g.period == 2 &&
        g.detail == "2nd" && g.clock == "04:12" && g.start_ms == kStart && g.fetched_ms == now && g.first_seen_ms == now &&
        g.prev_poll_ms == 0);
  CHECK(g.home_abbr == "UCONN" && g.home_name == "UConn" && g.away_abbr == "S CAR" && g.away_name == "South Carolina" &&
        g.home_score == 34 && g.away_score == 31 && g.home_lines.empty() && g.away_lines.empty() && !g.sb_wp);
  CHECK(games[1].state == "pre" && games[1].period == 0 && games[1].home_score == 0 && games[1].away_abbr == "IOWA" &&
        games[1].start_ms == 1793925000000LL);
  CHECK(games[2].state == "post" && games[2].period == 0 && games[2].home_score == 61 && games[2].away_score == 58);
  // the clock as ESPN's: "04:12" is 252 s; the YES team's side
  const auto ref = espn_ref(*espn_league_for(SC), g, false, std::nullopt);
  CHECK(ref.clock_s == 252 && ref.per == "Q2" && ref.yes_score == 31 && ref.opp_score == 34 && ref.src == "NCAA" && !ref.espn);
  // currentPeriod: an ordinal is its number; a word, or a number that is not one, is unread
  json sb = {{"games", {game("1", "live", "1st", "9:00", "A", 0, "B", 0), game("2", "LIVE", "3", "", "A", 0, "B", 0),
                        game("3", "live", "HALF", "0:00", "A", 0, "B", 0), game("4", "live", "OT", "4:00", "A", 0, "B", 0),
                        game("5", "live", "2OT", "4:00", "A", 0, "B", 0), game("6", "live", "END 1st", "", "A", 0, "B", 0),
                        game("7", "delayed", "", "", "A", 0, "B", 0), game("8", "", "", "", "A", 0, "B", 0),
                        game("9", "Final", "FINAL", "", "A", 0, "B", 0)}}};
  sb["games"][1]["game"]["gameID"] = 802;   // a number
  sb["games"][2]["game"].erase("gameID");
  sb["games"][2]["game"]["url"] = "/game/803";
  const auto p = parse_ncaa_scoreboard(sb, "x", now);
  CHECK(p.size() == 8);   // no state: not a game
  if (p.size() == 8) {
    CHECK(p[0].period == 1 && p[1].period == 3 && p[1].state == "in" && p[1].id == "802");
    CHECK(p[2].period == 0 && p[2].id == "/game/803" && p[3].period == 0 && p[4].period == 0 && p[5].period == 0);
    CHECK(p[6].state == "delayed" && p[7].state == "post");
  }
}

void matching() {
  const auto games = parse_ncaa_scoreboard(fixture(), "basketball-women/d1", kStart);
  KalshiGame k{EV, "KXNCAAWBGAME", "2026-11-05", -1, SC, "SC", "South Carolina", UCONN, "UCONN", "UConn"};
  auto m = match_game(k, games);
  CHECK(m && m->id == "6305901" && !m->a_home);   // market a (SC) is the away team
  KalshiGame pre{"KXNCAAWBGAME-26NOV05IOWADRAKE", "KXNCAAWBGAME", "2026-11-05", -1, "KXNCAAWBGAME-26NOV05IOWADRAKE-DRAKE",
                 "DRAKE", "Drake", "KXNCAAWBGAME-26NOV05IOWADRAKE-IOWA", "IOWA", "Iowa"};
  m = match_game(pre, games);
  CHECK(m && m->id == "6305902" && m->a_home);
}

// The feed against a stand-in for data.ncaa.com, the fixture's file for 2026-11-05.
void feed() {
  const std::int64_t t0 = kStart + 2700000;   // 23:45Z: the first game in its 2nd quarter, Drake-Iowa 45 min off
  std::mutex mu;
  json sb = fixture();
  std::atomic<int> wbb{0}, wvb{0};
  std::atomic<bool> down{false};
  httplib::Server srv;
  srv.Get("/casablanca/scoreboard/basketball-women/d1/2026/11/05/scoreboard.json", [&](const httplib::Request& req, httplib::Response& res) {
    if (down) { res.status = 503; return; }
    ++wbb;
    CHECK(req.get_header_value("User-Agent") == "curl/8.5.0");
    std::lock_guard<std::mutex> lk(mu);
    res.set_content(sb.dump(), "application/json");
  });
  srv.Get("/casablanca/scoreboard/volleyball-women/d1/2026/11/05/scoreboard.json", [&](const httplib::Request&, httplib::Response& res) {
    ++wvb;
    res.set_content(json{{"games", json::array()}}.dump(), "application/json");
  });
  const int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  std::string why;
  auto ep = net::Endpoint::parse("http://127.0.0.1:" + std::to_string(port), &why);
  CHECK(ep.has_value());
  // the in-progress game, a volleyball match the file does not list, and a date with no file (404)
  std::vector<kalshi::MarketRow> watched = {market(SC, EV, "South Carolina"), market(UCONN, EV, "UConn"),
                                            market("KXNCAAWVMATCH-26NOV05UCLAUSC-UCLA", "KXNCAAWVMATCH-26NOV05UCLAUSC", "UCLA"),
                                            market("KXNCAAWVMATCH-26NOV05UCLAUSC-USC", "KXNCAAWVMATCH-26NOV05UCLAUSC", "USC"),
                                            market("KXNCAAWBGAME-26NOV06AAABBB-AAA", "KXNCAAWBGAME-26NOV06AAABBB", "Aaa"),
                                            market("KXNCAAWBGAME-26NOV06AAABBB-BBB", "KXNCAAWBGAME-26NOV06AAABBB", "Bbb"),
                                            market("KXNBAGAME-26NOV05LALBOS-LAL", "KXNBAGAME-26NOV05LALBOS", "Los Angeles L")};
  NcaaFeed::Options o;
  o.fast_series = {"KXNCAAWBGAME"};
  NcaaFeed f(*ep, [&] { return watched; }, o);
  std::vector<EspnGame> told;
  f.set_on_change([&](const EspnGame& g) { told.push_back(g); });
  auto sc = [&] { return f.board()->at(SC); };

  f.step(t0);
  CHECK(wbb == 1 && wvb == 1 && f.board()->size() == 2 && told.size() == 1);
  CHECK(sc().src == "NCAA" && sc().state == "in" && sc().per == "Q2" && sc().clock_s == 252 && sc().yes_score == 31 &&
        sc().opp_score == 34 && sc().first_seen_ms == t0 && sc().fetched_ms == t0 && !sc().yes_home);
  CHECK(f.board()->at(UCONN).yes_home && f.board()->at(UCONN).yes_score == 34);
  // a date without games has no file: no games, not a failure
  CHECK(f.status()["no_games"] == 1 && f.status()["failures"] == 0 && f.status()["error"] == "");
  if (told.size() == 1) CHECK(told[0].src == "NCAA" && told[0].league == "basketball-women/d1" && told[0].id == "6305901");
  {   // the clock runs: a 3 s read for the fast series' date, 30 s for the volleyball one
    std::lock_guard<std::mutex> lk(mu);
    sb["games"][0]["game"]["contestClock"] = "03:58";
  }
  f.step(t0 + 3000);
  CHECK(wbb == 2 && wvb == 1 && sc().first_seen_ms == t0 && sc().fetched_ms == t0 + 3000 && sc().clock == "03:58" && told.size() == 1);
  CHECK(f.status()["hot"] == json::array({"basketball-women/d1|2026-11-05"}) && f.status()["fast_series"] == json::array({"KXNCAAWBGAME"}));
  {   // UConn scores: a new state, first seen at this poll, after the one before
    std::lock_guard<std::mutex> lk(mu);
    sb["games"][0]["game"]["home"]["score"] = "36";
  }
  f.step(t0 + 6000);
  CHECK(wbb == 3 && sc().first_seen_ms == t0 + 6000 && sc().opp_score == 36 && told.size() == 2);
  if (told.size() == 2) CHECK(told[1].home_score == 36 && told[1].first_seen_ms == t0 + 6000 && told[1].prev_poll_ms == t0 + 3000);
  f.step(t0 + 7000);   // paced
  CHECK(wbb == 3);
  // NCAA.com goes away: a failure, what was shown stays two minutes, then nothing
  down = true;
  f.step(t0 + 9000);
  CHECK(wbb == 3 && f.status()["failures"] == 1 && f.status()["error"].get<std::string>().find("HTTP 503") != std::string::npos);
  CHECK(f.board()->size() == 2);
  f.step(t0 + 30000);
  CHECK(wvb == 2);
  f.step(t0 + 6000 + 120001);
  CHECK(f.board()->empty());
  down = false;
  f.step(t0 + 130000);
  CHECK(f.board()->size() == 2 && sc().first_seen_ms == t0 + 6000);   // the same state as before the outage

  // nothing reads hot without a fast series, whatever the game
  NcaaFeed slow(*ep, [&] { return watched; });
  const int before = wbb;
  slow.step(t0);
  slow.step(t0 + 3000);
  CHECK(wbb == before + 1 && slow.status()["hot"].empty());
  slow.step(t0 + 30000);
  CHECK(wbb == before + 2);
  // a game 45 minutes from its start waits 30 s; from 15 minutes before it, it is read hot
  std::vector<kalshi::MarketRow> later = {market("KXNCAAWBGAME-26NOV05IOWADRAKE-IOWA", "KXNCAAWBGAME-26NOV05IOWADRAKE", "Iowa"),
                                          market("KXNCAAWBGAME-26NOV05IOWADRAKE-DRAKE", "KXNCAAWBGAME-26NOV05IOWADRAKE", "Drake")};
  NcaaFeed pre(*ep, [&] { return later; }, o);
  const int b2 = wbb;
  pre.step(t0);
  CHECK(wbb == b2 + 1 && pre.board()->at("KXNCAAWBGAME-26NOV05IOWADRAKE-DRAKE").state == "pre");
  pre.step(t0 + 3000);
  CHECK(wbb == b2 + 1);
  pre.step(t0 + 1800000);   // 15 min before the tip: due at 30 s anyway, and hot from now
  pre.step(t0 + 1803000);
  CHECK(wbb == b2 + 3);

  // a venue host, or any host but data.ncaa.com, is never reachable from this connection
  for (const char* url : {"https://demo-api.kalshi.co", "https://site.api.espn.com"}) {
    auto other = net::Endpoint::parse(url, &why);
    NcaaFeed wrong(*other, [&] { return later; });
    wrong.step(t0);
    const std::string err = wrong.status()["error"];
    CHECK(wrong.board()->empty() && wrong.status()["failures"] == 1 &&
          (err.find("venue host") != std::string::npos || err.find("not an allowed data host") != std::string::npos));
  }
  srv.stop();
  th.join();
}

// One market's ESPN and NCAA rows as the boards have them
EspnRef ref(const std::string& src, std::int64_t fetched, std::int64_t first_seen, int yes, int opp, int period = 1,
            const std::string& state = "in") {
  EspnRef r;
  r.src = src;
  r.state = state;
  r.fetched_ms = fetched;
  r.first_seen_ms = first_seen;
  r.yes_score = yes;
  r.opp_score = opp;
  r.period = period;
  if (src == "ESPN") {   // only ESPN reads linescores
    r.yes_lines = {yes};
    r.opp_lines = {opp};
  }
  return r;
}

// college_gate.py's Score cases (times in seconds there, ms here), and the rest of the rule
void merge() {
  const std::int64_t s = 1000, age = 5 * s;
  // both fresh and agreeing: ESPN's row, the earlier first sighting, ESPN's last poll
  auto e = ref("ESPN", 120 * s, 100 * s, 0, 0), n = ref("NCAA", 119 * s, 110 * s, 0, 0);
  auto r = merge_score(&e, &n, 120 * s, age);
  CHECK(r && r->src == "ESPN" && r->state == "in" && r->first_seen_ms == 100 * s && r->fetched_ms == 120 * s && r->why.empty() &&
        r->yes_lines == std::vector<int>{0});
  n.first_seen_ms = 90 * s;
  CHECK(merge_score(&e, &n, 120 * s, age)->first_seen_ms == 90 * s);
  // ESPN's last poll 10 s ago (an outage): NCAA's, fresh, is used
  e = ref("ESPN", 200 * s, 200 * s, 10, 0);
  n = ref("NCAA", 210 * s, 110 * s, 0, 0);
  r = merge_score(&e, &n, 210 * s, age);
  CHECK(r && r->src == "NCAA" && r->fetched_ms == 210 * s && r->first_seen_ms == 110 * s && r->yes_score == 0);
  // within the max age ESPN is still fresh, and both agree (at the edge, too)
  e = ref("ESPN", 150 * s, 100 * s, 0, 0);
  r = merge_score(&e, &n, 152 * s, age);
  CHECK(r && r->src == "ESPN" && r->fetched_ms == 150 * s);
  CHECK(merge_score(&e, &n, 155 * s, age)->src == "ESPN" && merge_score(&e, &n, 155 * s + 1, age)->src == "NCAA");
  // both fresh and different: no score, the why names both from the market's side
  e = ref("ESPN", 205 * s, 200 * s, 10, 0);
  n = ref("NCAA", 205 * s, 110 * s, 0, 0);
  r = merge_score(&e, &n, 205 * s, age);
  CHECK(r && r->state.empty() && r->src.empty() && r->first_seen_ms == 0 && r->fetched_ms == 0 &&
        r->why == "ESPN has in 10-0 (period 1) and NCAA in 0-0 (period 1)");
  n = ref("NCAA", 205 * s, 110 * s, 10, 2);              // the other team's score
  CHECK(merge_score(&e, &n, 205 * s, age)->why == "ESPN has in 10-0 (period 1) and NCAA in 10-2 (period 1)");
  n = ref("NCAA", 205 * s, 110 * s, 10, 0, 1, "post");   // the state
  CHECK(merge_score(&e, &n, 205 * s, age)->why == "ESPN has in 10-0 (period 1) and NCAA post 10-0 (period 1)");
  n = ref("NCAA", 205 * s, 110 * s, 10, 0, 2);           // the period, both read
  CHECK(merge_score(&e, &n, 205 * s, age)->why == "ESPN has in 10-0 (period 1) and NCAA in 10-0 (period 2)");
  n = ref("NCAA", 205 * s, 110 * s, 10, 0, 0);           // NCAA's "HALF": no period read, so no difference
  CHECK(merge_score(&e, &n, 205 * s, age)->src == "ESPN");
  e.period = 0;
  n.period = 2;
  CHECK(merge_score(&e, &n, 205 * s, age)->why.empty());
  // one source, old: its row, which the age then pulls (SCORE_OLD)
  e = ref("ESPN", 201 * s, 200 * s, 10, 0);
  r = merge_score(&e, nullptr, 300 * s, age);
  CHECK(r && r->fetched_ms == 201 * s && r->state == "in" && r->src == "ESPN");
  r = merge_score(nullptr, &n, 300 * s, age);
  CHECK(r && r->src == "NCAA");
  // neither fresh: the fresher one's row; ESPN's on a tie
  n = ref("NCAA", 202 * s, 150 * s, 0, 0);
  CHECK(merge_score(&e, &n, 300 * s, age)->src == "NCAA");
  n.fetched_ms = 201 * s;
  CHECK(merge_score(&e, &n, 300 * s, age)->src == "ESPN");
  CHECK(!merge_score(nullptr, nullptr, 300 * s, age));

  // The merged board, from both feeds' boards: ESPN alone (another league) as it is; a college
  // game from both; a market only NCAA shows; the sources at odds named in status
  EspnBoard espn, ncaa;
  espn["KXNBAGAME-26NOV05LALBOS-LAL"] = ref("ESPN", 10 * s, 1 * s, 50, 48, 3);   // old, and alone: unchanged
  espn[SC] = ref("ESPN", 100 * s, 90 * s, 31, 34, 2);
  ncaa[SC] = ref("NCAA", 99 * s, 80 * s, 31, 34, 2);
  espn[UCONN] = ref("ESPN", 100 * s, 90 * s, 34, 31, 2);
  ncaa[UCONN] = ref("NCAA", 99 * s, 80 * s, 36, 31, 2);
  ncaa["KXNCAAWBGAME-26NOV05IOWADRAKE-IOWA"] = ref("NCAA", 99 * s, 80 * s, 0, 0, 0, "pre");
  bool off = false;
  ScoreMerge::Env env;
  env.espn = [&] { return std::make_shared<const EspnBoard>(espn); };
  env.ncaa = [&] { return off ? nullptr : std::make_shared<const EspnBoard>(ncaa); };
  ScoreMerge sm(env);
  CHECK(sm.board()->empty() && sm.status()["max_age_ms"] == ScoreMerge::kMaxAgeMs);
  sm.step(100 * s);
  auto b = sm.board();
  CHECK(b->size() == 4);
  CHECK(b->at("KXNBAGAME-26NOV05LALBOS-LAL").fetched_ms == 10 * s && b->at("KXNBAGAME-26NOV05LALBOS-LAL").src == "ESPN");
  CHECK(b->at(SC).src == "ESPN" && b->at(SC).first_seen_ms == 80 * s && b->at(SC).fetched_ms == 100 * s);
  CHECK(b->at(UCONN).state.empty() && b->at(UCONN).why == "ESPN has in 34-31 (period 2) and NCAA in 36-31 (period 2)");
  CHECK(b->at("KXNCAAWBGAME-26NOV05IOWADRAKE-IOWA").src == "NCAA");
  const auto st = sm.status();
  CHECK(st["rows"]["ESPN"] == 2 && st["rows"]["NCAA"] == 1 && st["disagree"].size() == 1 && st["disagree"][UCONN] == b->at(UCONN).why);
  off = true;   // NCAA.com off: ESPN's board as it is
  sm.step(101 * s);
  b = sm.board();
  CHECK(b->size() == 3 && b->at(UCONN).src == "ESPN" && b->at(UCONN).yes_score == 34 && b->at(SC).first_seen_ms == 90 * s &&
        sm.status()["disagree"].empty());
}

// The store's scores row for a game state: both sides' linescores, no last poll before it as null
void rows() {
  EspnGame g;
  g.src = "NCAA";
  g.league = "basketball-women/d1";
  g.id = "6305901";
  g.state = "in";
  g.period = 2;
  g.clock = "04:12";
  g.home_abbr = "UCONN";
  g.away_abbr = "S CAR";
  g.home_score = 34;
  g.away_score = 31;
  g.first_seen_ms = 5000;
  json row = score_row(g);
  CHECK(row == json({{"src", "NCAA"}, {"league", "basketball-women/d1"}, {"game_id", "6305901"}, {"state", "in"}, {"period", 2},
                     {"clock", "04:12"}, {"home", "UCONN"}, {"away", "S CAR"}, {"home_score", 34}, {"away_score", 31},
                     {"lines", json::array({json::array(), json::array()})}, {"first_seen_ms", 5000}, {"prev_poll_ms", nullptr}}));
  g.home_lines = {18, 16};
  g.away_lines = {15, 16};
  g.prev_poll_ms = 2000;
  row = score_row(g);
  CHECK(row["lines"] == json::parse("[[18,16],[15,16]]") && row["prev_poll_ms"] == 2000);
}
}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  parsing(); matching(); feed(); merge(); rows();
  return finish("deskd-ncaa");
}
