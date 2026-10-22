// strat/take: the IOC game lanes' one decision. clip_count against hand-worked sizes; each lane's
// defaults; every case of the three hand-worked fixtures the Python gate tools share
// (college_take_cases.json for C1, x1_golden.json for X1, n1_golden.json for N1 and N2), which
// between them reach every pull; a game seen from the other market is the same take; the order's
// own fields; the reasons in words; no game cap, no inventory limit and a huge clip stay bounded
// by the touch; the inventory room is gw's, on either side of flat.
// TakeEngine against a fake Env (gw a stub inbox): one IOC a run with every field, gw's answer
// awaited and a refusal's back-off, the signal runs, what blocks a post and the shadow rows, PULL
// rows, the inputs it builds (fair on the board's state, worst case, Systematic's positions and
// working orders, books, volume, clock, set ends, ESPN's number), which games, each series' own
// rule, the signals' markouts and LaneMarkout to gw.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <variant>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/gw/fees.h"
#include "deskd/gw/sys_book.h"
#include "deskd/ref/scores.h"
#include "deskd/rest/state.h"
#include "deskd/strat/take.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
std::string fixture(const std::string& name) { return std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/" + name; }
json read_json(const std::string& path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return json::parse(s.str(), nullptr, false);
}

std::set<std::string> seen_pulls;   // every pull a case below reached

// ---- the fixtures' format (see any of the three files' "how" and "units")

// over laid on base: objects merged key by key, anything else replaced
json merge(json base, const json& over) {
  if (!base.is_object() || !over.is_object()) return over;
  for (const auto& [k, v] : over.items()) base[k] = base.contains(k) ? merge(base[k], v) : v;
  return base;
}
Px px_of(const json& v) { return *parse_price(v.get<std::string>()); }
Money money_of(const json& v) { return *parse_money(v.get<std::string>()); }
Qty qty_of(const json& v) { return *qty_from_number(v.get<double>(), true); }

TakeParams params_of(const json& j, TakeMode mode) {
  TakeParams p;
  p.mode = mode;
  p.edge = money_of(j.at("edge"));
  p.clip = money_of(j.at("clip"));
  p.max_count = qty_of(j.at("max_count"));
  p.fixed_count = qty_of(j.at("fixed_count"));
  p.game_cap = money_of(j.at("game_cap")) * 100;
  p.inventory = qty_of(j.at("inventory"));
  p.max_takes = j.at("max_takes").get<int>();
  p.max_score_age_ms = j.at("max_score_age_ms").get<std::int64_t>();
  p.signal_ms = j.at("signal_ms").get<std::int64_t>();
  p.cool_ms = j.at("cool_ms").get<std::int64_t>();
  p.max_volume = qty_of(j.at("max_volume"));
  p.stale_only = j.at("stale_only").get<bool>();
  p.leader_only = j.at("leader_only").get<bool>();
  p.points_required = j.at("points_required").get<bool>();
  p.set_window_ms = j.at("set_window_ms").get<std::int64_t>();
  p.max_spread = px_of(j.at("max_spread"));
  p.min_px = px_of(j.at("min_px"));
  p.max_px = px_of(j.at("max_px"));
  p.trade_ot = j.at("trade_ot").get<bool>();
  p.min_t_left_s = j.at("min_t_left_s").get<double>();
  p.fav_lo = px_of(j.at("fav_lo"));
  p.fav_hi = px_of(j.at("fav_hi"));
  p.fav_from_ms = j.at("fav_from_ms").get<std::int64_t>();
  p.fav_to_ms = j.at("fav_to_ms").get<std::int64_t>();
  return p;
}
bool same_params(const TakeParams& x, const TakeParams& y) {
  return x.mode == y.mode && x.edge == y.edge && x.clip == y.clip && x.max_count == y.max_count &&
         x.fixed_count == y.fixed_count && x.game_cap == y.game_cap && x.inventory == y.inventory && x.max_takes == y.max_takes &&
         x.max_score_age_ms == y.max_score_age_ms && x.signal_ms == y.signal_ms && x.cool_ms == y.cool_ms &&
         x.max_volume == y.max_volume && x.stale_only == y.stale_only && x.leader_only == y.leader_only &&
         x.points_required == y.points_required && x.set_window_ms == y.set_window_ms && x.max_spread == y.max_spread &&
         x.min_px == y.min_px && x.max_px == y.max_px && x.trade_ot == y.trade_ot && x.min_t_left_s == y.min_t_left_s &&
         x.fav_lo == y.fav_lo && x.fav_hi == y.fav_hi && x.fav_from_ms == y.fav_from_ms && x.fav_to_ms == y.fav_to_ms;
}

// A TakeInput and the books its legs point at
struct Game {
  MdBook book_a, book_b;
  TakeInput in;
  Game() = default;
  Game(const Game& o) { *this = o; }
  Game& operator=(const Game& o) {
    book_a = o.book_a;
    book_b = o.book_b;
    in = o.in;
    in.a.book = &book_a;
    in.b.book = &book_b;
    return *this;
  }
};
void leg_of(const json& j, TakeLeg* l, MdBook* b) {
  l->ticker = j.at("ticker").get<std::string>();
  b->ticker = l->ticker;
  if (!j.at("bid").is_null()) b->bids = {{px_of(j.at("bid")), qty_of(j.at("bid_qty"))}};
  if (!j.at("ask").is_null()) b->asks = {{px_of(j.at("ask")), qty_of(j.at("ask_qty"))}};
  b->stale = j.at("stale").get<bool>();
  b->reason = b->stale ? "gap" : "";
  if (!j.at("fee_mult").is_null()) l->fee_mult = j.at("fee_mult").get<double>();
  l->fractional = j.at("fractional").get<bool>();
  l->position = qty_of(j.at("position"));
  l->sys_position = qty_of(j.at("sys_position"));
  l->sys_buying = qty_of(j.at("sys_buying"));
  l->sys_selling = qty_of(j.at("sys_selling"));
  l->bid_since_ms = j.at("bid_since_ms").get<std::int64_t>();
  l->ask_since_ms = j.at("ask_since_ms").get<std::int64_t>();
  l->book = b;
}
Game game_of(const json& j) {
  Game g;
  TakeInput& in = g.in;
  leg_of(j.at("a"), &in.a, &g.book_a);
  leg_of(j.at("b"), &in.b, &g.book_b);
  in.state = j.at("state").get<std::string>();
  in.src = j.at("src").get<std::string>();
  in.score_why = j.at("score_why").get<std::string>();
  in.fetched_ms = j.at("fetched_ms").get<std::int64_t>();
  in.first_seen_ms = j.at("first_seen_ms").get<std::int64_t>();
  in.start_ms = j.at("start_ms").get<std::int64_t>();
  in.a_score = j.at("a_score").get<int>();
  in.b_score = j.at("b_score").get<int>();
  if (!j.at("t_left_s").is_null()) in.t_left_s = j.at("t_left_s").get<double>();
  in.ot = j.at("ot").get<bool>();
  in.points = j.at("points").get<bool>();
  in.set_end_ms = j.at("set_end_ms").get<std::int64_t>();
  if (!j.at("p0").is_null()) in.p0 = px_of(j.at("p0"));
  if (!j.at("fair").is_null()) in.fair = px_of(j.at("fair"));
  in.fair_why = j.at("fair_why").get<std::string>();
  if (!j.at("espn").is_null()) in.espn = px_of(j.at("espn"));
  in.espn_seen_ms = j.at("espn_seen_ms").get<std::int64_t>();
  in.now_ms = j.at("now_ms").get<std::int64_t>();
  in.last_take_ms = j.at("last_take_ms").get<std::int64_t>();
  in.game_used = money_of(j.at("game_used")) * 100;
  in.game_takes = j.at("game_takes").get<int>();
  in.fired[0] = j.at("fired").at(0).get<bool>();
  in.fired[1] = j.at("fired").at(1).get<bool>();
  in.private_up = j.at("private_up").get<bool>();
  in.not_tradable = j.at("not_tradable").get<std::string>();
  in.volume = qty_of(j.at("volume"));
  // the engine fills these from GameFair and the score board; every case sets them all
  for (const char* k : {"a", "b", "state", "src", "score_why", "fetched_ms", "first_seen_ms", "start_ms", "a_score", "b_score",
                        "t_left_s", "ot", "points", "set_end_ms", "p0", "fair", "fair_why", "espn", "espn_seen_ms", "now_ms",
                        "last_take_ms", "game_used", "game_takes", "fired", "private_up", "not_tradable", "volume"})
    CHECK(j.contains(k));
  return g;
}

std::string dir_str(int d) { return d == 0 ? "A" : d == 1 ? "B" : ""; }
std::string said(const TakeDecision& d) {
  if (d.take)
    return "take " + std::string(d.take->side == Side::BUY ? "BUY " : "SELL ") + d.take->ticker + " " + price_str(d.take->price) +
           " x" + std::to_string(d.take->count) + " fee " + money_str(d.take->fee) + " edge " + money_str(d.take->edge) +
           " dir " + dir_str(d.take->dir);
  return "pull " + d.pull + " (" + d.why + ") edge " + (d.edge ? money_str(*d.edge) : std::string("none")) + " dir " + dir_str(d.dir);
}

// Every case of one fixture file gives its 'want'; the file's params for each mode are deskd's
// defaults, so the Python tools start from the same rule. Returns the number of cases run.
int run_file(const std::string& name) {
  const json f = read_json(fixture(name));
  CHECK(!f.is_discarded() && f.value("kind", "") == "take_cases" && f.value("v", 0) == 1);
  if (f.is_discarded()) return 0;
  for (const auto& [mode_s, pj] : f.at("params").items()) {
    const auto mode = parse_take_mode(mode_s);
    CHECK(mode.has_value());
    if (!mode) continue;
    const bool same = same_params(params_of(pj, *mode), take_params(*mode));
    if (!same) std::fprintf(stderr, "%s: params.%s are not take_params(%s)\n", name.c_str(), mode_s.c_str(), mode_s.c_str());
    CHECK(same);
    CHECK(f.at("base").contains(mode_s));
  }
  std::set<std::string> names;
  int n = 0;
  for (const auto& c : f.at("cases")) {
    const std::string cname = c.at("name").get<std::string>(), mode_s = c.at("mode").get<std::string>();
    CHECK(names.insert(cname).second);   // each name once
    const auto mode = parse_take_mode(mode_s);
    CHECK(mode.has_value());
    if (!mode) continue;
    const TakeParams p = params_of(merge(f.at("params").at(mode_s), c.value("params", json::object())), *mode);
    const Game g = game_of(merge(f.at("base").at(mode_s), c.value("in", json::object())));
    const TakeDecision d = take_decide(g.in, p);
    const json& w = c.at("want");
    bool ok;
    if (w.contains("take")) {
      const json& t = w.at("take");
      ok = d.take && d.pull.empty() && d.take->ticker == t.at("ticker").get<std::string>() &&
           (d.take->side == Side::BUY ? "BUY" : "SELL") == t.at("side").get<std::string>() &&
           d.take->price == px_of(t.at("price")) && d.take->count == qty_of(t.at("count")) &&
           d.take->fee == money_of(t.at("fee")) && d.take->edge == money_of(t.at("edge")) &&
           dir_str(d.take->dir) == t.at("dir").get<std::string>() && d.edge == d.take->edge && d.dir == d.take->dir;
    } else {
      seen_pulls.insert(w.at("pull").get<std::string>());
      const bool edge = w.at("edge").is_null() ? !d.edge : d.edge && *d.edge == money_of(w.at("edge"));
      ok = !d.take && d.pull == w.at("pull").get<std::string>() && !d.why.empty() && edge &&
           dir_str(d.dir) == (w.at("dir").is_null() ? std::string() : w.at("dir").get<std::string>());
    }
    if (!ok) std::fprintf(stderr, "%s %s: %s; want %s\n", name.c_str(), cname.c_str(), said(d).c_str(), w.dump().c_str());
    CHECK(ok);
    ++n;
  }
  return n;
}

// ---- the tests

void clip_counts() {
  // nba.json's: $2 at 0.48 is 4 (1.92 + 0.07 = 1.99), at 0.97 is 2 (1.94 + 0.01); never 0 while
  // one contract fits: $1 at 0.99 is 1 (0.99 + 0.01)
  CHECK(clip_count(20000, 4800, 1, 0, true) == 400);
  CHECK(clip_count(20000, 9700, 1, 0, true) == 200);
  CHECK(clip_count(10000, 9900, 1, 0, true) == 100);
  CHECK(clip_count(5000, 6000, 1, 0, true) == 0);   // $0.50 buys nothing at 0.60
  // whole contracts unless the market trades fractions: $2 at 0.30 is 6 (1.80 + 0.09) or 6.33
  // (1.899 + 0.10 = 1.999; 6.34 would be 2.002)
  CHECK(clip_count(20000, 3000, 1, 0, true) == 600);
  CHECK(clip_count(20000, 3000, 1, 0, false) == 633);
  CHECK(clip_count(20000, 3000, 1, 400, true) == 400);   // max_count
  CHECK(clip_count(20000, 3000, 1, 450, true) == 400);   // a cap between whole contracts
  CHECK(clip_count(20000, 4800, 0.5, 0, true) == 400);
  CHECK(clip_count(20000, 4800, 0, 0, false) == 416);    // no fee: 4.16 x 0.48 = 1.9968
  CHECK(clip_count(20000, 0, 1, 0, true) == 0 && clip_count(20000, kOneDollar, 1, 0, true) == 0);
  CHECK(clip_count(0, 4800, 1, 0, true) == 0 && clip_count(-20000, 4800, 1, 0, true) == 0);
  // the count is the largest that fits: one more step never does
  for (Px px : {100, 1700, 4500, 5000, 8300, 9900})
    for (Money clip : {10000, 20000, 50000})
      for (bool whole : {true, false}) {
        const Qty c = clip_count(clip, px, 1, 0, whole), step = whole ? kOneContract : 1;
        CHECK(px * c + 100 * kalshi_fee_order_cent(px, c, 1) <= clip * 100);
        CHECK(px * (c + step) + 100 * kalshi_fee_order_cent(px, c + step, 1) > clip * 100);
      }
  // the cent rule the takes are sized with (college.json): 5 at 0.40 is $0.09 (0.084), 1 at 0.50
  // $0.02 (0.0175), and the multiplier scales the raw fee before rounding
  CHECK(kalshi_fee_order_cent(4000, 500, 1) == 900);
  CHECK(kalshi_fee_order_cent(5000, 100, 1) == 200);
  CHECK(kalshi_fee_order_cent(4000, 500, 0.5) == 500);
}

void defaults() {
  for (TakeMode m : {TakeMode::COLLEGE, TakeMode::MODEL, TakeMode::ESPN_FOLLOW, TakeMode::ESPN_FADE, TakeMode::FAV_PREGAME}) {
    CHECK(parse_take_mode(take_mode_str(m)) == m);
    CHECK(take_params(m).mode == m);
    CHECK(take_params(m).game_cap == 10000000);   // $10 a game, every lane
  }
  CHECK(!parse_take_mode("college") && !parse_take_mode(""));
  const TakeParams c = take_params(TakeMode::COLLEGE), n = take_params(TakeMode::MODEL), f = take_params(TakeMode::FAV_PREGAME);
  CHECK(c.edge == 2000 && c.clip == 20000 && c.max_score_age_ms == 5000 && c.cool_ms == 5000 && c.stale_only &&
        c.max_volume == 20000 * kOneContract && !c.leader_only && c.points_required && c.set_window_ms == 120000 && c.max_spread == 0);
  CHECK(n.edge == 1000 && n.max_count == 400 && n.max_takes == 4 && n.signal_ms == 30000 && n.cool_ms == 10000 &&
        n.max_spread == 300 && n.min_px == 500 && n.max_px == 9500 && !n.trade_ot && n.min_t_left_s == 120 && !n.stale_only);
  CHECK(same_params(take_params(TakeMode::ESPN_FOLLOW), [] {
    auto p = take_params(TakeMode::MODEL);
    p.mode = TakeMode::ESPN_FOLLOW;
    p.inventory = 400;   // X1's 4 contracts a market, N1's 10
    return p;
  }()));
  CHECK(f.fixed_count == 500 && f.fav_lo == 9000 && f.fav_hi == 9500 && f.fav_from_ms == 3600000 && f.fav_to_ms == 120000 &&
        f.max_spread == 1000 && f.edge == 0);
  // each lane's inventory is the one gw holds it to
  for (const auto& [m, lane] : std::vector<std::pair<TakeMode, std::string>>{{TakeMode::COLLEGE, "C1"},
                                                                            {TakeMode::MODEL, "N1"},
                                                                            {TakeMode::ESPN_FOLLOW, "X1"},
                                                                            {TakeMode::ESPN_FADE, "X1"},
                                                                            {TakeMode::FAV_PREGAME, "N2"}}) {
    const auto l = std::find_if(default_lanes().begin(), default_lanes().end(), [&lane = lane](const LaneSpec& s) { return s.id == lane; });
    CHECK(l != default_lanes().end() && take_params(m).inventory == l->inventory);
  }
  CHECK(c.inventory == 2000 && n.inventory == 1000 && f.inventory == 500);
  // every code once, in the checks' order
  const auto& codes = take_pull_codes();
  CHECK(std::set<std::string>(codes.begin(), codes.end()).size() == codes.size() && codes.size() == 32);
}

void fixtures() {
  const int c = run_file("college_take_cases.json"), x = run_file("x1_golden.json"), n = run_file("n1_golden.json");
  CHECK(c >= 40 && x >= 25 && n >= 35);
}

// C1's base game, built here: A's team at 0.45 against a 0.70 fair
Game college_game() {
  const json f = read_json(fixture("college_take_cases.json"));
  return game_of(f.at("base").at("COLLEGE"));
}

// The same game listed the other way round is the same take, for the other team
void mirrored() {
  const TakeParams p = take_params(TakeMode::COLLEGE);
  const Game g = college_game();
  Game m = g;
  std::swap(m.book_a, m.book_b);
  std::swap(m.in.a, m.in.b);
  m.in.a.book = &m.book_a;
  m.in.b.book = &m.book_b;
  std::swap(m.in.a_score, m.in.b_score);
  m.in.p0 = kOneDollar - *g.in.p0;
  m.in.fair = kOneDollar - *g.in.fair;
  const TakeDecision d = take_decide(g.in, p), e = take_decide(m.in, p);
  CHECK(d.take && e.take);
  if (!d.take || !e.take) return;
  CHECK(d.take->dir == 0 && e.take->dir == 1);
  CHECK(e.take->ticker == d.take->ticker && e.take->side == d.take->side && e.take->price == d.take->price &&
        e.take->count == d.take->count && e.take->fee == d.take->fee && e.take->edge == d.take->edge);
  // and a pull names the mirrored team
  m.in.fired[1] = true;
  const TakeDecision f = take_decide(m.in, p);
  CHECK(f.pull == "FIRED" && f.dir == 1 && has(f.why, "AAA"));
}

void order_fields() {
  const TakeParams p = take_params(TakeMode::COLLEGE);
  Game g = college_game();
  TakeDecision d = take_decide(g.in, p);
  CHECK(d.take && d.take->team_px == 4500 && d.take->ref == 7000 && d.take->cost == 4500 * 400 + 100 * 700 &&
        d.take->touch_since_ms == 9000000 && d.take->side == Side::BUY);
  g.book_a.asks.clear();   // through B: SELL at B's bid, A's team at 1 - it
  d = take_decide(g.in, p);
  CHECK(d.take && d.take->ticker == g.in.b.ticker && d.take->side == Side::SELL && d.take->price == 5200 &&
        d.take->team_px == 4800 && d.take->ref == 7000 && d.take->cost == 4800 * 400 + 100 * 700);
  // FADE prices the team against ESPN's number
  const json f = read_json(fixture("x1_golden.json"));
  const Game x = game_of(f.at("base").at("ESPN_FADE"));
  d = take_decide(x.in, take_params(TakeMode::ESPN_FADE));
  CHECK(d.take && d.take->dir == 1 && d.take->ref == 3000 && d.take->team_px == 4500 && d.take->edge == 1450);
}

void whys() {
  const TakeParams p = take_params(TakeMode::COLLEGE);
  Game g = college_game();
  g.in.fetched_ms = g.in.now_ms - 5001;
  TakeDecision d = take_decide(g.in, p);
  CHECK(d.pull == "SCORE_OLD" && has(d.why, "5001 ms") && d.dir == -1 && !d.edge);
  g = college_game();
  g.in.fair.reset();
  g.in.fair_why = "no game clock in period 0";
  d = take_decide(g.in, p);
  CHECK(d.pull == "NO_FAIR" && has(d.why, "no game clock in period 0"));
  g = college_game();
  g.in.fair = 6000;
  d = take_decide(g.in, p);
  CHECK(d.pull == "NO_EDGE" && has(d.why, "0.1325") && has(d.why, "0.2000"));
  g = college_game();
  g.in.not_tradable = "the gate measured another model";
  d = take_decide(g.in, p);
  CHECK(d.pull == "NOT_TRADABLE" && has(d.why, "another model"));
  // a NaN clock or fee multiplier fails closed
  TakeParams n = p;
  n.min_t_left_s = 120;
  g = college_game();
  g.in.t_left_s = std::nan("");
  CHECK(take_decide(g.in, n).pull == "LATE");
  g = college_game();
  g.in.a.fee_mult = std::nan("");
  CHECK(take_decide(g.in, p).pull == "NO_FEE");
  g.in.a.fee_mult = -1;
  CHECK(take_decide(g.in, p).pull == "NO_FEE");
}

// A pull past a route's count carries that route's order (the engine's PULL row); a take, and a
// pull before any count, carry none
void routes() {
  const TakeParams p = take_params(TakeMode::COLLEGE);
  Game g = college_game();
  CHECK(take_decide(g.in, p).take && !take_decide(g.in, p).route);
  g.in.first_seen_ms = 9000000;   // the touches, first seen at 9000000, are not older than the score
  TakeDecision d = take_decide(g.in, p);
  CHECK(d.pull == "TOUCH_AFTER_SCORE" && !d.take && d.route && d.route->ticker == g.in.a.ticker && d.route->side == Side::BUY &&
        d.route->price == 4500 && d.route->count == 400 && d.route->fee == 700 && d.route->edge == 2325 && d.route->dir == 0 &&
        d.route->ref == 7000 && d.route->touch_since_ms == 9000000);
  g = college_game();
  g.in.fair = 6000;
  d = take_decide(g.in, p);
  CHECK(d.pull == "NO_EDGE" && d.route && d.edge && d.route->edge == *d.edge);
  g = college_game();
  g.in.fetched_ms = 0;
  CHECK(take_decide(g.in, p).pull == "SCORE_OLD" && !take_decide(g.in, p).route);
}

// With no game cap, no inventory limit and a clip far past the book, the touch is the bound
void unbounded() {
  TakeParams p = take_params(TakeMode::COLLEGE);
  p.game_cap = 0;
  p.inventory = 0;
  p.clip = 900000000;   // $90,000
  Game g = college_game();
  g.book_a.asks = {{4500, 123456 * kOneContract}};
  const TakeDecision d = take_decide(g.in, p);
  CHECK(d.take && d.take->count == 123456 * kOneContract && d.take->fee == kalshi_fee_order_cent(4500, 123456 * kOneContract, 1));
  CHECK(clip_count(900000000, 1, 1, 0, false) == 1000000 * kOneContract);   // at most a million contracts
}

// gw's inventory check (gateway.cpp place_sys): an order is refused when Systematic's position in
// the market, with its working orders on the side and the order, ends past the lane's limit and
// further from flat than it was
bool gw_refuses(Qty pos, Qty working, Side side, Qty count, Qty limit) {
  const Qty after = pos + (side == Side::BUY ? 1 : -1) * (working + count);
  return std::abs(after) > limit && std::abs(after) > std::abs(pos);
}

// The inventory room is gw's own: over Systematic's position in the market sent to, on either side
// of flat and past the limit, and its working orders on the side, a take is never one gw refuses
// for inventory, one step more would be whenever the room held the count down, and with no room
// the pull is INVENTORY. The other side's working orders and the other market's position do not
// count; inventory 0 is no cap.
void inventory() {
  TakeParams p = take_params(TakeMode::COLLEGE);
  p.game_cap = 0;
  p.clip = 1000000;   // $100: the room binds, not the clip
  for (const bool buy : {true, false}) {
    Game g = college_game();   // one route only: BUY A at 0.45, or SELL B at 0.52 (A's team at 0.48)
    g.book_a.bids.clear();
    g.book_b.asks.clear();
    (buy ? g.book_b.bids : g.book_a.asks).clear();
    (buy ? g.book_a.asks : g.book_b.bids).front().qty = 1000 * kOneContract;
    TakeLeg& leg = buy ? g.in.a : g.in.b;
    TakeLeg& other = buy ? g.in.b : g.in.a;
    const Side side = buy ? Side::BUY : Side::SELL;
    TakeParams free = p;
    free.inventory = 0;
    leg.sys_position = 500 * kOneContract;
    const TakeDecision f = take_decide(g.in, free);
    CHECK(f.take && f.take->count > 100 * kOneContract);
    if (!f.take) continue;
    const Qty most = f.take->count;   // what the clip buys
    other.sys_position = buy ? -50 * kOneContract : 50 * kOneContract;   // the other market's: not this one's
    (buy ? leg.sys_selling : leg.sys_buying) = 50 * kOneContract;        // the other side's: not this one's
    int capped = 0, none = 0;
    for (Qty pos = -45 * kOneContract; pos <= 45 * kOneContract; pos += 50)   // half contracts too
      for (const Qty w : {Qty{0}, Qty{100}, Qty{350}, Qty{2500}}) {
        leg.sys_position = pos;
        (buy ? leg.sys_buying : leg.sys_selling) = w;
        const TakeDecision d = take_decide(g.in, p);
        bool ok;
        if (d.take) {
          ok = d.take->ticker == leg.ticker && d.take->side == side && !gw_refuses(pos, w, side, d.take->count, p.inventory) &&
               (d.take->count == most || gw_refuses(pos, w, side, d.take->count + kOneContract, p.inventory));
          capped += d.take->count < most;
        } else {
          ok = d.pull == "INVENTORY" && d.dir == 0 && !d.edge && gw_refuses(pos, w, side, kOneContract, p.inventory);
          ++none;
        }
        if (!ok) std::fprintf(stderr, "inventory %s, %.2f held, %.2f working: %s\n", buy ? "BUY" : "SELL", qty_to_number(pos),
                              qty_to_number(w), said(d).c_str());
        CHECK(ok);
      }
    CHECK(capped > 100 && none > 100);
  }
  // the reason says what gw would (B's team has no touch: A's routes go furthest)
  Game g = college_game();
  g.book_a.bids.clear();
  g.book_b.asks.clear();
  g.in.a.sys_position = 2000;
  g.in.b.sys_position = -1500;
  g.in.b.sys_selling = 600;
  const TakeDecision d = take_decide(g.in, take_params(TakeMode::COLLEGE));
  CHECK(d.pull == "INVENTORY" && d.dir == 0 &&
        d.why == "the inventory room in KXNCAAWBGAME-26NOV14AAABBB-AAA buys no contract: 20.0 held, 0.0 working, limit 20.0");
}

void coverage() {
  for (const auto& c : take_pull_codes()) {
    const bool hit = seen_pulls.count(c) > 0;
    if (!hit) std::fprintf(stderr, "no case pulls %s\n", c.c_str());
    CHECK(hit);
  }
  for (const auto& c : seen_pulls) CHECK(std::find(take_pull_codes().begin(), take_pull_codes().end(), c) != take_pull_codes().end());
}

// ---- TakeEngine, against a fake Env: gw a stub inbox; the listing, books, boards and mids in
// memory; the store a list of rows

const std::int64_t T0 = 1794700800000;   // 2026-11-14 19:00 ET, the games' day
const std::string EV = "KXNCAAWBGAME-26NOV14AAABBB", TA = EV + "-AAA", TB = EV + "-BBB";

struct Rig {
  std::string ev, a, b;   // the game, its market A and market B
  std::shared_ptr<Gateway::SysView> view = std::make_shared<Gateway::SysView>();
  std::shared_ptr<rest::Snapshot> snap = std::make_shared<rest::Snapshot>();
  std::map<std::string, MdBook> books;
  GameBoard fairs;
  EspnBoard scores;
  std::map<std::string, std::string> tradable;
  bool private_up = true, inbox_full = false;
  std::pair<std::int64_t, Money> mo{0, 0};
  std::map<std::string, Px> mids;                             // mid_at's answer, at any time
  std::vector<std::pair<std::string, std::int64_t>> asked;    // what mid_at was asked
  std::vector<GwMsg> posted;
  std::vector<json> rows;                                     // the signals rows, as recorded
  std::set<std::string> wanted;
  std::vector<std::string> alerts;
  explicit Rig(std::string event = EV, std::string ta = TA, std::string tb = TB)
      : ev(std::move(event)), a(std::move(ta)), b(std::move(tb)) {
    view->run = RunState::RUN;
    view->lanes_on = {"C1"};
    tradable[ev.substr(0, ev.find('-'))] = "";
    market(a, ev);
    market(b, ev);
    book(a, 4000, 4500);
    book(b, 5200, 5800);
  }
  void market(const std::string& tk, const std::string& event, std::optional<Qty> volume = 3000 * kOneContract) {
    kalshi::MarketRow m;
    m.ticker = tk;
    m.event_ticker = event;
    m.status = "active";
    m.close_ms = T0 + 7 * 86400000LL;
    m.fee_type = "quadratic";
    m.fee_multiplier = 1.0;
    m.volume = volume;
    snap->markets[tk] = m;
  }
  void book(const std::string& tk, Px bid, Px ask, Qty q = 10 * kOneContract) {
    MdBook x;
    x.ticker = tk;
    x.stale = false;
    x.bids = {{bid, q}};
    x.asks = {{ask, q}};
    books[tk] = x;
  }
  // The game's score from A's side, and GameFair's rows priced on that state (A's p; B's 1 - p)
  void score(std::int64_t first_seen, std::int64_t fetched, std::optional<double> p, int sa = 60, int sb = 48,
             const std::string& state = "in", int period = 2, double clock_s = 300) {
    for (const auto& tk : {a, b}) {
      const bool is_a = tk == a;
      EspnRef r;
      r.state = state;
      r.fetched_ms = fetched;
      r.first_seen_ms = first_seen;
      r.start_ms = T0 - 3600000;
      r.yes_score = is_a ? sa : sb;
      r.opp_score = is_a ? sb : sa;
      r.period = period;
      r.clock_s = clock_s;
      r.per = "H2";
      r.clock = "5:00";
      r.yes_home = is_a;
      scores[tk] = r;
      GameFairRow f;
      f.event = ev;
      f.model = "wcbb_logit";
      f.p0 = is_a ? 0.55 : 0.45;
      if (p) f.p = is_a ? *p : 1 - *p;
      f.state = state;
      f.src = "ESPN";
      f.first_seen_ms = first_seen;
      f.fetched_ms = fetched;
      fairs[tk] = f;
    }
  }
  TakeEngine::Env env() {
    TakeEngine::Env e;
    e.view = [this] { return std::shared_ptr<const Gateway::SysView>(view); };
    e.snapshot = [this] { return std::shared_ptr<const rest::Snapshot>(snap); };
    e.book = [this](const std::string& tk) {
      const auto it = books.find(tk);
      return it == books.end() ? std::nullopt : std::optional<MdBook>(it->second);
    };
    e.fairs = [this] { return std::make_shared<const GameBoard>(fairs); };
    e.scores = [this] { return std::make_shared<const EspnBoard>(scores); };
    e.tradable = [this] { return tradable; };
    e.private_up = [this] { return private_up; };
    e.post = [this](GwMsg m) {
      if (inbox_full) return false;
      posted.push_back(std::move(m));
      return true;
    };
    e.want = [this](std::set<std::string> w) { wanted = std::move(w); };
    e.markout = [this] { return mo; };
    e.mid_at = [this](const std::string& tk, std::int64_t at) {
      asked.emplace_back(tk, at);
      const auto it = mids.find(tk);
      return it == mids.end() ? std::nullopt : std::optional<Px>(it->second);
    };
    e.record = [this](const std::string& table, std::string row) {
      CHECK(table == "signals");
      rows.push_back(json::parse(row));
      return true;
    };
    e.alert = [this](const std::string& lvl, const std::string& msg) { alerts.push_back(lvl + " " + msg); };
    return e;
  }
  std::vector<PlaceIntent*> places() {
    std::vector<PlaceIntent*> out;
    for (auto& m : posted)
      if (auto* p = std::get_if<PlaceIntent>(&m)) out.push_back(p);
    return out;
  }
  void answer(std::size_t i, OrderReply r) { places().at(i)->reply->set_value(std::move(r)); }
};
json game_status(const TakeEngine& e, const std::string& ev = EV) { return e.status()["games"][ev]; }

// The first look dates the touches; a score after it with the edge takes once: one IOC with every
// field, and its row; the game waits for gw's answer, then the cool-down, then the run's team is
// fired; a new score state is a new run
void engine_takes() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  CHECK(g.posted.empty() && g.rows.empty() && g.wanted == std::set<std::string>({TA, TB}));
  CHECK(game_status(e)["pull"] == "NO_EDGE" && game_status(e)["fair"] == "0.5000");
  // A's team scores: its fair 0.70 against the 0.45 ask, first seen before the score
  g.score(T0 + 3000, T0 + 3000, 0.70);
  e.step(T0 + 3200);
  CHECK(g.places().size() == 1);
  if (g.places().size() != 1) return;
  const PlaceIntent& p = *g.places()[0];
  CHECK(p.cid.rfind("SC1-", 0) == 0 && p.cid.size() <= 40 && p.owner == Owner::SYS && p.lane == "C1" && p.venue == Venue::K &&
        p.ticker == TA && p.side == Side::BUY && p.price == 4500 && p.count == 400 && p.tif == Tif::IOC && p.ttl_s == 0 &&
        !p.reduce_only && !p.override_guard && p.touch == Px{4500} && p.fair == Px{7000} && p.t_recv_ms == T0 + 3200 && p.reply);
  CHECK(g.rows.size() == 1);
  const json row = g.rows.back();
  CHECK(row["action"] == "TAKE" && row["shadow"] == false && row["lane"] == "C1" && row["mode"] == "COLLEGE" && row["tk"] == TA &&
        row["ev"] == EV && row["side"] == "BUY" && row["px"] == "0.4500" && row["qty"] == 4 && row["fair"] == "0.7000" &&
        row["edge"] == "0.2325" && row["fee"] == "0.0700" && row["dir"] == "A" && row["score"] == "60-48 H2 5:00" &&
        row["src"] == "ESPN" && row["age_ms"] == 200 && row["first_seen_ms"] == T0 + 3000 && row["touch_since_ms"] == T0 &&
        row["cid"] == p.cid && row["ts_ms"] == T0 + 3200 && row["mo10"].is_null() && row["mo60"].is_null() && row["mo300"].is_null());
  CHECK(row["id"] == "C1-" + EV + "-" + std::to_string(T0 + 3200) + "-A");
  CHECK(e.status()["takes"] == 1 && game_status(e)["pull"] == "" && game_status(e)["takes"] == 1);
  // gw has not answered: the game waits
  e.step(T0 + 3450);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "WAIT");
  g.answer(0, OrderReply{});
  e.step(T0 + 3700);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "COOL_DOWN" && g.rows.size() == 1);
  // past the cool-down, A's team is taken in this run (both its routes)
  g.score(T0 + 3000, T0 + 8200, 0.70);
  e.step(T0 + 8300);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED" && game_status(e)["dir"] == "A");
  // a new score state is a new run: A's team again
  g.score(T0 + 9000, T0 + 9000, 0.70, 62, 48);
  e.step(T0 + 9100);
  CHECK(g.places().size() == 2 && g.places().back()->ticker == TA && g.rows.size() == 2 && g.rows.back()["first_seen_ms"] == T0 + 9000);
  CHECK(e.status()["takes"] == 2 && game_status(e)["takes"] == 2 && e.status()["refusals"] == 0);
}

// A run ends run_quiet_ms after its edge was last seen, judged by decisions that show no edge; a
// check of the game's that hides the edge (an old score) neither ends nor extends it
void engine_runs() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  g.score(T0 + 3000, T0 + 3000, 0.70);
  e.step(T0 + 3200);
  g.answer(0, OrderReply{});
  g.score(T0 + 3000, T0 + 8200, 0.70);
  e.step(T0 + 8300);   // the edge, fired: its last sighting
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED");
  g.score(T0 + 3000, T0 + 9000, 0.50);   // the same state, the clock run: no edge
  e.step(T0 + 9000);
  CHECK(game_status(e)["pull"] == "NO_EDGE");
  g.score(T0 + 3000, T0 + 9200, 0.70);
  e.step(T0 + 9250);   // back within the quiet time: still the run's
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED");
  e.step(T0 + 25000);  // the score 15.8 s old hides the edge: neither the end nor the edge
  CHECK(game_status(e)["pull"] == "SCORE_OLD");
  g.score(T0 + 3000, T0 + 25200, 0.70);
  e.step(T0 + 25250);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED");
  g.score(T0 + 3000, T0 + 35000, 0.50);
  e.step(T0 + 35249);  // 9,999 ms without
  g.score(T0 + 3000, T0 + 35250, 0.50);
  e.step(T0 + 35250);  // 10 s without: the run ends
  g.score(T0 + 3000, T0 + 35400, 0.70);
  e.step(T0 + 35500);  // a new run in the same score state
  CHECK(g.places().size() == 2 && g.rows.back()["action"] == "TAKE" && g.rows.back()["id"] != g.rows.front()["id"]);
}

// The score board's row when ESPN and NCAA.com disagree (ref/scores.h): no score, with the
// sources' why; it neither starts a signal run nor ends one, so when they agree again on the state
// before, the team that run took is not taken again
void engine_scores() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  g.score(T0 + 3000, T0 + 3000, 0.70);
  e.step(T0 + 3200);
  CHECK(g.places().size() == 1);
  g.answer(0, OrderReply{});
  const std::string why = "ESPN has in 60-48 (period 2) and NCAA in 58-48 (period 2)";
  for (const auto& tk : {TA, TB}) {
    EspnRef none;
    none.src = "";
    none.why = why;
    g.scores[tk] = none;
  }
  e.step(T0 + 8300);   // past the cool-down
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "NO_SCORE" && game_status(e)["why"] == "no score: " + why &&
        game_status(e)["state"] == "");
  g.score(T0 + 3000, T0 + 8500, 0.70);
  e.step(T0 + 8600);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED" && game_status(e)["dir"] == "A");
}

// The merged board (ref/scores.h) between ESPN's and NCAA.com's: NCAA.com sees A's team go up
// 60-48 first, ESPN 1.5 s later, and C1 takes A's team. NCAA.com then misses polls past the max
// age, comes back, and leaves its board: the state's first sighting stays NCAA.com's, so the run is
// the same and A's team, FIRED, is not taken again past the cool-down. Then ESPN calls the game
// final and slows down while NCAA.com, lagging in play, is fresh with A's team ahead and the edge:
// the game is final, nothing is taken
void engine_merged() {
  Rig g;
  EspnBoard espn, ncaa;
  ScoreMerge::Env se;
  se.espn = [&] { return std::make_shared<const EspnBoard>(espn); };
  se.ncaa = [&] { return std::make_shared<const EspnBoard>(ncaa); };
  ScoreMerge sm(se);
  TakeEngine::Env env = g.env();
  env.scores = [&] { return sm.board(); };
  TakeEngine e("C1", {"KXNCAAWBGAME"}, env, take_params(TakeMode::COLLEGE));
  // a source's rows for the game, from each team's side
  auto rows = [&](EspnBoard& b, const std::string& src, std::int64_t fetched, std::int64_t first, std::int64_t prev, int sa,
                  int sb, const std::string& state = "in") {
    for (const auto& tk : {TA, TB}) {
      EspnRef r;
      r.src = src;
      r.state = state;
      r.fetched_ms = fetched;
      r.first_seen_ms = first;
      r.prev_poll_ms = prev;
      r.start_ms = T0 - 3600000;
      r.yes_score = tk == TA ? sa : sb;
      r.opp_score = tk == TA ? sb : sa;
      r.period = 2;
      r.clock_s = 300;
      r.yes_home = tk == TA;
      b[tk] = r;
    }
  };
  // the merge, GameFair priced on its rows (A's p), then the engine: strat's order
  auto step = [&](std::int64_t now, double p) {
    sm.step(now);
    for (const auto& tk : {TA, TB}) {
      const EspnRef& r = sm.board()->at(tk);
      GameFairRow& f = g.fairs[tk];
      f.event = EV;
      f.model = "wcbb_logit";
      f.p0 = tk == TA ? 0.55 : 0.45;
      f.p = tk == TA ? p : 1 - p;
      f.state = r.state;
      f.src = r.src;
      f.first_seen_ms = r.first_seen_ms;
      f.fetched_ms = r.fetched_ms;
    }
    e.step(now);
  };
  rows(espn, "ESPN", T0 - 1000, T0 - 60000, T0 - 63000, 58, 48);
  rows(ncaa, "NCAA", T0 - 2000, T0 - 61000, T0 - 64000, 58, 48);
  step(T0, 0.50);   // the touches dated
  CHECK(g.posted.empty() && game_status(e)["pull"] == "NO_EDGE");
  rows(ncaa, "NCAA", T0 + 3000, T0 + 3000, T0, 60, 48);
  rows(espn, "ESPN", T0 + 4500, T0 + 4500, T0 + 1500, 60, 48);
  step(T0 + 4600, 0.70);
  CHECK(g.places().size() == 1 && sm.board()->at(TA).first_seen_ms == T0 + 3000);
  if (g.places().size() != 1) return;
  g.answer(0, OrderReply{});
  // NCAA.com's polls are late: ESPN's row alone is fresh, past the cool-down
  for (std::int64_t t = T0 + 8100; t <= T0 + 11000; t += 250) {
    rows(espn, "ESPN", t, T0 + 4500, T0 + 1500, 60, 48);
    step(t, 0.70);
  }
  CHECK(g.places().size() == 1 && sm.board()->at(TA).src == "ESPN" && sm.board()->at(TA).first_seen_ms == T0 + 3000 &&
        game_status(e)["pull"] == "FIRED" && game_status(e)["dir"] == "A");
  rows(ncaa, "NCAA", T0 + 11500, T0 + 3000, T0, 60, 48);   // back
  step(T0 + 11600, 0.70);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED");
  ncaa.clear();   // off its board
  rows(espn, "ESPN", T0 + 140000, T0 + 4500, T0 + 1500, 60, 48);
  step(T0 + 140000, 0.70);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FIRED" && sm.board()->at(TA).first_seen_ms == T0 + 3000);
  // ESPN: 60-61 final, then 20 s scoreboards; NCAA.com fresh on 60-59, a state it saw after the touch
  rows(espn, "ESPN", T0 + 150000, T0 + 150000, T0 + 147000, 60, 61, "post");
  rows(ncaa, "NCAA", T0 + 158000, T0 + 149000, T0 + 146000, 60, 59);
  step(T0 + 158000, 0.95);
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "FINAL" && g.wanted.empty());
}

// gw's refusal: the row says so and the game backs off 10 s; an inbox that is full is one too; an
// answer that never comes is waited for 30 s
void engine_refused() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  g.score(T0 + 3000, T0 + 3000, 0.70);
  e.step(T0 + 3200);
  CHECK(g.places().size() == 1);
  g.answer(0, OrderReply{"SYS_CEILING", "lane C1's worst case in event " + EV + " past 10.0000", ""});
  e.step(T0 + 3450);
  CHECK(g.rows.size() == 2 && g.rows.back()["id"] == g.rows.front()["id"] && g.rows.back()["action"] == "REFUSED" &&
        g.rows.back()["why"] == "SYS_CEILING: lane C1's worst case in event " + EV + " past 10.0000");
  CHECK(game_status(e)["pull"] == "BACKOFF" && e.status()["refusals"] == 1);
  g.score(T0 + 6000, T0 + 6000, 0.70);   // a new run with the edge: nothing for 10 s
  for (std::int64_t t = T0 + 6000; t < T0 + 13450; t += 250) {
    g.score(T0 + 6000, t, 0.70);
    e.step(t);
  }
  CHECK(g.places().size() == 1 && game_status(e)["pull"] == "BACKOFF");
  g.score(T0 + 6000, T0 + 13450, 0.70);
  e.step(T0 + 13450);
  CHECK(g.places().size() == 2);
  // no answer in 30 s: it stops waiting, says so, and backs off
  e.step(T0 + 13450 + 30000);
  CHECK(game_status(e)["pull"] == "WAIT" && g.alerts.empty());
  e.step(T0 + 13451 + 30000);
  CHECK(g.alerts.size() == 1 && g.alerts[0] == "WARN C1 " + EV + ": no answer from gw in 30000 ms" &&
        g.rows.back()["action"] == "TAKE" && g.rows.back()["why"] == "no answer from gw in 30000 ms");
  CHECK(game_status(e)["pull"] == "BACKOFF" && e.status()["refusals"] == 1);
  // gw's inbox full: refused there and then
  Rig h;
  h.inbox_full = true;
  TakeEngine f("C1", {"KXNCAAWBGAME"}, h.env(), take_params(TakeMode::COLLEGE));
  h.score(T0 - 60000, T0 - 1000, 0.50);
  f.step(T0);
  h.score(T0 + 3000, T0 + 3000, 0.70);
  f.step(T0 + 3200);
  CHECK(h.posted.empty() && h.rows.size() == 1 && h.rows[0]["action"] == "REFUSED" && h.rows[0]["why"] == "gw's inbox is full");
  CHECK(game_status(f)["pull"] == "BACKOFF" && f.status()["refusals"] == 1);
}

// What stops a post: the lane off, Systematic stopped, a market Manual's, the gate, the private
// feed, the markout hold. Without shadow nothing is recorded (and an off lane wants no books);
// with it, one SHADOW row a run says why, and counts as the run's take
void engine_blocked() {
  {
    Rig g;
    g.view->lanes_on.clear();
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    e.step(T0 + 3200);
    CHECK(g.posted.empty() && g.rows.empty() && g.wanted.empty() && game_status(e)["pull"] == "OFF");
  }
  struct Case {
    std::string why;
    std::function<void(Rig&)> set;
  };
  const std::vector<Case> cases = {
      {"lane C1 is off", [](Rig& r) { r.view->lanes_on.clear(); }},
      {"Systematic is not running", [](Rig& r) { r.view->run = RunState::STOP; }},
      {TB + " is held by Manual", [](Rig& r) { r.view->held = {TB}; }},
      {"not tradable: the gate measured another model", [](Rig& r) { r.tradable["KXNCAAWBGAME"] = "the gate measured another model"; }},
      {"not tradable: no gate for KXNCAAWBGAME", [](Rig& r) { r.tradable.clear(); }},
      {"the private feed is down", [](Rig& r) { r.private_up = false; }},
      {"markout hold: 100 fills marked, mean 0.0000 a contract", [](Rig& r) { r.mo = {100, 0}; }},
  };
  for (const bool shadow : {false, true})
    for (const auto& c : cases) {
      Rig g;
      c.set(g);
      TakeOptions o;
      o.shadow = shadow;
      o.mo_after = 100;
      TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE), o);
      g.score(T0 - 60000, T0 - 1000, 0.50);
      e.step(T0);
      g.score(T0 + 3000, T0 + 3000, 0.70);
      e.step(T0 + 3200);
      g.score(T0 + 3000, T0 + 8200, 0.70);
      e.step(T0 + 8300);   // past the cool-down: the run's team already shadowed
      bool ok = g.posted.empty();
      if (shadow) {
        ok = ok && g.rows.size() == 1 && g.rows[0]["action"] == "SHADOW" && g.rows[0]["shadow"] == true && g.rows[0]["why"] == c.why &&
             g.rows[0]["cid"].is_null() && g.rows[0]["tk"] == TA && g.rows[0]["px"] == "0.4500" && e.status()["shadows"] == 1 &&
             g.wanted == std::set<std::string>({TA, TB});
        // the status says what blocks the lane; the rule's own decision is the run's FIRED
        const json pull = game_status(e)["pull"];
        ok = ok && (c.why.rfind("not tradable", 0) == 0 ? pull == "NOT_TRADABLE"
                    : c.why == "the private feed is down" ? pull == "PRIVATE_DOWN"
                                                          : pull == "FIRED");
      } else {
        const json s = game_status(e);
        const bool lane_off = c.why == "lane C1 is off";
        ok = ok && g.rows.empty() && e.status()["shadows"] == 0 &&
             (lane_off ? s["pull"] == "OFF"
                       : c.why.rfind("not tradable", 0) == 0 ? s["pull"] == "NOT_TRADABLE" && g.wanted.empty()
                       : c.why == "the private feed is down" ? s["pull"] == "PRIVATE_DOWN"
                                                             : s["pull"] == "BLOCKED" && s["why"] == c.why);
      }
      if (!ok)
        std::fprintf(stderr, "blocked (%s, shadow %d): status %s, %zu rows\n", c.why.c_str(), shadow, game_status(e).dump().c_str(),
                     g.rows.size());
      CHECK(ok);
    }
  // the hold engages at 100 fills with a mean at or under -mo_floor (alerted once), not before
  for (const auto& [mo, holds] : std::vector<std::pair<std::pair<std::int64_t, Money>, bool>>{
           {{100, 0}, true}, {{140, -30}, true}, {{99, -500}, false}, {{100, 1}, false}}) {
    Rig g;
    g.mo = mo;
    TakeOptions o;
    o.mo_after = 100;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE), o);
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    e.step(T0 + 3200);
    e.step(T0 + 3450);
    CHECK(g.places().size() == (holds ? 0u : 1u) && g.alerts.size() == (holds ? 1u : 0u));
    CHECK(e.status()["hold"].is_null() == !holds);
  }
}

// A team with the edge that a later check pulls is recorded once a run as PULL, with the route it
// would have taken: here a touch first seen after the score (the engine's first look)
void engine_pulls() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 5000, T0 - 500, 0.70);
  e.step(T0);
  CHECK(g.posted.empty() && g.rows.size() == 1 && game_status(e)["pull"] == "TOUCH_AFTER_SCORE");
  if (g.rows.size() != 1) return;
  const json row = g.rows[0];
  CHECK(row["action"] == "PULL" && row["shadow"] == false && row["tk"] == TA && row["side"] == "BUY" && row["px"] == "0.4500" &&
        row["qty"] == 4 && row["edge"] == "0.2325" && row["touch_since_ms"] == T0 && row["first_seen_ms"] == T0 - 5000 &&
        row["cid"].is_null() && row["why"].get<std::string>().rfind("TOUCH_AFTER_SCORE: ", 0) == 0);
  e.step(T0 + 250);
  CHECK(g.rows.size() == 1 && e.status()["pulls"] == 1);
  // a new ask, newer still; then a score after it: the take, at that ask
  g.book(TA, 4000, 4400);
  g.score(T0 - 5000, T0 + 400, 0.70);
  e.step(T0 + 500);
  CHECK(g.posted.empty() && g.rows.size() == 1);
  g.score(T0 + 1000, T0 + 1000, 0.70);
  e.step(T0 + 1100);
  CHECK(g.places().size() == 1 && g.places()[0]->price == 4400 && g.rows.size() == 2 && g.rows[1]["touch_since_ms"] == T0 + 500);
}

// The inputs the engine builds: GameFair's fair only on the board's score state; its worst case in
// the game from SysView::worst; Systematic's position and working orders in each market, as gw's
// inventory counts them; everyone else's book; the in-game volume; the basketball clock;
// volleyball's set ends; ESPN's number and its date
void engine_inputs() {
  {   // GameFair a step behind the board: no fair this step; no row at all: no p0
    Rig g;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    g.fairs[TA].first_seen_ms = T0 - 60000;
    e.step(T0 + 3200);
    CHECK(g.posted.empty() && game_status(e)["pull"] == "NO_FAIR" && has(game_status(e)["why"], "a step behind"));
    g.fairs[TA].first_seen_ms = T0 + 3000;
    e.step(T0 + 3450);
    CHECK(g.places().size() == 1);
    g.answer(0, OrderReply{});
    g.score(T0 + 3000, T0 + 19900, 0.70);
    g.fairs.clear();
    e.step(T0 + 20000);
    CHECK(game_status(e)["pull"] == "NO_P0");
  }
  {   // the lane's worst case in the game, both markets (another lane's is not its)
    Rig g;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    g.view->worst = {{"C1|" + TA, 9000000}, {"C1|" + TB, 1000000}, {"M1|" + TA, 5000000}};
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    e.step(T0 + 3200);
    CHECK(g.posted.empty() && game_status(e)["pull"] == "GAME_CAP");
    g.view->worst = {{"C1|" + TA, 8500000}, {"M1|" + TA, 5000000}};   // $1.50 of room: 3 at 0.45 and a 6c fee
    e.step(T0 + 3450);
    CHECK(g.places().size() == 1 && g.places()[0]->count == 300);
  }
  {   // gw's inventory measure: every lane's position in the market, and what is left of Systematic's
      // orders there not ended, on the side: 15 (C1) + 3 (M1) of A held and 1 working leave 1 of
      // C1's 20, so BUY A at 0.45 x1 (edge 0.2300) over SELL B (0.2025); B's position and the
      // other side's order are not A's buy side
    Rig g;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    g.view->positions = {{"C1|" + TA, 1500}, {"M1|" + TA, 300}, {"M1|" + TB, -500}};
    for (const auto& [cid, side, left, state] : std::vector<std::tuple<std::string, Side, Qty, OrdState>>{
             {"SM1-a", Side::BUY, 100, OrdState::WORK}, {"SM1-b", Side::BUY, 500, OrdState::CXL}, {"SM1-c", Side::SELL, 500, OrdState::WORK}}) {
      Gateway::SysOrder o;
      o.cid = cid;
      o.lane = "M1";
      o.ticker = TA;
      o.side = side;
      o.price = 3000;   // off the touches: everyone else's book is the same
      o.count = o.left = left;
      o.state = state;
      o.terminal = state == OrdState::CXL;
      g.view->orders[cid] = o;
    }
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    e.step(T0 + 3200);
    CHECK(g.places().size() == 1 && g.places()[0]->ticker == TA && g.places()[0]->side == Side::BUY && g.places()[0]->count == 100);
    g.answer(0, OrderReply{});
    g.view->positions["C1|" + TA] = 1600;   // its fill: A is full, so A's team goes through B (5 short there)
    g.score(T0 + 9000, T0 + 9000, 0.70);
    e.step(T0 + 9100);
    CHECK(g.places().size() == 2 && g.places()[1]->ticker == TB && g.places()[1]->side == Side::SELL && g.places()[1]->count == 400);
  }
  {   // Systematic's working order at A's ask is not everyone else's: A's team through B's bid
    Rig g;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    Gateway::SysOrder o;
    o.cid = "SM1-x";
    o.lane = "M1";
    o.ticker = TA;
    o.side = Side::SELL;
    o.price = 4500;
    o.count = o.left = 10 * kOneContract;
    o.state = OrdState::WORK;
    g.view->orders[o.cid] = o;
    g.score(T0 - 60000, T0 - 1000, 0.50);
    e.step(T0);
    g.score(T0 + 3000, T0 + 3000, 0.70);
    e.step(T0 + 3200);
    CHECK(g.places().size() == 1 && g.places()[0]->ticker == TB && g.places()[0]->side == Side::SELL && g.places()[0]->price == 5200 &&
          g.places()[0]->fair == Px{3000});
  }
  {   // the in-game volume: with no pregame reading, all of it (21,000: not thin)
    Rig g;
    g.market(TA, EV, 15000 * kOneContract);
    g.market(TB, EV, 6000 * kOneContract);
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
    g.score(T0 - 60000, T0 - 1000, 0.70);
    e.step(T0);
    CHECK(game_status(e)["pull"] == "VOLUME" && has(game_status(e)["why"], "21000 contracts"));
    // read before the start, only what traded since: 2,000
    Rig h;
    h.market(TA, EV, 15000 * kOneContract);
    h.market(TB, EV, 6000 * kOneContract);
    TakeEngine f("C1", {"KXNCAAWBGAME"}, h.env(), take_params(TakeMode::COLLEGE));
    h.score(T0 - 60000, T0 - 1000, std::nullopt, 0, 0, "pre");
    f.step(T0);
    CHECK(game_status(f)["pull"] == "STATE");
    h.market(TA, EV, 16000 * kOneContract);
    h.market(TB, EV, 7000 * kOneContract);
    h.score(T0 + 3000, T0 + 3000, 0.70);
    f.step(T0 + 3200);
    CHECK(h.places().size() == 1);
    // a volume the listing does not give is never thin
    Rig u;
    u.market(TB, EV, std::nullopt);
    TakeEngine x("C1", {"KXNCAAWBGAME"}, u.env(), take_params(TakeMode::COLLEGE));
    u.score(T0 - 60000, T0 - 1000, 0.70);
    x.step(T0);
    CHECK(game_status(x)["pull"] == "VOLUME" && game_status(x)["why"] == "the listing gives no volume: not a thin game");
  }
  {   // the basketball clock: 1:30 of the 4th quarter is 90 s of regulation left
    Rig g;
    TakeParams p = take_params(TakeMode::COLLEGE);
    p.min_t_left_s = 120;
    TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), p);
    g.score(T0 - 60000, T0 - 1000, 0.50, 60, 48, "in", 4, 90);
    e.step(T0);
    CHECK(game_status(e)["pull"] == "LATE" && has(game_status(e)["why"], "90.0 s"));
    g.score(T0 - 60000, T0 - 500, 0.50, 60, 48, "in", 4, 121);
    e.step(T0 + 250);
    CHECK(game_status(e)["pull"] == "NO_EDGE");
  }
  {   // volleyball at level "set": a set's end, as the engine saw the sets total go up
    const std::string ev = "KXNCAAWVMATCH-26NOV14CCCDDD";
    Rig g(ev, ev + "-CCC", ev + "-DDD");
    TakeParams p = take_params(TakeMode::COLLEGE);
    p.points_required = false;
    TakeEngine e("C1", {"KXNCAAWVMATCH"}, g.env(), p);
    g.score(T0 - 60000, T0 - 1000, 0.50, 1, 0);   // first seen at 1-0: when that set ended is not known
    e.step(T0);
    CHECK(game_status(e, ev)["pull"] == "SET_WINDOW" && game_status(e, ev)["why"] == "no set has ended");
    g.score(T0 + 3000, T0 + 3000, 0.70, 1, 1);
    e.step(T0 + 3200);
    CHECK(g.places().size() == 1);
    g.answer(0, OrderReply{});
    g.score(T0 + 3000 + 120001, T0 + 3000 + 120001, 0.70, 1, 1);   // a point later, past the window
    e.step(T0 + 3000 + 120001);
    CHECK(game_status(e, ev)["pull"] == "SET_WINDOW" && has(game_status(e, ev)["why"], "120001 ms since a set ended"));
  }
  {   // X1 ESPN_FOLLOW: the scoreboard's number dated by the score state; the summary's by its play
    const std::string ev = "KXNBAGAME-26NOV14LALBOS";
    Rig g(ev, ev + "-BOS", ev + "-LAL");   // A: the first market by ticker
    g.view->lanes_on = {"X1"};
    g.book(g.a, 4000, 4200);
    g.book(g.b, 5800, 6000);
    TakeEngine e("X1", {"KXNBAGAME"}, g.env(), take_params(TakeMode::ESPN_FOLLOW));
    g.score(T0 - 60000, T0 - 1000, std::nullopt, 60, 48, "in", 3, 300);
    g.scores[g.a].sb_wp = 6000;
    e.step(T0);
    CHECK(game_status(e, ev)["pull"] == "SIGNAL_OLD");   // the state first seen 60 s ago
    g.score(T0 + 3000, T0 + 3000, std::nullopt, 62, 48, "in", 3, 280);
    g.scores[g.a].sb_wp = 6000;
    e.step(T0 + 3200);
    CHECK(g.places().size() == 1 && g.places()[0]->ticker == g.a && g.places()[0]->price == 4200 && g.places()[0]->count == 400 &&
          g.places()[0]->fair == Px{6000} && g.rows.back()["fair"] == "0.6000" && g.rows.back()["mode"] == "ESPN_FOLLOW");
    g.answer(0, OrderReply{});
    // a new state with the summary's number, whose play was first seen 30.001 s ago: it comes
    // before the scoreboard's (which the new state would date now)
    g.score(T0 + 20000, T0 + 20000, std::nullopt, 64, 48, "in", 3, 250);
    g.scores[g.a].espn = 6000;
    g.scores[g.a].sb_wp = 6000;
    g.scores[g.a].wp_seen_ms = T0 + 20200 - 30001;
    e.step(T0 + 20200);
    CHECK(game_status(e, ev)["pull"] == "SIGNAL_OLD" && has(game_status(e, ev)["why"], "ESPN's number was first seen 30001 ms ago"));
  }
}

// Which games: its series' events of two active markets dated today or yesterday (a late game);
// not one a day older or the next day's, another series', one of three markets, or a final one
void engine_games() {
  Rig g;
  for (const std::string ev : {"KXNCAAWBGAME-26NOV13CCCDDD", "KXNCAAWBGAME-26NOV12EEEFFF", "KXNCAAWBGAME-26NOV15GGGHHH",
                               "KXNBAGAME-26NOV14LALBOS"}) {
    g.market(ev + "-X", ev);
    g.market(ev + "-Y", ev);
  }
  const std::string three = "KXNCAAWBGAME-26NOV14IIIJJJ", fin = "KXNCAAWBGAME-26NOV14LLLMMM", shut = "KXNCAAWBGAME-26NOV14NNNOOO";
  for (const std::string s : {"-I", "-J", "-K"}) g.market(three + s, three);
  g.market(fin + "-L", fin);
  g.market(fin + "-M", fin);
  g.market(shut + "-N", shut);
  g.market(shut + "-O", shut);
  g.snap->markets[shut + "-N"].close_ms = T0;
  g.snap->markets[shut + "-O"].status = "closed";
  g.scores[fin + "-L"].state = "post";
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  CHECK(g.wanted == std::set<std::string>({TA, TB, "KXNCAAWBGAME-26NOV13CCCDDD-X", "KXNCAAWBGAME-26NOV13CCCDDD-Y"}));
  CHECK(game_status(e, three)["pull"] == "MARKETS" && game_status(e, fin)["pull"] == "FINAL");
  CHECK(e.status()["games"].size() == 4);   // the two games, the three-market event and the final one
}

// Each series its own rule (C1: the gate's level and leader_only, series by series): the same
// trailing team with the edge is pulled TRAILER in the series measured leader-only and taken in
// the other; the rules of one lane share its mode
void engine_series() {
  const std::string ev = "KXNCAAMBGAME-26NOV14CCCDDD";
  Rig g, h(ev, ev + "-CCC", ev + "-DDD");
  g.snap->markets.insert(h.snap->markets.begin(), h.snap->markets.end());
  g.books.insert(h.books.begin(), h.books.end());
  g.tradable.insert(h.tradable.begin(), h.tradable.end());
  TakeParams leader = take_params(TakeMode::COLLEGE);
  leader.leader_only = true;
  TakeEngine e("C1", std::map<std::string, TakeParams>{{"KXNCAAWBGAME", leader}, {"KXNCAAMBGAME", take_params(TakeMode::COLLEGE)}},
               g.env());
  auto score = [&](std::int64_t first_seen, double p) {   // both games: A's team trails 40-48
    g.score(first_seen, first_seen, p, 40, 48);
    h.score(first_seen, first_seen, p, 40, 48);
    for (const auto& tk : {h.a, h.b}) {
      g.scores[tk] = h.scores[tk];
      g.fairs[tk] = h.fairs[tk];
    }
  };
  score(T0 - 1000, 0.50);
  e.step(T0);
  CHECK(g.posted.empty() && g.wanted == std::set<std::string>({TA, TB, h.a, h.b}));
  score(T0 + 3000, 0.70);
  e.step(T0 + 3200);
  CHECK(g.places().size() == 1 && g.places()[0]->ticker == h.a && g.places()[0]->cid.rfind("SC1-", 0) == 0);
  CHECK(game_status(e)["pull"] == "TRAILER" && game_status(e, ev)["pull"] == "");
  CHECK(g.rows.size() == 2 && g.rows[0]["action"] == "TAKE" && g.rows[0]["tk"] == h.a && g.rows[1]["action"] == "PULL" &&
        g.rows[1]["tk"] == TA && has(g.rows[1]["why"], "TRAILER: "));
  bool refused = false;
  try {
    TakeEngine x("C1", std::map<std::string, TakeParams>{{"KXNCAAWBGAME", leader}, {"KXNBAGAME", take_params(TakeMode::MODEL)}}, g.env());
  } catch (const std::invalid_argument& ex) {
    refused = has(ex.what(), "KXNCAAWBGAME is COLLEGE, KXNBAGAME MODEL: a lane has one mode");
  }
  CHECK(refused);
}

// Signals' markouts: each horizon's mid half a second after it, + in the signal's favour, null
// with no mid; the row sent again each time. And the fills' markouts at the lane's horizon go to gw.
void engine_markouts() {
  Rig g;
  TakeEngine e("C1", {"KXNCAAWBGAME"}, g.env(), take_params(TakeMode::COLLEGE));
  g.score(T0 - 60000, T0 - 1000, 0.50);
  e.step(T0);
  g.score(T0 + 3000, T0 + 3000, 0.70);
  const std::int64_t ts = T0 + 3200;
  e.step(ts);
  g.answer(0, OrderReply{});
  g.mids[TA] = 4800;
  e.step(ts + 10499);
  CHECK(g.rows.size() == 1);
  e.step(ts + 10500);
  CHECK(g.rows.size() == 2 && g.rows.back()["mo10"] == "0.0300" && g.rows.back()["mo60"].is_null() && g.rows.back()["id"] == g.rows[0]["id"]);
  CHECK(!g.asked.empty() && g.asked.back() == std::make_pair(TA, ts + 10000));
  g.mids.erase(TA);   // no mid: null, never guessed
  e.step(ts + 60500);
  CHECK(g.rows.size() == 3 && g.rows.back()["mo60"].is_null() && g.rows.back()["mo10"] == "0.0300");
  g.mids[TA] = 4300;
  e.step(ts + 300500);
  CHECK(g.rows.size() == 4 && g.rows.back()["mo300"] == "-0.0200" && g.rows.back()["action"] == "TAKE");
  e.step(ts + 400000);
  CHECK(g.rows.size() == 4);
  // a SELL's markout is the other way: SELL B at 0.52, B's mid 0.50 later is +2c
  Rig h;
  h.book(TA, 4000, 4900);   // A's ask dearer than 1 - B's bid: through B's bid
  TakeEngine f("C1", {"KXNCAAWBGAME"}, h.env(), take_params(TakeMode::COLLEGE));
  h.score(T0 - 60000, T0 - 1000, 0.50);
  f.step(T0);
  h.score(T0 + 3000, T0 + 3000, 0.75);
  f.step(ts);
  h.mids[TB] = 5000;
  f.step(ts + 10500);
  CHECK(h.places().size() == 1 && h.places()[0]->side == Side::SELL && h.rows.back()["mo10"] == "0.0200");
  // LaneMarkout: its own fills at its horizon only
  Rig m;
  TakeOptions o;
  o.mo_h = "300s";
  TakeEngine c1("C1", {"KXNCAAWBGAME"}, m.env(), take_params(TakeMode::COLLEGE), o);
  MarkoutRow r;
  r.fid = "fill-1";
  r.lane = "C1";
  r.h = "300s";
  r.mo = 300;
  r.net = 120;
  r.qty = 400;
  c1.markout(r);
  r.h = "60s";
  c1.markout(r);
  r.h = "300s";
  r.lane = "M1";
  c1.markout(r);
  CHECK(m.posted.size() == 1 && c1.status()["markouts_to_gw"] == 1);
  if (m.posted.size() == 1) {
    const auto* lm = std::get_if<LaneMarkout>(&m.posted[0]);
    CHECK(lm && lm->lane == "C1" && lm->fid == "fill-1" && lm->h == "300s" && lm->mo_net == 120 && lm->qty == 400);
  }
  TakeEngine none("C1", {"KXNCAAWBGAME"}, m.env(), take_params(TakeMode::COLLEGE));   // no horizon: none go
  r.lane = "C1";
  none.markout(r);
  CHECK(m.posted.size() == 1);
}

}  // namespace

int main() {
  clip_counts();
  defaults();
  fixtures();
  mirrored();
  order_fields();
  whys();
  routes();
  unbounded();
  inventory();
  coverage();
  engine_takes();
  engine_runs();
  engine_scores();
  engine_merged();
  engine_refused();
  engine_blocked();
  engine_pulls();
  engine_inputs();
  engine_games();
  engine_series();
  engine_markouts();
  return finish("deskd-take");
}
