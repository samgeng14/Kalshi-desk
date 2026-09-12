#include "desk/hedge.h"

#include "desk/edge.h"

#include <algorithm>
#include <cmath>

#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/timeutil.h"

namespace desk {

std::vector<Holding> event_holdings(Store& store, const std::string& event_ticker) {
  std::vector<Holding> out;
  for (const auto& p : store.positions()) {
    if (p.position == 0) continue;
    auto m = store.get_market(p.ticker);
    if (!m || m->event_ticker != event_ticker) continue;
    out.push_back(Holding{p.ticker, p.position > 0, std::fabs(p.position), std::fabs(p.exposure), false});
  }
  for (const auto& o : store.orders("resting")) {
    auto m = store.get_market(o.ticker);
    if (!m || m->event_ticker != event_ticker) continue;
    double price = o.outcome_side == "yes" ? o.yes_price : o.no_price;
    double cost = o.remaining_count * price;
    if (auto s = store.get_series(m->series_ticker)) cost += FeeModel::from_series(*s).fee(price, o.remaining_count, false);
    out.push_back(Holding{o.ticker, o.outcome_side == "yes", o.remaining_count, cost, true});
  }
  return out;
}

namespace {

double worst_of(const std::vector<StatePayoff>& states) {
  double worst = 0;
  for (const auto& s : states) worst = std::max(worst, s.loss);
  return worst;
}

// Market-implied expected P&L: -sum(prob * loss). Only meaningful when every state carries a
// probability, which needs a two-sided quote in every market of the event.
bool expected_pnl(const std::vector<StatePayoff>& states, double& out) {
  double total_p = 0, ev = 0;
  for (const auto& s : states) {
    if (s.prob < 0) return false;
    total_p += s.prob;
    ev += s.prob * -s.loss;
  }
  if (total_p < 0.80 || total_p > 1.25) return false;  // quotes too wide to read as probabilities
  out = ev / total_p;
  return true;
}

}  // namespace

HedgePlan plan_hedge(Store& store, const Config& cfg, const std::string& event_ticker,
                     const std::vector<Holding>& existing, const std::optional<Holding>& add,
                     double loss_limit) {
  HedgePlan plan;
  EventStructure st = event_structure(store, event_ticker);
  std::vector<Holding> book = existing;
  if (add) book.push_back(*add);
  plan.have_structure = st.exclusive;
  plan.exhaustive = st.exhaustive;
  if (!st.exclusive) {
    plan.note = "no verified event structure yet: Kalshi has not confirmed these markets are mutually "
                "exclusive, so the desk cannot say which contract protects which. Nothing suggested.";
    plan.worst_before = event_risk(store, event_ticker, book).worst_loss;
    return plan;
  }
  plan.before = event_payoffs(store, event_ticker, book);
  plan.worst_before = worst_of(plan.before);
  plan.have_ev = expected_pnl(plan.before, plan.ev_before);
  if (plan.worst_before <= 0.01) {
    plan.note = "nothing at risk in this event.";
    return plan;
  }
  // The state that hurts: the hedge has to pay in it.
  std::vector<std::string> worst_states;
  for (const auto& s : plan.before)
    if (s.loss >= plan.worst_before - 1e-9) worst_states.push_back(s.ticker);

  double held_count = 0;
  for (const auto& h : book) held_count = std::max(held_count, h.count);
  double budget = std::max(1.0, std::min(plan.worst_before, cfg.risk.max_event_frac * cfg.bankroll_dollars));

  std::vector<HedgeLeg> legs;
  for (const auto& t : st.tickers) {
    // Hedging a trade you have not made yet with the same contract is not a hedge: it pays two
    // spreads to end up where a smaller entry would have. Only the other markets count.
    if (add && add->ticker == t) continue;
    auto m = store.get_market(t);
    if (!m || !m->tradable()) continue;
    auto series = store.get_series(m->series_ticker);
    FeeModel fm = series ? FeeModel::from_series(*series) : FeeModel{};
    long long mins = minutes_to_close(m->close_time, m->expected_expiration_time, now_s());
    if (mins != kUnknownMinutes && mins <= 0) continue;   // this leg cannot be bought any more
    for (const char* side : {"yes", "no"}) {
      bool yes = std::string(side) == "yes";
      // Taker prices: a hedge that does not fill is not a hedge. (The maker price one tick
      // inside is reported in the note so you can post it instead when there is time.)
      const MarketSidePrices touch = normalize_prices(*m);   // NO ask = 1 - YES bid, resting on the YES bid
      double px = touch.ask(yes ? Side::Yes : Side::No);
      double avail = touch.ask_size(yes ? Side::Yes : Side::No);
      if (px <= 0 || px >= 1) continue;
      if (px < cfg.risk.min_price || px > 1 - cfg.risk.min_price) continue;
      double max_n = std::floor(budget / px);
      if (avail > 0) max_n = std::min(max_n, std::floor(avail));
      max_n = std::min(max_n, std::ceil(held_count) + 5);
      for (double n = 1; n <= max_n; ++n) {
        HedgeLeg leg;
        leg.ticker = t;
        leg.side = side;
        leg.count = n;
        leg.price = px;
        leg.fee_per = fm.fee_per_contract(px, true);
        leg.cost = n * (px + leg.fee_per);
        if (leg.cost > budget + 1e-9) break;
        std::vector<Holding> with = book;
        with.push_back(Holding{t, yes, n, leg.cost, false});
        leg.after = event_payoffs(store, event_ticker, with);
        leg.worst_before = plan.worst_before;
        leg.worst_after = worst_of(leg.after);
        leg.ev_before = plan.ev_before;
        leg.have_ev = plan.have_ev && expected_pnl(leg.after, leg.ev_after);
        if (leg.worst_after >= plan.worst_before - 0.01) continue;  // no protection: skip
        for (size_t i = 0; i < st.tickers.size(); ++i)
          if (st.tickers[i] == t) leg.label = st.labels[i];
        legs.push_back(std::move(leg));
      }
    }
  }
  if (legs.empty()) {
    plan.note = "no contract in this event reduces the worst case at the current offers.";
    return plan;
  }
  // Prefer legs that bring the worst case under the limit; among those, the cheapest in expected
  // value (least insurance premium). Otherwise the biggest worst-case cut per dollar spent.
  auto under = [&](const HedgeLeg& l) { return l.worst_after <= loss_limit + 1e-9; };
  std::sort(legs.begin(), legs.end(), [&](const HedgeLeg& a, const HedgeLeg& b) {
    if (under(a) != under(b)) return under(a);
    if (under(a) && under(b)) {
      if (a.have_ev && b.have_ev && std::fabs(a.ev_after - b.ev_after) > 1e-9) return a.ev_after > b.ev_after;
      return a.cost < b.cost;
    }
    double ra = (a.worst_before - a.worst_after) / std::max(0.01, a.cost);
    double rb = (b.worst_before - b.worst_after) / std::max(0.01, b.cost);
    if (std::fabs(ra - rb) > 1e-9) return ra > rb;
    return a.worst_after < b.worst_after;
  });
  // One line per (ticker, side): the sizes in between are noise on the screen.
  std::vector<HedgeLeg> best;
  for (const auto& l : legs) {
    bool seen = false;
    for (const auto& b : best) if (b.ticker == l.ticker && b.side == l.side) seen = true;
    if (!seen) best.push_back(l);
    if (best.size() >= 4) break;
  }
  for (auto& l : best) {
    char buf[240];
    std::snprintf(buf, sizeof buf, "worst case %s$%.2f -> $%.2f for $%.2f%s", l.worst_before < 0 ? "-" : "",
                  std::fabs(l.worst_before), l.worst_after, l.cost,
                  l.have_ev ? (l.ev_after >= l.ev_before ? ", expected value improves" : ", costs expected value") : "");
    l.note = buf;
  }
  plan.options = std::move(best);
  if (!plan.options.empty() && plan.options.front().worst_after > loss_limit)
    plan.note = "nothing gets the worst case under $" + fmt2(loss_limit) + "; the best available cut is shown.";
  return plan;
}

}  // namespace desk
