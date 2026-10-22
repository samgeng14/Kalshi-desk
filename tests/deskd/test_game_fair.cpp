// strat/game_fair: the game markets' pregame price and in-game fair, one engine for WCBB, NBA and
// volleyball. The effective two-market book; p0 captured only while the score says pre, before
// the start and at 15c or less, the last such capture kept and never an in-play mid; the fair
// from the home team's side and 1 - it for the other market (so they sum to 1 under a model that
// is not symmetric); NBA overtime without the ot feature; volleyball from the linescores; the
// captures kept in game_p0.json, written at most once a minute, dropped after 12 h; an NCAA board
// behind ESPN's; a model on another sport's series refused.
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/rest/state.h"
#include "deskd/strat/game_fair.h"
#include "deskd/strat/wcbb_model.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
std::string fixture(const std::string& name) { return std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/" + name; }
std::string read(const std::string& path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

const std::int64_t T = 1793142000000LL;   // the scheduled start, 2026-10-28T00:20:00Z
const std::string E = "KXNBAGAME-26OCT27LALBOS", BOS = E + "-BOS", LAL = E + "-LAL";   // Boston at home

MdBook book(const std::string& tk, Px bid, Px ask, bool stale = false) {
  MdBook b;
  b.ticker = tk;
  if (bid > 0) b.bids = {{bid, 500}};
  if (ask > 0) b.asks = {{ask, 500}};
  b.stale = stale;
  b.reason = stale ? "gap" : "";
  return b;
}

// A game's score as ESPN's board has it for one market (its YES team's side)
EspnRef score(const std::string& state, bool yes_home, int period = 0, double clock_s = -1, int yes = 0, int opp = 0) {
  EspnRef r;
  r.state = state;
  r.yes_home = yes_home;
  r.period = period;
  r.clock_s = clock_s;
  r.yes_score = yes;
  r.opp_score = opp;
  r.start_ms = T;
  r.first_seen_ms = T - 5000;
  r.fetched_ms = T - 1000;
  return r;
}

// The world a GameFair reads, each part replaceable between steps
struct World {
  std::shared_ptr<rest::Snapshot> snap = std::make_shared<rest::Snapshot>();
  std::map<std::string, MdBook> books;
  EspnBoard espn, ncaa;
  bool with_ncaa = false;
  void list(const std::string& event, const std::vector<std::string>& tickers) {
    for (const auto& tk : tickers) {
      kalshi::MarketRow m;
      m.ticker = tk;
      m.event_ticker = event;
      m.status = "active";
      snap->markets[tk] = m;
    }
  }
  // a game in play or before it, both markets' sides of one score
  void game(const std::string& b_home, const std::string& b_away, const std::string& state, int period = 0, double clock_s = -1,
            int home = 0, int away = 0) {
    espn[b_home] = score(state, true, period, clock_s, home, away);
    espn[b_away] = score(state, false, period, clock_s, away, home);
  }
  GameFair::Env env() {
    GameFair::Env e;
    e.snapshot = [this] { return std::shared_ptr<const rest::Snapshot>(snap); };
    e.books = [this] { return std::make_shared<const std::map<std::string, MdBook>>(books); };
    e.espn = [this] { return std::make_shared<const EspnBoard>(espn); };
    if (with_ncaa) e.ncaa = [this] { return std::make_shared<const EspnBoard>(ncaa); };
    return e;
  }
};

// NBA, not symmetric (an intercept): P(home) and P(away) from each side would not sum to 1
BballModel nba(bool ot = false) {
  BballModel m;
  m.league = "nba";
  m.regulation_s = 2880;
  m.features = {"logit_p0", "margin_over_sd"};
  m.mean = {0, 0};
  m.scale = {1, 1};
  m.coef = {1, 0.3};
  m.intercept = 0.2;
  if (ot) { m.features.push_back("ot"); m.mean.push_back(0); m.scale.push_back(1); m.coef.push_back(-0.1); }
  return m;
}

void effective() {
  const auto a = book(BOS, 4000, 4800), b = book(LAL, 5000, 5800);
  auto e = effective_book(&a, &b);   // bid max(0.40, 1 - 0.58), ask min(0.48, 1 - 0.50)
  CHECK(e && e->bid == 4200 && e->ask == 4800);
  e = effective_book(&a, nullptr);
  CHECK(e && e->bid == 4000 && e->ask == 4800);
  e = effective_book(nullptr, &b);
  CHECK(e && e->bid == 4200 && e->ask == 5000);
  const auto gapped = book(BOS, 4500, 4600, true);   // a stale book is no price
  e = effective_book(&gapped, &b);
  CHECK(e && e->bid == 4200 && e->ask == 5000);
  const auto bids_a = book(BOS, 4000, 0), bids_b = book(LAL, 5000, 0);   // one side each: still a book
  e = effective_book(&bids_a, &bids_b);
  CHECK(e && e->bid == 4000 && e->ask == 5000);
  CHECK(!effective_book(&bids_a, nullptr) && !effective_book(&gapped, nullptr) && !effective_book(nullptr, nullptr));
}

void capture() {
  World w;
  w.list(E, {BOS, LAL});
  w.game(BOS, LAL, "pre");
  w.books[BOS] = book(BOS, 6000, 6600);
  w.books[LAL] = book(LAL, 3300, 4100);   // BOS's effective book: 0.60 / 0.66
  GameFair g(w.env(), {{"KXNBAGAME", nba()}});
  g.step(T - 600000);
  auto b = g.board();
  CHECK(b->size() == 2 && b->at(BOS).p0 == 0.63 && b->at(LAL).p0 && std::fabs(*b->at(LAL).p0 - 0.37) < 1e-15);
  CHECK(!b->at(BOS).p && b->at(BOS).why == "not started" && b->at(BOS).model == "nba_logit" && b->at(BOS).event == E);
  // the last capture before the start wins
  w.books[BOS] = book(BOS, 6200, 6600);
  g.step(T - 60000);
  CHECK(g.board()->at(BOS).p0 == 0.64);
  // wider than 15c is no price; 15c is
  w.books.erase(LAL);
  w.books[BOS] = book(BOS, 5099, 6600);
  g.step(T - 50000);
  CHECK(g.board()->at(BOS).p0 == 0.64);
  w.books[BOS] = book(BOS, 5100, 6600);
  g.step(T - 40000);
  CHECK(g.board()->at(BOS).p0 == 0.585);
  // a late tip: still "pre" at the scheduled start, and the mid is no longer pregame
  w.books[BOS] = book(BOS, 7000, 7200);
  g.step(T);
  CHECK(g.board()->at(BOS).p0 == 0.585);
  // in play: the mid never replaces it
  w.game(BOS, LAL, "in", 1, 700, 2, 0);
  g.step(T + 60000);
  CHECK(g.board()->at(BOS).p0 == 0.585 && g.board()->at(BOS).p);
  // a game first seen after its start has no p0, and so no fair
  const std::string E2 = "KXNBAGAME-26OCT27NYKMIA", MIA = E2 + "-MIA", NYK = E2 + "-NYK";
  w.list(E2, {MIA, NYK});
  w.game(MIA, NYK, "in", 1, 650, 4, 2);
  w.books[MIA] = book(MIA, 5000, 5200);
  g.step(T + 61000);
  b = g.board();
  CHECK(!b->at(MIA).p0 && !b->at(MIA).p && !b->at(MIA).shown() && has(b->at(MIA).why, "no pregame price"));
  CHECK(!g.status()["p0"].contains(E2) && g.status()["p0"][E]["ticker"] == BOS);
}

void fair() {
  World w;
  w.list(E, {BOS, LAL});
  w.game(BOS, LAL, "pre");
  w.books[BOS] = book(BOS, 6000, 6600);
  const auto m = nba();
  GameFair g(w.env(), {{"KXNBAGAME", m}});
  g.step(T - 600000);
  // Q3 4:12, Boston up 74-70
  w.game(BOS, LAL, "in", 3, 252, 74, 70);
  g.step(T + 3600000);
  auto b = g.board();
  const double home = *bball_live_p(m, "in", 3, 252, 4, 0.63);
  CHECK(b->at(BOS).p == home && b->at(LAL).p == 1 - home && b->at(BOS).shown() && b->at(LAL).shown());
  CHECK(std::fabs(*b->at(BOS).p + *b->at(LAL).p - 1) < 1e-15);
  // from the away side alone the model would not agree with itself: the home side is the rule
  CHECK(std::fabs(*bball_live_p(m, "in", 3, 252, -4, 0.37) - (1 - home)) > 1e-3);
  CHECK(b->at(BOS).src == "ESPN" && b->at(BOS).first_seen_ms == T - 5000 && b->at(BOS).fetched_ms == T - 1000 && !b->at(BOS).ot);
  CHECK(g.status()["markets"][BOS]["fair"] == price_str(static_cast<Px>(std::llround(home * kOneDollar))));
  // NBA overtime reads its own clock: a model without the ot feature has no fair there
  w.game(BOS, LAL, "in", 5, 180, 100, 100);
  g.step(T + 9000000);
  b = g.board();
  CHECK(!b->at(BOS).p && b->at(BOS).ot && b->at(BOS).why == "overtime: the model has no ot feature");
  GameFair with_ot(w.env(), {{"KXNBAGAME", nba(true)}});
  w.game(BOS, LAL, "pre");
  with_ot.step(T - 600000);
  w.game(BOS, LAL, "in", 5, 180, 100, 100);
  with_ot.step(T + 9000000);
  b = with_ot.board();
  CHECK(b->at(BOS).p && b->at(BOS).ot && *b->at(BOS).p == *bball_live_p(nba(true), "in", 5, 180, 0, 0.63));
  // no clock, the final: no fair, and why
  w.game(BOS, LAL, "in", 3, -1, 74, 70);
  g.step(T + 9001000);
  CHECK(!g.board()->at(BOS).p && has(g.board()->at(BOS).why, "no game clock in period 3"));
  w.game(BOS, LAL, "post", 4, 0, 110, 104);
  g.step(T + 9002000);
  CHECK(!g.board()->at(LAL).p && g.board()->at(LAL).why == "final");
}

// women's college basketball: the trainer's file, overtime at t_left 0 (as it was trained)
void wcbb() {
  std::string why;
  const auto m = load_wcbb_model(fixture("wcbb_synthetic.json"), &why);
  CHECK(m.has_value());
  if (!m) return;
  const std::string EV = "KXNCAAWBGAME-26OCT27UCONNSC", SC = EV + "-SC", UCONN = EV + "-UCONN";   // UConn at home
  World w;
  w.list(EV, {SC, UCONN});
  w.game(UCONN, SC, "pre");
  w.books[UCONN] = book(UCONN, 7000, 7400);
  w.books[SC] = book(SC, 2700, 3100);   // UConn's effective book: 0.70 / 0.73
  GameFair g(w.env(), {{"KXNCAAWBGAME", *m}});
  g.step(T - 60000);   // SC's effective book 0.27 / 0.30, so UConn's p0 is 1 - 0.285
  const auto p0 = g.board()->at(UCONN).p0;
  CHECK(p0 && std::fabs(*p0 - 0.715) < 1e-15 && g.board()->at(SC).p0 == 0.285);
  if (!p0) return;
  w.game(UCONN, SC, "in", 2, 300, 30, 33);
  g.step(T + 1800000);
  auto b = g.board();
  const double home = *wcbb_live_p(*m, "in", 2, 300, -3, *p0);
  CHECK(b->at(UCONN).p == home && b->at(SC).p == 1 - home && b->at(UCONN).model == "wcbb_logit");
  w.game(UCONN, SC, "in", 5, 120, 70, 70);
  g.step(T + 7200000);
  b = g.board();
  CHECK(b->at(UCONN).ot && b->at(UCONN).p == *wcbb_live_p(*m, "in", 5, 120, 0, *p0));
}

void volleyball() {
  std::string why;
  const auto m = load_volley_model(fixture("volleyball_golden.json"), &why);
  CHECK(m.has_value());
  if (!m) return;
  const std::string EV = "KXNCAAWVMATCH-26OCT27UCLAUSC", UCLA = EV + "-UCLA", USC = EV + "-USC";   // USC at home
  World w;
  w.list(EV, {UCLA, USC});
  w.game(USC, UCLA, "pre");
  w.books[UCLA] = book(UCLA, 4000, 4400);
  GameFair g(w.env(), {{"KXNCAAWVMATCH", *m}});
  g.step(T - 60000);
  const auto p0 = g.board()->at(USC).p0;   // 1 - UCLA's 0.42
  CHECK(g.board()->at(UCLA).p0 == 0.42 && p0 && std::fabs(*p0 - 0.58) < 1e-15);
  if (!p0) return;
  // set 3, UCLA 10-8 up, a set each (UCLA 25-20, USC 25-22)
  auto set3 = [&](std::vector<int> usc, std::vector<int> ucla, int usc_sets, int ucla_sets) {
    w.game(USC, UCLA, "in", 3, -1, usc_sets, ucla_sets);
    w.espn[USC].yes_lines = w.espn[UCLA].opp_lines = usc;
    w.espn[USC].opp_lines = w.espn[UCLA].yes_lines = ucla;
  };
  set3({20, 25, 8}, {25, 22, 10}, 1, 1);
  g.step(T + 3600000);
  auto b = g.board();
  const double home = *volley_live_p(*m, VolleyState{1, 1, 8, 10, true}, *p0);
  CHECK(b->at(USC).p == home && b->at(UCLA).p == 1 - home && b->at(USC).shown() && b->at(UCLA).model == "volley_markov");
  // the model is symmetric: from UCLA's own side it is the same to rounding
  CHECK(std::fabs(*b->at(UCLA).p - *volley_live_p(*m, VolleyState{1, 1, 10, 8, true}, 0.42)) < 1e-12);
  // the set's points not read: a P with the set at 0-0, kept but not shown
  set3({20, 25}, {25, 22}, 1, 1);
  g.step(T + 3601000);
  b = g.board();
  CHECK(b->at(USC).p == *volley_live_p(*m, VolleyState{1, 1, 0, 0, false}, *p0) && !b->at(USC).shown() && !b->at(USC).points);
  CHECK(has(b->at(USC).why, "set 3's points were not read"));
  // lines the score does not agree with: no state, and volley_state's why
  set3({20, 25, 8}, {25, 22, 10}, 2, 0);
  g.step(T + 3602000);
  CHECK(!g.board()->at(USC).p && has(g.board()->at(USC).why, "the linescores give 1-1 in sets, the score 2-0"));
}

void persistence() {
  TempDir dir;
  const std::string path = dir.file("game_p0.json");
  World w;
  w.list(E, {BOS, LAL});
  w.game(BOS, LAL, "pre");
  w.books[BOS] = book(BOS, 6000, 6600);
  const auto t0 = T - 3600000;
  {
    GameFair g(w.env(), {{"KXNBAGAME", nba()}});
    g.persist(path);
    CHECK(g.status()["error"] == "" && g.status()["path"] == path);
    g.step(t0);
    auto j = json::parse(read(path), nullptr, false);
    CHECK(j["v"] == 1 && j["p0"][E] == json({{"ticker", BOS}, {"p0", 0.63}, {"ms", t0}}));
    w.books[BOS] = book(BOS, 6200, 6600);
    g.step(t0 + 30000);   // at most once a minute
    CHECK(json::parse(read(path))["p0"][E]["p0"] == 0.63 && g.board()->at(BOS).p0 == 0.64);
    g.step(t0 + 60000);
    CHECK(json::parse(read(path))["p0"][E] == json({{"ticker", BOS}, {"p0", 0.64}, {"ms", t0 + 60000}}));
  }
  // a restart in play: the capture comes back, and with it the fair
  w.game(BOS, LAL, "in", 2, 400, 50, 48);
  GameFair h(w.env(), {{"KXNBAGAME", nba()}});
  h.persist(path);
  CHECK(h.status()["p0"][E]["p0"] == 0.64);
  h.step(T + 1800000);
  CHECK(h.board()->at(BOS).shown() && h.board()->at(BOS).p == *bball_live_p(nba(), "in", 2, 400, 2, 0.64));
  // 12 h after its capture it is dropped, from the board and then the file
  h.step(t0 + 60000 + GameFair::kKeepMs + 1);
  CHECK(!h.board()->at(BOS).p0 && !h.board()->at(BOS).p && h.status()["p0"].empty());
  CHECK(json::parse(read(path))["p0"].empty());
  // an unreadable file: said, and started from none
  { std::ofstream f(path); f << "{\"v\": 1, \"p0\": {\"" << E << "\": {\"ticker\": \"" << BOS << "\", \"p0\": 1.5, \"ms\": 1}}}"; }
  GameFair bad(w.env(), {{"KXNBAGAME", nba()}});
  bad.persist(path);
  CHECK(has(bad.status()["error"], "cannot be read") && bad.status()["p0"].empty());
  { std::ofstream f(path); f << "not json"; }
  GameFair junk(w.env(), {{"KXNBAGAME", nba()}});
  junk.persist(path);
  CHECK(has(junk.status()["error"], "not a version 1 file"));
  // no file asked for: captures in memory only
  GameFair quiet(w.env(), {{"KXNBAGAME", nba()}});
  w.game(BOS, LAL, "pre");
  quiet.step(t0);
  CHECK(quiet.board()->at(BOS).p0 == 0.64 && quiet.status()["path"] == "");
}

void sources() {
  World w;
  w.with_ncaa = true;
  w.list(E, {BOS, LAL});
  w.game(BOS, LAL, "pre");
  w.ncaa[BOS] = score("in", true, 2, 100, 40, 30);   // ESPN shows the game: its score is used
  w.ncaa[BOS].src = "NCAA";
  const std::string E3 = "KXNBAGAME-26OCT27DENPHX", PHX = E3 + "-PHX", DEN = E3 + "-DEN";
  w.list(E3, {PHX, DEN});
  for (auto [tk, home] : {std::pair{PHX, true}, std::pair{DEN, false}}) {
    w.ncaa[tk] = score("pre", home);
    w.ncaa[tk].src = "NCAA";
  }
  w.books[PHX] = book(PHX, 5500, 5700);
  w.espn["KXNBAGAME-26OCT27XXXYYY-XXX"] = score("pre", true);   // on a board, not in the listing
  w.espn["KXNFLGAME-26OCT27KCBUF-KC"] = score("pre", true);      // a series without a model
  GameFair g(w.env(), {{"KXNBAGAME", nba()}});
  g.step(T - 60000);
  auto b = g.board();
  CHECK(b->at(BOS).src == "ESPN" && b->at(BOS).state == "pre");
  CHECK(b->at(PHX).src == "NCAA" && b->at(DEN).p0 == 0.44 && b->at(PHX).p0 && std::fabs(*b->at(PHX).p0 - 0.56) < 1e-15 &&
        b->at(DEN).src == "NCAA");
  CHECK(b->at("KXNBAGAME-26OCT27XXXYYY-XXX").why == "not in the listing" && !b->count("KXNFLGAME-26OCT27KCBUF-KC"));
  // three markets in one event are not a game
  w.list(E, {BOS, LAL, E + "-TIE"});
  w.espn[E + "-TIE"] = score("pre", false);
  g.step(T - 50000);
  b = g.board();
  CHECK(has(b->at(BOS).why, "3 markets in the event") && !b->at(BOS).p);
}

void models() {
  std::string why;
  const auto w8 = load_wcbb_model(fixture("wcbb_synthetic.json"), &why);
  const auto vb = load_volley_model(fixture("volleyball_golden.json"), &why);
  CHECK(w8 && vb);
  if (!w8 || !vb) return;
  World w;
  w.list(E, {BOS, LAL});
  w.game(BOS, LAL, "pre");
  w.books[BOS] = book(BOS, 6000, 6600);
  GameFair g(w.env(), {{"KXNBAGAME", *w8}, {"KXNCAAWBGAME", *vb}, {"KXNCAAWVMATCH", *vb}});
  const auto st = g.status();
  CHECK(st["refused"]["KXNBAGAME"] == "the wcbb model is KXNCAAWBGAME's");
  CHECK(st["refused"]["KXNCAAWBGAME"] == "the volleyball model is only for a volleyball series");
  CHECK(st["models"].size() == 1 && st["models"]["KXNCAAWVMATCH"]["model"] == "volley_markov" &&
        st["models"]["KXNCAAWVMATCH"]["sha256"] == vb->sha256);
  g.step(T - 60000);
  CHECK(g.board()->empty());   // KXNBAGAME has no model here
}

}  // namespace

int main() {
  effective();
  capture();
  fair();
  wcbb();
  volleyball();
  persistence();
  sources();
  models();
  return finish("deskd-game_fair");
}
