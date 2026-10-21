#pragma once
// Systematic's half of gw's book (gw thread only; no locks, no I/O): the lanes and their
// operator switches, each lane's order group per shard, and each lane's day by average cost.
//
// Lanes (plan section 5; budgets from section 4: "LIP-15 $55, band $25, arbitrage $15"):
//   M1 LIP-15        REST  $55   the 15-minute financial markets (natural gas first)
//   M2 BAND 96-99    REST  $25   post-only bids on the 96-99c side
//   S1 ARB           IOC   $15   ladder and dominance arbitrage
//   C1 COLLEGE TAKE  IOC   $10   thin-college late-game stale quotes
//   X1 ESPN EXP      IOC    $5   EXPERIMENT: $1-2 takes on ESPN's direction
// Every lane starts off; the operator turns one on (POST /api/lane/{id}).
//
// Order groups (plan section 3): one per lane that is on and shard, each with a contracts limit over the
// venue's rolling 15 s window of fills (about 2.5 x the per-side size x the markets in it).
// Stop and KILL trigger them (the venue cancels every order in a triggered group and refuses new
// ones); gw resets them once nothing is running, so the next Start finds them active. The GROUPS
// preflight row passes when every lane that is on has an active group on every shard.
//
// A lane's day (contract S.lane): real = realized P&L net of fees of today's fills, by average
// cost per market, and of today's settlements; unrl = the lane's open positions at the mid (the
// last mid seen when there is none now, and the worst case, as if worth nothing, when there never
// was one: the daily stop never counts a position it cannot price as flat); open = their worst
// case (a long can lose its cost, a short its 1 - price); used = open + the reservations of the
// lane's working orders. Positions carry over midnight; the counters and real start again at ET
// midnight.
//
// Fills arrive twice, from the private feed and from each REST read, and each counts once by its
// trade id. A settlement (the market's result, from REST) realizes the position at $1 or $0.
// to_json / restore carry the book across a restart: the day, the positions and HALT.
//
// A lane's life (plan 5(d) and 5(e)'s kill rules): its trades, realized P&L and fees since it
// was first traded or last reset, never rolled at midnight, and its net markouts at the horizon
// its retire rule reads. A lane whose rule fires is retired: off, and kept off across restarts
// until deskctl lane-reset clears its life (decisions 9). Lanes without a rule never retire.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"

namespace deskd {
using Micros = std::int64_t;   // 1/1,000,000 dollar (as kalshi::Micros): Px x Qty

struct LaneSpec {
  std::string id, name, style;          // style: REST or IOC (contract LANE_STYLE)
  Money budget = 0;                     // 1/10,000 dollar
  Qty inventory = 2000;                 // net position limit per market (plan section 4: 20, 40 on LIP series)
  Qty group_limit = 5000;               // contracts over the venue's 15 s window
  std::vector<std::string> series;      // the markets it trades ("" = any)
  std::optional<Money> thr;             // IOC lanes: the net edge per contract it needs
  // false: no engine trades it yet (S1 needs event exclusivity, C1 its model, X1 its backtest), so
  // it cannot be switched on (on, it would only hold order groups that GROUPS waits for)
  bool engine = true;
  // D1 PAPER: gw refuses its orders on the production account (ManualPath::live), and deskd lists
  // it only on demo; on at start, so START on PAPER trades at once (other lanes start off)
  bool paper_only = false, on_at_start = false;
  // the venue it trades (contract LANE_VENUE: K, P, or KP for both), and its budget family: the
  // lanes of one family ("ARB": S1 and W1) share a cap besides their own budgets; "" = none
  std::string venue = "K", family = "";
  Micros event_cap = 0;   // a cap on its worst case in one event (one game), besides the group's; 0 = none
  std::string gate_why = "";   // why its evidence gate keeps it off; "" = open
  // Retirement (SysBook::retire_due): CAP after retire_trades trades; MARKOUT at retire_trades
  // trades or more once SysBook::kRetireMarked fills are marked at retire_h ("300s") and their mean
  // net markout is <= 0; and under any rule, NONE too, once its life's P&L (real - fees +
  // unrealized at the mark) is down retire_loss, at any count. 0 = no such rule.
  enum class RetireRule { NONE, CAP, MARKOUT };
  int retire_trades = 0;
  RetireRule retire_rule = RetireRule::NONE;
  Micros retire_loss = 0;
  std::string retire_h = "";
};
const std::vector<LaneSpec>& default_lanes();
// D1 PAPER (strat/maker.h paper_quote): $20, 2 contracts a market, any market; demo only
LaneSpec paper_lane();

// Correlated groups (plan section 4, limit 2): Systematic's worst case in one group may not pass
// $15, $25 in an LIP-15 group. The 15-minute series group by what moves them together: METALS
// (gold, silver, copper, platinum, palladium), FX (the currency pairs), ENERGY (natural gas, oil);
// any other market by its event (one city-day of weather, one game, one event's outcomes).
std::string corr_group(const std::string& ticker, const std::string& event_ticker);
Micros corr_group_cap(const std::string& group);   // micro-dollars

class SysBook {
 public:
  explicit SysBook(std::vector<LaneSpec> lanes = default_lanes());
  const std::vector<LaneSpec>& lanes() const { return lanes_; }
  const LaneSpec* lane(const std::string& id) const;
  bool on(const std::string& id) const { return on_.count(id) > 0; }
  bool set_on(const std::string& id, bool on);   // false: no such lane
  std::set<std::string> on_lanes() const { return on_; }

  // ---- order groups, by (lane, shard)
  enum class GroupState { NONE, CREATING, ACTIVE, TRIGGERING, TRIGGERED, RESETTING };
  struct Group {
    std::string id;
    GroupState state = GroupState::NONE;
    std::int64_t asked_ms = 0;
    // a reset the venue answered 200 to, still RESETTING until order_group_updates says "reset"
    // (plan section 3: re-arm and Start wait for it); 0 = none pending
    std::int64_t reset_acked_ms = 0;
  };
  Group& group(const std::string& lane, int shard) { return groups_[{lane, shard}]; }
  const Group* find_group(const std::string& lane, int shard) const;
  std::map<std::pair<std::string, int>, Group>& groups() { return groups_; }
  // every lane that is on has an active group on every shard; else why not
  bool groups_ready(const std::set<int>& shards, std::string* why) const;

  // ---- a lane's day
  struct Inv {
    Qty pos = 0;        // signed YES position, 1/100 contract
    Micros cost = 0;    // what the position cost (long) or brought in (short), at average
  };
  struct Day {
    std::int64_t day_start_ms = 0;
    int sent = 0, fills = 0;
    std::set<std::string> hit;          // cids with any fill today
    Qty ctrs = 0;
    Micros fees = 0, real = 0;          // real: realized P&L before fees
    std::int64_t last_ms = 0;
    std::map<std::string, Inv> inv;     // by market
  };
  void sent(const std::string& lane, std::int64_t at);
  // false: already counted (the same trade id), or nothing to count
  bool fill(const std::string& lane, const std::string& cid, const std::string& ticker, Side side, Qty count,
            Px yes_price, Micros fee, std::int64_t at, const std::string& trade_id = "");
  // the market settled at value (10000 YES won, 0 NO won): the lane's position there is realized
  // and gone; its realized P&L, or 0 when it held nothing there
  Micros settle(const std::string& lane, const std::string& ticker, Px value, std::int64_t at);
  // (lane, ticker) of every open position
  std::vector<std::pair<std::string, std::string>> held() const;
  // ET midnight: the counters and real start again; positions stay. day_start is that midnight.
  void roll(std::int64_t day_start_ms);
  const Day* day(const std::string& lane) const;
  Qty position(const std::string& lane, const std::string& ticker) const;

  using MarkFn = std::function<std::optional<Px>(const std::string&)>;
  Micros unrealized(const std::string& lane, const MarkFn& mark) const;
  Micros open_worst(const std::string& lane) const;
  // the same, over the lane's markets `in` accepts
  Micros open_worst_in(const std::string& lane, const std::function<bool(const std::string&)>& in) const;
  // Systematic's day across lanes: real - fees + unrl (the daily stop's measure; no LIP yet)
  Micros day_total(const MarkFn& mark) const;
  std::int64_t day_start_ms() const { return day_start_ms_; }

  // ---- a lane's life
  // the newest kLifeIds ids, to count each once: a cid's fills and a fill's markout come within
  // minutes, so one this old does not come again; oldest first
  static constexpr std::size_t kLifeIds = 500;
  struct Ids {
    std::deque<std::string> order;
    std::set<std::string> in;
    bool add(const std::string& id);   // false: already there
  };
  static constexpr int kRetireMarked = 50;   // MARKOUT reads the mean once this many are marked
  struct Life {
    int trades = 0;                     // orders (cids) with any fill
    Micros real = 0, fees = 0;          // as Day's, never rolled
    Ids hit;                            // the cids counted
    std::int64_t first_ms = 0;          // its first trade; 0 = none yet
    std::int64_t mo_n = 0;              // fills marked at the lane's retire_h
    Qty mo_ctrs = 0;                    // their contracts
    std::int64_t mo_sum = 0;            // their net markout, Money x Qty (the mean: mo_sum / mo_ctrs)
    Ids mo_fids;                        // the fills marked
    bool retired = false;
    std::int64_t retired_ms = 0;
    std::string retired_why;
  };
  const Life* life(const std::string& lane) const;
  // a fill's net markout (per contract, + in our favour) at horizon h: counted when h is the
  // lane's retire_h; false: not counted (no such lane, another horizon, the fid already counted)
  bool markout(const std::string& lane, const std::string& fid, const std::string& h, Money mo_net, Qty qty);
  // why l's retire rule fires now; "" when it does not, or the lane is already retired
  std::string retire_due(const LaneSpec& l, const MarkFn& mark) const;
  // retired from `at` for `why`, and off; false: no such lane, or already retired (the first
  // retirement stays)
  bool retire(const std::string& lane, std::int64_t at, const std::string& why);
  // a fresh life: no trades, markouts or retirement (deskctl lane-reset); false: no such lane
  bool reset_life(const std::string& lane);

  // ---- across a restart
  struct Halt {
    bool on = false;
    std::int64_t ms = 0;
    std::string why;
    // the equity floor's HALT (plan section 4, limit 5) lasts until the operator starts Systematic
    // again with the equity back over the floor; the daily stop's ends at ET midnight
    bool floor() const { return on && why.rfind(kFloorWhy, 0) == 0; }
    static constexpr const char* kFloorWhy = "equity floor";
  };
  nlohmann::json to_json(const Halt& halt, std::int64_t saved_ms) const;
  // A saved book: positions and lives always (a retired lane stays off; a book saved before lives
  // were kept starts each afresh); the day's counters, trade ids and a daily-stop HALT only when
  // it was saved on the ET day that began at day_start_ms (a floor HALT on any day). false (and
  // nothing changed) on a malformed one.
  bool restore(const nlohmann::json& j, std::int64_t day_start_ms, Halt* halt, std::string* why);
  // the contract's lane row; a retired or gated lane is OFF with why it is
  // lip: the lane's LIP estimate today (strat/lip.h), its own line: never in "day" or the stop
  nlohmann::json lane_json(const LaneSpec& l, RunState run, Micros reserved, int working, Micros eff_left,
                           const MarkFn& mark, const std::string& blocked, Micros lip = 0) const;

 private:
  std::vector<LaneSpec> lanes_;
  std::set<std::string> on_;
  std::map<std::pair<std::string, int>, Group> groups_;
  std::map<std::string, Day> days_;
  std::map<std::string, Life> lives_;
  std::set<std::string> trades_;   // today's counted trade ids, every lane
  std::int64_t day_start_ms_ = 0;
};

}  // namespace deskd
