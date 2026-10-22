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
// ArbEngine, the leg executor, with a fake Env:
//   - scan mode logs and sends nothing; off does nothing; a scan row a minute or on a new result
//   - the scarcest leg alone, the rest in the step its answer arrives, sized to its fill; complete
//     once the fills are booked; nothing filled or refused: done, and the event cools down
//   - a short leg completed a tick through within complete_ms (legging: that tick), else unwound
//   - a 201 that says filled while its fills come late: no unwind until they are booked, so the
//     hedge is never sold; each excess over the least held closed a tick through the bid, a pass
//     a second, until frozen for good; refusals, a one-sided or wide book
//   - EXCHANGE: nothing more in the event until the view shows the order ended; frozen past
//     uncertain_max_ms
//   - Stop, KILL (legs deleted unanswered), the lane off, the private feed down: frozen, then only
//     unwound; Manual taking a market over: frozen for good
//   - the legging pause from s1.json and from this run, sticky across a new engine until the lane
//     is turned off and on; the lane's day as a backstop
//   - a restart mid-basket and orphans (no s1.json, or one that cannot be read): unwound from S1's
//     positions once the start's account read is in
//   - the finder's limits: the lane's budget by market or by state, the group, the ceiling, the
//     shard's cash; the write budget
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "check.h"
#include "deskd/gw/event_risk.h"
#include "deskd/gw/fees.h"
#include "deskd/strat/arb.h"
#include "deskd/strat/maker.h"

using namespace deskd;
using namespace deskd::arb;
using json = nlohmann::json;

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

// ---- ArbEngine against a fake Env: gw a stub inbox whose answers the test gives; the view, the
// account read and the books in memory, changed by the test; the store a list of rows

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr std::int64_t T = NOW + 10000;   // an engine's first step is at NOW, with an account read then: by T it is 5 s old

struct Rig {
  Scene sc = ladder();   // under 79, 79-80, over 80, flagged exclusive: a cover at .28/.31/.33
  std::shared_ptr<Gateway::SysView> view = std::make_shared<Gateway::SysView>();
  std::shared_ptr<rest::Snapshot> snap = std::make_shared<rest::Snapshot>();
  std::vector<GwMsg> posted;
  std::vector<std::pair<std::string, json>> rows;
  std::set<std::string> wanted, wanted_events;
  std::vector<std::string> alerts;
  bool private_up = true;
  double room = 400;
  std::string state_file;
  int unsaved = 0;                                        // orders posted before s1.json had them
  Rig() {
    view->run = RunState::RUN;
    view->lanes_on = {"S1"};
    view->eff = usd(100);
    snap->markets = sc.listing;
    snap->events[DAY] = sc.row;
    snap->listing_ready = snap->account_ready = true;
    snap->account_began_ms = snap->account_ms = NOW;
    snap->balances[1] = 1000 * kOneDollar;
    // B79.5's 12 is the scarcest: it goes first. Spreads of 2c: an unwind may go into them
    sc.set(T79(), {L(.26, 20)}, {L(.28, 20)});
    sc.set(B79(), {L(.29, 12)}, {L(.31, 12)});
    sc.set(T80(), {L(.31, 20)}, {L(.33, 20)});
  }
  ArbEngine::Env env() {
    ArbEngine::Env e;
    e.view = [this] { return std::shared_ptr<const Gateway::SysView>(view); };
    e.snapshot = [this] { return std::shared_ptr<const rest::Snapshot>(snap); };
    e.book = [this](const std::string& tk) {
      const auto it = sc.books.find(tk);
      return it == sc.books.end() ? std::nullopt : std::optional<MdBook>(it->second);
    };
    e.post = [this](GwMsg m) {
      // s1.json holds an order before it reaches gw
      if (const auto* p = std::get_if<PlaceIntent>(&m); p && !state_file.empty())
        unsaved += read_file(state_file).find("\"" + p->cid + "\"") == std::string::npos;
      posted.push_back(std::move(m));
      return true;
    };
    e.want = [this](std::set<std::string> w) { wanted = std::move(w); };
    e.want_events = [this](std::set<std::string> w) { wanted_events = std::move(w); };
    e.private_up = [this] { return private_up; };
    e.write_room = [this](int) { return room; };
    e.lattice = [](const std::string& series) { return series.rfind("KXHIGH", 0) == 0 ? 1.0 : 0.0; };
    e.record = [this](const std::string& table, std::string row) {
      rows.emplace_back(table, json::parse(row));
      return true;
    };
    e.alert = [this](const std::string& lvl, const std::string& msg) { alerts.push_back(lvl + " " + msg); };
    e.state_file = state_file;
    return e;
  }
  static arb::Params params(const std::string& mode = "trade") {
    arb::Params p;
    p.mode = mode;
    return p;
  }
  std::vector<PlaceIntent*> places() {
    std::vector<PlaceIntent*> out;
    for (auto& m : posted)
      if (auto* p = std::get_if<PlaceIntent>(&m)) out.push_back(p);
    return out;
  }
  std::size_t n() { return places().size(); }
  PlaceIntent& place(std::size_t i) { return *places().at(i); }
  // the view shows order i ended with f filled (the venue's count)
  void ended(std::size_t i, int f) {
    const PlaceIntent& p = place(i);
    Gateway::SysOrder o;
    o.cid = p.cid;
    o.lane = "S1";
    o.ticker = p.ticker;
    o.side = p.side;
    o.price = p.price;
    o.count = p.count;
    o.filled = ctr(f);
    o.state = o.filled == p.count ? OrdState::FILL : OrdState::CXL;
    o.terminal = true;
    o.done_ms = T;
    view->orders[p.cid] = o;
  }
  // gw's answer to order i: the venue took it and its 201 says f filled. The view shows it ended,
  // with its fills booked (executions, S1's position) unless they are still to come.
  void answer(std::size_t i, int f, bool fills = true, std::optional<double> at = std::nullopt) {
    PlaceIntent& p = place(i);
    const json ord = {{"cid", p.cid}, {"filled", f}, {"st", ctr(f) == p.count ? "FILL" : "CXL"}};
    p.reply->set_value(OrderReply{"", "", ord.dump()});
    ended(i, f);
    if (fills) book(i, at);
  }
  // order i's fills reach gw's book, at the limit or at `at`
  void book(std::size_t i, std::optional<double> at = std::nullopt) {
    const PlaceIntent& p = place(i);
    auto& o = view->orders.at(p.cid);
    o.executed = o.filled;
    o.exec_cost = (at ? px(*at) : p.price) * o.filled;
    view->positions["S1|" + p.ticker] += (p.side == Side::BUY ? 1 : -1) * o.filled;
  }
  void refuse(std::size_t i, const std::string& code) { place(i).reply->set_value(OrderReply{code, "refused here", ""}); }
  Qty pos(const std::string& tk) const {
    const auto it = view->positions.find("S1|" + tk);
    return it == view->positions.end() ? 0 : it->second;
  }
  // the account read agrees with S1's positions (arb_market keeps an event out until it does)
  void agree() {
    snap->positions.clear();
    for (const auto& [key, q] : view->positions) {
      kalshi::PositionRow r;
      r.ticker = key.substr(key.find('|') + 1);
      r.position = q;
      snap->positions.push_back(r);
    }
  }
  std::vector<json> of(const std::string& table) const {
    std::vector<json> out;
    for (const auto& [t, r] : rows)
      if (t == table) out.push_back(r);
    return out;
  }
  json last(const std::string& table) const {
    const auto r = of(table);
    return r.empty() ? json() : r.back();
  }
  bool alerted(const std::string& part) const {
    return std::any_of(alerts.begin(), alerts.end(), [&part](const std::string& a) { return has(a, part); });
  }
};
bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
json read_json(const std::string& path) {
  std::ifstream in(path);
  return json::parse(in, nullptr, false);
}
json basket_status(const ArbEngine& e) {
  const auto b = e.status()["baskets"];
  return b.empty() ? json() : b[0];
}

// scan mode finds and logs and sends nothing; off does nothing
void engine_scan() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params("scan"));
  CHECK(e.step(NOW) == 250);
  CHECK((g.wanted == std::set<std::string>{T79(), B79(), T80()}) && (g.wanted_events == std::set<std::string>{DAY}));
  CHECK(e.step(T) == 250 && g.posted.empty());
  auto scans = g.of("arb_scans");
  CHECK(scans.size() == 1);   // the same result: no row again within the minute
  const json r = scans.back();
  CHECK(r["event"] == DAY && r["kind"] == "YES_COVER" && r["mode"] == "scan" && r["fired"] == false && r["why"] == "scan mode" &&
        r["qty"] == 10 && r["cost"] == "9.2000" && r["payout_min"] == "10.0000" && r["legs"].size() == 3 &&
        r["legs"][0]["tk"] == B79() && r["legs"][0]["side"] == "BUY" && r["legs"][0]["px"] == "0.3100");
  CHECK(e.status()["mode"] == "scan" && e.status()["events"][DAY]["kind"] == "YES_COVER");
  // a new result is a row at once; the same one again a minute on
  g.sc.set(T80(), {L(.31, 20)}, {L(.32, 20)});
  e.step(T + 300);
  CHECK(g.of("arb_scans").size() == 2 && g.last("arb_scans")["cost"] == "9.1000");
  e.step(T + 600);
  CHECK(g.of("arb_scans").size() == 2);
  e.step(T + 60300);
  CHECK(g.of("arb_scans").size() == 3 && g.posted.empty());
  // nothing locks: one row saying why
  g.sc.set(T80(), {L(.31, 20)}, {L(.45, 20)});
  e.step(T + 60600);
  CHECK(g.of("arb_scans").size() == 4 && g.last("arb_scans")["kind"].is_null() && has(g.last("arb_scans")["why"], "YES_COVER: locks"));

  Rig h;
  ArbEngine off("S1", {"KXHIGHNY"}, h.env(), Rig::params("off"));
  CHECK(off.step(NOW) == 250 && off.step(T) == 250 && h.posted.empty() && h.rows.empty() && h.wanted.empty());
  CHECK(off.status()["mode"] == "off");
}

// The scarcest leg alone, the rest in the step its answer arrives, each for its fill; complete
// once every leg's fills are booked; the event then cools down
void engine_cover() {
  TempDir dir;
  Rig g;
  g.state_file = dir.file("s1.json");
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  CHECK(e.step(NOW) == 250 && g.posted.empty());   // the account read is not 5 s old yet
  CHECK(has(g.last("arb_scans")["why"], "wait for an account read after the start"));
  CHECK(e.step(T) == 10 && g.n() == 1);
  if (g.n() != 1) return;
  const PlaceIntent& p = g.place(0);
  CHECK(p.cid.rfind("SS1-", 0) == 0 && ends_with(p.cid, "-b1l0") && p.cid.size() <= 40 && p.owner == Owner::SYS &&
        p.lane == "S1" && p.venue == Venue::K && p.ticker == B79() && p.side == Side::BUY && p.price == px(.31) &&
        p.count == ctr(10) && p.tif == Tif::IOC && p.ttl_s == 0 && p.slip_ticks == 0 && !p.reduce_only && !p.override_guard &&
        p.touch == px(.31) && p.t_recv_ms == T && p.reply);
  CHECK(g.last("arb_scans")["fired"] == true && has(g.last("arb_scans")["why"], "fired S1-"));
  // the basket was in s1.json before its leg went
  json f = read_json(g.state_file);
  CHECK(f["v"] == 1 && f["baskets"].size() == 1 && f["baskets"][0]["orders"][0]["cid"] == p.cid &&
        f["baskets"][0]["orders"][0]["is"] == "SENT" && f["baskets"][0]["st"] == "FIRST");
  CHECK(g.last("baskets")["state"] == "FIRST" && g.last("baskets")["kind"] == "YES_COVER");
  CHECK(e.step(T + 10) == 10 && g.n() == 1);   // its answer to come
  g.answer(0, 10);
  CHECK(e.step(T + 20) == 10 && g.n() == 3);
  for (std::size_t i : {1, 2}) {
    const PlaceIntent& q = g.place(i);
    CHECK(q.side == Side::BUY && q.count == ctr(10) && q.slip_ticks == 0 && ends_with(q.cid, "l" + std::to_string(i)));
  }
  CHECK(g.place(1).ticker == T79() && g.place(1).price == px(.28) && g.place(2).ticker == T80() && g.place(2).price == px(.33));
  CHECK(g.last("baskets")["state"] == "REST");
  // the 201s say filled; the fills are still to come: not done yet
  g.answer(1, 10, false);
  g.answer(2, 10, false);
  CHECK(e.step(T + 30) == 10 && basket_status(e)["state"] == "REST");
  g.book(1);
  g.book(2);
  CHECK(e.step(T + 40) == 250 && g.n() == 3);
  const json d = g.last("baskets");
  CHECK(d["state"] == "DONE" && d["why"] == "complete: every leg holds 10" && d["legging"] == "0.0000" &&
        d["edge_real"] == "0.8000" && d["t_end_ms"] == T + 40 && d["legs"][2]["filled"] == 10);
  CHECK(read_json(g.state_file)["baskets"].empty() && read_json(g.state_file)["legging"] == 0);
  CHECK(e.status()["done"] == 1 && e.status()["baskets"].empty() && e.status()["legging"] == "0.0000");
  // the event cools down, then the next basket fires; a fresh account read holds nothing up
  g.agree();
  g.snap->account_began_ms = g.snap->account_ms = T + 30000;
  e.step(T + 300);
  CHECK(g.n() == 3 && has(g.last("arb_scans")["why"], "the event cools down"));
  CHECK(e.step(T + 40 + 30000) == 10 && g.n() == 4 && ends_with(g.place(3).cid, "-b2l0"));
  CHECK(g.unsaved == 0);
}

// The start's account read counts once it is 5 s old, while reads keep coming every 700 ms (a
// fast poll, or a private event's refresh): the newest one never is
void engine_polls() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  std::int64_t t = NOW;
  for (; t <= NOW + 5600; t += 700) {   // the read at NOW began before the first step; the next at NOW + 650
    g.snap->account_began_ms = t - 100;
    g.snap->account_ms = t - 50;
    e.step(t);
  }
  CHECK(g.posted.empty() && has(g.last("arb_scans")["why"], "wait for an account read after the start"));
  g.snap->account_began_ms = t - 100;
  g.snap->account_ms = t - 50;
  CHECK(e.step(t) == 10 && g.n() == 1);   // NOW + 6300: the read of NOW + 650 is 5 s old
}

// The first leg fills part: the rest go for as many. Nothing, or a refusal: done, and a cool-down.
void engine_first() {
  {
    Rig g;
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    e.step(T);
    g.answer(0, 6);
    e.step(T + 10);
    CHECK(g.n() == 3 && g.place(1).count == ctr(6) && g.place(2).count == ctr(6));
  }
  {
    Rig g;
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    e.step(T);
    g.answer(0, 0);
    CHECK(e.step(T + 10) == 250 && g.n() == 1 && g.last("baskets")["state"] == "DONE" &&
          g.last("baskets")["why"] == "the first leg filled nothing");
    e.step(T + 300);
    CHECK(g.n() == 1 && has(g.last("arb_scans")["why"], "the event cools down"));
  }
  {
    Rig g;
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    e.step(T);
    g.refuse(0, "SYS_CEILING");
    CHECK(e.step(T + 10) == 250 && g.n() == 1 && g.last("baskets")["why"] == "the first leg was refused: SYS_CEILING: refused here");
  }
}

// A leg short by 4 while its touch is a tick away: one completion for 4 a tick through the planned
// touch; it fills there, the basket is complete and legging is that tick on 4 contracts
void engine_completion() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.answer(0, 10);
  e.step(T + 10);
  g.answer(1, 10);
  g.answer(2, 6);
  g.sc.set(T80(), {L(.31, 20)}, {L(.34, 20)});   // the ask moved up a tick
  CHECK(e.step(T + 20) == 10 && g.n() == 4);
  const PlaceIntent& c = g.place(3);
  CHECK(c.ticker == T80() && c.side == Side::BUY && c.price == px(.34) && c.count == ctr(4) && c.slip_ticks == 1 &&
        c.touch == px(.34) && ends_with(c.cid, "l2c") && !c.reduce_only);
  CHECK(g.last("baskets")["state"] == "COMPLETE");
  g.answer(3, 4);
  CHECK(e.step(T + 30) == 250 && g.last("baskets")["state"] == "DONE" && g.last("baskets")["why"] == "complete: every leg holds 10");
  CHECK(g.last("baskets")["legging"] == "-0.0400" && e.status()["legging"] == "-0.0400");
  CHECK(g.last("baskets")["edge_real"] == "0.7600");

  // two ticks away: no completion; the excess is unwound
  Rig h;
  ArbEngine f("S1", {"KXHIGHNY"}, h.env(), Rig::params());
  f.step(NOW);
  f.step(T);
  h.answer(0, 10);
  f.step(T + 10);
  h.answer(1, 10);
  h.answer(2, 6);
  h.sc.set(T80(), {L(.31, 20)}, {L(.35, 20)});
  f.step(T + 20);
  CHECK(h.n() == 5 && has(h.of("baskets")[h.of("baskets").size() - 2]["why"], "no completion; " + T80() + "'s touch is 0.3500"));
  CHECK(h.place(3).ticker == B79() && h.place(3).side == Side::SELL && h.place(3).count == ctr(4) && h.place(3).price == px(.28));
  CHECK(h.place(4).ticker == T79() && h.place(4).side == Side::SELL && h.place(4).count == ctr(4) && h.place(4).price == px(.25));
  // past complete_ms the same
  Rig k;
  ArbEngine late("S1", {"KXHIGHNY"}, k.env(), Rig::params());
  late.step(NOW);
  late.step(T);
  k.answer(0, 10);
  late.step(T + 10);
  k.answer(1, 10);
  k.answer(2, 6);
  late.step(T + 10 + 2001);
  CHECK(k.n() == 5 && k.place(3).side == Side::SELL && has(k.of("baskets")[k.of("baskets").size() - 2]["why"], "short past complete_ms"));
}

// A 201 that says filled while the fills come late: the unwind waits for them, so it never sells
// the hedge (sized from S1's positions before B79.5's fills it would sell all of T79.5 and T80);
// then each leg's excess over the least goes one tick through the bid, retried each second
void engine_unwind() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.answer(0, 10, false);   // B79.5: its 201 says 10, its fills come late
  e.step(T + 10);
  g.answer(1, 10);
  g.answer(2, 6);
  g.sc.set(T80(), {L(.31, 20)}, {L(.36, 20)});   // no completion
  CHECK(e.step(T + 20) == 10 && g.n() == 3 && basket_status(e)["state"] == "UNWIND");
  for (std::int64_t t = T + 1020; t < T + 6000; t += 1000) e.step(t);
  CHECK(g.n() == 3 && basket_status(e)["tries"] == 0);
  g.book(0);
  CHECK(e.step(T + 6000) == 10 && g.n() == 5);
  const PlaceIntent& a = g.place(3);
  const PlaceIntent& b = g.place(4);
  CHECK(a.ticker == B79() && a.side == Side::SELL && a.price == px(.28) && a.count == ctr(4) && a.touch == px(.29) &&
        a.slip_ticks == 1 && !a.reduce_only && ends_with(a.cid, "l0u1"));
  CHECK(b.ticker == T79() && b.side == Side::SELL && b.price == px(.25) && b.count == ctr(4) && ends_with(b.cid, "l1u1"));
  CHECK(g.last("baskets")["state"] == "UNWIND" && has(g.last("baskets")["why"], "pass 1: 2 leg(s) closing to 6"));
  // B79.5's unwind fills at the bid; T79's misses: a second pass a second on, for T79's 4 alone
  g.answer(3, 4, true, .29);
  g.answer(4, 0);
  e.step(T + 6500);
  CHECK(g.n() == 5);
  g.sc.set(T79(), {L(.25, 20)}, {L(.27, 20)});
  e.step(T + 7000);
  CHECK(g.n() == 6 && g.place(5).ticker == T79() && g.place(5).price == px(.24) && g.place(5).count == ctr(4) &&
        ends_with(g.place(5).cid, "l1u2"));
  g.answer(5, 4, true, .25);
  CHECK(e.step(T + 7010) == 250 && g.last("baskets")["state"] == "DONE" && g.last("baskets")["why"] == "unwound: every leg holds 6");
  // legging: B79.5 bought at .31, sold at .29; T79 bought at .28, sold at .25; 4 each
  CHECK(g.last("baskets")["legging"] == "-0.2000" && e.status()["legging"] == "-0.2000");
  CHECK(g.pos(B79()) == ctr(6) && g.pos(T79()) == ctr(6) && g.pos(T80()) == ctr(6));
}

// gw refuses a leg: a completion, refused too: the excess unwound. A one-sided or wide book is
// not unwound into; the passes run out and the basket freezes with what it holds.
void engine_refused() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.answer(0, 10);
  e.step(T + 10);
  g.refuse(1, "SYS_CEILING");
  g.answer(2, 10);
  e.step(T + 20);
  CHECK(g.n() == 4 && g.place(3).ticker == T79() && g.place(3).price == px(.29) && g.place(3).count == ctr(10));
  g.refuse(3, "SYS_CEILING");
  g.sc.set(T80(), {}, {L(.33, 20)});   // T80 one-sided
  e.step(T + 30);
  CHECK(g.n() == 5 && g.place(4).ticker == B79() && g.place(4).side == Side::SELL && g.place(4).count == ctr(10));
  CHECK(has(g.last("baskets")["why"], T80() + "'s book is one-sided"));
  g.answer(4, 10, true, .29);
  g.sc.set(T80(), {L(.20, 20)}, {L(.33, 20)});   // 13c wide
  for (int i = 1; i <= 9; ++i) e.step(T + 30 + 1000 * i);
  CHECK(g.n() == 5 && has(g.last("baskets")["why"], "is over 3 ticks and 3c") && basket_status(e)["tries"] == 10);
  e.step(T + 12000);
  CHECK(basket_status(e)["state"] == "FROZEN" && basket_status(e)["stuck"] == true && g.alerted("CRIT S1 basket") &&
        g.alerted("10 unwind passes spent, 10 " + T80() + " over the complete set held"));
  e.step(T + 20000);
  CHECK(g.n() == 5 && basket_status(e)["state"] == "FROZEN");
  // and the event is held: no new basket there
  g.agree();
  e.step(T + 40000);
  CHECK(g.n() == 5 && has(g.last("arb_scans")["why"], "is open in the event (FROZEN)"));
}

// gw answers EXCHANGE: nothing more goes in the event until the view shows the order ended
void engine_uncertain() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.answer(0, 10);
  e.step(T + 10);
  g.place(1).reply->set_value(OrderReply{"EXCHANGE", "the venue did not confirm this order in time", ""});
  g.answer(2, 6);
  CHECK(e.step(T + 20) == 10 && g.n() == 3 && basket_status(e)["state"] == "UNCERTAIN");
  e.step(T + 3000);
  CHECK(g.n() == 3 && basket_status(e)["state"] == "UNCERTAIN");
  // the private feed settles it: it filled 10
  g.ended(1, 10);
  g.book(1);
  e.step(T + 3010);
  // back in REST, past complete_ms: T80's 4 over... T80 is short, so T79 and B79.5 unwind 4
  CHECK(g.n() == 5 && g.place(3).ticker == B79() && g.place(4).ticker == T79() && g.place(3).count == ctr(4));
  // uncertain past uncertain_max_ms: frozen for good
  Rig h;
  arb::Params p = Rig::params();
  p.uncertain_max_ms = 5000;
  ArbEngine f("S1", {"KXHIGHNY"}, h.env(), p);
  f.step(NOW);
  f.step(T);
  h.place(0).reply->set_value(OrderReply{"EXCHANGE", "no confirmation", ""});
  f.step(T + 10);
  CHECK(basket_status(f)["state"] == "UNCERTAIN");
  f.step(T + 5020);
  CHECK(basket_status(f)["state"] == "FROZEN" && basket_status(f)["stuck"] == true && h.alerted("still uncertain after 5 s"));
}

// Stop mid-basket: frozen, a CRIT alert, nothing sent; once Systematic runs it is only unwound,
// never completed (decisions 10). A KILL deletes the queued legs: the same.
void engine_frozen() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.view->run = RunState::STOP;
  g.answer(0, 10);
  CHECK(e.step(T + 10) == 250 && g.n() == 1 && basket_status(e)["state"] == "FROZEN" && g.alerted("CRIT S1 basket S1-") &&
        g.alerted("is incomplete and frozen: Systematic is STOP; it is unwound once it may send again"));
  e.step(T + 2000);
  CHECK(g.n() == 1);
  g.view->run = RunState::RUN;
  CHECK(e.step(T + 3000) == 10 && g.n() == 2);
  CHECK(g.place(1).ticker == B79() && g.place(1).side == Side::SELL && g.place(1).count == ctr(10) && ends_with(g.place(1).cid, "l0u1"));
  g.answer(1, 10, true, .29);
  e.step(T + 3010);
  CHECK(g.last("baskets")["why"] == "unwound: every leg holds 0" && g.last("baskets")["legging"] == "-0.2000");

  // the lane off, the private feed down: the same freeze
  for (int which = 0; which < 2; ++which) {
    Rig h;
    ArbEngine f("S1", {"KXHIGHNY"}, h.env(), Rig::params());
    f.step(NOW);
    f.step(T);
    if (which == 0) h.view->lanes_on.clear();
    else h.private_up = false;
    h.answer(0, 10);
    f.step(T + 10);
    CHECK(h.n() == 1 && basket_status(f)["state"] == "FROZEN" && has(basket_status(f)["why"], which == 0 ? "lane S1 is off" : "private feed"));
  }

  // KILL: gw deletes the queued legs unanswered; after the re-arm and a Start, an unwind
  Rig k;
  ArbEngine x("S1", {"KXHIGHNY"}, k.env(), Rig::params());
  x.step(NOW);
  x.step(T);
  k.answer(0, 10);
  x.step(T + 10);
  CHECK(k.n() == 3);
  k.view->run = RunState::KILLED;
  k.posted.erase(k.posted.begin() + 1, k.posted.end());   // the two legs never left gw's inbox
  x.step(T + 20);
  CHECK(k.n() == 1 && basket_status(x)["state"] == "FROZEN" && has(basket_status(x)["why"], "Systematic is KILLED"));
  k.view->run = RunState::RUN;
  x.step(T + 60000);
  CHECK(k.n() == 2 && k.place(1).ticker == B79() && k.place(1).side == Side::SELL && k.place(1).count == ctr(10));
}

// Manual takes over a basket market: frozen for good, nothing unwound, the event left alone
void engine_takeover() {
  Rig g;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  e.step(T);
  g.answer(0, 10);
  e.step(T + 10);
  g.answer(1, 10);
  g.answer(2, 6);
  g.view->held.insert(T80());
  e.step(T + 20);
  CHECK(g.n() == 3 && basket_status(e)["state"] == "FROZEN" && basket_status(e)["stuck"] == true &&
        g.alerted("Manual took " + T80() + " over: the basket leaves the event and unwinds nothing"));
  g.view->held.clear();
  e.step(T + 5000);
  e.step(T + 10000);
  CHECK(g.n() == 3 && basket_status(e)["state"] == "FROZEN");
}

// The legging pause: from s1.json it holds new baskets, also in a new engine that sees the lane
// on; the lane turned off and on resets it, with an alert. A held basket still unwinds.
void engine_pause() {
  TempDir dir;
  const std::string file = dir.file("s1.json");
  {
    std::ofstream out(file);
    out << json{{"v", 1}, {"seq", 4}, {"legging", -6000000}, {"error", ""}, {"baskets", json::array()}}.dump();
  }
  Rig g;
  g.state_file = file;
  ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
  e.step(NOW);
  CHECK(g.alerted("CRIT S1 paused: legging -$6.0000"));
  e.step(T);
  CHECK(g.n() == 0 && has(g.last("arb_scans")["why"], "paused: legging -$6.0000, past -$5.0000; turn S1 off and on to reset"));
  CHECK(e.status()["paused"] == true && e.status()["legging"] == "-6.0000");
  {
    Rig h;
    h.state_file = file;
    ArbEngine again("S1", {"KXHIGHNY"}, h.env(), Rig::params());
    again.step(NOW);
    again.step(T);
    again.step(T + 300);
    CHECK(h.n() == 0 && again.status()["paused"] == true);
  }
  g.view->lanes_on.clear();
  e.step(T + 300);
  CHECK(g.n() == 0 && !g.alerted("reset"));
  g.view->lanes_on = {"S1"};
  e.step(T + 600);
  CHECK(g.alerted("WARN S1's pause (legging -$6.0000) reset by the lane turned off and on"));
  CHECK(g.n() == 1 && e.status()["paused"] == false && read_json(file)["legging"] == 0 && read_json(file)["seq"] == 5);

  // legging past the pause in this run: the alert, and no new basket while the unwind goes on
  Rig k;
  arb::Params p = Rig::params();
  p.legging_pause = usd(.40);
  ArbEngine x("S1", {"KXHIGHNY"}, k.env(), p);
  x.step(NOW);
  x.step(T);
  k.answer(0, 10);
  x.step(T + 10);
  k.answer(1, 10);
  k.answer(2, 0);
  k.sc.set(T80(), {L(.31, 20)}, {L(.36, 20)});
  x.step(T + 20);
  CHECK(k.n() == 5);
  k.answer(3, 10, true, .29);
  k.answer(4, 10, true, .26);
  x.step(T + 30);
  CHECK(x.status()["legging"] == "-0.4000" && x.status()["paused"] == true && k.alerted("CRIT S1 paused: legging -$0.4000"));
  k.sc.set(T80(), {L(.31, 20)}, {L(.33, 20)});
  k.agree();
  x.step(T + 40000);
  CHECK(k.n() == 5 && has(k.last("arb_scans")["why"], "paused: legging -$0.4000"));
  // the lane's day past the pause holds new baskets too (s1.json lost)
  Rig d;
  d.view->lane_day["S1"] = -usd(5);
  ArbEngine y("S1", {"KXHIGHNY"}, d.env(), Rig::params());
  y.step(NOW);
  y.step(T);
  CHECK(d.n() == 0 && has(d.last("arb_scans")["why"], "held: S1's day is -$5.0000"));
}

// A restart mid-basket: the open basket comes back from s1.json frozen, its legs never completed;
// once an account read after the start is 5 s old, the excess over the least held is unwound
void engine_restart() {
  TempDir dir;
  const std::string file = dir.file("s1.json");
  Rig g;
  g.state_file = file;
  {
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    e.step(T);
    g.answer(0, 10);
    e.step(T + 10);
    CHECK(g.n() == 3 && read_json(file)["baskets"][0]["st"] == "REST");
  }
  // deskd is gone; gw's rebuild from the venue's fills shows what the legs did
  Rig h;
  h.state_file = file;
  h.view->positions = {{"S1|" + B79(), ctr(10)}, {"S1|" + T79(), ctr(10)}, {"S1|" + T80(), ctr(4)}};
  h.agree();
  const std::int64_t T2 = T + 120000;
  h.snap->account_began_ms = h.snap->account_ms = T2;
  ArbEngine e("S1", {"KXHIGHNY"}, h.env(), Rig::params());
  // frozen, then unwinding once it may send: the positions wait for the start's account read
  CHECK(e.step(T2) == 10 && h.n() == 0 && basket_status(e)["state"] == "UNWIND" &&
        h.alerted("was open at the start (FROZEN): it is unwound once Systematic runs"));
  e.step(T2 + 3000);
  CHECK(h.n() == 0);
  CHECK(e.step(T2 + 5000) == 10 && h.n() == 2);
  CHECK(h.place(0).ticker == B79() && h.place(0).side == Side::SELL && h.place(0).count == ctr(6) && ends_with(h.place(0).cid, "l0u1"));
  CHECK(h.place(1).ticker == T79() && h.place(1).side == Side::SELL && h.place(1).count == ctr(6));
  h.answer(0, 6, true, .29);
  h.answer(1, 6, true, .26);
  e.step(T2 + 5010);
  CHECK(h.last("baskets")["why"] == "unwound: every leg holds 4" && read_json(file)["baskets"].empty());
  CHECK(g.unsaved == 0 && h.unsaved == 0);
}

// No s1.json and S1 holding positions: an orphan basket per event, unwound to equal holdings.
// An s1.json that cannot be read: the same, and new baskets pause until the lane is turned off and on.
void engine_orphans() {
  for (int unreadable = 0; unreadable < 2; ++unreadable) {
    TempDir dir;
    Rig g;
    if (unreadable) {
      g.state_file = dir.file("s1.json");
      std::ofstream(g.state_file) << "{not json";
    }
    g.view->positions = {{"S1|" + T79(), ctr(5)}, {"S1|" + B79(), ctr(5)}, {"S1|" + T80(), ctr(2)}, {"M2|" + T80(), ctr(9)}};
    g.view->inv = {{"S1|" + T79(), {ctr(5), usd(1.40)}}, {"S1|" + B79(), {ctr(5), usd(1.55)}}, {"S1|" + T80(), {ctr(2), usd(.66)}}};
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    CHECK(g.n() == 0 && e.status()["baskets"].empty());
    CHECK(unreadable == (g.alerted("cannot be read") ? 1 : 0));
    e.step(T);
    CHECK(g.alerted("WARN S1 " + DAY + ": S1 holds 5 " + B79() + ", 5 " + T79() + ", 2 " + T80() + " with no s1.json"));
    CHECK(g.n() == 2 && basket_status(e)["kind"] == "ORPHAN");
    if (g.n() != 2) continue;
    CHECK(g.place(0).ticker == B79() && g.place(0).side == Side::SELL && g.place(0).count == ctr(3) && g.place(0).price == px(.28));
    CHECK(g.place(1).ticker == T79() && g.place(1).side == Side::SELL && g.place(1).count == ctr(3) && g.place(1).price == px(.25));
    g.answer(0, 3, true, .29);
    g.answer(1, 3, true, .26);
    e.step(T + 10);
    // legging: 3 of B79.5's 5 (bought for $1.55) sold at .29, 3 of T79's ($1.40) at .26
    CHECK(g.last("baskets")["why"] == "unwound: every leg holds 2" && g.last("baskets")["legging"] == "-0.1200");
    CHECK(e.status()["paused"] == (unreadable == 1));
    if (unreadable) {
      CHECK(has(e.status()["pause"], "s1.json cannot be read") && read_json(g.state_file)["error"] != "");
      g.view->lanes_on.clear();
      e.step(T + 300);
      g.view->lanes_on = {"S1"};
      e.step(T + 600);
      CHECK(e.status()["paused"] == false && read_json(g.state_file)["error"] == "");
    }
  }
}

// What the finder is given: the lane's budget less what gw counts S1 using (market by market, or
// by state), the event's group, the shard's cash less its margins; and the write budget
void engine_limits() {
  // S1 holds a complete cover of 10 (bought at .28/.31/.33): by market it counts $9.20 of the $15
  const auto holding = [](Rig& g) {
    g.view->positions = {{"S1|" + T79(), ctr(10)}, {"S1|" + B79(), ctr(10)}, {"S1|" + T80(), ctr(10)}};
    g.view->inv = {{"S1|" + T79(), {ctr(10), usd(2.80)}}, {"S1|" + B79(), {ctr(10), usd(3.10)}}, {"S1|" + T80(), {ctr(10), usd(3.30)}}};
    g.view->worst = {{"S1|" + T79(), usd(2.80)}, {"S1|" + B79(), usd(3.10)}, {"S1|" + T80(), usd(3.30)}};
    g.agree();
  };
  {
    Rig g;
    holding(g);
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params("scan"));
    e.step(NOW);
    CHECK(g.last("arb_scans")["qty"] == 6);   // $5.80 left: 6 at $0.92
  }
  {
    Rig g;
    holding(g);
    arb::Params p = Rig::params("scan");
    p.limits.by_state = true;   // deskd.event_risk: the held cover counts about $0
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), p);
    e.step(NOW);
    CHECK(g.last("arb_scans")["qty"] == 10);
  }
  {
    Rig g;   // another lane's $14.50 in the event's group leaves $0.50
    g.view->worst = {{"M2|" + T80(), usd(14.50)}};
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params("scan"));
    e.step(NOW);
    CHECK(g.last("arb_scans")["kind"].is_null() && has(g.last("arb_scans")["why"], "a contract counts $0.9200, over the $0.5000 the budget leaves"));
  }
  {
    Rig g;   // the effective ceiling
    g.view->eff = usd(3);
    g.view->used = usd(1);
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params("scan"));
    e.step(NOW);
    CHECK(g.last("arb_scans")["qty"] == 2);
  }
  {
    Rig g;   // $5 on the shard, less 6 legs' fee allowance ($0.185 each): 4 contracts fit
    g.snap->balances[1] = 5 * kOneDollar;
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params("scan"));
    e.step(NOW);
    CHECK(g.last("arb_scans")["qty"] == 4);
    // a Systematic order working on the shard holds its collateral
    Gateway::SysOrder o;
    o.cid = "SM2-x-1";
    o.lane = "M2";
    o.ticker = T80();
    o.side = Side::BUY;
    o.price = px(.50);
    o.count = o.left = ctr(2);
    o.state = OrdState::WORK;
    g.view->orders[o.cid] = o;   // $1.00: 2 contracts fit
    e.step(NOW + 300);
    CHECK(g.last("arb_scans")["qty"] == 2);
  }
  {
    Rig g;   // the write budget: 3 legs need 30 tokens
    g.room = 25;
    ArbEngine e("S1", {"KXHIGHNY"}, g.env(), Rig::params());
    e.step(NOW);
    e.step(T);
    CHECK(g.n() == 0 && e.status()["events"][DAY]["why"] == "write budget: shard 1 has 25 tokens, 3 legs need 30");
  }
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
  engine_scan();
  engine_cover();
  engine_polls();
  engine_first();
  engine_completion();
  engine_unwind();
  engine_refused();
  engine_uncertain();
  engine_frozen();
  engine_takeover();
  engine_pause();
  engine_restart();
  engine_orphans();
  engine_limits();
  return finish("deskd-arb");
}
