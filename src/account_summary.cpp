#include "desk/account_summary.h"

#include <algorithm>
#include <cmath>

#include "desk/eval.h"
#include "desk/log.h"
#include "desk/timeutil.h"

namespace desk {
namespace {

json number(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return nullptr;
  double value = j[key].get<double>();
  return std::isfinite(value) ? json(value) : json(nullptr);
}

json sum(const json& a, const json& b) {
  return a.is_number() && b.is_number() ? json(a.get<double>() + b.get<double>()) : json(nullptr);
}

bool table_exists(Store& store, const std::string& table) {
  return !store.query("SELECT name FROM sqlite_master WHERE type='table' AND name=?", {table}).empty();
}

json baseline(Store& store, bool perps, long long day_start, long long to) {
  const std::string table = perps ? "perp_account" : "account";
  const std::string equity = perps ? "equity" : "balance+portfolio_value";
  if (!table_exists(store, table) || to < day_start) return nullptr;
  auto rows = store.query("SELECT ts," + equity + " AS equity FROM " + table +
                         " WHERE ts>=? AND ts<=? ORDER BY ts LIMIT 1", {day_start, to});
  return rows.empty() ? json(nullptr) : rows[0];
}

}  // namespace

json calculate_account_summary(const json& inputs, long long now, long long day_start) {
  json result{{"events", json::object()}, {"perps", json::object()}, {"combined", json::object()},
              {"as_of", nullptr}, {"generated_ts", now}, {"complete", true}, {"warnings", json::array()},
              {"pnl_today_scope", "since_first_sync_of_local_day"}};
  const json flows = inputs.value("cashflows", json::array());
  long long oldest = now;
  bool have_both = true;
  for (const auto* account : {"events", "perps"}) {
    const bool perps = std::string(account) == "perps";
    json in = inputs.value(account, json::object());
    json& out = result[account];
    long long ts = in.value("ts", 0LL);
    bool synced = ts > 0 && ts <= now + 5;
    out = {{"cash", synced ? number(in, "cash") : json(nullptr)},
           {"equity", synced ? number(in, "equity") : json(nullptr)},
           {"pnl_today", nullptr}, {"pnl_total", synced ? number(in, "pnl_total") : json(nullptr)},
           {"pnl_recorded", synced ? number(in, "pnl_recorded") : json(nullptr)},
           {"pnl_total_scope", in.value("pnl_total_scope", "unknown")},
           {"ts", synced ? json(ts) : json(nullptr)}, {"stale", !synced || now - ts > 180},
           {"baseline_ts", nullptr}, {"external_flows", nullptr}};
    if (in.contains("unrealized")) out["unrealized"] = synced ? number(in, "unrealized") : json(nullptr);
    if (in.contains("available")) out["available"] = synced ? number(in, "available") : json(nullptr);
    if (!synced || out["cash"].is_null() || out["equity"].is_null()) {
      have_both = false;
      result["warnings"].push_back(std::string(account) + " account has no valid balance snapshot");
    } else {
      oldest = std::min(oldest, ts);
    }
    if (out["stale"].get<bool>()) {
      result["complete"] = false;
      if (synced) result["warnings"].push_back(std::string(account) + " balance snapshot is over 180 seconds old");
    }
    json start = in.value("baseline", json::object());
    long long start_ts = start.is_object() ? start.value("ts", 0LL) : 0;
    auto start_equity = number(start, "equity");
    if (synced && !out["equity"].is_null() && start_ts >= day_start && start_ts <= ts && start_equity.is_number()) {
      double moved = 0;
      for (const auto& f : flows) {
        long long flow_ts = f.value("ts", 0LL);
        if (flow_ts <= start_ts || flow_ts > ts) continue;
        auto amount = number(f, "amount");
        if (!amount.is_number()) continue;
        const std::string kind = f.value("kind", "");
        const bool internal = kind == "to_margined" || kind == "from_margined";
        if (!perps || internal) moved += amount.get<double>() * (perps ? -1 : 1);
      }
      out["baseline_ts"] = start_ts;
      out["external_flows"] = moved;
      out["pnl_today"] = out["equity"].get<double>() - start_equity.get<double>() - moved;
    } else {
      result["complete"] = false;
      result["warnings"].push_back(std::string(account) + " P&L today needs a baseline from this local day");
    }
    if (out["pnl_total"].is_null()) {
      result["complete"] = false;
      result["warnings"].push_back(std::string(account) + " lifetime P&L is unavailable; recorded history is shown separately");
    }
    if (out["pnl_total_scope"] == "recorded_history") result["complete"] = false;
  }
  for (const auto* key : {"cash", "equity", "pnl_today", "pnl_total", "pnl_recorded"})
    result["combined"][key] = sum(result["events"][key], result["perps"][key]);
  result["combined"]["pnl_total_scope"] = result["events"]["pnl_total_scope"] == "lifetime" &&
      result["perps"]["pnl_total_scope"] == "lifetime" ? "lifetime" : "incomplete_history";
  if (have_both) {
    result["as_of"] = oldest;
    long long skew = std::llabs(result["events"]["ts"].get<long long>() - result["perps"]["ts"].get<long long>());
    if (skew > 60) {
      result["complete"] = false;
      result["warnings"].push_back("Events and perps snapshots differ by more than 60 seconds; a transfer may be visible on only one side");
    }
  } else result["complete"] = false;
  if (!inputs.value("cashflows_complete", false)) {
    result["complete"] = false;
    result["warnings"].push_back("Daily P&L adjusts recorded transfers and deposits; cashflow history completeness has not been verified");
  }
  return result;
}

json account_summary(Store& store, const Config& cfg, const json& perps_view) {
  const long long now = now_s();
  const std::string day = trading_day(cfg.risk.day_tz, now);
  // Resolve the offset at local midnight, including DST transition days.
  const long long midnight_utc = parse_iso(day + "T00:00:00Z");
  long long day_start = midnight_utc - tz_offset_seconds(cfg.risk.day_tz, midnight_utc);
  day_start = midnight_utc - tz_offset_seconds(cfg.risk.day_tz, day_start);
  json inputs{{"events", json::object()}, {"perps", json::object()},
              {"cashflows", store.query("SELECT ts,kind,amount FROM cashflows WHERE ts>=? AND ts<=?", {day_start, now})}};
  if (auto a = store.last_account()) {
    const json pnl = build_pnl(store, cfg.risk.day_tz);
    inputs["events"] = {{"ts", a->updated_ts}, {"cash", a->balance}, {"equity", a->balance + a->portfolio_value},
      {"baseline", baseline(store, false, day_start, a->updated_ts)},
      {"pnl_total", number(pnl, "total")}, {"pnl_recorded", number(pnl, "total")},
      {"pnl_total_scope", "recorded_history"}, {"unrealized", number(pnl, "unrealized")}};
  }
  json p = perps_view;
  long long perps_ts = p.is_object() ? p.value("account_ts", 0LL) : 0;
  json balance = p.is_object() && p.contains("balance") ? p["balance"] : p;
  // A disabled or newly initialized PerpsDesk can still have a persisted balance.
  if (perps_ts <= 0 && table_exists(store, "perp_account")) {
    auto rows = store.query("SELECT * FROM perp_account ORDER BY ts DESC LIMIT 1");
    if (!rows.empty()) {
      balance = rows[0];
      perps_ts = rows[0].value("ts", 0LL);
      p = json::object(); // use persisted positions with a persisted balance
    }
  }
  if (perps_ts > 0) {
    json unreal = p.is_object() && p.contains("total_unrealized") ? number(p, "total_unrealized") : number(p, "unrealized");
    if (unreal.is_null() && table_exists(store, "perp_positions")) {
      auto rows = store.query("SELECT COALESCE(SUM(unrealized_pnl),0) AS unrealized FROM perp_positions");
      unreal = number(rows[0], "unrealized");
    }
    json recorded = nullptr;
    if (table_exists(store, "perp_fills")) {
      auto rows = store.query("SELECT COALESCE(SUM(realized_pnl-fees),0) AS net FROM perp_fills WHERE ts<=?", {perps_ts});
      recorded = sum(number(rows[0], "net"), unreal);
    }
    // The importer currently reads one fills page and has no funding-payment
    // ledger. Its sum is useful, but it cannot establish lifetime performance.
    inputs["perps"] = {{"ts", perps_ts}, {"cash", number(balance, "settled_funds")},
      {"equity", number(balance, "equity")}, {"available", number(balance, "available")},
      {"baseline", baseline(store, true, day_start, perps_ts)}, {"unrealized", unreal},
      {"pnl_total", nullptr}, {"pnl_recorded", recorded}, {"pnl_total_scope", "unknown"}};
  }
  json out = calculate_account_summary(inputs, now, day_start);
  out["day"] = day;
  out["day_tz"] = cfg.risk.day_tz;
  out["perps"]["pnl_recorded_scope"] = "recorded_fills_net_fees_plus_current_unrealized_excludes_funding";
  out["warnings"].push_back("Perps recorded P&L excludes funding payments and may omit older fills; it is not lifetime P&L");
  return out;
}

}  // namespace desk
