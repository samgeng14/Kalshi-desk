#include "desk/engine.h"
#include "desk/decision_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "desk/fees.h"
#include "desk/risk.h"
#include "desk/hedge.h"
#include "desk/ladder.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/timeutil.h"
#include "desk/universe.h"
#include "desk/record.h"
#include "desk/reject.h"
#include "desk/route.h"
#include "desk/sports_models.h"
#include "desk/strategies.h"

namespace desk {

Engine::Engine(Config cfg, Store& store, KalshiClient& client, Executor& exec)
    : cfg_(std::move(cfg)), store_(store), client_(client), exec_(exec),
      manual_orders_(store),
      systematic_(store, &exec, cfg_,
                  SystematicTrader::Hooks{[this] { return kill_.load(); },
                                          [this](const std::string& why) { set_kill(true, why); },
                                          [this] { force_reconcile_ = true; },
                                          // A fresh read of one book when the exchange (or the
                                          // executor's pre-flight check) says the book moved.
                                          // Called from step(), under the engine lock.
                                          [this](const std::string& ticker) -> json {
                                            try { return capture_book(ticker); }
                                            catch (const std::exception& e) {
                                              LOG_WARN("book %s: %s", ticker.c_str(), e.what());
                                              return json();
                                            }
                                          }}),
      perps_(cfg_, store, client),
      poly_(cfg_, store,
            make_poly_client(poly_config(cfg_.strategy_params.is_object() && cfg_.strategy_params.contains("polymarket")
                                             ? cfg_.strategy_params["polymarket"] : json::object()), cfg_.user_agent),
            &exec, PolyDesk::Hooks{[this] { return kill_.load(); }, [this] { force_reconcile_ = true; }}), pair_models_(store) {
  for (const auto& s : cfg_.watch_series) watch_.insert(s);
  // The settlement reference for the 15-minute commodity contracts. The socket is only opened
  // when this desk actually watches one of those series: it is an authenticated channel, and a
  // desk that trades none of them has no business holding it open. Without the feed the terminal
  // models are not built at all, so nothing can quietly price them off a missing number.
  {
    const json cparams = cfg_.strategy_params.is_object() && cfg_.strategy_params.contains("commodity")
                             ? cfg_.strategy_params["commodity"] : json::object();
    ReferenceFeedSettings rs = reference_feed_settings(cparams, cfg_.env);
    bool watched = false;
    for (const auto& u : CommodityTerminal::default_underlyings())
      for (const auto& ser : u.series)
        if (watch_.count(ser)) watched = true;
    if (rs.enabled && watched) {
      reference_feed_ = std::make_unique<ReferenceFeed>(store_, rs, client_.signer());
      reference_feed_->start();
      MarketQuoteFeed::Settings qs = market_quote_settings(cparams, cfg_.env);
      if (qs.enabled) {
        quote_feed_ = std::make_unique<MarketQuoteFeed>(store_, qs, client_.signer());
        quote_feed_->start();
      }
    }
  }
  strategies_ = build_strategies(cfg_, reference_feed_.get());
  // Persisted state overrides the config file.
  if (auto k = store_.kv_get("kill")) kill_ = (*k == "1");
  if (auto r = store_.kv_get("kill_reason")) kill_reason_ = *r;
  // A halt raised by a rule that has since been switched off should not outlive the rule. The
  // daily loss stop is the only halt with that property: it fires on a threshold rather than on
  // something going wrong, so once the threshold is disabled there is nothing left to review.
  // Every other kill reason (a rejected order, a hand kill, an unexplained balance) stands until
  // a person clears it, because the thing that caused it has not been switched off by editing a
  // config file. Clearing it here, at startup, rather than in the cycle, keeps this a deliberate
  // consequence of a restart with a changed config and not something that can un-kill a running
  // desk behind you.
  if (kill_ && kill_reason_.rfind("Daily loss stop:", 0) == 0 &&
      !account_stop_policy(store_, cfg_).value("enabled", true)) {
    kill_ = false;
    kill_pending_ = false;
    kill_reason_.clear();
    store_.kv_set("kill", "0");
    store_.kv_set("kill_reason", "");
    const std::string msg = "daily-loss halt cleared at startup: the account daily stop is switched off";
    LOG_WARN("%s", msg.c_str());
    store_.log_event("warn", msg);
  }
  if (auto w = store_.kv_get("watch")) {
    json j = json::parse(*w, nullptr, false);
    if (j.is_array()) {
      watch_.clear();
      for (const auto& s : j) watch_.insert(s.get<std::string>());
    }
  }
  for (auto& st : strategies_) {
    if (auto m = store_.kv_get("mode:" + st->name())) cfg_.strategy_modes[st->name()] = *m;
  }
  {
    std::lock_guard<std::mutex> lk(modes_mu_);
    modes_copy_ = cfg_.strategy_modes;
  }
  // A request from the Trades board names the decision it was reviewed on: check it is still on
  // the board before accepting it (the full revalidation runs again just before it is sent).
  manual_orders_.set_precheck([this](const json& request) { precheck_manual_decision(request); });
  publish_snapshot();
}

// ---- reference data ---------------------------------------------------------------

void Engine::refresh_series() {
  long long now = now_s();
  if (now - last_series_refresh_ < cfg_.series_refresh_minutes * 60 && !series_cache_.empty()) return;
  int n = 0;
  for (const auto& st : watch_) {
    try {
      Series s = client_.get_series_one(st);
      if (s.ticker.empty()) s.ticker = st;
      series_cache_[st] = s;
      store_.upsert_series(s);
      ++n;
    } catch (const std::exception& e) {
      LOG_WARN("series %s: %s", st.c_str(), e.what());
      if (auto s = store_.get_series(st)) series_cache_[st] = *s;
    }
  }
  last_series_refresh_ = now;
  LOG_INFO("series refreshed: %d", n);
}

void Engine::refresh_markets() {
  long long now = now_s();
  int seen = 0;
  for (const auto& st : watch_) {
    std::vector<Market> ms;
    try {
      ms = client_.get_markets(st, "open");
    } catch (const std::exception& e) {
      LOG_WARN("markets %s: %s", st.c_str(), e.what());
      last_error_ = e.what();
      {
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (!market_feeds_.contains(st)) market_feeds_[st] = {{"last_success_ts", 0}, {"open_count", 0}};
        market_feeds_[st]["last_error"] = e.what();
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(snap_mu_);
      market_feeds_[st] = {{"last_success_ts", now}, {"open_count", ms.size()}, {"last_error", ""}};
    }
    for (auto& m : ms) {
      if (m.series_ticker.empty()) m.series_ticker = st;
      store_.upsert_market(m);
      ++seen;
      // Research history: log the top of book when it changes, or at least every 60 s.
      auto lq = last_quote_.find(m.ticker);
      bool changed = lq == last_quote_.end() || lq->second.first != m.yes_bid || lq->second.second != m.yes_ask;
      long long last = last_quote_log_.count(m.ticker) ? last_quote_log_[m.ticker] : 0;
      if ((changed && now - last >= 5) || now - last >= 60) {
        store_.insert_quote(now, m);
        last_quote_log_[m.ticker] = now;
        last_quote_[m.ticker] = {m.yes_bid, m.yes_ask};
      }
    }
  }
  markets_seen_ = seen;
  // Markets that left the open set (settled/closed) get their status refreshed lazily by reconcile.
}

// ---- proposals ---------------------------------------------------------------------

Calibration Engine::calibration_for(const std::string& strategy) const {
  const long long now = now_s();
  {
    std::lock_guard<std::mutex> lk(calib_mu_);
    auto it = calib_.find(strategy);
    if (it != calib_.end() && now - it->second.second <= 900) return it->second.first;
  }
  // The settled-market query runs unlocked; two callers racing here both compute the same answer.
  Calibration c = strategy_calibration(store_, strategy, cfg_.risk.kelly_min_samples);
  std::lock_guard<std::mutex> lk(calib_mu_);
  calib_[strategy] = {c, now};
  return c;
}

std::map<std::string, double> Engine::exposure_by(const std::string& kind, bool include_proposals) {
  auto exposure=committed_exposure(store_);
  auto out=kind=="cluster"?exposure.clusters:exposure.events;
  if(include_proposals) for(const auto& p:store_.proposals("pending",500)) {
    if (is_exit_strategy(p.strategy) || p.strategy == "close") continue;  // reduce risk
    auto m=store_.get_market(p.ticker);
    std::string key=kind=="cluster" && m ? risk_cluster(store_,*m):p.event_ticker;
    out[key]+=p.count*(p.price+p.fee_per);
  }
  return out;
}
double Engine::open_exposure() { return committed_exposure(store_).total(); }

void Engine::run_strategies() {
  long long now = now_s();
  std::vector<Market> markets = store_.open_markets();
  // Only markets in the watch set, and only series whose quotes were refreshed recently: after a
  // failed refresh the stored book is whatever it was, and a fair against a stale book proposes
  // trades that no longer exist (or worse, ones that do, at the wrong price).
  // The desk's one freshness limit for opening risk (Config::data_max_age): the board and the
  // systematic screen judge a quote by the same number, so nothing is offered that this gate
  // would have withdrawn. Managing what is already on (exits, stops, resting entries) keeps its
  // own, looser limit: see board_view.
  long long quote_ttl = cfg_.data_max_age();
  std::vector<std::string> stale_tickers;
  markets.erase(std::remove_if(markets.begin(), markets.end(),
                               [&](const Market& m) {
                                 if (!watch_.count(m.series_ticker)) return true;
                                 bool stale = m.quote_ts > 0 && now - m.quote_ts > quote_ttl;
                                 if (stale) stale_tickers.push_back(m.ticker);
                                 return stale;
                               }),
                markets.end());
  if (!stale_tickers.empty())
    LOG_WARN("%zu markets skipped this cycle: quotes older than %llds", stale_tickers.size(), quote_ttl);
  // Bookkeeping maps are keyed by ticker and would otherwise grow for the life of the process.
  if (++prune_counter_ % 200 == 0) {
    auto prune = [&](auto& m, long long keep_since) {
      for (auto it = m.begin(); it != m.end();) it = it->second < keep_since ? m.erase(it) : std::next(it);
    };
    prune(last_fair_log_, now - 6 * 3600);
    prune(last_quote_log_, now - 6 * 3600);
    for (auto it = latest_fair_ts_.begin(); it != latest_fair_ts_.end();) {
      if (now - it->second > 6 * 3600) {
        latest_fair_.erase(it->first);
        latest_quote_.erase(it->first);
        latest_fair_strategy_.erase(it->first);
        it = latest_fair_ts_.erase(it);
      } else ++it;
    }
    for (auto it = last_quote_.begin(); it != last_quote_.end();)
      it = last_quote_log_.count(it->first) ? std::next(it) : last_quote_.erase(it);
  }
  for (auto& st : strategies_) {
    if (mode_for(st->name()) == "off") continue;  // "research" still collects: prepare() + fair logs
    try {
      st->prepare(markets, store_);
    } catch (const std::exception& e) {
      LOG_WARN("strategy %s prepare: %s", st->name().c_str(), e.what());
    }
  }
  // A proposal you could still click after its market went stale is a price from before the
  // outage. Withdraw it rather than leave it clickable.
  for (const auto& t : stale_tickers)
    for (const char* side : {"yes", "no"})
      for (auto& stp : strategies_)
        if (auto old_p = store_.find_open_proposal(t, side, stp->name()))
          if (old_p->status == "pending") {
            old_p->status = "expired";
            old_p->note = "quotes stale: the book behind this price is older than " + std::to_string(quote_ttl) + "s";
            store_.update_proposal(*old_p);
          }
  fair_set_this_cycle_.clear();
  std::map<std::string, std::vector<Order>> resting;
  for (const auto& o : store_.orders("resting")) resting[o.ticker].push_back(o);
  // Is this order still resting as far as the desk knows? A cancel that failed leaves its row
  // resting (Executor::cancel updates the row only on success), which is what a retry keys off.
  auto still_resting = [&](const std::string& order_id) {
    for (const auto& [tk, os] : resting)
      for (const auto& o : os)
        if (o.order_id == order_id) return true;
    return false;
  };

  for (const auto& m : markets) {
    auto sit = series_cache_.find(m.series_ticker);
    if (sit == series_cache_.end()) continue;
    const Series& s = sit->second;
    for (auto& st : strategies_) {
      std::string mode = mode_for(st->name());
      if (mode == "off") continue;
      if (!st->covers(m, s)) continue;
      std::optional<Fair> f;
      try {
        f = st->fair(m, s, store_);
      } catch (const std::exception& e) {
        LOG_WARN("strategy %s fair(%s): %s", st->name().c_str(), m.ticker.c_str(), e.what());
        continue;
      }
      if (!f) continue;
      // The model's own verdict on its estimate, before the engine's gates below write over it:
      // this is what goes into the ProbabilityQuote.
      const bool model_research_only = f->research_only;
      const std::string model_research_reason = f->research_reason;
      if (f->inputs.contains("valid") && !f->inputs.value("valid", false)) {
        f->research_only = true;
        f->research_reason = "model inputs need review: " + f->inputs.value("reasons", json::array()).dump();
      }
      double raw_p = std::min(0.999, std::max(0.001, f->p));
      f->p_raw = raw_p;
      // Log every raw estimate (traded or not) at most every 5 minutes: this is the Brier sample.
      std::string fk = m.ticker + "|" + st->name();
      if (now - last_fair_log_[fk] >= 300) {
        store_.insert_model_fair(now, m.ticker, st->name(), raw_p, f->sigma, m.yes_bid, m.yes_ask);
        last_fair_log_[fk] = now;
      }
      // Shrink toward the market until the strategy has earned trust: p = (1-w) model + w mid.
      double w = market_weight(st->name());
      f->market_blend_weight = w;
      double mid = m.mid();
      if (w > 0 && mid > 0 && mid < 1) {
        f->p = (1.0 - w) * raw_p + w * mid;
        f->note += " | raw " + fmt2(raw_p) + " mid " + fmt2(mid) + " w=" + fmt2(w);
      } else {
        f->p = raw_p;
      }
      // The first covering strategy governs a ticker's fair (and its exits); later ones only log.
      bool governs = !fair_set_this_cycle_.count(m.ticker);
      if (governs) {
        fair_set_this_cycle_.insert(m.ticker);
        latest_fair_[m.ticker] = *f;
        latest_fair_ts_[m.ticker] = now;
        latest_fair_strategy_[m.ticker] = st->name();
        // The canonical model output: the same number, wrapped. Nothing downstream reads the Fair
        // to decide an entry; the edge engine and the router read this.
        const std::string category = systematic_category(json{{"category", s.category}, {"series_ticker", m.series_ticker}});
        ProbabilityQuote q = quote_from_fair(m, category, st->name(), *f, model_research_only, model_research_reason, now, now);
        if (!st->model_version().empty()) q.model_version = st->model_version();
        if (auto verified = st->settlement_verified(m)) q.settlement_verified = *verified;
        latest_quote_[m.ticker] = std::move(q);
      }
      // Resting orders from our proposals whose price no longer has edge under the current fair.
      // Runs before any gate below so an order never keeps working on a number we have stopped
      // trusting (research-only, mode change, disagreement).
      if (governs) {
        auto rit0 = resting.find(m.ticker);
        if (rit0 != resting.end()) {
          FeeModel fm0 = FeeModel::from_series(s);
          for (const auto& o : rit0->second) {
            long long pid0 = store_.proposal_for_order(o.order_id);
            if (pid0 == 0) continue;  // placed by hand: leave it
            auto pr0 = store_.get_proposal(pid0);
            // A model may cancel only its own unattended entry. A hand-approved signal is
            // manual too; the systematic and pair engines manage their own order IDs.
            if (!pr0 || pr0->strategy != st->name() ||
                (pr0->mode != "auto" && pr0->mode != "bot") ||
                store_.kv_get("manual_order:" + o.order_id).value_or("0") == "1") continue;
            double px = o.outcome_side == "yes" ? o.yes_price : o.no_price;
            double pw = o.outcome_side == "yes" ? f->p : 1.0 - f->p;
            double edge = pw - px - fm0.fee_per_contract(px, false);
            if (edge < -0.005) {  // small hysteresis against the placement threshold
              LOG_INFO("cancel stale %s %s @ %.2f (fair %.3f)", o.ticker.c_str(), o.outcome_side.c_str(), px, f->p);
              try {
                exec_.cancel(o.order_id);
              } catch (const std::exception& e) {
                LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what());
              }
            }
          }
        }
      }
      // Research-only: the number is logged (Brier sample above) but never traded on.
      //  - strategy mode "research"
      //  - the strategy flagged its own estimate (e.g. same-day weather before WX-1c)
      //  - weather markets whose settlement source is not the validated one
      //  - a simple model disagreeing with a liquid market by more than max_disagreement:
      //    on 2026-09-05 every such disagreement was the model being wrong.
      std::string research_reason;
      // Disagreement with the tape. Two-sided book: distance to the mid. One-sided book: the
      // side that exists is still an opinion (bids only = "at least this", asks only = "at most
      // this"), and it is exactly the side a wrong model would trade into.
      double disagreement = 0;
      {
        bool has_bid = m.yes_bid > 0 && m.yes_bid < 1, has_ask = m.yes_ask > 0 && m.yes_ask < 1;
        if (has_bid && has_ask) disagreement = std::fabs(raw_p - mid);
        else if (has_bid) disagreement = std::max(0.0, m.yes_bid - raw_p);
        else if (has_ask) disagreement = std::max(0.0, raw_p - m.yes_ask);
      }
      if (mode == "research") research_reason = "strategy in research mode";
      else if (f->research_only) research_reason = f->research_reason.empty() ? "strategy flagged research-only" : f->research_reason;
      else if (st->name() == "weather_nws" && !weather_source_supported(m)) research_reason = "settlement source not validated";
      else if (st->name() != "manual_fair" && disagreement > cfg_.risk.max_disagreement)
        research_reason = "model/market disagreement " + fmt2(disagreement) + " > " +
                          fmt2(cfg_.risk.max_disagreement) + ": check the model, not the market";
      if (!research_reason.empty()) {
        f->research_only = true;
        f->research_reason = research_reason;
        f->note += " | RESEARCH ONLY: " + research_reason;
        if (governs) latest_fair_[m.ticker] = *f;
        for (const char* side : {"yes", "no"})
          if (auto old = store_.find_open_proposal(m.ticker, side, st->name())) {
            if (old->status == "pending") {
              old->status = "expired";
              old->note = "research only: " + research_reason;
              store_.update_proposal(*old);
            } else if ((old->status == "submitted" || old->status == "cancel_pending") && !old->order_id.empty() &&
                       (old->status == "submitted" || still_resting(old->order_id))) {
              // A live resting order on a number we no longer trust: pull it. A cancel that failed
              // (cancel_pending with the order still resting here) is sent again: Kalshi cancels
              // are idempotent, and leaving it would strand the order until someone noticed.
              try { exec_.cancel(old->order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", old->order_id.c_str(), e.what()); }
            }
          }
        continue;
      }
      if (!governs) continue;  // a second strategy on the same ticker: logged above, never traded
      // Killed: keep researching (fairs are logged above), propose nothing.
      if (kill_) continue;
      // Entries are the decision router's (session 15). The model's number is a ProbabilityQuote
      // (above); the edge engine and the router decide in the cycle, for the Trades board and the
      // algorithm alike, and nothing here proposes an entry. A pending entry proposal left from
      // before is withdrawn so it can be neither approved nor sent.
      for (const char* side : {"yes", "no"})
        if (auto old = store_.find_open_proposal(m.ticker, side, st->name()))
          if (old->status == "pending") {
            old->status = "expired";
            old->note = "withdrawn: model entries now come from the decision router (Trades board or the algorithm)";
            store_.update_proposal(*old);
          }
      break;  // first strategy that covers the market wins
    }
  }
  run_ladder_arb(markets);
  int expired = store_.expire_proposals(now - cfg_.risk.proposal_ttl_seconds);
  if (expired > 0) LOG_INFO("expired %d stale proposals", expired);
}

// Locked baskets in mutually exclusive events (ladder.h). These are not a forecast: the legs
// pay for each other whatever settles, so they are proposed as one basket and go together.
void Engine::run_ladder_arb(const std::vector<Market>& markets) {
  // Not in the config: default to manual. The basket queues for a click; it never sends itself.
  std::string mode = cfg_.strategy_modes.count("ladder_arb") ? cfg_.mode_for("ladder_arb") : "manual";
  if (mode.empty() || mode == "off") return;
  long long now = now_s();
  std::map<std::string, std::vector<const Market*>> by_event;
  for (const auto& m : markets) by_event[m.event_ticker].push_back(&m);
  double bankroll = cfg_.bankroll_dollars;
  if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
  double cash = 0;
  if (auto a = store_.last_account()) cash = std::max(0.0, a->balance - committed_exposure(store_).resting);
  double budget = std::min({cfg_.risk.ladder_max_cost, cash, cfg_.risk.max_open_frac * bankroll});
  int baskets = 0;
  for (const auto& [ev, ms] : by_event) {
    if (baskets >= 2) break;                       // two live baskets at a time is plenty
    if (ms.size() < 2 || budget < 1) break;
    // Already working this event by hand or with another strategy: leave it alone.
    if (!store_.query("SELECT ticker FROM positions WHERE position!=0 AND ticker IN "
                      "(SELECT ticker FROM markets WHERE event_ticker=?)", {ev}).empty()) continue;
    std::optional<LadderArb> arb;
    try {
      // Dominance first: it needs no exclusivity flag, so it is the one that fires on the
      // weather ladders, and its two legs are easier to fill than a whole basket.
      arb = find_dominance_arb(store_, cfg_, ev, ms, budget);
      if (!arb) arb = find_ladder_arb(store_, cfg_, ev, ms, budget);
    } catch (const std::exception& e) {
      LOG_WARN("ladder %s: %s", ev.c_str(), e.what());
      continue;
    }
    if (!arb) continue;
    // One basket per event at a time: if its legs are already queued, refresh nothing and move on.
    std::string basket = "basket " + ev + "@" + std::to_string(now / 60);
    bool already = false;
    for (const auto& p : store_.proposals("pending", 200))
      if (p.event_ticker == ev && p.strategy == "ladder_arb") already = true;
    if (already) continue;
    LOG_INFO("%s", arb->note.c_str());
    for (const auto& leg : arb->legs) {
      Proposal p;
      p.ticker = leg.ticker;
      p.event_ticker = ev;
      auto m = store_.get_market(leg.ticker);
      p.series_ticker = m ? m->series_ticker : "";
      p.strategy = "ladder_arb";
      p.mode = mode;
      p.outcome_side = leg.side;
      p.exec_style = "taker";
      p.status = "pending";
      p.price = leg.price;
      p.count = arb->count;
      p.p_model = leg.side == "no" ? 1.0 - leg.yes_bid : leg.yes_ask;   // the market's own number
      p.sigma = 0.0;
      p.fee_per = leg.fee_per;
      p.net_edge = arb->edge_per_contract / static_cast<double>(arb->legs.size());
      p.yes_bid = leg.yes_bid;
      p.yes_ask = leg.yes_ask;
      p.cluster = m ? risk_cluster(store_, *m) : ev;
      p.note = arb->note + " | " + basket + " | leg " + leg.side + " " + leg.label +
               " | all legs or none: a partial fill leaves a small directional position";
      store_.insert_proposal(p);
    }
    ++baskets;
    budget -= arb->total_cost;
  }
}

void Engine::update_daily_stop() {
  auto d=daily_equity(store_,cfg_);
  // Say so when the balance moved for reasons the desk cannot name. The stop still applies - it
  // is the only brake there is - but a number nobody can explain should never pass silently.
  if (!d.value("cash_explained", true)) {
    std::string msg = "cash does not reconcile: $" + fmt2(d.value("cash_residual", 0.0)) +
                      " of today's balance movement is unexplained; equity and the daily stop are "
                      "computed on it anyway";
    if (last_error_.find("cash does not reconcile") == std::string::npos) {
      LOG_WARN("%s", msg.c_str());
      store_.log_event("warn", msg);
    }
  }
  double off = d.value("untracked_transferred", 0.0);
  if (off > 0.01 && cycle_count_ % 200 == 1)
    LOG_WARN("$%.2f sits on another exchange instance and is not in any equity or risk number here",
             off);
  if(d.value("stop_enabled",true) && d["start_pv"].get<double>()>0 && d["pv"].get<double>()<=d["stop_pv"].get<double>() && !kill_)
    set_kill(true, "Daily loss stop: equity $" + fmt2(d["pv"].get<double>()) + " <= floor $" + fmt2(d["stop_pv"].get<double>()) +
                       " (day start $" + fmt2(d["start_pv"].get<double>()) + ", money moved " + fmt2(d.value("external_flows", 0.0)) + ")");
}

bool Engine::auto_rule_ok(const Proposal& p, std::string* why) const {
  json r = auto_rule_json(p.strategy);
  double min_edge = r.value("min_edge", 0.05), max_cost = r.value("max_cost", 5.0),
         max_sigma = r.value("max_sigma", 0.15);
  int min_minutes = r.value("min_minutes_to_close", 10);
  auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
  if (p.net_edge < min_edge) return fail("edge " + fmt2(p.net_edge) + " < " + fmt2(min_edge));
  double cost = p.count * (p.price + p.fee_per);
  if (cost > max_cost) return fail("cost $" + fmt2(cost) + " > $" + fmt2(max_cost));
  if (p.sigma > max_sigma) return fail("sigma " + fmt2(p.sigma) + " > " + fmt2(max_sigma));
  if (r.contains("styles") && r["styles"].is_array()) {
    bool ok = false;
    for (const auto& st : r["styles"]) if (st == p.exec_style) ok = true;
    if (!ok) return fail("style " + p.exec_style + " not allowed");
  }
  if (auto m = store_.get_market(p.ticker)) {
    long long mins = minutes_to_close(m->close_time, m->expected_expiration_time, now_s());
    if (mins != kUnknownMinutes && mins < min_minutes)
      return fail("closes in " + std::to_string(mins) + " min < " + std::to_string(min_minutes));
    // Liquidity provision only where the book is wide: the auto rule may demand a minimum spread.
    double min_spread = r.value("min_spread", 0.0);
    if (min_spread > 0 && m->yes_bid > 0 && m->yes_ask > 0 && m->yes_ask < 1 && m->yes_ask - m->yes_bid < min_spread - 1e-9)
      return fail("spread " + fmt2(m->yes_ask - m->yes_bid) + " < " + fmt2(min_spread));
  }
  if (why) *why = "auto";
  return true;
}

json Engine::auto_rule_json(const std::string& strategy) const {
  json dflt{{"min_edge", 0.05}, {"max_cost", 5.0}, {"max_sigma", 0.15}, {"min_minutes_to_close", 10},
            {"styles", json::array({"maker", "taker"})}};
  // The arbs and the auto hedger are not forecasts, so the model rule does not describe them: a
  // basket's edge is locked and lives in the basket, not in any one leg (a three-leg lock worth
  // three cents shows up as one cent per leg), and a hedge is bought to remove risk rather than
  // for an edge of its own. Their defaults reflect that; the config still overrides.
  if (strategy == "ladder_arb")
    dflt = json{{"min_edge", 0.0}, {"max_cost", cfg_.risk.ladder_max_cost}, {"max_sigma", 1.0},
                {"min_minutes_to_close", 2}, {"styles", json::array({"taker"})}};
  else if (strategy == "auto_hedge")
    dflt = json{{"min_edge", -1.0}, {"max_cost", cfg_.risk.max_event_frac * cfg_.bankroll_dollars},
                {"max_sigma", 1.0}, {"min_minutes_to_close", 2}, {"styles", json::array({"taker", "maker"})}};
  const json& P = cfg_.strategy_params;
  if (P.is_object() && P.contains(strategy) && P[strategy].is_object() && P[strategy].contains("auto") &&
      P[strategy]["auto"].is_object()) {
    json r = dflt;
    for (auto& [k, v] : P[strategy]["auto"].items()) r[k] = v;
    return r;
  }
  return dflt;
}

// Exits: for every position whose market has a fair from a running strategy, keep one
// reduce-only order working at (our side's fair + min_edge). If the other side of the book is
// already paying more than that, take it. Exit proposals carry the strategy's mode: auto/bot
// send them alone, manual queues them next to the entries.
void Engine::run_exits() {
  long long now = now_s();
  auto positions = store_.positions();
  std::map<std::string, std::vector<Order>> resting;
  for (const auto& o : store_.orders("resting")) resting[o.ticker].push_back(o);

  // Kalshi will not accept reduce_only on a resting order, so the desk has to enforce it. The
  // loop below only visits tickers that still have a position, which means a resting exit whose
  // position has gone - settled, closed by hand, or finished by another route - is never looked
  // at again. If it then filled it would open the opposite position, which is the exact failure
  // the exchange flag existed to prevent. Sweep those first, and any exit now larger than what
  // is left to sell.
  for (const auto& [ticker, orders] : resting) {
    double held = 0;
    for (const auto& pos : positions)
      if (pos.ticker == ticker) held = std::fabs(pos.position);
    for (const auto& o : orders) {
      long long pid = store_.proposal_for_order(o.order_id);
      auto pr = pid ? store_.get_proposal(pid) : std::nullopt;
      if (!pr || !is_exit_strategy(pr->strategy)) continue;
      const bool revoked_model_exit = pr->strategy == "exit" &&
          store_.kv_get("model_exit:" + ticker).value_or("0") != "1";
      if (!revoked_model_exit && o.remaining_count <= held + 1e-9) continue;
      // Say what is being done before doing it. A cancel that fails still leaves an order resting
      // that would open the opposite position, and that has to be visible rather than swallowed.
      store_.log_event("warn", "pulling resting exit on " + ticker + ": " + fmt2(o.remaining_count) +
                                   " resting against " + fmt2(held) +
                                   " held, and a resting order carries no exchange reduce-only flag");
      try {
        exec_.cancel(o.order_id);
      } catch (const std::exception& e) {
        LOG_WARN("could not pull oversized exit %s: %s", o.order_id.c_str(), e.what());
        store_.log_event("error", "resting exit on " + ticker +
                                      " could NOT be pulled and would open the opposite position "
                                      "if it fills: " + e.what());
      }
    }
  }

  std::set<std::string> have_position;
  std::set<std::string> proposed_exits;
  // Positions the systematic trader opened are its to manage: it has its own take-profit, stop,
  // time stop and hold-value rules, and two exit managers on one position would fight.
  const std::set<std::string> algo_owned = systematic_.owned_tickers();
  for (const auto& pos : positions) {
    if (pos.position == 0) continue;
    have_position.insert(pos.ticker);
    if (algo_owned.count(pos.ticker)) continue;
    // Knowing a fair does not confer ownership of an account position. Manual/dashboard/app
    // positions stay manual. The separately configured user brackets still run above.
    if (store_.kv_get("model_exit:" + pos.ticker).value_or("0") != "1") continue;
    auto fit = latest_fair_.find(pos.ticker);
    auto sit = latest_fair_strategy_.find(pos.ticker);
    auto tsit = latest_fair_ts_.find(pos.ticker);
    std::string mode = sit == latest_fair_strategy_.end() ? "off" : mode_for(sit->second);
    bool eligible = fit != latest_fair_.end() && !fit->second.research_only &&
        tsit != latest_fair_ts_.end() && now - tsit->second <= 3 * std::max(1, cfg_.poll_seconds) + 5 &&
        mode != "off" && mode != "research";
    if (!eligible) {
      for (const auto& o : resting[pos.ticker]) {
        auto p = store_.get_proposal(store_.proposal_for_order(o.order_id));
        if (p && p->strategy == "exit") {
          try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel invalidated exit %s: %s", o.order_id.c_str(), e.what()); }
        }
      }
      continue;
    }
    // A dashboard bracket (take-profit / stop) owns this position's exit.
    if (!store_.query("SELECT ticker FROM brackets WHERE ticker=?", {pos.ticker}).empty()) continue;
    // Unrelated resting orders in this market (a partially filled entry, a hand order): the exit
    // would be refused at submit time; skip rather than queue a proposal that cannot go.
    {
      bool foreign = false;
      auto rit0 = resting.find(pos.ticker);
      if (rit0 != resting.end())
        for (const auto& o : rit0->second) {
          long long pid = store_.proposal_for_order(o.order_id);
          auto pr = pid ? store_.get_proposal(pid) : std::nullopt;
          if (!pr || pr->strategy != "exit") foreign = true;
        }
      if (foreign) continue;
    }
    auto m = store_.get_market(pos.ticker);
    if (!m || !m->tradable()) continue;
    auto srs = series_cache_.find(m->series_ticker);
    if (srs == series_cache_.end()) continue;
    FeeModel fm = FeeModel::from_series(srs->second);
    double tick = m->tick > 0 ? m->tick : 0.01;
    bool long_yes = pos.position > 0;
    double p_side = long_yes ? fit->second.p : 1.0 - fit->second.p;   // fair value of what we hold
    double best_bid = long_yes ? m->yes_bid : (m->no_bid > 0 ? m->no_bid : (m->yes_ask > 0 ? 1.0 - m->yes_ask : 0));
    double best_ask = long_yes ? m->yes_ask : (m->no_ask > 0 ? m->no_ask : (m->yes_bid > 0 ? 1.0 - m->yes_bid : 0));
    double n = std::fabs(pos.position);
    double edge = cfg_.risk.min_edge_maker;
    std::string style;
    double sell_px;  // price of our side at which we sell
    if (best_bid > 0 && best_bid - fm.fee_per_contract(best_bid, true) >= p_side + edge) {
      style = "taker";  // someone is paying more than fair + edge for what we hold: take it
      sell_px = best_bid;
    } else {
      style = "maker";
      sell_px = ceil_to_tick(p_side + edge, tick);
      if (best_ask > 0 && sell_px > best_ask) sell_px = best_ask;   // never worse than the current offer
      if (best_bid > 0 && sell_px <= best_bid) sell_px = best_bid + tick;  // post, do not cross
    }
    sell_px = std::max(tick, std::min(1.0 - tick, sell_px));
    // Must clear fair by more than the fee, with a margin above the sweep's cancel threshold.
    if (sell_px - p_side - fm.fee_per_contract(sell_px, style == "taker") <= 0.005) continue;
    // The exit buys the opposite outcome at 1 - sell_px (reduce-only).
    std::string buy_side = long_yes ? "no" : "yes";
    double buy_px = 1.0 - sell_px;
    // Existing exit for this ticker?
    auto existing = store_.find_open_proposal(pos.ticker, buy_side, "exit");
    // A cancel of the last exit that failed or is unresolved is handled like a working exit: while
    // its order still rests it is left alone or pulled again, and only once it is gone is a new
    // exit proposed. It is never stacked behind.
    if (existing && (existing->status == "submitted" || existing->status == "cancel_pending")) {
      // A resting exit: leave it unless the price moved two ticks or more; the stale-order sweep
      // in run_strategies cancels it when the fair passes through it.
      auto rit = resting.find(pos.ticker);
      bool still_resting = false;
      if (rit != resting.end())
        for (const auto& o : rit->second)
          if (store_.proposal_for_order(o.order_id) == existing->id) {
            still_resting = true;
            double resting_sell = long_yes ? o.yes_price : o.no_price;  // our side's price on that order
            if (std::fabs(resting_sell - sell_px) >= 2 * tick - 1e-9 || style == "taker") {
              LOG_INFO("exit %s: re-price %.2f -> %.2f", pos.ticker.c_str(), resting_sell, sell_px);
              try {
                exec_.cancel(o.order_id);
                still_resting = false;
              } catch (const std::exception& e) {
                LOG_WARN("cancel %s: %s (exit left working)", o.order_id.c_str(), e.what());
              }
            }
          }
      if (still_resting) continue;
      existing.reset();  // canceled or gone: propose afresh
    }
    Proposal p = (existing && existing->status == "pending") ? *existing : Proposal{};
    p.ticker = pos.ticker;
    p.event_ticker = m->event_ticker;
    p.series_ticker = m->series_ticker;
    p.strategy = "exit";
    p.mode = mode;
    p.outcome_side = buy_side;
    p.exec_style = style;
    p.status = "pending";
    p.price = buy_px;
    p.count = n;
    p.p_model = fit->second.p;
    p.sigma = fit->second.sigma;
    p.fee_per = fm.fee_per_contract(buy_px, style == "taker");
    p.net_edge = sell_px - p_side - p.fee_per;
    p.yes_bid = m->yes_bid;
    p.yes_ask = m->yes_ask;
    p.cluster = risk_cluster(store_, *m);
    p.updated_ts = now;
    char buf[200];
    std::snprintf(buf, sizeof buf, "exit: sell %s x%.2f @ %.2f (fair %.3f, %s) via %s", long_yes ? "YES" : "NO", n, sell_px,
                  p_side, sit->second.c_str(), style.c_str());
    p.note = buf;
    if (existing) store_.update_proposal(p);
    else { p.ts = now; store_.insert_proposal(p); }
    proposed_exits.insert(pos.ticker);
  }
  // Positions gone: withdraw pending exits, targets and stops.
  for (auto& p : store_.proposals("pending", 200))
    if ((p.strategy == "exit" && !proposed_exits.count(p.ticker)) ||
        (is_exit_strategy(p.strategy) && !have_position.count(p.ticker))) {
      p.status = "expired";
      p.note = have_position.count(p.ticker) ? "model exit no longer eligible" : "position closed";
      store_.update_proposal(p);
    }
}

// Settled results for markets the strategies logged a fair on. The open-markets refresh never
// sees a market again once it closes, so without this the research Brier sample only contained
// markets we traded. A few per cycle keeps the API load flat.
void Engine::refresh_results() {
  long long now = now_s();
  // Closed for at least an hour, no result yet, and a fair was logged: settle them, oldest first.
  // Markets that keep coming back without a result (settlement pending) are retried later via
  // updated_ts, so one slow settlement does not block the rest.
  json rows = store_.query(
      "SELECT ticker FROM markets WHERE (result IS NULL OR result='') AND close_time!='' AND close_time<? "
      "AND updated_ts<? AND ticker IN (SELECT DISTINCT ticker FROM model_fairs UNION SELECT DISTINCT ticker FROM positions "
      "UNION SELECT DISTINCT home_ticker FROM ext_wp) ORDER BY updated_ts LIMIT 10",
      {iso_of(now - 3600), now - 6 * 3600});
  for (const auto& r : rows) {
    std::string t = r.value("ticker", "");
    try {
      Market m = client_.get_market(t);
      store_.upsert_market(m);  // stamps updated_ts = now
      if (!m.result.empty()) results_settled_++;
    } catch (const std::exception& e) {
      LOG_WARN("result %s: %s", t.c_str(), e.what());
      store_.exec("UPDATE markets SET updated_ts=? WHERE ticker=?", {now, t});
    }
  }
}

// Dashboard brackets: a take-profit resting order at your price, a stop that dumps into the bid
// when the market trades through it, and an optional trailing stop that ratchets up with the bid.
// Prices are for the side you hold. "alert" mode only flags the row; "auto" mode executes.
void Engine::run_brackets() {
  long long now = now_s();
  json rows = store_.query("SELECT * FROM brackets", {});
  if (rows.empty()) return;
  auto positions = store_.positions();
  std::map<std::string, std::vector<Order>> resting;
  for (const auto& o : store_.orders("resting")) resting[o.ticker].push_back(o);
  for (const auto& b : rows) {
    std::string ticker = b.value("ticker", "");
    auto pit = std::find_if(positions.begin(), positions.end(), [&](const Position& p) { return p.ticker == ticker; });
    if (pit == positions.end() || pit->position == 0) {
      store_.exec("DELETE FROM brackets WHERE ticker=?", {ticker});
      store_.log_event("info", "bracket " + ticker + " removed: position closed");
      continue;
    }
    auto m = store_.get_market(ticker);
    if (!m) continue;
    auto srs = series_cache_.find(m->series_ticker);
    FeeModel fm = srs != series_cache_.end() ? FeeModel::from_series(srs->second) : FeeModel{};
    double tick = m->tick > 0 ? m->tick : 0.01;
    bool long_yes = pit->position > 0;
    double n = manual_position_quantity(store_, *pit);
    if (n < .01) {
      // A bracket never acquires algorithm lots, even when the manual portion has closed.
      for (const auto& o : resting[ticker]) {
        auto pr = store_.get_proposal(store_.proposal_for_order(o.order_id));
        if (pr && (pr->strategy == "target" || pr->strategy == "stop")) {
          try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel bracket %s: %s", o.order_id.c_str(), e.what()); }
        }
      }
      store_.exec("UPDATE proposals SET status='expired',note='no manual quantity remains' WHERE ticker=? AND status='pending' AND strategy IN ('target','stop')", {ticker});
      continue;
    }
    double bid = long_yes ? m->yes_bid : (m->no_bid > 0 ? m->no_bid : (m->yes_ask > 0 ? 1.0 - m->yes_ask : 0));
    double tp = b.value("tp_price", 0.0), sl = b.value("sl_price", 0.0), trail = b.value("trail", 0.0);
    std::string mode = b.value("mode", "alert");
    // Trailing stop ratchets with the bid, never down.
    if (trail > 0 && bid > 0 && bid - trail > sl) {
      sl = floor_to_tick(bid - trail, tick);
      store_.exec("UPDATE brackets SET sl_price=?, updated_ts=? WHERE ticker=?", {sl, now, ticker});
    }
    bool hit_sl = sl > 0 && bid > 0 && bid <= sl + 1e-9;
    bool hit_tp = tp > 0 && bid > 0 && bid >= tp - 1e-9;
    std::string alerted = b.value("alerted", "");
    if ((hit_sl || hit_tp) && alerted.empty()) {
      store_.exec("UPDATE brackets SET alerted=? WHERE ticker=?", {hit_sl ? "stop" : "target", ticker});
      store_.log_event("warn", "bracket " + ticker + (hit_sl ? " STOP hit: bid " : " TARGET reached: bid ") + fmt2(bid) +
                                   (mode == "auto" ? " (executing)" : " (alert only)"));
    }
    if (mode != "auto" || kill_) continue;
    std::string buy_side = long_yes ? "no" : "yes";
    // Stop: pull the target order, dump into the bid as a taker.
    if (hit_sl) {
      bool blocked = false;
      auto rit = resting.find(ticker);
      if (rit != resting.end())
        for (const auto& o : rit->second) {
          auto pr = store_.get_proposal(store_.proposal_for_order(o.order_id));
          if (!pr || (pr->strategy != "target" && pr->strategy != "stop")) { blocked = true; continue; }
          try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what()); blocked = true; }
        }
      if (blocked) continue;
      store_.exec("UPDATE proposals SET status='expired',note='stop hit' WHERE status='pending' AND ticker=? AND strategy='target'", {ticker});
      if (auto sp = store_.find_open_proposal(ticker, buy_side, "stop"))
        if (sp->status == "pending" || sp->status == "submitted" || sp->status == "cancel_pending") continue;  // already on its way
      Proposal p;
      p.ticker = ticker; p.event_ticker = m->event_ticker; p.series_ticker = m->series_ticker;
      p.strategy = "stop"; p.mode = "bot"; p.outcome_side = buy_side; p.exec_style = "taker"; p.status = "pending";
      p.price = 1.0 - bid; p.count = n; p.fee_per = fm.fee_per_contract(1.0 - bid, true);
      p.yes_bid = m->yes_bid; p.yes_ask = m->yes_ask; p.cluster = risk_cluster(store_, *m); p.ts = now; p.updated_ts = now;
      p.note = "STOP: sell " + std::string(long_yes ? "YES" : "NO") + " x" + fmt2(n) + " into the bid " + fmt2(bid) + " (stop " + fmt2(sl) + ")";
      store_.insert_proposal(p);
      continue;
    }
    // Target: one maker order at tp for the manual portion only.
    if (tp > 0) {
      double tp_px = std::max(tick, std::min(1.0 - tick, tp));
      bool have = false;
      auto rit = resting.find(ticker);
      if (rit != resting.end())
        for (const auto& o : rit->second) {
          long long pid = store_.proposal_for_order(o.order_id);
          auto pr = pid ? store_.get_proposal(pid) : std::nullopt;
          double our_px = long_yes ? o.yes_price : o.no_price;
          if (pr && pr->strategy == "target" && std::fabs(our_px - tp_px) < tick / 2 && std::fabs(o.remaining_count - n) < .009) have = true;
          else if (pr && pr->strategy == "target") {
            try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what()); have = true; }
          } else {
            have = true;  // a foreign order is working in this market; do not stack a second one
          }
        }
      if (have) continue;
      if (auto tp_p = store_.find_open_proposal(ticker, buy_side, "target"))
        if (tp_p->status == "pending") {
          if (std::fabs((1.0 - tp_p->price) - tp_px) > tick / 2 || std::fabs(tp_p->count - n) > 0.01) {
            tp_p->price = 1.0 - tp_px; tp_p->count = n; tp_p->updated_ts = now; store_.update_proposal(*tp_p);
          }
          continue;
        }
      // If the bid is already at or through the target, take it rather than post behind it.
      bool take = hit_tp;
      Proposal p;
      p.ticker = ticker; p.event_ticker = m->event_ticker; p.series_ticker = m->series_ticker;
      p.strategy = "target"; p.mode = "bot"; p.outcome_side = buy_side; p.exec_style = take ? "taker" : "maker"; p.status = "pending";
      double sell = take ? bid : tp_px;
      p.price = 1.0 - sell; p.count = n; p.fee_per = fm.fee_per_contract(1.0 - sell, take);
      p.yes_bid = m->yes_bid; p.yes_ask = m->yes_ask; p.cluster = risk_cluster(store_, *m); p.ts = now; p.updated_ts = now;
      p.note = "TARGET: sell " + std::string(long_yes ? "YES" : "NO") + " x" + fmt2(n) + " @ " + fmt2(sell) + (take ? " (taking the bid)" : " (resting)");
      store_.insert_proposal(p);
    }
  }
}

double Engine::auto_spent_today() {
  auto v = store_.kv_get("auto_spent:" + trading_day(cfg_.risk.day_tz, now_s()));
  return v ? std::atof(v->c_str()) : 0.0;
}

// The same day's unattended spending, split by Kalshi category and by exchange shard, so one
// bucket running hot cannot spend the whole allowance.
double Engine::auto_spent_bucket(const std::string& key) {
  auto v = store_.kv_get("auto_spent:" + trading_day(cfg_.risk.day_tz, now_s()) + ":" + key);
  return v ? std::atof(v->c_str()) : 0.0;
}
void Engine::add_auto_spent_bucket(const std::string& key, double amount) {
  std::string k = "auto_spent:" + trading_day(cfg_.risk.day_tz, now_s()) + ":" + key;
  auto v = store_.kv_get(k);
  store_.kv_set(k, std::to_string((v ? std::atof(v->c_str()) : 0.0) + amount));
}
std::pair<std::string, int> Engine::bucket_of(const Proposal& p) {
  std::string category;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    auto it = snap_series_.find(p.series_ticker);
    if (it != snap_series_.end()) category = it->second.category;
  }
  if (category.empty())
    if (auto s = store_.get_series(p.series_ticker)) category = s->category;
  int shard = 0;
  if (auto m = store_.get_market(p.ticker)) shard = m->exchange_index;
  return {category, shard};
}
json Engine::auto_budget_view() {
  json cats = json::array(), shards = json::array();
  for (const auto& [k, v] : cfg_.risk.auto_budget_category)
    cats.push_back({{"key", k}, {"budget", v}, {"spent", auto_spent_bucket("cat:" + k)}});
  for (const auto& [k, v] : cfg_.risk.auto_budget_shard)
    shards.push_back({{"key", k}, {"budget", v}, {"spent", auto_spent_bucket("shard:" + std::to_string(k))}});
  return json{{"total_budget", cfg_.risk.max_auto_dollars_per_day},
              {"total_spent", auto_spent_today()},
              {"by_category", cats},
              {"by_shard", shards}};
}

void Engine::auto_submit() {
  if (kill_) return;
  if (!exec_.armed()) return;
  int n = 0;
  double spent = auto_spent_today();
  const long long started = now_s();
  for (auto it = soft_backoff_.begin(); it != soft_backoff_.end();)
    it = it->second <= started ? soft_backoff_.erase(it) : std::next(it);
  for (auto& p : store_.proposals("pending", 200)) {
    if (n >= cfg_.risk.max_orders_per_cycle) break;
    // A qualified addition is visible so the user can review the extra concentration. The
    // instant lane must not turn that review row back into an unattended order.
    if (p.approval_required) continue;
    std::string why;
    bool reduces = is_exit_strategy(p.strategy);
    // A model's ENTRY never goes from here (session 15): the decision router owns execution
    // eligibility, AUTO decisions are the algorithm's and MANUAL ones are yours on the Trades
    // board. Exits, brackets, the auto hedge and locked baskets are unchanged.
    if (!reduces && std::any_of(strategies_.begin(), strategies_.end(),
                                [&](const std::unique_ptr<Strategy>& st) { return st->name() == p.strategy; }))
      continue;
    // The market just refused (or would have refused) an entry on this ticker: give entries a
    // breather. Exits, targets and stops on the same ticker are never held back by it.
    if (!reduces && soft_backoff_.count(p.ticker)) continue;
    // The fast lane: an edge this big is gone by the time a human confirms it, so it goes now.
    // Mode still decides whether the strategy may trade at all - off and research never send -
    // but manual no longer means "wait" once the edge clears the bar.
    double lane = instant_edge();
    bool instant = !reduces && lane > 0 && p.net_edge >= lane - 1e-9 && (p.mode == "auto" || p.mode == "bot");
    if (p.strategy == "auto_hedge" && store_.kv_get("auto_hedge:"+p.event_ticker).value_or("0") != "1") continue;
    if (instant && p.count * (p.price + p.fee_per) > cfg_.risk.instant_max_cost + 1e-9) {
      // Too big to send unattended: cut it to the instant cap rather than skipping the edge.
      double per = p.price + p.fee_per;
      double n = per > 0 ? std::floor(cfg_.risk.instant_max_cost / per) : 0;
      if (n < 1) instant = false;
      else if (n < p.count) { p.count = n; store_.update_proposal(p); }
    }
    if (reduces) { if (p.mode != "bot" && p.mode != "auto") continue; why = p.mode + " " + p.strategy; }
    else if (p.mode == "bot") why = "bot";
    else if (p.mode == "auto") { if (!auto_rule_ok(p, &why) && !instant) continue; if (instant) why = "instant"; }
    else if (instant) why = "instant";
    else continue;
    if (instant) why = "INSTANT: edge $" + fmt2(p.net_edge) + " >= $" + fmt2(lane) + " (" + p.mode + " mode)";
    bool bracket = p.strategy == "target" || p.strategy == "stop";
    if (bracket) {
      auto settings = store_.query("SELECT mode FROM brackets WHERE ticker=?", {p.ticker});
      if (settings.empty() || settings[0].value("mode", "alert") != "auto") continue;
    }
    if (!auto_armed() && !bracket) continue;  // explicit user brackets are independent of the model switch
    if (p.strategy == "exit") {
      if (store_.kv_get("model_exit:" + p.ticker).value_or("0") != "1") continue;
      auto source = latest_fair_strategy_.find(p.ticker);
      auto fair = latest_fair_.find(p.ticker);
      if (source == latest_fair_strategy_.end() || fair == latest_fair_.end() || fair->second.research_only) continue;
      std::string current = mode_for(source->second);
      if (current != "auto" && current != "bot") continue;
    } else if (!bracket && !instant) {
      std::string current = mode_for(p.strategy);
      if (current != "auto" && current != "bot") continue;
    }
    double cost = p.count * (p.price + p.fee_per);
    std::string over;
    if (!reduces) {
      if (spent + cost > cfg_.risk.max_auto_dollars_per_day + 1e-9)
        over = "auto budget: $" + fmt2(spent) + " of $" + fmt2(cfg_.risk.max_auto_dollars_per_day) + " used today";
      auto [category, shard] = bucket_of(p);
      auto cit = cfg_.risk.auto_budget_category.find(category);
      if (over.empty() && cit != cfg_.risk.auto_budget_category.end()) {
        double used = auto_spent_bucket("cat:" + category);
        if (used + cost > cit->second + 1e-9)
          over = "auto budget for " + category + ": $" + fmt2(used) + " of $" + fmt2(cit->second) + " used today";
      }
      auto sit2 = cfg_.risk.auto_budget_shard.find(shard);
      if (over.empty() && sit2 != cfg_.risk.auto_budget_shard.end()) {
        double used = auto_spent_bucket("shard:" + std::to_string(shard));
        if (used + cost > sit2->second + 1e-9)
          over = "auto budget for shard " + std::to_string(shard) + ": $" + fmt2(used) + " of $" +
                 fmt2(sit2->second) + " used today";
      }
    }
    if (!over.empty()) {
      if (p.note.find("auto budget") == std::string::npos) {
        p.note += " | " + over + "; waiting for you";
        store_.update_proposal(p);
      }
      continue;
    }
    try {
      p.note = p.note.empty() ? why : p.note + " | " + why;
      if (reduces) exec_.submit_exit(p); else exec_.submit(p);
      ++n;
      if (!reduces) {
        spent += cost;
        store_.kv_set("auto_spent:" + trading_day(cfg_.risk.day_tz, now_s()), std::to_string(spent));
        auto [category, shard] = bucket_of(p);
        if (!category.empty()) add_auto_spent_bucket("cat:" + category, cost);
        add_auto_spent_bucket("shard:" + std::to_string(shard), cost);
      }
    } catch (const ApiError& e) {
      LOG_ERR("%s submit %s: api %d %s", why.c_str(), p.ticker.c_str(), e.status, e.body.substr(0, 160).c_str());
      last_error_ = e.what();
      // A refused ENTRY means the desk and the exchange disagree about what is allowed, and
      // nothing more should go out until somebody looks. A refused EXIT is the opposite case:
      // killing the desk there leaves the position open and stops the machinery that was trying
      // to close it. On 2026-09-06 and 07 a malformed exit did exactly that four times. Mark it
      // failed, keep going, and let the operator see it.
      if (reduces) {
        p.status = "failed";
        p.note += " | exit refused: " + e.body.substr(0, 120);
        store_.update_proposal(p);
        store_.log_event("error", "exit refused for " + p.ticker + " and the position is still open: " +
                                      e.body.substr(0, 160));
        continue;
      }
      // Only a refusal that says the desk itself is wrong stops the desk (reject.h). A post-only
      // price the book moved through, a closed market or a short shard is the market moving: the
      // proposal is spent (the executor marked it failed) and the next cycle re-prices from a
      // fresh book.
      const RejectClass cls = classify_rejection(e.status, e.body);
      if (reject_trips_kill(cls)) {
        set_kill(true, std::string("order rejected (") + reject_class_name(cls) + "): " + e.body.substr(0, 160));
        break;
      }
      if (cls == RejectClass::Retryable) break;   // 429 / server trouble: stop sending this cycle
      store_.log_event("warn", "not sent (" + std::string(reject_class_name(cls)) + ") " + p.ticker + " " +
                                   p.outcome_side + ": " + e.body.substr(0, 120) + "; re-priced after a breather");
      ++n;                                        // it was an order write; it counts
      soft_backoff_[p.ticker] = now_s() + 30;
      continue;
    } catch (const SoftRejection& e) {
      // Refused before sending because the book has already moved (a post-only price that would
      // cross). Nothing went out; the pending proposal is re-priced in place next cycle.
      LOG_INFO("%s submit %s not sent: %s", why.c_str(), p.ticker.c_str(), e.what());
      if (p.note.find("not sent:") == std::string::npos) {
        p.note += std::string(" | not sent: ") + e.what();
        store_.update_proposal(p);
      }
      if (!reduces) soft_backoff_[p.ticker] = now_s() + 30;
      continue;
    } catch (const std::exception& e) {
      // Pre-flight refusals (caps, stale account, another order resting, catching up) are not
      // exchange rejections: log, leave the proposal queued, move on.
      LOG_WARN("%s submit %s not sent: %s", why.c_str(), p.ticker.c_str(), e.what());
      last_error_ = e.what();
      if (p.note.find("not sent:") == std::string::npos) {
        p.note += std::string(" | not sent: ") + e.what();
        store_.update_proposal(p);
      }
      break;
    }
  }
}

void Engine::publish_snapshot() {
  json strategies = json::array();
  for (const auto& st : strategies_)
    strategies.push_back({{"name", st->name()},
                          {"category", st->category()},
                          {"mode", mode_for(st->name())},
                          {"market_weight", market_weight(st->name())},
                          {"auto_rule", auto_rule_json(st->name())},
                          {"describe", st->describe()}});
  auto acct = store_.last_account();
  // Freshness per source, as raw timestamps: the server stamps `now` when the page asks, so the
  // dashboard subtracts and gets a real age. Publishing an age computed here would freeze at
  // whatever it was on the last cycle, which is exactly the reading you must not trust - on
  // 2026-09-07 this desk sat dead for nine and a half hours behind an ordinary-looking header.
  auto newest = [&](const char* table) -> long long {
    json r = store_.query(std::string("SELECT MAX(ts) AS t FROM ") + table);
    return r.empty() || r[0]["t"].is_null() ? 0 : r[0].value("t", 0LL);
  };
  long long model_ts = 0;
  for (const auto& [tk, ts] : latest_fair_ts_) model_ts = std::max(model_ts, ts);
  json status{{"env", cfg_.env},
              {"discovery_ts", last_universe_refresh_},
              {"model_ts", model_ts},
              {"prices_ts", newest("ext_prices")},
              {"weather_ts", newest("ext_weather")},
              {"sports_ts", newest("ext_sports")},
              {"armed", exec_.armed()},
              {"kill", kill_.load()},
              {"kill_reason", kill_reason_},
              {"cycle", cycle_count_},
              {"last_cycle_ts", last_cycle_ts_},
              {"last_error", last_error_},
              {"markets_seen", markets_seen_},
              {"bankroll", cfg_.bankroll_dollars},
              {"watch", watch()},
              {"strategies", strategies},
              {"balance", acct ? acct->balance : 0.0},
              {"portfolio_value", acct ? acct->portfolio_value : 0.0},
              {"account_ts", acct ? acct->updated_ts : 0},
              {"perps", perps_.summary()},
              {"polymarket", poly_.summary()}};
  // The settlement reference, named in the header like any other data source: a commodity model
  // priced off a socket that died twenty minutes ago is the failure this line exists to show.
  if (reference_feed_) status["reference"] = reference_feed_->status();
  if (quote_feed_) status["commodity_quotes"] = quote_feed_->status();
  // Where each commodity model stands economically, from what has actually been recorded. Nothing
  // here promotes anything: the two trading states are set by hand and by nothing else.
  if (reference_feed_) {
    json models = json::array();
    for (const auto& st2 : strategies_)
      if (st2->name().rfind("commodity_terminal", 0) == 0)
        models.push_back(commodity_model_status(store_, st2->name()));
    status["commodity_models"] = models;
  }
  std::lock_guard<std::mutex> lk(snap_mu_);
  snap_fair_ = latest_fair_;
  snap_quotes_ = latest_quote_;
  snap_research_books_ = research_books_;
  snap_fair_strategy_ = latest_fair_strategy_;
  snap_fair_ts_ = latest_fair_ts_;
  snap_series_ = series_cache_;
  snap_watch_ = watch_;
  snap_strategies_ = strategies;
  snap_status_ = status;
}

void Engine::process_manual_orders() {
  auto lookup=[&](const std::string& id)->json {
    auto orders=store_.query("SELECT order_id,fill_count,remaining_count,proposal_id FROM orders WHERE client_order_id=?",{id});
    if(!orders.empty()) {auto result=orders[0];result["status"]="submitted";return result;}
    return json::parse(store_.kv_get("manual_attempt:"+id).value_or("{}"));
  };
  manual_orders_.process([&](const json& b,const std::string& id,long long deadline) {
    OrderRequest req;req.ticker=b.value("ticker","");req.outcome_side=b.value("outcome_side","yes");
    req.price=b.value("price",0.);req.count=b.value("count",0.);req.post_only=b.value("post_only",true);
    req.tif=req.post_only?"good_till_canceled":"immediate_or_cancel";req.client_order_id=id;
    // A request reviewed on a Trades board decision is revalidated against the desk as it is now,
    // never sent from the browser's snapshot. It throws with what changed.
    json fresh;
    if(b.contains("decision_id")) fresh=revalidate_manual_decision(b,id);
    try {
      auto p=exec_.ticket(req,b.value("note","manual queued order"),deadline,fresh.is_object()?fresh.value("decision_id",""):"");
      force_reconcile_=true;return p.to_json();
    } catch(const RetryManualOrder&) {
      throw;
    } catch(const std::exception& e) {
      const std::string error=e.what();
      if(!store_.kv_get("manual_attempt:"+id) && error.find("account data has not caught up")!=std::string::npos)
        throw RetryManualOrder("Waiting for the exchange account snapshot; no order sent yet");
      // A final refusal from the shared risk checks or the exchange: say so on the decision. An
      // order whose delivery is not known (a 5xx, a timeout) is "unknown", not refused.
      if(fresh.is_object()) {
        const json attempt=json::parse(store_.kv_get("manual_attempt:"+id).value_or("{}"),nullptr,false);
        const std::string state=attempt.is_object()&&attempt.contains("status")&&attempt["status"].is_string()?attempt["status"].get<std::string>():"";
        record_decision(store_,fresh,(state=="unknown"||state=="sending")?"unknown":"rejected",now_s(),0,
                        "manual request "+id+": "+error,req.count,req.price,req.post_only?"maker":"taker");
      }
      throw;
    }
  },lookup,now_s());
}

json Engine::sports_models_view() {
  json out=json::array();std::set<std::string> seen;const auto now=now_s();
  for(const auto& w:store_.query("SELECT * FROM ext_wp WHERE ts>=? ORDER BY ts DESC LIMIT 300",{now-90})){
    const auto event=w.value("event_ticker","");if(!seen.insert(event).second)continue;
    const auto home=w.value("home_ticker",""),away=w.value("away_ticker","");
    auto market=store_.get_market(home);if(!market)continue;
    auto lg=SportsLive::default_leagues().find(market->series_ticker);
    if(lg==SportsLive::default_leagues().end()||!lg->second.leadtime||w.value("state","")!="in"||w.value("period",0)>lg->second.periods)continue;
    double left=SportsLive::frac_left(lg->second,w.value("period",0),w.value("clock",""),w.value("state",""));
    double wp=w.value("wp_home",-1.);if(wp<0||wp>1)continue;
    for(bool is_home:{true,false}){
      const auto ticker=is_home?home:away;auto m=store_.get_market(ticker);if(!m)continue;
      auto prior=store_.query(is_home?"SELECT MAX(mkt_home) AS earlier FROM ext_wp WHERE event_ticker=? AND ts>=? AND ts<=?":"SELECT MAX(mkt_away) AS earlier FROM ext_wp WHERE event_ticker=? AND ts>=? AND ts<=?",{event,now-600,now-60});
      if(prior.empty()||!prior[0]["earlier"].is_number())continue;
      auto sr=store_.get_series(m->series_ticker);if(!sr)continue;
      auto c=sports_comeback_candidate(m->yes_ask,prior[0]["earlier"].get<double>(),is_home?wp:1-wp,FeeModel::from_series(*sr).fee(m->yes_ask,1,true),left,
        w.value("ts",0LL)<=now&&m->quote_ts<=now&&now-m->quote_ts<=30&&m->yes_ask_size>=1&&m->yes_bid>0&&m->yes_ask>m->yes_bid);
      c["ticker"]=ticker;c["fraction_left"]=left;c["price"]=m->yes_ask;c["per_trade_dollars"]=2.;out.push_back(c);
    }
  }
  return json{{"candidates",out},{"live_enabled",false},{"notice","Comeback watch uses current ESPN win probability and a recorded dip. It does not predict the probability of doubling your exit price. Baseball/hockey need sport-specific remaining-opportunity models; they are not passed through a clock model."}};
}

void Engine::step_pair_models() {
  const auto lab=pair_models_.view();
  if(!lab.value("running",false)&&lab.value("positions",json::array()).empty())return;
  auto rows=poly_.pairs(now_s());json chosen=json::array();std::set<std::string> wanted;
  // Open paper legs always receive exit quotes. New research is bounded to four additional games.
  for(const auto& p:lab["positions"])wanted.insert(p.value("key",""));
  int extra=0;std::set<std::string> events;
  std::map<std::string,json> depth;
  for(auto r:rows){
    const auto ticker=r.value("kalshi_ticker",""),slug=r.value("slug",""),event=r.value("kalshi_event","");
    const bool held=wanted.count(ticker+"|"+slug)>0;
    if(!held&&(extra>=4||events.count(event)))continue;
    events.insert(event);if(!held)++extra;
    try{if(!depth.count(slug)){auto b=poly_.book(slug);b["ts"]=now_s();depth[slug]=b;}
      const auto& b=depth[slug];bool same=r.value("pm_yes_is_kalshi_yes",true);
      r["pm_bid"]=same?b.value("best_bid",0.):1-b.value("best_ask",0.);
      r["pm_ask"]=same?b.value("best_ask",0.):1-b.value("best_bid",0.);
      r["pm_bid_size"]=b.value(same?"bid_size":"ask_size",0.);r["pm_ask_size"]=b.value(same?"ask_size":"bid_size",0.);r["pm_depth_ts"]=b["ts"];
    }catch(const std::exception&){r["pm_depth_ts"]=0;}
    if(auto km=store_.get_market(ticker)){if(auto series=store_.get_series(km->series_ticker)){r["fee_type"]=series->fee_type;r["fee_multiplier"]=series->fee_multiplier;}}
    r["regime"]="";const auto now=now_s();
    const auto wp=store_.query("SELECT * FROM ext_wp WHERE event_ticker=? ORDER BY ts DESC LIMIT 1",{event});
    const auto soccer=store_.query("SELECT * FROM match_state WHERE event_ticker=?",{event});
    if(!wp.empty()&&now-wp[0].value("ts",0LL)<=90&&wp[0].value("ts",0LL)<=now){
      const auto& w=wp[0];r["regime"]=w.value("espn_id","")+":"+w.value("state","")+":"+w["period"].dump()+":"+w["home_score"].dump()+":"+w["away_score"].dump();
    }else if(!soccer.empty()&&now-soccer[0].value("data_ts",0LL)<=90&&soccer[0].value("data_ts",0LL)<=now){
      const auto& w=soccer[0];r["regime"]=w.value("espn_id","")+":"+w.value("phase","")+":"+w["home_score"].dump()+":"+w["away_score"].dump()+":"+w["red_home"].dump()+":"+w["red_away"].dump();
    }else if(r.value("start_ts",0LL)>now)r["regime"]="pregame:"+std::to_string(r.value("start_ts",0LL));
    chosen.push_back(r);process_manual_orders();
  }
  const auto sys=systematic_.state();pair_models_.step(chosen,now_s(),sys.value("settings",json::object()).value("capital_budget_dollars",40.));
}

void Engine::cycle() {
  std::lock_guard<std::mutex> lk(mu_);
  ++cycle_count_;
  apply_kill_sweep();
  process_manual_orders();
  bool ok = true;
  auto step = [&](const char* name, auto&& fn) {
    try { fn(); } catch (const std::exception& e) {
      LOG_ERR("cycle %s: %s", name, e.what());
      last_error_ = std::string(name) + ": " + e.what();
      ok = false;
    }
    process_manual_orders();
  };
  // The reference socket collects on its own thread and never touches the store; this is the
  // only writer. First in the cycle so a slow cycle does not let the pending queue grow.
  if (reference_feed_) step("reference", [&] { reference_feed_->flush(now_s()); });
  if (quote_feed_) step("quotes", [&] {
    // Follow the windows that are open now. They roll every fifteen minutes, so the set is sent
    // as a difference rather than by reconnecting, and a window that has closed is dropped as
    // soon as it does: the study is about the live book, and an expired market has none.
    std::set<std::string> series;
    for (const auto& u : CommodityTerminal::default_underlyings())
      for (const auto& ser : u.series) series.insert(ser);
    std::vector<std::string> tickers;
    const long long t = now_s();
    for (const auto& m : store_.open_markets("")) {
      if (!series.count(m.series_ticker) || !m.tradable()) continue;
      const long long close = parse_iso(m.close_time);
      if (close > 0 && close <= t) continue;
      tickers.push_back(m.ticker);
    }
    quote_feed_->watch(tickers);
    quote_feed_->flush(t);
  });
  step("refresh", [&] { refresh_series(); refresh_markets(); refresh_event_meta(); });
  step("universe", [&] { refresh_universe(); });
  if ((cycle_count_ - 1) % std::max(1, cfg_.reconcile_every) == 0 || force_reconcile_.exchange(false))
    step("reconcile", [&] { exec_.reconcile(cycle_count_ == 1); update_daily_stop(); });
  // The margined instance: marks and the index every few seconds, the account on its own cadence
  // (and right after anything was sent there), then the basis rule.
  step("perps", [&] { perps_.refresh_public(now_s()); perps_.refresh_account(now_s()); perps_.step(now_s()); });
  // Polymarket: the league universe on its own slow clock, quotes for matched markets every few
  // seconds, the account, then the Kalshi<->Polymarket pairs the strategies read this cycle.
  step("polymarket", [&] { poly_.refresh_universe(now_s()); poly_.match(now_s()); poly_.refresh_quotes(now_s()); poly_.refresh_account(now_s()); });
  step("strategies", [&] { run_strategies(); });
  // Strategies first: research-only NBA/weather fairs can nominate liquid books for capture in
  // this same cycle. Previously only held or already-proposed contracts had depth history.
  step("books", [&] { record_books(); });
  if (cycle_count_ % 2 == 0) step("results", [&] { refresh_results(); });
  step("brackets", [&] { run_brackets(); });
  step("auto hedge", [&] { run_auto_hedge(); });
  step("exits", [&] { run_exits(); });
  // The systematic trader reads the published snapshot (fairs, books), so publish this cycle's
  // numbers before it runs, and let it send before the queue is worked so a locked basket it
  // owns is not sent twice.
  publish_snapshot();
  // One decision set a cycle: quotes -> edges -> routes. The same rows go to the Trades board
  // (MANUAL, and AUTO marked), to the research record (RESEARCH) and to the algorithm (AUTO).
  step("systematic", [&] {
    const long long t = now_s();
    json board = research_board();
    const RouterContext ctx = router_context(t);
    json screen = systematic_screen(board, systematic_.settings_copy(), ctx);
    const json st = systematic_.state();
    // `running` is what the operator ASKED FOR. A halted engine still reports running=true here,
    // which is how a screen could read RUNNING while nothing was being sent. Both facts are now
    // published so the UI can show the difference instead of guessing: requested_running is the
    // switch, halted is the engine's own state, and `running` stays as it was for compatibility.
    const bool requested = st.contains("running") && st["running"].is_boolean() && st["running"].get<bool>();
    const bool halted = st.contains("halted") && st["halted"].is_boolean() && st["halted"].get<bool>();
    screen["algorithm"] = {{"running", requested},
                           {"requested_running", requested},
                           {"halted", halted},
                           {"halt_reason", st.contains("halt_reason") && st["halt_reason"].is_string()
                                               ? st["halt_reason"].get<std::string>() : std::string()},
                           {"halt_class", st.contains("halt_class") && st["halt_class"].is_string()
                                              ? st["halt_class"].get<std::string>() : std::string()},
                           {"actually_running", requested && !halted},
                           {"mode", st.contains("mode") && st["mode"].is_string() ? st["mode"].get<std::string>() : "paper"},
                           {"auto_armed", ctx.auto_armed},
                           {"auto_off_reason", ctx.auto_armed ? std::string() : ctx.auto_off_reason}};
    screen["market_data_max_age_seconds"] = cfg_.data_max_age();
    publish_decisions(screen, t);
    systematic_.step(board, screen, t);
  });
  step("pairs", [&] { poly_.step(now_s()); });
  step("pair model lab", [&] { step_pair_models(); });
  step("auto", [&] { auto_submit(); });
  apply_kill_sweep();
  if (ok) last_error_.clear();
  last_cycle_ts_ = now_s();
  publish_snapshot();
}

// ---- operations --------------------------------------------------------------------

void Engine::request_kill(const std::string& reason) {
  // No engine lock: the flag stops every send path at once; the sweep follows in the cycle.
  kill_ = true;
  kill_pending_ = true;
  kill_reason_ = reason;
  store_.kv_set("kill", "1");
  store_.kv_set("kill_reason", reason);
  LOG_WARN("kill switch ON (requested) %s", reason.c_str());
  store_.log_event("error", "KILL: " + reason);
  // Best effort right now if nobody holds the cycle lock.
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (lk.owns_lock()) apply_kill_sweep();
}

void Engine::apply_kill_sweep() {
  if (!kill_pending_.exchange(false)) return;
  for (const auto& o : store_.orders("resting")) {
    try {
      exec_.cancel(o.order_id);
    } catch (const std::exception& e) {
      LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what());
    }
  }
  store_.exec("UPDATE proposals SET status='expired',note='kill' WHERE status='pending'");
  try {
    perps_.cancel_all("kill switch");
  } catch (const std::exception& e) {
    LOG_WARN("perps kill sweep: %s", e.what());
  }
  try {
    poly_.cancel_all("kill switch");
  } catch (const std::exception& e) {
    LOG_WARN("polymarket kill sweep: %s", e.what());
  }
  publish_snapshot();
}

void Engine::set_kill(bool on, const std::string& reason) {
  kill_ = on;
  kill_reason_ = on ? reason : "";
  store_.kv_set("kill", on ? "1" : "0");
  store_.kv_set("kill_reason", kill_reason_);
  LOG_WARN("kill switch %s %s", on ? "ON" : "off", reason.c_str());
  store_.log_event(on ? "error" : "info", on ? "KILL: " + reason : "re-armed");
  if (on) {
    kill_pending_ = true;
    apply_kill_sweep();
  }
  publish_snapshot();
}

bool Engine::auto_armed() const { return store_.kv_get("auto_armed").value_or("0") == "1"; }

// The fast-lane threshold, persisted so a restart keeps it. The config value is the default.
double Engine::instant_edge() const {
  auto v = store_.kv_get("instant_edge");
  return v ? std::atof(v->c_str()) : cfg_.risk.instant_edge;
}
void Engine::set_instant_edge(double edge) {
  if (edge < 0 || edge > 1) throw std::runtime_error("instant edge must be between 0 and 1");
  store_.kv_set("instant_edge", fmt2(edge));
  store_.log_event("warn", edge > 0 ? "instant lane set: proposals with a net edge of $" + fmt2(edge) +
                                          " or more send themselves the moment they appear"
                                    : "instant lane off: every model proposal waits for you");
  LOG_WARN("instant edge %.2f", edge);
}

void Engine::set_auto_armed(bool on) {
  store_.kv_set("auto_armed", on ? "1" : "0");
  store_.log_event("warn", on ? "AUTO ARMED: auto/bot proposals send without you" : "auto disarmed: model entries and exits queue; enabled user brackets remain active");
  LOG_WARN("auto %s", on ? "ARMED" : "disarmed");
}

void Engine::set_mode(const std::string& strategy, const std::string& mode) {
  if (mode != "off" && mode != "research" && mode != "manual" && mode != "auto" && mode != "bot")
    throw std::runtime_error("bad mode " + mode);
  store_.log_event("info", strategy + " mode -> " + mode);
  cfg_.strategy_modes[strategy] = mode;
  {
    std::lock_guard<std::mutex> lk(modes_mu_);
    modes_copy_ = cfg_.strategy_modes;
  }
  if (mode == "off" || mode == "research") {
    for (const auto& o : store_.orders("resting")) {
      long long pid = store_.proposal_for_order(o.order_id);
      auto pr = pid ? store_.get_proposal(pid) : std::nullopt;
      bool governed_exit = pr && pr->strategy == "exit" &&
          ((latest_fair_strategy_.count(pr->ticker) && latest_fair_strategy_.at(pr->ticker) == strategy) ||
           pr->note.find(" via " + strategy) != std::string::npos);
      if (pr && (pr->strategy == strategy || governed_exit)) {
        try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what()); }
      }
    }
    store_.exec("UPDATE proposals SET status='expired',note='mode -> " + mode + "' WHERE status='pending' AND strategy=?", {strategy});
  }
  // Generic exit rows carry the originating strategy in their note; withdraw old mode decisions.
  for (auto& p : store_.proposals("pending", 1000)) {
    if (p.strategy == "exit" &&
        ((latest_fair_strategy_.count(p.ticker) && latest_fair_strategy_.at(p.ticker) == strategy) ||
         p.note.find(" via " + strategy) != std::string::npos)) {
      p.status = "expired"; p.note = "originating strategy mode changed"; store_.update_proposal(p);
    } else if (p.strategy == strategy && (mode == "manual" || mode == "auto" || mode == "bot")) {
      p.mode = mode; store_.update_proposal(p);
    }
  }
  store_.kv_set("mode:" + strategy, mode);
  publish_snapshot();
}

std::string Engine::mode_for(const std::string& strategy) const { return cfg_.mode_for(strategy); }

double Engine::market_weight(const std::string& strategy) const {
  // The strategy's own default, not a list of names kept in step by hand: a model added later
  // that must not be blended would otherwise be blended by silence.
  double dflt = 0.5;
  for (const auto& st : strategies_)
    if (st->name() == strategy) { dflt = st->default_market_weight(); break; }
  const json& P = cfg_.strategy_params;
  if (P.is_object() && P.contains(strategy) && P[strategy].is_object())
    return std::min(1.0, std::max(0.0, P[strategy].value("market_weight", dflt)));
  return dflt;
}

void Engine::add_watch(const std::string& s) {
  watch_.insert(s);
  store_.kv_set("watch", json(std::vector<std::string>(watch_.begin(), watch_.end())).dump());
  last_series_refresh_ = 0;
  publish_snapshot();
}
void Engine::remove_watch(const std::string& s) {
  watch_.erase(s);
  series_cache_.erase(s);
  store_.kv_set("watch", json(std::vector<std::string>(watch_.begin(), watch_.end())).dump());
  publish_snapshot();
}
std::vector<std::string> Engine::watch() const { return {watch_.begin(), watch_.end()}; }

json Engine::approve(long long id, double expected_price, double expected_count) {
  auto p = store_.get_proposal(id);
  if (!p) throw std::runtime_error("no such proposal");
  if (p->status != "pending") throw std::runtime_error("proposal is " + p->status);
  if (!is_exit_strategy(p->strategy) &&
      std::any_of(strategies_.begin(), strategies_.end(),
                  [&](const std::unique_ptr<Strategy>& st) { return st->name() == p->strategy; }))
    throw std::runtime_error("model entries are traded from the Trades board, where the decision is revalidated "
                             "just before it is sent; this proposal predates that and is not sent");
  // The row you clicked is a few seconds old, and an in-play model re-prices every cycle, so
  // demanding an exact match refused almost every click on a live match. Send what you confirmed
  // instead: the order goes at YOUR price and size when the proposal has only drifted a tick, and
  // is refused outright when it has moved further than that.
  double tick = 0.01;
  if (auto m = store_.get_market(p->ticker)) tick = m->tick > 0 ? m->tick : 0.01;
  if (expected_price >= 0 && std::fabs(expected_price - p->price) > tick + 1e-9)
    throw std::runtime_error("this row moved more than a tick since you looked (it is now " + fmt2(p->count) +
                             " @ " + fmt2(p->price) + ", you confirmed " + fmt2(expected_count) + " @ " +
                             fmt2(expected_price) + "); not sent. Click again on the refreshed row");
  if (expected_price >= 0) p->price = expected_price;             // never pay more than you agreed
  if (expected_count >= 0) p->count = std::min(p->count, expected_count);  // never buy more than you agreed
  if (p->count < 0.01) throw std::runtime_error("nothing left to buy at your size");
  if (kill_) throw std::runtime_error("kill switch is on");
  std::string oid = exec_.submit(*p);
  store_.kv_set("manual_order:" + oid, "1");
  force_reconcile_ = true;    // positions and orders on the screen within a cycle, not in 30 s
  return json{{"order_id", oid}, {"proposal", p->to_json()}};
}

json Engine::reject(long long id, const std::string& why) {
  auto p = store_.get_proposal(id);
  if (!p) throw std::runtime_error("no such proposal");
  p->status = "rejected";
  p->note = why.empty() ? "rejected" : why;
  store_.update_proposal(*p);
  return p->to_json();
}

json Engine::markets_view(const std::string& series_filter) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  const long long now = now_s(), stale_after = cfg_.data_max_age();
  json out = json::array();
  for (const auto& m : store_.open_markets(series_filter)) {
    if (!snap_watch_.count(m.series_ticker)) continue;
    json j = market_to_json(m);
    j["event_date"] = event_date(m.event_ticker);
    j["quote_ts"] = m.quote_ts;
    j["quote_stale"] = m.quote_ts <= 0 || now - m.quote_ts > stale_after;
    j["fair_ts"] = 0;
    j["fair_stale"] = true;
    j["research_only"] = false;
    j["research_reason"] = "";
    j["minutes_to_close"] = minutes_to_close(m.close_time, m.expected_expiration_time, now);
    auto sit = snap_series_.find(m.series_ticker);
    if (sit != snap_series_.end()) {
      j["category"] = sit->second.category;
      j["fee_type"] = sit->second.fee_type;
      j["fee_multiplier"] = sit->second.fee_multiplier;
    }
    auto f = snap_fair_.find(m.ticker);
    if (f != snap_fair_.end()) {
      j["p_model"] = f->second.p;
      j["sigma"] = f->second.sigma;
      j["fair_note"] = f->second.note;
      auto fs = snap_fair_strategy_.find(m.ticker);
      j["fair_strategy"] = fs == snap_fair_strategy_.end() ? "" : fs->second;
      j["edge_vs_mid"] = f->second.p - m.mid();
      j["research_only"] = f->second.research_only;
      j["research_reason"] = f->second.research_reason;
      auto ft = snap_fair_ts_.find(m.ticker);
      long long fair_ts = ft == snap_fair_ts_.end() ? 0 : ft->second;
      j["fair_ts"] = fair_ts;
      j["fair_stale"] = fair_ts <= 0 || now - fair_ts > stale_after;
    }
    out.push_back(j);
  }
  return out;
}

// Kalshi's own mutually_exclusive flag per event, cached in event_meta. Without it the risk
// book has to assume every bracket can settle YES independently, and no hedge can be verified.
// An execution claim needs the book that was actually there at the moment of the decision. This
// desk has recorded two million top-of-book rows and, before this, not one order book: no study
// of what would have filled has ever been possible on it, and no amount of later work can make
// the past ones possible. This is what makes future ones possible.
// Which books to read this cycle, in priority order: risk first (positions, pending proposals, the
// algorithm's own positions), then research candidates in every category whose route is not
// "off". Data follows the route, not execution: a manual or research candidate needs the same live
// book as an automatic one, so only an "off" category spends nothing here. Pure selection, no I/O.
std::vector<std::string> Engine::book_capture_plan() {
  static constexpr int kMaxBooksPerCycle = 20;
  std::vector<std::string> want;
  std::set<std::string> seen;
  auto add = [&](const std::string& ticker) {
    if (!ticker.empty() && seen.insert(ticker).second) want.push_back(ticker);
  };
  // Risk first, research second. Vector order matters when the request cap is reached.
  for (const auto& pos : store_.positions())
    if (pos.position != 0) add(pos.ticker);
  for (const auto& p : store_.proposals("pending", 100)) add(p.ticker);
  // The algorithm's positions and routes. A view that cannot be built must not take book capture
  // down with it (it did, for a NULL column): fall back to the defaults and the account's positions.
  json lab = json::object();
  try { lab = systematic_.view(); } catch (const std::exception& e) { LOG_WARN("book plan: systematic view: %s", e.what()); }
  if (lab.contains("positions") && lab["positions"].is_array())
    for (const auto& pos : lab["positions"])
      if (pos.is_object() && pos.contains("ticker") && pos["ticker"].is_string()) add(pos["ticker"].get<std::string>());
  json settings = lab.contains("settings") ? lab["settings"] : json();
  if (!settings.is_object()) settings = systematic_defaults();
  auto num = [&](const char* key, double dflt) {
    return settings.contains(key) && settings[key].is_number() ? settings[key].get<double>() : dflt;
  };
  const double min_p = num("min_win_probability", .6), min_edge = num("min_edge", .04);

  struct ResearchBook { std::string ticker; double liquidity = 0; };
  std::vector<ResearchBook> research;
  for (const auto& m : store_.open_markets()) {
    if (!m.tradable() || !watch_.count(m.series_ticker) || !latest_fair_.count(m.ticker)) continue;
    auto s = series_cache_.find(m.series_ticker);
    if (s == series_cache_.end()) continue;
    // The dedicated 15-minute crypto path remains frozen. Hourly/daily crypto may now have
    // execution observations, just like the other research categories.
    if (m.series_ticker.find("15M") != std::string::npos) continue;
    // The same category the systematic screen routes by, read as a route and never as a boolean.
    const std::string cat = systematic_category(json{{"category", s->second.category}, {"series_ticker", m.series_ticker}});
    if (!route_captures_data(route_setting(settings, cat))) continue;
    double score = m.liquidity + m.volume + m.open_interest;
    if (m.yes_bid > 0 && m.yes_ask > m.yes_bid) score += 100.0 / (m.yes_ask - m.yes_bid);
    const Fair& f = latest_fair_.at(m.ticker);
    FeeModel fm = FeeModel::from_series(s->second);
    double edge = -1.0;
    const MarketSidePrices book = normalize_prices(m);
    const double yes_ask = book.ask(Side::Yes), no_ask = book.ask(Side::No);
    if (f.p >= min_p && yes_ask > 0)
      edge = f.p - yes_ask - fm.fee_per_contract(yes_ask, true);
    if (1.0 - f.p >= min_p && no_ask > 0)
      edge = std::max(edge, 1.0 - f.p - no_ask - fm.fee_per_contract(no_ask, true));
    if (edge >= min_edge) {
      score += 1e9;
      const bool inputs_valid = f.inputs.is_object() && f.inputs.contains("valid") &&
                                f.inputs["valid"].is_boolean() && f.inputs["valid"].get<bool>();
      if (inputs_valid) score += 1e9;
    }
    research.push_back({m.ticker, score});
  }
  std::sort(research.begin(), research.end(), [](const auto& a, const auto& b) {
    return a.liquidity > b.liquidity;
  });
  for (const auto& r : research) {
    if (want.size() >= static_cast<size_t>(kMaxBooksPerCycle)) break;
    add(r.ticker);
  }
  return want;
}

// Reads one book, keeps it as the research depth for that ticker and records it as an
// observation (on change, plus a one-minute heartbeat). Returns the depth fields the board merges.
json Engine::capture_book(const std::string& t) {
  const long long now = now_s();
  Orderbook b = client_.get_orderbook(t, 10);
  json row = {{"quote_ts", now_s()}, {"yes_bid", b.best_yes_bid()}, {"yes_ask", b.best_yes_ask()},
              {"no_bid", b.best_no_bid()}, {"no_ask", b.best_no_ask()},
              {"yes_bid_size", b.yes_bids.empty() ? 0.0 : b.yes_bids.back().qty},
              {"yes_ask_size", b.no_bids.empty() ? 0.0 : b.no_bids.back().qty},
              {"no_bid_size", b.no_bids.empty() ? 0.0 : b.no_bids.back().qty},
              {"no_ask_size", b.yes_bids.empty() ? 0.0 : b.yes_bids.back().qty}};
  research_books_[t] = row;
  Observation o;
  o.source = "kalshi.orderbook";
  o.subject = t;
  o.kind = "book";
  o.valid_ts = now;          // the book is about the moment it was read
  o.received_ts = now_s();   // and this is when we actually had it
  o.payload = orderbook_to_json(b).dump();
  // Record changes plus a one-minute heartbeat. The heartbeat distinguishes "the book stayed
  // unchanged" from "the feed disappeared", which matters in fill and queue-position tests.
  auto seen = last_book_.find(t);
  if (seen == last_book_.end() || seen->second != o.payload || now - last_book_log_ts_[t] >= 60) {
    last_book_[t] = o.payload;
    last_book_log_ts_[t] = now;
    record_observation(store_, o);
  }
  return row;
}

void Engine::record_books() {
  static constexpr int kMaxBooksPerCycle = 20;
  int n = 0;
  for (const auto& t : book_capture_plan()) {
    if (n >= kMaxBooksPerCycle) break;
    // A market that has stopped trading has no book to learn from. A position in one still shows
    // here until settlement clears, and without this the desk spends a request a cycle fetching
    // the same empty book (KXNCAAFGAME-26SEP06WISND-WIS, determined 03:09, fifteen times over).
    auto m = store_.get_market(t);
    if (m && !m->tradable()) continue;
    try {
      capture_book(t);
      ++n;
    } catch (const std::exception& e) {
      LOG_WARN("book %s: %s", t.c_str(), e.what());
    }
  }
}

// Discovery has to redo itself when a day turns over, and the days it cares about are local to
// each contract: New York rolls three hours before Los Angeles, and a universe refreshed on one
// clock keeps offering yesterday's Los Angeles market as today's for those three hours. Cheap
// enough to run often; the point of the roll check is that it cannot be late.
void Engine::refresh_universe() {
  long long now = now_s();
  static constexpr long long kRefreshSeconds = 300;
  bool rolled = false;
  for (const auto& spec : settlement_specs()) {
    std::string day = trading_day(spec.tz, now);
    auto it = universe_day_.find(spec.tz);
    if (it == universe_day_.end()) universe_day_[spec.tz] = day;
    else if (it->second != day) {
      LOG_INFO("universe: %s rolled over to %s", spec.tz, day.c_str());
      it->second = day;
      rolled = true;
    }
  }
  if (!rolled && now - last_universe_refresh_ < kRefreshSeconds) return;
  last_universe_refresh_ = now;
  auto rows = build_universe(store_.open_markets(), now);
  save_universe(store_, rows, now);
}

void Engine::refresh_event_meta() {
  long long now = now_s();
  // Known events, in one query: the per-market lookup this used to do ran thousands of
  // statements a cycle once the watch list covered every series on the exchange.
  std::set<std::string> fresh;
  for (const auto& r : store_.query("SELECT event_ticker FROM event_meta WHERE checked_ts>?", {now - 86400}))
    fresh.insert(r.value("event_ticker", ""));
  std::vector<std::string> want;      // events we hold or have orders in come first
  std::set<std::string> seen;
  auto add = [&](const std::string& ev) {
    if (ev.empty() || fresh.count(ev) || !seen.insert(ev).second) return;
    if (now - event_meta_ts_[ev] < 300) return;        // do not retry a failure every cycle
    want.push_back(ev);
  };
  for (const auto& p : store_.positions())
    if (p.position != 0) if (auto m = store_.get_market(p.ticker)) add(m->event_ticker);
  for (const auto& o : store_.orders("resting"))
    if (auto m = store_.get_market(o.ticker)) add(m->event_ticker);
  for (const auto& r : store_.query(
           "SELECT DISTINCT event_ticker, series_ticker FROM markets WHERE status IN ('active','open') "
           "ORDER BY event_ticker", {}))
    if (watch_.count(r.value("series_ticker", ""))) add(r.value("event_ticker", ""));
  int budget = 3;
  for (const auto& ev : want) {
    if (budget-- <= 0) break;
    event_meta_ts_[ev] = now;
    try {
      KalshiClient::EventMeta e = client_.get_event(ev);
      if (!e.found) continue;
      store_.exec(
          "INSERT INTO event_meta(event_ticker,title,series_ticker,mutually_exclusive,exhaustive,checked_ts) "
          "VALUES(?,?,?,?,0,?) ON CONFLICT(event_ticker) DO UPDATE SET title=excluded.title,"
          "series_ticker=excluded.series_ticker,mutually_exclusive=excluded.mutually_exclusive,checked_ts=excluded.checked_ts",
          {ev, e.title, e.series_ticker, static_cast<long long>(e.mutually_exclusive ? 1 : 0), now});
    } catch (const std::exception& ex) {
      LOG_WARN("event meta %s: %s", ev.c_str(), ex.what());
    }
  }
}

json Engine::hedge_view(const std::string& event_ticker, const std::string& ticker, const std::string& side,
                        double count, double price) {
  std::string ev = event_ticker;
  std::optional<Holding> add;
  if (!ticker.empty()) {
    auto m = store_.get_market(ticker);
    if (!m) throw std::runtime_error("unknown market " + ticker);
    if (ev.empty()) ev = m->event_ticker;
    if (count > 0 && price > 0) {
      auto s = store_.get_series(m->series_ticker);
      double fee = s ? FeeModel::from_series(*s).fee(price, count, true) : 0.0;
      add = Holding{ticker, side != "no", count, count * price + fee, false};
    }
  }
  if (ev.empty()) throw std::runtime_error("need an event or a ticker");
  std::vector<Holding> existing = event_holdings(store_, ev);
  double limit = cfg_.risk.max_event_frac * cfg_.bankroll_dollars;
  HedgePlan plan = plan_hedge(store_, cfg_, ev, existing, add, limit);
  auto states_json = [](const std::vector<StatePayoff>& v) {
    json a = json::array();
    for (const auto& s : v)
      a.push_back({{"ticker", s.ticker}, {"label", s.label}, {"loss", s.loss}, {"pnl", -s.loss}, {"prob", s.prob}});
    return a;
  };
  json opts = json::array();
  for (const auto& l : plan.options)
    opts.push_back({{"ticker", l.ticker}, {"label", l.label}, {"side", l.side}, {"style", l.style},
                    {"count", l.count}, {"price", l.price}, {"fee_per", l.fee_per}, {"cost", l.cost},
                    {"worst_before", l.worst_before}, {"worst_after", l.worst_after},
                    {"ev_before", l.ev_before}, {"ev_after", l.ev_after}, {"have_ev", l.have_ev},
                    {"note", l.note}, {"after", states_json(l.after)}});
  return json{{"event_ticker", ev},
              {"have_structure", plan.have_structure},
              {"exhaustive", plan.exhaustive},
              {"worst_before", plan.worst_before},
              {"ev_before", plan.ev_before},
              {"have_ev", plan.have_ev},
              {"loss_limit", limit},
              {"before", states_json(plan.before)},
              {"options", opts},
              {"note", plan.note}};
}

// The hedging book: one row per event carrying risk, split into the part the desk covers by
// itself and the part left for you. The trigger is the risk you are being paid to take; above
// it the auto hedger covers `hedge_auto_fraction` of the excess. Everything cheaper than
// `hedge_price_limit` dollars of premium per dollar of worst case removed is worth buying by
// that rule, and what remains after applying it is the "optimal remainder" the manual tab asks
// you to decide about.
json Engine::hedge_book_view() {
  double bankroll = cfg_.bankroll_dollars;
  if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
  double trigger = cfg_.risk.hedge_trigger_dollars > 0 ? cfg_.risk.hedge_trigger_dollars
                                                       : cfg_.risk.max_event_frac * bankroll;
  Exposure ex = committed_exposure(store_);
  json events = json::array();
  double total_worst = 0, total_auto_target = 0, total_optimal = 0;
  for (const auto& [ev, holdings] : ex.holdings) {
    double worst = ex.events.count(ev) ? ex.events.at(ev) : 0.0;
    if (worst <= 0.01) continue;
    total_worst += worst;
    double excess = std::max(0.0, worst - trigger);
    double auto_target = worst - cfg_.risk.hedge_auto_fraction * excess;
    HedgePlan plan = plan_hedge(store_, cfg_, ev, holdings, std::nullopt, auto_target);
    json opts = json::array();
    double optimal_left = worst;
    for (const auto& l : plan.options) {
      double protect = l.worst_before - l.worst_after;
      double price_per_dollar = protect > 0 ? l.cost / protect : 1e9;
      bool worth_it = price_per_dollar <= cfg_.risk.hedge_price_limit;
      if (worth_it) optimal_left = std::min(optimal_left, l.worst_after);
      json after = json::array();
      for (const auto& sp : l.after)
        after.push_back({{"ticker", sp.ticker}, {"label", sp.label}, {"pnl", -sp.loss}, {"prob", sp.prob}});
      opts.push_back({{"ticker", l.ticker}, {"label", l.label}, {"side", l.side}, {"count", l.count},
                      {"price", l.price}, {"cost", l.cost}, {"worst_after", l.worst_after},
                      {"protects", protect}, {"price_per_dollar", price_per_dollar}, {"worth_it", worth_it},
                      {"ev_before", l.ev_before}, {"ev_after", l.ev_after}, {"have_ev", l.have_ev},
                      {"covers_auto", l.worst_after <= auto_target + 1e-9}, {"after", after}});
    }
    json states = json::array();
    for (const auto& sp : plan.before)
      states.push_back({{"ticker", sp.ticker}, {"label", sp.label}, {"pnl", -sp.loss}, {"prob", sp.prob}});
    total_auto_target += std::min(worst, auto_target);
    total_optimal += optimal_left;
    events.push_back({{"event_ticker", ev},
                      {"worst", worst},
                      {"worst_state", ex.worst_state.count(ev) ? ex.worst_state.at(ev) : ""},
                      {"trigger", trigger},
                      {"excess", excess},
                      {"auto_target", auto_target},
                      {"needs_auto", excess > 0.01 && !plan.options.empty()},
                      {"optimal_left", optimal_left},
                      {"have_structure", plan.have_structure},
                      {"exhaustive", plan.exhaustive},
                      {"note", plan.note},
                      {"states", states},
                      {"options", opts}});
  }
  return json{{"trigger", trigger},
              {"auto_fraction", cfg_.risk.hedge_auto_fraction},
              {"price_limit", cfg_.risk.hedge_price_limit},
              {"auto_mode", cfg_.strategy_modes.count("auto_hedge") ? cfg_.mode_for("auto_hedge") : "manual"},
              {"auto_armed", auto_armed()},
              {"total_worst", total_worst},
              {"total_after_auto", total_auto_target},
              {"total_optimal", total_optimal},
              {"events", events}};
}

// The automatic half: cover the configured share of any event's excess worst case. The legs are
// ordinary proposals, so the caps, the kill switch and the daily stop all still apply; they only
// leave on their own when AUTO is armed and auto_hedge is in auto or bot mode.
void Engine::run_auto_hedge() {
  std::string mode = cfg_.strategy_modes.count("auto_hedge") ? cfg_.mode_for("auto_hedge") : "manual";
  if (mode == "off" || cfg_.risk.hedge_auto_fraction <= 0) return;
  double bankroll = cfg_.bankroll_dollars;
  if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
  double trigger = cfg_.risk.hedge_trigger_dollars > 0 ? cfg_.risk.hedge_trigger_dollars
                                                       : cfg_.risk.max_event_frac * bankroll;
  Exposure ex = committed_exposure(store_);
  for (const auto& [ev, holdings] : ex.holdings) {
    double worst = ex.events.count(ev) ? ex.events.at(ev) : 0.0;
    if (worst <= trigger + 0.01) continue;                       // inside the risk you accept
    if (!store_.query("SELECT id FROM proposals WHERE event_ticker=? AND strategy='auto_hedge' AND status IN "
                      "('pending','submitted','cancel_pending')", {ev}).empty()) continue;
    double target = worst - cfg_.risk.hedge_auto_fraction * (worst - trigger);
    HedgePlan plan = plan_hedge(store_, cfg_, ev, holdings, std::nullopt, target);
    if (plan.options.empty()) continue;
    const HedgeLeg* pick = nullptr;
    for (const auto& l : plan.options) {
      double protect = l.worst_before - l.worst_after;
      if (protect <= 0) continue;
      if (l.cost / protect > cfg_.risk.hedge_price_limit) continue;   // too expensive to be worth it
      pick = &l;
      break;
    }
    if (!pick) continue;
    auto m = store_.get_market(pick->ticker);
    Proposal p;
    p.ticker = pick->ticker;
    p.event_ticker = ev;
    p.series_ticker = m ? m->series_ticker : "";
    p.strategy = "auto_hedge";
    p.entry_kind = "hedge";
    p.mode = store_.kv_get("auto_hedge:"+ev).value_or("0") == "1" ? mode : "manual";
    p.outcome_side = pick->side;
    p.exec_style = "taker";
    p.status = "pending";
    p.price = pick->price;
    p.count = pick->count;
    p.p_model = pick->side == "yes" ? pick->price : 1.0 - pick->price;
    p.sigma = 0;
    p.fee_per = pick->fee_per;
    p.net_edge = 0;
    p.cluster = m ? risk_cluster(store_, *m) : ev;
    p.note = "auto hedge: " + ev + " worst case " + fmt2(worst) + " over the " + fmt2(trigger) +
             " you accept; this leg takes it to " + fmt2(pick->worst_after) + " for " + fmt2(pick->cost) +
             " (" + fmt2(pick->cost / std::max(0.01, pick->worst_before - pick->worst_after)) +
             " per dollar of worst case removed)";
    store_.insert_proposal(p);
    LOG_INFO("%s", p.note.c_str());
  }
}

json Engine::size_suggestion(const std::string& ticker, const std::string& side, double price,
                             const std::string& style, const std::string& decision_id) {
  if((side!="yes"&&side!="no")||(style!="maker"&&style!="taker")||!std::isfinite(price)||price<=0||price>=1)throw std::runtime_error("invalid sizing input");
  auto m = store_.get_market(ticker);
  if (!m) throw std::runtime_error("unknown market " + ticker);
  auto srs = store_.get_series(m->series_ticker);
  FeeModel fm = srs ? FeeModel::from_series(*srs) : FeeModel{};
  bool taker = style != "maker";
  double fee = fm.fee_per_contract(price, taker);
  double cost = price + fee;
  double bankroll = cfg_.bankroll_dollars;
  if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
  auto by_event = exposure_by("event", true);
  auto by_cluster = exposure_by("cluster", true);
  std::string cluster = risk_cluster(store_, *m);
  double room_event = cfg_.risk.max_event_frac * bankroll - by_event[m->event_ticker];
  double room_cluster = cfg_.risk.max_cluster_frac * bankroll - by_cluster[cluster];
  double room_open = cfg_.risk.max_open_frac * bankroll - open_exposure();
  double room = std::min({room_event, room_cluster, room_open});
  double cash = 0;
  if (auto a = store_.last_account()) cash = std::max(0.0, a->balance - committed_exposure(store_).resting);
  const auto shard=shard_cash_view(store_,*m,now_s());
  room = std::min({room,cash,shard.value("available",0.),cfg_.risk.max_order_dollars});
  double n_caps = cost > 0 ? std::floor(std::max(0.0, room) / cost) : 0;
  double n = n_caps;
  std::string basis = shard.value("fresh",false)?"risk caps + exchange shard "+std::to_string(m->exchange_index)
                                                :shard.value("reason",std::string("shard balance unavailable; sync first"));
  // What is actually binding, in words, so a refusal names its own cause instead of listing four
  // numbers and leaving the reader to work out which one is zero.
  std::string binding;
  if (!shard.value("fresh",false)) binding = shard.value("reason",std::string("the shard balance is not known"));
  else if (room==shard.value("available",0.)) binding = "exchange shard "+std::to_string(m->exchange_index)+
      " has $"+fmt2(shard.value("available",0.))+" available";
  else if (room==cash) binding = "account cash net of resting orders is $"+fmt2(cash);
  else if (room==room_event) binding = "the per-event cap leaves $"+fmt2(std::max(0.0,room_event));
  else if (room==room_cluster) binding = "the correlated-cluster cap leaves $"+fmt2(std::max(0.0,room_cluster));
  else if (room==room_open) binding = "the total-open cap leaves $"+fmt2(std::max(0.0,room_open));
  else binding = "the per-order cap is $"+fmt2(cfg_.risk.max_order_dollars);
  // The model's number for this contract and the edge at this price and style, both from the one
  // edge engine: the decision you are reviewing when the dialog names one (the book and the
  // probability it was decided on), else the ticker's latest ProbabilityQuote against the stored
  // book. The order dialog shows these numbers; it computes none of them. Kelly runs on a
  // probability the model stands behind. An order placed on a model's decision also gets the flat
  // cap for a model that has not yet beaten the price on settled markets: the cap the old proposal
  // lane put on every model signal it sized, which is what a Trades-board decision replaces.
  const json settings = decision_settings();
  double p_win = -1;
  std::string model;
  json edge = nullptr, decision = nullptr;
  if (!decision_id.empty()) {
    json shown;
    {
      std::lock_guard<std::mutex> lk(decisions_mu_);
      auto it = recent_decisions_.find(decision_id);
      if (it != recent_decisions_.end()) shown = it->second.first;
    }
    if (shown.is_object() && jstr(shown, "ticker") == ticker && jstr(shown, "side") == side) {
      const EdgeCandidate at = decision_edge_at(shown, style, price, settings);
      const double floor = jnum(settings, "manual_min_edge", 0.05);
      edge = candidate_to_json(at);
      edge["model"] = jstr(shown, "model_key");
      edge["sigma"] = std::isfinite(at.fair.sigma) ? json(at.fair.sigma) : json(nullptr);
      decision = {{"decision_id", decision_id}, {"found", true}, {"route", jstr(shown, "route")},
                  {"reasons", shown.contains("reasons") ? shown["reasons"] : json::array()},
                  {"manual_min_edge", floor}, {"clears_floor", at.executable && at.executable_edge + 1e-9 >= floor}};
      if (std::isfinite(at.fair_probability) && jstr(shown, "route") == "manual") {
        p_win = at.fair_probability;
        model = jstr(shown, "model");
      }
    } else {
      decision = {{"decision_id", decision_id}, {"found", false},
                  {"reason", "this row is no longer on the board; the desk will refuse a ticket on it"}};
    }
  } else {
    std::lock_guard<std::mutex> lk(snap_mu_);
    auto q = snap_quotes_.find(ticker);
    if (q != snap_quotes_.end() && quote_valid(q->second) && q->second.inputs_valid) {
      EdgeSettings es;
      es.uncertainty_multiplier = jnum(settings, "uncertainty_buffer");
      es.taker_slippage = jnum(settings, "slippage_estimate");
      const EdgeCandidate at = candidate_at(q->second, normalize_prices(*m), side_from(side), style, price, fm, es);
      edge = candidate_to_json(at);
      edge["model"] = q->second.model_key();
      edge["sigma"] = std::isfinite(q->second.sigma) ? json(q->second.sigma) : json(nullptr);
      edge["research_only"] = q->second.research_only;
      // A hand ticket is sized as it always was: the caps, and Kelly on a number the model stands
      // behind. The uncalibrated-model cap below is for orders placed ON a model's decision.
      if (!q->second.research_only) p_win = at.fair_probability;
    }
  }
  if (p_win > 0) {
    double kelly = size_contracts(bankroll, p_win, cost, cfg_.risk.kelly_frac, cfg_.risk.max_event_frac);
    if (kelly < n) { n = kelly; basis = "quarter Kelly"; }
  }
  if (!model.empty() && !calibration_for(model).demonstrated) {
    const double flat = cfg_.risk.uncalibrated_max_order_frac * bankroll;
    const double n_cal = cost > 0 ? std::floor(flat / cost) : 0;
    if (n_cal < n) {
      n = n_cal;
      basis = "uncalibrated model cap";
      binding = model + " has not beaten the price on settled markets yet, so an order on its number is capped at $" + fmt2(flat);
    }
  }
  return json{{"count", std::max(0.0, n)}, {"cost_per", cost}, {"fee_per", fee}, {"room", std::max(0.0, room)},
              {"basis", basis}, {"cap_event", std::max(0.0, room_event)}, {"cap_cluster", std::max(0.0, room_cluster)},
              {"cap_open", std::max(0.0, room_open)}, {"cash", cash}, {"shard",shard}, {"p_win", p_win},
              {"binding", binding}, {"cost_total", cost}, {"price", price}, {"style", style},
              {"edge", edge}, {"decision", decision}};
}

json Engine::board_row_locked(const Market& m, long long now, long long stale_after, long long manage_after,
                              double* rank_edge, const std::map<std::string, double>* held) {
  std::string category;
  double fee_mult = 1.0;
  std::string fee_type = "quadratic";
  auto sit = snap_series_.find(m.series_ticker);
  if (sit != snap_series_.end()) {
    category = sit->second.category;
    fee_type = sit->second.fee_type;
    fee_mult = sit->second.fee_multiplier;
  }
  double edge = -9, best_price = 0, best_fee = 0;
  std::string best_side = "yes";
  long long settle_ts = expected_settlement_ts(m);
  json j = {{"ticker", m.ticker},
            {"event_ticker", m.event_ticker},
            {"series_ticker", m.series_ticker},
            {"title", m.title},
            {"yes_sub_title", m.yes_sub_title.empty() ? m.subtitle : m.yes_sub_title},
            {"category", category},
            {"fee_type", fee_type},
            {"fee_multiplier", fee_mult},
            {"event_date", event_date(m.event_ticker)},
            {"yes_bid", m.yes_bid},
            {"yes_ask", m.yes_ask},
            {"no_bid", m.no_bid},
            {"no_ask", m.no_ask},
            {"yes_bid_size", m.yes_bid_size},
            {"yes_ask_size", m.yes_ask_size},
            {"tick", m.tick > 0 ? m.tick : 0.01},
            {"quote_ts", m.quote_ts},
            {"open_ts", parse_iso(m.open_time)},
            {"close_ts", parse_iso(m.close_time)},
            {"market_status", m.status},
            {"minutes_to_close", minutes_to_close(m.close_time, m.expected_expiration_time, now)},
            {"settlement_ts", settle_ts},
            {"hours_to_settlement", settle_ts > 0 ? (settle_ts - now) / 3600.0 : -1.0},
            {"quote_stale", m.quote_ts <= 0 || now - m.quote_ts > stale_after},
            {"quote_stale_manage", m.quote_ts <= 0 || now - m.quote_ts > manage_after},
            {"fair_stale", true},
            {"fair_stale_manage", true},
            {"research_only", false},
            {"research_reason", ""}};
  auto f = snap_fair_.find(m.ticker);
  if (f != snap_fair_.end()) {
    j["p_model"] = f->second.p;
    j["p_raw"] = f->second.p_raw >= 0 ? f->second.p_raw : f->second.p;
    j["market_blend_weight"] = f->second.market_blend_weight;
    j["model_inputs"] = f->second.inputs;
    j["model_research_only"] = f->second.research_only;
    j["model_research_reason"] = f->second.research_reason;
    j["sigma"] = f->second.sigma;
    j["fair_note"] = f->second.note;
    j["research_only"] = f->second.research_only;
    j["research_reason"] = f->second.research_reason;
    auto fs = snap_fair_strategy_.find(m.ticker);
    j["fair_strategy"] = fs == snap_fair_strategy_.end() ? "" : fs->second;
    auto ft = snap_fair_ts_.find(m.ticker);
    long long fair_ts = ft == snap_fair_ts_.end() ? 0 : ft->second;
    j["fair_ts"] = fair_ts;
    j["fair_stale"] = fair_ts <= 0 || now - fair_ts > stale_after;
    j["fair_stale_manage"] = fair_ts <= 0 || now - fair_ts > manage_after;
    // The canonical model output travels with the row: the router reads the quote, not the
    // engine's display fields.
    auto qit = snap_quotes_.find(m.ticker);
    ProbabilityQuote rq;
    if (qit != snap_quotes_.end()) {
      rq = qit->second;
      j["quote"] = quote_to_json(rq);
      j["model_version"] = rq.model_version;
      j["settlement_verified"] = rq.settlement_verified;
    } else {
      rq.ticker = m.ticker;
      rq.p_yes = f->second.p;
      rq.sigma = f->second.sigma;
    }
    // Rank by the edge a taker would keep on the better side, from the one edge engine (no
    // uncertainty deduction here: this orders the board, the router decides).
    FeeModel fm{fee_type, fee_mult};
    EdgeSettings rank_settings;
    rank_settings.uncertainty_multiplier = 0;
    const MarketSidePrices px = normalize_prices(m);
    for (Side side : {Side::Yes, Side::No}) {
      EdgeCandidate c = taker_candidate(rq, px, side, fm, rank_settings);
      if (c.executable && c.net_edge > edge) {
        edge = c.net_edge;
        best_side = side_name(side);
        best_price = c.entry_price;
        best_fee = c.entry_fee;
      }
    }
  }
  j["edge"] = edge > -8 ? edge : 0.0;
  j["have_edge"] = edge > -8;
  Proposal probe;
  probe.ticker = m.ticker;
  probe.event_ticker = m.event_ticker;
  probe.series_ticker = m.series_ticker;
  probe.strategy = j.value("fair_strategy", "board");
  probe.outcome_side = best_side;
  probe.exec_style = "taker";
  probe.price = best_price > 0 ? best_price : 0.5;
  probe.count = 1;
  probe.fee_per = best_fee;
  probe.net_edge = edge > -8 ? edge : 0.0;
  EntryContext admission = entry_context(store_, cfg_, probe, m, now);
  j["entry_context"] = admission.to_json();
  if (!admission.allowed) {
    j["research_only"] = true;
    j["research_reason"] = admission.reason;
  }
  // A position on this ticker: the entry policy for the side it holds, where a same-side addition
  // can qualify, priced at that side's own offer (the router reads entry_context_<side> first).
  double position = 0;
  if (held) {
    auto hit = held->find(m.ticker);
    if (hit != held->end()) position = hit->second;
  } else {
    for (const auto& pos : store_.positions()) if (pos.ticker == m.ticker) position = pos.position;
  }
  if (position != 0) {
    const std::string held_side = position > 0 ? "yes" : "no";
    const auto qit = snap_quotes_.find(m.ticker);
    EdgeSettings no_buffer;
    no_buffer.uncertainty_multiplier = 0;
    ProbabilityQuote hq;
    if (qit != snap_quotes_.end()) hq = qit->second;
    const EdgeCandidate c = taker_candidate(hq, normalize_prices(m), side_from(held_side),
                                            FeeModel{j.value("fee_type", std::string("quadratic")), j.value("fee_multiplier", 1.0)},
                                            no_buffer);
    Proposal add = probe;
    add.outcome_side = held_side;
    add.price = c.executable ? c.entry_price : 0.5;
    add.fee_per = c.entry_fee;
    add.net_edge = c.executable ? c.net_edge : 0.0;
    j["entry_context_" + held_side] = entry_context(store_, cfg_, add, m, now).to_json();
  }
  if (rank_edge) *rank_edge = edge;
  return j;
}

json Engine::board_view(int limit) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  // Two ages, two jobs. `quote_stale` / `fair_stale` use the desk's one limit for OPENING risk
  // (Config::data_max_age), shared with the strategy engine and the systematic screen. The
  // `*_manage` flags keep the older, looser rule for MANAGING what is already on (exits, stops,
  // resting entries): refusing to manage a position is a risk of its own.
  const long long now = now_s(), stale_after = cfg_.data_max_age(),
                  manage_after = std::max(60, cfg_.poll_seconds * 3);
  struct Row { json j; double edge; };
  std::vector<Row> rows;
  rows.reserve(512);
  std::map<std::string, double> held;
  for (const auto& pos : store_.positions()) if (pos.position != 0) held[pos.ticker] = pos.position;
  for (const auto& m : store_.open_markets()) {
    if (!snap_watch_.count(m.series_ticker)) continue;
    double edge = -9;
    json j = board_row_locked(m, now, stale_after, manage_after, &edge, &held);
    rows.push_back({std::move(j), edge});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.edge > b.edge; });
  json out = json::array();
  int n = 0;
  for (auto& r : rows) {
    if (limit > 0 && n++ >= limit) break;
    out.push_back(std::move(r.j));
  }
  return json{{"total", rows.size()}, {"shown", out.size()}, {"markets", out},
              {"market_data_max_age_seconds", stale_after}};
}

// Research uses actual captured depth, without rewriting the quotes or strategy inputs used by
// the existing desk. Filtering happens before any presentation cap, including for BTC/ETH.
json Engine::research_board() {
  json board = board_view(0);
  const long long now = now_s(), stale_after = cfg_.data_max_age(),
                  manage_after = std::max(60, cfg_.poll_seconds * 3);
  std::lock_guard<std::mutex> lk(snap_mu_);
  for (auto& row : board["markets"]) {
    const std::string ticker = row.value("ticker", "");
    auto book = snap_research_books_.find(ticker);
    const long long book_ts = book == snap_research_books_.end() ? 0 : book->second.value("quote_ts", 0LL);
    const long long row_ts = row.value("quote_ts", 0LL);
    // A captured book is merged (prices and depth) while it is young enough to manage on, as it
    // always was: a stop needs the captured depth to hit the bid, and one failed read in a slow
    // cycle must not turn it into a resting sell. It is also merged when it is at least as new as
    // the market's own top of book. Only a capture past the management limit AND older than the
    // market row is dropped (it used to overwrite that fresher quote and leave the row stale for
    // good); the row is then treated like one with no capture: its own quote, no captured depth.
    // The flags come from the capture's age: entries stop at the entry limit, management at its own.
    // Positions and resting entries are captured first every cycle, so for them the capture is
    // effectively always what is used.
    const bool capture_manageable = book_ts > 0 && book_ts <= now && now - book_ts <= manage_after;
    if (book != snap_research_books_.end() && (capture_manageable || book_ts >= row_ts)) {
      for (const auto& [key, value] : book->second.items()) row[key] = value;
      row["quote_stale"] = book_ts <= 0 || now < book_ts || now - book_ts > stale_after;
      row["quote_stale_manage"] = book_ts <= 0 || now < book_ts || now - book_ts > manage_after;
    } else {
      row["yes_bid_size"] = 0.0;
      row["yes_ask_size"] = 0.0;
      row["no_bid_size"] = 0.0;
      row["no_ask_size"] = 0.0;
    }
  }
  // Paper holdings may survive the open universe. Keep observing their last available book;
  // the simulator will mark it stale and leave exits pending rather than inventing a fill.
  return board;
}

json Engine::research_screen(const json& settings) {
  return systematic_screen(research_board(), settings, now_s(), {}, {}, cfg_.risk.max_disagreement);
}

json Engine::systematic_view(int screen_limit) {
  json out = systematic_.view();
  // The router with the desk's real state: ownership, cooldowns, the kill switch and the master
  // switch (a row the algorithm would take while it is stopped is shown as yours, with the reason).
  const long long t = now_s();
  json screen = systematic_screen(research_board(), systematic_.settings_copy(), router_context(t));
  // The rows are sorted: algo, manual, weak, blocked. The blocked tail is most of the exchange
  // and the page does not need all of it every five seconds.
  size_t total = screen["rows"].size();
  if (screen_limit > 0 && total > static_cast<size_t>(screen_limit))
    screen["rows"].erase(screen["rows"].begin() + screen_limit, screen["rows"].end());
  screen["shown"] = screen["rows"].size();
  out["screen"] = std::move(screen);
  out["live_auto_armed"] = auto_armed();
  return out;
}

json Engine::configure_systematic(const json& body) {
  systematic_.configure(body);
  return systematic_view();
}

json Engine::strategies_view() const {
  std::lock_guard<std::mutex> lk(snap_mu_);
  return snap_strategies_;
}

std::optional<std::pair<Fair, std::string>> Engine::fair_for(const std::string& ticker) const {
  std::lock_guard<std::mutex> lk(snap_mu_);
  auto it = snap_fair_.find(ticker);
  if (it == snap_fair_.end()) return std::nullopt;
  auto st = snap_fair_strategy_.find(ticker);
  return std::make_pair(it->second, st == snap_fair_strategy_.end() ? std::string() : st->second);
}

json Engine::risk_view() {
  double bank = cfg_.bankroll_dollars;
  if (auto a=store_.last_account()) bank=std::min(bank,std::max(0.0,a->balance+a->portfolio_value));
  auto ev = exposure_by("event", false);
  auto cl = exposure_by("cluster", false);
  auto top = [&](std::map<std::string, double>& m, double cap) {
    std::vector<std::pair<std::string, double>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    json out = json::array();
    for (size_t i = 0; i < v.size() && i < 8; ++i)
      out.push_back({{"key", v[i].first}, {"used", v[i].second}, {"cap", cap}, {"pct", cap > 0 ? v[i].second / cap : 0}});
    return out;
  };
  double open = open_exposure();
  json daily = daily_equity(store_,cfg_);
  auto committed = committed_exposure(store_);
  double proposed = 0;
  for (const auto& p:store_.proposals("pending",500)) proposed += p.count*(p.price+p.fee_per);
  return json{{"bankroll", bank},
              {"open_exposure", open}, {"filled_cost",committed.filled}, {"resting_cost",committed.resting},
              {"settling_cost", committed.settling}, {"settling_n", committed.settling_tickers.size()},
              {"proposal_cost",proposed},
              {"open_cap", cfg_.risk.max_open_frac * bank},
              {"events", top(ev, cfg_.risk.max_event_frac * bank)},
              {"clusters", top(cl, cfg_.risk.max_cluster_frac * bank)},
              {"daily", daily},
              {"kill", kill_.load()},
              {"kill_reason", kill_reason_}};
}

// ---- the book -------------------------------------------------------------------------------
// One switch over lanes that already exist, rather than a fifth algorithm. Each lane keeps its
// own rules, budgets and exits; this only starts and stops them together and reports the line.

namespace {
// A lane the switch can drive. `available` is the question "could this run at all right now",
// which is not the same as "is it on": a lane with no key, or switched off in config, must say
// so in words rather than silently stay dark when the switch is thrown.
json lane_row(const std::string& name, const std::string& covers, bool available,
              const std::string& why, bool running, bool paper) {
  return json{{"name", name}, {"covers", covers}, {"available", available},
              {"unavailable_reason", available ? "" : why},
              {"running", running}, {"mode", paper ? "paper" : "live"}};
}
}  // namespace

json Engine::book() {
  json lanes = json::array();
  int on = 0;

  const json sys = systematic_.view();
  const bool sys_live = jstr(sys, "mode") == "live";
  const bool sys_ok = !sys_live || exec_.armed();
  lanes.push_back(lane_row("systematic", "weather, crypto, sports and the locked baskets",
                           sys_ok, "live mode needs an API key", sys.value("running", false), !sys_live));
  if (sys.value("running", false)) ++on;

  const json pm = poly_.summary();
  lanes.push_back(lane_row("pairs", "the same game on Kalshi and Polymarket",
                           pm.value("enabled", false) && pm.value("armed", false),
                           pm.value("enabled", false) ? "no Polymarket key: prices only"
                                              : "Polymarket is off in config",
                           pm.value("algo_running", false), pm.value("algo_paper", false)));
  if (pm.value("algo_running", false)) ++on;

  const json pp = perps_.summary();
  lanes.push_back(lane_row("perps", "the margined instance: basis against the index",
                           pp.value("enabled", false) && pp.value("armed", false),
                           pp.value("enabled", false) ? "the margined instance is not reachable"
                                              : "perps are off in config",
                           pp.value("algo_running", false), pp.value("algo_paper", false)));
  if (pp.value("algo_running", false)) ++on;

  // Lane 2 is not a queue of its own: it is the rows the screen already produced and held back.
  // Counting them rather than inventing a second list keeps one definition of the line. The
  // screen is the one systematic_view() last computed (the dashboard asks for it every few
  // seconds); this used to read systematic_.view(), which has no screen, so it always said zero.
  // Older than a minute, or never computed, and the counts are unknown rather than zero.
  json summary;
  {
    std::lock_guard<std::mutex> lk(screen_summary_mu_);
    summary = screen_summary_;
  }
  const long long screen_ts = summary.value("generated_ts", 0LL), now = now_s();
  const bool known = screen_ts > 0 && screen_ts <= now + 1 && now - screen_ts <= 60;
  return json{{"running", on > 0}, {"lanes_on", on}, {"lanes", lanes},
              {"for_you", known ? json(summary.value("for_you", 0)) : json(nullptr)},
              {"blocked", known ? json(summary.value("blocked", 0)) : json(nullptr)},
              {"research", known ? json(summary.value("research", 0)) : json(nullptr)},
              {"best_edge", known ? summary.value("best_edge", 0.0) : 0.0},
              {"best_ticker", known ? summary.value("best_ticker", std::string()) : std::string()},
              {"screen_ts", screen_ts},
              {"kill", kill_.load()}, {"kill_reason", kill_reason_}};
}

json Engine::book_configure(const json& body) {
  if (!body.is_object()) throw std::runtime_error("request must be an object");
  if (!body.contains("running") || !body["running"].is_boolean())
    throw std::runtime_error("book takes {running: true|false} and an optional lane");
  const bool want = body["running"].get<bool>();
  // An optional lane turns the master switch into a per-lane one. Running the pairs lane without
  // the weather book, or stopping perps alone, is an ordinary thing to want and used to mean
  // going to a different tab for each.
  const std::string only = body.value("lane", "");
  if (!only.empty() && only != "systematic" && only != "pairs" && only != "perps")
    throw std::runtime_error("unknown lane: " + only);
  if (want && kill_)
    throw std::runtime_error("the kill switch is on; re-arm the desk first");

  // Each lane is asked separately and a refusal from one does not stop the others: a missing
  // Polymarket key should not keep weather and crypto switched off.
  json started = json::array(), refused = json::array();
  auto drive = [&](const char* name, auto&& fn) {
    if (!only.empty() && only != name) return;
    try { fn(); started.push_back(name); }
    catch (const std::exception& e) { refused.push_back(json{{"lane", name}, {"why", e.what()}}); }
  };
  drive("systematic", [&] { systematic_.configure(json{{"running", want}}); });
  drive("pairs",      [&] { poly_.configure_algo(json{{"running", want}}); });
  drive("perps",      [&] { perps_.configure_algo(json{{"running", want}}); });

  store_.log_event("info", (only.empty() ? std::string("book ") : "lane " + only + " ") +
                               (want ? "started: " : "stopped: ") + std::to_string(started.size()) +
                               " lanes" +
                               (refused.empty() ? "" : ", " + std::to_string(refused.size()) + " refused"));
  json out = book();
  out["changed"] = started;
  out["refused"] = refused;
  return out;
}

json Engine::status() {
  std::lock_guard<std::mutex> lk(snap_mu_);
  return snap_status_.is_null() ? json{{"env", cfg_.env}, {"cycle", 0}, {"watch", json::array()},
                                       {"strategies", json::array()}, {"kill", kill_.load()}, {"armed", exec_.armed()},
                                       {"markets_seen", 0}, {"bankroll", cfg_.bankroll_dollars},
                                       {"balance", 0.0}, {"portfolio_value", 0.0}, {"last_cycle_ts", 0},
                                       {"last_error", ""}, {"kill_reason", ""}, {"account_ts", 0}}
                                : snap_status_;
}

// ---- the decision pipeline (session 15) -------------------------------------------------------

RouterContext Engine::router_context(long long now, bool include_manual_queue) {
  RouterContext ctx;
  ctx.now = now;
  ctx.killed = kill_.load();
  systematic_.router_context(ctx);   // the algorithm's trades, account positions, cooldowns, switch
  ctx.strategy_modes = strategy_modes();   // the locked copy: this also runs on HTTP threads
  // A working order owns its event the way a position does: one event, one entry decision.
  for (const auto& o : store_.orders("resting"))
    if (auto m = store_.get_market(o.ticker)) { ctx.held_events.insert(m->event_ticker); ctx.working_events.insert(m->event_ticker); }
  // So does a proposal still waiting to be sent (an arbitrage leg waiting for approval, an exit).
  for (const auto& p : store_.proposals("pending", 500)) {
    std::string event = p.event_ticker;
    if (event.empty())
      if (auto m = store_.get_market(p.ticker)) event = m->event_ticker;
    ctx.held_events.insert(event);
    ctx.working_events.insert(event);
  }
  // So does a manual request you have already queued there.
  if (include_manual_queue)
    for (const auto& r : store_.query("SELECT event_ticker FROM manual_requests WHERE status IN ('queued','processing','unknown')"))
      if (r.contains("event_ticker") && r["event_ticker"].is_string()) {
        ctx.held_events.insert(r["event_ticker"].get<std::string>());
        ctx.working_events.insert(r["event_ticker"].get<std::string>());
      }
  ctx.held_events.erase("");
  ctx.working_events.erase("");
  return ctx;
}

void Engine::publish_decisions(const json& screen, long long now) {
  std::vector<json> offered, research;
  // Lane 2 of the Book panel, counted from this cycle's decisions: the rows offered to you, the
  // blocked and the research ones, and the best executable edge waiting for you.
  {
    int for_you = 0, blocked = 0, n_research = 0;
    double best = 0;
    std::string best_ticker;
    if (screen.contains("rows") && screen["rows"].is_array())
      for (const auto& r : screen["rows"]) {
        const std::string route = jstr(r, "route");
        if (route == "blocked") { ++blocked; continue; }
        if (route == "research") { ++n_research; continue; }
        if (route != "manual") continue;
        ++for_you;
        if (jnum(r, "executable_edge") > best) { best = jnum(r, "executable_edge"); best_ticker = jstr(r, "ticker"); }
      }
    std::lock_guard<std::mutex> lk(screen_summary_mu_);
    screen_summary_ = json{{"generated_ts", screen.value("generated_ts", 0LL)}, {"for_you", for_you},
                           {"blocked", blocked}, {"research", n_research}, {"best_edge", best},
                           {"best_ticker", best_ticker}};
  }
  {
    std::lock_guard<std::mutex> lk(decisions_mu_);
    cycle_screen_ = screen;
    if (screen.contains("rows") && screen["rows"].is_array())
      for (const auto& r : screen["rows"]) {
        const std::string route = r.value("route", std::string());
        if (route != "manual" && route != "auto" && route != "research") continue;
        const std::string id = r.value("decision_id", std::string());
        if (!id.empty() && route != "research") recent_decisions_[id] = {r, now};
        // Snapshot what was offered when it first appears, when its route changes or its edge moves
        // by 2c, and on a 5-minute heartbeat; not every cycle (the book's timestamp alone changes
        // every five seconds). What happened to a decision (approved, revalidated, auto, rejected,
        // submitted) is always recorded; this is the sampled record of what the board showed.
        const double price = jnum(r, "price"), edge = jnum(r, "executable_edge", -1);
        // The research record is for the candidates a research model would have taken (an edge
        // after the buffer), sampled: when one first appears or changes route, then every 15 min.
        // Its every estimate is already in model_fairs; this table is for decisions.
        if (route == "research" && !(edge > 0)) continue;
        const std::string key = jstr(r, "ticker") + "|" + jstr(r, "side") + "|" + jstr(r, "model_key");
        Offered& o = offered_[key];
        const bool changed = o.ts == 0 || o.route != route || (route != "research" && std::fabs(o.edge - edge) >= 0.02 - 1e-9);
        if (changed || now - o.ts >= (route == "research" ? 900 : 300)) {
          (route == "research" ? research : offered).push_back(r);
          o = Offered{route, id, price, edge, now};
        }
      }
    // An unsent review expires after 90 s, so a displayed decision is only needed that long.
    for (auto it = recent_decisions_.begin(); it != recent_decisions_.end();)
      it = now - it->second.second > 150 ? recent_decisions_.erase(it) : std::next(it);
    for (auto it = offered_.begin(); it != offered_.end();)
      it = now - it->second.ts > 3600 ? offered_.erase(it) : std::next(it);
  }
  for (const auto& r : offered) record_decision(store_, r, "offered", now);
  for (const auto& r : research) record_decision(store_, r, "research", now);
  // The sampled records age out (offered after 30 days, research after 90); what happened to a
  // decision, and so every order's trail, is kept.
  if (now - last_decision_prune_ >= 86400) {
    last_decision_prune_ = now;
    store_.exec("DELETE FROM trade_decisions WHERE (event='offered' AND ts<?) OR (event='research' AND ts<?)",
                {now - 30LL * 86400, now - 90LL * 86400});
  }
}

json Engine::decisions_view(bool include_research, int limit) {
  json screen;
  {
    std::lock_guard<std::mutex> lk(decisions_mu_);
    screen = cycle_screen_;
  }
  std::vector<json> lanes, research;
  if (screen.contains("rows") && screen["rows"].is_array())
    for (const auto& r : screen["rows"]) {
      const std::string route = jstr(r, "route");
      if (route == "manual" || route == "auto") lanes.push_back(r);
      // A research row is on the board's research filter only when its model would have an edge
      // there: the rest are recorded (trade_decisions) and shown on the Systematic tab.
      else if (route == "research" && jnum(r, "executable_edge", -1) > 0) research.push_back(r);
    }
  // Ranked by edge per dollar of risk (Kelly), the order the algorithm itself takes AUTO rows in.
  auto by_kelly = [](const json& a, const json& b) { return jnum(a, "kelly", -1) > jnum(b, "kelly", -1); };
  std::stable_sort(lanes.begin(), lanes.end(), by_kelly);
  std::stable_sort(research.begin(), research.end(), by_kelly);
  // One row per event and route: the best one. An event is one entry decision (the others are
  // blocked the moment one is taken), and a single event's brackets would otherwise fill the
  // board. `event_rows` says how many rows the event had on that route.
  auto one_per_event = [](std::vector<json>& rows) {
    std::map<std::string, size_t> first;
    std::vector<json> out;
    for (auto& r : rows) {
      const std::string key = jstr(r, "event_ticker") + "|" + jstr(r, "route");
      auto it = first.find(key);
      if (it != first.end()) {
        out[it->second]["event_rows"] = out[it->second].value("event_rows", 1) + 1;
        continue;
      }
      first[key] = out.size();
      r["event_rows"] = 1;
      out.push_back(std::move(r));
    }
    rows = std::move(out);
  };
  one_per_event(lanes);
  one_per_event(research);
  json rows = json::array();
  int rank = 0;
  for (auto& r : lanes) {
    if (limit > 0 && rank >= limit) break;
    r["rank"] = ++rank;
    rows.push_back(std::move(r));
  }
  if (include_research)
    for (auto& r : research) {
      if (limit > 0 && static_cast<int>(rows.size()) >= 2 * limit) break;
      r["rank"] = nullptr;
      rows.push_back(std::move(r));
    }
  // ---- funnel and reason histogram -------------------------------------------------------
  // Built from the UNFILTERED screen, because the whole point is to account for the rows the
  // board does not show. An empty board is the normal state of a working router, and without
  // this the operator cannot tell it apart from a broken one.
  // Counted as DISTINCT MARKETS, not rows. A row is one side of one market, so counting rows made
  // the funnel read "2124 markets -> 4248 priced", which is not a funnel: the second number was
  // sides and the first was markets. Every step now counts the same thing.
  std::set<std::string> priced_mkts, edge_mkts, auto_mkts, manual_mkts;
  int f_auto = 0, f_manual = 0;
  std::map<std::string, int> reason_counts;
  if (screen.contains("rows") && screen["rows"].is_array())
    for (const auto& r : screen["rows"]) {
      const std::string v = jstr(r, "verdict");
      const std::string tk = jstr(r, "ticker");
      if (jnum(r, "fair", 0) > 0 || jnum(r, "p_yes", 0) > 0) priced_mkts.insert(tk);
      if (jnum(r, "executable_edge", -1) > 0) edge_mkts.insert(tk);
      if (v == "algo") { auto_mkts.insert(tk); ++f_auto; }
      else if (v == "manual") { manual_mkts.insert(tk); ++f_manual; }
      else if (r.contains("reasons") && r["reasons"].is_array()) {
        // Only non-actionable rows contribute: these are the answers to "why not?".
        for (const auto& x : r["reasons"])
          if (x.is_string()) ++reason_counts[x.get<std::string>()];
      }
    }
  json reasons = json::array();
  {
    std::vector<std::pair<std::string, int>> v(reason_counts.begin(), reason_counts.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [k, n] : v) reasons.push_back({{"reason", k}, {"count", n}});
  }
  const json counts = screen.value("counts", json::object());
  const json funnel = {{"watched", jnum(counts, "markets", 0)},
                       {"priced", static_cast<int>(priced_mkts.size())},
                       {"edge", static_cast<int>(edge_mkts.size())},
                       {"actionable", static_cast<int>(auto_mkts.size() + manual_mkts.size())},
                       {"auto", static_cast<int>(auto_mkts.size())},
                       {"manual", static_cast<int>(manual_mkts.size())},
                       {"sides", jnum(counts, "sides", 0)}};
  const json settings = screen.value("settings", json::object());
  return json{{"generated_ts", screen.value("generated_ts", 0LL)},
              {"counts", counts},
              {"funnel", funnel},
              {"reasons", reasons},
              {"algorithm", screen.value("algorithm", json::object())},
              {"market_data_max_age_seconds", screen.value("market_data_max_age_seconds", 0)},
              {"rows", rows},
              {"floors", {{"min_edge", jnum(settings, "min_edge")},
                          {"manual_min_edge", jnum(settings, "manual_min_edge")},
                          {"uncertainty_buffer", jnum(settings, "uncertainty_buffer")},
                          {"slippage_estimate", jnum(settings, "slippage_estimate")},
                          {"auto_max_sigma", jnum(settings, "auto_max_sigma")}}}};
}

json Engine::decision_settings() {
  {
    std::lock_guard<std::mutex> lk(decisions_mu_);
    if (cycle_screen_.contains("settings") && cycle_screen_["settings"].is_object()) return cycle_screen_["settings"];
  }
  return validate_systematic_settings(json::object(), systematic_.settings_copy());   // before the first cycle
}

void Engine::precheck_manual_decision(const json& b) {
  const std::string did = b.value("decision_id", std::string());
  json shown;
  {
    std::lock_guard<std::mutex> lk(decisions_mu_);
    auto it = recent_decisions_.find(did);
    if (it != recent_decisions_.end()) shown = it->second.first;
  }
  if (!shown.is_object())
    throw std::runtime_error("the row you reviewed (decision " + did + ") is no longer on the board; review the current row");
  if (shown.value("ticker", std::string()) != b.value("ticker", std::string()) ||
      shown.value("side", std::string()) != b.value("outcome_side", std::string()))
    throw std::runtime_error("the ticket does not match decision " + did);
  const std::string route = shown.value("route", std::string());
  if (route == "auto") throw std::runtime_error("that row is the algorithm's to send (AUTO); it is not offered to you");
  if (route != "manual") throw std::runtime_error("that row is not offered to you (" + route + ")");
  // The price and style you chose, on the book and probability you were shown: a ticket under the
  // manual floor is refused at the click rather than a cycle later. The full revalidation, on a
  // fresh book, still runs just before the order goes. (The cycle's settings: this runs on the
  // HTTP thread inside the queue's lock, and the algorithm's own lock is held across live sends.)
  const json settings = decision_settings();
  const std::string style = b.value("post_only", true) ? "maker" : "taker";
  const double price = b.value("price", 0.0);
  const EdgeCandidate at = decision_edge_at(shown, style, price, settings);
  const double floor = settings.value("manual_min_edge", 0.05);
  if (!at.executable || at.executable_edge + 1e-9 < floor)
    throw std::runtime_error("at " + fmt2(price) + " (" + style + ") the edge after the fee, slippage and the uncertainty buffer is " +
                             fmt2(at.executable_edge) + ", under the " + fmt2(floor) + " manual floor");
}

json Engine::revalidate_manual_decision(const json& b, const std::string& request_id) {
  const long long now = now_s();
  const std::string did = b.value("decision_id", std::string()), ticker = b.value("ticker", std::string()),
                    side = b.value("outcome_side", std::string("yes"));
  const double price = b.value("price", 0.0), count = b.value("count", 0.0);
  const std::string style = b.value("post_only", true) ? "maker" : "taker";
  json shown;
  {
    std::lock_guard<std::mutex> lk(decisions_mu_);
    auto it = recent_decisions_.find(did);
    if (it != recent_decisions_.end()) shown = it->second.first;
  }
  const std::string note = "manual request " + request_id;
  auto refuse = [&](const std::string& why) {
    json r = shown.is_object() ? shown : json{{"decision_id", did}, {"ticker", ticker}, {"side", side}, {"route", "manual"}};
    record_decision(store_, r, "rejected", now, 0, note + ": " + why, count, price, style);
    throw std::runtime_error(why + "; nothing sent");
  };
  if (!shown.is_object()) refuse("the row you reviewed is no longer on the board (decision " + did + ")");
  if (shown.value("ticker", std::string()) != ticker || shown.value("side", std::string()) != side)
    refuse("the ticket does not match the decision it names");
  // An estimate that is merely a little old is a wait, not a verdict: the queue's first pass of a
  // cycle runs before the strategies step, so after a slow cycle the model's number can sit just
  // past the entry limit. The request stays queued, inside its 90 s review window, and is checked
  // again after the next estimate. One that is missing, or long gone, is refused below with why.
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    auto ft = snap_fair_ts_.find(ticker);
    if (ft != snap_fair_ts_.end() && now - ft->second > cfg_.data_max_age() && now - ft->second <= 120)
      throw RetryManualOrder("waiting for the model's next estimate before the last check; nothing sent yet");
  }
  // The book as it is now, straight from the exchange, and the model's latest number: never the
  // screen the browser was looking at.
  Market mk = client_.get_market(ticker);
  store_.upsert_market(mk);
  mk.quote_ts = now;
  json row;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    row = board_row_locked(mk, now, cfg_.data_max_age(), std::max(60, cfg_.poll_seconds * 3));
  }
  RouterContext ctx = router_context(now, false);   // this request's own queue reservation is not a conflict
  ctx.max_age = cfg_.data_max_age();
  const json settings = validate_systematic_settings(json::object(), systematic_.settings_copy());
  const TradeDecision d = decide(row, side_from(side), settings, ctx);
  json fresh = decision_to_row(d);
  if (fresh.value("model_key", std::string()) != shown.value("model_key", std::string()))
    refuse("the model behind this row changed (" + shown.value("model_key", std::string()) + " -> " +
           fresh.value("model_key", std::string()) + ")");
  if (d.route == DecisionRoute::Auto) refuse("this row is now the algorithm's to send (AUTO)");
  if (d.route != DecisionRoute::Manual)
    refuse("this row is no longer offered to you (" + d.verdict + "): " + (d.reasons.empty() ? std::string() : d.reasons.front()));
  // The edge at the price and style you chose, from the same engine and the same floor.
  EdgeSettings es;
  es.uncertainty_multiplier = settings.value("uncertainty_buffer", 0.5);
  es.taker_slippage = settings.value("slippage_estimate", 0.0);
  const FeeModel fm{row.value("fee_type", std::string("quadratic")), row.value("fee_multiplier", 1.0)};
  const EdgeCandidate at = candidate_at(d.candidate.fair, normalize_prices_from_row(row), side_from(side), style, price, fm, es);
  const double floor = settings.value("manual_min_edge", 0.05);
  if (!at.executable || at.executable_edge + 1e-9 < floor)
    refuse("at your price " + fmt2(price) + " (" + style + ") the edge after the fee, slippage and the uncertainty buffer is " +
           fmt2(at.executable_edge) + ", under the " + fmt2(floor) + " manual floor");
  // The cap an order on an unproven model's number has always had: the flat fraction of the
  // bankroll the old proposal lane sized every model signal against. A hand ticket (no decision)
  // is the operator's own and is not capped here, as it never was.
  const std::string model_name = fresh.value("model", std::string());
  if (!model_name.empty() && !calibration_for(model_name).demonstrated) {
    double bankroll = cfg_.bankroll_dollars;
    if (auto a = store_.last_account()) bankroll = std::min(bankroll, std::max(0.0, a->balance + a->portfolio_value));
    const double flat = cfg_.risk.uncalibrated_max_order_frac * bankroll;
    const double cost = count * (at.entry_price + at.entry_fee);
    if (cost > flat + 1e-9)
      refuse(model_name + " has not beaten the price on settled markets yet, so an order on its number is capped at $" +
             fmt2(flat) + "; this one costs $" + fmt2(cost));
  }
  record_decision(store_, shown, "approved", now, 0, note, count, price, style);
  // What is actually sent: the fresh decision at your price and style.
  fresh["price"] = at.entry_price;
  fresh["style"] = at.style;
  fresh["fee"] = at.entry_fee;
  fresh["slippage"] = at.estimated_slippage;
  fresh["raw_edge"] = at.raw_edge;
  fresh["executable_edge"] = at.executable_edge;
  fresh["approved_from"] = did;
  record_decision(store_, fresh, "revalidated", now, 0, note + " (approved from " + did + ")", count, price, style);
  return fresh;
}

json Engine::decision_trace(const std::string& order_id) { return desk::decision_trace(store_, order_id); }

}  // namespace desk
