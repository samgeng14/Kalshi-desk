#include "desk/ladder.h"

#include <algorithm>
#include <cmath>

#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/risk.h"
#include "desk/timeutil.h"

namespace desk {

namespace {
// The settle values a contract pays on, as a closed integer interval. Kalshi strikes are
// integers: "less" with cap c pays when T <= c-1, "greater" with floor f pays when T >= f+1.
struct Span { double lo, hi; bool ok = false; };
Span span_of(const Market& m) {
  const double inf = 1e12;
  Span s;
  if (m.strike_type == "less" && m.cap_strike) { s = {-inf, *m.cap_strike - 1, true}; }
  else if (m.strike_type == "greater" && m.floor_strike) { s = {*m.floor_strike + 1, inf, true}; }
  else if (m.strike_type == "between" && m.floor_strike && m.cap_strike) { s = {*m.floor_strike, *m.cap_strike, true}; }
  return s;
}
bool contains(const Span& outer, const Span& inner) {
  return outer.ok && inner.ok && outer.lo <= inner.lo + 1e-9 && outer.hi >= inner.hi - 1e-9 &&
         !(outer.lo >= inner.lo - 1e-9 && outer.hi <= inner.hi + 1e-9);   // strictly wider
}
}  // namespace

std::optional<LadderArb> find_dominance_arb(Store& store, const Config& cfg, const std::string& event_ticker,
                                            const std::vector<const Market*>& markets, double max_cost) {
  long long now = now_s();
  LadderArb best;
  best.event_ticker = event_ticker;
  best.kind = "dominance";
  double best_edge = 0;
  for (const Market* hard : markets) {           // the contract with the narrower condition
    Span sh = span_of(*hard);
    if (!sh.ok || !hard->tradable() || hard->yes_bid <= 0 || hard->yes_bid >= 1 || hard->yes_bid_size <= 0) continue;
    long long mh = minutes_to_close(hard->close_time, hard->expected_expiration_time, now);
    if (mh != kUnknownMinutes && mh <= 1) continue;
    for (const Market* easy : markets) {         // the contract it implies
      if (easy == hard) continue;
      Span se = span_of(*easy);
      if (!contains(se, sh)) continue;           // hard's outcomes are a strict subset of easy's
      if (!easy->tradable() || !(easy->yes_ask > 0 && easy->yes_ask < 1) || easy->yes_ask_size <= 0) continue;
      long long me = minutes_to_close(easy->close_time, easy->expected_expiration_time, now);
      if (me != kUnknownMinutes && me <= 1) continue;
      auto s_hard = store.get_series(hard->series_ticker);
      auto s_easy = store.get_series(easy->series_ticker);
      FeeModel fh = s_hard ? FeeModel::from_series(*s_hard) : FeeModel{};
      FeeModel fe = s_easy ? FeeModel::from_series(*s_easy) : FeeModel{};
      double sell_px = 1.0 - hard->yes_bid;      // buying NO on the harder contract
      double buy_px = easy->yes_ask;             // buying YES on the easier one
      double fee_sell = fh.fee_per_contract(sell_px, true), fee_buy = fe.fee_per_contract(buy_px, true);
      // Every state pays at least $1: hard true -> easy true (the YES leg pays); hard false ->
      // the NO leg pays. The middle state pays $2.
      double edge = hard->yes_bid - easy->yes_ask - fee_sell - fee_buy;
      if (edge <= best_edge) continue;
      double cost = sell_px + fee_sell + buy_px + fee_buy;
      double size = std::min(hard->yes_bid_size, easy->yes_ask_size);
      double count = std::floor(std::min(size, max_cost > 0 ? max_cost / std::max(0.01, cost) : size));
      if (count < 1) continue;
      best_edge = edge;
      best.legs = {LadderLeg{hard->ticker, !hard->yes_sub_title.empty() ? hard->yes_sub_title : hard->subtitle, "no",
                             sell_px, fee_sell, hard->yes_bid, hard->yes_ask, hard->yes_bid_size},
                   LadderLeg{easy->ticker, !easy->yes_sub_title.empty() ? easy->yes_sub_title : easy->subtitle, "yes",
                             buy_px, fee_buy, easy->yes_bid, easy->yes_ask, easy->yes_ask_size}};
      best.sum_prices = hard->yes_bid - easy->yes_ask;
      best.edge_per_contract = edge;
      best.cost_per_contract = cost;
      best.count = count;
    }
  }
  if (best.legs.empty() || best.edge_per_contract < cfg.risk.ladder_min_edge) return std::nullopt;
  best.total_edge = best.edge_per_contract * best.count;
  best.total_cost = best.cost_per_contract * best.count;
  char buf[360];
  std::snprintf(buf, sizeof buf,
                "dominance arb: %s (%s) implies %s (%s), yet the book bids %.2f for the harder one and offers the "
                "easier at %.2f. Selling the harder and buying the easier pays at least $1 in every state for "
                "$%.3f, locking $%.3f per contract after taker fees",
                best.legs[0].ticker.c_str(), best.legs[0].label.c_str(), best.legs[1].ticker.c_str(),
                best.legs[1].label.c_str(), best.legs[0].yes_bid, best.legs[1].yes_ask, best.cost_per_contract,
                best.edge_per_contract);
  best.note = buf;
  return best;
}

std::optional<LadderArb> find_ladder_arb(Store& store, const Config& cfg, const std::string& event_ticker,
                                         const std::vector<const Market*>& markets, double max_cost) {
  EventStructure st = event_structure(store, event_ticker);
  if (!st.exclusive) return std::nullopt;   // the whole argument rests on the exchange's flag
  if (markets.size() < 2) return std::nullopt;
  long long now = now_s();

  struct Cand {
    const Market* m;
    double bid, ask, bid_size, ask_size, fee_no, fee_yes;
    std::string label;
  };
  std::vector<Cand> cands;
  double sum_asks = 0, sum_ask_fees = 0;
  bool every_bracket_offered = true;
  for (const Market* m : markets) {
    if (!m->tradable()) return std::nullopt;   // a halted bracket breaks the argument
    long long mins = minutes_to_close(m->close_time, m->expected_expiration_time, now);
    if (mins != kUnknownMinutes && mins <= 1) return std::nullopt;  // no time to leg in
    auto series = store.get_series(m->series_ticker);
    FeeModel fm = series ? FeeModel::from_series(*series) : FeeModel{};
    Cand c;
    c.m = m;
    c.bid = m->yes_bid;
    c.ask = m->yes_ask > 0 && m->yes_ask < 1 ? m->yes_ask : 0;
    c.bid_size = m->yes_bid_size;
    c.ask_size = m->yes_ask_size;
    c.fee_no = fm.fee_per_contract(1.0 - c.bid, true);   // buying NO at 1 - bid
    c.fee_yes = c.ask > 0 ? fm.fee_per_contract(c.ask, true) : 0;
    c.label = !m->yes_sub_title.empty() ? m->yes_sub_title : m->subtitle;
    if (c.ask > 0 && c.ask_size > 0) { sum_asks += c.ask; sum_ask_fees += c.fee_yes; }
    else every_bracket_offered = false;
    if (c.bid > 0 && c.bid < 1 && c.bid_size > 0) cands.push_back(c);
  }

  LadderArb best;
  best.event_ticker = event_ticker;

  // ---- sell side: any subset whose YES bids sum past $1 -------------------------------------
  // Adding a bracket adds its bid and its fee, so a leg helps whenever bid > fee. Cash is the
  // binding constraint (each NO contract ties up about 1 - bid), so take the fattest bids first
  // and stop when the next leg costs more cash than the edge it buys.
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.bid > b.bid; });
  {
    double sum_bid = 0, sum_fee = 0, cost = 0;
    std::vector<LadderLeg> legs;
    double size = 0;
    for (const auto& c : cands) {
      if (c.bid <= c.fee_no) continue;                    // this leg cannot pay for itself
      double next_sum = sum_bid + c.bid, next_fee = sum_fee + c.fee_no;
      double next_cost = cost + (1.0 - c.bid) + c.fee_no;
      double next_edge = next_sum - 1.0 - next_fee;
      double edge_now = sum_bid - 1.0 - sum_fee;
      // Before the sum passes $1 there is no locked trade yet, so keep adding; after that, only
      // add a leg when it improves the return on the cash committed.
      bool improves = edge_now <= 0 ? true : (next_edge / next_cost) > (edge_now / cost);
      if (!improves) break;
      sum_bid = next_sum;
      sum_fee = next_fee;
      cost = next_cost;
      size = legs.empty() ? c.bid_size : std::min(size, c.bid_size);
      legs.push_back(LadderLeg{c.m->ticker, c.label, "no", 1.0 - c.bid, c.fee_no, c.bid, c.ask, c.bid_size});
    }
    double edge = sum_bid - 1.0 - sum_fee;
    if (legs.size() >= 2 && edge > 0) {
      best.kind = "sell";
      best.legs = legs;
      best.sum_prices = sum_bid;
      best.edge_per_contract = edge;
      best.cost_per_contract = cost;
      best.count = std::floor(std::min(size, max_cost > 0 ? max_cost / std::max(0.01, cost) : size));
    }
  }

  // ---- buy side: the whole ladder for less than the dollar it must pay ------------------------
  // This one needs the brackets to cover every outcome, or "nothing settles YES" is a total loss.
  if (best.kind.empty() && st.exhaustive && every_bracket_offered && sum_asks > 0) {
    double edge = 1.0 - sum_asks - sum_ask_fees;
    if (edge > 0) {
      std::vector<LadderLeg> legs;
      double size = 0, cost = 0;
      bool ok = true;
      for (const Market* m : markets) {
        auto series = store.get_series(m->series_ticker);
        FeeModel fm = series ? FeeModel::from_series(*series) : FeeModel{};
        double ask = m->yes_ask;
        if (!(ask > 0 && ask < 1) || m->yes_ask_size <= 0) { ok = false; break; }
        double fee = fm.fee_per_contract(ask, true);
        size = legs.empty() ? m->yes_ask_size : std::min(size, m->yes_ask_size);
        cost += ask + fee;
        legs.push_back(LadderLeg{m->ticker, !m->yes_sub_title.empty() ? m->yes_sub_title : m->subtitle, "yes",
                                 ask, fee, m->yes_bid, ask, m->yes_ask_size});
      }
      if (ok && legs.size() >= 2) {
        best.kind = "buy";
        best.legs = legs;
        best.sum_prices = sum_asks;
        best.edge_per_contract = edge;
        best.cost_per_contract = cost;
        best.count = std::floor(std::min(size, max_cost > 0 ? max_cost / std::max(0.01, cost) : size));
      }
    }
  }

  if (best.kind.empty() || best.count < 1) return std::nullopt;
  // A basket worth less than a cent a contract is noise: one tick of movement while legging in
  // wipes it out.
  if (best.edge_per_contract < cfg.risk.ladder_min_edge) return std::nullopt;
  best.total_edge = best.edge_per_contract * best.count;
  best.total_cost = best.cost_per_contract * best.count;
  char buf[320];
  if (best.kind == "sell")
    std::snprintf(buf, sizeof buf,
                  "ladder arb: YES bids sum to %.3f across %zu brackets of %s (at most one can settle YES), "
                  "so selling all of them locks $%.3f per contract after taker fees",
                  best.sum_prices, best.legs.size(), event_ticker.c_str(), best.edge_per_contract);
  else
    std::snprintf(buf, sizeof buf,
                  "ladder arb: every bracket of %s offered, asks sum to %.3f < $1 and the brackets cover "
                  "every outcome, so buying the ladder locks $%.3f per contract after taker fees",
                  event_ticker.c_str(), best.sum_prices, best.edge_per_contract);
  best.note = buf;
  return best;
}

}  // namespace desk
