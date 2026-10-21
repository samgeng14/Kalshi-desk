// ESPN for Kalshi's game markets (ref/espn.h): parsing, the team match, the win-probability
// rule, the contract's game fields, and the feed against a local stand-in for ESPN.
#include <httplib.h>
#include <signal.h>
#include <atomic>
#include <thread>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/ref/espn.h"
#include "deskd/web/contract.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
json team(const std::string& ha, const std::string& abbr, const std::string& name, int score) {
  return {{"homeAway", ha}, {"score", std::to_string(score)}, {"team", {{"abbreviation", abbr}, {"displayName", name}}}};
}
json event(const std::string& id, const std::string& date, const std::string& state, int period, const std::string& clock,
           json away, json home, const std::string& last_play = "") {
  json c = {{"status", {{"period", period}, {"displayClock", clock}, {"type", {{"state", state}, {"shortDetail", "detail"}}}}},
            {"competitors", {away, home}}};
  if (!last_play.empty()) c["situation"] = {{"lastPlay", {{"id", last_play}}}};
  return {{"id", id}, {"date", date}, {"competitions", {c}}};
}
json summary(const std::string& id, int period, const std::string& clock, int away, int home, double wp_home,
             const std::string& play, const std::string& wallclock) {
  return {{"header", {{"id", id}, {"competitions", {{{"status", {{"period", period}, {"displayClock", clock}, {"type", {{"state", "in"}}}}},
                                                     {"competitors", {{{"homeAway", "away"}, {"score", std::to_string(away)}},
                                                                      {{"homeAway", "home"}, {"score", std::to_string(home)}}}}}}}}},
          {"winprobability", {{{"playId", "old"}, {"homeWinPercentage", 0.5}}, {{"playId", play}, {"homeWinPercentage", wp_home}, {"tiePercentage", 0.0}}}},
          {"plays", {{{"id", "old"}}, {{"id", play}, {"wallclock", wallclock}}}}};
}
kalshi::MarketRow market(const std::string& ticker, const std::string& event, const std::string& name) {
  kalshi::MarketRow m; m.ticker = ticker; m.event_ticker = event; m.yes_sub_title = name; m.status = "active"; return m;
}

void parsing() {
  CHECK(kalshi_event_date("KXNBAGAME-26OCT10LALBOS") == "2026-10-10");
  CHECK(kalshi_event_date("KXNBAGAME-26XYZ10LALBOS").empty() && kalshi_event_date("KXNBAGAME").empty());
  CHECK(kalshi_event_start_minutes("KXMLBGAME-26OCT101905NYYBOS") == 19 * 60 + 5);
  CHECK(kalshi_event_start_minutes("KXNBAGAME-26OCT10LALBOS") == -1);
  // "Chicago C" is the Cubs, never the White Sox (review 2026-09-23)
  CHECK(team_name_overlap("Chicago C", "Chicago Cubs") == 1.0);
  CHECK(team_name_overlap("Chicago C", "Chicago White Sox") < 1.0);
  CHECK(team_name_overlap("LA Rams", "Los Angeles Rams") == 1.0 && team_name_overlap("A's", "Athletics") == 1.0);
  CHECK(espn_league_for("KXNCAAWBGAME-26NOV05UCONNSC-UCONN")->league == "womens-college-basketball");
  CHECK(!espn_league_for("KXNBAGAMEX-1") && !espn_league_for("KXHIGHNY-26OCT10-B70"));
  const auto now = now_ms();
  json sb = {{"events", {event("401", "2026-10-10T23:30Z", "in", 3, "4:12", team("away", "LAL", "Los Angeles Lakers", 70),
                               team("home", "BOS", "Boston Celtics", 74), "p9"),
                         event("402", "2026-10-11T00:00Z", "pre", 0, "0:00", team("away", "NY", "New York Knicks", 0),
                               team("home", "MIA", "Miami Heat", 0)),
                         {{"id", "403"}}}}};   // no competitions: skipped
  const auto games = parse_scoreboard(sb, "nba", now);
  CHECK(games.size() == 2);
  if (games.size() != 2) return;
  const auto& g = games[0];
  CHECK(g.id == "401" && g.state == "in" && g.period == 3 && g.clock == "4:12" && g.home_abbr == "BOS" &&
        g.away_score == 70 && g.home_score == 74 && g.last_play_id == "p9" && g.fetched_ms == now);
  CHECK(g.start_ms == 1791675000000LL);   // 2026-10-10T23:30Z
  // win probability: only for exactly this state and its latest play
  CHECK(parse_summary_wp(summary("401", 3, "4:12", 70, 74, 0.71, "p9", "2026-10-10T23:58:01Z"), g)->home == 0.71);
  CHECK(parse_summary_wp(summary("401", 3, "4:12", 70, 74, 0.71, "p9", "2026-10-10T23:58:01Z"), g)->play_ms == 1791676681000LL);
  CHECK(!parse_summary_wp(summary("401", 3, "4:12", 70, 76, 0.71, "p9", ""), g));   // the score moved
  CHECK(!parse_summary_wp(summary("401", 3, "3:58", 70, 74, 0.71, "p9", ""), g));   // the clock moved
  CHECK(!parse_summary_wp(summary("401", 3, "4:12", 70, 74, 0.71, "p10", ""), g));  // a newer play than the scoreboard's
  auto stale_chart = summary("401", 3, "4:12", 70, 74, 0.71, "p9", "");
  stale_chart["winprobability"].erase(1);   // the chart's last point is an older play
  CHECK(!parse_summary_wp(stale_chart, g));
  CHECK(!parse_summary_wp(summary("401", 3, "4:12", 70, 74, 1.2, "p9", ""), g));
  // the contract's fields; the away team's chance is the rest after home and a tie
  const auto* nba = espn_league_for("KXNBAGAME-26OCT10LALBOS-LAL");
  auto ref = espn_ref(*nba, g, false, EspnWp{0.71, 0.0, 1791676681000LL});
  CHECK(ref.per == "Q3" && ref.clock == "4:12" && ref.score == "70-74" && ref.espn == Px{2900} && ref.espn_ts_ms == 1791676681000LL);
  CHECK(espn_ref(*nba, g, true, EspnWp{0.71, 0.0, 0}).espn == Px{7100});
  // the same state as numbers, from the YES team's side, for a model's fair
  CHECK(ref.state == "in" && ref.period == 3 && ref.clock_s == 252 && ref.yes_score == 70 && ref.opp_score == 74);
  const auto home = espn_ref(*nba, g, true, std::nullopt);
  CHECK(home.yes_score == 74 && home.opp_score == 70);
  auto late = g; late.clock = "0:35.4";
  CHECK(espn_ref(*nba, late, true, std::nullopt).clock_s == 35.4);
  auto secs = g; secs.clock = "12";
  CHECK(espn_ref(*nba, secs, true, std::nullopt).clock_s == 12);
  auto blank = g; blank.clock = "";
  CHECK(espn_ref(*nba, blank, true, std::nullopt).clock_s == -1);
  auto junk = g; junk.clock = "x:y";
  CHECK(espn_ref(*nba, junk, true, std::nullopt).clock_s == -1);
  auto ot = g; ot.period = 5;
  CHECK(espn_ref(*nba, ot, true, std::nullopt).per == "OT" && !espn_ref(*nba, ot, true, std::nullopt).espn);
  const auto pre = espn_ref(*nba, games[1], true, std::nullopt);
  CHECK(pre.per == "PRE" && pre.clock == "20:00");   // 00:00Z is 8 pm Eastern (EDT)
  auto post = g; post.state = "post";
  CHECK(espn_ref(*nba, post, true, std::nullopt).per == "FINAL");
  const auto* ncaam = espn_league_for("KXNCAAMBGAME-26NOV05DUKEUNC-DUKE");
  auto half = g; half.period = 2;
  CHECK(espn_ref(*ncaam, half, true, std::nullopt).per == "H2");
}

void matching() {
  const auto now = now_ms();
  json sb = {{"events", {event("401", "2026-10-10T23:30Z", "in", 3, "4:12", team("away", "LAL", "Los Angeles Lakers", 70),
                               team("home", "BOS", "Boston Celtics", 74)),
                         event("501", "2026-10-10T17:05Z", "in", 3, "", team("away", "CHC", "Chicago Cubs", 1),
                               team("home", "CHW", "Chicago White Sox", 2)),
                         event("502", "2026-10-10T23:10Z", "pre", 0, "", team("away", "CHC", "Chicago Cubs", 0),
                               team("home", "CHW", "Chicago White Sox", 0))}}};
  const auto games = parse_scoreboard(sb, "x", now);
  KalshiGame k{"KXNBAGAME-26OCT10LALBOS", "KXNBAGAME", "2026-10-10", -1,
               "KXNBAGAME-26OCT10LALBOS-BOS", "BOS", "Boston", "KXNBAGAME-26OCT10LALBOS-LAL", "LAL", "Los Angeles L"};
  auto m = match_game(k, games);
  CHECK(m && m->id == "401" && m->a_home);   // market a (BOS) is the home team, whatever the ticker order
  k.date = "2026-10-13";   // three days off: not this game
  CHECK(!match_game(k, games));
  // a doubleheader: the start time in the ticker decides
  KalshiGame dh{"KXMLBGAME-26OCT101905CHCCWS", "KXMLBGAME", "2026-10-10", 19 * 60 + 5,
                "KXMLBGAME-26OCT101905CHCCWS-CHC", "CHC", "Chicago C", "KXMLBGAME-26OCT101905CHCCWS-CHW", "CHW", "Chicago WS"};
  m = match_game(dh, games);
  CHECK(m && m->id == "502" && !m->a_home);
  dh.start_min = 13 * 60 + 5;
  m = match_game(dh, games);
  CHECK(m && m->id == "501");
  // no time and the same two teams twice that day: two games we cannot tell apart
  dh.start_min = -1;
  CHECK(!match_game(dh, games));
  // only one team has evidence: no match
  KalshiGame one{"KXNBAGAME-26OCT10LALXYZ", "KXNBAGAME", "2026-10-10", -1,
                 "KXNBAGAME-26OCT10LALXYZ-XYZ", "XYZ", "Nowhere", "KXNBAGAME-26OCT10LALXYZ-LAL", "LAL", "Los Angeles L"};
  CHECK(!match_game(one, games));
}

void feed() {
  httplib::Server srv;
  std::atomic<int> boards{0}, sums{0};
  std::atomic<bool> down{false};
  srv.Get("/apis/site/v2/sports/basketball/nba/scoreboard", [&](const httplib::Request& req, httplib::Response& res) {
    if (down) { res.status = 503; return; }
    ++boards;
    CHECK(req.get_param_value("dates") == "20261010" && req.get_param_value("limit") == "300");
    CHECK(req.get_header_value("User-Agent") == "curl/8.5.0");
    json sb = {{"events", {event("401", "2026-10-10T23:30Z", "in", 3, "4:12", team("away", "LAL", "Los Angeles Lakers", 70),
                                 team("home", "BOS", "Boston Celtics", 74), "p9")}}};
    res.set_content(sb.dump(), "application/json");
  });
  srv.Get("/apis/site/v2/sports/basketball/nba/summary", [&](const httplib::Request& req, httplib::Response& res) {
    ++sums;
    CHECK(req.get_param_value("event") == "401");
    res.set_content(summary("401", 3, "4:12", 70, 74, 0.71, "p9", "2026-10-10T23:58:01Z").dump(), "application/json");
  });
  const int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  std::string why;
  auto ep = net::Endpoint::parse("http://127.0.0.1:" + std::to_string(port), &why);
  CHECK(ep.has_value());
  std::vector<kalshi::MarketRow> watched = {market("KXNBAGAME-26OCT10LALBOS-LAL", "KXNBAGAME-26OCT10LALBOS", "Los Angeles L"),
                                            market("KXNBAGAME-26OCT10LALBOS-BOS", "KXNBAGAME-26OCT10LALBOS", "Boston"),
                                            market("KXHIGHNY-26OCT10-B70", "KXHIGHNY-26OCT10", "70")};
  EspnFeed f(*ep, [&] { return watched; });
  const std::int64_t now = 1791676700000LL;   // 2026-10-10T23:58:20Z
  f.step(now);
  auto b = f.board();
  CHECK(b->size() == 2 && boards == 1 && sums == 1);
  if (b->size() == 2) {
    const auto& lal = b->at("KXNBAGAME-26OCT10LALBOS-LAL");
    const auto& bos = b->at("KXNBAGAME-26OCT10LALBOS-BOS");
    CHECK(lal.per == "Q3" && lal.score == "70-74" && lal.espn == Px{2900} && bos.espn == Px{7100});
    CHECK(lal.espn_ts_ms == 1791676681000LL);
  }
  f.step(now + 5000);   // paced: nothing fetched again
  CHECK(boards == 1 && sums == 1);
  f.step(now + 31000);
  CHECK(boards == 2 && sums == 2);
  // ESPN goes away: what was shown stays two minutes, then nothing
  down = true;
  f.step(now + 52000);
  CHECK(f.board()->size() == 2);
  f.step(now + 31000 + 120001);
  CHECK(f.board()->empty());
  CHECK(f.status()["failures"].get<int>() >= 1);
  // a venue host is never reachable from this connection
  auto venue = net::Endpoint::parse("https://demo-api.kalshi.co", &why);
  EspnFeed wrong(*venue, [&] { return watched; });
  wrong.step(now);
  CHECK(wrong.board()->empty() && wrong.status()["error"].get<std::string>().find("venue host") != std::string::npos);
  srv.stop();
  th.join();
}
}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  parsing(); matching(); feed();
  return finish("deskd-espn");
}
