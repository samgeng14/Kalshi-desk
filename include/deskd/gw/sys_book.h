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
#include <cstdint>
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
};
const std::vector<LaneSpec>& default_lanes();

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
  // Systematic's day across lanes: real - fees + unrl (the daily stop's measure; no LIP yet)
  Micros day_total(const MarkFn& mark) const;
  std::int64_t day_start_ms() const { return day_start_ms_; }

  // ---- across a restart
  struct Halt { bool on = false; std::int64_t ms = 0; std::string why; };
  nlohmann::json to_json(const Halt& halt, std::int64_t saved_ms) const;
  // A saved book: positions always; the day's counters, trade ids and HALT only when it was saved
  // on the ET day that began at day_start_ms. false (and nothing changed) on a malformed one.
  bool restore(const nlohmann::json& j, std::int64_t day_start_ms, Halt* halt, std::string* why);
  // the contract's lane row
  nlohmann::json lane_json(const LaneSpec& l, RunState run, Micros reserved, int working, Micros eff_left,
                           const MarkFn& mark, const std::string& blocked) const;

 private:
  std::vector<LaneSpec> lanes_;
  std::set<std::string> on_;
  std::map<std::pair<std::string, int>, Group> groups_;
  std::map<std::string, Day> days_;
  std::set<std::string> trades_;   // today's counted trade ids, every lane
  std::int64_t day_start_ms_ = 0;
};

}  // namespace deskd
