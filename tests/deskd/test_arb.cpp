// S1's basket finder (strat/arb.h):
//   - book_without_own: Systematic's resting orders come off the book, every lane's
//   - YES_COVER on a ladder proven exhaustive: the scarcest leg first, a dollar back a contract,
//     the fee bound's rounding unit a leg ($0.01 leaves this lock too thin); none on asks summing
//     $0.99 or on a gapped ladder
//   - NO_SET on the old scanner's bids (.45/.42/.20) with the venue's flag: |S| - 1 back, the
//     lane's budget counted as gw counts it (market by market, or by state), the greedy pick and
//     max_legs; none without the flag or under $1
//   - DOMINANCE on the old cases: "above 83" over "above 79", a bracket inside a threshold; none
//     priced consistently or on disjoint brackets
//   - 1c legs: a cover with a 1c tail, a NO set that needs its 1c bid
//   - the walk: two grid ticks through the touch on a cent and a tapered grid, never a third;
//     never into Systematic's own resting order (STP), a leg dropped when the touch would cross it
//   - every filter: the outcome date, the close lead, not open, the book (none, stale, crossed),
//     fee metadata, Manual anywhere in the event (taken over, a resting order, a position, no
//     account read), a market not scanned; arb_market's view of a market
//   - sizing: max_qty, the levels, gw's inventory room, the budget, the cash, whole contracts, a
//     lock only mid-size; and 3,000 random ladders against a size-by-size scan
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "deskd/gw/event_risk.h"
#include "deskd/gw/fees.h"
#include "deskd/strat/arb.h"
#include "deskd/strat/maker.h"

using namespace deskd;
using namespace deskd::arb;

namespace {
constexpr std::int64_t NOW = 1792684800000;   // 2026-10-22 12:00 ET
constexpr std::int64_t HOUR = 3600000;
Micros usd(double d) { return std::llround(d * 1000000); }
Qty ctr(int n) { return n * kOneContract; }
Px px(double p) { return static_cast<Px>(std::llround(p * kOneDollar)); }
MdLevel L(double p, int n) { return {px(p), ctr(n)}; }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

const std::vector<kalshi::PriceRange> CENT{{0, 10000, 100}};
// the tapered grid: 0.1c under 10c and over 90c
const std::vector<kalshi::PriceRange> TAPERED{{0, 1000, 10}, {1000, 9000, 100}, {9000, 10000, 10}};

kalshi::MarketRow mk(const std::string& tk, const std::string& type, std::optional<double> floor,
                     std::optional<double> cap) {
  kalshi::MarketRow m;
  m.ticker = tk;
  m.event_ticker = tk.substr(0, tk.rfind('-'));
  m.status = "active";
  m.strike_type = type;
  m.floor_strike = floor;
  m.cap_strike = cap;
  m.exchange_index = 1;
  m.open_ms = NOW - 10 * HOUR;
  m.close_ms = NOW + 12 * HOUR;
  m.price_ranges = CENT;
  m.fee_type = "quadratic";
  m.fee_multiplier = 1.0;
  m.fee_ms = NOW - 1000;
  return m;
}

MdBook book(const std::string& tk, std::vector<MdLevel> bids, std::vector<MdLevel> asks) {
  MdBook b;
  b.ticker = tk;
  b.bids = std::move(bids);
  b.asks = std::move(asks);
  b.stale = false;
  b.reason.clear();
  return b;
}

// An event, its listing, its shape, and each market's book: the markets the finder is given
struct Scene {
  kalshi::EventRow row;
  std::map<std::string, kalshi::MarketRow> listing;
  EventShape shape;
  std::vector<std::string> tk;                 // as given
  std::map<std::string, MdBook> books;
  std::map<std::string, Px> own_bid, own_ask;
  std::map<std::string, Qty> sys_position;
  std::vector<ArbMarket> markets() const {
    std::vector<ArbMarket> out;
    for (const auto& t : tk) {
      ArbMarket a;
      a.row = &listing.at(t);
      if (books.count(t)) a.book = books.at(t);
      if (own_bid.count(t)) a.own_bid = own_bid.at(t);
      if (own_ask.count(t)) a.own_ask = own_ask.at(t);
      if (sys_position.count(t)) a.sys_position = sys_position.at(t);
      out.push_back(std::move(a));
    }
    return out;
  }
  std::vector<Plan> find(const Limits& lim, std::string* why = nullptr) const {
    std::string w;
    return find_baskets(shape, markets(), NOW, lim, why ? why : &w);
  }
  void set(const std::string& t, std::vector<MdLevel> bids, std::vector<MdLevel> asks) { books[t] = book(t, std::move(bids), std::move(asks)); }
};
Scene scene(const std::string& event, std::optional<bool> flag, const std::vector<kalshi::MarketRow>& ms, double lattice = 0) {
  Scene s;
  s.row.event_ticker = event;
  s.row.series_ticker = event.substr(0, event.find('-'));
  s.row.mutually_exclusive = flag;
  s.row.markets = ms;
  for (const auto& m : ms) {
    s.listing[m.ticker] = m;
    s.tk.push_back(m.ticker);
  }
  std::string why;
  const auto sh = event_shape(s.row, s.listing, lattice, &why);
  CHECK(sh.has_value());
  if (sh) s.shape = *sh;
  return s;
}

// a city's high on one day: under 79, 79-80, over 80 (the 2026-09-05 LAX ladder's strikes)
std::string DAY = "KXHIGHNY-26OCT22";
std::string T79(const std::string& e = DAY) { return e + "-T79"; }
std::string B79(const std::string& e = DAY) { return e + "-B79.5"; }
std::string T80(const std::string& e = DAY) { return e + "-T80"; }
Scene ladder(const std::string& e = DAY, std::optional<bool> flag = true) {
  return scene(e, flag, {mk(T79(e), "less", std::nullopt, 79), mk(B79(e), "between", 79, 80), mk(T80(e), "greater", 80, std::nullopt)});
}
// three markets the venue flags exclusive, without strikes (the old scanner's KXBTC brackets)
const std::string WHO = "KXWHO-26OCT22";
Scene flagged(std::optional<bool> flag = true) {
  return scene(WHO, flag, {mk(WHO + "-A", "custom", std::nullopt, std::nullopt), mk(WHO + "-B", "custom", std::nullopt, std::nullopt),
                           mk(WHO + "-C", "custom", std::nullopt, std::nullopt)});
}

Limits big() {
  Limits l;
  l.budget_left = usd(100);
  l.cash_left = usd(1000);
  return l;
}

const Plan* of(const std::vector<Plan>& ps, Kind k) {
  for (const auto& p : ps)
    if (p.kind == k) return &p;
  return nullptr;
}
const Leg* leg(const Plan& p, const std::string& tk) {
  for (const auto& l : p.legs)
    if (l.ticker == tk) return &l;
  return nullptr;
}
// the best plan's size; -1: none
Qty size_of(const std::vector<Plan>& ps) { return ps.empty() ? -1 : ps[0].qty; }

void without_own() {
  const std::string tk = "KXHIGHNY-26OCT22-B79.5";
  Gateway::SysView v;
  auto order = [&](const std::string& cid, const std::string& t, Side side, Px price, Qty left, OrdState st, bool terminal = false) {
    Gateway::SysOrder o;
    o.cid = cid;
    o.lane = "M2";
    o.ticker = t;
    o.side = side;
    o.price = price;
    o.count = o.left = left;
    o.state = st;
    o.terminal = terminal;
    v.orders[cid] = o;
  };
  order("SM2-a-1", tk, Side::BUY, 4400, ctr(5), OrdState::WORK);
  order("SP1-a-2", tk, Side::SELL, 4800, ctr(3), OrdState::PART);    // another lane's: off too
  order("SS1-a-3", tk, Side::BUY, 4500, ctr(2), OrdState::PEND);     // not in the book yet
  order("SM2-a-4", tk, Side::BUY, 4500, ctr(2), OrdState::CXL, true);
  order("SM2-a-5", "KXHIGHNY-26OCT22-T80", Side::BUY, 4500, ctr(2), OrdState::WORK);
  MdBook b = book(tk, {L(.45, 10), L(.44, 8)}, {L(.48, 3), L(.49, 1)});
  book_without_own(b, v, tk);
  CHECK((b.bids == std::vector<MdLevel>{L(.45, 10), L(.44, 3)}));
  CHECK((b.asks == std::vector<MdLevel>{L(.49, 1)}));   // a level ours alone made goes
}

void yes_cover() {
  Scene s = ladder();
  CHECK(s.shape.exclusive && s.shape.exhaustive && s.shape.tickers.size() == 3);
  s.set(T79(), {L(.25, 10)}, {L(.30, 5)});
  s.set(B79(), {L(.26, 10)}, {L(.31, 20)});
  s.set(T80(), {L(.28, 10)}, {L(.33, 20)});
  Limits lim = big();
  lim.fee_precision = 100;   // $0.0001: a direct member
  auto ps = s.find(lim);
  CHECK(ps.size() == 1);
  const Plan* p = of(ps, Kind::YES_COVER);
  CHECK(p != nullptr);
  if (!p) return;
  CHECK(p->event == DAY && p->legs.size() == 3 && p->shard == 1);
  CHECK(p->legs[0].ticker == T79());   // the 5-lot goes first
  CHECK(p->qty == ctr(5));
  for (const auto& l : p->legs) CHECK(l.side == Side::BUY && l.planned == l.limit && l.market == s.shape.index(l.ticker));
  CHECK(p->payout_min == usd(5));      // one bracket pays, whichever
  CHECK(p->cost == usd(5 * .94));
  CHECK(p->fees == kalshi_model_fee(1, 3000, ctr(5)) + kalshi_model_fee(1, 3100, ctr(5)) + kalshi_model_fee(1, 3300, ctr(5)) + 300);
  CHECK(p->fees == 225750 + 300);
  CHECK(p->edge == p->payout_min - p->cost - p->fees && p->edge >= lim.thr * p->qty);
  CHECK(plan_edge(s.shape, p->legs, p->qty) == p->edge);
  // gw while the legs work: market by market, every leg's reservation; by state, all but the
  // cheapest (whichever settles YES, its leg loses nothing)
  CHECK(p->gross == usd(4.70) && p->contingent_worst == usd(3.20));
  // a leg that fills a tick worse locks 5c less
  auto worse = p->legs;
  worse[1].cost += 100 * p->qty;
  CHECK(plan_edge(s.shape, worse, p->qty) == p->edge - usd(.05));

  // at $0.01 each leg's rounding unit is a cent: $0.0442 locked on 5, under 1c a contract
  std::string why;
  CHECK(s.find(big(), &why).empty());
  CHECK(has(why, "YES_COVER: locks $0.0442 on 5 at best, under the $0.0100 a contract it needs"));
  // asks summing $0.99 lock nothing after fees
  s.set(T79(), {L(.25, 10)}, {L(.32, 5)});
  s.set(B79(), {L(.26, 10)}, {L(.33, 20)});
  s.set(T80(), {L(.28, 10)}, {L(.34, 20)});
  CHECK(s.find(lim).empty());

  // a gapped ladder (under 79, over 85) is exclusive, not exhaustive: nothing covers 79-85
  Scene g = scene(DAY, true, {mk(T79(), "less", std::nullopt, 79), mk(DAY + "-T85", "greater", 85, std::nullopt)});
  g.set(T79(), {L(.20, 10)}, {L(.30, 10)});
  g.set(DAY + "-T85", {L(.20, 10)}, {L(.31, 10)});
  CHECK(g.find(lim, &why).empty() && has(why, "YES_COVER: not proven exhaustive"));
  // six brackets with gaps on the real line: exhaustive only on KXHIGH's whole degrees
  const std::vector<kalshi::MarketRow> six{mk(DAY + "-T79", "less", std::nullopt, 79), mk(DAY + "-B79.5", "between", 79, 80),
                                           mk(DAY + "-B81.5", "between", 81, 82), mk(DAY + "-B83.5", "between", 83, 84),
                                           mk(DAY + "-B85.5", "between", 85, 86), mk(DAY + "-T86", "greater", 86, std::nullopt)};
  for (double lattice : {0.0, 1.0}) {
    Scene l6 = scene(DAY, true, six, lattice);
    for (const auto& t : l6.tk) l6.set(t, {L(.10, 10)}, {L(.15, 10)});
    const auto found = l6.find(lim, &why);
    CHECK(found.size() == (lattice > 0 ? 1u : 0u));
    if (lattice > 0 && !found.empty()) CHECK(found[0].kind == Kind::YES_COVER && found[0].legs.size() == 6 && found[0].payout_min == usd(10));
    // more legs than max_legs
    Limits five = lim;
    five.max_legs = 5;
    CHECK(l6.find(five, &why).empty());
    if (lattice > 0) CHECK(has(why, "YES_COVER: 6 markets, not 2 to 5"));
  }
}

void no_set() {
  // the old scanner's case: bids .45/.42/.20 on three brackets the venue flags exclusive
  Scene s = flagged();
  CHECK(s.shape.exclusive && !s.shape.exhaustive && !s.shape.spans);
  s.set(WHO + "-A", {L(.45, 40)}, {L(.48, 40)});
  s.set(WHO + "-B", {L(.42, 25)}, {L(.45, 25)});
  s.set(WHO + "-C", {L(.20, 60)}, {L(.23, 60)});
  auto ps = s.find(big());
  CHECK(ps.size() == 1);
  const Plan* p = of(ps, Kind::NO_SET);
  CHECK(p != nullptr);
  if (!p) return;
  CHECK(p->qty == ctr(10));   // max_qty
  CHECK(p->legs.size() == 3 && p->legs[0].ticker == WHO + "-B" && p->legs[1].ticker == WHO + "-A" && p->legs[2].ticker == WHO + "-C");
  for (const auto& l : p->legs) CHECK(l.side == Side::SELL);
  CHECK(leg(*p, WHO + "-C")->cost == usd(8.00));   // NO at 1 - the bid
  CHECK(p->payout_min == usd(20));   // |S| - 1: one bracket settles YES at most
  CHECK(p->fees == 173250 + 170520 + 112000 + 30000);
  CHECK(p->edge == usd(.70) - p->fees);
  CHECK(p->gross == usd(19.30) && p->contingent_worst == usd(8.00));   // by state: the dearest NO, its bracket YES

  // the lane's $15 budget, counted as gw counts S1 today (market by market): 7 x $1.93
  Limits lim = big();
  lim.budget_left = usd(15);
  ps = s.find(lim);
  CHECK(ps.size() == 1 && ps[0].qty == ctr(7) && ps[0].gross <= usd(15));
  // with deskd.event_risk: $8.00 at 10
  lim.by_state = true;
  ps = s.find(lim);
  CHECK(ps.size() == 1 && ps[0].qty == ctr(10));
  // the cash: the reservations and the fees
  lim = big();
  lim.cash_left = usd(10);
  ps = s.find(lim);
  CHECK(ps.size() == 1 && ps[0].qty == ctr(5) && ps[0].gross + ps[0].fees <= usd(10));
  std::string why;
  lim.cash_left = usd(1.5);
  CHECK(s.find(lim, &why).empty() && has(why, "NO_SET: a contract needs $2.0055 of cash, over the $1.5000 free"));
  lim = big();
  lim.budget_left = usd(1);
  CHECK(s.find(lim, &why).empty() && has(why, "NO_SET: a contract counts $1.9300, over the $1.0000 the budget leaves"));

  // no flag, no argument: disjoint brackets without the venue's word are not enough (an unflagged
  // event without strikes has no shape at all)
  Scene d = ladder(DAY, std::nullopt);
  CHECK(!d.shape.exclusive && d.shape.exhaustive);
  d.set(T79(), {L(.45, 40)}, {L(.99, 40)});
  d.set(B79(), {L(.42, 25)}, {L(.99, 25)});
  d.set(T80(), {L(.20, 60)}, {L(.99, 60)});
  CHECK(d.find(big(), &why).empty() && has(why, "NO_SET: the venue does not flag it mutually exclusive"));
  // bids under a dollar are only a market
  s.set(WHO + "-A", {L(.30, 40)}, {L(.33, 40)});
  s.set(WHO + "-B", {L(.30, 25)}, {L(.33, 25)});
  CHECK(s.find(big(), &why).empty() && has(why, "NO_SET: the best 3 bids sum to $0.8000 a contract"));

  // the pick: .60 and .50 lock alone; .40 adds 38c for 62c tied up and goes in, unless max_legs is 2
  s.set(WHO + "-A", {L(.60, 40)}, {L(.63, 40)});
  s.set(WHO + "-B", {L(.50, 40)}, {L(.53, 40)});
  s.set(WHO + "-C", {L(.40, 40)}, {L(.43, 40)});
  ps = s.find(big());
  CHECK(ps.size() == 1 && ps[0].legs.size() == 3);
  lim = big();
  lim.max_legs = 2;
  ps = s.find(lim);
  CHECK(ps.size() == 1 && ps[0].legs.size() == 2 && !leg(ps[0], WHO + "-C"));
  CHECK(ps.size() == 1 && ps[0].payout_min == usd(10));
  // a 1c bid would add 0.9c for 99c tied up, under the pair's 6.6c for 93c: it stays out
  s.set(WHO + "-C", {L(.01, 40)}, {L(.04, 40)});
  ps = s.find(big());
  CHECK(ps.size() == 1 && ps[0].legs.size() == 2 && !leg(ps[0], WHO + "-C"));
}

void dominance() {
  // T83 pays above 83, T79 above 79: T83's outcomes sit inside T79's, and no flag is needed
  const auto cum = [](const std::string& hard, const std::string& htype, double hf, std::optional<double> hc) {
    return scene(DAY, std::nullopt, {mk(hard, htype, hf, hc), mk(DAY + "-T79", "greater", 79, std::nullopt)});
  };
  Scene s = cum(DAY + "-T83", "greater", 83, std::nullopt);
  CHECK(s.shape.spans && !s.shape.exclusive && !s.shape.exhaustive);
  s.set(DAY + "-T83", {L(.60, 30)}, {L(.63, 30)});
  s.set(DAY + "-T79", {L(.50, 20)}, {L(.52, 20)});
  auto ps = s.find(big());
  CHECK(ps.size() == 1);
  const Plan* p = of(ps, Kind::DOMINANCE);
  CHECK(p != nullptr);
  if (!p) return;
  CHECK(p->legs.size() == 2 && p->qty == ctr(10));
  CHECK(p->legs[0].ticker == DAY + "-T79" && p->legs[0].side == Side::BUY && p->legs[0].planned == px(.52));   // 20 shown
  CHECK(p->legs[1].ticker == DAY + "-T83" && p->legs[1].side == Side::SELL && p->legs[1].planned == px(.60));  // 30 shown
  CHECK(p->payout_min == usd(10));   // $1 at least: at 78 the NO pays, at 81 both, at 85 the YES
  CHECK(p->fees == 168000 + 174720 + 20000);
  CHECK(p->edge == usd(.80) - p->fees);
  // priced consistently: nothing
  s.set(DAY + "-T83", {L(.40, 30)}, {L(.43, 30)});
  s.set(DAY + "-T79", {L(.60, 20)}, {L(.62, 20)});
  std::string why;
  CHECK(s.find(big(), &why).empty() && has(why, "DOMINANCE: " + DAY + "-T83 implies " + DAY + "-T79, bid 0.4000 against ask 0.6200"));
  // a range bracket inside a threshold: the same rule
  Scene r = cum(DAY + "-B84.5", "between", 84, 85);
  r.set(DAY + "-B84.5", {L(.55, 25)}, {L(.58, 25)});
  r.set(DAY + "-T79", {L(.45, 25)}, {L(.47, 25)});
  ps = r.find(big());
  CHECK(ps.size() == 1 && ps[0].kind == Kind::DOMINANCE && leg(ps[0], DAY + "-B84.5") && leg(ps[0], DAY + "-B84.5")->side == Side::SELL);
  CHECK(ps.size() == 1 && ps[0].edge == usd(.08 * 10) - kalshi_model_fee(1, 5500, ctr(10)) - kalshi_model_fee(1, 4700, ctr(10)) - 20000);
  // disjoint brackets imply nothing about each other
  Scene j = scene(DAY, std::nullopt, {mk(DAY + "-B84.5", "between", 84, 85), mk(DAY + "-B78.5", "between", 78, 79)});
  j.set(DAY + "-B84.5", {L(.55, 25)}, {L(.58, 25)});
  j.set(DAY + "-B78.5", {L(.45, 25)}, {L(.47, 25)});
  CHECK(j.find(big(), &why).empty() && has(why, "DOMINANCE: no market implies another"));
  // without strikes there is nothing to imply
  Scene f = flagged();
  for (const auto& t : f.tk) f.set(t, {L(.20, 10)}, {L(.40, 10)});
  f.find(big(), &why);
  CHECK(has(why, "DOMINANCE: a market has no strikes"));
}

void one_cent() {
  // a cover whose tail bracket is offered at 1c
  Scene s = ladder();
  s.set(T79(), {L(.46, 10)}, {L(.48, 10)});
  s.set(B79(), {L(.43, 10)}, {L(.45, 10)});
  s.set(T80(), {}, {L(.01, 10)});
  auto ps = s.find(big());
  const Plan* p = of(ps, Kind::YES_COVER);
  const Leg* tail = p ? leg(*p, T80()) : nullptr;
  CHECK(p && tail && tail->planned == px(.01) && p->qty == ctr(10));
  CHECK(p && p->edge == usd(.60) - (174720 + 173250 + 6930 + 30000));
  // a NO set that locks thr only with its 1c bid: .63 and .41 lock 0.7c a contract, the 1c leg
  // adds 0.9c for 99c tied up and raises the lock per dollar
  Scene n = ladder();
  n.set(T79(), {L(.63, 10)}, {L(.66, 10)});
  n.set(B79(), {L(.41, 10)}, {L(.44, 10)});
  n.set(T80(), {L(.01, 10)}, {L(.04, 10)});
  ps = n.find(big());
  p = of(ps, Kind::NO_SET);
  tail = p ? leg(*p, T80()) : nullptr;
  CHECK(p && p->legs.size() == 3 && tail && tail->side == Side::SELL && tail->planned == px(.01));
  CHECK(p && tail && tail->cost == usd(9.90) && p->payout_min == usd(20) && p->edge >= big().thr * p->qty);
  // without it the pair does not lock 1c a contract
  n.set(T80(), {}, {L(.04, 10)});
  CHECK(n.find(big()).empty());
}

void walk() {
  // a BUY walks the asks two cent ticks up, .30 to .32: the .31 level counts, the .33 never does
  Scene s = ladder();
  s.set(T79(), {L(.20, 10)}, {L(.30, 2), L(.31, 3), L(.33, 50)});
  s.set(B79(), {L(.20, 10)}, {L(.25, 20)});
  s.set(T80(), {L(.20, 10)}, {L(.30, 20)});
  auto ps = s.find(big());
  const Plan* p = of(ps, Kind::YES_COVER);
  CHECK(p && p->qty == ctr(5) && p->legs[0].ticker == T79());
  const Leg* a = p ? leg(*p, T79()) : nullptr;
  CHECK(a && a->planned == px(.30) && a->limit == px(.31) && a->shown == ctr(5) && a->cost == usd(2 * .30 + 3 * .31));
  CHECK(p && p->gross == usd(5 * (.31 + .25 + .30)));   // each leg's reservation at its limit
  // on the tapered grid a tick is 0.1c: .050 to .052
  s.listing[T79()].price_ranges = TAPERED;
  s.set(T79(), {L(.04, 10)}, {L(.050, 2), L(.051, 3), L(.053, 50)});
  s.set(B79(), {L(.20, 10)}, {L(.40, 20)});
  s.set(T80(), {L(.20, 10)}, {L(.45, 20)});
  ps = s.find(big());
  p = of(ps, Kind::YES_COVER);
  a = p ? leg(*p, T79()) : nullptr;
  CHECK(p && p->qty == ctr(5) && a && a->limit == px(.051) && a->shown == ctr(5));
  // a 1-contract fill at 0.1c prices is not whole cents: its unit stays in the bound
  CHECK(a && a->fee == order_fee_bound(1, {L(.050, 2), L(.051, 3)}) && a->fee == kalshi_model_fee(1, 500, ctr(2)) + kalshi_model_fee(1, 510, ctr(3)) + 20000);
  // a SELL walks the bids down: .45 to .43
  Scene n = flagged();
  n.set(WHO + "-A", {L(.45, 2), L(.44, 3), L(.42, 50)}, {L(.48, 40)});
  n.set(WHO + "-B", {L(.42, 25)}, {L(.45, 25)});
  n.set(WHO + "-C", {L(.20, 60)}, {L(.23, 60)});
  ps = n.find(big());
  p = of(ps, Kind::NO_SET);
  a = p ? leg(*p, WHO + "-A") : nullptr;
  CHECK(p && p->qty == ctr(5) && a && a->planned == px(.45) && a->limit == px(.44) && a->cost == usd(2 * .55 + 3 * .56));
  // two ticks reach .43; max_ticks 1 stops at .44
  n.set(WHO + "-A", {L(.45, 4), L(.43, 3)}, {L(.48, 40)});
  CHECK(size_of(n.find(big())) == ctr(7));
  Limits one = big();
  one.max_ticks = 1;
  CHECK(size_of(n.find(one)) == ctr(4));

  // Systematic's own resting bid (M2's) at .44: a SELL's limit stays above it
  n.set(WHO + "-A", {L(.45, 4), L(.44, 3), L(.42, 50)}, {L(.48, 40)});
  CHECK(size_of(n.find(big())) == ctr(7));
  n.own_bid[WHO + "-A"] = px(.44);
  ps = n.find(big());
  p = of(ps, Kind::NO_SET);
  a = p ? leg(*p, WHO + "-A") : nullptr;
  CHECK(p && p->qty == ctr(4) && a && a->limit == px(.45));
  // ...and at the touch the leg is dropped: without A the bids do not pass $1
  n.own_bid[WHO + "-A"] = px(.45);
  CHECK(n.find(big()).empty());
  // a BUY's limit stays below Systematic's own resting ask
  s = ladder();
  s.set(T79(), {L(.20, 10)}, {L(.30, 2), L(.31, 3)});
  s.set(B79(), {L(.20, 10)}, {L(.25, 20)});
  s.set(T80(), {L(.20, 10)}, {L(.30, 20)});
  s.own_ask[T79()] = px(.31);
  ps = s.find(big());
  a = ps.size() == 1 ? leg(ps[0], T79()) : nullptr;
  CHECK(ps.size() == 1 && ps[0].qty == ctr(2) && a && a->limit == px(.30));
  s.own_ask[T79()] = px(.30);
  std::string why;
  CHECK(s.find(big(), &why).empty() && has(why, "YES_COVER: " + T79() + ": Systematic's own ask at 0.3000 is at or through the touch"));
}

// a cover that locks: every filter on one of its markets takes it away, and says why
void filters() {
  const auto cover = [](const std::string& e) {
    Scene s = ladder(e);
    s.set(T79(e), {L(.25, 10)}, {L(.28, 10)});
    s.set(B79(e), {L(.26, 10)}, {L(.31, 10)});
    s.set(T80(e), {L(.28, 10)}, {L(.33, 10)});
    return s;
  };
  std::string why;
  CHECK(cover(DAY).find(big()).size() == 1);
  CHECK(cover("KXHIGHNY-26OCT23").find(big()).size() == 1);   // tomorrow
  // the day after tomorrow
  CHECK(cover("KXHIGHNY-26OCT24").find(big(), &why).empty() &&
        has(why, "YES_COVER: KXHIGHNY-26OCT24-T79 settles 2026-10-24, not today or tomorrow"));
  // yesterday's (a late close)
  CHECK(cover("KXHIGHNY-26OCT21").find(big()).empty());
  // the close lead: 4 minutes out is too late, 6 is not
  Scene s = cover(DAY);
  s.listing[B79()].close_ms = NOW + 240000;
  CHECK(s.find(big(), &why).empty() && has(why, B79() + " closes within 300 s"));
  s.listing[B79()].close_ms = NOW + 360000;
  CHECK(s.find(big()).size() == 1);
  s.listing[B79()].close_ms.reset();
  CHECK(s.find(big()).empty());
  s = cover(DAY);
  s.listing[B79()].open_ms = NOW + 1000;
  CHECK(s.find(big(), &why).empty() && has(why, B79() + " has not opened"));
  s = cover(DAY);
  s.listing[B79()].status = "closed";
  CHECK(s.find(big(), &why).empty() && has(why, B79() + " is closed"));
  // fee metadata: missing, stale, or of an unknown type
  s = cover(DAY);
  s.listing[T80()].fee_ms = 0;
  CHECK(s.find(big(), &why).empty() && has(why, T80() + "'s fee metadata is missing or stale"));
  s.listing[T80()].fee_ms = NOW - 300000;
  CHECK(s.find(big()).empty());
  s.listing[T80()].fee_ms = NOW - 1000;
  s.listing[T80()].fee_type = "flat";
  CHECK(s.find(big()).empty());
  s = cover(DAY);
  s.listing[T80()].exchange_index.reset();
  CHECK(s.find(big(), &why).empty() && has(why, T80() + " has no exchange index"));
  s = cover(DAY);
  s.listing[T80()].price_ranges.clear();
  CHECK(s.find(big(), &why).empty() && has(why, T80() + " has no price grid"));
  // the book: none yet, stale, crossed
  s = cover(DAY);
  s.books.erase(T79());
  CHECK(s.find(big(), &why).empty() && has(why, T79() + " has no book yet"));
  s = cover(DAY);
  s.books[T79()].stale = true;
  CHECK(s.find(big(), &why).empty() && has(why, T79() + "'s book is stale"));
  s = cover(DAY);
  s.set(T79(), {L(.29, 10)}, {L(.28, 10)});
  CHECK(s.find(big(), &why).empty() && has(why, T79() + "'s book is crossed"));
  // a market of the event not given: no cover; one not in the event: ignored
  s = cover(DAY);
  s.tk.erase(s.tk.begin());
  CHECK(s.find(big(), &why).empty() && has(why, "YES_COVER: " + T79() + " is not among the markets scanned"));
  s = cover(DAY);
  s.listing["KXHIGHCHI-26OCT22-T60"] = mk("KXHIGHCHI-26OCT22-T60", "greater", 60, std::nullopt);
  s.tk.push_back("KXHIGHCHI-26OCT22-T60");
  CHECK(s.find(big()).size() == 1);

  // Manual anywhere in the event keeps the whole event out, a market no leg needs too
  s = cover(DAY);
  auto ms = s.markets();
  ms[1].manual = "Manual took " + B79() + " over";
  CHECK(find_baskets(s.shape, ms, NOW, big(), &why).empty() && why == "Manual took " + B79() + " over");
  Scene n = flagged();
  n.row.markets.push_back(mk(WHO + "-D", "custom", std::nullopt, std::nullopt));
  n.listing[WHO + "-D"] = n.row.markets.back();
  n.tk.push_back(WHO + "-D");
  n.shape = *event_shape(n.row, n.listing, 0, &why);
  n.set(WHO + "-A", {L(.45, 40)}, {L(.48, 40)});
  n.set(WHO + "-B", {L(.42, 25)}, {L(.45, 25)});
  n.set(WHO + "-C", {L(.20, 60)}, {L(.23, 60)});
  n.set(WHO + "-D", {}, {});   // no bid: never a leg
  CHECK(n.find(big()).size() == 1);
  ms = n.markets();
  ms[3].manual = "the account holds 3 in " + WHO + "-D, Systematic's lanes 0";
  CHECK(find_baskets(n.shape, ms, NOW, big(), &why).empty() && has(why, "the account holds 3"));
}

// arb_market: what strat's view and the account read say about one market
void market_view() {
  const kalshi::MarketRow r = mk(B79(), "between", 79, 80);
  const std::string tk = r.ticker;
  Gateway::SysView v;
  auto order = [&](const std::string& cid, const std::string& lane, Side side, Px price, Qty left, OrdState st, bool terminal = false) {
    Gateway::SysOrder o;
    o.cid = cid;
    o.lane = lane;
    o.ticker = tk;
    o.side = side;
    o.price = price;
    o.count = o.left = left;
    o.state = st;
    o.terminal = terminal;
    v.orders[cid] = o;
  };
  order("SM2-a-1", "M2", Side::BUY, px(.44), ctr(5), OrdState::WORK);
  order("SM2-a-2", "M2", Side::SELL, px(.48), ctr(3), OrdState::WORK);
  order("SS1-a-3", "S1", Side::BUY, px(.45), ctr(2), OrdState::PEND);   // not acknowledged: may rest any moment
  order("SM2-a-4", "M2", Side::SELL, px(.46), ctr(2), OrdState::CXL, true);
  v.positions["M2|" + tk] = ctr(3);
  v.positions["S1|" + tk] = ctr(-1);
  v.positions["M2|" + T80()] = ctr(9);
  rest::Snapshot s;
  s.account_ready = true;
  kalshi::PositionRow pr;
  pr.ticker = tk;
  pr.position = ctr(2);
  s.positions.push_back(pr);
  kalshi::OrderRow sys;
  sys.order_id = "o-1";
  sys.client_order_id = "SM2-a-1";
  sys.ticker = tk;
  sys.remaining = ctr(5);
  s.orders.push_back(sys);
  ArbMarket a = arb_market(r, book(tk, {L(.45, 10), L(.44, 8)}, {L(.48, 3), L(.49, 1)}), v, s);
  CHECK(a.row == &r && a.manual.empty());
  CHECK(a.own_bid == px(.45) && a.own_ask == px(.48));
  CHECK(a.sys_position == ctr(2) && a.sys_buying == ctr(7) && a.sys_selling == ctr(3));
  CHECK(a.book && (a.book->bids == std::vector<MdLevel>{L(.45, 10), L(.44, 3)}) && (a.book->asks == std::vector<MdLevel>{L(.49, 1)}));
  CHECK(!arb_market(r, std::nullopt, v, s).book);
  // Manual took it over
  auto held = v;
  held.held.insert(tk);
  CHECK(arb_market(r, std::nullopt, held, s).manual == "Manual took " + tk + " over");
  // an order not Systematic's rests there: Manual's, or another app's (no client id)
  auto man = s;
  kalshi::OrderRow mo = sys;
  mo.order_id = "o-2";
  mo.client_order_id = "M-ticket-7";
  man.orders.push_back(mo);
  CHECK(arb_market(r, std::nullopt, v, man).manual == "an order not Systematic's rests in " + tk + " (M-ticket-7)");
  man.orders.back().client_order_id.clear();
  CHECK(has(arb_market(r, std::nullopt, v, man).manual, "(o-2)"));
  man.orders.back().status = kalshi::OrderStatus::CANCELED;   // ended: no matter
  CHECK(arb_market(r, std::nullopt, v, man).manual.empty());
  // a position Systematic's lanes do not hold (Manual's, or the read behind Systematic's fills)
  auto pos = s;
  pos.positions[0].position = ctr(5);
  CHECK(arb_market(r, std::nullopt, v, pos).manual == "the account holds 5 in " + tk + ", Systematic's lanes 2");
  pos.positions.clear();
  CHECK(has(arb_market(r, std::nullopt, v, pos).manual, "the account holds 0"));
  // no account read: unknown, so out
  auto none = s;
  none.account_ready = false;
  CHECK(has(arb_market(r, std::nullopt, v, none).manual, "no account read yet"));
}

void sizing() {
  Scene s = ladder();
  s.set(T79(), {L(.25, 10)}, {L(.28, 20)});
  s.set(B79(), {L(.26, 10)}, {L(.31, 20)});
  s.set(T80(), {L(.28, 10)}, {L(.33, 20)});
  CHECK(size_of(s.find(big())) == ctr(10));   // max_qty 10
  Limits lim = big();
  lim.max_qty = ctr(3);
  CHECK(size_of(s.find(lim)) == ctr(3));
  lim.max_qty = 250;   // whole contracts
  CHECK(size_of(s.find(lim)) == ctr(2));
  lim.max_qty = 50;
  std::string why;
  CHECK(s.find(lim, &why).empty() && has(why, "YES_COVER: max_qty 0.5: not one contract"));
  // the levels: 2.5 contracts shown is 2 to take
  s.set(B79(), {L(.26, 10)}, {MdLevel{px(.31), 250}});
  CHECK(size_of(s.find(big())) == ctr(2));
  s.set(B79(), {L(.26, 10)}, {L(.31, 20)});
  // gw's inventory room: Systematic long 18 in T79 leaves 2 to buy; short 18, 38 (the other legs
  // keep their 20)
  s.sys_position[T79()] = ctr(18);
  CHECK(size_of(s.find(big())) == ctr(2));
  s.sys_position[T79()] = ctr(-18);
  lim = big();
  lim.max_qty = ctr(30);
  s.set(T79(), {L(.25, 10)}, {L(.28, 40)});
  s.set(B79(), {L(.26, 10)}, {L(.31, 40)});
  s.set(T80(), {L(.28, 10)}, {L(.33, 40)});
  CHECK(size_of(s.find(lim)) == ctr(20));
  s.sys_position[B79()] = s.sys_position[T80()] = ctr(-18);
  CHECK(size_of(s.find(lim)) == ctr(30));
  s.sys_position[T79()] = ctr(25);   // past the limit already: nothing more that way
  CHECK(s.find(lim, &why).empty() && has(why, T79() + "'s inventory room is 0"));
  s.sys_position.clear();
  // the budget: $0.92 a contract market by market, $0.64 by state
  lim = big();
  lim.budget_left = usd(4);
  CHECK(size_of(s.find(lim)) == ctr(4));
  lim.by_state = true;
  CHECK(size_of(s.find(lim)) == ctr(6));

  // a lock only mid-size: under 3 contracts the three rounding units ($0.03) eat it, past 5 the
  // dearer levels do. The search finds the far end of the run, 5, not the 3 at its peak.
  s.set(T79(), {L(.25, 10)}, {L(.30, 3), L(.31, 3), L(.32, 20)});
  s.set(B79(), {L(.26, 10)}, {L(.31, 3), L(.32, 3), L(.33, 20)});
  s.set(T80(), {L(.28, 10)}, {L(.33, 20)});
  lim = big();
  lim.thr = 0;
  const auto edge = [&](int n) {
    Limits at = lim;
    at.max_qty = ctr(n);
    const auto ps = s.find(at);
    return ps.empty() || ps[0].qty != ctr(n) ? std::optional<Micros>() : std::optional<Micros>(ps[0].edge);
  };
  CHECK(!edge(1) && !edge(2) && edge(3) == usd(.01455) && edge(5) == usd(.003186) && !edge(6) && !edge(7));
  CHECK(size_of(s.find(lim)) == ctr(5));
}

// The binary search against a scan of every size, on random cent ladders: a cover's lock is
// exactly $1 a contract less the walked asks and the fee bound
void sizing_random() {
  std::mt19937 rng(20261022);
  const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  int found = 0, rounds = 3000;
  for (int round = 0; round < rounds; ++round) {
    Scene s = ladder();
    std::vector<std::vector<MdLevel>> asks;
    for (const auto& t : s.tk) {
      std::vector<MdLevel> a;
      int c = pick(5, 45);
      for (int k = pick(1, 4); k > 0 && c < 100; --k, c += pick(1, 2)) a.push_back({c * 100, ctr(pick(1, 6))});
      asks.push_back(a);
      s.set(t, {}, a);
    }
    Limits lim;
    lim.thr = 100 * pick(0, 3);
    lim.max_qty = ctr(pick(1, 15));
    lim.budget_left = usd(pick(1, 12));
    lim.cash_left = usd(pick(1, 15));
    lim.by_state = pick(0, 1) == 1;
    lim.fee_precision = pick(0, 1) ? 100 : 10000;
    const auto ps = s.find(lim);
    const Plan* p = of(ps, Kind::YES_COVER);
    // the scan: each size walked by hand
    int best = 0;
    for (int n = 1; n * kOneContract <= lim.max_qty; ++n) {
      Micros cost = 0, fees = 0, gross = 0, least = 0;
      bool ok = true;
      for (std::size_t i = 0; i < asks.size(); ++i) {
        Qty left = ctr(n);
        Px limit = 0;
        std::vector<MdLevel> fills;
        for (const auto& l : asks[i]) {
          if (left <= 0 || l.px > asks[i][0].px + 200) break;   // two cent ticks
          const Qty t = std::min(left, l.qty);
          cost += l.px * t;
          fills.push_back({l.px, t});
          limit = l.px;
          left -= t;
        }
        if (left > 0) ok = false;
        fees += order_fee_bound(1, fills, lim.fee_precision);
        gross += limit * ctr(n);
        least = i == 0 ? limit * ctr(n) : std::min(least, limit * ctr(n));
      }
      if (!ok) break;
      const Micros counted = lim.by_state ? gross - least : gross;
      if (counted <= lim.budget_left && gross + fees <= lim.cash_left && usd(n) - cost - fees >= lim.thr * ctr(n)) best = n;
    }
    CHECK((p == nullptr) == (best == 0));
    if (p) {
      CHECK(p->qty == ctr(best));
      ++found;
    }
  }
  CHECK(found > rounds / 10);   // the random ladders lock often enough to test the search
}

}  // namespace

int main() {
  without_own();
  yes_cover();
  no_set();
  dominance();
  one_cent();
  walk();
  filters();
  market_view();
  sizing();
  sizing_random();
  return finish("deskd-arb");
}
