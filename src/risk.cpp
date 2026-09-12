#include "desk/risk.h"

#include "desk/universe.h"
#include "desk/log.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/timeutil.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <set>
#include <stdexcept>
namespace desk {
double algorithm_position_quantity(Store& store, const std::string& ticker, bool yes_side) {
  const std::string side = yes_side ? "yes" : "no";
  double owned = 0;
  auto exists = [&](const std::string& table) {
    return !store.query("SELECT name FROM sqlite_master WHERE type='table' AND name=?", {table}).empty();
  };
  if (exists("systematic_trades")) {
    // Either fills or orders may update first. Protect acknowledged entry fills from both
    // feeds. Once open, filled is the remaining strategy quantity after partial exits.
    for (const auto& r : store.query(
        "SELECT COALESCE(t.filled,0) AS filled, t.state, COALESCE(o.fill_count,0) AS entry_filled, "
        "(SELECT COALESCE(SUM(f.count),0) FROM fills f WHERE t.entry_order_id!='' AND f.order_id=t.entry_order_id AND f.outcome_side=t.side) AS recorded_filled "
        "FROM systematic_trades t LEFT JOIN orders o ON o.order_id=t.entry_order_id "
        "WHERE t.ticker=? AND t.side=? AND t.mode='live' AND t.state IN ('entering','open','exiting','settling')",
        {ticker, side}))
      owned += r.value("state", "") == "entering" ? std::max({r.value("filled", 0.0), r.value("entry_filled", 0.0), r.value("recorded_filled", 0.0)}) : r.value("filled", 0.0);
  }
  if (exists("xvenue_trades")) {
    for (const auto& r : store.query(
        "SELECT COALESCE(t.kalshi_filled,0) AS filled, COALESCE(o.fill_count,0) AS entry_filled, "
        "(SELECT COALESCE(SUM(f.count),0) FROM fills f WHERE t.kalshi_order_id!='' AND f.order_id=t.kalshi_order_id AND f.outcome_side=t.kalshi_side) AS recorded_filled "
        "FROM xvenue_trades t LEFT JOIN orders o ON o.order_id=t.kalshi_order_id "
        "WHERE t.kalshi_ticker=? AND t.kalshi_side=? AND t.mode='live' "
        "AND t.state IN ('kalshi_working','hedging','locked','unhedged','awaiting_settlement')", {ticker, side}))
      owned += std::max({r.value("filled", 0.0), r.value("entry_filled", 0.0), r.value("recorded_filled", 0.0)});
  }
  return std::max(0.0, owned);
}

double manual_position_quantity(Store& store, const Position& position) {
  return floor_contracts(std::max(0.0, std::fabs(position.position) -
      algorithm_position_quantity(store, position.ticker, position.position > 0)));
}

std::string risk_cluster(Store& store, const Market& m) {
  auto s = store.get_series(m.series_ticker);
  std::string date = event_date(m.event_ticker);
  if (s && s->category.find("Weather") != std::string::npos) return "wx|" + date;
  if (s && s->category == "Crypto") return m.series_ticker + "|" + date;
  // Series metadata may not have been refreshed for an external holding.
  if (m.series_ticker.find("KXHIGH") == 0 || m.series_ticker.find("KXLOW") == 0 || m.series_ticker == "KXDENHIGH") return "wx|" + date;
  if (m.series_ticker == "KXBTC" || m.series_ticker == "KXETH") return m.series_ticker + "|" + date;
  return m.event_ticker.empty() ? m.ticker : m.event_ticker;
}
bool weather_source_supported(const Market& m) {
  std::string rules = m.rules_primary;
  std::transform(rules.begin(), rules.end(), rules.begin(), [](unsigned char c){return std::tolower(c);});
  // The pre-August form: the rules named the National Weather Service as the authority outright.
  if (rules.find("weather company") == std::string::npos &&
      (rules.find("national weather service") != std::string::npos || rules.find("nws") != std::string::npos))
    return true;
  // Kalshi moved the settlement authority to The Weather Company on 2026-08-27, and since then
  // every listed temperature contract names TWC. Refusing all of them - which is what this
  // function did - left the entire weather category permanently research-only: 111 of 111 active
  // markets failed the gate, and no amount of model work could change that.
  //
  // TWC is accepted only when the contract STILL names an NWS daily climate product as its
  // location, and that product is the one the desk has mapped for this series. Both halves
  // matter: the product code is what research/wx_discover.py reads out of these same rules and
  // checks against api.weather.gov, and requiring it to match the mapping means a contract that
  // quietly changes station stops verifying instead of silently verifying against the wrong one.
  //
  // The evidence is reports/wx_settlement_watch.md, which is a standing check rather than the
  // one-off six-for-one-date in docs/settlement.md: TWC's settled value fell inside the Kalshi
  // bracket that settled YES on 30 of 30 completed station-days, across six cities and five
  // dates. Thirty agreements is a sample, not a proof, so this unlocks MANUAL only - graduation
  // to AUTO still has to clear its own gate, and one disagreement in that report should send
  // this straight back to false.
  const SettlementSpec* spec = settlement_for(m.series_ticker);
  if (!spec || !spec->product || !*spec->product) return false;
  std::string product = spec->product;
  std::transform(product.begin(), product.end(), product.begin(),
                 [](unsigned char ch){return std::tolower(ch);});
  return rules.find("(" + product + ")") != std::string::npos;
}
// An event ticker alone does not imply exclusivity or completeness. Thresholds can overlap,
// and a local cache can omit brackets or a game's draw. Fail closed until event metadata is
// fetched and verified: retain only exact-contract offsets, with both outcomes per ticker.
static std::map<std::string, std::pair<double, double>> market_losses(const std::vector<Holding>& holdings) {
  std::map<std::string, std::pair<double, double>> losses;
  for (const auto& h : holdings) {
    double yes_loss = h.cost - (h.yes ? h.count : 0.0);
    double no_loss = h.cost - (h.yes ? 0.0 : h.count);
    // Each outstanding order may independently fill or stay unfilled. Maximizing its loss
    // per outcome covers every partial-fill subset and never spends an unfilled hedge.
    if (h.contingent) { yes_loss = std::max(0.0, yes_loss); no_loss = std::max(0.0, no_loss); }
    losses[h.ticker].first += yes_loss;
    losses[h.ticker].second += no_loss;
  }
  return losses;
}

// Do the event's brackets tile the whole line? Strike types come from the market rows:
//   less(cap)      -> settle <= cap-1        greater(floor) -> settle >= floor+1
//   between(lo,hi) -> lo <= settle <= hi
// Contiguous coverage with an open bracket at each end means exactly one of them settles YES.
static bool strikes_are_exhaustive(const std::vector<Market>& ms) {
  bool have_low_open = false, have_high_open = false;
  std::vector<std::pair<double, double>> closed;  // inclusive integer ranges
  double low_open_upto = 0, high_open_from = 0;
  for (const auto& m : ms) {
    if (m.strike_type == "less" && m.cap_strike) {
      if (have_low_open) return false;  // two open ends: not a clean ladder
      have_low_open = true;
      low_open_upto = *m.cap_strike - 1;
    } else if (m.strike_type == "greater" && m.floor_strike) {
      if (have_high_open) return false;
      have_high_open = true;
      high_open_from = *m.floor_strike + 1;
    } else if (m.strike_type == "between" && m.floor_strike && m.cap_strike) {
      closed.push_back({*m.floor_strike, *m.cap_strike});
    } else {
      return false;  // structured or unknown strike: coverage cannot be proven from strikes
    }
  }
  if (!have_low_open || !have_high_open) return false;
  std::sort(closed.begin(), closed.end());
  double covered_to = low_open_upto;
  for (const auto& [lo, hi] : closed) {
    if (lo > covered_to + 1.5) return false;   // a gap: some settle value wins nothing
    covered_to = std::max(covered_to, hi);
  }
  return high_open_from <= covered_to + 1.5;
}

EventStructure event_structure(Store& store, const std::string& event_ticker) {
  // The structure of an event does not change while it is open, and the risk book asks for it
  // once per event per refresh. Cache it for a minute so a book with many events does not
  // re-read every bracket from SQLite on every call.
  struct Cached { EventStructure st; long long ts; };
  static std::map<std::string, Cached> cache;
  static std::mutex cache_mu;
  long long now = now_s();
  EventStructure st;
  if (event_ticker.empty()) return st;
  // The flag itself is one indexed read and is never cached: an event whose metadata has just
  // arrived must be usable on the next call, not a minute later.
  json meta = store.query("SELECT mutually_exclusive,exhaustive FROM event_meta WHERE event_ticker=?", {event_ticker});
  st.exclusive = !meta.empty() && meta[0].value("mutually_exclusive", 0) == 1;
  bool recorded_exhaustive = !meta.empty() && meta[0].value("exhaustive", 0) == 1;
  if (!st.exclusive) return st;   // without the exchange's own flag, assume nothing
  // The bracket list and the coverage proof are the expensive part, and they do not change
  // while the event is open.
  {
    std::lock_guard<std::mutex> lk(cache_mu);
    auto it = cache.find(event_ticker);
    if (it != cache.end() && now - it->second.ts < 60) return it->second.st;
    if (cache.size() > 4000) cache.clear();
  }
  std::vector<Market> ms = store.markets_in_event(event_ticker);
  if (ms.size() < 2) { st.exclusive = false; return st; }   // not cached: brackets may still be listing
  std::sort(ms.begin(), ms.end(), [](const Market& a, const Market& b) {
    double ka = a.floor_strike ? *a.floor_strike : (a.cap_strike ? *a.cap_strike - 1000 : 0);
    double kb = b.floor_strike ? *b.floor_strike : (b.cap_strike ? *b.cap_strike - 1000 : 0);
    return ka == kb ? a.ticker < b.ticker : ka < kb;
  });
  for (const auto& m : ms) {
    st.tickers.push_back(m.ticker);
    st.labels.push_back(!m.yes_sub_title.empty() ? m.yes_sub_title : (!m.subtitle.empty() ? m.subtitle : m.ticker));
  }
  // Exhaustive means no "nothing settles yes" state. Brackets prove it by tiling the line. A
  // three-way match has no strikes to tile, but proves it another way: the contract settles on
  // the result after ninety minutes plus stoppage, and a match that has been played ends home,
  // draw or away. Without this the book carries a state that cannot happen, and in that phantom
  // state every leg loses - which makes a genuine hedge look like it buys nothing.
  const SettlementSpec* spec = ms.empty() ? nullptr : settlement_for(ms.front().series_ticker);
  bool three_way_match = spec && std::string(spec->kind) == "soccer_90" && ms.size() == 3;
  st.exhaustive = strikes_are_exhaustive(ms) || three_way_match || recorded_exhaustive;
  {
    std::lock_guard<std::mutex> lk(cache_mu);
    cache[event_ticker] = {st, now};
  }
  return st;
}

// Loss of the book if `yes_ticker` settles YES and every other market in the event settles NO.
// An unfilled order may simply not fill, so each contingent leg takes the better of the two.
static double loss_in_state(const std::vector<Holding>& holdings, const std::string& yes_ticker) {
  double loss = 0;
  for (const auto& h : holdings) {
    bool wins = h.yes ? (h.ticker == yes_ticker) : (h.ticker != yes_ticker);
    double l = h.cost - (wins ? h.count : 0.0);
    loss += h.contingent ? std::max(0.0, l) : l;
  }
  return loss;
}

std::vector<StatePayoff> event_payoffs(Store& store, const std::string& event_ticker,
                                       const std::vector<Holding>& holdings) {
  std::vector<StatePayoff> out;
  EventStructure st = event_structure(store, event_ticker);
  if (!st.exclusive) return out;   // no verified structure: the payoff table would be a guess
  for (size_t i = 0; i < st.tickers.size(); ++i) {
    StatePayoff sp;
    sp.ticker = st.tickers[i];
    sp.label = st.labels[i];
    sp.loss = loss_in_state(holdings, st.tickers[i]);
    if (auto m = store.get_market(st.tickers[i])) {
      double mid = m->mid();
      if (mid > 0 && mid < 1) sp.prob = mid;
    }
    out.push_back(sp);
  }
  if (!st.exhaustive) {
    StatePayoff none;
    none.label = "none of them";
    none.loss = loss_in_state(holdings, "");
    double rest = 1.0;
    bool known = true;
    for (const auto& sp : out) { if (sp.prob < 0) known = false; else rest -= sp.prob; }
    none.prob = known ? std::max(0.0, rest) : -1;
    out.push_back(none);
  }
  return out;
}

EventRisk event_risk(Store& store, const std::string& event_ticker, const std::vector<Holding>& holdings) {
  EventRisk r;
  for (const auto& h : holdings) r.cost += h.cost;
  // Verified mutually exclusive: exactly one market settles YES (plus a "none" state unless the
  // brackets tile the line). That is the whole state space, so the worst case is a max over it
  // instead of the sum of per-ticker worst cases.
  std::vector<StatePayoff> states = event_payoffs(store, event_ticker, holdings);
  if (!states.empty()) {
    const StatePayoff* worst = &states[0];
    for (const auto& sp : states) if (sp.loss > worst->loss) worst = &sp;
    r.worst_loss = std::max(0.0, worst->loss);
    r.worst_state = worst->ticker.empty() ? "nothing settles YES" : worst->ticker + "=YES";
    return r;
  }
  for (const auto& [ticker, losses] : market_losses(holdings)) {
    r.worst_loss += std::max({0.0, losses.first, losses.second});
    if (!r.worst_state.empty()) r.worst_state += ", ";
    r.worst_state += ticker + (losses.first >= losses.second ? "=YES" : "=NO");
  }
  return r;
}

StackCheck stack_check(Store& store, const std::string& event_ticker,
                       const std::vector<Holding>& existing, const Holding& add) {
  StackCheck c;
  c.added_cost = add.cost;
  c.worst_before = event_risk(store, event_ticker, existing).worst_loss;
  if (c.worst_before <= 1e-9) {
    c.reason = "nothing at risk in this event yet";
    return c;   // the first leg in an event cannot stack anything
  }
  std::vector<Holding> after = existing;
  after.push_back(add);
  c.worst_after = event_risk(store, event_ticker, after).worst_loss;

  // The old test asked whether the new leg pays in ANY state where the book was already at its
  // worst, and called that a hedge. Paris YES plus Marseille NO passed it: Marseille NO does pay
  // on a draw, which was one of two equally bad states. It also loses its whole cost in the other
  // one, so the worst case went from $1.55 to $7.89. Asking what happens to the worst case
  // answers the question the old test only approximated.
  double rise = c.worst_after - c.worst_before;
  c.protection = c.added_cost - rise;   // 0 = it bought nothing back; == cost = fully protective
  c.stacks = rise >= c.added_cost - 1e-9;
  if (c.stacks)
    c.reason = "worst case rises by the leg's full cost ($" + fmt2(c.added_cost) +
               ", $" + fmt2(c.worst_before) + " -> $" + fmt2(c.worst_after) +
               "): there is no settlement state where this pays back while the book is at its "
               "worst, so it is the same bet again rather than a hedge";
  else
    c.reason = "worst case $" + fmt2(c.worst_before) + " -> $" + fmt2(c.worst_after) + "; $" +
               fmt2(c.protection) + " of the $" + fmt2(c.added_cost) + " bought back";
  return c;
}

bool stacks_event(Store& store, const std::string& event_ticker, const std::vector<Holding>& existing, const Holding& add) {
  return stack_check(store, event_ticker, existing, add).stacks;
}

Exposure committed_exposure(Store& store) {
  Exposure out;
  std::map<std::string, std::string> cluster_of;
  auto add = [&](const std::string& t, bool yes, double count, double cost, bool contingent = false) {
    auto m = store.get_market(t);
    std::string ev = m ? m->event_ticker : t;
    out.holdings[ev].push_back(Holding{t, yes, count, cost, contingent});
    cluster_of[ev] = m ? risk_cluster(store, *m) : t;
  };
  long long now = now_s();
  for (const auto& p: store.positions()) {
    if (p.position == 0) continue;
    double cost=std::fabs(p.exposure);
    // A market that has stopped trading is no longer risk you can manage: the outcome is fixed
    // and the money comes back (or not) at settlement. Keep it out of today's caps and bars.
    auto m = store.get_market(p.ticker);
    long long tclose = m ? parse_iso(m->close_time) : 0;
    if (m && (!m->tradable() || (tclose > 0 && tclose <= now))) {
      out.settling += cost;
      out.settling_tickers.push_back(p.ticker);
      continue;
    }
    out.filled+=cost;
    add(p.ticker, p.position > 0, std::fabs(p.position), cost);
  }
  for (const auto& o: store.orders("resting")) {
    double price=o.outcome_side=="yes"?o.yes_price:o.no_price;
    double cost=o.remaining_count*price;
    auto m=store.get_market(o.ticker);
    auto s=m?store.get_series(m->series_ticker):std::nullopt;
    if(s) cost+=FeeModel::from_series(*s).fee(price,o.remaining_count,false);
    out.resting+=cost;
    add(o.ticker, o.outcome_side == "yes", o.remaining_count, cost, true);
  }
  for (const auto& [ev, hs] : out.holdings) {
    EventRisk r = event_risk(store, ev, hs);
    out.events[ev] = r.worst_loss;
    out.worst_state[ev] = r.worst_state;
    out.clusters[cluster_of[ev]] += r.worst_loss;
    out.committed += r.worst_loss;
  }
  return out;
}
CashReconciliation reconcile_cash(Store& store, long long from_ts, long long to_ts) {
  CashReconciliation r;
  // Positions have to be replayed from the beginning, or an offsetting fill inside the window
  // looks like an opening trade. Only the window is scored.
  std::map<std::string, double> pos;   // ticker -> net YES contracts, negative for net NO
  for (const auto& f : store.query(
           "SELECT ts,ticker,outcome_side,count,yes_price,no_price,fee FROM fills ORDER BY ts")) {
    std::string ticker = f.value("ticker", "");
    bool yes = f.value("outcome_side", "") == "yes";
    double n = f.value("count", 0.0);
    double px = yes ? f.value("yes_price", 0.0) : f.value("no_price", 0.0);
    double fee = f.value("fee", 0.0);
    long long ts = f.value("ts", 0LL);
    double held = pos[ticker];
    double offset = (held != 0 && (held > 0) != yes) ? std::min(n, std::fabs(held)) : 0.0;
    if (ts >= from_ts && ts <= to_ts) {
      // The whole fill is paid for, offsetting part included; what comes back is the $1 of
      // collateral per matched contract. Netting the cost out instead of adding the release is
      // the mistake that leaves a reconciliation short by the closing price.
      r.paid += n * px;
      r.opened += (n - offset) * px;
      r.released += offset;
      r.fees += fee;
    }
    pos[ticker] = held + (yes ? n : -n);
  }
  for (const auto& s : store.query("SELECT revenue,settled_time FROM settlements"))
    if (parse_iso(s.value("settled_time", "")) >= from_ts && parse_iso(s.value("settled_time", "")) <= to_ts)
      r.settlements += s.value("revenue", 0.0);
  for (const auto& c : store.query("SELECT amount FROM cashflows WHERE ts>=? AND ts<=?", {from_ts, to_ts}))
    r.flows += c.value("amount", 0.0);

  auto open_row = store.query("SELECT balance FROM account WHERE ts>=? ORDER BY ts LIMIT 1", {from_ts});
  auto last_row = store.query("SELECT balance FROM account WHERE ts<=? ORDER BY ts DESC LIMIT 1", {to_ts});
  if (!open_row.empty() && !last_row.empty())
    r.actual = last_row[0].value("balance", 0.0) - open_row[0].value("balance", 0.0);
  r.predicted = -r.paid - r.fees + r.released + r.settlements + r.flows;
  r.residual = r.actual - r.predicted;
  r.explained = std::fabs(r.residual) < 0.01;
  return r;
}

Calibration strategy_calibration(Store& store, const std::string& strategy, int min_n) {
  Calibration c;
  // Every fair this strategy logged on a market that has since settled, against the mid it was
  // quoting at the time. Both are scored on the same events, so the comparison is paired.
  //
  // One sample per MARKET. The rows are not independent: the same contract quoted every cycle
  // for a day shares a single outcome, so averaging over rows both inflates the sample count and
  // weights whichever markets happened to be quoted longest. Take the last fair before the
  // market settled - the most information the strategy ever had on it - and let each market
  // count once.
  json rows = store.query(
      "SELECT f.ticker AS ticker, f.p AS p, f.yes_bid AS bid, f.yes_ask AS ask, m.result AS result "
      "FROM model_fairs f JOIN markets m ON m.ticker=f.ticker "
      "WHERE f.strategy=? AND m.result IN ('yes','no') "
      "AND f.ts = (SELECT MAX(g.ts) FROM model_fairs g WHERE g.ticker=f.ticker AND g.strategy=f.strategy "
      "            AND g.yes_bid>0 AND g.yes_ask>0 AND g.yes_ask<1)", {strategy});
  std::vector<double> diffs;
  double sm = 0, sk = 0;
  std::set<std::string> seen;
  for (const auto& r : rows) {
    double bid = r.value("bid", 0.0), ask = r.value("ask", 0.0);
    if (bid <= 0 || ask <= 0 || ask >= 1) continue;   // no two-sided market to be judged against
    const std::string ticker = r.value("ticker", "");
    if (!seen.insert(ticker).second) continue;        // a tie on ts must not count twice
    double outcome = r.value("result", "") == "yes" ? 1.0 : 0.0;
    double pm = r.value("p", 0.0), mid = 0.5 * (bid + ask);
    double bm = (pm - outcome) * (pm - outcome), bk = (mid - outcome) * (mid - outcome);
    sm += bm; sk += bk; diffs.push_back(bm - bk);
    ++c.n;
  }
  c.rows = static_cast<int>(rows.size());
  if (c.n > 0) { c.brier_model = sm / c.n; c.brier_market = sk / c.n; c.diff = c.brier_model - c.brier_market; }
  if (c.n > 1) {
    double var = 0;
    for (double d : diffs) var += (d - c.diff) * (d - c.diff);
    const double se = std::sqrt(var / (c.n - 1) / c.n);
    c.diff_lo = c.diff - 1.96 * se;
    c.diff_hi = c.diff + 1.96 * se;
  }
  // Demonstrated means the whole interval sits below zero. A point estimate that merely ties is
  // the null result it looks like, and this gate decides what trades unattended.
  c.demonstrated = c.n >= min_n && c.diff_hi < 0;
  const std::string scores = "Brier " + fmt2(c.brier_model) + " vs the market's " + fmt2(c.brier_market) +
                             " on " + std::to_string(c.n) + " settled markets";
  if (c.n < min_n)
    c.note = std::to_string(c.n) + " settled markets, " + std::to_string(min_n) + " needed" +
             (c.rows > c.n ? " (" + std::to_string(c.rows) + " fairs behind them)" : "");
  else if (c.demonstrated)
    c.note = scores + "; the model is better by " + fmt2(-c.diff) + " (95% CI " + fmt2(-c.diff_hi) +
             " to " + fmt2(-c.diff_lo) + ")";
  else if (c.diff_lo > 0)
    c.note = scores + ": the price is the better estimate";
  else
    c.note = scores + ": indistinguishable from the price (95% CI on the difference " +
             fmt2(c.diff_lo) + " to " + fmt2(c.diff_hi) + ")";
  return c;
}

double untracked_transferred(Store& store) {
  double out = 0;
  // /portfolio/balance reports the event-contract instance, summed over its exchange shards.
  // Money moved to the margined instance leaves that sum and there is no documented way to read
  // it back: the endpoint takes only `subaccount` and `exchange_index`, and Kalshi does not
  // publish which index, if any, addresses the margined instance. So this is the net amount SENT
  // there, which is what it is worth only if nothing has been traded on it. It is not a loss and
  // it is not spendable here; it is simply outside every number this desk computes.
  for (const auto& c : store.query("SELECT amount,kind FROM cashflows"))
    if (c.value("kind", "").find("margined") != std::string::npos) out -= c.value("amount", 0.0);
  return std::max(0.0, out);
}

json EntryContext::to_json() const {
  return {{"kind", kind},
          {"allowed", allowed},
          {"approval_required", approval_required},
          {"has_event_position", has_event_position},
          {"has_working_order", has_working_order},
          {"required_edge", required_edge},
          {"cooldown_remaining", cooldown_remaining},
          {"settlement_ts", settlement_ts},
          {"reason", reason}};
}

long long expected_settlement_ts(const Market& m) {
  // close_time is only when orders stop. It can precede determination by days and therefore
  // cannot prove the user's T+2 settlement constraint.
  for (const std::string* s : {&m.expected_expiration_time, &m.expiration_time}) {
    long long ts = parse_iso(*s);
    if (ts > 0) return ts;
  }
  return 0;
}

EntryContext entry_context(Store& store, const Config& cfg, const Proposal& p, const Market& m,
                           long long now) {
  if (now <= 0) now = now_s();
  EntryContext c;
  c.settlement_ts = expected_settlement_ts(m);
  const long long horizon = static_cast<long long>(std::max(0.0, cfg.risk.max_entry_horizon_hours) * 3600.0);
  if (horizon > 0 && c.settlement_ts <= 0) {
    c.allowed = false;
    c.reason = "expected settlement time unavailable; T+2 entry gate cannot be verified";
    return c;
  }
  if (horizon > 0 && c.settlement_ts <= now) {
    c.allowed = false;
    c.reason = "expected settlement has passed";
    return c;
  }
  if (horizon > 0 && c.settlement_ts - now > horizon) {
    c.allowed = false;
    c.reason = "expected settlement is " + fmt2((c.settlement_ts - now) / 3600.0) +
               "h away; entry limit is " + fmt2(cfg.risk.max_entry_horizon_hours) + "h (T+2)";
    return c;
  }

  auto held = store.query(
      "SELECT p.ticker,p.position,m.event_ticker FROM positions p JOIN markets m ON m.ticker=p.ticker "
      "WHERE p.position!=0 AND m.event_ticker=?", {m.event_ticker});
  c.has_event_position = !held.empty();
  // Working = at the exchange (submitted, or in doubt). A pending proposal is a suggestion nobody
  // has sent yet: it reserves nothing, and letting it block an entry made every event with a
  // strategy signal "an event with a working order" on the board.
  auto working = store.query(
      "SELECT p.id FROM proposals p WHERE p.event_ticker=? AND p.status IN ('submitted','unknown','cancel_pending') "
      "AND p.id!=? LIMIT 1", {m.event_ticker, p.id});
  if (working.empty()) {
    // Orders placed outside this desk have no proposal row but still reserve and concentrate risk.
    working = store.query(
        "SELECT o.order_id FROM orders o JOIN markets m ON m.ticker=o.ticker WHERE m.event_ticker=? "
        "AND o.status='resting' AND o.remaining_count>0 AND o.order_id NOT IN "
        "(SELECT order_id FROM proposals WHERE id=?) LIMIT 1", {m.event_ticker, p.id});
  }
  c.has_working_order = !working.empty();

  if (p.strategy == "ladder_arb") {
    // A locked basket is several legs in one event by construction. Its own legs must not block
    // each other - the first leg to go would otherwise make every later leg "an event with a
    // working order" - and once a leg has filled, the position it left is the basket, not a bet
    // to be managed. Anything else in the event (a hand position, another strategy's order)
    // still blocks: a basket on top of an unrelated position is no longer locked.
    c.kind = "basket";
    std::string key;
    auto b = p.note.find("basket ");
    if (b != std::string::npos) {
      auto e = p.note.find('|', b);
      key = p.note.substr(b, e == std::string::npos ? std::string::npos : e - b);
      while (!key.empty() && key.back() == ' ') key.pop_back();
    }
    std::string like = "%" + (key.empty() ? std::string("basket ") : key) + "%";
    auto siblings = store.query(
        "SELECT ticker FROM proposals WHERE event_ticker=? AND strategy='ladder_arb' AND note LIKE ? AND id!=?",
        {m.event_ticker, like, p.id});
    std::set<std::string> sibling_tickers;
    for (const auto& r : siblings) sibling_tickers.insert(r.value("ticker", ""));
    for (const auto& r : held)
      if (!sibling_tickers.count(r.value("ticker", "")) && r.value("ticker", "") != m.ticker) {
        c.allowed = false;
        c.reason = "event already has exposure outside this basket; a basket on top of it is not locked";
        return c;
      }
    auto foreign = store.query(
        "SELECT id FROM proposals WHERE event_ticker=? AND status IN ('submitted','unknown','cancel_pending') "
        "AND id!=? AND NOT (strategy='ladder_arb' AND note LIKE ?) LIMIT 1", {m.event_ticker, p.id, like});
    if (foreign.empty())
      foreign = store.query(
          "SELECT o.order_id FROM orders o JOIN markets mk ON mk.ticker=o.ticker WHERE mk.event_ticker=? AND o.status='resting' "
          "AND o.remaining_count>0 AND o.order_id NOT IN (SELECT order_id FROM proposals WHERE event_ticker=? AND strategy='ladder_arb' "
          "AND note LIKE ?) LIMIT 1", {m.event_ticker, m.event_ticker, like});
    c.has_working_order = !foreign.empty();
    if (c.has_working_order) {
      c.allowed = false;
      c.reason = "event already has a working order outside this basket";
      return c;
    }
    c.allowed = true;
    c.reason = "locked basket leg";
    return c;
  }
  if (p.entry_kind == "hedge" || p.strategy == "auto_hedge") {
    c.kind = "hedge";
    Exposure ex = committed_exposure(store);
    Holding add{m.ticker, p.outcome_side == "yes", p.count,
                p.count * (p.price + p.fee_per), true};
    StackCheck sc = stack_check(store, m.event_ticker, ex.holdings[m.event_ticker], add);
    c.allowed = c.has_event_position && sc.protection > 1e-8 && sc.worst_after < sc.worst_before - 1e-8;
    c.reason = c.allowed ? "verified hedge reduces event worst case by $" + fmt2(sc.worst_before - sc.worst_after)
                         : "hedge refused: this leg does not reduce the event worst case";
    return c;
  }

  if (!c.has_event_position) {
    if (c.has_working_order) {
      c.allowed = false;
      c.kind = "managed";
      c.reason = "event already has a working order; manage that order before another entry";
    }
    return c;
  }

  bool exact_same_side = false;
  for (const auto& r : held) {
    if (r.value("ticker", "") != m.ticker) continue;
    double pos = r.value("position", 0.0);
    if ((pos > 0 && p.outcome_side == "yes") || (pos < 0 && p.outcome_side == "no"))
      exact_same_side = true;
  }
  if (!exact_same_side) {
    c.allowed = false;
    c.kind = "managed";
    c.reason = "event already has exposure; use its position or hedge controls instead of a new signal";
    return c;
  }

  c.kind = "addition";
  c.required_edge = cfg.risk.add_min_edge;
  auto last = store.query(
      "SELECT f.ts,COALESCE(p.net_edge,0) AS edge FROM fills f "
      "LEFT JOIN orders o ON o.order_id=f.order_id LEFT JOIN proposals p ON p.id=o.proposal_id "
      "WHERE f.ticker=? AND f.outcome_side=? ORDER BY f.ts DESC LIMIT 1",
      {m.ticker, p.outcome_side});
  if (!last.empty()) {
    long long last_ts = last[0].value("ts", 0LL);
    double last_edge = last[0].value("edge", 0.0);
    if (last_edge > 0) c.required_edge = std::max(c.required_edge, last_edge + cfg.risk.add_edge_improvement);
    c.cooldown_remaining = std::max(0LL, last_ts + cfg.risk.add_cooldown_seconds - now);
  }
  c.approval_required = !cfg.risk.auto_additions;
  if (c.has_working_order) {
    c.allowed = false;
    c.reason = "same-side addition waits while this event has a working order";
  } else if (c.cooldown_remaining > 0) {
    c.allowed = false;
    c.reason = "same-side addition cooling off for " + std::to_string(c.cooldown_remaining) + "s";
  } else if (p.net_edge + 1e-9 < c.required_edge) {
    c.allowed = false;
    c.reason = "same-side addition needs edge " + fmt2(c.required_edge) + "; current edge " +
               fmt2(p.net_edge);
  } else {
    c.allowed = true;
    c.reason = c.approval_required ? "qualified same-side addition; manual approval required"
                                   : "qualified same-side addition";
  }
  return c;
}

json account_stop_policy(Store& store, const Config& cfg) {
  json policy{{"enabled",cfg.risk.daily_loss_stop_enabled},{"loss_fraction",cfg.risk.daily_loss_stop_frac}};
  if(auto value=store.kv_get("account_daily_stop:v1")) {
    auto saved=json::parse(*value);
    if(!saved.is_object())throw std::runtime_error("invalid saved account stop policy");
    policy.update(saved);
  }
  return policy;
}
json shard_cash_view(Store& store,const Market& market,long long now) {
  const auto account=store.last_account();const auto ts=account?account->updated_ts:0;
  const auto raw=json::parse(store.kv_get("shard_balances").value_or("[]"),nullptr,false);
  bool known=false;double balance=0,reserved=0,queued=0;
  if(raw.is_array())for(const auto& s:raw)if(s.value("exchange_index",-1)==market.exchange_index && s.contains("balance")) {
    try {balance=s["balance"].is_string()?std::stod(s["balance"].get<std::string>()):s["balance"].get<double>();known=std::isfinite(balance)&&balance>=0;}catch(...){}
  }
  for(const auto& o:store.orders("resting")) {
    auto m=store.get_market(o.ticker);if(!m){known=false;continue;}if(m->exchange_index!=market.exchange_index)continue;
    auto series=store.get_series(m->series_ticker);if(!series){known=false;continue;}
    const double px=o.outcome_side=="yes"?o.yes_price:o.no_price;
    reserved+=o.remaining_count*px+FeeModel::from_series(*series).fee(px,o.remaining_count,false);
  }
  for(const auto& r:store.query("SELECT ticker,payload FROM manual_requests WHERE status IN ('queued','processing','unknown')")) {
    auto m=store.get_market(r.value("ticker",""));if(!m){known=false;continue;}if(m->exchange_index!=market.exchange_index)continue;
    const auto b=json::parse(r["payload"].get<std::string>());auto s=store.get_series(m->series_ticker);if(!s){known=false;continue;}
    queued+=b.value("count",0.)*b.value("price",0.)+FeeModel::from_series(*s).fee(b.value("price",0.),b.value("count",0.),!b.value("post_only",true));
  }
  const bool fresh=known&&ts>0&&now>=ts&&now-ts<=90;
  // Sizing off a balance nobody has checked recently is how a desk oversends, so an unknown or
  // stale shard is treated as zero. That is right, but it is NOT the same fact as "this shard is
  // empty", and the two used to reach the screen as the same sentence: on 2026-09-11 a 125-second
  // old snapshot told the user to fund a shard rather than to sync. Say which it is.
  std::string reason;
  if (!known) reason = "the desk has no balance for this shard yet; sync the account";
  else if (ts<=0) reason = "no account snapshot yet; sync the account";
  else if (now<ts) reason = "the account snapshot is timestamped in the future; check the clock";
  else if (now-ts>90) reason = "the account snapshot is "+std::to_string(now-ts)+"s old; sync the account";
  else if (balance-reserved-queued<=0) reason = "this shard is funded at $"+fmt2(balance)+
      " and $"+fmt2(reserved+queued)+" of that is already reserved by resting or queued orders";
  return {{"exchange_index",market.exchange_index},{"known",known},{"fresh",fresh},{"ts",ts},
    {"age",ts>0?now-ts:-1},{"reason",reason},
    {"balance",balance},{"resting_reserved",reserved},{"queued_reserved",queued},{"available",fresh?std::max(0.,balance-reserved-queued):0.}};
}
json set_account_stop_policy(Store& store, const Config& cfg, const json& changes) {
  if(!changes.is_object())throw std::runtime_error("account stop settings must be an object");
  auto policy=account_stop_policy(store,cfg);
  for(const auto& item:changes.items()) {
    if(item.key()!="enabled" && item.key()!="loss_fraction")throw std::runtime_error("unknown account stop setting: "+item.key());
    policy[item.key()]=item.value();
  }
  if(!policy["enabled"].is_boolean() || !policy["loss_fraction"].is_number())throw std::runtime_error("invalid account stop settings");
  double fraction=policy["loss_fraction"].get<double>();
  if(!std::isfinite(fraction)||fraction<.01||fraction>.99)throw std::runtime_error("account stop percentage must be between 1 and 99");
  store.kv_set("account_daily_stop:v1",policy.dump());
  return policy;
}
json daily_equity(Store& store, const Config& cfg) {
  const auto policy=account_stop_policy(store,cfg);
  auto a=store.last_account();
  std::string day=trading_day(cfg.risk.day_tz, now_s()), key="day_equity_v2:"+day;
  double start=0;
  if(auto v=store.kv_get(key)) start=std::stod(*v);
  // The reported account cash already includes resting reservations. On 2026-09-05,
  // cash 100.2273 fell to 98.2732 by exactly 1.9541 of fills/fees while other orders
  // remained unfilled. Adding their purchase cost again would invent equity.
  long long day_start = parse_iso(day+"T00:00:00Z") - tz_offset_seconds(cfg.risk.day_tz, now_s());
  long long start_ts = std::stoll(store.kv_get("day_equity_ts:"+day).value_or("0"));
  if(start<=0 && a) {
    // Baseline: the first sync of the local day.
    auto rows=store.query("SELECT ts, balance+portfolio_value AS equity FROM account WHERE ts>=? AND balance+portfolio_value>0 ORDER BY ts LIMIT 1", {day_start});
    if(!rows.empty()) { start=rows[0]["equity"].get<double>(); start_ts=rows[0]["ts"].get<long long>(); }
    if(start>0) { store.kv_set(key,std::to_string(start)); store.kv_set("day_equity_ts:"+day,std::to_string(start_ts)); }
  }
  if (start_ts == 0) start_ts = day_start;
  // Money moved in or out since the baseline (deposits, withdrawals, transfers to/from the
  // perpetuals instance) is not P&L: it shifts the baseline. Shard-to-shard moves stay inside
  // the balance we read and need no adjustment.
  double flows = 0;
  for (const auto& f : store.query("SELECT amount FROM cashflows WHERE ts>?", {start_ts})) flows += f.value("amount", 0.0);
  double equity=a?a->balance+a->portfolio_value:0;
  double adjusted_start = start + flows;
  // Whether the day's cash movement can be accounted for at all. A daily stop computed on a
  // balance the desk cannot explain is a stop on a number, not on the money.
  CashReconciliation rec = reconcile_cash(store, start_ts, now_s());
  return json{{"start_pv",start},{"pv",equity},{"cash",a?a->balance:0},{"external_flows",flows},{"adjusted_start",adjusted_start},
    {"positions_value",a?a->portfolio_value:0},{"stop_pv",adjusted_start*(1-policy["loss_fraction"].get<double>())},
    {"stop_enabled",policy["enabled"]},{"stop_loss_fraction",policy["loss_fraction"]},
    {"pnl",start>0?equity-adjusted_start:0},{"account_ts",a?a->updated_ts:0},
    {"day",day},{"day_tz",cfg.risk.day_tz},
    {"untracked_transferred",untracked_transferred(store)},
    {"cash_explained",rec.explained},{"cash_residual",rec.residual},
    {"cash_paid",rec.paid},{"cash_released",rec.released},{"cash_settlements",rec.settlements},
    {"turnover",rec.paid},{"turnover_cap",cfg.risk.max_turnover_dollars_per_day},
    {"basis","Reported cash (including resting reservations) + marked positions vs the first sync of the local day ("+cfg.risk.day_tz+"), adjusted for deposits, withdrawals and transfers to/from the perpetuals instance. Money on the perpetuals instance itself is not tracked here."}};
}
void validate_entry(Store& store,const Config& cfg,const Proposal& p,const Market& m) {
  if(p.strategy!="ticket" && !store.query("SELECT id FROM manual_requests WHERE event_ticker=? AND status IN ('queued','processing','unknown')",{m.event_ticker}).empty())
    throw std::runtime_error("Event reserved by a manual queued order");
  if (!store.kv_get("uncertain_client_order_id").value_or("").empty()) throw std::runtime_error("An earlier order outcome is uncertain; reconcile it before new entries");
  if (store.kv_get("kill").value_or("0")=="1") throw std::runtime_error("kill switch is on");
  if(!std::isfinite(p.price)||!std::isfinite(p.count)||p.price<=0||p.price>=1||p.count<.01 || std::fabs(p.count-floor_contracts(p.count))>1e-8)
    throw std::runtime_error("Price must be between 0 and 1; quantity must be at least 0.01, in 0.01 increments");
  if(p.outcome_side!="yes"&&p.outcome_side!="no") throw std::runtime_error("Invalid outcome side");
  if(p.exec_style!="maker"&&p.exec_style!="taker") throw std::runtime_error("Invalid execution style");
  if(p.price<cfg.risk.min_price||p.price>1-cfg.risk.min_price) throw std::runtime_error("Entry price is outside configured risk range");
  if(!m.tradable() || (!m.close_time.empty() && parse_iso(m.close_time)<=now_s())) throw std::runtime_error("Market is closed to new orders");
  EntryContext admission = entry_context(store, cfg, p, m);
  if (!admission.allowed) throw std::runtime_error(admission.reason);
  if(p.strategy=="weather_nws"&&!weather_source_supported(m)) throw std::runtime_error("Weather proposal is research-only: settlement source has not been validated against the NWS model");
  if(p.strategy!="ticket" && now_s()-p.updated_ts>cfg.risk.proposal_ttl_seconds) throw std::runtime_error("Proposal expired; refresh before approving");
  auto a=store.last_account();
  auto sync=store.kv_get("portfolio_sync_ts");
  if(!a||!sync||now_s()-std::stoll(*sync)>60 || now_s()-a->updated_ts>60) throw std::runtime_error("Account data stale or incomplete; sync before trading");
  auto daily=daily_equity(store,cfg);
  if(daily.value("stop_enabled",true) && daily["start_pv"].get<double>()>0 && daily["pv"].get<double>()<=daily["stop_pv"].get<double>()) throw std::runtime_error("Daily equity loss limit reached; new entries blocked");
  auto s=store.get_series(m.series_ticker);
  if(!s) throw std::runtime_error("Fee metadata unavailable; new entry blocked");
  double cost=p.price*p.count+FeeModel::from_series(*s).fee(p.price,p.count,p.exec_style=="taker");
  auto exposure=committed_exposure(store);
  double bank=std::min(cfg.bankroll_dollars,std::max(0.0,a->balance+a->portfolio_value));
  // Payoff-space: the event's risk with this stake added is its worst-case loss across outcomes,
  // so a real hedge gets credit and the same bet twice does not.
  Holding add{m.ticker, p.outcome_side=="yes", p.count, cost, true};
  const auto& existing = exposure.holdings[m.event_ticker];
  // Every entry path is checked. A qualified same-side addition is the one deliberate exception;
  // it has already cleared the stricter edge/cooldown rule above. Hedges must reduce worst case.
  if (p.strategy != "close") {
    StackCheck sc = stack_check(store, m.event_ticker, existing, add);
    if (admission.kind == "hedge" && sc.protection <= 1e-8)
      throw std::runtime_error("Hedge refused: this leg does not reduce event worst case");
    // A basket's legs are locked as a set by the finder that proposed them; judged one at a time
    // against the conservative independent-settlement model they can look like the same bet twice.
    if (sc.stacks && admission.kind != "addition" && admission.kind != "basket")
      throw std::runtime_error("Stacks the same bet in " + m.event_ticker + ": " + sc.reason);
  }
  std::vector<Holding> after = existing;
  after.push_back(add);
  EventRisk before_r = event_risk(store, m.event_ticker, existing), after_r = event_risk(store, m.event_ticker, after);
  double delta = after_r.worst_loss - before_r.worst_loss;
  // Per-order and per-day turnover, enforced here so every path meets them: hand tickets,
  // strategy entries, working orders and hedges all pass through this function.
  if (cost > cfg.risk.max_order_dollars + 1e-9)
    throw std::runtime_error("Order cap: this order costs $" + fmt2(cost) + ", limit $" +
                             fmt2(cfg.risk.max_order_dollars) + " per order");
  double turnover = daily.value("turnover", 0.0);
  if (turnover + cost > cfg.risk.max_turnover_dollars_per_day + 1e-9)
    throw std::runtime_error("Turnover cap: $" + fmt2(turnover) + " traded today, this adds $" +
                             fmt2(cost) + ", limit $" + fmt2(cfg.risk.max_turnover_dollars_per_day) +
                             ". Fees are charged on volume, not on profit");
  auto require=[&](double used,double add_risk,double cap,const char* label){ if(used+add_risk>cap+1e-8) throw std::runtime_error(std::string(label)+" cap: at risk $"+fmt2(used)+", this adds $"+fmt2(add_risk)+", limit $"+fmt2(cap)); };
  require(exposure.events[m.event_ticker],delta,cfg.risk.max_event_frac*bank,"Event");
  require(exposure.clusters[risk_cluster(store,m)],delta,cfg.risk.max_cluster_frac*bank,"Cluster");
  require(exposure.total(),delta,cfg.risk.max_open_frac*bank,"Total exposure");
  require(0.0,cost,std::max(0.0,a->balance-exposure.resting),"Available cash");
}
ExitDecision exit_decision(double count, double held_p, double bid, const FeeModel& fees,
                           bool stale, bool have_model, const ExitPolicy& policy) {
  ExitDecision d;
  count = std::fabs(count);
  d.sell_now = bid > 0 ? bid * count - fees.fee(bid, count, true) : 0.0;
  if (!have_model) {
    d.action = "unpriced";
    d.why = "no model covers this market, so there is nothing to compare the bid against; "
            "holding by default rather than selling on no information";
    return d;
  }
  d.hold_value = held_p * count;
  if (bid <= policy.min_bid) {
    d.action = "no_bid";
    d.why = bid <= 0 ? "nothing is bid: there is no exit here at any price"
                     : "the best bid is " + fmt2(bid) + ", which is not an exit, it is a giveaway";
    return d;
  }
  d.advantage = d.hold_value - d.sell_now;
  d.per_contract = count > 0 ? d.advantage / count : 0;
  if (stale) {
    // A number too old to enter on is also too old to liquidate on. Freeze, do not panic.
    d.action = "stale";
    d.why = "the model number behind this position is stale, so it may not drive a sale either; "
            "additions are stopped and the position is left alone until it refreshes";
    return d;
  }
  std::string basis = "holding is worth " + fmt2(d.hold_value) + " (" + fmt2(held_p) +
                      " x " + fmt2(count) + " contracts), selling now returns " + fmt2(d.sell_now) +
                      " after fees: " + (d.per_contract >= 0 ? "+" : "") + fmt2(d.per_contract) +
                      " a contract in favour of " + (d.per_contract >= 0 ? "holding" : "selling");
  if (d.per_contract <= policy.close_below) {
    d.action = "close";
    d.suggest_count = count;
    d.why = basis + ". The market is paying more than the position is worth, so take it.";
  } else if (d.per_contract <= policy.reduce_below) {
    d.action = "reduce";
    d.suggest_count = floor_contracts(count * policy.reduce_fraction);
    if (d.suggest_count < 0.01) { d.action = "hold"; d.suggest_count = 0; }
    d.why = basis + ". Close to fair, so take part of it off rather than betting the whole "
                    "position on a number that is barely ahead.";
  } else {
    d.action = "hold";
    d.why = basis + ". Selling here would be giving away the edge that is the reason for holding.";
  }
  return d;
}

json risk_book(Store& store,const Config& cfg) {
  auto e=committed_exposure(store);
  json rows=json::array();
  for(const auto& p:store.positions()) {
    if (p.position == 0) continue;
    auto m=store.get_market(p.ticker); double q=std::fabs(p.position),cost=std::fabs(p.exposure);
    bool settling = std::find(e.settling_tickers.begin(), e.settling_tickers.end(), p.ticker) != e.settling_tickers.end();
    double bid=m?(p.position>0?m->yes_bid:m->no_bid):0;
    // bid_value is gross and reads better than the truth: selling costs a taker fee, so what
    // actually comes back is less. "Is this worth taking now" is the question a position page is
    // asked, and it cannot be answered with a number that ignores the cost of the answer.
    double sell_net = bid * q;
    if (m && bid > 0) {
      if (auto sr = store.get_series(m->series_ticker))
        sell_net = bid * q - FeeModel::from_series(*sr).fee(bid, q, true);
    }
    // Why this position is being held, reduced or closed - the comparison, in words, per
    // position. Deliberately not a function of what it cost.
    ExitDecision xd;
    {
      json fr = store.query("SELECT p,ts FROM fair_values WHERE ticker=?", {p.ticker});
      bool have = !fr.empty();
      double fp = have ? fr[0].value("p", 0.0) : 0.0;
      long long fts = have ? fr[0].value("ts", 0LL) : 0;
      double held_p = p.position > 0 ? fp : 1.0 - fp;
      FeeModel fm{"quadratic", 1.0};
      if (m) if (auto sr = store.get_series(m->series_ticker)) fm = FeeModel::from_series(*sr);
      bool stale = have && (fts <= 0 || now_s() - fts > cfg.risk.proposal_ttl_seconds);
      xd = exit_decision(q, held_p, bid, fm, stale, have, ExitPolicy{});
    }
    rows.push_back({{"ticker",p.ticker},{"title",m?m->title:p.ticker},{"side",p.position>0?"yes":"no"},
      {"count",q},{"cost",cost},{"entry",q>0?cost/q:0},{"bid",bid},{"bid_value",bid*q},
      {"sell_net",sell_net},{"sell_pnl",sell_net-cost},
      {"exit_action",xd.action},{"exit_why",xd.why},{"exit_advantage",xd.advantage},
      {"exit_per_contract",xd.per_contract},{"exit_suggest",xd.suggest_count},
      {"cluster",m?risk_cluster(store,*m):p.ticker},{"event",m?m->event_ticker:p.ticker},
      {"exchange_index",m?m->exchange_index:0},{"rules",m?m->rules_primary:""},
      {"close_time",m?m->close_time:""},{"expected_expiration_time",m?m->expected_expiration_time:""},
      {"yes_payout",p.position>0?q:0},{"no_payout",p.position<0?q:0},{"settling",settling}});
  }
  json ev = json::object();
  for (const auto& kv : e.events) {
    const std::string& k = kv.first;
    double c = 0;
    for (const auto& h : e.holdings[k]) c += h.cost;
    ev[k] = json{{"worst_loss", kv.second}, {"worst_state", e.worst_state[k]}, {"cost", c}};
  }
  return json{{"positions",rows},{"filled_cost",e.filled},{"resting_cost",e.resting},{"settling_cost",e.settling},
    {"committed",e.total()},{"events",e.events},{"event_risk",ev},{"clusters",e.clusters},{"daily",daily_equity(store,cfg)},
    {"shards",json::parse(store.kv_get("shard_balances").value_or("[]"),nullptr,false)},
    {"synced_ts",store.kv_get("portfolio_sync_ts").value_or("0")}};
}
}
