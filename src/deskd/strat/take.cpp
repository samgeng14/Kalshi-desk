#include "deskd/strat/take.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <stdexcept>

#include "deskd/app/log.h"
#include "deskd/gw/fees.h"
#include "deskd/rest/state.h"
#include "deskd/strat/bball_model.h"
#include "deskd/strat/game_fair.h"
#include "deskd/web/contract.h"

namespace deskd {
using json = nlohmann::json;

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
std::string qty_text(Qty q) { return json(qty_to_number(q)).dump(); }   // 20.0, 2.5: as gw's

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
      p.inventory = 20 * kOneContract;
      p.cool_ms = 5000;
      p.max_volume = 20000 * kOneContract;
      p.stale_only = true;
      break;
    case TakeMode::MODEL:     // nba.json's TakeParams (thr 1000 until the gate's)
    case TakeMode::ESPN_FOLLOW:
    case TakeMode::ESPN_FADE:
      p.edge = 1000;
      p.max_count = 4 * kOneContract;
      p.inventory = (m == TakeMode::MODEL ? 10 : 4) * kOneContract;   // N1's; X1's
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
      p.inventory = 5 * kOneContract;
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
      "VOLUME", "SPREAD", "COOL_DOWN", "TAKES", "GAME_CAP", "NO_TOUCH", "HELD", "INVENTORY", "CLIP", "SIZE",
      "NO_EDGE", "BAND", "TOUCH_AFTER_SCORE", "TRAILER", "FIRED"};
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
    // gw's inventory (gateway.cpp place_sys): Systematic's position in the market sent to, pos in
    // the order's direction, with its working orders on the side, ends at most inventory from flat
    // or no further from it than pos
    const Qty pos = (buy ? 1 : -1) * r.leg->sys_position, working = buy ? r.leg->sys_buying : r.leg->sys_selling;
    const Qty by_inv = p.inventory > 0 ? std::max<Qty>(0, std::max(p.inventory, std::abs(pos)) - pos - working) / step * step
                                       : std::numeric_limits<Qty>::max();
    const Qty touch = side.front().qty / step * step;
    r.count = std::min({want, by_room, by_inv, touch});
    if (r.count <= 0) {
      if (fav && want <= 0) return fail("HELD", "holds " + std::to_string(held / kOneContract) + " of " + team);
      if (by_room <= 0) return fail("GAME_CAP", "the game's room $" + money_str(room / 100) + " buys no contract at " + price_str(r.team_px));
      if (by_inv <= 0)
        return fail("INVENTORY", "the inventory room in " + r.leg->ticker + " buys no contract: " + qty_text(r.leg->sys_position) +
                                     " held, " + qty_text(working) + " working, limit " + qty_text(p.inventory));
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
    // the score's change, between the poll before its first sighting and that sighting
    const std::int64_t gap = in.first_seen_ms - in.prev_poll_ms;
    if (p.stale_only && (in.prev_poll_ms <= 0 || gap > p.max_poll_gap_ms))
      return fail("TOUCH_AFTER_SCORE", "the score's change is not dated: " +
                                           (in.prev_poll_ms > 0 ? ms(gap) + " from the poll before it (at most " + ms(p.max_poll_gap_ms) + ")"
                                                                : std::string("no poll before its first sighting")));
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

  auto order = [](const Route& r) {
    TakeOrder o;
    o.ticker = r.leg->ticker;
    o.side = r.buy ? Side::BUY : Side::SELL;
    o.price = r.price;
    o.count = r.count;
    o.fee = r.fee;
    o.edge = r.edge ? r.edge->floor() : 0;
    o.team_px = r.team_px;
    o.ref = r.ref;
    o.cost = r.cost;
    o.dir = r.dir;
    o.touch_since_ms = r.since;
    return o;
  };
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
    d.take = order(*best);
    d.edge = d.take->edge;
    d.dir = d.take->dir;
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
  if (far->count > 0) d.route = order(*far);
  return pull(far->code.c_str(), far->why);
}

// ---- TakeEngine

namespace {
std::string series_of(const std::string& tk) { return tk.substr(0, tk.find('-')); }
Px to_px(double p) { return static_cast<Px>(std::llround(std::min(1.0, std::max(0.0, p)) * kOneDollar)); }

// The decision found a team worth taking: it takes, or a route got past its edge (FAV_PREGAME:
// into its band) before a later check pulled it
bool has_edge(const TakeDecision& d, TakeMode m) {
  static const std::vector<std::string> past_edge = {"BAND", "TOUCH_AFTER_SCORE", "TRAILER", "FIRED"};
  static const std::vector<std::string> past_band = {"HELD", "GAME_CAP", "INVENTORY", "CLIP", "SIZE", "TOUCH_AFTER_SCORE",
                                                      "TRAILER", "FIRED"};
  if (d.take) return true;
  if (d.dir < 0) return false;
  const auto& past = m == TakeMode::FAV_PREGAME ? past_band : past_edge;
  return std::find(past.begin(), past.end(), d.pull) != past.end();
}

// Everyone else's book: Systematic's working orders on the market come off their levels (as the
// makers' without_own), and a level they made alone goes
void without_own(MdBook& b, const Gateway::SysView& v, const std::string& tk) {
  auto take = [](std::vector<MdLevel>& side, Px px, Qty q) {
    for (auto it = side.begin(); it != side.end(); ++it) {
      if (it->px != px) continue;
      it->qty -= q;
      if (it->qty <= 0) side.erase(it);
      return;
    }
  };
  for (const auto& [cid, o] : v.orders) {
    if (o.ticker != tk || o.terminal || o.left <= 0 || (o.state != OrdState::WORK && o.state != OrdState::PART)) continue;
    take(o.side == Side::BUY ? b.bids : b.asks, o.price, o.left);
  }
}
}  // namespace

TakeEngine::TakeEngine(std::string lane, std::vector<std::string> series, Env env, TakeParams params, TakeOptions options)
    : lane_(std::move(lane)), series_(std::move(series)), env_(std::move(env)), p_(params), o_(std::move(options)) {
  for (const auto& s : series_) params_[s] = p_;
}

TakeEngine::TakeEngine(std::string lane, std::map<std::string, TakeParams> series, Env env, TakeOptions options)
    : lane_(std::move(lane)), env_(std::move(env)), params_(std::move(series)), o_(std::move(options)) {
  if (!params_.empty()) p_ = params_.begin()->second;
  for (const auto& [s, p] : params_) {
    if (p.mode != p_.mode)
      throw std::invalid_argument("TakeEngine " + lane_ + ": " + s + " is " + take_mode_str(p.mode) + ", " +
                                  params_.begin()->first + " " + take_mode_str(p_.mode) + ": a lane has one mode");
    series_.push_back(s);
  }
}

std::string TakeEngine::new_cid(std::int64_t now) {
  // S<lane>-<ms in base 36>-<seq>, as the makers': at most 40 characters, [A-Za-z0-9-]
  std::string t;
  for (auto v = static_cast<std::uint64_t>(now); v; v /= 36) t.insert(t.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]);
  return "S" + lane_ + "-" + t + "-" + std::to_string(++seq_);
}

void TakeEngine::step(std::int64_t now) {
  const auto view = env_.view ? env_.view() : nullptr;
  const auto snap = env_.snapshot ? env_.snapshot() : nullptr;
  if (!view || !snap) return;
  answers(now);   // first: a refusal backs its game off this step
  marks(now);
  const auto fairs = env_.fairs ? env_.fairs() : nullptr;
  const auto scores = env_.scores ? env_.scores() : nullptr;
  const auto tradable = env_.tradable ? env_.tradable() : std::map<std::string, std::string>{};
  const bool on = view->lanes_on.count(lane_) > 0;
  // the markout hold (nba.json's, as the makers' mo_after): new takes wait while the lane's marked
  // fills average at or under -mo_floor a contract
  std::string hold;
  if (o_.mo_after > 0 && env_.markout) {
    const auto [n, mean] = env_.markout();
    if (n >= o_.mo_after && mean <= -o_.mo_floor)
      hold = "markout hold: " + std::to_string(n) + " fills marked, mean " + money_str(mean) + " a contract";
  }
  if (!hold.empty() && !holding_ && env_.alert) env_.alert("WARN", lane_ + " " + hold);
  holding_ = !hold.empty();
  {
    std::lock_guard<std::mutex> lk(status_mu_);
    hold_ = hold;
  }
  // the games: the series' events dated today or yesterday (ET), each two markets
  const std::string today = et_date(now), yesterday = et_date(et_midnight_ms(now) - 1);
  std::map<std::string, std::vector<const kalshi::MarketRow*>> events;
  for (const auto& [tk, m] : snap->markets) {
    const bool mine = std::any_of(series_.begin(), series_.end(), [&tk = tk](const std::string& s) {
      return tk.size() > s.size() && tk.compare(0, s.size(), s) == 0 && tk[s.size()] == '-';
    });
    if (!mine || (m.status != "active" && m.status != "open") || !m.close_ms || *m.close_ms <= now) continue;
    const auto date = kalshi_event_date(m.event_ticker);
    if (date == today || date == yesterday) events[m.event_ticker].push_back(&m);
  }
  std::set<std::string> want, looked;
  for (const auto& [ev, ms] : events) {
    games_[ev].seen_ms = now;
    if (ms.size() != 2) {
      set_last(ev, {{"pull", "MARKETS"}, {"why", std::to_string(ms.size()) + " markets in the event: a game has two"}});
      continue;
    }
    const std::string series = series_of(ms[0]->ticker);
    const auto t = tradable.find(series);
    const std::string not_tradable = t == tradable.end() ? "no gate for " + series : t->second;
    if (scores) {
      const auto r = scores->find(ms[0]->ticker);
      if (r != scores->end() && r->second.state == "post") {
        set_last(ev, {{"pull", "FINAL"}, {"why", "the game is final"}});
        continue;
      }
    }
    if (!on && !o_.shadow) {
      set_last(ev, {{"pull", "OFF"}, {"why", "lane " + lane_ + " is off"}});
      continue;
    }
    if (not_tradable.empty() || o_.shadow) want.insert({ms[0]->ticker, ms[1]->ticker});
    looked.insert({ms[0]->ticker, ms[1]->ticker});
    game(ev, *ms[0], *ms[1], *view, fairs.get(), scores.get(), not_tradable, hold, now);
  }
  if (env_.want) env_.want(want);
  // forget a touch not looked at (a later look dates it again) and a game gone for 6 h
  for (auto it = touch_.begin(); it != touch_.end();) it = looked.count(it->first) ? std::next(it) : touch_.erase(it);
  std::lock_guard<std::mutex> lk(status_mu_);
  for (auto it = last_.begin(); it != last_.end();) it = events.count(it->first) ? std::next(it) : last_.erase(it);
  for (auto it = games_.begin(); it != games_.end();)
    it = now - it->second.seen_ms > 6 * 3600000LL && !it->second.answer.valid() ? games_.erase(it) : std::next(it);
}

void TakeEngine::game(const std::string& ev, const kalshi::MarketRow& ma, const kalshi::MarketRow& mb, const Gateway::SysView& v,
                      const GameBoard* fairs, const EspnBoard* scores, const std::string& not_tradable, const std::string& hold,
                      std::int64_t now) {
  Game& g = games_[ev];
  const kalshi::MarketRow* const m[2] = {&ma, &mb};
  // everyone else's books, and when this engine first saw each touch price
  std::optional<MdBook> book[2];
  for (int i = 0; i < 2; ++i) {
    const std::string& tk = m[i]->ticker;
    book[i] = env_.book ? env_.book(tk) : std::nullopt;
    if (book[i]) without_own(*book[i], v, tk);
    if (!book[i] || book[i]->stale) {
      touch_.erase(tk);
      continue;
    }
    const Px bid = book[i]->bids.empty() ? -1 : book[i]->bids.front().px, ask = book[i]->asks.empty() ? -1 : book[i]->asks.front().px;
    auto [it, fresh] = touch_.try_emplace(tk);
    Touch& t = it->second;
    if (fresh || bid != t.bid) t = Touch{bid, t.ask, now, t.ask_ms};
    if (fresh || ask != t.ask) t = Touch{t.bid, ask, t.bid_ms, now};
  }
  const EspnRef* r = nullptr;
  if (scores) {
    const auto it = scores->find(ma.ticker);
    if (it != scores->end()) r = &it->second;
  }
  const GameFairRow* f = nullptr;
  if (fairs) {
    const auto it = fairs->find(ma.ticker);
    if (it != fairs->end()) f = &it->second;
  }
  TakeInput in;
  in.now_ms = now;
  for (int i = 0; i < 2; ++i) {
    TakeLeg& l = i ? in.b : in.a;
    l.ticker = m[i]->ticker;
    l.book = book[i] ? &*book[i] : nullptr;
    l.fee_mult = m[i]->fee_multiplier;
    l.fractional = m[i]->fractional.value_or(false);
    const auto pos = v.positions.find(lane_ + "|" + l.ticker);
    l.position = pos == v.positions.end() ? 0 : pos->second;
    // as gw's inventory counts: every lane's position, and Systematic's orders not ended by side
    for (const auto& [key, q] : v.positions)
      if (key.substr(key.find('|') + 1) == l.ticker) l.sys_position += q;
    for (const auto& [cid, o] : v.orders)
      if (o.ticker == l.ticker && !o.terminal) (o.side == Side::BUY ? l.sys_buying : l.sys_selling) += o.left;
    const auto t = touch_.find(l.ticker);
    if (t != touch_.end()) {
      l.bid_since_ms = t->second.bid_ms;
      l.ask_since_ms = t->second.ask_ms;
    }
    const auto w = v.worst.find(lane_ + "|" + l.ticker);
    if (w != v.worst.end()) in.game_used += w->second;
  }
  std::string score;
  if (r) {
    in.state = r->state;
    in.src = r->src;
    in.score_why = r->why;
    in.fetched_ms = r->fetched_ms;
    in.first_seen_ms = r->first_seen_ms;
    in.prev_poll_ms = r->prev_poll_ms;
    in.start_ms = r->start_ms;
    in.a_score = r->yes_score;
    in.b_score = r->opp_score;
    if (const auto* lg = bball_league_for_ticker(ma.ticker))
      if (const auto c = bball_t_left(*lg, r->period, r->clock_s)) {
        in.t_left_s = c->t_left_s;
        in.ot = c->ot;
      }
    if (r->espn) {
      in.espn = r->espn;
      in.espn_seen_ms = r->wp_seen_ms;
    } else if (r->sb_wp) {   // the scoreboard's number, dated by the score state (no later than its play)
      in.espn = r->sb_wp;
      in.espn_seen_ms = r->first_seen_ms;
    }
    score = std::to_string(r->yes_score) + "-" + std::to_string(r->opp_score) + (r->per.empty() ? "" : " " + r->per) +
            (r->clock.empty() ? "" : " " + r->clock);
  }
  if (!f) {
    in.fair_why = "no fair for " + series_of(ma.ticker);
  } else {
    if (f->p0) in.p0 = to_px(*f->p0);
    if (f->p && r && f->state == r->state && f->src == r->src && f->first_seen_ms == r->first_seen_ms) {
      in.fair = to_px(*f->p);
      in.points = f->points;
    } else {
      in.fair_why = f->p ? "the fair is a step behind the score: the next step prices it" : f->why;
    }
  }
  // volleyball: when the sets total went up
  const auto* lg = espn_league_for(ma.ticker);
  if (r && !r->state.empty() && lg && lg->sport == "volleyball") {
    const int sets = r->yes_score + r->opp_score;
    if (g.sets >= 0 && sets > g.sets) g.set_end_ms = r->first_seen_ms > 0 ? std::min(now, r->first_seen_ms) : now;
    g.sets = sets;
  }
  in.set_end_ms = g.set_end_ms;
  // the in-game volume: since each market's last pregame reading, else all of it
  bool volume_known = true;
  for (const auto* x : m) {
    if (!x->volume) {
      volume_known = false;
      continue;
    }
    if (r && r->state == "pre") g.pre_volume[x->ticker] = *x->volume;
    const auto pre = g.pre_volume.find(x->ticker);
    in.volume += *x->volume - (pre == g.pre_volume.end() ? 0 : std::min(pre->second, *x->volume));
  }
  const auto rule = params_.find(series_of(ma.ticker));
  const TakeParams& p = rule == params_.end() ? p_ : rule->second;   // the game's series' rule
  if (!volume_known && p.max_volume > 0) in.volume = p.max_volume;   // unknown is never a thin game
  in.last_take_ms = g.last_take_ms;
  in.game_takes = g.takes;
  in.private_up = env_.private_up && env_.private_up();
  in.not_tradable = not_tradable;
  // a new score state starts a new signal run; no score (none, or the sources at odds) is none
  if (!in.state.empty() && in.first_seen_ms != g.run_state) {
    g.run_state = in.first_seen_ms;
    g.fired[0] = g.fired[1] = g.noted[0] = g.noted[1] = false;
  }
  in.fired[0] = g.fired[0];
  in.fired[1] = g.fired[1];
  json st = {{"a", ma.ticker}, {"b", mb.ticker}, {"state", in.state}, {"src", in.src},
             {"age_ms", in.fetched_ms > 0 ? json(now - in.fetched_ms) : json(nullptr)},
             {"fair", in.fair ? json(price_str(*in.fair)) : json(nullptr)}, {"takes", g.takes}};
  if (g.answer.valid()) {
    st["pull"] = "WAIT";
    st["why"] = "waiting for gw's answer to its take";
    return set_last(ev, std::move(st));
  }
  if (now < g.backoff_until) {
    st["pull"] = "BACKOFF";
    st["why"] = "gw refused its take: " + ms(g.backoff_until - now) + " more";
    return set_last(ev, std::move(st));
  }
  auto decide = [&](const TakeInput& x) {
    TakeDecision d = take_decide(x, p);
    if (d.pull == "VOLUME" && !volume_known) d.why = "the listing gives no volume: not a thin game";
    return d;
  };
  const TakeDecision d = decide(in);
  // why the lane may not post what the rule takes; shadowing, fd is the rule's decision as if it
  // might (the gate and the private feed are the lane's, not the rule's)
  std::string blocked;
  if (!v.lanes_on.count(lane_)) blocked = "lane " + lane_ + " is off";
  else if (v.run != RunState::RUN) blocked = "Systematic is not running";
  else if (v.held.count(ma.ticker) || v.held.count(mb.ticker))
    blocked = (v.held.count(ma.ticker) ? ma.ticker : mb.ticker) + " is held by Manual";
  else if (!hold.empty()) blocked = hold;
  TakeDecision fd = d;
  if (d.pull == "NOT_TRADABLE" || d.pull == "PRIVATE_DOWN") {
    if (blocked.empty()) blocked = d.why;
    if (o_.shadow) {
      TakeInput x = in;
      x.not_tradable.clear();
      x.private_up = true;
      fd = decide(x);
    }
  }
  // the run: its edge seen now, or a decision without one past the quiet time ends it
  if (has_edge(fd, p_.mode)) {
    g.edge_ms = now;
  } else if (fd.dir >= 0 && now - g.edge_ms >= o_.run_quiet_ms) {
    g.fired[0] = g.fired[1] = g.noted[0] = g.noted[1] = false;
  }
  st["edge"] = fd.edge ? json(money_str(*fd.edge)) : json(nullptr);
  st["dir"] = fd.dir == 0 ? json("A") : fd.dir == 1 ? json("B") : json(nullptr);
  if (fd.take) {
    const TakeOrder& t = *fd.take;
    auto consume = [&] {   // the run's take of the team, and the game's
      g.fired[t.dir] = true;
      g.last_take_ms = now;
      ++g.takes;
    };
    if (blocked.empty()) {
      PlaceIntent pi;
      pi.cid = new_cid(now);
      pi.owner = Owner::SYS;
      pi.lane = lane_;
      pi.ticker = t.ticker;
      pi.side = t.side;
      pi.price = t.price;
      pi.count = t.count;
      pi.tif = Tif::IOC;
      pi.ttl_s = 0;
      pi.reduce_only = false;
      pi.override_guard = false;   // an IOC at the touch is inside the 2-tick guard
      pi.t_recv_ms = now;
      pi.touch = t.price;
      if (t.ref > 0 && p_.mode != TakeMode::ESPN_FADE && p_.mode != TakeMode::FAV_PREGAME)
        pi.fair = t.side == Side::BUY ? t.ref : kOneDollar - t.ref;   // the market's own YES fair
      auto reply = std::make_shared<std::promise<OrderReply>>();
      auto answer = reply->get_future();
      pi.reply = std::move(reply);
      const std::string cid = pi.cid;
      const bool sent = env_.post && env_.post(std::move(pi));
      const std::string id = signal(ev, t, in, score, sent ? "TAKE" : "REFUSED", false, sent ? "" : "gw's inbox is full", cid);
      consume();
      if (sent) {
        g.answer = std::move(answer);
        g.answer_id = id;
        g.posted_ms = now;
        signals_[id].open = true;
        ++takes_;
        DLOG_INFO("%s %s: take %s %s %s x%s, edge %s", lane_.c_str(), ev.c_str(), t.side == Side::BUY ? "BUY" : "SELL",
                  t.ticker.c_str(), price_str(t.price).c_str(), json(qty_to_number(t.count)).dump().c_str(), money_str(t.edge).c_str());
        st["pull"] = "";
        st["why"] = "took " + std::string(t.side == Side::BUY ? "BUY " : "SELL ") + t.ticker + " at " + price_str(t.price);
      } else {
        ++refusals_;
        g.backoff_until = now + o_.backoff_ms;
        st["pull"] = "BACKOFF";
        st["why"] = "gw's inbox is full";
      }
    } else {
      if (o_.shadow) {
        signal(ev, t, in, score, "SHADOW", true, blocked, "");
        consume();
        ++shadows_;
      }
      st["pull"] = "BLOCKED";
      st["why"] = blocked + (o_.shadow ? " (recorded in shadow)" : "");
    }
  } else {
    if (fd.route && !g.noted[fd.dir] && (blocked.empty() || o_.shadow) &&
        (fd.pull == "BAND" || fd.pull == "TOUCH_AFTER_SCORE" || fd.pull == "TRAILER")) {
      signal(ev, *fd.route, in, score, "PULL", !blocked.empty(), fd.pull + ": " + fd.why, "");
      g.noted[fd.dir] = true;
      ++pulls_;
    }
    st["pull"] = d.pull;
    st["why"] = d.why;
  }
  st["takes"] = g.takes;
  set_last(ev, std::move(st));
}

// gw's answers to the games' takes: a refusal backs the game off and its row says so
void TakeEngine::answers(std::int64_t now) {
  for (auto& [ev, g] : games_) {
    if (!g.answer.valid()) continue;
    OrderReply r;
    bool known = true;   // gw's own answer, not its absence
    if (g.answer.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        r = g.answer.get();
      } catch (const std::future_error&) {
        r = OrderReply{"DROPPED", "gw dropped the take unanswered", ""};
        known = false;
      }
    } else if (now - g.posted_ms > o_.answer_ms) {
      g.answer = {};
      r = OrderReply{"NO_ANSWER", "no answer from gw in " + ms(o_.answer_ms), ""};
      known = false;
    } else {
      continue;
    }
    const auto s = signals_.find(g.answer_id);
    if (s != signals_.end()) s->second.open = false;
    if (r.code.empty()) continue;
    g.backoff_until = now + o_.backoff_ms;
    if (!known) {
      if (env_.alert) env_.alert("WARN", lane_ + " " + ev + ": " + r.msg);
      if (s != signals_.end()) s->second.row["why"] = r.msg;
    } else {
      ++refusals_;
      DLOG_INFO("%s %s: gw refused its take %s (%s)", lane_.c_str(), ev.c_str(), r.code.c_str(), r.msg.c_str());
      if (s != signals_.end()) {
        s->second.row["action"] = "REFUSED";
        s->second.row["why"] = r.code + ": " + r.msg;
      }
    }
    if (s != signals_.end()) record(s->second.row);
  }
}

// each signal's markouts as they come due: the mid 10, 60 and 300 s on, read half a second later
// (md samples a mid at most every 200 ms; Markouts keeps 330 s of them)
void TakeEngine::marks(std::int64_t now) {
  static constexpr std::int64_t kH[3] = {10000, 60000, 300000};
  static constexpr const char* kKey[3] = {"mo10", "mo60", "mo300"};
  for (auto it = signals_.begin(); it != signals_.end();) {
    Signal& s = it->second;
    bool changed = false;
    while (s.marked < 3 && now >= s.ts_ms + kH[s.marked] + 500) {
      const auto mid = env_.mid_at ? env_.mid_at(s.row["tk"].get<std::string>(), s.ts_ms + kH[s.marked]) : std::nullopt;
      s.row[kKey[s.marked]] = mid ? json(money_str(s.side == Side::BUY ? *mid - s.px : s.px - *mid)) : json(nullptr);
      ++s.marked;
      changed = true;
    }
    if (changed) record(s.row);
    it = s.marked == 3 && !s.open ? signals_.erase(it) : std::next(it);
  }
}

std::string TakeEngine::signal(const std::string& ev, const TakeOrder& o, const TakeInput& in, const std::string& score,
                               const char* action, bool shadow, const std::string& why, const std::string& cid) {
  const std::string id = lane_ + "-" + ev + "-" + std::to_string(in.now_ms) + "-" + (o.dir ? "B" : "A");
  auto opt = [](std::int64_t v) { return v > 0 ? json(v) : json(nullptr); };
  json row = {{"id", id},
              {"lane", lane_},
              {"mode", take_mode_str(p_.mode)},
              {"shadow", shadow},
              {"ts_ms", in.now_ms},
              {"tk", o.ticker},
              {"ev", ev},
              {"side", o.side == Side::BUY ? "BUY" : "SELL"},
              {"px", price_str(o.price)},
              {"qty", qty_to_number(o.count)},
              // the market's own YES fair (X1: ESPN's number), none for FAV_PREGAME's band
              {"fair", p_.mode == TakeMode::FAV_PREGAME ? json(nullptr)
                                                        : json(price_str(o.side == Side::BUY ? o.ref : kOneDollar - o.ref))},
              {"edge", money_str(o.edge)},
              {"score", score},
              {"src", in.src},
              {"age_ms", in.fetched_ms > 0 ? json(in.now_ms - in.fetched_ms) : json(nullptr)},
              {"first_seen_ms", opt(in.first_seen_ms)},
              {"touch_since_ms", opt(o.touch_since_ms)},
              {"action", action},
              {"why", why},
              {"cid", cid.empty() ? json(nullptr) : json(cid)},
              {"mo10", nullptr},
              {"mo60", nullptr},
              {"mo300", nullptr},
              {"dir", o.dir ? "B" : "A"},
              {"fee", money_str(o.fee)}};
  Signal s;
  s.row = row;
  s.side = o.side;
  s.px = o.price;
  s.ts_ms = in.now_ms;
  signals_[id] = std::move(s);
  record(row);
  return id;
}

void TakeEngine::record(const json& row) {
  if (!env_.record || !env_.record("signals", row.dump())) ++rows_lost_;
}

void TakeEngine::set_last(const std::string& ev, json j) {
  std::lock_guard<std::mutex> lk(status_mu_);
  last_[ev] = std::move(j);
}

void TakeEngine::markout(const MarkoutRow& row) {
  if (row.lane != lane_ || o_.mo_h.empty() || row.h != o_.mo_h || !env_.post) return;
  if (env_.post(LaneMarkout{row.lane, row.fid, row.h, row.net, row.qty})) ++markouts_;
}

json TakeEngine::status() const {
  json games = json::object();
  std::string hold;
  {
    std::lock_guard<std::mutex> lk(status_mu_);
    for (const auto& [ev, j] : last_) games[ev] = j;
    hold = hold_;
  }
  return {{"lane", lane_},
          {"mode", take_mode_str(p_.mode)},
          {"shadow", o_.shadow},
          {"hold", hold.empty() ? json(nullptr) : json(hold)},
          {"takes", takes_.load()},
          {"shadows", shadows_.load()},
          {"pulls", pulls_.load()},
          {"refusals", refusals_.load()},
          {"rows_lost", rows_lost_.load()},
          {"markouts_to_gw", markouts_.load()},
          {"games", games}};
}

}  // namespace deskd
