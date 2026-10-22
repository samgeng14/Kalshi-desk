#include "deskd/strat/take.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

#include "deskd/gw/fees.h"
#include "deskd/strat/game_fair.h"

namespace deskd {

namespace {
constexpr int kPassed = 100;   // a route's stage once every check passed

// a / b rounded down (b > 0), as Python's //
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  if (a % b != 0 && a < 0) --q;
  return q;
}
std::string ms(std::int64_t v) { return std::to_string(v) + " ms"; }
std::string secs(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f s", v);
  return b;
}

// A route's edge a contract, num / den Money (den > 0)
struct Edge {
  std::int64_t num = 0, den = 1;
  Money floor() const { return floor_div(num, den); }
  bool at_least(Money thr) const { return static_cast<__int128>(num) >= static_cast<__int128>(thr) * den; }
};
bool edge_less(const Edge& x, const Edge& y) {
  return static_cast<__int128>(x.num) * y.den < static_cast<__int128>(y.num) * x.den;
}

// One way to take a team: BUY its market at the ask, or SELL the other market at its bid
struct Route {
  int dir = 0;
  bool buy = true;
  const TakeLeg* leg = nullptr;   // the market sent to
  int stage = 0;                  // the check it failed, by its place in the mode's list (kPassed: none)
  std::string code, why;
  Px price = 0, team_px = 0, ref = 0;
  Qty count = 0;
  Money fee = 0;
  Micros cost = 0;
  std::optional<Edge> edge;
  std::int64_t since = 0;
};

// The most whole steps of contracts at px whose cost and fee come to at most budget
// micro-dollars, at most cap (0: none). Cost and fee only grow with the count, so halving finds it.
// A million contracts at most keeps the fee's arithmetic in range.
Qty fit(Micros budget, Px px, double mult, Qty step, Qty cap) {
  if (budget <= 0 || px <= 0 || px >= kOneDollar || step <= 0) return 0;
  std::int64_t lo = 0, hi = budget / px / step;   // px x Qty is micro-dollars: the price alone allows hi
  hi = std::min<std::int64_t>(hi, 1000000 * kOneContract / step);
  if (cap > 0) hi = std::min<std::int64_t>(hi, cap / step);
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo + 1) / 2;
    const Qty c = mid * step;
    if (px * c + 100 * kalshi_fee_order_cent(px, c, mult) <= budget) lo = mid;
    else hi = mid - 1;
  }
  return lo * step;
}

bool in_mode(TakeMode m, std::initializer_list<TakeMode> ms) {
  return std::find(ms.begin(), ms.end(), m) != ms.end();
}
}  // namespace

const char* take_mode_str(TakeMode m) {
  switch (m) {
    case TakeMode::COLLEGE: return "COLLEGE";
    case TakeMode::MODEL: return "MODEL";
    case TakeMode::ESPN_FOLLOW: return "ESPN_FOLLOW";
    case TakeMode::ESPN_FADE: return "ESPN_FADE";
    case TakeMode::FAV_PREGAME: return "FAV_PREGAME";
  }
  return "?";
}

std::optional<TakeMode> parse_take_mode(const std::string& s) {
  for (TakeMode m : {TakeMode::COLLEGE, TakeMode::MODEL, TakeMode::ESPN_FOLLOW, TakeMode::ESPN_FADE, TakeMode::FAV_PREGAME})
    if (s == take_mode_str(m)) return m;
  return std::nullopt;
}

TakeParams take_params(TakeMode m) {
  TakeParams p;
  p.mode = m;
  switch (m) {
    case TakeMode::COLLEGE:   // college.json's TakeParams; the gate may raise edge, never below 10c
      p.edge = 2000;
      p.cool_ms = 5000;
      p.max_volume = 20000 * kOneContract;
      p.stale_only = true;
      break;
    case TakeMode::MODEL:     // nba.json's TakeParams (thr 1000 until the gate's)
    case TakeMode::ESPN_FOLLOW:
    case TakeMode::ESPN_FADE:
      p.edge = 1000;
      p.max_count = 4 * kOneContract;
      p.max_takes = 4;
      p.signal_ms = 30000;
      p.cool_ms = 10000;
      p.max_spread = 300;
      p.min_px = 500;
      p.max_px = 9500;
      p.trade_ot = false;
      p.min_t_left_s = 120;
      break;
    case TakeMode::FAV_PREGAME:   // the sports hunt's forward test: 5 contracts, hold to settlement
      p.edge = 0;
      p.clip = 0;
      p.fixed_count = 5 * kOneContract;
      p.max_score_age_ms = 60000;
      p.cool_ms = 10000;
      p.max_spread = 1000;
      break;
  }
  return p;
}

const std::vector<std::string>& take_pull_codes() {
  static const std::vector<std::string> codes = {
      "NOT_TRADABLE", "PRIVATE_DOWN", "STALE_BOOK", "NO_FEE", "NO_SCORE", "SCORE_OLD", "STATE", "WINDOW",
      "NO_P0", "NO_FAIR", "NO_ESPN", "NO_MID", "NO_POINTS", "SET_WINDOW", "OVERTIME", "LATE", "SIGNAL_OLD",
      "VOLUME", "SPREAD", "COOL_DOWN", "TAKES", "GAME_CAP", "NO_TOUCH", "HELD", "CLIP", "SIZE", "NO_EDGE",
      "BAND", "TOUCH_AFTER_SCORE", "TRAILER", "FIRED"};
  return codes;
}

Qty clip_count(Money clip, Px px, double fee_mult, Qty max_count, bool whole_only) {
  return fit(clip * 100, px, fee_mult, whole_only ? kOneContract : 1, max_count);
}

TakeDecision take_decide(const TakeInput& in, const TakeParams& p) {
  TakeDecision d;
  auto pull = [&](const char* code, std::string why) {
    d.pull = code;
    d.why = std::move(why);
    return d;
  };
  const TakeMode mode = p.mode;
  const bool fav = mode == TakeMode::FAV_PREGAME, fade = mode == TakeMode::ESPN_FADE;
  const bool espn = in_mode(mode, {TakeMode::ESPN_FOLLOW, TakeMode::ESPN_FADE});
  const bool model = in_mode(mode, {TakeMode::COLLEGE, TakeMode::MODEL});
  const std::int64_t now = in.now_ms;
  const std::array<const TakeLeg*, 2> legs{&in.a, &in.b};

  // ---- the game
  if (!in.not_tradable.empty()) return pull("NOT_TRADABLE", "not tradable: " + in.not_tradable);
  if (!in.private_up) return pull("PRIVATE_DOWN", "the private feed is down");
  for (const TakeLeg* l : legs) {
    if (!l->book) return pull("STALE_BOOK", l->ticker + " has no book");
    if (l->book->stale) return pull("STALE_BOOK", l->ticker + "'s book is stale (" + l->book->reason + ")");
    if (!l->book->bids.empty() && !l->book->asks.empty() && l->book->bids.front().px >= l->book->asks.front().px)
      return pull("STALE_BOOK", l->ticker + "'s book is crossed");
  }
  for (const TakeLeg* l : legs)   // fee_ready() refuses the same multipliers
    if (!l->fee_mult || !std::isfinite(*l->fee_mult) || *l->fee_mult < 0)
      return pull("NO_FEE", l->ticker + "'s fee multiplier is not known yet");
  if (!in.score_why.empty()) return pull("NO_SCORE", "no score: " + in.score_why);
  if (in.state.empty()) return pull("NO_SCORE", "no score");
  if (in.fetched_ms <= 0 || now - in.fetched_ms > p.max_score_age_ms)
    return pull("SCORE_OLD", "the score is " + (in.fetched_ms > 0 ? ms(now - in.fetched_ms) : std::string("never")) +
                                 " old (at most " + ms(p.max_score_age_ms) + ")");
  if (fav ? in.state != "pre" : in.state != "in")
    return pull("STATE", "the game is " + in.state + (fav ? ": only before the start" : ": not in play"));
  if (fav) {
    if (in.start_ms <= 0) return pull("WINDOW", "no scheduled start");
    if (now < in.start_ms - p.fav_from_ms || now > in.start_ms - p.fav_to_ms)
      return pull("WINDOW", ms(in.start_ms - now) + " before the start: outside " + ms(p.fav_from_ms) + " to " + ms(p.fav_to_ms));
  }
  if (model && !in.p0) return pull("NO_P0", "no pregame price");
  if (model && !in.fair) return pull("NO_FAIR", "no fair" + (in.fair_why.empty() ? std::string() : ": " + in.fair_why));
  if (espn && !in.espn) return pull("NO_ESPN", "no ESPN win probability");
  std::optional<EffBook> eff;
  if (fade) {
    eff = effective_book(in.a.book, in.b.book);
    if (!eff) return pull("NO_MID", "no two-sided effective book for a mid");
  }
  if (p.points_required && !in.points) return pull("NO_POINTS", "the set's points were not read");
  if (!p.points_required && (in.set_end_ms <= 0 || now - in.set_end_ms > p.set_window_ms))
    return pull("SET_WINDOW", in.set_end_ms <= 0 ? std::string("no set has ended")
                                                 : ms(now - in.set_end_ms) + " since a set ended (at most " + ms(p.set_window_ms) + ")");
  if (in.ot && !p.trade_ot) return pull("OVERTIME", "in overtime");
  if (p.min_t_left_s > 0 && (!in.t_left_s || !(*in.t_left_s >= p.min_t_left_s)))
    return pull("LATE", in.t_left_s ? secs(*in.t_left_s) + " of regulation left (at least " + secs(p.min_t_left_s) + ")"
                                    : std::string("no game clock"));
  if (p.signal_ms > 0) {
    const std::int64_t seen = espn ? in.espn_seen_ms : in.first_seen_ms;
    if (seen <= 0 || now - seen > p.signal_ms)
      return pull("SIGNAL_OLD", std::string(espn ? "ESPN's number" : "the score") + " was first seen " +
                                    (seen > 0 ? ms(now - seen) + " ago" : std::string("never")) + " (at most " + ms(p.signal_ms) + ")");
  }
  if (p.max_volume > 0 && in.volume >= p.max_volume)
    return pull("VOLUME", "in-game volume " + std::to_string(in.volume / kOneContract) + " contracts: not a thin game");
  if (p.max_spread > 0)
    for (const TakeLeg* l : legs) {
      if (l->book->bids.empty() || l->book->asks.empty()) return pull("SPREAD", l->ticker + "'s book is one-sided");
      const Px s = l->book->asks.front().px - l->book->bids.front().px;
      if (s > p.max_spread) return pull("SPREAD", l->ticker + "'s spread " + price_str(s) + " is over " + price_str(p.max_spread));
    }
  if (in.last_take_ms > 0 && now - in.last_take_ms < p.cool_ms)
    return pull("COOL_DOWN", ms(now - in.last_take_ms) + " since the last take in the game (at least " + ms(p.cool_ms) + ")");
  if (p.max_takes > 0 && in.game_takes >= p.max_takes) return pull("TAKES", std::to_string(in.game_takes) + " takes in the game");
  if (p.game_cap > 0 && in.game_used >= p.game_cap)
    return pull("GAME_CAP", "the game's worst case $" + money_str(in.game_used / 100) + " is at its cap");

  // ---- the routes
  const Micros room = p.game_cap - in.game_used;   // with game_cap > 0
  const int score[2] = {in.a_score, in.b_score};
  auto price = [&](int dir, bool buy) {
    Route r;
    r.dir = dir;
    r.buy = buy;
    const TakeLeg& own = *legs[dir];
    const TakeLeg& other = *legs[1 - dir];
    r.leg = buy ? &own : &other;
    const std::string& team = own.ticker;
    int stage = 0;
    auto fail = [&](const char* code, std::string why) {
      r.stage = stage;
      r.code = code;
      r.why = std::move(why);
      return r;
    };
    const auto& side = buy ? own.book->asks : other.book->bids;
    if (side.empty()) return fail("NO_TOUCH", r.leg->ticker + (buy ? " has no ask" : " has no bid"));
    ++stage;
    r.price = side.front().px;
    r.team_px = buy ? r.price : kOneDollar - r.price;
    r.since = buy ? own.ask_since_ms : other.bid_since_ms;
    const std::string px_s = "the team's price " + price_str(r.team_px);
    if (fav) {
      if (r.team_px < p.fav_lo || r.team_px >= p.fav_hi)
        return fail("BAND", px_s + " is outside [" + price_str(p.fav_lo) + ", " + price_str(p.fav_hi) + ")");
      ++stage;
    }
    // the count
    const double mult = *r.leg->fee_mult;
    const Qty step = r.leg->fractional ? 1 : kOneContract;
    const Qty held = std::max<Qty>(0, own.position) + std::max<Qty>(0, -other.position);
    const Qty want = fav ? (p.fixed_count - held) / step * step : fit(p.clip * 100, r.team_px, mult, step, p.max_count);
    const Qty by_room = p.game_cap > 0 ? fit(room, r.team_px, mult, step, 0) : std::numeric_limits<Qty>::max();
    const Qty touch = side.front().qty / step * step;
    r.count = std::min({want, by_room, touch});
    if (r.count <= 0) {
      if (fav && want <= 0) return fail("HELD", "holds " + std::to_string(held / kOneContract) + " of " + team);
      if (by_room <= 0) return fail("GAME_CAP", "the game's room $" + money_str(room / 100) + " buys no contract at " + price_str(r.team_px));
      if (want <= 0) return fail("CLIP", "the clip $" + money_str(p.clip) + " buys no contract at " + price_str(r.team_px));
      return fail("SIZE", "the touch is under one contract");
    }
    ++stage;
    r.fee = kalshi_fee_order_cent(r.team_px, r.count, mult);
    r.cost = r.team_px * r.count + 100 * r.fee;
    // the edge
    if (fade) {
      const std::int64_t gap_a = eff->bid + eff->ask - 2 * *in.espn;   // twice A's mid - ESPN's
      r.ref = dir == 0 ? *in.espn : kOneDollar - *in.espn;
      r.edge = Edge{dir == 0 ? gap_a : -gap_a, 2};
    } else if (!fav) {
      const Px ref_a = model ? *in.fair : *in.espn;
      r.ref = dir == 0 ? ref_a : kOneDollar - ref_a;
      r.edge = Edge{(r.ref - r.team_px) * r.count - 100 * r.fee, r.count};
    } else {
      r.edge = Edge{};
    }
    if (!fav) {
      if (!r.edge->at_least(p.edge))
        return fail("NO_EDGE", "edge " + money_str(r.edge->floor()) + " is under " + money_str(p.edge));
      ++stage;
      if (r.team_px < p.min_px || r.team_px > p.max_px)
        return fail("BAND", px_s + " is outside " + price_str(p.min_px) + "-" + price_str(p.max_px));
      ++stage;
    }
    if (p.stale_only && !(r.since > 0 && r.since < in.first_seen_ms))
      return fail("TOUCH_AFTER_SCORE", r.since > 0 ? "the touch is " + ms(r.since - in.first_seen_ms) + " newer than the score"
                                                   : std::string("the touch's age is not known"));
    ++stage;
    if (p.leader_only && score[dir] <= score[1 - dir])
      return fail("TRAILER", team + "'s team does not lead (" + std::to_string(score[dir]) + "-" + std::to_string(score[1 - dir]) + ")");
    ++stage;
    if (in.fired[dir]) return fail("FIRED", "this run already took " + team);
    r.stage = kPassed;
    return r;
  };
  std::vector<Route> routes;
  for (int dir : {0, 1})
    for (bool buy : {true, false}) routes.push_back(price(dir, buy));

  // the best route that passed: the largest edge, then the lowest cost a contract, then the first
  const Route* best = nullptr;
  for (const auto& r : routes) {
    if (r.stage != kPassed) continue;
    if (!best || edge_less(*best->edge, *r.edge) ||
        (!edge_less(*r.edge, *best->edge) &&
         static_cast<__int128>(r.cost) * best->count < static_cast<__int128>(best->cost) * r.count))
      best = &r;
  }
  if (best) {
    TakeOrder o;
    o.ticker = best->leg->ticker;
    o.side = best->buy ? Side::BUY : Side::SELL;
    o.price = best->price;
    o.count = best->count;
    o.fee = best->fee;
    o.edge = best->edge->floor();
    o.team_px = best->team_px;
    o.ref = best->ref;
    o.cost = best->cost;
    o.dir = best->dir;
    o.touch_since_ms = best->since;
    d.edge = o.edge;
    d.dir = o.dir;
    d.take = std::move(o);
    return d;
  }
  // none: the reason of the route that got furthest (then the larger edge, then the first)
  const Route* far = &routes.front();
  for (const auto& r : routes)
    if (r.stage > far->stage ||
        (r.stage == far->stage && r.edge && (!far->edge || edge_less(*far->edge, *r.edge))))
      far = &r;
  d.edge = far->edge ? std::optional<Money>(far->edge->floor()) : std::nullopt;
  d.dir = far->dir;
  return pull(far->code.c_str(), far->why);
}

}  // namespace deskd
