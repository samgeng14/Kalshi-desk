#pragma once
// The IOC game lanes' one decision (decisions 13: one take engine for C1, X1, N1 and N2): from a
// game's two markets, its score and a reference price, whether to take a touch now, which one and
// how many. take_decide is pure and runs on any thread. tools/college_gate.py (C1),
// tools/x1_backtest.py (X1) and tools/nba_gate.py (N1, N2) carry the same rule in Python, so a
// gate measures exactly what its lane trades (plan 5(e): "the S2 mistake was testing one order
// type and trading another"). The hand-worked cases in tests/deskd/fixtures/
// college_take_cases.json, x1_golden.json and n1_golden.json pin both languages.
//
// A game is two markets: A's YES pays if A's team wins, B's if the other team does. "A wins" can
// be bought two ways, BUY A at its ask or SELL B at its bid (B's NO at 1 - the bid), and "B wins"
// likewise. Each way is a route, and the team's price on it is what one contract of that team
// costs: the ask, or 1 - the other market's bid. A take is an IOC at the touch itself, with no
// slippage.
//
// Modes, by what a route is priced against (per contract):
//   COLLEGE      C1 (plan 5(d)): GameFair's model fair for the team - its price - the fee / count,
//                at least edge (20c); WCBB and volleyball
//   MODEL        N1: the same against the NBA model's fair, at least the gate's thr
//   ESPN_FOLLOW  X1: ESPN's win probability for the team - its price - the fee / count, at least thr
//   ESPN_FADE    X1: Kalshi's effective mid for the team - ESPN's probability, at least thr (Kalshi
//                has moved past ESPN's number: buy the team it moved toward)
//   FAV_PREGAME  N2 (sports hunt: pregame [90, 95) favourites): the team's price in
//                [fav_lo, fav_hi) from fav_from_ms to fav_to_ms before the start, bought up to
//                fixed_count held; no edge, the band is the rule
// COLLEGE and MODEL differ only in their defaults (take_params).
//
// Size: the most contracts whose cost and fee (kalshi_fee_order_cent, gw/fees.h's cent rule) fit
// the clip, at most max_count, the touch's size and the game's room (game_cap less the lane's
// worst case in the game); whole contracts unless the market trades fractions. The edge is taken
// at that count, so a smaller take pays more fee a contract. All of it is integer arithmetic: a
// route's edge is ((ref - price) x count - fee x 100) / count, in micro-dollars over Qty, which is
// Money a contract, compared with edge exactly and reported rounded down; FADE's is
// (eff bid + eff ask - 2 x ESPN) / 2.
//
// The game's checks, in this order; the first it fails is the pull:
//   NOT_TRADABLE  the gate does not let the lane trade the series
//   PRIVATE_DOWN  the private feed is down
//   STALE_BOOK    a market has no book, or a stale or crossed one
//   NO_FEE        a market's fee multiplier is not known yet
//   NO_SCORE      no score (none, or ESPN and NCAA disagree)
//   SCORE_OLD     the score's last poll over max_score_age_ms ago (5 s)
//   STATE         not in play (FAV_PREGAME: not before it)
//   WINDOW        FAV_PREGAME: now outside [start - fav_from_ms, start - fav_to_ms]
//   NO_P0         COLLEGE, MODEL: no pregame price
//   NO_FAIR       COLLEGE, MODEL: no fair
//   NO_ESPN       FOLLOW, FADE: no ESPN probability
//   NO_MID        FADE: no two-sided effective book
//   NO_POINTS     points_required and the set's points were not read (volleyball, level "point")
//   SET_WINDOW    not points_required (level "set"): over set_window_ms since a set ended, or none has
//   OVERTIME      in overtime, unless trade_ot
//   LATE          under min_t_left_s of regulation left, or no clock
//   SIGNAL_OLD    the signal first seen over signal_ms ago: the score state, or (FOLLOW, FADE)
//                 ESPN's number
//   VOLUME        the game's in-game volume at or over max_volume: not a thin game
//   SPREAD        a book one-sided, or wider than max_spread
//   COOL_DOWN     under cool_ms since the lane's last take in the game
//   TAKES         max_takes already taken in the game
//   GAME_CAP      the lane's worst case in the game at game_cap
// Then each route, in the order A BUY, A SELL, B BUY, B SELL, is checked in this order:
//   NO_TOUCH           no ask (BUY) or bid (SELL) to take
//   HELD, GAME_CAP, CLIP, SIZE
//                      no count: FAV_PREGAME already holds fixed_count of the team; the game's
//                      room or the clip buys no contract; the touch is under one contract
//   NO_EDGE            the edge under edge (not FAV_PREGAME)
//   BAND               the team's price outside [min_px, max_px]; FAV_PREGAME's band
//                      [fav_lo, fav_hi) comes second instead, before the count
//   TOUCH_AFTER_SCORE  stale_only: the touch was not first seen before the score state was
//   TRAILER            leader_only: the team does not lead
//   FIRED              this signal run already took the team
// The take is the route that passes every check with the largest edge, then the lowest cost a
// contract, then the first in route order. With none, the pull is the reason of the route that got
// furthest down its checks (ties: the larger edge, then route order).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"
#include "deskd/md/kalshi_md.h"

namespace deskd {

using Micros = std::int64_t;   // 1/1,000,000 dollar (as gw/fees.h)

enum class TakeMode : std::uint8_t { COLLEGE, MODEL, ESPN_FOLLOW, ESPN_FADE, FAV_PREGAME };
const char* take_mode_str(TakeMode m);   // "COLLEGE", ...
std::optional<TakeMode> parse_take_mode(const std::string& s);

struct TakeParams {
  TakeMode mode = TakeMode::COLLEGE;
  Money edge = 2000;                    // the least a contract (COLLEGE: 20c; MODEL, FOLLOW: thr; FADE: the gap)
  Money clip = 2 * kOneDollar;          // a take's cost, fee included
  Qty max_count = 0;                    // a take's contracts at most (0: no cap)
  Qty fixed_count = 0;                  // FAV_PREGAME: the team held up to this
  Micros game_cap = 10000000;           // the lane's worst case in one game, working included (0: none)
  int max_takes = 0;                    // takes a game (0: no cap)
  std::int64_t max_score_age_ms = 5000; // plan 5(d): "a score feed at most 5 s old"
  std::int64_t signal_ms = 0;           // the signal first seen at most this long ago (0: any)
  std::int64_t cool_ms = 5000;          // after any take in the game
  Qty max_volume = 0;                   // the game's in-game volume, both markets, under this (0: any)
  bool stale_only = false;              // the touch taken must be older than the score state's first sighting
  bool leader_only = false;             // only the team that leads (a tie leads neither)
  bool points_required = true;          // false: the gate's level "set", only within set_window_ms of a set's end
  std::int64_t set_window_ms = 120000;
  Px max_spread = 0;                    // each book two-sided, its spread at most this (0: any)
  Px min_px = 0, max_px = kOneDollar;   // the team's price, inclusive
  bool trade_ot = true;
  double min_t_left_s = 0;              // basketball: regulation time left at least this (0: any)
  Px fav_lo = 9000, fav_hi = 9500;      // FAV_PREGAME's band, [lo, hi)
  std::int64_t fav_from_ms = 3600000, fav_to_ms = 120000;   // FAV_PREGAME: from start - from to start - to
};
// Each lane's rule as its spec set it: COLLEGE C1 (20c net, $2 clips, $10 a game, 5 s scores,
// 5 s apart, thin games under 20k contracts, a touch older than the score); MODEL N1 (10c, $2 up
// to 4 contracts, 4 takes 10 s apart, the state at most 30 s old, spreads of 3c or less, 5-95c,
// no overtime nor the last 2 minutes); ESPN_FOLLOW and ESPN_FADE X1 (as N1, ESPN's number 30 s
// old at most); FAV_PREGAME N2 (90-95c from 60 to 2 minutes before the start, 5 contracts,
// spreads of 10c or less, the score 60 s old at most).
TakeParams take_params(TakeMode m);

struct TakeLeg {
  std::string ticker;
  const MdBook* book = nullptr;         // everyone else's: the lane's own orders taken out
  std::optional<double> fee_mult;       // the market's fee_multiplier (none: not known yet)
  bool fractional = false;              // the market takes 0.01-contract steps
  Qty position = 0;                     // the lane's, signed (YES > 0)
  std::int64_t bid_since_ms = 0, ask_since_ms = 0;   // when the touch's price was first seen (0: not known)
};

struct TakeInput {
  TakeLeg a, b;                         // A's YES pays if A's team wins, B's if the other does
  // The score, from A's side (GameFair's row for A and the board it read)
  std::string state, src;               // pre, in, post ("": none); ESPN, NCAA
  std::string score_why;                // why there is no score (the sources disagree); "" when there is
  std::int64_t fetched_ms = 0, first_seen_ms = 0, start_ms = 0;   // the last poll, the state's first sighting, the start
  int a_score = 0, b_score = 0;         // points (volleyball: sets)
  std::optional<double> t_left_s;       // basketball: regulation time left
  bool ot = false;
  bool points = true;                   // volleyball: the set's points were read (GameFairRow::points)
  std::int64_t set_end_ms = 0;          // volleyball: when the latest set's end was first seen (0: none)
  std::optional<Px> p0, fair;           // A's pregame price and P(A's team wins): COLLEGE, MODEL
  std::string fair_why;                 // why no fair (GameFairRow::why)
  std::optional<Px> espn;               // ESPN's P(A's team wins): FOLLOW, FADE
  std::int64_t espn_seen_ms = 0;        // that number's first sighting
  // The lane in this game
  std::int64_t now_ms = 0, last_take_ms = 0;   // last_take_ms 0: none
  Micros game_used = 0;                 // its worst case in the game, working included
  int game_takes = 0;
  bool fired[2] = {false, false};       // this signal run already took A's team / B's
  bool private_up = false;
  std::string not_tradable;             // why the lane may not trade the series ("" it may)
  Qty volume = 0;                       // the game's in-game volume, both markets
};

struct TakeOrder {
  std::string ticker;                   // the market sent to
  Side side = Side::BUY;                // BUY the team's market, or SELL the other's
  Px price = 0;                         // the touch itself: the ask, or the bid
  Qty count = 0;
  Money fee = 0;                        // kalshi_fee_order_cent
  Money edge = 0;                       // a contract, rounded down (FADE: the gap; FAV_PREGAME: 0)
  Px team_px = 0;                       // the team's price: the ask, or 1 - the bid
  Px ref = 0;                           // the team's fair or ESPN's number (FADE: ESPN's; FAV_PREGAME: 0)
  Micros cost = 0;                      // the worst case: team_px x count + the fee
  int dir = 0;                          // 0: A's team, 1: B's
  std::int64_t touch_since_ms = 0;
};
struct TakeDecision {
  std::optional<TakeOrder> take;
  std::string pull, why;                // pull: the reason's code ("" when taking); why: in words
  std::optional<Money> edge;            // the take's edge, or the pulled route's when it had one
  int dir = -1;                         // the take's team, or the pulled route's (-1: the game's checks)
};
TakeDecision take_decide(const TakeInput& in, const TakeParams& p);

// Every pull code take_decide gives, in the order of the checks above
const std::vector<std::string>& take_pull_codes();

// The most contracts at px whose cost and fee (kalshi_fee_order_cent) come to at most clip: whole
// contracts when whole_only, else 0.01-contract steps; at most max_count (0: no cap). 0 when not
// one step fits. $2 at 48c: 4 contracts ($1.92 and a 7c fee).
Qty clip_count(Money clip, Px px, double fee_mult, Qty max_count, bool whole_only);

}  // namespace deskd
