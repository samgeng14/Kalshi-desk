#include "desk/eval.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

#include "desk/log.h"
#include "desk/timeutil.h"

namespace desk {

BootCI bootstrap_mean(const std::vector<double>& x, int reps, double alpha, unsigned seed) {
  BootCI r;
  r.n = static_cast<int>(x.size());
  if (x.empty()) return r;
  double s = 0;
  for (double v : x) s += v;
  r.mean = s / x.size();
  if (x.size() < 2) {
    r.lo = r.hi = r.mean;
    return r;
  }
  std::mt19937 rng(seed);
  std::uniform_int_distribution<size_t> pick(0, x.size() - 1);
  std::vector<double> means;
  means.reserve(reps);
  for (int i = 0; i < reps; ++i) {
    double t = 0;
    for (size_t k = 0; k < x.size(); ++k) t += x[pick(rng)];
    means.push_back(t / x.size());
  }
  std::sort(means.begin(), means.end());
  auto q = [&](double p) { return means[std::min(means.size() - 1, static_cast<size_t>(p * means.size()))]; };
  r.lo = q(alpha / 2);
  r.hi = q(1 - alpha / 2);
  return r;
}

static double gnum(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null() || !it->is_number()) return 0;
  return it->get<double>();
}
static std::string gstr(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null() || !it->is_string()) return "";
  return it->get<std::string>();
}

json build_report(Store& store, const Config& cfg) {
  json rep;
  long long now = now_s();

  // ---- 1. settled trades: one row per settled market we held ------------------------
  json settled = store.query(
      "SELECT s.ticker, s.event_ticker, s.market_result, s.yes_count, s.no_count, s.yes_cost, s.no_cost,"
      " s.revenue, s.fee, s.settled_time,"
      " (SELECT COALESCE(SUM(f.fee),0) FROM fills f WHERE f.ticker=s.ticker) AS trade_fees,"
      " (SELECT COUNT(*) FROM fills f WHERE f.ticker=s.ticker) AS n_fills,"
      " (SELECT p.p_model FROM proposals p JOIN orders o ON o.proposal_id=p.id WHERE o.ticker=s.ticker"
      "   AND p.p_model>0 ORDER BY p.id LIMIT 1) AS p_model,"
      " (SELECT p.outcome_side FROM proposals p JOIN orders o ON o.proposal_id=p.id WHERE o.ticker=s.ticker"
      "   ORDER BY p.id LIMIT 1) AS side,"
      " (SELECT p.strategy FROM proposals p JOIN orders o ON o.proposal_id=p.id WHERE o.ticker=s.ticker"
      "   ORDER BY p.id LIMIT 1) AS strategy"
      " FROM settlements s WHERE (s.yes_count>0 OR s.no_count>0) ORDER BY s.settled_time");
  std::vector<double> ret;  // net return per $ staked, per settled market
  std::set<std::string> events;
  double total_pnl = 0, total_staked = 0;
  std::map<std::string, std::vector<double>> by_strategy;
  json trades = json::array();
  std::map<std::string, std::pair<double, double>> ledger;
  for (const auto& r : store.query("SELECT ticker, realized_pnl, fees_paid FROM realized", {}))
    ledger[gstr(r, "ticker")] = {gnum(r, "realized_pnl"), gnum(r, "fees_paid")};
  for (const auto& r : settled) {
    double staked = gnum(r, "yes_cost") + gnum(r, "no_cost");
    if (staked <= 0) continue;
    std::string tk = gstr(r, "ticker");
    // Kalshi's realized number when we have it (settlement records show revenue 0 for markets
    // held on both sides); the settlement arithmetic otherwise.
    double pnl = ledger.count(tk) ? ledger[tk].first - ledger[tk].second
                                  : gnum(r, "revenue") - staked - gnum(r, "fee") - gnum(r, "trade_fees");
    double rr = pnl / staked;
    ret.push_back(rr);
    total_pnl += pnl;
    total_staked += staked;
    events.insert(gstr(r, "event_ticker"));
    std::string strat = gstr(r, "strategy");
    if (strat.empty()) strat = "external";
    by_strategy[strat].push_back(rr);
    trades.push_back({{"ticker", gstr(r, "ticker")},
                      {"result", gstr(r, "market_result")},
                      {"staked", staked},
                      {"pnl", pnl},
                      {"ret", rr},
                      {"p_model", gnum(r, "p_model")},
                      {"side", gstr(r, "side")},
                      {"strategy", strat},
                      {"settled_time", gstr(r, "settled_time")}});
  }
  BootCI edge = bootstrap_mean(ret);
  json strat_json = json::object();
  for (auto& [k, v] : by_strategy) {
    BootCI b = bootstrap_mean(v);
    strat_json[k] = {{"n", b.n}, {"mean", b.mean}, {"lo", b.lo}, {"hi", b.hi}};
  }
  rep["realized"] = {{"n_markets", static_cast<int>(ret.size())},
                     {"n_events", static_cast<int>(events.size())},
                     {"total_pnl", total_pnl},
                     {"total_staked", total_staked},
                     {"edge_mean", edge.mean},
                     {"edge_lo90", edge.lo},
                     {"edge_hi90", edge.hi},
                     {"by_strategy", strat_json}};
  rep["trades"] = trades;

  // ---- 2. calibration of traded model probabilities --------------------------------------
  json filled = store.query(
      "SELECT p.p_model, p.outcome_side, m.result FROM proposals p JOIN markets m ON m.ticker=p.ticker"
      " WHERE p.status='filled' AND p.p_model>0 AND m.result IN ('yes','no')");
  struct Bucket { double lo, hi; int n = 0; double sum_p = 0; int wins = 0; };
  std::vector<Bucket> buckets = {{0.0, 0.5}, {0.5, 0.6}, {0.6, 0.7}, {0.7, 0.85}, {0.85, 1.01}};
  for (const auto& r : filled) {
    double pm = gnum(r, "p_model");
    std::string side = gstr(r, "outcome_side");
    double pw = side == "yes" ? pm : 1.0 - pm;
    bool win = gstr(r, "result") == side;
    for (auto& b : buckets)
      if (pw >= b.lo && pw < b.hi) {
        b.n++;
        b.sum_p += pw;
        b.wins += win ? 1 : 0;
      }
  }
  json cal = json::array();
  bool cal_ok = true;
  for (const auto& b : buckets) {
    double mean_p = b.n ? b.sum_p / b.n : 0, win_rate = b.n ? static_cast<double>(b.wins) / b.n : 0;
    bool ok = b.n < 10 || std::fabs(win_rate - mean_p) <= 0.10;
    if (!ok) cal_ok = false;
    cal.push_back({{"lo", b.lo}, {"hi", b.hi}, {"n", b.n}, {"mean_p", mean_p}, {"win_rate", win_rate}, {"ok", ok}});
  }
  rep["calibration"] = cal;

  // ---- 3. paired Brier: model vs market mid, last estimate per (ticker, strategy) -------------
  json mf = store.query(
      "SELECT f.ticker, f.strategy, f.p, f.yes_bid, f.yes_ask, m.result FROM model_fairs f"
      " JOIN markets m ON m.ticker=f.ticker"
      " WHERE m.result IN ('yes','no') AND f.id IN (SELECT MAX(id) FROM model_fairs GROUP BY ticker, strategy)");
  std::vector<double> diffs;
  std::map<std::string, std::vector<double>> diffs_by;
  for (const auto& r : mf) {
    double yb = gnum(r, "yes_bid"), ya = gnum(r, "yes_ask");
    if (yb <= 0 && ya <= 0) continue;
    double mid = (yb > 0 && ya > 0) ? 0.5 * (yb + ya) : std::max(yb, ya);
    double y = gstr(r, "result") == "yes" ? 1.0 : 0.0;
    double pm = gnum(r, "p");
    double d = (pm - y) * (pm - y) - (mid - y) * (mid - y);
    diffs.push_back(d);
    diffs_by[gstr(r, "strategy")].push_back(d);
  }
  BootCI bd = bootstrap_mean(diffs);
  json bs = json::object();
  for (auto& [k, v] : diffs_by) {
    BootCI b = bootstrap_mean(v);
    bs[k] = {{"n", b.n}, {"mean", b.mean}, {"lo", b.lo}, {"hi", b.hi}};
  }
  rep["brier"] = {{"n", bd.n}, {"diff_mean", bd.mean}, {"diff_lo90", bd.lo}, {"diff_hi90", bd.hi}, {"by_strategy", bs}};

  // ---- 4. ops --------------------------------------------------------------------------
  json ops = store.query(
      "SELECT (SELECT COUNT(*) FROM proposals WHERE status='failed') AS failed,"
      " (SELECT COUNT(*) FROM proposals WHERE status='filled') AS filled,"
      " (SELECT COUNT(*) FROM proposals WHERE status='submitted') AS open_orders,"
      " (SELECT COUNT(*) FROM proposals WHERE status='rejected') AS rejected,"
      " (SELECT MIN(ts) FROM fills) AS first_fill_ts,"
      " (SELECT COUNT(*) FROM fills WHERE order_id NOT IN (SELECT order_id FROM orders WHERE proposal_id>0)) AS external_fills");
  const json& o = ops.empty() ? json::object() : ops[0];
  double first_fill = gnum(o, "first_fill_ts");
  double live_days = first_fill > 0 ? (now - first_fill) / 86400.0 : 0;
  rep["ops"] = {{"failed_orders", gnum(o, "failed")},
                {"filled", gnum(o, "filled")},
                {"open", gnum(o, "open_orders")},
                {"rejected", gnum(o, "rejected")},
                {"external_fills", gnum(o, "external_fills")},
                {"live_days", live_days}};

  // ---- 5. the gate -----------------------------------------------------------------------
  json gate = json::array();
  auto crit = [&](const std::string& name, bool pass, const std::string& detail) {
    gate.push_back({{"name", name}, {"pass", pass}, {"detail", detail}});
  };
  crit("150 independent settled events", events.size() >= 150, std::to_string(events.size()) + " so far");
  crit("28 days live", live_days >= 28, std::to_string(static_cast<int>(live_days)) + " days");
  crit("zero rejected/unintended orders", gnum(o, "failed") == 0, std::to_string(static_cast<int>(gnum(o, "failed"))) + " failed");
  char buf[160];
  std::snprintf(buf, sizeof buf, "mean %.3f, 90%% CI [%.3f, %.3f], n=%d", edge.mean, edge.lo, edge.hi, edge.n);
  crit("realized net edge > 0 and CI low > -0.05", edge.n > 0 && edge.mean > 0 && edge.lo > -0.05, buf);
  crit("calibration within 10pp per bucket", cal_ok && !filled.empty(), std::to_string(filled.size()) + " settled model trades");
  std::snprintf(buf, sizeof buf, "Brier diff %.4f (model - market), n=%d", bd.mean, bd.n);
  crit("model Brier <= market Brier (n>=100)", bd.n >= 100 && bd.mean <= 0, buf);
  bool all = true;
  for (const auto& g : gate) all = all && g["pass"].get<bool>();
  rep["gate"] = gate;
  rep["gate_pass"] = all;
  rep["bankroll"] = cfg.bankroll_dollars;
  rep["generated_ts"] = now;
  return rep;
}

json build_pnl(Store& store, const std::string& day_tz) {
  long long now = now_s();
  std::string today = trading_day(day_tz, now);
  int tz_off = tz_offset_seconds(day_tz, now);
  json rep;
  // Realized P&L ledger: Kalshi's gross realized number per market, kept for flat markets too
  // (a position closed before settlement) and frozen once the market settles.
  std::map<std::string, std::pair<double, double>> ledger;  // ticker -> (realized, fees_paid)
  for (const auto& r : store.query("SELECT ticker, realized_pnl, fees_paid FROM realized", {}))
    ledger[gstr(r, "ticker")] = {gnum(r, "realized_pnl"), gnum(r, "fees_paid")};
  // Settled: revenue - cost - trade fees, attributed to the first proposal's strategy.
  json settled = store.query(
      "SELECT s.ticker, s.settled_time, s.revenue, s.yes_cost, s.no_cost, s.fee,"
      " (SELECT COALESCE(SUM(f.fee),0) FROM fills f WHERE f.ticker=s.ticker) AS trade_fees,"
      " (SELECT p.strategy FROM proposals p JOIN orders o ON o.proposal_id=p.id WHERE o.ticker=s.ticker ORDER BY p.id LIMIT 1) AS strategy,"
      " (SELECT sr.category FROM markets m JOIN series sr ON sr.ticker=m.series_ticker WHERE m.ticker=s.ticker) AS category"
      " FROM settlements s WHERE (s.yes_count>0 OR s.no_count>0)");
  double settled_total = 0, settled_today = 0, fees_total = 0;
  std::map<std::string, double> by_strategy, by_category;
  std::map<std::string, int> n_strategy;
  std::set<std::string> settled_tickers;
  for (const auto& r : settled) {
    std::string t = gstr(r, "ticker");
    settled_tickers.insert(t);
    // One source per market. Kalshi's realized number (ledger) already includes the settlement
    // and any earlier closes; the settlement record alone is used only for markets that predate
    // the ledger (its revenue is 0 for markets that were held on both sides, so it is the
    // weaker source).
    double pnl, fees;
    if (ledger.count(t)) { pnl = ledger[t].first - ledger[t].second; fees = ledger[t].second; }
    else { pnl = gnum(r, "revenue") - gnum(r, "yes_cost") - gnum(r, "no_cost") - gnum(r, "fee") - gnum(r, "trade_fees"); fees = gnum(r, "trade_fees") + gnum(r, "fee"); }
    settled_total += pnl;
    fees_total += fees;
    long long settled_at = parse_iso(gstr(r, "settled_time"));
    if (settled_at > 0 && trading_day(day_tz, settled_at) == today) settled_today += pnl;
    std::string st = gstr(r, "strategy");
    if (st.empty()) st = "external";
    by_strategy[st] += pnl;
    n_strategy[st]++;
    std::string cat = gstr(r, "category");
    by_category[cat.empty() ? "?" : cat] += pnl;
  }
  // Markets settled flat (closed out before expiry, or a zero-count settlement record): the
  // realized ledger is the whole story for them.
  double realized_closed = 0, fees_closed = 0;
  std::set<std::string> open_tickers;
  for (const auto& p : store.positions()) open_tickers.insert(p.ticker);
  for (const auto& [t, v] : ledger) {
    if (settled_tickers.count(t) || open_tickers.count(t)) continue;
    realized_closed += v.first;
    fees_closed += v.second;
  }
  // Open positions: realized so far (Kalshi's number) and unrealized at the mid.
  double realized_open = 0, unrealized = 0, settling = 0, fees_open = 0, exposure = 0;
  json pos = json::array();
  for (const auto& p : store.positions()) {
    auto m = store.get_market(p.ticker);
    double mid = m ? m->mark() : 0;
    double n = std::fabs(p.position);
    double value = p.position > 0 ? n * mid : n * (1.0 - mid);
    double u = m ? value - std::fabs(p.exposure) : 0;
    bool closed = m && (!m->tradable() || (parse_iso(m->close_time) > 0 && parse_iso(m->close_time) <= now));
    // Trading closed: the outcome is fixed and only the payout is pending. That is not
    // "unrealized" in any useful sense; report it as settling.
    if (closed) settling += u; else unrealized += u;
    realized_open += p.realized_pnl;
    fees_open += p.fees_paid;
    exposure += std::fabs(p.exposure);
    pos.push_back({{"ticker", p.ticker}, {"position", p.position}, {"cost", std::fabs(p.exposure)},
                   {"mid", mid}, {"value", value}, {"unrealized", u}, {"realized", p.realized_pnl},
                   {"fees", p.fees_paid}, {"close_time", m ? m->close_time : ""},
                   {"expected_expiration_time", m ? m->expected_expiration_time : ""},
                   {"trading_closed", m ? (!m->tradable() || (parse_iso(m->close_time) > 0 && parse_iso(m->close_time) <= now)) : false}});
  }
  json bs = json::object();
  for (auto& [k, v] : by_strategy) bs[k] = {{"pnl", v}, {"n", n_strategy[k]}};
  json bc = json::object();
  for (auto& [k, v] : by_category) bc[k] = v;
  rep["settled_pnl"] = settled_total;
  rep["settled_today"] = settled_today;
  rep["realized_open"] = realized_open;
  rep["realized_closed"] = realized_closed - fees_closed;  // flat markets, net of their fees
  rep["unrealized"] = unrealized;   // live markets only
  rep["settling"] = settling;       // trading closed, payout pending (outcome fixed)
  rep["fees"] = fees_total + fees_open + fees_closed;
  rep["exposure"] = exposure;
  rep["total"] = settled_total + realized_open - fees_open + realized_closed - fees_closed + unrealized + settling;
  rep["day"] = today;
  rep["day_tz"] = day_tz;
  (void)tz_off;
  rep["by_strategy"] = bs;
  rep["by_category"] = bc;
  rep["positions"] = pos;
  rep["n_settled"] = static_cast<int>(settled.size());
  rep["generated_ts"] = now;
  return rep;
}

}  // namespace desk
