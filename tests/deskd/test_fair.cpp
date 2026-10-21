// The 15-minute financial fair (strat/fair.h): the strike check, the probability, and when a fair
// is shown at all.
#include <cmath>
#include <fstream>
#include "check.h"
#include "deskd/rest/state.h"
#include "deskd/strat/fair.h"
using namespace deskd;
namespace {
const std::int64_t T0 = 1791000000000;   // 2026-10-03, ms
const std::string NG = "Commodities.Index.NATGAS/USD";

// A lognormal walk sampled every second: sigma per sqrt(second), deterministic.
std::vector<std::pair<std::int64_t, double>> walk(double s0, double sigma, std::int64_t from, int seconds) {
  std::vector<std::pair<std::int64_t, double>> out;
  std::uint64_t x = 88172645463325252ULL;
  auto uni = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (static_cast<double>(x >> 11) + 0.5) / 9007199254740992.0; };
  double s = s0;
  for (int i = 0; i < seconds; ++i) {
    const double z = std::sqrt(-2 * std::log(uni())) * std::cos(2 * M_PI * uni());
    s *= std::exp(sigma * z - sigma * sigma / 2);
    out.emplace_back(from + i * 1000LL, std::round(s * 1e5) / 1e5);
  }
  return out;
}
RefView view_of(const std::vector<std::pair<std::int64_t, double>>& ticks, std::int64_t since, std::int64_t now) {
  RefView v; v.symbol = NG; v.fresh = true;
  for (const auto& t : ticks) if (t.first >= since && t.first <= now) v.ticks.push_back(t);
  if (!v.ticks.empty()) { v.value = v.ticks.back().second; v.source_ms = v.recv_ms = v.ticks.back().first; }
  return v;
}
kalshi::MarketRow window(const std::string& ticker, std::int64_t open, double strike) {
  kalshi::MarketRow m; m.ticker = ticker; m.status = "active";
  m.open_ms = open; m.close_ms = open + 900000;
  m.strike_type = "greater_or_equal"; m.floor_strike = strike;
  return m;
}

void strikes() {
  CHECK(FinancialFair::same_strike(3.08115, 3.08115, 5) && FinancialFair::same_strike(3.08116, 3.08115, 5));
  CHECK(!FinancialFair::same_strike(3.08118, 3.08115, 5));
  CHECK(FinancialFair::same_strike(1012.43, 1012.4, -1) && !FinancialFair::same_strike(1012.6, 1012.4, -1));
  CHECK(!FinancialFair::same_strike(0, 3.1, 5) && !FinancialFair::same_strike(NAN, 3.1, 5));
  CHECK(underlying_for("KXNATGAS15M-26OCT101200-00") && underlying_for("KXNATGAS15M-26OCT101200-00")->key == "natgas");
  CHECK(!underlying_for("KXNATGAS15MX-1") && !underlying_for("KXNATGAS15M") && !underlying_for("KXHIGHNY-26OCT10-B70"));
}

void probability() {
  const double sigma = 0.6 / std::sqrt(365.25 * 86400);   // 60% a year
  const auto ticks = walk(3.0, sigma, T0, 3600);
  const auto now = T0 + 3599000;
  const auto v = view_of(ticks, now - 3600000, now);
  std::string why;
  auto at_money = window("KXNATGAS15M-A", now - 300000, v.value);
  auto f = FinancialFair::price(at_money, v, now, VolSettings{}, &why);
  CHECK(f.has_value());
  if (!f) { std::fprintf(stderr, "no fair: %s\n", why.c_str()); return; }
  // at the strike with zero drift: one half, whatever the volatility (so its band is nil)
  CHECK(f->fair == 5000 && f->sd == 0);
  CHECK(f->sigma > sigma * 0.75 && f->sigma < sigma * 1.25);
  CHECK(std::fabs(f->tau_s - 600) < 1e-6);
  // two daily standard deviations' worth above: a long way out of the money in ten minutes
  auto far = window("KXNATGAS15M-B", now - 300000, v.value * 1.01);
  auto g = FinancialFair::price(far, v, now, VolSettings{}, &why);
  CHECK(g && g->fair < 100);
  auto near = window("KXNATGAS15M-C", now - 300000, v.value * 1.0005);
  auto h = FinancialFair::price(near, v, now, VolSettings{}, &why);
  CHECK(h && h->fair < f->fair && h->fair > g->fair);
  CHECK(h && h->sd > 0 && h->sd < 1000);   // off the strike the volatility band matters
  // not shown: stale feed, cold volatility, no payoff, closed
  auto stale = v; stale.fresh = false;
  CHECK(!FinancialFair::price(at_money, stale, now, VolSettings{}, &why) && why == "reference value not fresh");
  const auto cold = view_of(ticks, now - 120000, now);
  CHECK(!FinancialFair::price(at_money, cold, now, VolSettings{}, &why) && why.find("warm-up") != std::string::npos);
  auto odd = at_money; odd.strike_type = "custom";
  CHECK(!FinancialFair::price(odd, v, now, VolSettings{}, &why) && why.find("no terminal payoff") == 0);
  CHECK(!FinancialFair::price(at_money, v, *at_money.close_ms, VolSettings{}, &why) && why == "closed");
}

void replication_gate() {
  const double sigma = 0.6 / std::sqrt(365.25 * 86400);
  const auto ticks = walk(3.0, sigma, T0, 3600);
  auto close_at = [&](std::int64_t open) {   // the last value in the minute before the open
    double c = 0;
    for (const auto& t : ticks) if (t.first >= open - 60000 && t.first < open) c = t.second;
    return c;
  };
  std::int64_t now = T0 + 3599000;
  TempDir td;
  const std::string dir = td.path;
  const std::string file = dir + "/replication.json";
  FinancialFair fair([](const std::vector<std::string>& p) { return p.front() == NG ? NG : ""; },
                     [&](const std::string&, std::int64_t since, std::int64_t at) { return std::optional<RefView>(view_of(ticks, since, at)); },
                     T0);
  fair.persist(file);   // none yet
  rest::Snapshot s;
  const std::int64_t w0 = T0 + 30000, w1 = T0 + 2100000, w2 = T0 + 3000000;
  s.markets["KXNATGAS15M-26OCT100000-00"] = window("KXNATGAS15M-26OCT100000-00", w0, close_at(w0));   // before we watched
  s.markets["KXNATGAS15M-26OCT100035-35"] = window("KXNATGAS15M-26OCT100035-35", w1, close_at(w1));   // matches
  s.markets["KXNATGAS15M-26OCT100050-50"] = window("KXNATGAS15M-26OCT100050-50", w2, close_at(w2) + 0.001);   // does not
  s.markets["KXGOLD15M-26OCT100050-50"] = window("KXGOLD15M-26OCT100050-50", w2, 2650.5);   // no gold symbol seen
  s.markets["KXHIGHNY-26OCT10-B70"] = window("KXHIGHNY-26OCT10-B70", w2, 70);              // not a financial series
  s.markets["KXNATGAS15M-26OCT100035-35"].close_ms = now + 600000;   // keep the matched one open
  fair.step(s, now);
  const auto b = fair.board();
  CHECK(b->size() == 1 && b->count("KXNATGAS15M-26OCT100035-35"));
  const auto st = fair.status();
  CHECK(st["replication"]["natgas"]["checked"] == 2 && st["replication"]["natgas"]["matched"] == 1);
  CHECK(st["replication"]["natgas"]["consecutive"] == 0 && st["replication"]["natgas"]["gate"] == false);
  CHECK(st["markets"]["KXNATGAS15M-26OCT100050-50"]["why"] == "this window's strike was not reproduced by the feed");
  CHECK(st["markets"]["KXNATGAS15M-26OCT100000-00"]["fair"].is_null());
  CHECK(st["markets"]["KXGOLD15M-26OCT100050-50"]["why"].get<std::string>().find("no pyth_value symbol") == 0);
  CHECK(!st["markets"].contains("KXHIGHNY-26OCT10-B70"));
  CHECK(st["markets"]["KXNATGAS15M-26OCT100035-35"]["fair"].is_string());
  // the move to the close is there for every listed window with a fresh, warm feed, its strike
  // checked or not (the makers' 0.5 sigma pull); none for a closed one or one without a symbol
  const auto mv = fair.move_sd();
  CHECK(mv->count("KXNATGAS15M-26OCT100035-35") && mv->count("KXNATGAS15M-26OCT100050-50"));
  CHECK(!mv->count("KXNATGAS15M-26OCT100000-00") && !mv->count("KXGOLD15M-26OCT100050-50"));
  if (mv->count("KXNATGAS15M-26OCT100035-35")) {
    const auto& f = b->at("KXNATGAS15M-26OCT100035-35");
    CHECK(std::fabs(mv->at("KXNATGAS15M-26OCT100035-35") - f.sigma * std::sqrt(f.tau_s)) < 1e-12);
  }
  // the counts outlive a restart: a new fair on the same file starts from them
  {
    FinancialFair again([](const std::vector<std::string>&) { return std::string(); },
                        [](const std::string&, std::int64_t, std::int64_t) { return std::optional<RefView>(); }, now);
    again.persist(file);
    const auto r = again.status()["replication"]["natgas"];
    CHECK(r["checked"] == 2 && r["matched"] == 1 && r["consecutive"] == 0 && r["last_ok"] == false);
    CHECK(!again.status().contains("replication_file_error"));
  }
  {   // an unreadable file starts from none, and says so
    { std::ofstream(dir + "/bad.json") << "{\"v\":1,\"replication\":{\"natgas\":{\"checked\":\"x\"}}}"; }
    FinancialFair bad([](const std::vector<std::string>&) { return std::string(); },
                      [](const std::string&, std::int64_t, std::int64_t) { return std::optional<RefView>(); }, now);
    bad.persist(dir + "/bad.json");
    CHECK(bad.status()["replication"].empty() && bad.status()["replication_file_error"].is_string());
  }
  // a window is checked once: a second step counts nothing new
  fair.step(s, now + 1000);
  CHECK(fair.status()["replication"]["natgas"]["checked"] == 2);
  // a window that has left the listing is forgotten
  s.markets.erase("KXNATGAS15M-26OCT100050-50");
  fair.step(s, now + 2000);
  CHECK(!fair.status()["markets"].contains("KXNATGAS15M-26OCT100050-50"));
}
}  // namespace

int main() {
  strikes(); probability(); replication_gate();
  return finish("deskd-fair");
}
