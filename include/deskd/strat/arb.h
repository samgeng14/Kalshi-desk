#pragma once
// S1's basket finder (plan 5(c)): baskets of IOC legs in one event that pay more than they cost,
// fees included, in every way the event can settle. It needs no forecast, only the event's shape
// (gw/event_risk.h: its settlement states, from the venue's mutually_exclusive flag and the
// markets' strikes). The old ladder.cpp's find_ladder_arb and find_dominance_arb, on deskd's units
// and books. Pure; any thread. The leg executor (ArbEngine, at the end) fires what it finds
// through gw, on strat.
//
// Three families, each needing what proves its lock:
//   YES_COVER  buy YES on every market of an event proven exhaustive (some market pays YES in
//              every state): at least $1 a contract back whatever settles
//   NO_SET     sell YES (buy NO) on two or more markets of an event the venue flags mutually
//              exclusive: at most one of them pays YES, so |S| - 1 of the NO legs pay at least.
//              The legs are picked as the old scanner did: the fattest bids first, each adding one
//              while the sum has not passed $1 and after that only while it raises the lock per
//              dollar the basket ties up
//   DOMINANCE  sell YES on market i and buy YES on market j where every state paying i pays j
//              ("84 or above" implies "80 or above"; strikes on both): one leg pays in every state
// Whatever a family proposes is then judged on the event's states alone (state_losses): a basket
// qualifies only if its least payout over every state, less what its legs cost walked through the
// book and the bound on their fees (gw/fees.h order_fee_bound), clears Limits::thr a contract.
//
// A leg is an IOC: a BUY takes YES at the asks, a SELL takes the YES bids (buying NO at 1 - the
// bid). It walks everyone else's book (book_without_own, strat/maker.h) from the touch through at
// most Limits::max_ticks grid ticks, so admission's PRICE_BAND (two ticks) never refuses it, and
// stops short of Systematic's own resting order on the other side: a SELL's limit stays above
// every Systematic bid in the market and a BUY's below every Systematic ask, or the venue's
// self-trade prevention (taker_at_cross) would cancel the leg against M2's quote. A leg the touch
// itself would cross is dropped. 1c legs are allowed: deskd has no min_price.
//
// A market can be a leg only if it is open and active, closes over Limits::min_close_lead_ms from
// now, settles today or tomorrow (ET, rest::outcome_date), has fresh fee metadata
// (rest::fee_ready), an exchange index and a price grid, and a book that is not stale or crossed.
// A whole event is left alone while Manual is in it anywhere (ArbMarket::manual): one basket must
// never trade against a hand's position or resting order, nor in a market Manual took over.
//
// Size: whole contracts, at most Limits::max_qty, what every leg's levels show and gw's inventory
// room on its side (Systematic's position across lanes, and its working orders, under
// Limits::inventory a market: SYS_CEILING "inventory"), the lane's budget and the venue's free
// cash. The budget holds what gw will count while the legs work: with deskd.event_risk on
// (Limits::by_state) the event's worst case with every leg contingent at its reservation
// (Plan::contingent_worst), else every leg's reservation market by market (Plan::gross), as gw
// counts S1 today. The cash holds the reservations and the fees. Of what fits, the most contracts
// that still lock thr each (find_baskets' binary search).
//
// Prices in Px and sizes in Qty (bus/units.h); money in micro-dollars, Px x Qty.
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
#include "deskd/gw/event_risk.h"
#include "deskd/gw/fees.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/rest/state.h"

namespace deskd::arb {

enum class Kind : std::uint8_t { YES_COVER, NO_SET, DOMINANCE };
const char* kind_str(Kind k);   // "YES_COVER", "NO_SET", "DOMINANCE"

struct Leg {
  std::string ticker;
  int market = -1;          // its index in the event's shape
  Side side = Side::BUY;    // BUY: YES at the asks; SELL: the YES bids (NO at 1 - the bid)
  Px planned = 0;           // the touch it plans to take
  Px limit = 0;             // the deepest level the basket's size walks to, at most max_ticks through the touch
  Qty shown = 0;            // what the book shows at or better than limit (the scarcer, the earlier it goes)
  Micros cost = 0;          // the basket's size walked: the YES prices bought, or 1 - the bids sold
  Micros fee = 0;           // order_fee_bound over those fills
};

struct Plan {
  std::string event, id;    // id: the engine's, when it fires
  Kind kind = Kind::NO_SET;
  std::vector<Leg> legs;    // the scarcest first
  Qty qty = 0;              // every leg's size
  int shard = -1;           // the legs' exchange index (-1: they differ)
  // cost and fees: the legs' sum; payout_min: the least the legs pay over the event's states;
  // edge = payout_min - cost - fees, at least Limits::thr x qty
  Micros cost = 0, fees = 0, payout_min = 0, edge = 0;
  Micros contingent_worst = 0;   // the event's worst case with every leg working, at its reservation
  Micros gross = 0;              // every leg's reservation (BUY: limit x qty, SELL: (1 - limit) x qty) summed
};

struct Limits {
  Money thr = 100;                     // the lock a basket contract must clear, after fees (LaneSpec S1 thr: 1c)
  Qty max_qty = 1000;                  // a basket's size at most (deskd.arb_max_qty: 10 contracts)
  Qty inventory = 2000;                // gw's limit on Systematic's position in a market (LaneSpec S1 inventory)
  Micros budget_left = 0;              // what the lane's budget, the 70% ceiling and the event's group leave
  Micros cash_left = 0;                // the venue's free cash
  bool by_state = false;               // gw counts S1 by settlement state (deskd.event_risk): budget_left
                                       // holds contingent_worst, else gross
  int max_legs = 6;                    // deskd.arb_max_legs
  int max_ticks = 2;                   // grid ticks a leg may walk through its touch
  Micros fee_precision = kFeePrecision;   // the account's balance precision (deskd.fee_precision)
  std::int64_t min_close_lead_ms = 300000;   // every leg's market closes over 5 min from now
};

// One market of the event, as the finder sees it (arb_market builds it).
struct ArbMarket {
  const kalshi::MarketRow* row = nullptr;   // the listing's
  std::optional<MdBook> book;               // everyone else's book (book_without_own)
  Px own_bid = 0, own_ask = kOneDollar;     // Systematic's highest bid and lowest ask working here (none: 0, $1)
  // gw's inventory measure: Systematic's position here, every lane's, signed; and what is left
  // of its working orders here, by side
  Qty sys_position = 0, sys_buying = 0, sys_selling = 0;
  std::string manual;                       // why Manual keeps the whole event out ("" when it does not)
};

// tk's ArbMarket from what strat holds: book is md's (nullopt: none yet), taken down to everyone
// else's. manual: the market is Manual's (SysView::held, gw's takeover), or the account read
// shows an order resting there that is not Systematic's (Manual's, or another app's: its client
// order id does not start with S), or a position there that Systematic's lanes do not hold, or
// there is no account read yet. The account read lags Systematic's own fills by up to a poll, so
// an event Systematic has just traded in stays out until it agrees (fail closed).
ArbMarket arb_market(const kalshi::MarketRow& row, std::optional<MdBook> book, const Gateway::SysView& v,
                     const rest::Snapshot& s);

// The baskets the event's markets offer now, the best edge first, at most one of each kind (a
// DOMINANCE pair is the best of its pairs). m: the event's markets (any others are ignored).
// none: *why says what stopped each family (Manual in the event, a filter, no lock after fees, no
// size), for the scan log.
std::vector<Plan> find_baskets(const EventShape& s, const std::vector<ArbMarket>& m, std::int64_t now_ms,
                               const Limits& lim, std::string* why = nullptr);

// What holding legs, qty each at its cost and fee, locks over s's states: the least payout less
// the legs' cost and fees (state_losses). A plan's edge; a basket's, on what its legs filled.
Micros plan_edge(const EventShape& s, const std::vector<Leg>& legs, Qty qty);

// ---- The leg executor's basket (ArbEngine, below)

enum class St : std::uint8_t { FIRST, REST, COMPLETE, UNWIND, UNCERTAIN, FROZEN, DONE };
const char* st_str(St s);   // "FIRST", ...

// One order a basket sent
struct BasketOrder {
  // SENT: gw's answer to come; OPEN: gw took it, the view follows it; UNKNOWN: gw answered
  // EXCHANGE (or not at all), the view settles it; ENDED: over, its fill count known; REFUSED: gw
  // refused it, or it never reached gw's book (nothing filled)
  enum class Is : std::uint8_t { SENT, OPEN, UNKNOWN, ENDED, REFUSED };
  std::string cid;
  int leg = -1;                 // its leg's index in the plan
  char role = 'l';              // 'l' the leg itself (FIRST, REST), 'c' a completion, 'u' an unwind
  Side side = Side::BUY;
  Px price = 0;
  Qty count = 0;
  Is is = Is::SENT;
  Qty filled = 0;               // the venue's count: the 201's fill_count, then the view's
  Qty executed = 0;             // its fills booked (SysOrder::executed), what they cost and their fees
  Micros exec_cost = 0, fees = 0;
  std::int64_t sent_ms = 0, done_ms = 0;
  bool prior = false;           // a previous run's: gw follows it no more (its fills are in S1's positions)
  std::string why;              // gw's refusal, or how it ended
};

struct Basket {
  std::string id, event;
  Plan plan;                    // an orphan's legs: what S1 held (Leg::shown its size, Leg::cost its cost)
  St st = St::FIRST;
  St resume = St::FIRST;        // UNCERTAIN: the state it waits in
  bool orphan = false;          // made from S1's positions in an event when s1.json was missing
  bool restored = false;        // read from s1.json at a start: its orders are a previous run's
  bool stuck = false;           // frozen for good
  std::vector<Qty> base;        // S1's position in each leg's market when it began
  std::vector<BasketOrder> orders;
  Qty target = 0;               // what every leg should hold: the first leg's fill
  std::int64_t t0 = 0, deadline = 0, t_end = 0, tried_ms = 0;
  int tries = 0;                // unwind passes
  std::string why;              // its last change of state, in words
};

// What legging cost a basket so far (micro-dollars, < 0 a loss): its unwinds' round trips, each
// leg's own fees on the part it closed and the closing order's fees included, and each completion's
// price past its leg's planned touch (one cent tick on 4 contracts: -$0.04). Orders the view still
// shows are read there (SysOrder's executions), the rest from what the basket kept of them; what a
// restored basket's leg closed past what its kept orders took in, as taken at its planned touch.
Micros legging_pnl(const Basket& b, const Gateway::SysView& v);
// What the basket locks to settlement on the part every leg holds (its least payout a contract
// times that part, less what that part cost and its fees) plus its round trips; an orphan's
// round trips alone. Micro-dollars.
Micros basket_edge(const Basket& b, const Gateway::SysView& v);

struct Params {
  std::string mode = "off";                   // deskd.arb_mode: off, scan (finds and logs, sends nothing), trade
  Limits limits;                              // the finder's (its budget_left and cash_left are the engine's each scan)
  Micros budget = 15000000;                   // the lane's budget (LaneSpec S1: $15)
  std::int64_t complete_ms = 2000;            // a short leg is completed this long after REST went, at most
  std::int64_t unwind_every_ms = 1000;        // an unwind pass this often, at most
  int unwind_tries = 10;                      // unwind passes before it freezes
  std::int64_t uncertain_max_ms = 180000;     // an order uncertain this long freezes its basket
  std::int64_t event_cooldown_ms = 30000;     // an event's next basket this long after the last ended
  Micros legging_pause = 5000000;             // new baskets stop once legging is down this much
  std::int64_t fills_wait_ms = 5000;          // after a start, an account read this old before positions count
  std::int64_t answer_ms = 30000;             // gw's answer awaited this long, then the view decides
  std::int64_t scan_every_ms = 250;           // the finder at most this often
};

}  // namespace deskd::arb

namespace deskd {

// ---- ArbEngine: S1's owner on strat (plan 5(c)), the IOC leg executor. Each step:
//   scan     at most every scan_every_ms: its series' markets that are active, close more than the
//            close lead from now and settle today or tomorrow (ET), by event. It wants their books
//            (Env::want) and their event rows (Env::want_events), with every open basket's. Each
//            event with a row is shaped (event_shape, the series' Env::lattice), its markets read
//            (arb_market, which keeps Manual out) and its baskets found (find_baskets) within what
//            gw would allow it: the lane's budget, the effective 70% ceiling and the event's
//            correlated group, each less what gw counts used there (SysView::worst; with
//            Limits::by_state, S1's stakes by state, risk_used), and the free cash of the event's
//            shards less Systematic's working orders and every leg's admission fee allowance. An
//            arb_scans row an event a minute, or when its result changes.
//   fire     the best basket of an event, when the mode is trade, Systematic runs, the lane is on,
//            it is not paused, no basket is in flight, none is open in the event, the event's
//            cool-down has passed, the fees the IOC lanes charged today (SysView::fees) and the
//            basket's fee bound are under gw's cost budget (Gateway::kCostBudget: past it gw
//            refuses an IOC lane's every order that grows a position, so a basket's other legs
//            too), the private feed is up, S1's positions are rebuilt (below: the
//            start's account read), S1's orders in the event have ended with their fills booked (so
//            where each leg starts is exact), and each leg's shard has 10 write tokens a leg left
//            (Env::write_room). The plan is from this step's books.
//   baskets  each open one's orders followed (gw's answers, then the view), and moved on:
//   FIRST     the scarcest leg alone: an IOC for the basket's size at its planned touch. Nothing
//             filled, or refused: DONE, and the event cools down.
//   REST      every other leg in the same step as that answer: an IOC each for the first leg's fill
//             at its limit.
//   COMPLETE  a leg short once every answer is in, within complete_ms of REST: an IOC a short leg
//             for its shortfall one grid tick through its planned touch, only while the live touch
//             is within that tick and short of Systematic's own order there.
//   UNWIND    still short: each leg's excess over what the least of them holds is closed by an IOC
//             on the other side one tick through the live touch, on a two-sided book no wider than
//             3 ticks or 3c, not reduce_only (admission's REDUCE reads a position that lags); a pass
//             every unwind_every_ms at the then-touch, unwind_tries passes. What a leg holds is S1's
//             position in its market (SysView, built from fills) less where it stood when the
//             basket began, and it is read only once every order of the basket has ended with its
//             fills booked: a 201 that says filled runs ahead of the fills, and an unwind sized
//             from a position that has not caught up would sell the other legs' hedge.
//   UNCERTAIN an order gw answered EXCHANGE (or did not answer in answer_ms): nothing more goes in
//             the event until the view shows it ended (uncertain_max_ms at most).
//   FROZEN    nothing may be sent: Systematic not running (Stop, HALT, KILL), the lane off, the
//             private feed down, scan mode; a CRIT alert names the basket. Once it may send again
//             a frozen basket is only unwound, never completed (decisions 10). Frozen for good, with
//             a CRIT alert, the excess held to settlement where gw's risk book counts it: a market
//             Manual took over (the basket leaves the event and unwinds nothing), the unwind's
//             passes spent, an order uncertain past uncertain_max_ms, a leg under where it began.
//   DONE      every leg holds the same (locked to settlement), or the excess is unwound, or every
//             market of the basket has closed. A baskets row on each change of state.
// Legging: each finished basket's legging_pnl adds to the lane's. Down legging_pause, no new
// basket fires (completions and unwinds still go) until the operator turns the lane off and on,
// which this engine must see happen while paused (a start with the lane off is not it); an alert
// says so either way. The lane's day (SysView::lane_day) down that much also holds new baskets,
// in case s1.json was lost.
// s1.json (Env::state_file, tmp + rename): the open baskets, written before each send, and the
// legging. At a start every open basket is FROZEN until it may send and then unwound from S1's
// positions, once an account read that began after the start is fills_wait_ms old (gw rebuilds
// Systematic's day from that read's fills). With no s1.json, S1's positions in an event become
// an orphan basket, unwound the same way to equal holdings (the excess over the complete set).
// An s1.json that cannot be read pauses new baskets, as legging does.
// Threading: step() on strat; status() from any thread.
class ArbEngine {
 public:
  struct Env {
    std::function<std::shared_ptr<const Gateway::SysView>()> view;
    std::function<std::shared_ptr<const rest::Snapshot>()> snapshot;
    std::function<std::optional<MdBook>(const std::string&)> book;
    std::function<bool(GwMsg)> post;                          // gw's inbox
    std::function<void(std::set<std::string>)> want;          // markets: their books and fee metadata
    std::function<void(std::set<std::string>)> want_events;   // events: their rows (rest::State::set_events)
    std::function<bool()> private_up;
    std::function<double(int shard)> write_room;              // write tokens Systematic may still spend there
    std::function<double(const std::string& series)> lattice; // deskd.arb_lattice: 1 for KXHIGH, KXLOW
    std::function<bool(const std::string& table, std::string row)> record;   // the store: arb_scans, baskets
    std::function<void(const std::string& lvl, const std::string& msg)> alert;
    std::string state_file;                                   // data_dir/s1.json ("": none kept)
  };
  ArbEngine(std::string lane, std::vector<std::string> series, Env env, arb::Params p);
  int step(std::int64_t now_ms);    // ms until it wants the next step: 10 while a basket is in flight, else 250
  nlohmann::json status() const;    // mode, pause, legging, each event's last scan, the open baskets

 private:
  struct Scanned {                  // an event's last scan, for its arb_scans row and status
    std::string sig;
    std::int64_t row_ms = 0;
    nlohmann::json last;
  };
  void load();
  void save();
  void lane_seen(const Gateway::SysView& v);
  bool paused(const Gateway::SysView& v, std::string* why) const;
  bool restart_ready(const rest::Snapshot& s, std::int64_t now);
  void orphans(const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now);
  const EventShape* shape_of(const std::string& event, const rest::Snapshot& s, std::string* why);
  Micros s1_used(const Gateway::SysView& v, const rest::Snapshot& s, const std::function<bool(const std::string&)>& in);
  arb::Limits limits(const EventShape& sh, const Gateway::SysView& v, const rest::Snapshot& s);
  std::string blocked(const std::string& event, const arb::Plan& p, const Gateway::SysView& v, const rest::Snapshot& s,
                      std::int64_t now);
  void scan(const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now);
  std::string fire(const arb::Plan& p, const Gateway::SysView& v, std::int64_t now);
  void send(arb::Basket& b, int leg, char role, Side side, Px price, Qty count, Px touch, std::int64_t now,
            const std::string& suffix);
  void flush();
  void follow(arb::Basket& b, const Gateway::SysView& v, std::int64_t now);
  bool booked(const arb::Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now);
  void drive(arb::Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now);
  void unwind(arb::Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now);
  void set_state(arb::Basket& b, arb::St st, const std::string& why);
  void freeze(arb::Basket& b, const std::string& why, bool stuck, std::int64_t now);
  void finish(arb::Basket& b, const Gateway::SysView& v, const std::string& why, std::int64_t now);
  nlohmann::json basket_row(const arb::Basket& b, const Gateway::SysView* v) const;
  void record(const std::string& table, const nlohmann::json& row);
  void publish(const Gateway::SysView& v);

  std::string lane_;
  std::vector<std::string> series_;
  Env env_;
  arb::Params p_;
  std::vector<arb::Basket> baskets_;                       // open ones
  std::map<std::string, std::future<OrderReply>> answers_; // by cid
  std::vector<PlaceIntent> outbox_;                        // this step's orders, sent once s1.json has them
  std::map<std::string, std::int64_t> cool_until_;         // by event
  std::map<std::string, Scanned> scanned_;                 // by event
  // this scan's shapes by event, and why there is none
  std::map<std::string, std::pair<std::optional<EventShape>, std::string>> shapes_;
  const Gateway::SysView* view_ = nullptr;                 // this step's
  std::uint64_t seq_ = 0;
  Micros legging_ = 0;                                     // finished baskets' legging since the last reset
  std::string state_error_;                                // s1.json could not be read: paused until reset
  bool state_found_ = false, orphans_done_ = false;
  bool ready_ = false;                                     // restart_ready held once: gw keeps S1's positions since
  std::int64_t first_read_ms_ = 0;                         // the first account read begun after boot_ms_ (its account_ms)
  std::int64_t boot_ms_ = 0, next_scan_ms_ = 0;
  int cycle_ = 0;                                          // the reset: 1 seen on while paused, 2 then off
  bool dirty_ = false, pause_alerted_ = false;
  int done_ = 0;
  // status(): written by step(), read from any thread
  mutable std::mutex status_mu_;
  nlohmann::json status_;
  std::atomic<std::int64_t> rows_lost_{0};
};

}  // namespace deskd
