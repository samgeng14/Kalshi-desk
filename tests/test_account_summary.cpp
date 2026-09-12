#include <cmath>
#include <cstdio>

#include "desk/account_summary.h"
#include "desk/log.h"
#include "desk/timeutil.h"

using namespace desk;
static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
#define NEAR(a,b) CHECK(std::fabs((a).get<double>()-(b)) < 1e-8)

static json fixture() {
  return {{"events", {{"cash", 80.0}, {"equity", 90.0}, {"ts", 2000},
                      {"baseline", {{"ts", 1000}, {"equity", 180.0}}},
                      {"pnl_total", 10.0}, {"pnl_recorded", 10.0}, {"pnl_total_scope", "lifetime"}}},
          {"perps", {{"cash", 95.0}, {"equity", 110.0}, {"available", 40.0}, {"ts", 2000},
                     {"baseline", {{"ts", 1000}, {"equity", 0.0}}},
                     {"pnl_total", 10.0}, {"pnl_recorded", 10.0}, {"pnl_total_scope", "lifetime"}}},
          {"cashflows_complete", true},
          {"cashflows", {{{"ts", 1500}, {"kind", "to_margined"}, {"amount", -100.0}}}}};
}

static void transfer_neutrality() {
  auto input = fixture();
  auto out = calculate_account_summary(input, 2020, 1000);
  NEAR(out["combined"]["cash"], 175.0); // not event cash + perps available margin
  NEAR(out["combined"]["equity"], 200.0); // position value already in both equities
  NEAR(out["events"]["pnl_today"], 10.0);
  NEAR(out["perps"]["pnl_today"], 10.0);
  NEAR(out["combined"]["pnl_today"], 20.0);
  NEAR(out["combined"]["pnl_total"], 20.0);
  CHECK(out["complete"] == true);
  CHECK(out["as_of"] == 2000);
  // Deposits only affect the event account; they are not a perps loss or profit.
  input["cashflows"].push_back({{"ts", 1700}, {"kind", "deposit"}, {"amount", 50.0}});
  input["events"]["cash"] = 130.0;
  input["events"]["equity"] = 140.0;
  out = calculate_account_summary(input, 2020, 1000);
  NEAR(out["combined"]["pnl_today"], 20.0);
  // The opposite transfer direction has the opposite event ledger sign.
  input["cashflows"].push_back({{"ts", 1800}, {"kind", "from_margined"}, {"amount", 25.0}});
  input["events"]["equity"] = 165.0;
  input["perps"]["equity"] = 85.0;
  out = calculate_account_summary(input, 2020, 1000);
  NEAR(out["combined"]["pnl_today"], 20.0);
  NEAR(out["events"]["pnl_today"], 10.0);
  NEAR(out["perps"]["pnl_today"], 10.0);
}

static void time_and_unknowns() {
  auto input = fixture();
  // A flow already reflected in the baseline or after the current snapshot is excluded.
  input["cashflows"].push_back({{"ts", 1000}, {"kind", "deposit"}, {"amount", 500.0}});
  input["cashflows"].push_back({{"ts", 2010}, {"kind", "deposit"}, {"amount", 500.0}});
  auto out = calculate_account_summary(input, 2020, 1000);
  NEAR(out["events"]["pnl_today"], 10.0);
  input["perps"]["pnl_total"] = nullptr;
  out = calculate_account_summary(input, 2020, 1000);
  CHECK(out["combined"]["pnl_total"].is_null());
  NEAR(out["combined"]["pnl_recorded"], 20.0);
  CHECK(out["complete"] == false);
  input["perps"]["ts"] = 0;
  out = calculate_account_summary(input, 2020, 1000);
  CHECK(out["combined"]["cash"].is_null());
  CHECK(out["combined"]["equity"].is_null());
  CHECK(out["combined"]["pnl_today"].is_null());
  CHECK(out["as_of"].is_null());
  input = fixture();
  input["events"]["baseline"]["ts"] = 999;
  out = calculate_account_summary(input, 2020, 1000);
  CHECK(out["events"]["pnl_today"].is_null()); // yesterday is not today's zero
  input = fixture();
  out = calculate_account_summary(input, 2200, 1000);
  CHECK(out["events"]["stale"] == true);
  CHECK(out["complete"] == false);
  input["perps"]["ts"] = 2100;
  out = calculate_account_summary(input, 2200, 1000);
  CHECK(out["as_of"] == 2000);
  CHECK(out["warnings"].size() >= 2);
  input["events"]["ts"] = 3000;
  out = calculate_account_summary(input, 2200, 1000);
  CHECK(out["events"]["cash"].is_null()); // future snapshots are invalid
}

static void database_view_is_read_only() {
  Store store(":memory:");
  Config cfg;
  cfg.risk.day_tz = "UTC";
  const long long now = now_s();
  store.insert_account(now, 80.0, 5.0);
  store.exec("CREATE TABLE perp_account(ts INTEGER PRIMARY KEY, settled_funds REAL, equity REAL, available REAL)");
  store.exec("CREATE TABLE perp_fills(ts INTEGER, realized_pnl REAL, fees REAL)");
  store.exec("CREATE TABLE perp_positions(unrealized_pnl REAL)");
  store.exec("INSERT INTO perp_account VALUES(?,?,?,?)", {now, 99.0, 102.0, 30.0});
  store.exec("INSERT INTO perp_fills VALUES(?,?,?)", {now, 5.0, 1.0});
  store.exec("INSERT INTO perp_positions VALUES(?)", {3.0});
  auto before = store.query("SELECT * FROM kv");
  auto out = account_summary(store, cfg, json::object());
  NEAR(out["combined"]["cash"], 179.0);
  NEAR(out["combined"]["equity"], 187.0);
  NEAR(out["combined"]["pnl_today"], 0.0);
  NEAR(out["perps"]["pnl_recorded"], 7.0); // realized 5 - fees 1 + unrealized 3
  CHECK(out["perps"]["pnl_total"].is_null());
  CHECK(out["combined"]["pnl_total"].is_null());
  CHECK(store.query("SELECT * FROM kv") == before); // a GET never creates daily baselines
  // Both full view and compact summary use settled funds for cash.
  out = account_summary(store, cfg, {{"account_ts", now}, {"balance", {{"settled_funds", 99.0},
    {"equity", 102.0}, {"available", 30.0}}}, {"total_unrealized", 3.0}});
  NEAR(out["perps"]["cash"], 99.0);
  NEAR(out["perps"]["pnl_recorded"], 7.0);
  out = account_summary(store, cfg, {{"account_ts", now}, {"settled_funds", 99.0},
    {"equity", 102.0}, {"available", 30.0}, {"unrealized", 3.0}});
  NEAR(out["perps"]["cash"], 99.0);
}

int main() {
  transfer_neutrality();
  time_and_unknowns();
  database_view_is_read_only();
  std::printf("account summary: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
