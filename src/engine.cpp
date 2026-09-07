#include "desk/engine.h"

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

namespace desk {

Engine::Engine(Config cfg, Store& store, KalshiClient& client, Executor& exec)
    : cfg_(std::move(cfg)), store_(store), client_(client), exec_(exec) {
  strategies_ = build_strategies(cfg_);
  for (const auto& s : cfg_.watch_series) watch_.insert(s);
  // Persisted state overrides the config file.
  if (auto k = store_.kv_get("kill")) kill_ = (*k == "1");
  if (auto r = store_.kv_get("kill_reason")) kill_reason_ = *r;
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

std::vector<ProposalCandidate> Engine::candidates(const Market& m, const Series& s, const Fair& f) const {
  std::vector<ProposalCandidate> out;
  FeeModel fm = FeeModel::from_series(s);
  const RiskConfig& r = cfg_.risk;
  double tick = m.tick > 0 ? m.tick : 0.01;
  double lo = r.min_price, hi = 1.0 - r.min_price;

  auto consider = [&](const std::string& side, const std::string& style, double price, double p_win) {
    if (price < lo || price > hi) return;
    bool taker = style == "taker";
    double fee = fm.fee_per_contract(price, taker);
    double edge = p_win - price - fee;
    double min_edge = taker ? r.min_edge_taker : r.min_edge_maker;
    if (edge < min_edge) return;
    if (edge < r.sigma_mult * f.sigma) return;
    ProposalCandidate c;
    c.outcome_side = side;
    c.exec_style = style;
    c.price = price;
    c.fee_per = fee;
    c.net_edge = edge;
    c.p_win = p_win;
    out.push_back(c);
  };

  double p = f.p, q = 1.0 - f.p;
  // YES side. Taker: lift the ask. Maker: post a bid at p - min_edge, not crossing the ask,
  // and only if it joins or improves the current best bid (otherwise it never fills).
  if (m.yes_ask > 0 && m.yes_ask < 1) consider("yes", "taker", m.yes_ask, p);
  {
    double L = floor_to_tick(p - r.min_edge_maker, tick);
    if (m.yes_ask > 0) L = std::min(L, m.yes_ask - tick);
    if (L >= m.yes_bid && L > 0) consider("yes", "maker", L, p);
  }
  // NO side. NO ask = 1 - yes_bid; NO bid = 1 - yes_ask.
  double no_ask = m.no_ask > 0 ? m.no_ask : (m.yes_bid > 0 ? 1.0 - m.yes_bid : 0);
  double no_bid = m.no_bid > 0 ? m.no_bid : (m.yes_ask > 0 ? 1.0 - m.yes_ask : 0);
  if (no_ask > 0 && no_ask < 1) consider("no", "taker", no_ask, q);
  {
    double L = floor_to_tick(q - r.min_edge_maker, tick);
    if (no_ask > 0) L = std::min(L, no_ask - tick);
    if (L >= no_bid && L > 0) consider("no", "maker", L, q);
  }
  std::sort(out.begin(), out.end(),
            [](const ProposalCandidate& a, const ProposalCandidate& b) { return a.net_edge > b.net_edge; });
  return out;
}

double Engine::size_for(const ProposalCandidate& c, const Market& m, double bankroll, double event_used,
                        double cluster_used, double open_used) const {
  const RiskConfig& r = cfg_.risk;
  double cost = c.price + c.fee_per;
  double n = size_contracts(bankroll, c.p_win, cost, r.kelly_frac, r.max_event_frac);
  double room_event = r.max_event_frac * bankroll - event_used;
  double room_cluster = r.max_cluster_frac * bankroll - cluster_used;
  double room_open = r.max_open_frac * bankroll - open_used;
  double room = std::min({room_event, room_cluster, room_open});
  if (room <= 0) return 0;
  n = std::min(n, std::floor(room / cost));
  if (c.exec_style == "taker") {
    double avail = c.outcome_side == "yes" ? m.yes_ask_size : m.yes_bid_size;  // NO ask == YES bid size
    if (avail <= 0) return 0;  // displayed size unknown: never lift an invisible offer
    n = std::min(n, std::floor(avail));
  }
  return n < 1 ? 0 : n;
}

std::map<std::string, double> Engine::exposure_by(const std::string& kind, bool include_proposals) {
  auto exposure=committed_exposure(store_);
  auto out=kind=="cluster"?exposure.clusters:exposure.events;
  if(include_proposals) for(const auto& p:store_.proposals("pending",500)) {
    if (p.strategy == "exit" || p.strategy == "close" || p.strategy == "target" || p.strategy == "stop") continue;  // reduce risk
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
  long long quote_ttl = 3 * std::max(1, cfg_.poll_seconds) + 10;
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
  auto by_event = exposure_by("event");
  auto by_cluster = exposure_by("cluster");
  double open_used = open_exposure();
  // Payoff-space book per event (positions + resting orders) plus pending proposals as if filled:
  // used to refuse a proposal that merely stacks the bet an event already carries.
  Exposure book = committed_exposure(store_);
  std::map<std::string, std::vector<std::pair<long long, Holding>>> pending_by_event;  // proposal id -> holding
  for(const auto& p:store_.proposals("pending",500)) {
    if (p.strategy == "exit" || p.strategy == "close" || p.strategy == "target" || p.strategy == "stop") continue;
    open_used += p.count*(p.price+p.fee_per);
    pending_by_event[p.event_ticker].push_back({p.id, Holding{p.ticker, p.outcome_side == "yes", p.count, p.count * (p.price + p.fee_per), true}});
  }
  // Every mutation below refreshes the reservation book. Otherwise new same-cycle proposals
  // escape the overlap check, and withdrawn proposals keep blocking unrelated candidates.
  auto forget_pending = [&](long long id) {
    for (auto& [ev, ps] : pending_by_event)
      ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const auto& item){ return item.first == id; }), ps.end());
  };
  auto remember_pending = [&](const Proposal& p) {
    forget_pending(p.id);
    pending_by_event[p.event_ticker].push_back({p.id, Holding{p.ticker, p.outcome_side == "yes", p.count,
                                                          p.count * (p.price + p.fee_per), true}});
  };
  auto release_pending = [&](const Proposal& p) {
    forget_pending(p.id);
    double reserved = p.count * (p.price + p.fee_per);
    auto m = store_.get_market(p.ticker);
    by_event[p.event_ticker] -= reserved;
    by_cluster[m ? risk_cluster(store_, *m) : p.cluster] -= reserved;
    open_used -= reserved;
  };
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
            release_pending(*old_p);
          }
  fair_set_this_cycle_.clear();
  double bankroll = cfg_.bankroll_dollars;
  if (auto a=store_.last_account()) bankroll=std::min(bankroll,std::max(0.0,a->balance+a->portfolio_value));
  auto positions = store_.positions();
  std::map<std::string, std::vector<Order>> resting;
  for (const auto& o : store_.orders("resting")) resting[o.ticker].push_back(o);

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
      double raw_p = std::min(0.999, std::max(0.001, f->p));
      // Log every raw estimate (traded or not) at most every 5 minutes: this is the Brier sample.
      std::string fk = m.ticker + "|" + st->name();
      if (now - last_fair_log_[fk] >= 300) {
        store_.insert_model_fair(now, m.ticker, st->name(), raw_p, f->sigma, m.yes_bid, m.yes_ask);
        last_fair_log_[fk] = now;
      }
      // Shrink toward the market until the strategy has earned trust: p = (1-w) model + w mid.
      double w = market_weight(st->name());
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
            if (auto pr0 = store_.get_proposal(pid0))
              if (pr0->strategy == "target" || pr0->strategy == "stop") continue;  // your bracket, not the model's call
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
              release_pending(*old);
            } else if (old->status == "submitted" && !old->order_id.empty()) {
              // A live resting order on a number we no longer trust: pull it.
              try { exec_.cancel(old->order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", old->order_id.c_str(), e.what()); }
            }
          }
        continue;
      }
      if (!governs) continue;  // a second strategy on the same ticker: logged above, never traded
      // Killed: keep researching (fairs are logged above), propose nothing.
      if (kill_) continue;
      // Existing position in this market: never add to it, never flip it here.
      auto pit = std::find_if(positions.begin(), positions.end(),
                              [&](const Position& p) { return p.ticker == m.ticker; });
      if (pit != positions.end() && pit->position != 0) continue;

      auto cands = candidates(m, s, *f);
      if (cands.empty()) {
        // Withdraw a stale pending proposal if the edge is gone.
        for (const char* side : {"yes", "no"})
          if (auto old = store_.find_open_proposal(m.ticker, side, st->name()))
            if (old->status == "pending") {
              old->status = "expired";
              old->note = "edge gone";
              store_.update_proposal(*old);
              release_pending(*old);
            }
        continue;
      }
      const ProposalCandidate& c = cands.front();
      std::string cluster = risk_cluster(store_, m);
      // Already resting / pending for this ticker+side+strategy: refresh numbers instead of stacking.
      auto existing = store_.find_open_proposal(m.ticker, c.outcome_side, st->name());
      if (existing && existing->status == "submitted") continue;
      // A pending proposal keeps the room it already holds: size it without counting itself.
      double self_used = existing ? existing->count * (existing->price + existing->fee_per) : 0.0;
      double n = size_for(c, m, bankroll, by_event[m.event_ticker] - self_used, by_cluster[cluster] - self_used,
                          open_used - self_used);
      if (std::getenv("DESK_DEBUG_SIZING"))
        LOG_INFO("size %s ev_used %.2f cl_used %.2f self %.2f open %.2f -> n %.0f @ %.2f (p_win %.3f)",
                 m.ticker.c_str(), by_event[m.event_ticker], by_cluster[cluster], self_used, open_used, n, c.price,
                 c.p_win);
      if (n < 1) {
        if (existing) {  // no room left for it: withdraw rather than leave a stale number
          existing->status = "expired";
          existing->note = "no room";
          store_.update_proposal(*existing);
          forget_pending(existing->id);
          by_event[m.event_ticker] -= self_used;
          by_cluster[cluster] -= self_used;
          open_used -= self_used;
        }
        continue;
      }
      // Same bet twice in one event (LAX B79.5 YES + LAX T80 NO, 2026-09-05): refuse it here.
      {
        std::vector<Holding> held = book.holdings[m.event_ticker];
        for (const auto& [pid, h] : pending_by_event[m.event_ticker])
          if (!existing || pid != existing->id) held.push_back(h);
        Holding add{m.ticker, c.outcome_side == "yes", n, n * (c.price + c.fee_per)};
        if (stacks_event(store_, m.event_ticker, held, add)) {
          if (existing && existing->status == "pending") {
            existing->status = "expired";
            existing->note = "stacks the bet already held in " + m.event_ticker;
            store_.update_proposal(*existing);
            forget_pending(existing->id);
            by_event[m.event_ticker] -= self_used;
            by_cluster[cluster] -= self_used;
            open_used -= self_used;
          }
          f->note += " | NOT PROPOSED: stacks the bet already held in " + m.event_ticker;
          latest_fair_[m.ticker] = *f;
          break;
        }
      }
      // Opposite side pending or resting: drop it; never work both sides of one market.
      if (auto opp = store_.find_open_proposal(m.ticker, c.outcome_side == "yes" ? "no" : "yes", st->name())) {
        if (opp->status == "pending") {
          opp->status = "expired";
          opp->note = "side flipped";
          store_.update_proposal(*opp);
          release_pending(*opp);
        } else if (opp->status == "submitted" && !opp->order_id.empty()) {
          try { exec_.cancel(opp->order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", opp->order_id.c_str(), e.what()); }
          continue;  // re-evaluate next cycle once the book is clean
        }
      }
      Proposal p = existing ? *existing : Proposal{};
      p.ticker = m.ticker;
      p.event_ticker = m.event_ticker;
      p.series_ticker = m.series_ticker;
      p.strategy = st->name();
      p.mode = mode;
      p.outcome_side = c.outcome_side;
      p.exec_style = c.exec_style;
      p.status = "pending";
      p.price = c.price;
      p.count = n;
      p.p_model = f->p;
      p.sigma = f->sigma;
      p.fee_per = c.fee_per;
      p.net_edge = c.net_edge;
      p.yes_bid = m.yes_bid;
      p.yes_ask = m.yes_ask;
      p.cluster = cluster;
      p.note = f->note;
      if (existing) {
        store_.update_proposal(p);
        by_event[m.event_ticker] += n * (c.price + c.fee_per) - self_used;
        by_cluster[cluster] += n * (c.price + c.fee_per) - self_used;
      } else {
        store_.insert_proposal(p);
        by_event[m.event_ticker] += n * (c.price + c.fee_per);
        by_cluster[cluster] += n * (c.price + c.fee_per);
      }
      open_used += n*(c.price+c.fee_per)-self_used;
      remember_pending(p);
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
  if(d["start_pv"].get<double>()>0 && d["pv"].get<double>()<=d["stop_pv"].get<double>() && !kill_)
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
  std::set<std::string> have_position;
  std::set<std::string> proposed_exits;
  for (const auto& pos : positions) {
    if (pos.position == 0) continue;
    have_position.insert(pos.ticker);
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
    if (existing && existing->status == "submitted") {
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
    double n = std::fabs(pit->position);
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
          try { exec_.cancel(o.order_id); } catch (const std::exception& e) { LOG_WARN("cancel %s: %s", o.order_id.c_str(), e.what()); blocked = true; }
        }
      if (blocked) continue;
      store_.exec("UPDATE proposals SET status='expired',note='stop hit' WHERE status='pending' AND ticker=? AND strategy IN ('target','exit')", {ticker});
      if (auto sp = store_.find_open_proposal(ticker, buy_side, "stop"))
        if (sp->status == "pending" || sp->status == "submitted") continue;  // already on its way
      Proposal p;
      p.ticker = ticker; p.event_ticker = m->event_ticker; p.series_ticker = m->series_ticker;
      p.strategy = "stop"; p.mode = "bot"; p.outcome_side = buy_side; p.exec_style = "taker"; p.status = "pending";
      p.price = 1.0 - bid; p.count = n; p.fee_per = fm.fee_per_contract(1.0 - bid, true);
      p.yes_bid = m->yes_bid; p.yes_ask = m->yes_ask; p.cluster = risk_cluster(store_, *m); p.ts = now; p.updated_ts = now;
      p.note = "STOP: sell " + std::string(long_yes ? "YES" : "NO") + " x" + fmt2(n) + " into the bid " + fmt2(bid) + " (stop " + fmt2(sl) + ")";
      store_.insert_proposal(p);
      continue;
    }
    // Target: keep one reduce-only maker order resting at tp for the whole position.
    if (tp > 0) {
      double tp_px = std::max(tick, std::min(1.0 - tick, tp));
      bool have = false;
      auto rit = resting.find(ticker);
      if (rit != resting.end())
        for (const auto& o : rit->second) {
          long long pid = store_.proposal_for_order(o.order_id);
          auto pr = pid ? store_.get_proposal(pid) : std::nullopt;
          double our_px = long_yes ? o.yes_price : o.no_price;
          if (pr && pr->strategy == "target" && std::fabs(our_px - tp_px) < tick / 2 && o.remaining_count >= n - 0.01) have = true;
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
  for (auto& p : store_.proposals("pending", 200)) {
    if (n >= cfg_.risk.max_orders_per_cycle) break;
    std::string why;
    bool reduces = is_exit_strategy(p.strategy);
    // The fast lane: an edge this big is gone by the time a human confirms it, so it goes now.
    // Mode still decides whether the strategy may trade at all - off and research never send -
    // but manual no longer means "wait" once the edge clears the bar.
    double lane = instant_edge();
    bool instant = !reduces && lane > 0 && p.net_edge >= lane - 1e-9 && p.mode != "off" && p.mode != "research";
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
      // The exchange refused an order we sent: stop unattended trading until a human looks.
      LOG_ERR("%s submit %s: api %d %s", why.c_str(), p.ticker.c_str(), e.status, e.body.substr(0, 160).c_str());
      last_error_ = e.what();
      if (e.status >= 400 && e.status < 500 && e.status != 429) set_kill(true, std::string("order rejected: ") + e.body.substr(0, 160));
      break;
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
  json status{{"env", cfg_.env},
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
              {"account_ts", acct ? acct->updated_ts : 0}};
  std::lock_guard<std::mutex> lk(snap_mu_);
  snap_fair_ = latest_fair_;
  snap_fair_strategy_ = latest_fair_strategy_;
  snap_fair_ts_ = latest_fair_ts_;
  snap_series_ = series_cache_;
  snap_watch_ = watch_;
  snap_strategies_ = strategies;
  snap_status_ = status;
}

void Engine::cycle() {
  std::lock_guard<std::mutex> lk(mu_);
  ++cycle_count_;
  apply_kill_sweep();
  bool ok = true;
  auto step = [&](const char* name, auto&& fn) {
    try { fn(); } catch (const std::exception& e) {
      LOG_ERR("cycle %s: %s", name, e.what());
      last_error_ = std::string(name) + ": " + e.what();
      ok = false;
    }
  };
  step("refresh", [&] { refresh_series(); refresh_markets(); refresh_event_meta(); });
  if ((cycle_count_ - 1) % std::max(1, cfg_.reconcile_every) == 0 || force_reconcile_.exchange(false))
    step("reconcile", [&] { exec_.reconcile(cycle_count_ == 1); update_daily_stop(); });
  step("strategies", [&] { run_strategies(); });
  if (cycle_count_ % 2 == 0) step("results", [&] { refresh_results(); });
  step("brackets", [&] { run_brackets(); });
  step("auto hedge", [&] { run_auto_hedge(); });
  step("exits", [&] { run_exits(); });
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
  double dflt = (strategy == "manual_fair") ? 0.0 : 0.5;
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
  const long long now = now_s(), stale_after = std::max(60, cfg_.poll_seconds * 3);
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
                      "('pending','submitted')", {ev}).empty()) continue;
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
    p.mode = mode;
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
                             const std::string& style) {
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
  room = std::min(room, cash);
  double n_caps = cost > 0 ? std::floor(std::max(0.0, room) / cost) : 0;
  double n = n_caps;
  std::string basis = "risk caps";
  double p_win = -1;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    auto f = snap_fair_.find(ticker);
    if (f != snap_fair_.end() && !f->second.research_only)
      p_win = side == "no" ? 1.0 - f->second.p : f->second.p;
  }
  if (p_win > 0) {
    double kelly = size_contracts(bankroll, p_win, cost, cfg_.risk.kelly_frac, cfg_.risk.max_event_frac);
    if (kelly < n) { n = kelly; basis = "quarter Kelly"; }
  }
  return json{{"count", std::max(0.0, n)}, {"cost_per", cost}, {"fee_per", fee}, {"room", std::max(0.0, room)},
              {"basis", basis}, {"cap_event", std::max(0.0, room_event)}, {"cap_cluster", std::max(0.0, room_cluster)},
              {"cap_open", std::max(0.0, room_open)}, {"cash", cash}, {"p_win", p_win}};
}

json Engine::board_view(int limit) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  const long long now = now_s(), stale_after = std::max(60, cfg_.poll_seconds * 3);
  struct Row { json j; double edge; };
  std::vector<Row> rows;
  rows.reserve(512);
  for (const auto& m : store_.open_markets()) {
    if (!snap_watch_.count(m.series_ticker)) continue;
    std::string category;
    double fee_mult = 1.0;
    std::string fee_type = "quadratic";
    auto sit = snap_series_.find(m.series_ticker);
    if (sit != snap_series_.end()) {
      category = sit->second.category;
      fee_type = sit->second.fee_type;
      fee_mult = sit->second.fee_multiplier;
    }
    double edge = -9;
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
              {"minutes_to_close", minutes_to_close(m.close_time, m.expected_expiration_time, now)},
              {"quote_stale", m.quote_ts <= 0 || now - m.quote_ts > stale_after},
              {"fair_stale", true},
              {"research_only", false},
              {"research_reason", ""}};
    auto f = snap_fair_.find(m.ticker);
    if (f != snap_fair_.end()) {
      j["p_model"] = f->second.p;
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
      // Edge a taker would keep on the better side, so the server can rank without the browser
      // parsing every contract on the exchange.
      FeeModel fm{fee_type, fee_mult};
      double p = f->second.p;
      double no_ask = m.no_ask > 0 ? m.no_ask : (m.yes_bid > 0 ? 1.0 - m.yes_bid : 0);
      if (m.yes_ask > 0 && m.yes_ask < 1) edge = std::max(edge, p - m.yes_ask - fm.fee_per_contract(m.yes_ask, true));
      if (no_ask > 0 && no_ask < 1) edge = std::max(edge, (1 - p) - no_ask - fm.fee_per_contract(no_ask, true));
    }
    j["edge"] = edge > -8 ? edge : 0.0;
    j["have_edge"] = edge > -8;
    rows.push_back({std::move(j), edge});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.edge > b.edge; });
  json out = json::array();
  int n = 0;
  for (auto& r : rows) {
    if (limit > 0 && n++ >= limit) break;
    out.push_back(std::move(r.j));
  }
  return json{{"total", rows.size()}, {"shown", out.size()}, {"markets", out}};
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

json Engine::status() {
  std::lock_guard<std::mutex> lk(snap_mu_);
  return snap_status_.is_null() ? json{{"env", cfg_.env}, {"cycle", 0}, {"watch", json::array()},
                                       {"strategies", json::array()}, {"kill", kill_.load()}, {"armed", exec_.armed()},
                                       {"markets_seen", 0}, {"bankroll", cfg_.bankroll_dollars},
                                       {"balance", 0.0}, {"portfolio_value", 0.0}, {"last_cycle_ts", 0},
                                       {"last_error", ""}, {"kill_reason", ""}, {"account_ts", 0}}
                                : snap_status_;
}

}  // namespace desk
