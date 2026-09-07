#include "desk/risk.h"
#include "desk/log.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/timeutil.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <stdexcept>
namespace desk {
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
  return rules.find("weather company") == std::string::npos &&
    (rules.find("national weather service") != std::string::npos || rules.find("nws") != std::string::npos);
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
  json meta = store.query("SELECT mutually_exclusive FROM event_meta WHERE event_ticker=?", {event_ticker});
  st.exclusive = !meta.empty() && meta[0].value("mutually_exclusive", 0) == 1;
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
  st.exhaustive = strikes_are_exhaustive(ms);
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

bool stacks_event(Store& store, const std::string& event_ticker, const std::vector<Holding>& existing, const Holding& add) {
  if (event_risk(store, event_ticker, existing).worst_loss <= 1e-9) return false;
  // With verified event structure the question has an exact answer: does the new leg pay in any
  // of the states where the book is already at its worst? If it never does, it is the same bet
  // again (LAX B79.5 YES + LAX T80 NO, 2026-09-05) rather than a hedge.
  std::vector<StatePayoff> states = event_payoffs(store, event_ticker, existing);
  if (!states.empty()) {
    double worst = states[0].loss;
    for (const auto& sp : states) worst = std::max(worst, sp.loss);
    for (const auto& sp : states) {
      if (sp.loss < worst - 1e-9) continue;                       // not a worst state
      bool pays = add.yes ? (sp.ticker == add.ticker) : (sp.ticker != add.ticker);
      if (pays) return false;                                     // it covers a worst state: a hedge
    }
    return true;
  }
  auto losses = market_losses(existing);
  auto it = losses.find(add.ticker);
  if (it == losses.end()) return true;  // overlap with other markets has not been verified
  double yes_loss = it->second.first, no_loss = it->second.second;
  return add.yes ? no_loss >= yes_loss - 1e-9 : yes_loss >= no_loss - 1e-9;
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
json daily_equity(Store& store, const Config& cfg) {
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
  return json{{"start_pv",start},{"pv",equity},{"cash",a?a->balance:0},{"external_flows",flows},{"adjusted_start",adjusted_start},
    {"positions_value",a?a->portfolio_value:0},{"stop_pv",adjusted_start*(1-cfg.risk.daily_loss_stop_frac)},
    {"pnl",start>0?equity-adjusted_start:0},{"account_ts",a?a->updated_ts:0},
    {"day",day},{"day_tz",cfg.risk.day_tz},
    {"basis","Reported cash (including resting reservations) + marked positions vs the first sync of the local day ("+cfg.risk.day_tz+"), adjusted for deposits, withdrawals and transfers to/from the perpetuals instance. Money on the perpetuals instance itself is not tracked here."}};
}
void validate_entry(Store& store,const Config& cfg,const Proposal& p,const Market& m) {
  if (!store.kv_get("uncertain_client_order_id").value_or("").empty()) throw std::runtime_error("An earlier order outcome is uncertain; reconcile it before new entries");
  if (store.kv_get("kill").value_or("0")=="1") throw std::runtime_error("kill switch is on");
  if(!std::isfinite(p.price)||!std::isfinite(p.count)||p.price<=0||p.price>=1||p.count<.01 || std::fabs(p.count-floor_contracts(p.count))>1e-8)
    throw std::runtime_error("Price must be between 0 and 1; quantity must be at least 0.01, in 0.01 increments");
  if(p.outcome_side!="yes"&&p.outcome_side!="no") throw std::runtime_error("Invalid outcome side");
  if(p.exec_style!="maker"&&p.exec_style!="taker") throw std::runtime_error("Invalid execution style");
  if(p.price<cfg.risk.min_price||p.price>1-cfg.risk.min_price) throw std::runtime_error("Entry price is outside configured risk range");
  if(!m.tradable() || (!m.close_time.empty() && parse_iso(m.close_time)<=now_s())) throw std::runtime_error("Market is closed to new orders");
  if(p.strategy=="weather_nws"&&!weather_source_supported(m)) throw std::runtime_error("Weather proposal is research-only: settlement source has not been validated against the NWS model");
  if(p.strategy!="ticket" && now_s()-p.updated_ts>cfg.risk.proposal_ttl_seconds) throw std::runtime_error("Proposal expired; refresh before approving");
  auto a=store.last_account();
  auto sync=store.kv_get("portfolio_sync_ts");
  if(!a||!sync||now_s()-std::stoll(*sync)>60 || now_s()-a->updated_ts>60) throw std::runtime_error("Account data stale or incomplete; sync before trading");
  auto daily=daily_equity(store,cfg);
  if(daily["start_pv"].get<double>()>0 && daily["pv"].get<double>()<=daily["stop_pv"].get<double>()) throw std::runtime_error("Daily equity loss limit reached; new entries blocked");
  auto s=store.get_series(m.series_ticker);
  if(!s) throw std::runtime_error("Fee metadata unavailable; new entry blocked");
  double cost=p.price*p.count+FeeModel::from_series(*s).fee(p.price,p.count,p.exec_style=="taker");
  auto exposure=committed_exposure(store);
  double bank=std::min(cfg.bankroll_dollars,std::max(0.0,a->balance+a->portfolio_value));
  // Payoff-space: the event's risk with this stake added is its worst-case loss across outcomes,
  // so a real hedge gets credit and the same bet twice does not.
  Holding add{m.ticker, p.outcome_side=="yes", p.count, cost, true};
  const auto& existing = exposure.holdings[m.event_ticker];
  if (p.strategy != "ticket" && p.strategy != "close" && stacks_event(store, m.event_ticker, existing, add))
    throw std::runtime_error("Stacks the same bet: this stake loses in the state where " + m.event_ticker +
                             " already loses most, or cross-market overlap is unverified. Tickets can override");
  std::vector<Holding> after = existing;
  after.push_back(add);
  EventRisk before_r = event_risk(store, m.event_ticker, existing), after_r = event_risk(store, m.event_ticker, after);
  double delta = after_r.worst_loss - before_r.worst_loss;
  auto require=[&](double used,double add_risk,double cap,const char* label){ if(used+add_risk>cap+1e-8) throw std::runtime_error(std::string(label)+" cap: at risk $"+fmt2(used)+", this adds $"+fmt2(add_risk)+", limit $"+fmt2(cap)); };
  require(exposure.events[m.event_ticker],delta,cfg.risk.max_event_frac*bank,"Event");
  require(exposure.clusters[risk_cluster(store,m)],delta,cfg.risk.max_cluster_frac*bank,"Cluster");
  require(exposure.total(),delta,cfg.risk.max_open_frac*bank,"Total exposure");
  require(0.0,cost,std::max(0.0,a->balance-exposure.resting),"Available cash");
}
json risk_book(Store& store,const Config& cfg) {
  auto e=committed_exposure(store);
  json rows=json::array();
  for(const auto& p:store.positions()) {
    if (p.position == 0) continue;
    auto m=store.get_market(p.ticker); double q=std::fabs(p.position),cost=std::fabs(p.exposure);
    bool settling = std::find(e.settling_tickers.begin(), e.settling_tickers.end(), p.ticker) != e.settling_tickers.end();
    double bid=m?(p.position>0?m->yes_bid:m->no_bid):0;
    rows.push_back({{"ticker",p.ticker},{"title",m?m->title:p.ticker},{"side",p.position>0?"yes":"no"},
      {"count",q},{"cost",cost},{"entry",q>0?cost/q:0},{"bid",bid},{"bid_value",bid*q},
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
