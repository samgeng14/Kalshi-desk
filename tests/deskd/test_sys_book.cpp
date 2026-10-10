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
  CHECK(b.unrealized("M2", [](const std::string&) { return std::optional<Px>(); }) == 0);   // no mark, no claim
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
}  // namespace

int main() {
  lanes_and_groups(); lane_day();
  return finish("deskd-sys_book");
}
