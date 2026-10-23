// ESPN for Kalshi's game markets (ref/espn.h): parsing, the team match, the win-probability
// rule, the contract's game fields, and the feed against a local stand-in for ESPN: its pacing,
// the fast series' hot cadence, and each state's first-seen time.
#include <httplib.h>
#include <signal.h>
#include <atomic>
#include <mutex>
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
json lines(json t, const std::vector<int>& points) {
  for (int p : points) t["linescores"].push_back({{"value", static_cast<double>(p)}});   // ESPN writes 18.0
  return t;
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
  CHECK(pre.state == "pre" && pre.start_ms == games[1].start_ms && pre.start_ms > 0);   // the WCBB p0's cutoff
  auto post = g; post.state = "post";
  CHECK(espn_ref(*nba, post, true, std::nullopt).per == "FINAL");
  const auto* ncaam = espn_league_for("KXNCAAMBGAME-26NOV05DUKEUNC-DUKE");
  auto half = g; half.period = 2;
  CHECK(espn_ref(*ncaam, half, true, std::nullopt).per == "H2");
  // a parse sees everything for the first time; nothing given is nothing read
  CHECK(g.src == "ESPN" && g.first_seen_ms == now && g.prev_poll_ms == 0);
  CHECK(g.home_lines.empty() && g.possession == 0 && !g.sb_wp);
  CHECK(ref.src == "ESPN" && ref.first_seen_ms == now && ref.wp_seen_ms == 0 && !ref.yes_home && !ref.sb_wp);

  // linescores, who has the ball (a team's id) and the scoreboard's own probability, TO CONFIRM
  // on a capture; the market's side of each
  auto away = lines(team("away", "LAL", "Los Angeles Lakers", 70), {20, 25, 25});
  auto home_team = lines(team("home", "BOS", "Boston Celtics", 74), {22, 30, 22});
  away["id"] = "13";
  home_team["team"]["id"] = "2";
  json ev = event("404", "2026-10-10T23:30Z", "in", 3, "4:12", away, home_team, "p9");
  ev["competitions"][0]["situation"]["possession"] = "2";
  ev["competitions"][0]["situation"]["lastPlay"]["probability"] = {{"homeWinPercentage", 0.64}, {"tiePercentage", 0.0}};
  auto one = [&] { auto v = parse_scoreboard(json{{"events", {ev}}}, "nba", now); return v.size() == 1 ? v[0] : EspnGame{}; };
  const auto x = one();
  CHECK(x.home_lines == std::vector<int>({22, 30, 22}) && x.away_lines == std::vector<int>({20, 25, 25}));
  CHECK(x.possession == 1 && x.sb_wp && x.sb_wp->home == 0.64 && x.sb_wp->play == "p9");
  const auto yes_away = espn_ref(*nba, x, false, std::nullopt);
  CHECK(!yes_away.yes_home && yes_away.yes_lines == std::vector<int>({20, 25, 25}) &&
        yes_away.opp_lines == std::vector<int>({22, 30, 22}) && yes_away.possession == -1 && yes_away.sb_wp == Px{3600});
  const auto yes_home = espn_ref(*nba, x, true, std::nullopt);
  CHECK(yes_home.yes_home && yes_home.yes_lines == std::vector<int>({22, 30, 22}) && yes_home.possession == 1 &&
        yes_home.sb_wp == Px{6400} && !yes_home.espn);
  ev["competitions"][0]["situation"]["possession"] = "13";   // the competitor's own id
  CHECK(one().possession == -1);
  ev["competitions"][0]["situation"]["possession"] = "99";   // neither team
  CHECK(one().possession == 0);
  ev["competitions"][0]["situation"]["lastPlay"]["probability"]["homeWinPercentage"] = 1.2;
  CHECK(!one().sb_wp);
  // the summary's probability carries its play, for the feed's first sighting
  const auto w = parse_summary_wp(summary("401", 3, "4:12", 70, 74, 0.71, "p9", "2026-10-10T23:58:01Z"), g);
  CHECK(w && w->play == "p9" && w->seen_ms == 0);
  auto seen = *w; seen.seen_ms = now + 1;
  CHECK(espn_ref(*nba, g, true, seen).wp_seen_ms == now + 1);
  // a state is state, period, scores and linescores; the clock alone is no change
  auto a = x; a.clock = "3:58"; a.fetched_ms = now + 3000;
  CHECK(same_score_state(x, a));
  auto b = x; b.home_lines.back() = 23;
  auto c = x; c.home_score = 76;
  auto d = x; d.period = 4;
  auto e = x; e.state = "post";
  CHECK(!same_score_state(x, b) && !same_score_state(x, c) && !same_score_state(x, d) && !same_score_state(x, e));

  // volleyball (the path TO CONFIRM): sets won as the score, each set's points as linescores
  const auto* wvb = espn_league_for("KXNCAAWVMATCH-26OCT10UCLAUSC-UCLA");
  CHECK(wvb && wvb->sport == "volleyball" && wvb->league == "womens-college-volleyball" && wvb->periods == 5);
  const auto vb = parse_scoreboard(
      json{{"events", {event("701", "2026-10-10T23:00Z", "in", 3, "", lines(team("away", "UCLA", "UCLA Bruins", 2), {25, 22, 10}),
                             lines(team("home", "USC", "USC Trojans", 1), {20, 25, 8}))}}},
      wvb->league, now);
  CHECK(vb.size() == 1);
  if (vb.size() == 1) {
    const auto r = espn_ref(*wvb, vb[0], false, std::nullopt);
    CHECK(r.per == "S3" && r.score == "2-1" && r.yes_score == 2 && r.opp_score == 1);
    CHECK(r.yes_lines == std::vector<int>({25, 22, 10}) && r.opp_lines == std::vector<int>({20, 25, 8}));
  }
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
    CHECK(lal.first_seen_ms == now && lal.fetched_ms == now && lal.wp_seen_ms == now);
  }
  f.step(now + 5000);   // paced: nothing fetched again
  CHECK(boards == 1 && sums == 1);
  f.step(now + 31000);
  CHECK(boards == 2 && sums == 2);
  {   // no fast series: nothing is read hot; the same state and play keep their first sighting
    const auto lal = f.board()->at("KXNBAGAME-26OCT10LALBOS-LAL");
    CHECK(lal.first_seen_ms == now && lal.fetched_ms == now + 31000 && lal.wp_seen_ms == now);
    CHECK(f.status()["hot"].empty() && f.status()["fast_series"].empty());
  }
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

// A fast series is read hot while its game is on or about to start: the scoreboard every 3 s and
// summaries every 10 s, while another league keeps 20 and 30 s. Each state's first sighting, and
// each play's, is kept on our own clock.
void fast() {
  const std::int64_t t0 = 1791676700000LL;   // 2026-10-10T23:58:20Z
  std::mutex mu;   // the game as ESPN tells it now
  std::string clock = "4:12", state = "in", play = "p9", wallclock = "2026-10-10T23:57:40Z";   // p9: 40 s before t0
  int bos = 74;
  std::vector<int> bos_lines = {22, 30, 22};
  std::atomic<int> nba_boards{0}, nfl_boards{0}, nba_sums{0}, nfl_sums{0};
  httplib::Server srv;
  srv.Get("/apis/site/v2/sports/basketball/nba/scoreboard", [&](const httplib::Request&, httplib::Response& res) {
    ++nba_boards;
    std::lock_guard<std::mutex> lk(mu);
    json sb = {{"events", {event("401", "2026-10-10T23:30Z", state, 3, clock, lines(team("away", "LAL", "Los Angeles Lakers", 70), {20, 25, 25}),
                                 lines(team("home", "BOS", "Boston Celtics", bos), bos_lines), play),
                           // tips 20 minutes after t0
                           event("402", "2026-10-11T00:18:20Z", "pre", 0, "", team("away", "NYK", "New York Knicks", 0),
                                 team("home", "MIA", "Miami Heat", 0))}}};
    res.set_content(sb.dump(), "application/json");
  });
  srv.Get("/apis/site/v2/sports/basketball/nba/summary", [&](const httplib::Request&, httplib::Response& res) {
    ++nba_sums;
    std::lock_guard<std::mutex> lk(mu);
    res.set_content(summary("401", 3, clock, 70, bos, 0.71, play, wallclock).dump(), "application/json");
  });
  srv.Get("/apis/site/v2/sports/football/nfl/scoreboard", [&](const httplib::Request&, httplib::Response& res) {
    ++nfl_boards;
    json sb = {{"events", {event("601", "2026-10-10T20:25Z", "in", 4, "2:00", team("away", "DAL", "Dallas Cowboys", 17),
                                 team("home", "NYG", "New York Giants", 20))}}};
    res.set_content(sb.dump(), "application/json");
  });
  srv.Get("/apis/site/v2/sports/football/nfl/summary", [&](const httplib::Request&, httplib::Response& res) {
    ++nfl_sums;
    res.set_content("{}", "application/json");
  });
  const int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  std::string why;
  auto ep = net::Endpoint::parse("http://127.0.0.1:" + std::to_string(port), &why);
  CHECK(ep.has_value());
  std::vector<kalshi::MarketRow> watched = {market("KXNBAGAME-26OCT10LALBOS-LAL", "KXNBAGAME-26OCT10LALBOS", "Los Angeles L"),
                                            market("KXNBAGAME-26OCT10LALBOS-BOS", "KXNBAGAME-26OCT10LALBOS", "Boston"),
                                            market("KXNFLGAME-26OCT10DALNYG-DAL", "KXNFLGAME-26OCT10DALNYG", "Dallas"),
                                            market("KXNFLGAME-26OCT10DALNYG-NYG", "KXNFLGAME-26OCT10DALNYG", "New York G")};
  EspnFeed::Options o;
  o.fast_series = {"KXNBAGAME"};
  EspnFeed f(*ep, [&] { return watched; }, o);
  std::vector<EspnGame> told;
  f.set_on_change([&](const EspnGame& g) { told.push_back(g); });
  auto lal = [&] { return f.board()->at("KXNBAGAME-26OCT10LALBOS-LAL"); };

  f.step(t0);
  CHECK(nba_boards == 1 && nfl_boards == 1 && nba_sums == 1 && nfl_sums == 1);
  CHECK(f.board()->size() == 4 && told.size() == 2);   // each watched game's first sighting
  CHECK(lal().first_seen_ms == t0 && lal().wp_seen_ms == t0 && lal().yes_lines == std::vector<int>({20, 25, 25}));
  {   // the clock runs: a 3 s read for the NBA, none for the NFL; no new state
    std::lock_guard<std::mutex> lk(mu);
    clock = "3:58";
  }
  f.step(t0 + 3000);
  CHECK(nba_boards == 2 && nfl_boards == 1 && nba_sums == 1 && nfl_sums == 1);
  CHECK(lal().first_seen_ms == t0 && lal().fetched_ms == t0 + 3000 && lal().clock == "3:58" && told.size() == 2);
  CHECK(f.status()["hot"] == json::array({"nba|2026-10-10"}) && f.status()["fast_series"] == json::array({"KXNBAGAME"}));
  {   // Boston scores: a new state, first seen at this poll, after the one before
    std::lock_guard<std::mutex> lk(mu);
    bos = 76;
    bos_lines = {22, 30, 24};
  }
  f.step(t0 + 6000);
  CHECK(nba_boards == 3 && nfl_boards == 1);
  CHECK(lal().first_seen_ms == t0 + 6000 && lal().opp_score == 76 && lal().opp_lines == std::vector<int>({22, 30, 24}));
  CHECK(told.size() == 3);
  if (told.size() == 3)
    CHECK(told[2].id == "401" && told[2].home_score == 76 && told[2].first_seen_ms == t0 + 6000 &&
          told[2].prev_poll_ms == t0 + 3000 && told[2].src == "ESPN");
  f.step(t0 + 10000);   // the summary at 10 s for the fast series: the same play keeps its sighting
  CHECK(nba_boards == 4 && nba_sums == 2 && nfl_sums == 1);
  CHECK(lal().first_seen_ms == t0 + 6000 && lal().espn == Px{2900} && lal().wp_seen_ms == t0);
  {   // a new play, 40 s after its own wall clock
    std::lock_guard<std::mutex> lk(mu);
    play = "p10";
    wallclock = "2026-10-10T23:58:00Z";
  }
  f.step(t0 + 20000);   // the NFL's 20 s scoreboard; its summary waits 30 s
  CHECK(nba_boards == 5 && nfl_boards == 2 && nba_sums == 3 && nfl_sums == 1);
  CHECK(lal().wp_seen_ms == t0 + 20000 && lal().first_seen_ms == t0 + 6000 && told.size() == 3);
  CHECK(f.status()["lag"]["p50_ms"] == 40000 && f.status()["lag"]["plays"] == 2);
  f.step(t0 + 30000);
  CHECK(nfl_sums == 2 && nba_sums == 4 && nba_boards == 6 && nfl_boards == 2);
  {   // the final: one more read, then 20 s again
    std::lock_guard<std::mutex> lk(mu);
    state = "post";
  }
  f.step(t0 + 33000);
  CHECK(nba_boards == 7 && lal().per == "FINAL" && told.size() == 4);
  f.step(t0 + 36000);
  f.step(t0 + 50000);
  CHECK(nba_boards == 7 && f.status()["hot"].empty());
  f.step(t0 + 53000);
  CHECK(nba_boards == 8 && nba_sums == 4);

  // A game 20 minutes from its start waits 20 s; from 15 minutes before it, it is read hot.
  std::vector<kalshi::MarketRow> later = {market("KXNBAGAME-26OCT10NYKMIA-NYK", "KXNBAGAME-26OCT10NYKMIA", "New York K"),
                                          market("KXNBAGAME-26OCT10NYKMIA-MIA", "KXNBAGAME-26OCT10NYKMIA", "Miami")};
  EspnFeed pre(*ep, [&] { return later; }, o);
  const int before = nba_boards;
  pre.step(t0);
  CHECK(nba_boards == before + 1 && pre.board()->size() == 2 && pre.board()->at("KXNBAGAME-26OCT10NYKMIA-MIA").state == "pre");
  pre.step(t0 + 3000);
  CHECK(nba_boards == before + 1);
  pre.step(t0 + 300000);   // 15 min before the tip: due at 20 s anyway, and hot from now
  pre.step(t0 + 303000);
  CHECK(nba_boards == before + 3 && nba_sums == 4);   // no summary before the tip
  // nothing reads hot without a fast series, whatever the game
  EspnFeed slow(*ep, [&] { return later; });
  slow.step(t0 + 300000);
  slow.step(t0 + 303000);
  CHECK(nba_boards == before + 4);
  srv.stop();
  th.join();
}
}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  parsing(); matching(); feed(); fast();
  return finish("deskd-espn");
}
