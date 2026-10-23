#include "deskd/gw/event_risk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace deskd {

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

std::string num(double x) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", x);
  return buf;
}

// k is a whole number, to the rounding of a division
bool whole(double k) { return std::fabs(k - std::round(k)) <= 1e-9 * std::max(1.0, std::fabs(k)); }

// a multiple of step strictly between a and b
bool lattice_between(double a, double b, double step) {
  const double ka = a / step, kb = b / step;
  const double next = whole(ka) ? std::round(ka) + 1 : std::floor(ka) + 1;
  return next < kb - 1e-9 * std::max(1.0, std::fabs(kb));
}

struct Region {
  double x;            // a value inside it
  std::string where;   // for a why
};
// The regions the strikes cut the line into: below the lowest, each strike, between two
// neighbours, above the highest. A span holds all of a region or none of it, so one value tells.
// On a lattice (step > 0) only the regions holding a multiple of step.
std::vector<Region> regions(const std::vector<Span>& sp, double step) {
  std::vector<double> e;
  for (const auto& s : sp) {
    if (!s.lo_inf) e.push_back(s.lo);
    if (!s.hi_inf) e.push_back(s.hi);
  }
  std::sort(e.begin(), e.end());
  e.erase(std::unique(e.begin(), e.end()), e.end());
  if (e.empty()) return {{0, "anywhere"}};
  std::vector<Region> out{{e.front() - 1 - std::fabs(e.front()), "below " + num(e.front())}};
  for (std::size_t i = 0; i < e.size(); ++i) {
    if (step <= 0 || whole(e[i] / step)) out.push_back({e[i], "at " + num(e[i])});
    if (i + 1 < e.size() && (step <= 0 || lattice_between(e[i], e[i + 1], step)))
      out.push_back({e[i] + (e[i + 1] - e[i]) / 2, "between " + num(e[i]) + " and " + num(e[i + 1])});
  }
  out.push_back({e.back() + 1 + std::fabs(e.back()), "above " + num(e.back())});
  return out;
}

// a stake's worst case by itself, over its market's two outcomes (a working order's at least 0)
Micros alone(const Stake& k) {
  const Micros wins = k.cost - k.qty * kOneDollar, loses = k.cost;
  return k.contingent ? std::max<Micros>({0, wins, loses}) : std::max(wins, loses);
}

Micros alone(const std::vector<Stake>& st) {
  Micros w = 0;
  for (const auto& k : st) w += alone(k);
  return w;
}
}  // namespace

bool Span::holds(double x) const {
  if (!ok) return false;
  const bool above = lo_inf || x > lo || (!lo_open && x == lo);
  const bool below = hi_inf || x < hi || (!hi_open && x == hi);
  return above && below;
}

Span span_of(const kalshi::MarketRow& m) {
  const auto real = [](const std::optional<double>& v) { return v && std::isfinite(*v); };
  const auto& t = m.strike_type;
  Span s;
  if ((t == "greater" || t == "greater_or_equal") && real(m.floor_strike)) {
    s.lo = *m.floor_strike;
    s.lo_open = t == "greater";
    s.hi_inf = true;
    s.ok = true;
  } else if ((t == "less" || t == "less_or_equal") && real(m.cap_strike)) {
    s.hi = *m.cap_strike;
    s.hi_open = t == "less";
    s.lo_inf = true;
    s.ok = true;
  } else if (t == "between" && real(m.floor_strike) && real(m.cap_strike) && *m.floor_strike <= *m.cap_strike) {
    // Kalshi's "between 79-80" includes both ends (docs/edge_results.json: 79 <= H <= 80)
    s.lo = *m.floor_strike;
    s.hi = *m.cap_strike;
    s.lo_open = s.hi_open = false;
    s.ok = true;
  }
  return s;
}

int EventShape::index(const std::string& ticker) const {
  const auto it = std::find(tickers.begin(), tickers.end(), ticker);
  return it == tickers.end() ? -1 : static_cast<int>(it - tickers.begin());
}

std::optional<EventShape> event_shape(const kalshi::EventRow& e, const std::map<std::string, kalshi::MarketRow>& markets,
                                      double lattice_step, std::string* why) {
  const auto fail = [&](const std::string& w) -> std::optional<EventShape> {
    if (why) *why = e.event_ticker + ": " + w;
    return std::nullopt;
  };
  // low strike first: a span's low end (-inf first, a closed end before an open one at the same
  // strike), then its high end; markets without a span after, by ticker
  using Key = std::tuple<bool, double, bool, double, bool, std::string>;
  std::vector<std::pair<Key, Span>> ms;
  std::set<std::string> seen;
  for (const auto& r : e.markets) {
    if (!seen.insert(r.ticker).second) continue;
    const auto it = markets.find(r.ticker);
    if (it == markets.end()) return fail("its market " + r.ticker + " is not listed");
    const Span s = span_of(it->second);
    ms.push_back({{!s.ok, s.lo_inf ? -kInf : s.lo, s.lo_open, s.hi_inf ? kInf : s.hi, !s.hi_open, r.ticker}, s});
  }
  if (ms.empty()) return fail("the venue lists no markets in it");
  std::sort(ms.begin(), ms.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  EventShape s;
  s.event = e.event_ticker;
  s.series = e.series_ticker;
  std::vector<Span> sp;
  for (const auto& [key, span] : ms) {
    s.tickers.push_back(std::get<5>(key));
    sp.push_back(span);
  }
  s.spans = std::all_of(sp.begin(), sp.end(), [](const Span& x) { return x.ok; });
  const bool flag = e.mutually_exclusive.value_or(false);
  if (!s.spans && !flag)
    return fail(std::string(e.mutually_exclusive ? "not mutually exclusive" : "no mutually_exclusive flag") +
                ", and a market without strikes");
  s.exclusive = flag;
  if (!s.spans) {
    // the flag alone: one market pays YES, or none does (nothing proves the markets cover every outcome)
    for (int i = 0; i < static_cast<int>(s.tickers.size()); ++i) s.states.push_back({i});
    s.states.push_back({});
    return s;
  }
  const double step = std::isfinite(lattice_step) && lattice_step > 0 ? lattice_step : 0;
  for (const auto& r : regions(sp, step)) {
    std::vector<int> yes;
    for (int i = 0; i < static_cast<int>(sp.size()); ++i)
      if (sp[i].holds(r.x)) yes.push_back(i);
    if (flag && yes.size() > 1)
      return fail("the venue says mutually exclusive, but " + s.tickers[yes[0]] + " and " + s.tickers[yes[1]] +
                  " both pay YES " + r.where);
    if (std::find(s.states.begin(), s.states.end(), yes) == s.states.end()) s.states.push_back(std::move(yes));
  }
  s.exhaustive = std::none_of(s.states.begin(), s.states.end(), [](const std::vector<int>& x) { return x.empty(); });
  return s;
}

Stake position_stake(const std::string& ticker, Qty pos, Micros cost) {
  if (pos >= 0) return Stake{ticker, true, pos, cost, false};
  return Stake{ticker, false, -pos, kOneDollar * -pos - cost, false};
}

Stake order_stake(const std::string& ticker, Side side, Qty qty, Micros reservation) {
  return Stake{ticker, side == Side::BUY, qty, reservation, true};
}

std::vector<Micros> state_losses(const EventShape& s, const std::vector<Stake>& st) {
  std::vector<Micros> out(s.states.size(), 0);
  for (const auto& k : st) {
    const int i = s.index(k.ticker);
    for (std::size_t j = 0; j < s.states.size(); ++j) {
      const auto& yes = s.states[j];
      const bool settles_yes = std::find(yes.begin(), yes.end(), i) != yes.end();
      const bool wins = i >= 0 && k.yes == settles_yes;
      const Micros l = k.cost - (wins ? k.qty * kOneDollar : 0);
      out[j] += k.contingent ? std::max<Micros>(0, l) : l;
    }
  }
  return out;
}

Micros worst_loss(const EventShape& s, const std::vector<Stake>& st) {
  const auto l = state_losses(s, st);
  return l.empty() ? alone(st) : *std::max_element(l.begin(), l.end());
}

Micros best_loss(const EventShape& s, const std::vector<Stake>& st) {
  const auto l = state_losses(s, st);
  return l.empty() ? alone(st) : *std::min_element(l.begin(), l.end());
}

StackCheck stack_check(const EventShape& s, const std::vector<Stake>& existing, const Stake& add) {
  StackCheck c;
  c.added = add.cost;
  c.before = std::max<Micros>(0, worst_loss(s, existing));
  auto after = existing;
  after.push_back(add);
  c.after = std::max<Micros>(0, worst_loss(s, after));
  c.protection = c.added - (c.after - c.before);
  c.stacks = c.before > 0 && c.after - c.before >= c.added;
  return c;
}

Micros risk_used(const std::vector<RiskStake>& s, const std::function<const EventShape*(const std::string& event)>& shape_of) {
  Micros used = 0;
  std::map<std::string, const EventShape*> shapes;
  std::map<std::string, std::vector<Stake>> shaped;   // by event: the stakes its shape lists
  for (const auto& k : s) {
    const EventShape* sh = nullptr;
    if (!k.event.empty()) {
      auto it = shapes.find(k.event);
      if (it == shapes.end()) it = shapes.emplace(k.event, shape_of ? shape_of(k.event) : nullptr).first;
      sh = it->second;
    }
    if (sh && sh->index(k.ticker) >= 0)
      shaped[k.event].push_back(k);
    else
      used += alone(k);
  }
  for (const auto& [ev, st] : shaped) used += std::max<Micros>(0, worst_loss(*shapes.at(ev), st));
  return used;
}

}  // namespace deskd
