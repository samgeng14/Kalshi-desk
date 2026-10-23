// An event's settlement states and a book's losses in them (gw/event_risk.h):
//   - span_of on each strike type, and the ones it refuses
//   - event_shape: the KXHIGHLAX ladder (old tests/test_main.cpp:2400), a ladder exhaustive only
//     on its lattice, a gap, a cumulative ladder with no flag, the flag alone, and every refusal
//   - parity with the old risk.cpp (plan section 8, "Risk book: parity with old risk.cpp"): the
//     2026-09-05 KXHIGHLAX pair and the 2026-09-06 Marseille-Paris legs, in micro-dollars
//   - baskets and contingent legs: covers, NO sets, a dominance pair, all-working legs, a
//     reducing order
//   - risk_used: on unshaped books exactly SysBook::open_worst plus the reservations (random
//     books), never more than that on shaped ones, a shaped event at 0 at most, events adding
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

#include "check.h"
#include "deskd/gw/event_risk.h"
#include "deskd/gw/sys_book.h"

using namespace deskd;

namespace {
Micros usd(double d) { return std::llround(d * 1000000); }
Qty ctr(int n) { return n * kOneContract; }

kalshi::MarketRow mk(const std::string& tk, const std::string& type, std::optional<double> floor,
                     std::optional<double> cap) {
  kalshi::MarketRow m;
  m.ticker = tk;
  m.event_ticker = tk.substr(0, tk.rfind('-'));
  m.status = "active";
  m.strike_type = type;
  m.floor_strike = floor;
  m.cap_strike = cap;
  return m;
}

struct Ev {
  kalshi::EventRow row;
  std::map<std::string, kalshi::MarketRow> listing;
};
Ev ev(const std::string& event, std::optional<bool> flag, const std::vector<kalshi::MarketRow>& ms) {
  Ev e;
  e.row.event_ticker = event;
  e.row.series_ticker = event.substr(0, event.find('-'));
  e.row.mutually_exclusive = flag;
  e.row.markets = ms;
  for (const auto& m : ms) e.listing[m.ticker] = m;
  return e;
}
std::optional<EventShape> shape(const Ev& e, double lattice = 0, std::string* why = nullptr) {
  std::string w;
  return event_shape(e.row, e.listing, lattice, why ? why : &w);
}
// a shape a case builds on: when there is none, a failure and an empty shape (which proves nothing)
EventShape need(const std::optional<EventShape>& s) {
  CHECK(s.has_value());
  return s ? *s : EventShape{};
}

// the 2026-09-05 LAX ladder: 78 or below, 79-80, 81 or above
const std::string LAX = "KXHIGHLAX-26SEP05";
const std::string T78 = LAX + "-T78", B79 = LAX + "-B79.5", T80 = LAX + "-T80";
Ev lax(std::optional<bool> flag = true) {
  // the venue's order is not the strikes'
  return ev(LAX, flag, {mk(T80, "greater", 80, std::nullopt), mk(T78, "less", std::nullopt, 79), mk(B79, "between", 79, 80)});
}
// six brackets of two degrees with gaps on the real line between them, as KXHIGH lists them
const std::string NY = "KXHIGHNY-26OCT22";
const std::vector<std::string> NYB{NY + "-T79", NY + "-B79.5", NY + "-B81.5", NY + "-B83.5", NY + "-B85.5", NY + "-T86"};
Ev six() {
  return ev(NY, true, {mk(NYB[0], "less", std::nullopt, 79), mk(NYB[1], "between", 79, 80), mk(NYB[2], "between", 81, 82),
                       mk(NYB[3], "between", 83, 84), mk(NYB[4], "between", 85, 86), mk(NYB[5], "greater", 86, std::nullopt)});
}

void spans() {
  Span s = span_of(mk("X-A", "greater", 80, std::nullopt));
  CHECK(s.ok && s.lo == 80 && s.lo_open && s.hi_inf && !s.lo_inf);
  CHECK(!s.holds(80) && s.holds(80.01) && s.holds(1e9) && !s.holds(79));
  s = span_of(mk("X-A", "greater_or_equal", 80, std::nullopt));
  CHECK(s.ok && !s.lo_open && s.holds(80) && !s.holds(79.99));
  s = span_of(mk("X-A", "less", std::nullopt, 79));
  CHECK(s.ok && s.hi == 79 && s.hi_open && s.lo_inf && !s.hi_inf);
  CHECK(!s.holds(79) && s.holds(78.99) && s.holds(-1e9));
  s = span_of(mk("X-A", "less_or_equal", std::nullopt, 79));
  CHECK(s.ok && !s.hi_open && s.holds(79) && !s.holds(79.01));
  s = span_of(mk("X-A", "between", 79, 80));
  CHECK(s.ok && s.lo == 79 && s.hi == 80 && !s.lo_open && !s.hi_open && !s.lo_inf && !s.hi_inf);
  CHECK(s.holds(79) && s.holds(79.5) && s.holds(80) && !s.holds(78.99) && !s.holds(80.01));
  CHECK(span_of(mk("X-A", "between", 80, 80)).holds(80));   // one value
  // a zero or negative strike is a strike (degrees below zero, a spread), unlike a commodity's
  CHECK(span_of(mk("X-A", "less", std::nullopt, -5)).ok && span_of(mk("X-A", "greater", 0, std::nullopt)).ok);
  // refused: other types, a strike missing or not a number, a between upside down
  for (const char* t : {"functional", "custom", "structured", ""}) CHECK(!span_of(mk("X-A", t, 79, 80)).ok);
  CHECK(!span_of(mk("X-A", "greater", std::nullopt, 80)).ok && !span_of(mk("X-A", "less", 79, std::nullopt)).ok);
  CHECK(!span_of(mk("X-A", "between", 79, std::nullopt)).ok && !span_of(mk("X-A", "between", std::nullopt, 80)).ok);
  CHECK(!span_of(mk("X-A", "between", 80, 79)).ok);
  CHECK(!span_of(mk("X-A", "greater", std::nan(""), std::nullopt)).ok);
  CHECK(!span_of(mk("X-A", "less", std::nullopt, INFINITY)).ok);
  CHECK(!Span{}.holds(0));
}

void shapes() {
  using States = std::vector<std::vector<int>>;
  // KXHIGHLAX with the flag: three states, one market each, low strike first; exhaustive on the
  // real line itself: (-inf, 79), [79, 80], (80, inf)
  for (const double lattice : {0.0, 1.0}) {
    const auto s = shape(lax(), lattice);
    CHECK(s && s->exclusive && s->exhaustive && s->spans);
    CHECK(s && s->event == LAX && s->series == "KXHIGHLAX");
    CHECK(s && s->tickers == std::vector<std::string>({T78, B79, T80}));
    CHECK(s && s->states == States({{0}, {1}, {2}}));
    CHECK(s && s->index(T80) == 2 && s->index(T78) == 0 && s->index("KXHIGHLAX-26SEP05-T99") == -1);
  }
  // no flag: the strikes prove the same states, but not the venue's exclusivity
  auto s = shape(lax(std::nullopt));
  CHECK(s && !s->exclusive && s->exhaustive && s->states == States({{0}, {1}, {2}}));
  s = shape(lax(false));
  CHECK(s && !s->exclusive && s->exhaustive);

  // six brackets: (80, 81) and the like pay nothing on the real line, and hold no whole degree
  s = shape(six());
  CHECK(s && s->exclusive && !s->exhaustive && s->tickers.size() == 6);
  CHECK(s && s->states == States({{0}, {1}, {}, {2}, {3}, {4}, {5}}));
  s = shape(six(), 1);
  CHECK(s && s->exclusive && s->exhaustive && s->states == States({{0}, {1}, {2}, {3}, {4}, {5}}));
  s = shape(six(), 2);   // even degrees only: still nothing between the brackets
  CHECK(s && s->exhaustive);
  s = shape(six(), 0.5);   // 80.5 settles nowhere
  CHECK(s && !s->exhaustive);
  for (const double bad : {-1.0, std::nan(""), static_cast<double>(INFINITY)}) {   // the real line
    s = shape(six(), bad);
    CHECK(s && !s->exhaustive);
  }

  // a gap ladder: 79 to 85 pays nothing
  const std::string G = "KXHIGHLAX-26SEP06";
  s = shape(ev(G, true, {mk(G + "-A", "less", std::nullopt, 79), mk(G + "-B", "greater", 85, std::nullopt)}), 1);
  CHECK(s && s->exclusive && !s->exhaustive && s->states == States({{0}, {}, {1}}));

  // a cumulative ladder with no flag: 79 or below pays none, 80-83 one, 84 or above both
  const std::string C = "KXHIGHCHI-26OCT22";
  const Ev cum = ev(C, std::nullopt, {mk(C + "-T83", "greater", 83, std::nullopt), mk(C + "-T79", "greater", 79, std::nullopt)});
  s = shape(cum, 1);
  CHECK(s && !s->exclusive && !s->exhaustive && s->spans);
  CHECK(s && s->tickers == std::vector<std::string>({C + "-T79", C + "-T83"}));
  CHECK(s && s->states == States({{}, {0}, {0, 1}}));
  // ... which the flag contradicts: no shape, and why
  std::string why;
  Ev bad = cum;
  bad.row.mutually_exclusive = true;
  CHECK(!shape(bad, 1, &why));
  CHECK(why == C + ": the venue says mutually exclusive, but " + C + "-T79 and " + C + "-T83 both pay YES above 83");
  // closed ends meet: 79-80 and 80-81 both pay at 80
  CHECK(!shape(ev(G, true, {mk(G + "-A", "between", 79, 80), mk(G + "-B", "between", 80, 81)}), 0, &why));
  CHECK(why.find("both pay YES at 80") != std::string::npos);
  // open ends meet at nothing: less 80 and greater_or_equal 80
  s = shape(ev(G, true, {mk(G + "-A", "less", std::nullopt, 80), mk(G + "-B", "greater_or_equal", 80, std::nullopt)}));
  CHECK(s && s->exhaustive && s->states == States({{0}, {1}}));

  // the flag alone (a game's outcomes): each market alone, or none of them, by ticker
  const std::string M = "KXLIGUE1GAME-26SEP06OLMPAR";
  s = shape(ev(M, true, {mk(M + "-TIE", "", std::nullopt, std::nullopt), mk(M + "-OLM", "", std::nullopt, std::nullopt),
                         mk(M + "-PAR", "", std::nullopt, std::nullopt)}));
  CHECK(s && s->exclusive && !s->exhaustive && !s->spans);
  CHECK(s && s->tickers == std::vector<std::string>({M + "-OLM", M + "-PAR", M + "-TIE"}));
  CHECK(s && s->states == States({{0}, {1}, {2}, {}}));
  // one market without strikes takes the strikes' proof away from the rest
  s = shape(ev(G, true, {mk(G + "-A", "less", std::nullopt, 79), mk(G + "-B", "custom", std::nullopt, std::nullopt)}));
  CHECK(s && !s->spans && s->tickers == std::vector<std::string>({G + "-A", G + "-B"}) && s->states.size() == 3);

  // refusals
  CHECK(!shape(ev(M, std::nullopt, {mk(M + "-OLM", "", std::nullopt, std::nullopt)}), 0, &why));
  CHECK(why == M + ": no mutually_exclusive flag, and a market without strikes");
  CHECK(!shape(ev(M, false, {mk(M + "-OLM", "", std::nullopt, std::nullopt)}), 0, &why));
  CHECK(why == M + ": not mutually exclusive, and a market without strikes");
  Ev missing = lax();
  missing.listing.erase(B79);
  CHECK(!shape(missing, 1, &why) && why == LAX + ": its market " + B79 + " is not listed");
  CHECK(!shape(ev(LAX, true, {}), 1, &why) && why == LAX + ": the venue lists no markets in it");
  CHECK(!event_shape(missing.row, missing.listing, 1, nullptr));   // why is optional
  // a market listed twice by the venue counts once
  Ev twice = lax();
  twice.row.markets.push_back(twice.row.markets.front());
  s = shape(twice);
  CHECK(s && s->tickers.size() == 3 && s->states.size() == 3);
}

// tests/test_main.cpp test_hedging, test_event_netting and test_nested_legs_are_not_a_hedge
void old_parity() {
  const auto s = need(shape(lax(), 1));
  const std::vector<Stake> held{{B79, true, ctr(18), usd(5.94)}};
  CHECK(state_losses(s, held) == std::vector<Micros>({usd(5.94), usd(5.94 - 18), usd(5.94)}));
  CHECK(worst_loss(s, held) == usd(5.94) && best_loss(s, held) == usd(-12.06));
  // the pair he put on: NO 81+ wins at 78 or below, but 78 and 81+ were equally the worst, so it
  // deepened 81+ by its whole cost, and 81+ settled
  const Stake no_t80{T80, false, ctr(13), usd(3.90)};
  StackCheck c = stack_check(s, held, no_t80);
  CHECK(c.stacks && c.before == usd(5.94) && c.after == usd(9.84) && c.added == usd(3.90) && c.protection == 0);
  const std::vector<Stake> pair{held[0], no_t80};
  CHECK(state_losses(s, pair) == std::vector<Micros>({usd(9.84 - 13), usd(9.84 - 18 - 13), usd(9.84)}));
  CHECK(worst_loss(s, pair) == usd(9.84));
  // more of the bracket already winning, and YES 81+ alone, pay in no worst state
  CHECK(stack_check(s, held, Stake{B79, true, ctr(5), usd(1.70)}).stacks);
  const Stake yes_t80{T80, true, ctr(10), usd(4.80)};
  CHECK(stack_check(s, held, yes_t80).stacks);
  // a first leg stacks nothing
  c = stack_check(s, {}, no_t80);
  CHECK(!c.stacks && c.before == 0 && c.after == usd(3.90));
  // once 81+ is the one worst state, YES 81+ is a real hedge
  c = stack_check(s, pair, yes_t80);
  CHECK(!c.stacks && c.before == usd(9.84) && c.after == usd(4.64) && c.protection == usd(10.00));
  // YES and NO 79-80 net to nothing; a working NO there releases nothing
  CHECK(worst_loss(s, {held[0], Stake{B79, false, ctr(18), usd(12.06)}}) == 0);
  CHECK(worst_loss(s, {held[0], Stake{B79, false, ctr(18), usd(12.06), true}}) == usd(5.94));
  // the gap state loses the premium
  const std::string G = "KXHIGHLAX-26SEP06";
  const auto gap = need(shape(ev(G, true, {mk(G + "-A", "less", std::nullopt, 79), mk(G + "-B", "greater", 85, std::nullopt)}), 1));
  CHECK(state_losses(gap, {Stake{G + "-A", true, ctr(10), usd(3.0)}}) == std::vector<Micros>({usd(-7.0), usd(3.0), usd(3.0)}));

  // Marseille-Paris, 2026-09-06: Paris YES, then Marseille NO, the same bet (the flag alone)
  const std::string M = "KXLIGUE1GAME-26SEP06OLMPAR";
  const auto m = need(shape(ev(M, true, {mk(M + "-OLM", "", std::nullopt, std::nullopt), mk(M + "-PAR", "", std::nullopt, std::nullopt),
                                         mk(M + "-TIE", "", std::nullopt, std::nullopt)})));
  const std::vector<Stake> paris{{M + "-PAR", true, ctr(7), usd(1.55)}};
  CHECK(worst_loss(m, paris) == usd(1.55));
  const Stake marseille_no{M + "-OLM", false, ctr(8), usd(6.34)};
  c = stack_check(m, paris, marseille_no);
  CHECK(c.stacks && c.before == usd(1.55) && c.after == usd(7.89) && c.added == usd(6.34) && c.protection == 0);
  // OLM, PAR, TIE, none: it pays on a draw, which is why the old guard let it through
  CHECK(state_losses(m, {paris[0], marseille_no}) ==
        std::vector<Micros>({usd(7.89), usd(7.89 - 15), usd(7.89 - 8), usd(7.89 - 8)}));
  CHECK(stack_check(m, paris, Stake{M + "-OLM", true, ctr(5), usd(1.10)}).stacks);
  CHECK(stack_check(m, paris, Stake{M + "-TIE", true, ctr(5), usd(1.10)}).stacks);
  c = stack_check(m, paris, Stake{M + "-PAR", false, ctr(7), usd(5.55)});
  CHECK(!c.stacks && c.after < c.before && c.protection > 0);
}

void baskets() {
  const auto s = need(shape(lax(), 1));
  const Qty q = ctr(10);
  // a complete YES cover (asks .30, .31, .33): every state pays q
  const std::vector<Stake> cover{{T78, true, q, 30 * q * 100}, {B79, true, q, 31 * q * 100}, {T80, true, q, 33 * q * 100}};
  CHECK(worst_loss(s, cover) == usd(9.40 - 10) && best_loss(s, cover) == usd(9.40 - 10));
  // a complete NO set (bids .45, .42, .20): every state pays (|S| - 1) q
  const std::vector<Stake> nos{{T78, false, q, 55 * q * 100}, {B79, false, q, 58 * q * 100}, {T80, false, q, 80 * q * 100}};
  CHECK(worst_loss(s, nos) == usd(19.30 - 20));
  // three NO legs of six: a state among them pays 2q, one outside 3q
  const auto x = need(shape(six(), 1));
  std::vector<Stake> part;
  Micros cost = 0;
  for (int i : {1, 2, 4}) {
    part.push_back({NYB[i], false, q, usd(6.0 + i)});
    cost += usd(6.0 + i);
  }
  CHECK(worst_loss(x, part) == cost - 2 * q * kOneDollar && best_loss(x, part) == cost - 3 * q * kOneDollar);
  // all of them working: the NO set at most its dearest leg, the cover all but its cheapest
  for (auto& k : part) k.contingent = true;
  CHECK(worst_loss(x, part) == usd(10.0) && best_loss(x, part) == 0);
  std::vector<Stake> working = cover;
  for (auto& k : working) k.contingent = true;
  CHECK(worst_loss(s, working) == usd(9.40 - 3.00));
  // its first leg filled, the rest working: all but the cheaper of those
  working[0].contingent = false;
  CHECK(worst_loss(s, working) == usd(9.40 - 3.10));
  // a reducing sale (reserves 0) changes nothing in any state
  const std::vector<Stake> held{{B79, true, ctr(18), usd(5.94)}};
  const std::vector<Stake> selling{held[0], order_stake(B79, Side::SELL, ctr(18), 0)};
  CHECK(state_losses(s, selling) == state_losses(s, held) && worst_loss(s, selling) == usd(5.94));

  // a dominance pair on a cumulative ladder: NO above 83 (bid .60) and YES above 79 (ask .52)
  // pay at least q in every state, 2q between them
  const std::string C = "KXHIGHCHI-26OCT22";
  const auto cum = need(shape(
      ev(C, std::nullopt, {mk(C + "-T79", "greater", 79, std::nullopt), mk(C + "-T83", "greater", 83, std::nullopt)}), 1));
  const std::vector<Stake> dom{{C + "-T83", false, q, 40 * q * 100}, {C + "-T79", true, q, 52 * q * 100}};
  CHECK(state_losses(cum, dom) == std::vector<Micros>({usd(-0.80), usd(-10.80), usd(-0.80)}));

  // a stake outside the shape never pays; a shape with no states proves nothing
  CHECK(state_losses(s, {Stake{NYB[0], false, q, usd(4.0)}}) == std::vector<Micros>(3, usd(4.0)));
  CHECK(worst_loss(EventShape{}, cover) == usd(9.40) && best_loss(EventShape{}, cover) == usd(9.40));

  // the risk book's stakes
  Stake k = position_stake(B79, ctr(18), usd(5.94));
  CHECK(k.yes && k.qty == ctr(18) && k.cost == usd(5.94) && !k.contingent);
  k = position_stake(T80, -ctr(13), usd(9.10));   // sold 13 YES at .70: NO at .30
  CHECK(!k.yes && k.qty == ctr(13) && k.cost == usd(3.90) && !k.contingent);
  k = order_stake(T80, Side::BUY, ctr(4), usd(1.92));
  CHECK(k.yes && k.qty == ctr(4) && k.cost == usd(1.92) && k.contingent);
  CHECK(!order_stake(T80, Side::SELL, ctr(4), 0).yes);
}

// risk_used against gw's measure today (Gateway::sys_used: each lane's SysBook::open_worst plus
// its working orders' reservations), on random books
void risk_used_parity() {
  const std::string A = LAX, B = NY;
  const auto la = need(shape(lax(), 1));
  const auto ny = need(shape(six(), 1));
  std::vector<std::pair<std::string, std::string>> markets;   // ticker, event
  for (const auto& t : {T78, B79, T80}) markets.push_back({t, A});
  for (const auto& t : NYB) markets.push_back({t, B});
  markets.push_back({"KXSOLO-26OCT22-X", "KXSOLO-26OCT22"});   // an event with no shape
  markets.push_back({"KXNONE", ""});                           // no event at all
  const auto none = [](const std::string&) -> const EventShape* { return nullptr; };
  int calls = 0;
  const auto shaped = [&](const std::string& e) -> const EventShape* {
    ++calls;
    return e == A ? &la : e == B ? &ny : nullptr;
  };
  const char* lanes[] = {"M1", "M2", "S1"};
  std::mt19937 rng(20261022);
  int trade = 0;
  for (int round = 0; round < 500; ++round) {
    SysBook b;
    const int fills = std::uniform_int_distribution<int>(0, 30)(rng);
    for (int i = 0; i < fills; ++i) {
      const auto& [tk, e] = markets[std::uniform_int_distribution<std::size_t>(0, markets.size() - 1)(rng)];
      const Side side = rng() % 2 ? Side::BUY : Side::SELL;
      const Qty n = std::uniform_int_distribution<Qty>(1, 3000)(rng);
      const Px px = std::uniform_int_distribution<Px>(1, kOneDollar - 1)(rng);
      b.fill(lanes[rng() % 3], "c" + std::to_string(i), tk, side, n, px, 0, 1, "t" + std::to_string(++trade));
    }
    std::vector<RiskStake> st;
    Micros want = 0;
    for (const char* l : lanes) {
      want += b.open_worst(l);
      if (const auto* d = b.day(l))
        for (const auto& [tk, inv] : d->inv) {
          std::string e;
          for (const auto& m : markets) if (m.first == tk) e = m.second;
          st.push_back(RiskStake{position_stake(tk, inv.pos, inv.cost), e});
        }
    }
    const int orders = std::uniform_int_distribution<int>(0, 8)(rng);
    for (int i = 0; i < orders; ++i) {
      const auto& [tk, e] = markets[std::uniform_int_distribution<std::size_t>(0, markets.size() - 1)(rng)];
      const Qty n = std::uniform_int_distribution<Qty>(1, 2000)(rng);
      const Micros r = rng() % 4 ? std::uniform_int_distribution<Micros>(0, n * kOneDollar)(rng) : 0;   // some reduce
      st.push_back(RiskStake{order_stake(tk, rng() % 2 ? Side::BUY : Side::SELL, n, r), e});
      want += r;
    }
    const Micros got = risk_used(st, none);
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  round %d: risk_used %lld, open_worst + reservations %lld\n", round,
                                  (long long)got, (long long)want);
    CHECK(risk_used(st, nullptr) == want);
    // by state it is never more, and each event's shape is asked for once
    calls = 0;
    const Micros by_state = risk_used(st, shaped);
    CHECK(by_state <= want && by_state >= 0);
    std::set<std::string> events;
    for (const auto& k : st) if (!k.event.empty()) events.insert(k.event);
    CHECK(calls == static_cast<int>(events.size()));
  }

  const Qty q = ctr(10);
  // a held YES cover counts 0, not its locked 60c, against anything else
  std::vector<RiskStake> st{{position_stake(T78, q, usd(3.00)), A}, {position_stake(B79, q, usd(3.10)), A},
                            {position_stake(T80, q, usd(3.30)), A}};
  CHECK(risk_used(st, shaped) == 0 && risk_used(st, none) == usd(9.40));
  st.push_back({position_stake("KXNONE", q, usd(2.00)), ""});
  CHECK(risk_used(st, shaped) == usd(2.00));
  // two shaped events add: LAX's YES 79-80 alone, and a working NO in New York's 81-82
  st = {{position_stake(B79, ctr(18), usd(5.94)), A}, {order_stake(NYB[2], Side::SELL, q, usd(7.00)), B}};
  CHECK(risk_used(st, shaped) == usd(5.94 + 7.00));
  // lanes net in one event (M2's YES 79-80 against S1's NO there); a market the shape does not
  // list, and a stake with no event, count alone
  st = {{position_stake(B79, ctr(18), usd(5.94)), A}, {position_stake(B79, -ctr(18), usd(5.94)), A}};
  CHECK(risk_used(st, shaped) == 0 && risk_used(st, none) == usd(18.00));
  st.push_back({position_stake("KXHIGHLAX-26SEP05-B99", q, usd(1.00)), A});
  st.push_back({position_stake(T78, q, usd(2.00)), ""});
  CHECK(risk_used(st, shaped) == usd(3.00));
}
}  // namespace

int main() {
  spans();
  shapes();
  old_parity();
  baskets();
  risk_used_parity();
  return finish("deskd-event_risk");
}
