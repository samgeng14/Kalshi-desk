// Systematic's book (gw/sys_book.h): lanes, groups and a lane's day by average cost.
#include "check.h"
#include "deskd/gw/sys_book.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
void lanes_and_groups() {
  SysBook b;
  CHECK(b.lanes().size() == 5 && b.lane("M1")->name == "LIP-15" && b.lane("M1")->budget == 550000);
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
  // a malformed one changes nothing
  SysBook m;
  CHECK(!m.restore(json{{"v", 1}, {"lanes", 3}}, day, &back, &why) && !why.empty() && m.held().empty());
  CHECK(!m.restore(json{{"v", 2}}, day, &back, &why));
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
  lanes_and_groups(); lane_day(); settle_and_restore(); groups_of_markets();
  return finish("deskd-sys_book");
}
