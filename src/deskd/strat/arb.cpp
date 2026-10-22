#include "deskd/strat/arb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

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

}  // namespace deskd::arb
