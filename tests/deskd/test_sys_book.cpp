// Systematic's book (gw/sys_book.h): lanes, groups, a lane's day by average cost, and its life.
#include <algorithm>
#include <cstdio>
#include "check.h"
#include "deskd/gw/sys_book.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
void lanes_and_groups() {
  SysBook b;
  CHECK(b.lanes().size() == 10 && b.lane("M1")->name == "LIP-15" && b.lane("M1")->budget == 550000);
  CHECK(b.lane("P1") && b.lane("P1")->name == "LIP PROBE" && b.lane("P1")->inventory == 4000 && !b.on("P1"));
  CHECK(!b.on("M1") && b.on_lanes().empty() && !b.set_on("Z9", true));
  std::string why;
  CHECK(b.groups_ready({0, 1}, &why) && why == "no lane is on");
  CHECK(b.set_on("M1", true) && b.on("M1"));
  CHECK(!b.groups_ready({0, 1}, &why) && why == "M1 shard 0: order group none");
  b.group("M1", 0) = {"og-0", SysBook::GroupState::ACTIVE, 1};
  b.group("M1", 1) = {"og-1", SysBook::GroupState::TRIGGERED, 1};
  CHECK(!b.groups_ready({0, 1}, &why) && why == "M1 shard 1: order group triggered");
  b.group("M1", 1).state = SysBook::GroupState::ACTIVE;
  CHECK(b.groups_ready({0, 1}, &why) && why.empty());
  CHECK(b.set_on("M1", false) && !b.on("M1"));
}

// Every lane's row (decisions 1 and the lane tracks' specs): its budget, venue, limits and rules.
// The lanes no engine trades yet have engine=false, and no lane is on at start.
void default_rows() {
  using R = LaneSpec::RetireRule;
  const auto& L = default_lanes();
  std::vector<std::string> ids;
  for (const auto& l : L) ids.push_back(l.id);
  CHECK(ids == std::vector<std::string>({"M1", "M2", "P1", "S1", "C1", "X1", "N1", "N2", "Q1", "W1"}));
  struct Row {
    const char* id;
    const char* name;
    const char* style;
    const char* venue;
    const char* family;
    Money budget;
    Qty inventory, group_limit;
    std::optional<Money> thr;
    bool engine;
    Micros event_cap;
    R rule;
    int trades;
    Micros loss;
    const char* h;
  };
  const std::vector<Row> want = {
      {"M1", "LIP-15", "REST", "K", "", 550000, 4000, 5000, std::nullopt, true, 0, R::NONE, 0, 0, ""},
      {"M2", "BAND 96-99", "REST", "K", "", 250000, 2000, 5000, std::nullopt, true, 0, R::NONE, 0, 0, ""},
      {"P1", "LIP PROBE", "REST", "K", "", 250000, 4000, 7500, std::nullopt, true, 0, R::NONE, 0, 0, ""},
      {"S1", "ARB", "IOC", "K", "ARB", 150000, 2000, 30000, Money{100}, false, 0, R::NONE, 0, 0, ""},
      {"C1", "COLLEGE TAKE", "IOC", "K", "", 400000, 2000, 5000, Money{2000}, false, 10000000, R::MARKOUT, 100, 20000000, "300s"},
      {"X1", "ESPN EXP", "IOC", "K", "", 50000, 400, 1000, Money{300}, false, 10000000, R::CAP, 100, 20000000, ""},
      {"N1", "NBA MODEL", "IOC", "K", "", 200000, 1000, 2000, Money{1000}, false, 10000000, R::NONE, 0, 20000000, ""},
      {"N2", "NBA FAV 90-95", "IOC", "K", "", 150000, 500, 1000, std::nullopt, false, 0, R::CAP, 60, 15000000, ""},
      {"Q1", "PM QUOTE", "REST", "P", "", 200000, 2000, 5000, std::nullopt, false, 0, R::NONE, 0, 0, ""},
      {"W1", "WX 2-VENUE", "IOC", "KP", "ARB", 100000, 2000, 5000, Money{100}, false, 0, R::NONE, 0, 0, ""},
  };
  for (const auto& w : want) {
    const auto it = std::find_if(L.begin(), L.end(), [&](const LaneSpec& l) { return l.id == w.id; });
    if (it == L.end()) { CHECK(!"every lane has a row"); continue; }
    const auto& l = *it;
    const int failed = fails;
    CHECK(l.name == w.name && l.style == w.style && l.venue == w.venue && l.family == w.family && l.budget == w.budget);
    CHECK(l.inventory == w.inventory && l.group_limit == w.group_limit && l.thr == w.thr && l.engine == w.engine);
    CHECK(l.event_cap == w.event_cap && l.retire_rule == w.rule && l.retire_trades == w.trades && l.retire_loss == w.loss &&
          l.retire_h == w.h);
    CHECK(!l.paper_only && !l.on_at_start && l.gate_why.empty());
    if (fails != failed) std::fprintf(stderr, "  (lane %s)\n", w.id);
  }
  // every lane off at start; the Kalshi budgets sum to $210 (a KP lane's counts on Kalshi), past
  // any ceiling under $300 of equity, so gw decides which may be on together (decisions 1)
  SysBook b;
  CHECK(b.on_lanes().empty());
  Money k = 0;
  for (const auto& l : L) if (l.venue.find('K') != std::string::npos) k += l.budget;
  CHECK(k == 210 * 10000);
  // a lane's row carries its venue and thr
  CHECK(b.lane_json(*b.lane("Q1"), RunState::RUN, 0, 0, 0, {}, "")["venue"] == "P");
  CHECK(b.lane_json(*b.lane("W1"), RunState::RUN, 0, 0, 0, {}, "")["venue"] == "KP");
  CHECK(b.lane_json(*b.lane("C1"), RunState::RUN, 0, 0, 0, {}, "")["thr"] == "0.2000");
  CHECK(b.lane_json(*b.lane("N2"), RunState::RUN, 0, 0, 0, {}, "")["thr"].is_null());
}

void lane_day() {
  SysBook b;
  const std::int64_t t = 1791000000000;
  b.roll(t);
  b.sent("M2", t);
  // buy 10 at 0.40, then 10 more at 0.50: long 20 at an average 0.45
  b.fill("M2", "SM2-1", "TK-A", Side::BUY, 1000, 4000, 70000, t + 1);
  b.fill("M2", "SM2-2", "TK-A", Side::BUY, 1000, 5000, 0, t + 2);
  CHECK(b.position("M2", "TK-A") == 2000 && b.day("M2")->inv.at("TK-A").cost == 9000000);
  // sell 15 at 0.60: realized (0.60 - 0.45) x 15 = $2.25; 5 left at 0.45
  b.fill("M2", "SM2-3", "TK-A", Side::SELL, 1500, 6000, 0, t + 3);
  CHECK(b.position("M2", "TK-A") == 500 && b.day("M2")->real == 2250000);
  CHECK(b.open_worst("M2") == 2250000);   // a long can lose its cost
  // marked at 0.55: unrealized (0.55 - 0.45) x 5 = $0.50; day = 2.25 - fees 0.07 + 0.50
  const auto mark = [](const std::string&) { return std::optional<Px>(5500); };
  CHECK(b.unrealized("M2", mark) == 500000);
  CHECK(b.day_total(mark) == 2250000 - 70000 + 500000);
  // sell 10 more: closes 5, opens a short of 5 at 0.30
  b.fill("M2", "SM2-4", "TK-A", Side::SELL, 1000, 3000, 0, t + 4);
  CHECK(b.position("M2", "TK-A") == -500 && b.day("M2")->inv.at("TK-A").cost == 1500000);
  CHECK(b.day("M2")->real == 2250000 + (3000 - 4500) * 500);
  CHECK(b.open_worst("M2") == 10000 * 500 - 1500000);   // a short can lose 1 - its price
  CHECK(b.unrealized("M2", mark) == 1500000 - 5500 * 500);
  // never priced: counted worthless, never flat (a short of 5 at 0.30 owes $1 a contract)
  CHECK(b.unrealized("M2", [](const std::string&) { return std::optional<Px>(); }) == 1500000 - 10000 * 500);
  // the contract's row
  const auto row = b.lane_json(*b.lane("M2"), RunState::RUN, 780000, 1, 100000000, mark, "");
  CHECK(row["state"] == "OFF" && row["why"] == "" && row["sent"] == 1 && row["fills"] == 4 && row["hit"] == 4);
  CHECK(row["ctrs"] == 45.0 && row["fees"] == "0.0700" && row["budget"] == "25.0000");
  CHECK(row["open"] == "3.5000" && row["used"] == "4.2800");
  b.set_on("M2", true);
  CHECK(b.lane_json(*b.lane("M2"), RunState::RUN, 0, 0, 100000000, mark, "")["state"] == "ON");
  CHECK(b.lane_json(*b.lane("M2"), RunState::STOP, 0, 0, 100000000, mark, "")["why"] == "STOP");
  CHECK(b.lane_json(*b.lane("M2"), RunState::RUN, 0, 0, 100000000, mark, "order group on shard 0 not active")["state"] == "BLOCKED");
  CHECK(b.lane_json(*b.lane("M2"), RunState::RUN, 0, 0, 0, mark, "")["state"] == "CAPPED");
  // a new day: counters and real start again, the position stays
  b.roll(t + 86400000);
  CHECK(b.day("M2")->sent == 0 && b.day("M2")->real == 0 && b.position("M2", "TK-A") == -500);
}

// A fill counted once by trade id (it arrives on the private feed and on every REST read); a
// settlement realizes the position at $1 or $0; the book across a restart.
void settle_and_restore() {
  SysBook b;
  const std::int64_t day = 1791000000000, t = day + 3600000;
  b.roll(day);
  CHECK(b.fill("M1", "SM1-1", "TK-A", Side::BUY, 1000, 6000, 0, t, "trade-1"));
  CHECK(!b.fill("M1", "SM1-1", "TK-A", Side::BUY, 1000, 6000, 0, t, "trade-1"));   // the REST copy
  CHECK(b.position("M1", "TK-A") == 1000 && b.day("M1")->fills == 1);
  CHECK(b.fill("M2", "SM2-1", "TK-B", Side::SELL, 500, 9700, 3500, t, "trade-2"));
  CHECK(b.held().size() == 2);
  // TK-A settles NO: the long of 10 at 0.60 loses $6.00, and the position is gone
  CHECK(b.settle("M1", "TK-A", 0, t + 1) == -6000000);
  CHECK(b.position("M1", "TK-A") == 0 && b.day("M1")->real == -6000000 && b.open_worst("M1") == 0);
  CHECK(b.settle("M1", "TK-A", 0, t + 2) == 0);   // once
  CHECK(b.day_total({}) == -6000000 + (9700 - 10000) * 500 - 3500);   // TK-B unpriced: worst case
  // HALTED, saved, and read back on the same day: the day, the trade ids and the HALT
  const SysBook::Halt h{true, t + 5, "daily stop"};
  const json saved = b.to_json(h, t + 6);
  SysBook c;
  SysBook::Halt back;
  std::string why;
  CHECK(c.restore(saved, day, &back, &why));
  CHECK(back.on && back.ms == t + 5 && back.why == "daily stop");
  CHECK(c.day("M1")->real == -6000000 && c.position("M2", "TK-B") == -500 && c.day("M2")->fees == 3500);
  CHECK(!c.fill("M2", "SM2-1", "TK-B", Side::SELL, 500, 9700, 3500, t, "trade-2"));   // still counted once
  // read back the next day: the positions only, no HALT
  SysBook n;
  CHECK(n.restore(saved, day + 86400000, &back, &why) && !back.on);
  CHECK(n.position("M2", "TK-B") == -500 && n.day("M2")->fees == 0 && n.day("M1")->real == 0);
  CHECK(n.fill("M2", "SM2-1", "TK-B", Side::SELL, 500, 9700, 3500, t, "trade-2"));   // a new day's ids
  // the equity floor's HALT is read back on any day: it lasts until the operator's Start
  const SysBook::Halt fl{true, t + 5, "equity floor: the account's equity $120 is under $150"};
  CHECK(fl.floor() && !h.floor() && !SysBook::Halt{}.floor());
  SysBook f;
  CHECK(f.restore(b.to_json(fl, t + 6), day + 86400000, &back, &why) && back.on && back.floor());
  // a malformed one changes nothing
  SysBook m;
  CHECK(!m.restore(json{{"v", 1}, {"lanes", 3}}, day, &back, &why) && !why.empty() && m.held().empty());
  CHECK(!m.restore(json{{"v", 2}}, day, &back, &why));
}

// A rebate (a negative fee) is P&L, so the day's and the life's fees are net of it; the IOC lanes'
// cost budget reads charged, each fill's max(0, fee) (decisions 6). Both read back; a book saved
// before charged was kept takes its fees.
void rebates() {
  SysBook b;
  const std::int64_t day = 1791000000000, t = day + 3600000;
  b.roll(day);
  CHECK(b.fill("M1", "SM1-1", "TK-A", Side::BUY, 1000, 4000, 70000, t, "tr-1"));
  CHECK(b.fill("M1", "SM1-2", "TK-A", Side::SELL, 1000, 4100, -20000, t + 1, "tr-2"));   // a maker's rebate
  CHECK(b.day("M1")->fees == 50000 && b.day("M1")->charged == 70000 && b.life("M1")->fees == 50000);
  CHECK(b.day("M1")->real == 100000 && b.day_total({}) == 100000 - 50000);
  const json saved = b.to_json({}, t + 2);
  SysBook::Halt back;
  std::string why;
  SysBook c;
  CHECK(c.restore(saved, day, &back, &why) && c.day("M1")->charged == 70000 && c.day("M1")->fees == 50000);
  json old = saved;
  old["lanes"]["M1"].erase("charged");
  SysBook o;
  CHECK(o.restore(old, day, &back, &why) && o.day("M1")->charged == 50000);
  b.roll(day + 86400000);   // a new day's budget
  CHECK(b.day("M1")->charged == 0 && b.day("M1")->fees == 0);
}

// default_lanes() with C1's and X1's retire rules at small counts
std::vector<LaneSpec> ruled_lanes() {
  auto lanes = default_lanes();
  for (auto& l : lanes) {
    if (l.id == "C1") {
      l.retire_rule = LaneSpec::RetireRule::MARKOUT;
      l.retire_trades = 4;
      l.retire_h = "300s";
      l.retire_loss = 20000000;
    }
    if (l.id == "X1") {
      l.retire_rule = LaneSpec::RetireRule::CAP;
      l.retire_trades = 3;
      l.retire_loss = 20000000;
    }
  }
  return lanes;
}
SysBook::MarkFn mark_at(Px px) {
  return [px](const std::string&) { return std::optional<Px>(px); };
}

// A lane's life: its trades by cid, its real and fees through fills and settlements, not rolled at
// ET midnight; its net markouts at the lane's horizon, each fill once; the ids it keeps are bounded.
void life_counts() {
  SysBook b(ruled_lanes());
  const std::int64_t day = 1791000000000, t = day + 3600000;
  b.roll(day);
  CHECK(!b.life("C1"));
  // one IOC filled in two parts is one trade; the REST copy of a part counts nothing
  CHECK(b.fill("C1", "SC1-1", "TK-A", Side::BUY, 300, 4000, 2100, t, "tr-1"));
  CHECK(b.fill("C1", "SC1-1", "TK-A", Side::BUY, 200, 4100, 1400, t + 1, "tr-2"));
  CHECK(!b.fill("C1", "SC1-1", "TK-A", Side::BUY, 200, 4100, 1400, t + 1, "tr-2"));
  CHECK(b.life("C1")->trades == 1 && b.life("C1")->first_ms == t && b.life("C1")->fees == 3500);
  // a second order sells the 5 at 0.50: $2.50 against their $2.02
  CHECK(b.fill("C1", "SC1-2", "TK-A", Side::SELL, 500, 5000, 1750, t + 2, "tr-3"));
  CHECK(b.life("C1")->trades == 2 && b.life("C1")->real == 480000 && b.day("C1")->real == 480000);
  // a settlement realizes into it too: 1 at 0.30 settles YES
  CHECK(b.fill("C1", "SC1-3", "TK-B", Side::BUY, 100, 3000, 700, t + 3, "tr-4"));
  CHECK(b.settle("C1", "TK-B", kOneDollar, t + 4) == 700000);
  CHECK(b.life("C1")->trades == 3 && b.life("C1")->real == 1180000 && b.life("C1")->fees == 5950);
  // ET midnight: the day starts again, the life goes on; an order counted yesterday is no new trade
  b.roll(day + 86400000);
  CHECK(b.day("C1")->real == 0 && b.day("C1")->fees == 0 && b.life("C1")->real == 1180000 && b.life("C1")->fees == 5950);
  CHECK(b.fill("C1", "SC1-2", "TK-A", Side::SELL, 100, 5000, 350, day + 86400000 + 1, "tr-5"));
  CHECK(b.life("C1")->trades == 3 && b.day("C1")->hit.size() == 1 && b.life("C1")->fees == 6300);
  CHECK(b.fill("C1", "SC1-4", "TK-A", Side::BUY, 100, 4000, 0, day + 86400000 + 2, "tr-6"));
  CHECK(b.life("C1")->trades == 4 && b.life("C1")->first_ms == t);
  // markouts at C1's horizon, each fill once, weighted by its contracts
  CHECK(b.markout("C1", "tr-1", "300s", -50, 300));
  CHECK(!b.markout("C1", "tr-1", "300s", -50, 300));   // again
  CHECK(!b.markout("C1", "tr-2", "60s", -50, 200));    // another horizon
  CHECK(!b.markout("M1", "tr-7", "300s", -50, 200));   // a lane with no markout rule
  CHECK(!b.markout("Z9", "tr-7", "300s", -50, 200));   // no such lane
  CHECK(b.markout("C1", "tr-2", "300s", 100, 200));
  CHECK(b.life("C1")->mo_n == 2 && b.life("C1")->mo_ctrs == 500 && b.life("C1")->mo_sum == -50 * 300 + 100 * 200);
  // it keeps the newest kLifeIds ids: one older than those would count again
  SysBook c(ruled_lanes());
  std::size_t n = 0;
  for (std::size_t i = 0; i <= SysBook::kLifeIds; ++i) n += c.markout("C1", "f" + std::to_string(i), "300s", 0, 100);
  const auto& ids = c.life("C1")->mo_fids;
  CHECK(n == SysBook::kLifeIds + 1 && ids.order.size() == SysBook::kLifeIds && ids.in.size() == SysBook::kLifeIds);
  CHECK(ids.order.front() == "f1" && !c.markout("C1", "f1", "300s", 0, 100) && c.markout("C1", "f0", "300s", 0, 100));
}

// The retire rules: CAP at its count; MARKOUT at its count once kRetireMarked fills are marked and
// their mean net markout is <= 0; the loss rule at any count, with open positions at the mark. A
// retired lane is off, OFF in its row, and stays retired for its first reason.
void retire_rules() {
  const std::int64_t day = 1791000000000, t = day + 3600000;
  const SysBook::MarkFn none = [](const std::string&) { return std::optional<Px>(); };
  // X1, CAP 3
  SysBook b(ruled_lanes());
  b.roll(day);
  CHECK(b.retire_due(*b.lane("X1"), none).empty());
  CHECK(b.fill("X1", "SX1-1", "TK-X", Side::BUY, 100, 5000, 0, t, "x1"));
  CHECK(b.fill("X1", "SX1-2", "TK-X", Side::BUY, 100, 5000, 0, t + 1, "x2"));
  CHECK(b.retire_due(*b.lane("X1"), mark_at(5000)).empty());
  CHECK(b.fill("X1", "SX1-3", "TK-X", Side::BUY, 100, 5000, 0, t + 2, "x3"));
  const std::string why = b.retire_due(*b.lane("X1"), mark_at(5000));
  CHECK(why == "its cap of 3 trades");
  CHECK(b.set_on("X1", true) && b.retire("X1", t + 10, why) && !b.on("X1"));
  CHECK(b.life("X1")->retired && b.life("X1")->retired_ms == t + 10 && b.life("X1")->retired_why == why);
  CHECK(!b.retire("X1", t + 11, "again") && b.life("X1")->retired_why == why);   // the first reason stays
  CHECK(b.retire_due(*b.lane("X1"), none).empty() && !b.retire("Z9", t, "no such lane"));
  const auto row = b.lane_json(*b.lane("X1"), RunState::RUN, 0, 0, 100000000, mark_at(5000), "");
  CHECK(row["state"] == "OFF" && row["on"] == false && row["why"] == "retired: its cap of 3 trades" && row["venue"] == "K");
  // C1, MARKOUT at 4 trades: so many trades, and so many fills marked at mo a contract
  const auto c1 = [&](int trades, int marked, Money mo, Qty qty) {
    SysBook s(ruled_lanes());
    s.roll(day);
    for (int i = 0; i < trades; ++i) s.fill("C1", "SC1-" + std::to_string(i), "TK-C", Side::BUY, 100, 5000, 0, t + i, "c" + std::to_string(i));
    for (int i = 0; i < marked; ++i) s.markout("C1", "m" + std::to_string(i), "300s", mo, qty);
    return s.retire_due(*s.lane("C1"), mark_at(5000));
  };
  CHECK(c1(3, 50, -10, 100).empty());   // a trade short of its count
  CHECK(c1(4, 49, -10, 100).empty());   // a fill short of 50 marked
  CHECK(c1(4, 50, -10, 100) == "4 trades, mean net 300s markout -0.0010 over 50 marked");
  CHECK(c1(4, 50, 0, 100) == "4 trades, mean net 300s markout 0.0000 over 50 marked");   // not positive
  CHECK(c1(4, 50, 1, 100).empty());     // positive: it trades on
  CHECK(!c1(9, 60, -10, 100).empty());  // past its count
  // the mean is per contract: 49 fills of 1 contract at -5c and one of 100 at +3c is positive
  SysBook w(ruled_lanes());
  w.roll(day);
  for (int i = 0; i < 4; ++i) w.fill("C1", "SC1-" + std::to_string(i), "TK-C", Side::BUY, 100, 5000, 0, t + i, "c" + std::to_string(i));
  for (int i = 0; i < 49; ++i) w.markout("C1", "m" + std::to_string(i), "300s", -500, 100);
  CHECK(w.markout("C1", "m49", "300s", 300, 10000) && w.retire_due(*w.lane("C1"), mark_at(5000)).empty());
  // the loss rule, at any count: X1 bought 100 at 0.30, down $19.99 at 0.1001 and $20 at 0.10
  SysBook l(ruled_lanes());
  l.roll(day);
  CHECK(l.fill("X1", "SX1-1", "TK-L", Side::BUY, 10000, 3000, 0, t, "l1"));
  CHECK(l.retire_due(*l.lane("X1"), mark_at(1001)).empty());
  CHECK(l.retire_due(*l.lane("X1"), mark_at(1000)) == "down $20.0000 over its life, its limit $20.0000");
  CHECK(!l.retire_due(*l.lane("X1"), none).empty());   // never priced: worthless
  // over its life, across ET midnight, with its fees: C1 lost $15 and paid $0.50 yesterday, and
  // holds 10 at 0.50 today: down $20 at 0.05, though today alone is down $4.50
  SysBook m(ruled_lanes());
  m.roll(day);
  CHECK(m.fill("C1", "SC1-1", "TK-D", Side::BUY, 2500, 6000, 500000, t, "d1"));
  CHECK(m.settle("C1", "TK-D", 0, t + 1) == -15000000);
  m.roll(day + 86400000);
  CHECK(m.fill("C1", "SC1-2", "TK-E", Side::BUY, 1000, 5000, 0, day + 86400000 + 1, "e1"));
  CHECK(m.retire_due(*m.lane("C1"), mark_at(600)).empty());
  CHECK(m.retire_due(*m.lane("C1"), mark_at(500)) == "down $20.0000 over its life, its limit $20.0000");
  CHECK(m.day_total(mark_at(500)) == -4500000);
  // lanes without a rule never retire, and every new field's default is a no-op
  const LaneSpec blank;
  CHECK(blank.venue == "K" && blank.family.empty() && blank.event_cap == 0 && blank.gate_why.empty());
  CHECK(blank.retire_rule == LaneSpec::RetireRule::NONE && blank.retire_trades == 0 && blank.retire_loss == 0 && blank.retire_h.empty());
  auto lanes = default_lanes();
  lanes.push_back(paper_lane());
  SysBook d(lanes);
  d.roll(day);
  CHECK(d.on("D1"));
  int unruled = 0;
  for (const auto& x : d.lanes()) {
    if (x.retire_rule != LaneSpec::RetireRule::NONE || x.retire_loss) continue;
    ++unruled;
    CHECK(x.retire_trades == 0 && x.retire_h.empty());
    for (int i = 0; i < 200; ++i)
      d.fill(x.id, "S" + x.id + "-" + std::to_string(i), "TK-" + x.id, Side::BUY, 10000, 9900, 0, t + i, x.id + std::to_string(i));
    CHECK(d.life(x.id)->trades == 200 && d.retire_due(x, none).empty() && !d.markout(x.id, x.id + "0", "", 0, 100));
  }
  CHECK(unruled == 7);   // M1 M2 P1 S1 Q1 W1 D1
}

// A life is saved and read back on any day: its counts, the ids it counted, its markouts and a
// retirement (the lane stays off, though it is on at start); a book saved before lives were kept
// restores with none; a malformed life changes nothing; reset_life (deskctl lane-reset) starts it
// afresh.
void life_across_restart() {
  auto lanes = ruled_lanes();
  lanes.push_back(paper_lane());
  lanes.back().retire_rule = LaneSpec::RetireRule::CAP;
  lanes.back().retire_trades = 2;
  const std::int64_t day = 1791000000000, t = day + 3600000;
  SysBook b(lanes);
  b.roll(day);
  CHECK(b.on("D1"));
  CHECK(b.fill("C1", "SC1-1", "TK-A", Side::BUY, 300, 4000, 2100, t, "tr-1"));
  CHECK(b.fill("C1", "SC1-2", "TK-A", Side::SELL, 100, 5000, 700, t + 1, "tr-2"));
  CHECK(b.markout("C1", "tr-1", "300s", -40, 300));
  CHECK(b.fill("D1", "SD1-1", "TK-B", Side::BUY, 100, 2000, 0, t + 2, "tr-3"));
  CHECK(b.fill("D1", "SD1-2", "TK-B", Side::BUY, 100, 2000, 0, t + 2, "tr-4"));
  const std::string why = b.retire_due(*b.lane("D1"), {});
  CHECK(why == "its cap of 2 trades" && b.retire("D1", t + 3, why) && !b.on("D1"));
  const json saved = b.to_json(SysBook::Halt{}, t + 4);
  CHECK(saved["v"] == 1 && saved["life"]["C1"]["trades"] == 2 && saved["life"]["D1"]["retired"] == true);
  // the next day, in a new process
  SysBook n(lanes);
  SysBook::Halt h;
  std::string err;
  CHECK(n.restore(saved, day + 86400000, &h, &err));
  const auto* f = n.life("C1");
  CHECK(f && f->trades == 2 && f->real == 100000 && f->fees == 2800 && f->first_ms == t);
  CHECK(f->mo_n == 1 && f->mo_ctrs == 300 && f->mo_sum == -40 * 300);
  CHECK(n.day("C1")->real == 0 && n.day("C1")->fees == 0 && n.position("C1", "TK-A") == 200);
  // what it counted still counts once
  CHECK(n.fill("C1", "SC1-2", "TK-A", Side::SELL, 100, 5000, 700, day + 86400000 + 1, "tr-5") && n.life("C1")->trades == 2);
  CHECK(!n.markout("C1", "tr-1", "300s", -40, 300) && n.markout("C1", "tr-5", "300s", 10, 100));
  // D1 is still retired, and off though it is on at start
  CHECK(!n.on("D1") && n.life("D1")->retired && n.life("D1")->retired_ms == t + 3 && n.life("D1")->retired_why == why);
  CHECK(n.retire_due(*n.lane("D1"), {}).empty());
  CHECK(n.lane_json(*n.lane("D1"), RunState::RUN, 0, 0, 100000000, {}, "")["why"] == "retired: its cap of 2 trades");
  // deskctl lane-reset: a fresh life, and the lane may be turned on again
  CHECK(n.reset_life("D1") && !n.life("D1") && !n.reset_life("Z9"));
  CHECK(n.lane_json(*n.lane("D1"), RunState::RUN, 0, 0, 100000000, {}, "")["why"] == "");
  CHECK(n.set_on("D1", true) && n.retire_due(*n.lane("D1"), {}).empty());
  // a book saved before lives were kept (v1 all the same): no life, D1 on at start
  json old = saved;
  old.erase("life");
  SysBook o(lanes);
  CHECK(o.restore(old, day, &h, &err) && !o.life("C1") && !o.life("D1") && o.on("D1"));
  CHECK(o.position("C1", "TK-A") == 200 && o.day("C1")->fills == 2);
  // a lane this build no longer has is dropped, as its day is
  json gone = saved;
  gone["life"]["Z9"] = saved["life"]["C1"];
  SysBook g(lanes);
  CHECK(g.restore(gone, day, &h, &err) && !g.life("Z9") && g.life("C1")->trades == 2);
  // a malformed life changes nothing
  SysBook m(lanes);
  for (const auto& bad : {json(3), json{{"C1", 3}}}) {
    json j = saved;
    j["life"] = bad;
    CHECK(!m.restore(j, day, &h, &err) && !err.empty());
  }
  json j = saved;
  j["life"]["C1"]["hit"] = 5;
  CHECK(!m.restore(j, day, &h, &err));
  j = saved;
  j["life"]["D1"].erase("retired_why");
  CHECK(!m.restore(j, day, &h, &err));
  CHECK(m.held().empty() && !m.life("C1") && !m.life("D1") && m.on("D1"));
}

// A gated lane: OFF with its gate's why, and not on at start; the row carries the lane's venue.
void gated_and_venue() {
  auto lanes = default_lanes();
  lanes.push_back(paper_lane());
  for (auto& l : lanes) {
    if (l.id == "C1") l.gate_why = "lane C1 is gated: no gates/C1.json";
    if (l.id == "D1") l.gate_why = "lane D1 is gated: a test";
    if (l.id == "S1") l.venue = "KP";
  }
  SysBook b(lanes);
  CHECK(!b.on("D1") && SysBook({paper_lane()}).on("D1"));
  const auto row = b.lane_json(*b.lane("C1"), RunState::RUN, 0, 0, 100000000, {}, "");
  CHECK(row["state"] == "OFF" && row["why"] == "lane C1 is gated: no gates/C1.json" && row["venue"] == "K");
  CHECK(b.lane_json(*b.lane("S1"), RunState::RUN, 0, 0, 100000000, {}, "")["venue"] == "KP");
  CHECK(b.lane_json(*b.lane("M1"), RunState::RUN, 0, 0, 100000000, {}, "")["why"] == "");
}
}  // namespace

// Correlated groups (plan section 4, limit 2) and a lane's worst case within one
void groups_of_markets() {
  CHECK(corr_group("KXGOLD15M-26OCT101200-00", "KXGOLD15M-26OCT101200") == "METALS");
  CHECK(corr_group("KXPLATINUM15M-26OCT101200-00", "") == "METALS");
  CHECK(corr_group("KXEURUSD15M-26OCT101200-00", "x") == "FX");
  CHECK(corr_group("KXNATGAS15M-26OCT101200-00", "x") == "ENERGY");
  CHECK(corr_group("KXHIGHNY-26OCT12-B70", "KXHIGHNY-26OCT12") == "KXHIGHNY-26OCT12");   // one city-day
  CHECK(corr_group("KXHIGHNY-26OCT12-B70", "") == "KXHIGHNY-26OCT12");                   // by the ticker
  CHECK(corr_group_cap("METALS") == 25000000 && corr_group_cap("KXHIGHNY-26OCT12") == 15000000);
  SysBook b;
  CHECK(b.fill("M2", "SM2-a", "KXHIGHNY-26OCT12-B70", Side::BUY, 500, 9700, 0, 1000, "t1"));   // 5 at 0.97: $4.85
  CHECK(b.fill("M2", "SM2-b", "KXHIGHCHI-26OCT12-B60", Side::SELL, 500, 300, 0, 1000, "t2"));  // short 5 at 0.03: $4.85
  auto city = [](const std::string& city) { return [city](const std::string& tk) { return tk.rfind(city, 0) == 0; }; };
  CHECK(b.open_worst_in("M2", city("KXHIGHNY")) == 4850000);
  CHECK(b.open_worst_in("M2", city("KXHIGHCHI")) == 4850000);
  CHECK(b.open_worst("M2") == 9700000 && b.open_worst_in("M1", city("KXHIGHNY")) == 0);
}

int main() {
  lanes_and_groups(); default_rows(); lane_day(); settle_and_restore(); rebates(); groups_of_markets();
  life_counts(); retire_rules(); life_across_restart(); gated_and_venue();
  return finish("deskd-sys_book");
}
