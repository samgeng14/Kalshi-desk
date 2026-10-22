#include "deskd/strat/arb.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include "deskd/app/log.h"
#include "deskd/gw/sys_book.h"
#include "deskd/strat/maker.h"
#include "deskd/web/contract.h"

namespace deskd::arb {

namespace {
constexpr Micros kContractDollar = kOneDollar * kOneContract;   // $1 a contract, micro-dollars

std::string usd(Micros m) {
  const Money v = m / 100;
  return (v < 0 ? "-$" : "$") + money_str(std::llabs(v));
}
std::string ctr(Qty q) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", qty_to_number(q));
  return buf;
}

// One market on one side: the levels a leg there may take, best first, and gw's inventory room
struct LegBook {
  int market = -1;
  const ArbMarket* a = nullptr;
  Side side = Side::BUY;
  std::vector<MdLevel> levels;
  Qty room = 0;
  double mult = 0;
};

// The legs at one size
struct Sized {
  std::vector<Leg> legs;
  Qty qty = 0;
  int shard = -1;
  Micros cost = 0, fees = 0, payout_min = 0, edge = 0, contingent_worst = 0, gross = 0;
};

// why a's market cannot be a leg now ("" when it can)
std::string unusable(const ArbMarket& a, std::int64_t now, const Limits& lim) {
  if (!a.row) return "a market has no listing row";
  const auto& r = *a.row;
  const std::string& tk = r.ticker;
  if (r.status != "active" && r.status != "open") return tk + " is " + (r.status.empty() ? "not active" : r.status);
  if (r.open_ms && *r.open_ms > now) return tk + " has not opened";
  if (!r.close_ms || *r.close_ms - now <= lim.min_close_lead_ms)
    return tk + " closes within " + std::to_string(lim.min_close_lead_ms / 1000) + " s";
  const std::string today = et_date(now), d = rest::outcome_date(r);
  if (d != today && d != et_date_after(today))
    return tk + " settles " + (d.empty() ? "on no known date" : d) + ", not today or tomorrow";
  if (!rest::fee_ready(r, now)) return tk + "'s fee metadata is missing or stale";
  if (!r.exchange_index) return tk + " has no exchange index";
  if (r.price_ranges.empty()) return tk + " has no price grid";
  if (!a.book) return tk + " has no book yet";
  if (a.book->stale) return tk + "'s book is stale";
  const auto& b = *a.book;
  if (!b.bids.empty() && !b.asks.empty() && b.bids.front().px >= b.asks.front().px) return tk + "'s book is crossed";
  return "";
}

// What a leg on a's market may take on side: everyone else's levels from the touch through
// lim.max_ticks grid ticks, short of Systematic's own order on the other side
std::optional<LegBook> leg_book(const EventShape& s, const ArbMarket& a, Side side, const Limits& lim, std::string* why) {
  const auto& r = *a.row;
  const bool buy = side == Side::BUY;
  const auto& lv = buy ? a.book->asks : a.book->bids;
  if (lv.empty()) {
    *why = r.ticker + " has no " + (buy ? "ask" : "bid");
    return std::nullopt;
  }
  Px far = lv.front().px;
  for (int i = 0; i < lim.max_ticks; ++i) {
    const auto next = buy ? grid_ceil(far + 1, r.price_ranges) : grid_floor(far - 1, r.price_ranges);
    if (!next) break;
    far = *next;
  }
  LegBook b;
  b.market = s.index(r.ticker);
  b.a = &a;
  b.side = side;
  b.mult = r.fee_multiplier.value_or(0);
  for (const auto& l : lv) {
    if (buy ? l.px > far || l.px >= a.own_ask : l.px < far || l.px <= a.own_bid) break;
    if (l.qty > 0) b.levels.push_back(l);
  }
  if (b.levels.empty()) {
    *why = r.ticker + ": Systematic's own " + (buy ? "ask at " + price_str(a.own_ask) : "bid at " + price_str(a.own_bid)) +
           " is at or through the touch";
    return std::nullopt;
  }
  // gw's inventory rule (place_sys): the position, every lane's, after this side's working orders
  // and the leg, at most inventory from flat, or no further from it than it already is
  const Qty x = buy ? a.sys_position : -a.sys_position, w = buy ? a.sys_buying : a.sys_selling;
  b.room = std::max<Qty>(0, std::max(lim.inventory, std::abs(x)) - x - w);
  return b;
}

// The legs at q each; nullopt when a leg's levels show less
std::optional<Sized> at(const EventShape& s, const std::vector<LegBook>& books, Qty q, const Limits& lim) {
  Sized z;
  z.qty = q;
  std::vector<Stake> working;
  bool first = true;
  for (const auto& b : books) {
    const bool buy = b.side == Side::BUY;
    Leg l;
    l.ticker = b.a->row->ticker;
    l.market = b.market;
    l.side = b.side;
    l.planned = b.levels.front().px;
    std::vector<MdLevel> fills;
    Qty left = q;
    for (const auto& v : b.levels) {
      if (left <= 0) break;
      const Qty t = std::min(left, v.qty);
      l.cost += (buy ? v.px : kOneDollar - v.px) * t;
      fills.push_back({v.px, t});
      l.limit = v.px;
      left -= t;
    }
    if (left > 0) return std::nullopt;
    for (const auto& v : b.levels) {
      if (buy ? v.px > l.limit : v.px < l.limit) break;
      l.shown += v.qty;
    }
    l.fee = order_fee_bound(b.mult, fills, lim.fee_precision);
    // what gw reserves for it: its limit's collateral
    const Micros reserve = (buy ? l.limit : kOneDollar - l.limit) * q;
    working.push_back(order_stake(l.ticker, b.side, q, reserve));
    const int shard = b.a->row->exchange_index.value_or(-1);
    z.shard = first || z.shard == shard ? shard : -1;
    first = false;
    z.cost += l.cost;
    z.fees += l.fee;
    z.gross += reserve;
    z.legs.push_back(std::move(l));
  }
  z.edge = plan_edge(s, z.legs, q);
  z.payout_min = z.edge + z.cost + z.fees;
  z.contingent_worst = std::max<Micros>(0, worst_loss(s, working));
  return z;
}

// The most whole contracts the legs take within lim (arb.h's Size), or nullopt with *why. What
// gw counts and the cash only grow with the size, so the sizes they allow are 1 up to some n. The
// lock less thr a contract is concave in the size: the payout grows by the same each contract,
// each further contract costs at least what the one before did, and the fee bound's unit an order
// is fixed. The sizes that clear thr are then one run, and two binary searches find its peak and
// its far end. Every size returned has passed every check: the bound's unit for a fill whose cost
// is not whole units (sub-cent prices) bends both rules a little, and can only make the search
// settle on fewer contracts, or none.
std::optional<Sized> size(const EventShape& s, const std::vector<LegBook>& books, const Limits& lim, std::string* why) {
  Qty cap = lim.max_qty;
  std::string short_of = "max_qty " + ctr(lim.max_qty);
  for (const auto& b : books) {
    Qty shown = 0;
    for (const auto& v : b.levels) shown += v.qty;
    if (shown < cap) {
      cap = shown;
      short_of = b.a->row->ticker + " shows " + ctr(shown) + " within reach";
    }
    if (b.room < cap) {
      cap = b.room;
      short_of = b.a->row->ticker + "'s inventory room is " + ctr(b.room);
    }
  }
  const std::int64_t most = cap / kOneContract;
  if (most < 1) {
    *why = short_of + ": not one contract";
    return std::nullopt;
  }
  std::map<std::int64_t, std::optional<Sized>> memo;
  const auto get = [&](std::int64_t n) -> const std::optional<Sized>& {
    auto it = memo.find(n);
    if (it == memo.end()) it = memo.emplace(n, at(s, books, n * kOneContract, lim)).first;
    return it->second;
  };
  const auto counted = [&](const Sized& z) { return lim.by_state ? z.contingent_worst : z.gross; };
  const auto fits = [&](std::int64_t n) {
    const auto& z = get(n);
    return z && counted(*z) <= lim.budget_left && z->gross + z->fees <= lim.cash_left;
  };
  // the lock past thr a contract
  const auto over = [&](std::int64_t n) {
    const auto& z = get(n);
    return z ? z->edge - lim.thr * n * kOneContract : std::numeric_limits<Micros>::min();
  };
  if (!fits(1)) {
    const auto& z = get(1);
    if (!z) *why = "no contract to take";
    else if (counted(*z) > lim.budget_left)
      *why = "a contract counts " + usd(counted(*z)) + ", over the " + usd(lim.budget_left) + " the budget leaves";
    else
      *why = "a contract needs " + usd(z->gross + z->fees) + " of cash, over the " + usd(lim.cash_left) + " free";
    return std::nullopt;
  }
  std::int64_t lo = 1, hi = most;
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo + 1) / 2;
    if (fits(mid)) lo = mid;
    else hi = mid - 1;
  }
  std::int64_t n = lo;
  if (over(n) < 0) {
    std::int64_t a = 1, b = n;   // the peak
    while (a < b) {
      const std::int64_t mid = a + (b - a) / 2;
      if (over(mid + 1) > over(mid)) a = mid + 1;
      else b = mid;
    }
    if (over(a) < 0) {
      *why = "locks " + usd(get(a)->edge) + " on " + std::to_string(a) + " at best, under the " +
             usd(lim.thr * kOneContract) + " a contract it needs";
      return std::nullopt;
    }
    for (b = n; a < b;) {   // the far end
      const std::int64_t mid = a + (b - a + 1) / 2;
      if (over(mid) >= 0) a = mid;
      else b = mid - 1;
    }
    n = a;
  }
  if (!fits(n) || over(n) < 0) {
    *why = "the rounding units leave no size that both fits and locks";
    return std::nullopt;
  }
  return get(n);
}

Plan plan_of(const EventShape& s, Kind k, Sized z) {
  Plan p;
  p.event = s.event;
  p.kind = k;
  p.legs = std::move(z.legs);
  std::stable_sort(p.legs.begin(), p.legs.end(), [](const Leg& a, const Leg& b) { return a.shown < b.shown; });
  p.qty = z.qty;
  p.shard = z.shard;
  p.cost = z.cost;
  p.fees = z.fees;
  p.payout_min = z.payout_min;
  p.edge = z.edge;
  p.contingent_worst = z.contingent_worst;
  p.gross = z.gross;
  return p;
}

using Markets = std::map<int, const ArbMarket*>;   // by index in the shape

// the market's leg on side, or nullopt with *why
std::optional<LegBook> leg_of(const EventShape& s, const ArbMarket& a, Side side, std::int64_t now, const Limits& lim,
                              std::string* why) {
  if (auto u = unusable(a, now, lim); !u.empty()) {
    *why = u;
    return std::nullopt;
  }
  return leg_book(s, a, side, lim, why);
}

std::optional<Plan> yes_cover(const EventShape& s, const Markets& by, std::int64_t now, const Limits& lim, std::string* why) {
  const int n = static_cast<int>(s.tickers.size());
  if (!s.exhaustive) {
    *why = "not proven exhaustive";
    return std::nullopt;
  }
  if (n < 2 || n > lim.max_legs) {
    *why = std::to_string(n) + " markets, not 2 to " + std::to_string(lim.max_legs);
    return std::nullopt;
  }
  std::vector<LegBook> books;
  for (int i = 0; i < n; ++i) {
    const auto it = by.find(i);
    if (it == by.end()) {
      *why = s.tickers[i] + " is not among the markets scanned";
      return std::nullopt;
    }
    auto b = leg_of(s, *it->second, Side::BUY, now, lim, why);
    if (!b) return std::nullopt;
    books.push_back(std::move(*b));
  }
  auto z = size(s, books, lim, why);
  if (!z) return std::nullopt;
  return plan_of(s, Kind::YES_COVER, std::move(*z));
}

std::optional<Plan> no_set(const EventShape& s, const Markets& by, std::int64_t now, const Limits& lim, std::string* why) {
  if (!s.exclusive) {
    *why = "the venue does not flag it mutually exclusive";
    return std::nullopt;
  }
  std::vector<LegBook> bids;
  std::string first;
  for (const auto& [i, a] : by) {
    std::string w;
    if (auto b = leg_of(s, *a, Side::SELL, now, lim, &w)) bids.push_back(std::move(*b));
    else if (first.empty()) first = w;
  }
  std::stable_sort(bids.begin(), bids.end(), [](const LegBook& a, const LegBook& b) { return a.levels.front().px > b.levels.front().px; });
  // The old scanner's pick, a contract at each touch: the fattest bids first. Each NO leg ties up
  // about 1 - its bid, so once the bids pass $1 a leg is added only while it raises the lock per
  // dollar tied up; before that nothing is locked yet, and every leg that pays its fee goes in.
  std::vector<LegBook> legs;
  Micros sum = 0, fees = 0, cost = 0;
  for (auto& b : bids) {
    if (static_cast<int>(legs.size()) >= lim.max_legs) break;
    const Px bid = b.levels.front().px;
    const Micros got = bid * kOneContract, fee = kalshi_model_fee(b.mult, bid, kOneContract);
    if (got <= fee) continue;   // this leg cannot pay for itself
    const Micros now_edge = sum - kContractDollar - fees, next_edge = now_edge + got - fee;
    const Micros next_cost = cost + (kContractDollar - got) + fee;
    if (now_edge > 0 && static_cast<__int128>(next_edge) * cost <= static_cast<__int128>(now_edge) * next_cost) break;
    sum += got;
    fees += fee;
    cost = next_cost;
    legs.push_back(std::move(b));
  }
  if (legs.size() < 2) {
    *why = bids.size() < 2 && !first.empty() ? first : "fewer than two bids that pay their fee";
    return std::nullopt;
  }
  if (sum - kContractDollar - fees <= 0) {
    *why = "the best " + std::to_string(legs.size()) + " bids sum to " + usd(sum) + " a contract, " + usd(fees) +
           " in fees: nothing locked";
    return std::nullopt;
  }
  auto z = size(s, legs, lim, why);
  if (!z) return std::nullopt;
  return plan_of(s, Kind::NO_SET, std::move(*z));
}

std::optional<Plan> dominance(const EventShape& s, const Markets& by, std::int64_t now, const Limits& lim, std::string* why) {
  if (!s.spans) {
    *why = "a market has no strikes";
    return std::nullopt;
  }
  // i implies j: every state that pays i pays j, and some state pays i
  const auto implies = [&s](int i, int j) {
    bool any = false;
    for (const auto& st : s.states) {
      const bool pi = std::find(st.begin(), st.end(), i) != st.end();
      if (pi && std::find(st.begin(), st.end(), j) == st.end()) return false;
      any = any || pi;
    }
    return any;
  };
  std::map<int, LegBook> sells, buys;
  std::string first;
  for (const auto& [i, a] : by) {
    std::string w;
    if (auto b = leg_of(s, *a, Side::SELL, now, lim, &w)) sells.emplace(i, std::move(*b));
    else if (first.empty()) first = w;
    if (auto b = leg_of(s, *a, Side::BUY, now, lim, &w)) buys.emplace(i, std::move(*b));
    else if (first.empty()) first = w;
  }
  // each pair's best: the largest edge, then the least cost; none: the first pair's reason
  std::optional<Sized> best;
  std::string pair_why;
  for (const auto& [i, hard] : sells)
    for (const auto& [j, easy] : buys) {
      if (i == j || !implies(i, j)) continue;
      const std::string pair = s.tickers[i] + " implies " + s.tickers[j];
      // a contract at the touches: the harder's bid over the easier's ask, less their fees
      const Px bid = hard.levels.front().px, ask = easy.levels.front().px;
      const Micros fee = kalshi_model_fee(hard.mult, bid, kOneContract) + kalshi_model_fee(easy.mult, ask, kOneContract);
      std::string w;
      std::optional<Sized> z;
      if ((bid - ask) * kOneContract <= fee) w = "bid " + price_str(bid) + " against ask " + price_str(ask) + ": nothing locked";
      else z = size(s, {hard, easy}, lim, &w);
      if (!z) {
        if (pair_why.empty()) pair_why = pair + ", " + w;
        continue;
      }
      if (!best || z->edge > best->edge || (z->edge == best->edge && z->cost < best->cost)) best = std::move(z);
    }
  if (best) return plan_of(s, Kind::DOMINANCE, std::move(*best));
  *why = !pair_why.empty() ? pair_why : sells.empty() && buys.empty() && !first.empty() ? first
                                                                                          : "no market implies another at a touch";
  return std::nullopt;
}
}  // namespace

const char* kind_str(Kind k) {
  switch (k) {
    case Kind::YES_COVER: return "YES_COVER";
    case Kind::NO_SET: return "NO_SET";
    case Kind::DOMINANCE: return "DOMINANCE";
  }
  return "?";
}

ArbMarket arb_market(const kalshi::MarketRow& row, std::optional<MdBook> book, const Gateway::SysView& v,
                     const rest::Snapshot& s) {
  ArbMarket a;
  a.row = &row;
  const std::string& tk = row.ticker;
  // every Systematic order not ended, sent or not: any of them may rest by the time a leg arrives
  for (const auto& [cid, o] : v.orders) {
    if (o.ticker != tk || o.terminal || o.left <= 0) continue;
    if (o.side == Side::BUY) {
      a.own_bid = std::max(a.own_bid, o.price);
      a.sys_buying += o.left;
    } else {
      a.own_ask = std::min(a.own_ask, o.price);
      a.sys_selling += o.left;
    }
  }
  for (const auto& [key, pos] : v.positions) {
    const auto bar = key.find('|');
    if (bar != std::string::npos && key.compare(bar + 1, std::string::npos, tk) == 0) a.sys_position += pos;
  }
  if (book) book_without_own(*book, v, tk);
  a.book = std::move(book);
  if (v.held.count(tk)) {
    a.manual = "Manual took " + tk + " over";
  } else if (!s.account_ready) {
    a.manual = "no account read yet: Manual's orders and positions in " + tk + " are not known";
  } else {
    for (const auto& o : s.orders)
      if (o.ticker == tk && o.status == kalshi::OrderStatus::RESTING && o.remaining > 0 &&
          (o.client_order_id.empty() || o.client_order_id[0] != 'S')) {
        a.manual = "an order not Systematic's rests in " + tk + " (" +
                   (o.client_order_id.empty() ? o.order_id : o.client_order_id) + ")";
        break;
      }
    Qty held = 0;
    for (const auto& p : s.positions)
      if (p.ticker == tk) held += p.position;
    if (a.manual.empty() && held != a.sys_position)
      a.manual = "the account holds " + ctr(held) + " in " + tk + ", Systematic's lanes " + ctr(a.sys_position);
  }
  return a;
}

std::vector<Plan> find_baskets(const EventShape& s, const std::vector<ArbMarket>& m, std::int64_t now_ms,
                               const Limits& lim, std::string* why) {
  std::vector<Plan> out;
  Markets by;
  for (const auto& a : m) {
    const int i = a.row ? s.index(a.row->ticker) : -1;
    if (i < 0) continue;
    if (!a.manual.empty()) {
      if (why) *why = a.manual;
      return out;
    }
    by.emplace(i, &a);
  }
  std::string w[3];
  if (auto p = yes_cover(s, by, now_ms, lim, &w[0])) out.push_back(std::move(*p));
  if (auto p = no_set(s, by, now_ms, lim, &w[1])) out.push_back(std::move(*p));
  if (auto p = dominance(s, by, now_ms, lim, &w[2])) out.push_back(std::move(*p));
  std::stable_sort(out.begin(), out.end(), [](const Plan& a, const Plan& b) { return a.edge > b.edge; });
  if (out.empty() && why) *why = "YES_COVER: " + w[0] + "; NO_SET: " + w[1] + "; DOMINANCE: " + w[2];
  return out;
}

Micros plan_edge(const EventShape& s, const std::vector<Leg>& legs, Qty qty) {
  std::vector<Stake> st;
  for (const auto& l : legs) st.push_back(Stake{l.ticker, l.side == Side::BUY, qty, l.cost + l.fee, false});
  return -worst_loss(s, st);
}

const char* st_str(St s) {
  switch (s) {
    case St::FIRST: return "FIRST";
    case St::REST: return "REST";
    case St::COMPLETE: return "COMPLETE";
    case St::UNWIND: return "UNWIND";
    case St::UNCERTAIN: return "UNCERTAIN";
    case St::FROZEN: return "FROZEN";
    case St::DONE: return "DONE";
  }
  return "?";
}

namespace {
// One leg's money, YES terms: what its own orders took (in: the leg and its completion; an
// orphan's, what S1 held) and what its unwinds closed (out); and its completion's slippage
struct Flow {
  Qty in_q = 0, out_q = 0;
  Micros in_c = 0, out_c = 0, in_f = 0, out_f = 0, slip = 0;
};
std::vector<Flow> flows(const Basket& b, const Gateway::SysView& v) {
  std::vector<Flow> f(b.plan.legs.size());
  if (b.orphan)
    for (std::size_t k = 0; k < f.size(); ++k) {
      f[k].in_q = b.plan.legs[k].shown;
      f[k].in_c = b.plan.legs[k].cost;
    }
  for (const auto& o : b.orders) {
    if (o.leg < 0 || o.leg >= static_cast<int>(f.size())) continue;
    Qty q = o.executed;
    Micros c = o.exec_cost, fee = o.fees;
    if (const auto it = v.orders.find(o.cid); !o.prior && it != v.orders.end()) {
      q = it->second.executed;
      c = it->second.exec_cost;
      fee = it->second.fees;
    }
    auto& x = f[o.leg];
    if (o.role == 'u') {
      x.out_q += q;
      x.out_c += c;
      x.out_f += fee;
      continue;
    }
    x.in_q += q;
    x.in_c += c;
    x.in_f += fee;
    // a completion's price past the planned touch: a BUY's above it, a SELL's below
    if (o.role == 'c') x.slip -= (o.side == Side::BUY ? 1 : -1) * (c - b.plan.legs[o.leg].planned * q);
  }
  return f;
}
// t's share for q of n contracts (n > 0), rounded toward zero
Micros share(Micros t, Qty q, Qty n) { return n > 0 ? static_cast<Micros>(static_cast<__int128>(t) * q / n) : 0; }
// the unwinds' round trips: each closed contract at the closing order's average less the leg's
// own, and both orders' fees on it
Micros round_trips(const Basket& b, const std::vector<Flow>& f) {
  Micros r = 0;
  for (std::size_t k = 0; k < f.size(); ++k) {
    const auto& x = f[k];
    const Qty u = std::min(x.out_q, x.in_q);
    if (u <= 0) continue;
    const int d = b.plan.legs[k].side == Side::BUY ? 1 : -1;
    r += d * (share(x.out_c, u, x.out_q) - share(x.in_c, u, x.in_q)) - share(x.out_f, u, x.out_q) - share(x.in_f, u, x.in_q);
  }
  return r;
}
}  // namespace

Micros legging_pnl(const Basket& b, const Gateway::SysView& v) {
  const auto f = flows(b, v);
  Micros slip = 0;
  for (const auto& x : f) slip += x.slip;
  return slip + round_trips(b, f);
}

Micros basket_edge(const Basket& b, const Gateway::SysView& v) {
  const auto f = flows(b, v);
  const Micros round = round_trips(b, f);
  if (b.orphan || f.empty() || b.plan.qty <= 0) return round;
  Qty m = std::numeric_limits<Qty>::max();
  for (const auto& x : f) m = std::min(m, x.in_q - x.out_q);
  if (m <= 0) return round;
  // its least payout over the event's states, a contract, times what every leg holds, less what
  // that part cost each leg (a SELL's: $1 less the YES price) and its fees
  Micros lock = b.plan.payout_min / b.plan.qty * m;
  for (std::size_t k = 0; k < f.size(); ++k) {
    const Micros yes = share(f[k].in_c, m, f[k].in_q);
    lock -= (b.plan.legs[k].side == Side::BUY ? yes : kOneDollar * m - yes) + share(f[k].in_f, m, f[k].in_q);
  }
  return lock + round;
}

}  // namespace deskd::arb

namespace deskd {
using json = nlohmann::json;
using arb::Basket;
using arb::BasketOrder;
using arb::Plan;
using arb::St;

namespace {

std::string base36(std::int64_t v) {
  std::string t;
  for (auto u = static_cast<std::uint64_t>(v); u; u /= 36) t.insert(t.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[u % 36]);
  return t.empty() ? "0" : t;
}
bool of_series(const std::string& tk, const std::vector<std::string>& series) {
  return std::any_of(series.begin(), series.end(), [&tk](const std::string& s) {
    return tk.size() > s.size() && tk.compare(0, s.size(), s) == 0 && tk[s.size()] == '-';
  });
}
// a market's event: its row's, else its ticker without the last segment (gw's event_key)
std::string event_of(const std::string& tk, const rest::Snapshot& s) {
  const auto m = s.markets.find(tk);
  if (m != s.markets.end() && !m->second.event_ticker.empty()) return m->second.event_ticker;
  const auto dash = tk.rfind('-');
  return dash == std::string::npos ? tk : tk.substr(0, dash);
}
bool trading(const kalshi::MarketRow& m) { return m.status == "active" || m.status == "open"; }
const char* side_str(Side s) { return s == Side::BUY ? "BUY" : "SELL"; }
bool in_flight(const Basket& b) { return b.st != St::FROZEN && b.st != St::DONE; }
// a leg's holding moves with S1's position in its direction: up for a BUY, down for a SELL
int dir(Side s) { return s == Side::BUY ? 1 : -1; }
Qty position(const Gateway::SysView& v, const std::string& lane, const std::string& tk) {
  const auto it = v.positions.find(lane + "|" + tk);
  return it == v.positions.end() ? 0 : it->second;
}
// what gw reserves for q of an order at px: its collateral
Micros collateral(Side side, Px px, Qty q) { return (side == Side::BUY ? px : kOneDollar - px) * q; }

const char* is_str(BasketOrder::Is i) {
  switch (i) {
    case BasketOrder::Is::SENT: return "SENT";
    case BasketOrder::Is::OPEN: return "OPEN";
    case BasketOrder::Is::UNKNOWN: return "UNKNOWN";
    case BasketOrder::Is::ENDED: return "ENDED";
    case BasketOrder::Is::REFUSED: return "REFUSED";
  }
  return "?";
}

// ---- s1.json: integers as deskd holds them (Px, Qty, micro-dollars)
json leg_json(const arb::Leg& l) {
  return {{"tk", l.ticker}, {"market", l.market}, {"side", side_str(l.side)}, {"planned", l.planned}, {"limit", l.limit},
          {"shown", l.shown}, {"cost", l.cost}, {"fee", l.fee}};
}
json order_json(const BasketOrder& o) {
  return {{"cid", o.cid}, {"leg", o.leg}, {"role", std::string(1, o.role)}, {"side", side_str(o.side)}, {"px", o.price},
          {"count", o.count}, {"is", is_str(o.is)}, {"filled", o.filled}, {"executed", o.executed},
          {"exec_cost", o.exec_cost}, {"fees", o.fees}, {"sent_ms", o.sent_ms}, {"done_ms", o.done_ms}, {"why", o.why}};
}
json basket_json(const Basket& b) {
  json legs = json::array(), orders = json::array();
  for (const auto& l : b.plan.legs) legs.push_back(leg_json(l));
  for (const auto& o : b.orders) orders.push_back(order_json(o));
  const auto& p = b.plan;
  return {{"id", b.id}, {"event", b.event}, {"kind", arb::kind_str(p.kind)}, {"orphan", b.orphan}, {"st", arb::st_str(b.st)},
          {"stuck", b.stuck}, {"base", b.base}, {"target", b.target}, {"t0", b.t0}, {"deadline", b.deadline},
          {"tried_ms", b.tried_ms}, {"tries", b.tries}, {"why", b.why},
          {"plan", {{"qty", p.qty}, {"shard", p.shard}, {"cost", p.cost}, {"fees", p.fees}, {"payout_min", p.payout_min},
                    {"edge", p.edge}, {"contingent_worst", p.contingent_worst}, {"gross", p.gross}, {"legs", legs}}},
          {"orders", orders}};
}
Side side_of(const std::string& s) {
  if (s == "BUY") return Side::BUY;
  if (s == "SELL") return Side::SELL;
  throw std::runtime_error("side " + s);
}
Basket basket_from(const json& j) {
  Basket b;
  b.id = j.at("id").get<std::string>();
  b.event = j.at("event").get<std::string>();
  const std::string kind = j.at("kind").get<std::string>(), st = j.at("st").get<std::string>();
  bool found = false;
  for (auto k : {arb::Kind::YES_COVER, arb::Kind::NO_SET, arb::Kind::DOMINANCE})
    if (kind == arb::kind_str(k)) b.plan.kind = k, found = true;
  if (!found) throw std::runtime_error("kind " + kind);
  found = false;
  for (auto s : {St::FIRST, St::REST, St::COMPLETE, St::UNWIND, St::UNCERTAIN, St::FROZEN, St::DONE})
    if (st == arb::st_str(s)) b.st = s, found = true;
  if (!found) throw std::runtime_error("state " + st);
  b.orphan = j.at("orphan").get<bool>();
  b.stuck = j.at("stuck").get<bool>();
  b.base = j.at("base").get<std::vector<Qty>>();
  b.target = j.at("target").get<Qty>();
  b.t0 = j.at("t0").get<std::int64_t>();
  b.deadline = j.at("deadline").get<std::int64_t>();
  b.tried_ms = j.at("tried_ms").get<std::int64_t>();
  b.tries = j.at("tries").get<int>();
  b.why = j.at("why").get<std::string>();
  const auto& p = j.at("plan");
  b.plan.event = b.event;
  b.plan.id = b.id;
  b.plan.qty = p.at("qty").get<Qty>();
  b.plan.shard = p.at("shard").get<int>();
  b.plan.cost = p.at("cost").get<Micros>();
  b.plan.fees = p.at("fees").get<Micros>();
  b.plan.payout_min = p.at("payout_min").get<Micros>();
  b.plan.edge = p.at("edge").get<Micros>();
  b.plan.contingent_worst = p.at("contingent_worst").get<Micros>();
  b.plan.gross = p.at("gross").get<Micros>();
  for (const auto& l : p.at("legs")) {
    arb::Leg x;
    x.ticker = l.at("tk").get<std::string>();
    x.market = l.at("market").get<int>();
    x.side = side_of(l.at("side").get<std::string>());
    x.planned = l.at("planned").get<Px>();
    x.limit = l.at("limit").get<Px>();
    x.shown = l.at("shown").get<Qty>();
    x.cost = l.at("cost").get<Micros>();
    x.fee = l.at("fee").get<Micros>();
    b.plan.legs.push_back(std::move(x));
  }
  if (b.plan.legs.empty() || b.base.size() != b.plan.legs.size()) throw std::runtime_error(b.id + ": its legs and bases differ");
  for (const auto& o : j.at("orders")) {
    BasketOrder x;
    x.cid = o.at("cid").get<std::string>();
    x.leg = o.at("leg").get<int>();
    const std::string role = o.at("role").get<std::string>(), is = o.at("is").get<std::string>();
    if (role.size() != 1 || std::string("lcu").find(role[0]) == std::string::npos) throw std::runtime_error("role " + role);
    x.role = role[0];
    x.side = side_of(o.at("side").get<std::string>());
    x.price = o.at("px").get<Px>();
    x.count = o.at("count").get<Qty>();
    found = false;
    for (auto i : {BasketOrder::Is::SENT, BasketOrder::Is::OPEN, BasketOrder::Is::UNKNOWN, BasketOrder::Is::ENDED, BasketOrder::Is::REFUSED})
      if (is == is_str(i)) x.is = i, found = true;
    if (!found || x.leg < 0 || x.leg >= static_cast<int>(b.plan.legs.size())) throw std::runtime_error(x.cid + ": is " + is);
    x.filled = o.at("filled").get<Qty>();
    x.executed = o.at("executed").get<Qty>();
    x.exec_cost = o.at("exec_cost").get<Micros>();
    x.fees = o.at("fees").get<Micros>();
    x.sent_ms = o.at("sent_ms").get<std::int64_t>();
    x.done_ms = o.at("done_ms").get<std::int64_t>();
    x.why = o.at("why").get<std::string>();
    b.orders.push_back(std::move(x));
  }
  return b;
}
}  // namespace

ArbEngine::ArbEngine(std::string lane, std::vector<std::string> series, Env env, arb::Params p)
    : lane_(std::move(lane)), series_(std::move(series)), env_(std::move(env)), p_(std::move(p)) {
  load();
}

// s1.json, read once before the first step. An open basket comes back FROZEN with its orders a
// previous run's: whatever they filled reaches S1's positions through gw's rebuild, and it is
// only unwound. A file that cannot be read keeps nothing but its error, which pauses new baskets.
void ArbEngine::load() {
  if (env_.state_file.empty()) return;
  std::ifstream in(env_.state_file);
  if (!in) return;   // none yet
  const auto j = json::parse(in, nullptr, false);
  try {
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1) throw std::runtime_error("not a version 1 file");
    seq_ = j.at("seq").get<std::uint64_t>();
    legging_ = j.at("legging").get<Micros>();
    state_error_ = j.at("error").get<std::string>();
    for (const auto& b : j.at("baskets")) baskets_.push_back(basket_from(b));
    state_found_ = true;
  } catch (const std::exception& e) {
    baskets_.clear();
    legging_ = 0;
    state_error_ = env_.state_file + " cannot be read (" + e.what() + ")";
    DLOG_WARN("%s: S1 pauses new baskets until the lane is turned off and on", state_error_.c_str());
  }
  for (auto& b : baskets_) {
    b.restored = true;
    for (auto& o : b.orders) {
      o.prior = true;
      if (o.is == BasketOrder::Is::SENT || o.is == BasketOrder::Is::OPEN || o.is == BasketOrder::Is::UNKNOWN) {
        o.is = BasketOrder::Is::ENDED;
        o.why = "a previous run's, its end unseen: what it filled is in S1's positions";
      }
    }
    if (b.st != St::DONE && !(b.st == St::FROZEN && b.stuck)) {
      b.st = St::FROZEN;
      b.why = "open at the start: unwound once it may send";
    }
  }
}

void ArbEngine::save() {
  dirty_ = false;
  if (env_.state_file.empty()) return;
  json bs = json::array();
  for (const auto& b : baskets_)
    if (b.st != St::DONE) bs.push_back(basket_json(b));
  const std::string tmp = env_.state_file + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json{{"v", 1}, {"seq", seq_}, {"legging", legging_}, {"error", state_error_}, {"baskets", bs}}.dump();
    if (!out) { DLOG_WARN("cannot write %s", tmp.c_str()); return; }
  }
  if (std::rename(tmp.c_str(), env_.state_file.c_str()) != 0) DLOG_WARN("cannot replace %s", env_.state_file.c_str());
}

int ArbEngine::step(std::int64_t now) {
  if (p_.mode != "scan" && p_.mode != "trade") return 250;
  const auto view = env_.view ? env_.view() : nullptr;
  const auto snap = env_.snapshot ? env_.snapshot() : nullptr;
  if (!boot_ms_) {
    boot_ms_ = now;
    if (!state_error_.empty() && env_.alert) env_.alert("CRIT", lane_ + ": " + state_error_ + "; no new basket until the lane is turned off and on");
    else if (legging_ <= -p_.legging_pause && env_.alert)
      env_.alert("CRIT", lane_ + " paused: legging " + arb::usd(legging_) + " (s1.json); no new basket until the lane is turned off and on");
    for (auto& b : baskets_) {
      if (env_.alert)
        env_.alert("WARN", lane_ + " basket " + b.id + " in " + b.event + " was open at the start (" + arb::st_str(b.st) +
                               "): " + (b.stuck ? "it stays frozen" : "it is unwound once Systematic runs"));
      record("baskets", basket_row(b, nullptr));
    }
  }
  if (!view || !snap) return 250;
  view_ = view.get();
  lane_seen(*view);
  orphans(*view, *snap, now);
  for (auto& b : baskets_) drive(b, *view, *snap, now);
  baskets_.erase(std::remove_if(baskets_.begin(), baskets_.end(), [](const Basket& b) { return b.st == St::DONE; }), baskets_.end());
  if (now >= next_scan_ms_) {
    next_scan_ms_ = now + p_.scan_every_ms;
    scan(*view, *snap, now);
  }
  flush();
  if (dirty_) save();
  publish(*view);
  view_ = nullptr;
  return std::any_of(baskets_.begin(), baskets_.end(), in_flight) ? 10 : 250;
}

// The pause's reset: the lane turned on, off and on again while paused, seen by this engine
void ArbEngine::lane_seen(const Gateway::SysView& v) {
  const bool on = v.lanes_on.count(lane_) > 0;
  if (state_error_.empty() && legging_ > -p_.legging_pause) {
    cycle_ = 0;
    return;
  }
  if (on && cycle_ == 2) {
    const std::string was = !state_error_.empty() ? state_error_ : "legging " + arb::usd(legging_);
    DLOG_WARN("%s: the pause (%s) reset: the lane was turned off and on", lane_.c_str(), was.c_str());
    if (env_.alert) env_.alert("WARN", lane_ + "'s pause (" + was + ") reset by the lane turned off and on: new baskets may fire");
    legging_ = 0;
    state_error_.clear();
    pause_alerted_ = false;
    cycle_ = 0;
    dirty_ = true;
  } else if (on) {
    cycle_ = std::max(cycle_, 1);
  } else if (cycle_ == 1) {
    cycle_ = 2;
  }
}

bool ArbEngine::paused(const Gateway::SysView& v, std::string* why) const {
  if (!state_error_.empty()) {
    *why = "paused: " + state_error_ + "; turn " + lane_ + " off and on to go on";
    return true;
  }
  if (legging_ <= -p_.legging_pause) {
    *why = "paused: legging " + arb::usd(legging_) + ", past -" + arb::usd(p_.legging_pause) + "; turn " + lane_ +
           " off and on to reset";
    return true;
  }
  const auto d = v.lane_day.find(lane_);
  if (d != v.lane_day.end() && d->second <= -p_.legging_pause) {
    *why = "held: " + lane_ + "'s day is " + arb::usd(d->second) + ", past -" + arb::usd(p_.legging_pause);
    return true;
  }
  return false;
}

// After a start S1's positions count once gw has rebuilt Systematic's day from the venue's fills:
// the first account read that began after this engine's first step, fills_wait_ms old. Reads may
// come every poll or faster (each private event asks for one), so it is that first read's age, not
// the latest's. From then on gw keeps them, so it holds for the rest of the run.
bool ArbEngine::restart_ready(const rest::Snapshot& s, std::int64_t now) {
  if (!first_read_ms_ && s.account_ready && s.account_began_ms >= boot_ms_) first_read_ms_ = s.account_ms;
  ready_ = ready_ || (first_read_ms_ && now - first_read_ms_ >= p_.fills_wait_ms);
  return ready_;
}

// With no s1.json (none, or unreadable), S1's positions in each event no open basket explains
// become an orphan basket, its legs what S1 holds there: it is unwound to equal holdings
void ArbEngine::orphans(const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now) {
  if (orphans_done_ || state_found_ || !restart_ready(s, now)) return;
  orphans_done_ = true;
  std::set<std::string> known;
  for (const auto& b : baskets_)
    for (const auto& l : b.plan.legs) known.insert(l.ticker);
  std::map<std::string, std::vector<std::string>> by_event;
  for (const auto& [key, pos] : v.positions) {
    const auto bar = key.find('|');
    if (bar == std::string::npos || key.compare(0, bar, lane_) != 0 || bar != lane_.size() || pos == 0) continue;
    const std::string tk = key.substr(bar + 1);
    if (!known.count(tk)) by_event[event_of(tk, s)].push_back(tk);
  }
  for (const auto& [ev, tks] : by_event) {
    Basket b;
    b.id = lane_ + "-" + base36(now) + "-b" + std::to_string(++seq_);
    b.event = ev;
    b.orphan = b.restored = true;
    b.t0 = now;
    std::string held;
    for (const auto& tk : tks) {
      const Qty pos = position(v, lane_, tk);
      const auto inv = v.inv.find(lane_ + "|" + tk);
      arb::Leg l;
      l.ticker = tk;
      l.side = pos > 0 ? Side::BUY : Side::SELL;
      l.shown = std::abs(pos);
      l.cost = inv == v.inv.end() ? 0 : inv->second.second;
      b.plan.legs.push_back(l);
      b.base.push_back(0);
      held += (held.empty() ? "" : ", ") + arb::ctr(pos) + " " + tk;
    }
    b.plan.event = ev;
    b.plan.id = b.id;
    b.st = St::FROZEN;
    b.why = "S1 holds " + held + " with no s1.json: unwound to equal holdings once it may send";
    DLOG_WARN("%s %s: %s", lane_.c_str(), ev.c_str(), b.why.c_str());
    if (env_.alert) env_.alert("WARN", lane_ + " " + ev + ": " + b.why);
    record("baskets", basket_row(b, &v));
    baskets_.push_back(std::move(b));
    dirty_ = true;
  }
}

const EventShape* ArbEngine::shape_of(const std::string& event, const rest::Snapshot& s, std::string* why) {
  auto it = shapes_.find(event);
  if (it == shapes_.end()) {
    std::string w;
    std::optional<EventShape> sh;
    const auto r = s.events.find(event);
    if (r == s.events.end()) {
      w = "no event row yet";
    } else {
      const std::string series = !r->second.series_ticker.empty() ? r->second.series_ticker : event.substr(0, event.find('-'));
      sh = event_shape(r->second, s.markets, env_.lattice ? env_.lattice(series) : 0, &w);
    }
    it = shapes_.emplace(event, std::make_pair(std::move(sh), w)).first;
  }
  if (why) *why = it->second.second;
  return it->second.first ? &*it->second.first : nullptr;
}

// What gw counts S1 using in the markets `in` accepts: SysView::worst market by market, or by
// state (deskd.event_risk): its positions, and its orders' reservations contingent, risk_used
Micros ArbEngine::s1_used(const Gateway::SysView& v, const rest::Snapshot& s, const std::function<bool(const std::string&)>& in) {
  Micros used = 0;
  if (!p_.limits.by_state) {
    for (const auto& [key, w] : v.worst) {
      const auto bar = key.find('|');
      if (bar == lane_.size() && key.compare(0, bar, lane_) == 0 && in(key.substr(bar + 1))) used += w;
    }
    return used;
  }
  std::vector<RiskStake> st;
  const auto add = [&](Stake x) {
    RiskStake r;
    static_cast<Stake&>(r) = std::move(x);
    r.event = event_of(r.ticker, s);
    st.push_back(std::move(r));
  };
  for (const auto& [key, inv] : v.inv) {
    const auto bar = key.find('|');
    if (bar != lane_.size() || key.compare(0, bar, lane_) != 0 || !in(key.substr(bar + 1))) continue;
    add(position_stake(key.substr(bar + 1), inv.first, inv.second));
  }
  // what an order may still add: what is left of it, and fills the venue reported that the book
  // has not taken yet (gw's rule for a by-state lane)
  for (const auto& [cid, o] : v.orders) {
    if (o.lane != lane_ || !in(o.ticker)) continue;
    const Qty q = (o.terminal ? 0 : o.left) + std::max<Qty>(0, o.filled - o.executed);
    if (q > 0) add(order_stake(o.ticker, o.side, q, collateral(o.side, o.price, q)));
  }
  return risk_used(st, [&](const std::string& e) { return shape_of(e, s, nullptr); });
}

// The finder's limits for an event: the least of what the lane's budget, the effective ceiling
// and the event's correlated group leave, as gw counts them; and the free cash of the event's
// shards, less Systematic's orders working there and admit_manual's fee allowance on every leg
// (the worst fee a contract, at the price nearest 50c, and a cent an order)
arb::Limits ArbEngine::limits(const EventShape& sh, const Gateway::SysView& v, const rest::Snapshot& s) {
  arb::Limits lim = p_.limits;
  const auto every = [](const std::string&) { return true; };
  const std::string grp = corr_group(sh.tickers.front(), sh.event);
  const auto in_grp = [&](const std::string& tk) { return corr_group(tk, event_of(tk, s)) == grp; };
  Micros grp_used = s1_used(v, s, in_grp);
  for (const auto& [key, w] : v.worst) {
    const auto bar = key.find('|');
    if (bar != std::string::npos && key.compare(0, bar, lane_) != 0 && in_grp(key.substr(bar + 1))) grp_used += w;
  }
  lim.budget_left = std::max<Micros>(0, std::min({p_.budget - s1_used(v, s, every), v.eff - v.used, corr_group_cap(grp) - grp_used}));
  std::set<int> shards;
  double mult = 0;
  for (const auto& tk : sh.tickers) {
    const auto m = s.markets.find(tk);
    if (m == s.markets.end()) continue;
    if (m->second.exchange_index) shards.insert(*m->second.exchange_index);
    mult = std::max(mult, m->second.fee_multiplier.value_or(0));
  }
  std::optional<Micros> cash;
  for (const int sh_id : shards) {
    const auto b = s.balances.find(sh_id);
    const Micros c = b == s.balances.end() ? 0 : b->second * 100;
    cash = cash ? std::min(*cash, c) : c;
  }
  Micros left = cash.value_or(0);
  for (const auto& [cid, o] : v.orders) {
    if (o.terminal || o.left <= 0) continue;
    const auto m = s.markets.find(o.ticker);
    if (m != s.markets.end() && m->second.exchange_index && shards.count(*m->second.exchange_index))
      left -= collateral(o.side, o.price, o.left);
  }
  left -= lim.max_legs * (10000 + kalshi_model_fee(mult, kOneDollar / 2, lim.max_qty));
  lim.cash_left = std::max<Micros>(0, left);
  return lim;
}

// why the event's best basket may not fire now ("" when it may)
std::string ArbEngine::blocked(const std::string& event, const Plan& p, const Gateway::SysView& v, const rest::Snapshot& s,
                               std::int64_t now) {
  if (p_.mode != "trade") return "scan mode";
  if (v.run != RunState::RUN) return std::string("Systematic is ") + run_code(v.run);
  if (!v.lanes_on.count(lane_)) return "lane " + lane_ + " is off";
  std::string why;
  if (paused(v, &why)) return why;
  for (const auto& b : baskets_) {
    if (b.event == event) return "basket " + b.id + " is open in the event (" + arb::st_str(b.st) + ")";
    if (in_flight(b)) return "basket " + b.id + " is in flight";
  }
  if (const auto c = cool_until_.find(event); c != cool_until_.end() && now < c->second)
    return "the event cools down " + std::to_string((c->second - now) / 1000) + " s more";
  // gw takes an IOC lane's order that grows a position only while the fees every IOC lane has
  // charged today are under the cost budget: the first leg's own fee could spend it and leave the
  // rest refused, so the whole basket's fee bound must fit
  if (v.fees + p.fees >= Gateway::kCostBudget * 100)
    return "the IOC lanes' cost budget: " + arb::usd(v.fees) + " charged today and the basket's fees up to " + arb::usd(p.fees) +
           " reach " + arb::usd(Gateway::kCostBudget * 100);
  if (!env_.private_up || !env_.private_up()) return "the private feed is down";
  if (!restart_ready(s, now)) return "S1's positions wait for an account read after the start";
  // where each leg starts must be exact: S1's own orders in the event ended, their fills booked
  for (const auto& [cid, o] : v.orders)
    if (o.lane == lane_ && event_of(o.ticker, s) == event && (!o.terminal || o.executed < o.filled))
      return "S1's order " + cid + " in the event has not ended with its fills booked";
  if (env_.write_room) {
    std::map<int, int> legs;
    for (const auto& l : p.legs)
      if (const auto m = s.markets.find(l.ticker); m != s.markets.end() && m->second.exchange_index) ++legs[*m->second.exchange_index];
    for (const auto& [shard, n] : legs) {
      const double room = env_.write_room(shard);
      if (room < 10.0 * n) {
        char b[160];
        std::snprintf(b, sizeof b, "write budget: shard %d has %.0f tokens, %d legs need %d", shard, room, n, 10 * n);
        return b;
      }
    }
  }
  return "";
}

void ArbEngine::scan(const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now) {
  shapes_.clear();
  const std::string today = et_date(now), tomorrow = et_date_after(today);
  std::map<std::string, std::vector<std::string>> events;
  std::set<std::string> want, want_events;
  for (const auto& [tk, m] : s.markets) {
    if (!of_series(tk, series_) || !trading(m) || !m.close_ms || *m.close_ms - now <= p_.limits.min_close_lead_ms) continue;
    const std::string d = rest::outcome_date(m);
    if (d != today && d != tomorrow) continue;
    events[event_of(tk, s)].push_back(tk);
    want.insert(tk);
  }
  for (const auto& [ev, tks] : events) want_events.insert(ev);
  for (const auto& b : baskets_) {
    want_events.insert(b.event);
    for (const auto& l : b.plan.legs) want.insert(l.ticker);
  }
  if (env_.want) env_.want(want);
  if (env_.want_events) env_.want_events(want_events);
  bool fired = false;
  for (const auto& [ev, tks] : events) {
    json row = {{"ts_ms", now}, {"event", ev}, {"kind", nullptr}, {"mode", p_.mode}, {"legs", json::array()},
                {"qty", nullptr}, {"edge", nullptr}, {"cost", nullptr}, {"payout_min", nullptr}, {"fired", false}};
    std::string why, sig = "none";
    if (const EventShape* sh = shape_of(ev, s, &why)) {
      std::vector<arb::ArbMarket> ms;
      for (const auto& tk : sh->tickers)
        if (const auto m = s.markets.find(tk); m != s.markets.end())
          ms.push_back(arb::arb_market(m->second, env_.book ? env_.book(tk) : std::nullopt, v, s));
      const auto plans = arb::find_baskets(*sh, ms, now, limits(*sh, v, s), &why);
      if (!plans.empty()) {
        const Plan& p = plans.front();
        row["kind"] = arb::kind_str(p.kind);
        for (const auto& l : p.legs)
          row["legs"].push_back({{"tk", l.ticker}, {"side", side_str(l.side)}, {"px", price_str(l.planned)},
                                 {"limit", price_str(l.limit)}, {"shown", qty_to_number(l.shown)}});
        row["qty"] = qty_to_number(p.qty);
        row["edge"] = money_str(p.edge / 100);
        row["cost"] = money_str(p.cost / 100);
        row["payout_min"] = money_str(p.payout_min / 100);
        sig = std::string(arb::kind_str(p.kind)) + "|" + std::to_string(p.qty) + "|" + std::to_string(p.edge);
        why = fired ? "a basket fired this step" : blocked(ev, p, v, s, now);
        if (why.empty()) {
          why = "fired " + fire(p, v, now);
          row["fired"] = fired = true;
          sig += "|fired";
        }
      }
    }
    row["why"] = why;
    auto& sc = scanned_[ev];
    sc.last = row;
    if (sig != sc.sig || now - sc.row_ms >= 60000) {
      sc.sig = sig;
      sc.row_ms = now;
      record("arb_scans", row);
    }
  }
  for (auto it = scanned_.begin(); it != scanned_.end();) it = events.count(it->first) ? std::next(it) : scanned_.erase(it);
}

// A new basket: kept (and so written to s1.json) before its first leg goes, with where each leg
// stands now. Returns its id.
std::string ArbEngine::fire(const Plan& p, const Gateway::SysView& v, std::int64_t now) {
  Basket b;
  b.id = lane_ + "-" + base36(now) + "-b" + std::to_string(++seq_);
  b.event = p.event;
  b.plan = p;
  b.plan.id = b.id;
  b.t0 = now;
  for (const auto& l : p.legs) b.base.push_back(position(v, lane_, l.ticker));
  const auto& first = b.plan.legs.front();
  send(b, 0, 'l', first.side, first.planned, p.qty, first.planned, now, "");
  DLOG_INFO("%s %s: fires %s %s, %s x%s, edge %s", lane_.c_str(), p.event.c_str(), b.id.c_str(), arb::kind_str(p.kind),
            first.ticker.c_str(), arb::ctr(p.qty).c_str(), arb::usd(p.edge).c_str());
  set_state(b, St::FIRST, "the scarcest leg, " + std::string(side_str(first.side)) + " " + first.ticker + " at " +
                              price_str(first.planned) + " x" + arb::ctr(p.qty));
  baskets_.push_back(std::move(b));
  return baskets_.back().id;
}

// An IOC of the basket's: recorded in the basket, and queued to go to gw once s1.json says so
// (flush). cid "S" + the basket's id + "l" + its leg + c (a completion) or u<pass> (an unwind).
void ArbEngine::send(Basket& b, int leg, char role, Side side, Px price, Qty count, Px touch, std::int64_t now,
                     const std::string& suffix) {
  BasketOrder o;
  o.cid = "S" + b.id + "l" + std::to_string(leg) + suffix;
  o.leg = leg;
  o.role = role;
  o.side = side;
  o.price = price;
  o.count = count;
  o.sent_ms = now;
  PlaceIntent pi;
  pi.cid = o.cid;
  pi.owner = Owner::SYS;
  pi.lane = lane_;
  pi.venue = Venue::K;
  pi.ticker = b.plan.legs[leg].ticker;
  pi.side = side;
  pi.price = price;
  pi.count = count;
  pi.tif = Tif::IOC;
  pi.ttl_s = 0;
  pi.slip_ticks = role == 'l' ? 0 : 1;
  pi.override_guard = false;   // within two ticks of the touch: admission's PRICE_BAND stays on
  pi.reduce_only = false;
  pi.t_recv_ms = now;
  pi.touch = touch;
  auto reply = std::make_shared<std::promise<OrderReply>>();
  answers_[o.cid] = reply->get_future();
  pi.reply = std::move(reply);
  outbox_.push_back(std::move(pi));
  b.orders.push_back(std::move(o));
  dirty_ = true;
}

// s1.json first, then the queued orders to gw: a start after a crash between the two finds the
// order in the file and counts it by S1's positions. One gw does not take was refused.
void ArbEngine::flush() {
  if (outbox_.empty()) return;
  save();
  for (auto& pi : outbox_) {
    const std::string cid = pi.cid;
    if (env_.post && env_.post(std::move(pi))) continue;
    answers_.erase(cid);
    for (auto& b : baskets_)
      for (auto& o : b.orders)
        if (o.cid == cid) {
          o.is = BasketOrder::Is::REFUSED;
          o.why = "gw's inbox is full";
          o.done_ms = o.sent_ms;
        }
    dirty_ = true;
  }
  outbox_.clear();
}

// Each order: gw's answer, then the view
void ArbEngine::follow(Basket& b, const Gateway::SysView& v, std::int64_t now) {
  using Is = BasketOrder::Is;
  for (auto& o : b.orders) {
    if (o.is == Is::SENT) {
      const auto f = answers_.find(o.cid);
      if (f == answers_.end()) {
        o.is = Is::UNKNOWN;
        o.why = "gw's answer is lost: the view decides";
      } else if (f->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        OrderReply r;
        bool dropped = false;
        try {
          r = f->second.get();
        } catch (const std::future_error&) {
          dropped = true;
        }
        answers_.erase(f);
        if (dropped) {
          o.is = Is::REFUSED;
          o.why = "gw dropped it unhandled (a KILL or Stop deleted it)";
          o.done_ms = now;
        } else if (r.code.empty()) {
          const auto j = json::parse(r.ord, nullptr, false);
          std::optional<Qty> filled;
          std::string st;
          if (j.is_object() && j.contains("filled") && j["filled"].is_number()) {
            filled = qty_from_number(j["filled"].get<double>());
            st = j.value("st", "");
          }
          if (!filled) {
            o.is = Is::UNKNOWN;
            o.why = "gw's answer has no fill count: the view decides";
          } else {
            o.filled = *filled;
            o.why = st;
            o.is = st == "FILL" || st == "CXL" || st == "EXP" || st == "REJ" ? Is::ENDED : Is::OPEN;
            if (o.is == Is::ENDED) o.done_ms = now;
          }
        } else if (r.code == "EXCHANGE") {
          o.is = Is::UNKNOWN;
          o.why = r.msg;
        } else {
          o.is = Is::REFUSED;
          o.why = r.code + ": " + r.msg;
          o.done_ms = now;
        }
        dirty_ = true;
      } else if (now - o.sent_ms > p_.answer_ms) {
        answers_.erase(f);
        o.is = Is::UNKNOWN;
        o.why = "no answer from gw in " + std::to_string(p_.answer_ms / 1000) + " s: the view decides";
        dirty_ = true;
      }
    }
    const auto it = o.prior ? v.orders.end() : v.orders.find(o.cid);
    if (it == v.orders.end()) continue;
    const auto& x = it->second;
    if ((o.is == Is::OPEN || o.is == Is::UNKNOWN) && x.terminal) {
      o.is = Is::ENDED;
      o.done_ms = x.done_ms ? x.done_ms : now;
      o.why = x.why;
      dirty_ = true;
    }
    if (o.is == Is::ENDED && (o.filled != std::max(o.filled, x.filled) || o.executed != x.executed || o.exec_cost != x.exec_cost)) {
      o.filled = std::max(o.filled, x.filled);
      o.executed = x.executed;
      o.exec_cost = x.exec_cost;
      o.fees = x.fees;
      dirty_ = true;
    }
  }
}

// Every order of the basket has ended and its fills are booked, so S1's positions hold what the
// basket did: an order that filled has its fills executed, as the view shows them (or, gone from
// the view, as it last showed them): a 201 runs ahead of them. A previous run's are in the
// positions once the start's account read is in (restart_ready).
bool ArbEngine::booked(const Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now) {
  using Is = BasketOrder::Is;
  if (b.restored && !restart_ready(s, now)) return false;
  for (const auto& o : b.orders) {
    if (o.is == Is::REFUSED) continue;
    if (o.is != Is::ENDED) return false;
    if (o.prior || o.filled <= 0) continue;
    const auto it = v.orders.find(o.cid);
    const Qty executed = it == v.orders.end() ? o.executed : it->second.executed;
    const Qty filled = it == v.orders.end() ? o.filled : std::max(o.filled, it->second.filled);
    if (executed < filled) return false;
  }
  return true;
}

void ArbEngine::drive(Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now) {
  using Is = BasketOrder::Is;
  if (b.st == St::DONE) return;
  follow(b, v, now);
  // every market of the basket closed: what it holds settles
  if (s.listing_ready && std::none_of(b.plan.legs.begin(), b.plan.legs.end(), [&s](const arb::Leg& l) {
        const auto m = s.markets.find(l.ticker);
        return m != s.markets.end() && trading(m->second);
      }))
    return finish(b, v, "its markets have closed: what it holds settles", now);
  for (const auto& l : b.plan.legs)
    if (v.held.count(l.ticker)) {
      if (!b.stuck) freeze(b, "Manual took " + l.ticker + " over: the basket leaves the event and unwinds nothing", true, now);
      return;
    }
  if (b.stuck) return;
  // answers to come; an order gw could not place for sure holds the whole basket
  const BasketOrder* unknown = nullptr;
  bool pending = false;
  for (const auto& o : b.orders) {
    if (o.is == Is::SENT || o.is == Is::OPEN) pending = true;
    if (o.is == Is::UNKNOWN && !unknown) unknown = &o;
  }
  if (unknown) {
    if (now - unknown->sent_ms > p_.uncertain_max_ms)
      return freeze(b, unknown->cid + " still uncertain after " + std::to_string(p_.uncertain_max_ms / 1000) + " s", true, now);
    if (b.st != St::UNCERTAIN) {
      b.resume = b.st;
      set_state(b, St::UNCERTAIN, unknown->cid + " (" + unknown->why + "): nothing more goes in the event until the view shows it ended");
    }
    return;
  }
  if (pending) return;
  if (b.st == St::UNCERTAIN) set_state(b, b.resume, "the uncertain order ended");
  // what each leg's own orders filled, by the venue's counts
  std::vector<Qty> got(b.plan.legs.size(), 0);
  for (const auto& o : b.orders)
    if (o.role != 'u' && o.is == Is::ENDED) got[o.leg] += o.filled;
  if (b.st == St::FIRST && got[0] <= 0) {
    const auto& o = b.orders.front();
    return finish(b, v, o.is == Is::REFUSED ? "the first leg was refused: " + o.why : "the first leg filled nothing", now);
  }
  if ((b.st == St::REST || b.st == St::COMPLETE) && std::all_of(got.begin(), got.end(), [&b](Qty q) { return q == b.target; })) {
    // complete; its legging (a completion's price) is read from its fills, once they are booked
    std::int64_t ended = 0;
    for (const auto& o : b.orders) ended = std::max(ended, o.done_ms);
    if (!booked(b, v, s, now) && now - ended <= p_.uncertain_max_ms) return;
    return finish(b, v, "complete: every leg holds " + arb::ctr(b.target), now);
  }
  // may it send?
  std::string no;
  if (p_.mode != "trade") no = "scan mode: it sends nothing";
  else if (v.run != RunState::RUN) no = std::string("Systematic is ") + run_code(v.run);
  else if (!v.lanes_on.count(lane_)) no = "lane " + lane_ + " is off";
  else if (!env_.private_up || !env_.private_up()) no = "the private feed is down";
  if (!no.empty()) {
    if (b.st != St::FROZEN) freeze(b, no, false, now);
    return;
  }
  if (b.st == St::FROZEN) set_state(b, St::UNWIND, "it may send again: a stranded basket is only unwound");
  if (b.st == St::FIRST) {
    b.target = got[0];
    for (std::size_t k = 1; k < b.plan.legs.size(); ++k) {
      const auto& l = b.plan.legs[k];
      send(b, static_cast<int>(k), 'l', l.side, l.limit, b.target, l.planned, now, "");
    }
    b.deadline = now + p_.complete_ms;
    return set_state(b, St::REST, "the first leg filled " + arb::ctr(b.target) + ": every other leg for as many");
  }
  if (b.st == St::REST && now <= b.deadline) {
    // the completion: a short leg's shortfall one tick through its planned touch, while the live
    // touch is within that tick and short of Systematic's own order there
    std::string why;
    int sent = 0;
    for (std::size_t k = 0; k < b.plan.legs.size(); ++k) {
      if (got[k] >= b.target) continue;
      const auto& l = b.plan.legs[k];
      const auto m = s.markets.find(l.ticker);
      std::optional<MdBook> book = env_.book ? env_.book(l.ticker) : std::nullopt;
      if (m == s.markets.end() || !book || book->stale) {
        why += "; " + l.ticker + " has no fresh book";
        continue;
      }
      const auto a = arb::arb_market(m->second, std::move(book), v, s);
      const bool buy = l.side == Side::BUY;
      const auto px = buy ? grid_ceil(l.planned + 1, m->second.price_ranges) : grid_floor(l.planned - 1, m->second.price_ranges);
      const auto& touch = buy ? a.book->asks : a.book->bids;
      if (!px || touch.empty() || (buy ? touch.front().px > *px || *px >= a.own_ask : touch.front().px < *px || *px <= a.own_bid)) {
        why += "; " + l.ticker + "'s touch " + (touch.empty() ? "is gone" : "is " + price_str(touch.front().px)) +
               ", over a tick from " + price_str(l.planned);
        continue;
      }
      send(b, static_cast<int>(k), 'c', l.side, *px, b.target - got[k], touch.front().px, now, "c");
      ++sent;
    }
    if (sent) return set_state(b, St::COMPLETE, std::to_string(sent) + " short leg(s) completed a tick through" + why);
    set_state(b, St::UNWIND, "short, and no completion" + why);
  } else if (b.st == St::REST || b.st == St::COMPLETE) {
    set_state(b, St::UNWIND, b.st == St::REST ? "short past complete_ms" : "short after the completion");
  }
  if (b.st == St::UNWIND) unwind(b, v, s, now);
}

// One unwind pass: what each leg holds is S1's position less where it began, read once every
// order is booked; each leg's excess over the least of them is closed one tick through the touch
void ArbEngine::unwind(Basket& b, const Gateway::SysView& v, const rest::Snapshot& s, std::int64_t now) {
  if (!booked(b, v, s, now)) {
    // fills the venue reported that never reach the book (gw's fills_due read did not find them)
    for (const auto& o : b.orders)
      if (o.is == BasketOrder::Is::ENDED && !o.prior && o.executed < o.filled && now - o.done_ms > p_.uncertain_max_ms)
        return freeze(b, o.cid + " filled " + arb::ctr(o.filled) + " and " + arb::ctr(o.executed) + " of its fills came in " +
                             std::to_string(p_.uncertain_max_ms / 1000) + " s", true, now);
    return;
  }
  std::vector<Qty> held;
  Qty least = std::numeric_limits<Qty>::max();
  for (std::size_t k = 0; k < b.plan.legs.size(); ++k) {
    const auto& l = b.plan.legs[k];
    held.push_back(dir(l.side) * (position(v, lane_, l.ticker) - b.base[k]));
    least = std::min(least, held.back());
  }
  for (std::size_t k = 0; k < held.size(); ++k)
    if (held[k] < 0)
      return freeze(b, lane_ + " holds " + arb::ctr(held[k]) + " in " + b.plan.legs[k].ticker + " against the basket's side", true, now);
  if (std::all_of(held.begin(), held.end(), [least](Qty q) { return q == least; }))
    return finish(b, v, (b.tries > 0 || b.orphan ? "unwound: every leg holds " : "complete: every leg holds ") + arb::ctr(least), now);
  if (now - b.tried_ms < p_.unwind_every_ms) return;
  if (b.tries >= p_.unwind_tries) {
    std::string excess;
    for (std::size_t k = 0; k < held.size(); ++k)
      if (held[k] > least) excess += (excess.empty() ? "" : ", ") + arb::ctr(held[k] - least) + " " + b.plan.legs[k].ticker;
    return freeze(b, std::to_string(p_.unwind_tries) + " unwind passes spent, " + excess + " over the complete set held", true, now);
  }
  ++b.tries;
  b.tried_ms = now;
  std::string why;
  int sent = 0;
  for (std::size_t k = 0; k < held.size(); ++k) {
    if (held[k] <= least) continue;
    const auto& l = b.plan.legs[k];
    const auto m = s.markets.find(l.ticker);
    std::optional<MdBook> book = env_.book ? env_.book(l.ticker) : std::nullopt;
    if (m == s.markets.end() || !trading(m->second) || !book || book->stale) {
      why += "; " + l.ticker + (m == s.markets.end() || !trading(m->second) ? " does not trade" : " has no fresh book");
      continue;
    }
    const auto a = arb::arb_market(m->second, std::move(book), v, s);
    const auto& g = m->second.price_ranges;
    if (a.book->bids.empty() || a.book->asks.empty()) {
      why += "; " + l.ticker + "'s book is one-sided";
      continue;
    }
    const Px bid = a.book->bids.front().px, ask = a.book->asks.front().px;
    if (ask - bid > std::max<Px>(3 * grid_step(bid, g), 300)) {
      why += "; " + l.ticker + "'s spread " + price_str(bid) + "-" + price_str(ask) + " is over 3 ticks and 3c";
      continue;
    }
    // a long leg sells one tick under the bid; a short one buys one tick over the ask
    const Side close = l.side == Side::BUY ? Side::SELL : Side::BUY;
    Px px = close == Side::SELL ? grid_floor(bid - 1, g).value_or(bid) : grid_ceil(ask + 1, g).value_or(ask);
    if (close == Side::SELL ? px <= a.own_bid : px >= a.own_ask) {
      why += "; " + l.ticker + ": Systematic's own " + (close == Side::SELL ? "bid " + price_str(a.own_bid) : "ask " + price_str(a.own_ask)) +
             " is in the way";
      continue;
    }
    send(b, static_cast<int>(k), 'u', close, px, held[k] - least, close == Side::SELL ? bid : ask, now, "u" + std::to_string(b.tries));
    ++sent;
  }
  set_state(b, St::UNWIND, "pass " + std::to_string(b.tries) + ": " + std::to_string(sent) + " leg(s) closing to " +
                               arb::ctr(least) + why);
}

void ArbEngine::set_state(Basket& b, St st, const std::string& why) {
  b.st = st;
  b.why = why;
  dirty_ = true;
  DLOG_INFO("%s basket %s (%s): %s %s", lane_.c_str(), b.id.c_str(), b.event.c_str(), arb::st_str(st), why.c_str());
  record("baskets", basket_row(b, view_));
}

void ArbEngine::freeze(Basket& b, const std::string& why, bool stuck, std::int64_t now) {
  b.stuck = stuck;
  set_state(b, St::FROZEN, why);
  if (env_.alert)
    env_.alert("CRIT", lane_ + " basket " + b.id + " in " + b.event + " is incomplete and frozen: " + why +
                           (stuck ? "; what it holds stays, counted by the risk book, until the operator acts or it settles"
                                  : "; it is unwound once it may send again"));
}

void ArbEngine::finish(Basket& b, const Gateway::SysView& v, const std::string& why, std::int64_t now) {
  const Micros lg = arb::legging_pnl(b, v);
  const bool was = legging_ <= -p_.legging_pause;
  legging_ += lg;
  b.t_end = now;
  for (const auto& o : b.orders) answers_.erase(o.cid);
  cool_until_[b.event] = now + p_.event_cooldown_ms;
  ++done_;
  set_state(b, St::DONE, why);
  if (!was && legging_ <= -p_.legging_pause && !pause_alerted_) {
    pause_alerted_ = true;
    if (env_.alert)
      env_.alert("CRIT", lane_ + " paused: legging " + arb::usd(legging_) + ", past -" + arb::usd(p_.legging_pause) +
                             "; no new basket until the lane is turned off and on");
  }
}

json ArbEngine::basket_row(const Basket& b, const Gateway::SysView* v) const {
  static const Gateway::SysView none;
  const Gateway::SysView& view = v ? *v : none;
  std::vector<Qty> got(b.plan.legs.size(), 0);
  for (const auto& o : b.orders)
    if (o.leg >= 0 && o.leg < static_cast<int>(got.size()))
      got[o.leg] += (o.role == 'u' ? -1 : 1) * (o.is == BasketOrder::Is::ENDED ? o.filled : 0);
  json legs = json::array();
  for (std::size_t k = 0; k < b.plan.legs.size(); ++k) {
    const auto& l = b.plan.legs[k];
    legs.push_back({{"tk", l.ticker}, {"side", side_str(l.side)}, {"planned", price_str(l.planned)}, {"limit", price_str(l.limit)},
                    {"filled", qty_to_number(got[k])}});
  }
  return {{"id", b.id},
          {"event", b.event},
          {"kind", b.orphan ? "ORPHAN" : arb::kind_str(b.plan.kind)},
          {"state", arb::st_str(b.st)},
          {"legs", legs},
          {"qty", qty_to_number(b.plan.qty)},
          {"target", qty_to_number(b.target)},
          {"edge_plan", b.orphan ? json(nullptr) : json(money_str(b.plan.edge / 100))},
          {"edge_real", money_str(arb::basket_edge(b, view) / 100)},
          {"legging", money_str(arb::legging_pnl(b, view) / 100)},
          {"t0_ms", b.t0},
          {"t_end_ms", b.t_end ? json(b.t_end) : json(nullptr)},
          {"tries", b.tries},
          {"stuck", b.stuck},
          {"why", b.why}};
}

void ArbEngine::record(const std::string& table, const json& row) {
  if (!env_.record || !env_.record(table, row.dump())) ++rows_lost_;
}

void ArbEngine::publish(const Gateway::SysView& v) {
  std::string why;
  const bool p = paused(v, &why);
  json open = json::array();
  for (const auto& b : baskets_) open.push_back(basket_row(b, &v));
  json events = json::object();
  for (const auto& [ev, sc] : scanned_) events[ev] = sc.last;
  std::lock_guard<std::mutex> lk(status_mu_);
  status_ = {{"lane", lane_},
             {"mode", p_.mode},
             {"paused", p},
             {"pause", p ? json(why) : json(nullptr)},
             {"legging", money_str(legging_ / 100)},
             {"state_error", state_error_.empty() ? json(nullptr) : json(state_error_)},
             {"baskets", open},
             {"done", done_},
             {"rows_lost", rows_lost_.load()},
             {"events", events}};
}

json ArbEngine::status() const {
  std::lock_guard<std::mutex> lk(status_mu_);
  if (status_.is_null()) return {{"lane", lane_}, {"mode", p_.mode}};
  return status_;
}

}  // namespace deskd
