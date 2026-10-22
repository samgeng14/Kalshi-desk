// strat/take: the IOC game lanes' one decision. clip_count against hand-worked sizes; each lane's
// defaults; every case of the three hand-worked fixtures the Python gate tools share
// (college_take_cases.json for C1, x1_golden.json for X1, n1_golden.json for N1 and N2), which
// between them reach every pull; a game seen from the other market is the same take; the order's
// own fields; the reasons in words; no game cap and a huge clip stay bounded by the touch.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/gw/fees.h"
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
         x.fixed_count == y.fixed_count && x.game_cap == y.game_cap && x.max_takes == y.max_takes &&
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
  CHECK(same_params(take_params(TakeMode::ESPN_FOLLOW), [] { auto p = take_params(TakeMode::MODEL); p.mode = TakeMode::ESPN_FOLLOW; return p; }()));
  CHECK(f.fixed_count == 500 && f.fav_lo == 9000 && f.fav_hi == 9500 && f.fav_from_ms == 3600000 && f.fav_to_ms == 120000 &&
        f.max_spread == 1000 && f.edge == 0);
  // every code once, in the checks' order
  const auto& codes = take_pull_codes();
  CHECK(std::set<std::string>(codes.begin(), codes.end()).size() == codes.size() && codes.size() == 31);
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

// With no game cap and a clip far past the book, the touch is the bound
void unbounded() {
  TakeParams p = take_params(TakeMode::COLLEGE);
  p.game_cap = 0;
  p.clip = 900000000;   // $90,000
  Game g = college_game();
  g.book_a.asks = {{4500, 123456 * kOneContract}};
  const TakeDecision d = take_decide(g.in, p);
  CHECK(d.take && d.take->count == 123456 * kOneContract && d.take->fee == kalshi_fee_order_cent(4500, 123456 * kOneContract, 1));
  CHECK(clip_count(900000000, 1, 1, 0, false) == 1000000 * kOneContract);   // at most a million contracts
}

void coverage() {
  for (const auto& c : take_pull_codes()) {
    const bool hit = seen_pulls.count(c) > 0;
    if (!hit) std::fprintf(stderr, "no case pulls %s\n", c.c_str());
    CHECK(hit);
  }
  for (const auto& c : seen_pulls) CHECK(std::find(take_pull_codes().begin(), take_pull_codes().end(), c) != take_pull_codes().end());
}

}  // namespace

int main() {
  clip_counts();
  defaults();
  fixtures();
  mirrored();
  order_fields();
  whys();
  unbounded();
  coverage();
  return finish("deskd-take");
}
