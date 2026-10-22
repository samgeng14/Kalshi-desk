#pragma once
// The IOC game lanes' one decision (decisions 13: one take engine for C1, X1, N1 and N2): from a
// game's two markets, its score and a reference price, whether to take a touch now, which one and
// how many. take_decide is pure and runs on any thread; TakeEngine (at the end) runs it for one
// lane on strat and sends its takes through gw. tools/college_gate.py (C1),
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
// the clip, at most max_count, the touch's size, the game's room (game_cap less the lane's worst
// case in the game) and the market's inventory room, gw's own (SYS_CEILING "inventory"): with x
// Systematic's position in the market sent to, every lane's, in the order's direction
// (sys_position for a BUY, -sys_position for a SELL) and w its working orders on that side,
// max(inventory, |x|) - x - w, so the position ends at most inventory from flat or no further
// from it than it was; whole contracts unless the market trades fractions. The edge is taken
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
//   HELD, GAME_CAP, INVENTORY, CLIP, SIZE
//                      no count: FAV_PREGAME already holds fixed_count of the team; the game's
//                      room, the market's inventory room or the clip buys no contract; the
//                      touch is under one contract
//   NO_EDGE            the edge under edge (not FAV_PREGAME)
//   BAND               the team's price outside [min_px, max_px]; FAV_PREGAME's band
//                      [fav_lo, fav_hi) comes second instead, before the count
//   TOUCH_AFTER_SCORE  stale_only: the score state's change is not dated (no poll before its
//                      first sighting, or that poll over max_poll_gap_ms before it: a feed that
//                      had only begun to read the game, which is when C1 is turned on mid-game),
//                      or the touch was not first seen before the score state was
//   TRAILER            leader_only: the team does not lead
//   FIRED              this signal run already took the team
// The take is the route that passes every check with the largest edge, then the lowest cost a
// contract, then the first in route order. With none, the pull is the reason of the route that got
// furthest down its checks (ties: the larger edge, then route order).
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"
#include "deskd/gw/gateway.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/ref/espn.h"
#include "deskd/strat/game_fair.h"
#include "deskd/strat/markouts.h"

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
  Qty inventory = 0;                    // gw's per-market limit (LaneSpec::inventory) on Systematic's position (0: none)
  int max_takes = 0;                    // takes a game (0: no cap)
  std::int64_t max_score_age_ms = 5000; // plan 5(d): "a score feed at most 5 s old"
  std::int64_t signal_ms = 0;           // the signal first seen at most this long ago (0: any)
  std::int64_t cool_ms = 5000;          // after any take in the game
  Qty max_volume = 0;                   // the game's in-game volume, both markets, under this (0: any)
  bool stale_only = false;              // the touch taken must be older than the score state's first sighting
  std::int64_t max_poll_gap_ms = 10000; // stale_only: ...and that sighting at most this long after the poll before it
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
// Each lane's rule as its spec set it: COLLEGE C1 (20c net, $2 clips, $10 a game, 20 contracts a
// market, 5 s scores, 5 s apart, thin games under 20k contracts, a touch older than the score,
// its change dated to within 10 s);
// MODEL N1 (10c, $2 up to 4 contracts, 10 a market, 4 takes 10 s apart, the state at most 30 s
// old, spreads of 3c or less, 5-95c, no overtime nor the last 2 minutes); ESPN_FOLLOW and
// ESPN_FADE X1 (as N1 but 4 a market, ESPN's number 30 s old at most); FAV_PREGAME N2 (90-95c
// from 60 to 2 minutes before the start, 5 contracts, 5 a market, spreads of 10c or less, the
// score 60 s old at most). The inventories are default_lanes()' (gw/sys_book.h).
TakeParams take_params(TakeMode m);

struct TakeLeg {
  std::string ticker;
  const MdBook* book = nullptr;         // everyone else's: the lane's own orders taken out
  std::optional<double> fee_mult;       // the market's fee_multiplier (none: not known yet)
  bool fractional = false;              // the market takes 0.01-contract steps
  Qty position = 0;                     // the lane's, signed (YES > 0)
  // gw's inventory measure: Systematic's position here, every lane's, signed; and what is left of
  // its working orders here, by side
  Qty sys_position = 0, sys_buying = 0, sys_selling = 0;
  std::int64_t bid_since_ms = 0, ask_since_ms = 0;   // when the touch's price was first seen (0: not known)
};

struct TakeInput {
  TakeLeg a, b;                         // A's YES pays if A's team wins, B's if the other does
  // The score, from A's side (GameFair's row for A and the board it read)
  std::string state, src;               // pre, in, post ("": none); ESPN, NCAA
  std::string score_why;                // why there is no score (the sources disagree); "" when there is
  std::int64_t fetched_ms = 0, first_seen_ms = 0, start_ms = 0;   // the last poll, the state's first sighting, the start
  std::int64_t prev_poll_ms = 0;        // the poll before that sighting (0: none): the state changed between the two
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
  std::optional<TakeOrder> route;       // a pull past a route's count: that route's order (a signal's row)
};
TakeDecision take_decide(const TakeInput& in, const TakeParams& p);

// Every pull code take_decide gives, in the order of the checks above
const std::vector<std::string>& take_pull_codes();

// The most contracts at px whose cost and fee (kalshi_fee_order_cent) come to at most clip: whole
// contracts when whole_only, else 0.01-contract steps; at most max_count (0: no cap). 0 when not
// one step fits. $2 at 48c: 4 contracts ($1.92 and a 7c fee).
Qty clip_count(Money clip, Px px, double fee_mult, Qty max_count, bool whole_only);

// ---- TakeEngine: one IOC game lane's owner (C1 COLLEGE, X1 ESPN_FOLLOW or ESPN_FADE, N1 MODEL,
// N2 FAV_PREGAME). Each step:
//   games   its series' events of two markets, active and dated (by the event ticker, ET) today
//           or yesterday, a late game running past midnight; A is the first market by ticker. It
//           wants their books (Env::want) while the lane is on or it shadows (only tradable
//           series' unless it shadows), and leaves a game the score board calls final.
//   input   take_decide's, from A's side:
//           score   the score board's row (Env::scores, ref/scores.h's merge of ESPN and
//                   NCAA.com): state, source, last poll, the state's first sighting and the poll
//                   before it, start, scores, why there is none (the sources disagree), the
//                   basketball clock (bball_t_left), and ESPN's number (the summary's dated by
//                   its play's first sighting, else the scoreboard's dated by the score state's)
//           fair    GameFair's row (Env::fairs): p0; and p and points only when the row was
//                   priced on the score state the board shows, since GameFair one step behind
//                   would price a new score at the old fair: the game waits the step instead
//           touch   since each touch price was first seen by this engine, at the step that saw
//                   it; a market's first look dates its touch then, so a touch can only look
//                   newer than it is (stale_only fails closed). The books and the feeds' reading
//                   of the game start together when the lane first wants its markets (C1 turned
//                   on mid-game), so the state the feeds first show is not dated either, and
//                   stale_only waits for the next (max_poll_gap_ms)
//           sets    volleyball: when the sets total first went up while it watched (none before)
//           volume  both markets' in-game volume: the listing's volume less its last pregame
//                   reading; with none read, the whole volume (it can only overstate)
//           lane    its positions, its worst case in the game (SysView::worst over both markets,
//                   gw's event cap measure), its takes there and the run's fired teams
//           sys     Systematic's position in each market, every lane's (SysView::positions),
//                   and what is left of its orders there not ended, by side (SysView::orders):
//                   what gw's inventory limit counts
//           books   everyone else's: Systematic's working orders taken out
// A take goes to gw as one IOC PlaceIntent (cid S<lane>-<ms>-<n>, owner SYS, the touch as its
// price, ttl 0, reduce_only and override_guard off, touch and fair for the order row) when the
// lane is on, Systematic runs, the series is tradable, the private feed is up, neither market is
// Manual's (view->held) and no markout hold applies (mo_after of its fills marked and the mean at
// or under -mo_floor). The game then waits for gw's answer (answer_ms at most) and, after a
// refusal, backs off backoff_ms.
//
// A signal run (take_decide's fired): each new score state (the board's first sighting, which
// ScoreMerge keeps while the state lasts, whichever source is fresh) starts a game's run, and a
// decision that finds no edge run_quiet_ms after the last that found one ends it; a game check
// that hides the edge (the cool-down, an old score, no score at all: none, or the sources at
// odds) neither ends nor extends it.
// In a run each team is taken, or recorded, once.
//
// Signals go to the store's "signals" table, a row per team and signal (store/store.h):
//   TAKE     posted (sent again as REFUSED with gw's code if gw refuses it)
//   SHADOW   TakeOptions::shadow: what it would take while it may not post (the lane off,
//            Systematic not running, the series not tradable, the private feed down, a market
//            Manual's, the markout hold); it counts as taken in the run and the game (fired,
//            cool-down, takes)
//   PULL     a team with the edge that a later check pulled: BAND, TOUCH_AFTER_SCORE, TRAILER
// and each row is sent again with mo10, mo60 and mo300: the gross markout a contract at its
// market's mid (Env::mid_at) 10, 60 and 300 s on, + in its favour (null with no mid).
// The markouts of its own fills at TakeOptions::mo_h go to gw as LaneMarkout (C1: "300s", its
// retire rule's), from the rows the runtime's markouts step hands to markout().
//
// Rules: one TakeParams for every series, or one for each series in one mode (C1's gate gives
// each series its level and leader_only: what it measured there).
//
// Threading: step() and markout() on strat; status() from any thread.

// TakeEngine's own rules, outside take_decide
struct TakeOptions {
  bool shadow = false;                  // record what it would take while it may not post
  std::int64_t run_quiet_ms = 10000;    // a signal run ends this long after its edge was last seen
  std::int64_t backoff_ms = 10000;      // a game posts nothing this long after gw refuses its take
  std::int64_t answer_ms = 30000;       // gw's answer awaited at most this long
  std::string mo_h;                     // its fills' markouts at this horizon go to gw ("": none)
  std::int64_t mo_after = 0;            // the markout hold: once this many fills are marked (0: none)
  Money mo_floor = 0;                   // ...a mean at or under -mo_floor a contract holds new takes
};

class TakeEngine {
 public:
  struct Env {
    std::function<std::shared_ptr<const Gateway::SysView>()> view;
    std::function<std::shared_ptr<const rest::Snapshot>()> snapshot;
    std::function<std::optional<MdBook>(const std::string&)> book;
    std::function<std::shared_ptr<const GameBoard>()> fairs;        // GameFair::board()
    std::function<std::shared_ptr<const EspnBoard>()> scores;       // the score board (ScoreMerge::board())
    // series -> "" when the lane may trade it, else why not (a series missing: no gate for it)
    std::function<std::map<std::string, std::string>()> tradable;
    std::function<bool()> private_up;
    std::function<bool(GwMsg)> post;                                // gw's inbox
    std::function<void(std::set<std::string>)> want;                // the markets whose books it needs
    std::function<std::pair<std::int64_t, Money>()> markout;        // its fills marked, and their mean (the hold)
    std::function<std::optional<Px>(const std::string&, std::int64_t)> mid_at;   // Markouts::mid_at
    std::function<bool(const std::string& table, std::string row)> record;       // the store
    std::function<void(const std::string& lvl, const std::string& msg)> alert;
  };
  TakeEngine(std::string lane, std::vector<std::string> series, Env env, TakeParams params, TakeOptions options = {});
  // each series with its own rule; every rule in one mode (std::invalid_argument otherwise)
  TakeEngine(std::string lane, std::map<std::string, TakeParams> series, Env env, TakeOptions options = {});
  void step(std::int64_t now_ms);
  void markout(const MarkoutRow& row);    // each markout row, once (the runtime's markouts step)
  nlohmann::json status() const;          // per game: the last decision; the counts of signals and refusals

 private:
  struct Game {
    std::int64_t run_state = -1, edge_ms = 0;   // the run's score state (its first sighting); its edge last seen
    bool fired[2] = {false, false}, noted[2] = {false, false};   // taken (or shadowed), recorded as PULL
    std::int64_t last_take_ms = 0;
    int takes = 0;
    std::future<OrderReply> answer;       // gw's answer to the last take
    std::string answer_id;                // that take's signal row
    std::int64_t posted_ms = 0, backoff_until = 0;
    int sets = -1;                        // volleyball: the sets total last seen (-1: none)
    std::int64_t set_end_ms = 0;
    std::map<std::string, Qty> pre_volume;   // each market's volume at its last pregame reading
    std::int64_t seen_ms = 0;             // the last step that listed it
  };
  struct Touch {
    Px bid = -1, ask = -1;
    std::int64_t bid_ms = 0, ask_ms = 0;
  };
  struct Signal {                         // a row whose markouts (or gw's answer) are still to come
    nlohmann::json row;
    Side side = Side::BUY;
    Px px = 0;
    std::int64_t ts_ms = 0;
    int marked = 0;                       // horizons done: 10 s, 60 s, 300 s
    bool open = false;                    // gw's answer to come
  };
  void game(const std::string& ev, const kalshi::MarketRow& ma, const kalshi::MarketRow& mb, const Gateway::SysView& v,
            const GameBoard* fairs, const EspnBoard* scores, const std::string& not_tradable, const std::string& hold,
            std::int64_t now);
  void answers(std::int64_t now);
  void marks(std::int64_t now);
  std::string signal(const std::string& ev, const TakeOrder& o, const TakeInput& in, const std::string& score,
                     const char* action, bool shadow, const std::string& why, const std::string& cid);
  void record(const nlohmann::json& row);
  void set_last(const std::string& ev, nlohmann::json j);
  std::string new_cid(std::int64_t now);
  std::string lane_;
  std::vector<std::string> series_;
  Env env_;
  TakeParams p_;                          // the lane's mode (the first series' rule)
  std::map<std::string, TakeParams> params_;   // each series' rule
  TakeOptions o_;
  std::map<std::string, Game> games_;     // by event ticker
  std::map<std::string, Touch> touch_;    // by market
  std::map<std::string, Signal> signals_; // by row id
  std::uint64_t seq_ = 0;
  bool holding_ = false;                  // the markout hold, as of the last step
  // status(): the per-game decisions under status_mu_, the counts atomic
  mutable std::mutex status_mu_;
  std::map<std::string, nlohmann::json> last_;
  std::string hold_;
  std::atomic<std::int64_t> takes_{0}, shadows_{0}, pulls_{0}, refusals_{0}, rows_lost_{0}, markouts_{0};
};

}  // namespace deskd
