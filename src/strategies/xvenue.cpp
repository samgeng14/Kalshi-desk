#include "desk/strategies.h"
#include "desk/log.h"

namespace desk {

void XVenue::prepare(const std::vector<Market>&, Store& store) {
  pairs_.clear();
  // Read before the rows, so a refresh that commits in between can only make the time older than
  // the book it is put on, never newer.
  const std::map<std::string, long long> polled = quote_ts_source_ ? quote_ts_source_() : std::map<std::string, long long>();
  std::map<std::string, std::pair<std::pair<double, double>, long long>> quotes;   // slug -> ((bid, ask), quote_ts)
  for (const auto& r : store.query("SELECT slug, bid, ask, quote_ts FROM poly_markets WHERE closed=0")) {
    const std::string slug = r.value("slug", "");
    long long ts = r.value("quote_ts", 0LL);
    // The poll time itself: it is never behind the stored quote_ts of a slug the map holds, and a
    // max would keep a pre-step time after a backward clock step (read as the future, so stale).
    if (auto p = polled.find(slug); p != polled.end()) ts = p->second;
    quotes[slug] = {{r.value("bid", 0.0), r.value("ask", 0.0)}, ts};
  }
  for (const auto& r : store.query("SELECT kalshi_ticker, slug, pm_yes_is_kalshi_yes FROM poly_pairs")) {
    auto q = quotes.find(r.value("slug", ""));
    if (q == quotes.end()) continue;
    Ref ref;
    ref.slug = r.value("slug", "");
    ref.flipped = r.value("pm_yes_is_kalshi_yes", 1LL) == 0;
    double bid = q->second.first.first, ask = q->second.first.second;
    if (ref.flipped) { ref.bid = ask > 0 ? 1 - ask : 0; ref.ask = bid > 0 ? 1 - bid : 0; }
    else { ref.bid = bid; ref.ask = ask; }
    ref.quote_ts = q->second.second;
    pairs_[r.value("kalshi_ticker", "")] = ref;
  }
}

std::optional<Fair> XVenue::fair(const Market& m, const Series&, Store&) {
  auto it = pairs_.find(m.ticker);
  if (it == pairs_.end()) return std::nullopt;
  const Ref& r = it->second;
  bool two_sided = r.bid > 0 && r.ask > 0 && r.ask > r.bid && r.ask < 1;
  long long age = r.quote_ts > 0 ? now_s() - r.quote_ts : -1;
  bool fresh = age >= 0 && age <= 90;
  Fair f;
  f.p = two_sided ? .5 * (r.bid + r.ask) : (r.bid > 0 ? r.bid : (r.ask > 0 ? r.ask : 0.5));
  // The other book's half spread is the uncertainty, with a floor: two markets on one outcome
  // disagree by more than their spreads all the time.
  f.sigma = std::max(0.01, two_sided ? .5 * (r.ask - r.bid) + 0.005 : 0.05);
  f.p_raw = f.p;
  f.market_blend_weight = 0;
  f.note = "Polymarket US " + r.slug + " " + (r.flipped ? "(NO side) " : "") + "bid " + std::to_string(r.bid).substr(0, 5) + " ask " +
           std::to_string(r.ask).substr(0, 5);
  f.inputs = {{"valid", fresh && two_sided}, {"polymarket_quote_age_s", age}, {"two_sided", two_sided}, {"slug", r.slug}};
  if (!(fresh && two_sided)) {
    f.inputs["reasons"] = json::array({fresh ? "Polymarket book is one-sided" : "Polymarket quote is stale"});
  }
  return f;
}

}  // namespace desk
