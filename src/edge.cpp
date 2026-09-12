#include "desk/edge.h"

#include <algorithm>

#include "desk/fixed.h"

namespace desk {
namespace {

bool real_price(double x) { return std::isfinite(x) && x > 0 && x < 1; }
double price_or_zero(double x) { return real_price(x) ? x : 0.0; }
double size_or_zero(double x) { return std::isfinite(x) && x > 0 ? x : 0.0; }
double row_num(const nlohmann::json& j, const char* k) {
  return j.is_object() && j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>()) ? j[k].get<double>() : 0.0;
}

}  // namespace

bool MarketSidePrices::two_sided(Side s) const {
  const double b = bid(s), a = ask(s);
  return real_price(a) && real_price(b) && b < a + 1e-9;
}

double MarketSidePrices::yes_mid() const { return two_sided(Side::Yes) ? 0.5 * (yes_bid + yes_ask) : 0.0; }

MarketSidePrices normalize_prices(double yes_bid, double yes_ask, double no_bid, double no_ask, double yes_bid_size,
                                  double yes_ask_size, double no_bid_size, double no_ask_size) {
  MarketSidePrices p;
  const double yb = price_or_zero(yes_bid), ya = price_or_zero(yes_ask), nb = price_or_zero(no_bid), na = price_or_zero(no_ask);
  // YES from its own book, else from the NO book it mirrors.
  p.yes_bid = yb > 0 ? yb : (na > 0 ? 1.0 - na : 0.0);
  p.yes_ask = ya > 0 ? ya : (nb > 0 ? 1.0 - nb : 0.0);
  // NO from its own book, else from the YES book: NO ask = 1 - YES bid, NO bid = 1 - YES ask.
  p.no_ask = na > 0 ? na : (yb > 0 ? 1.0 - yb : 0.0);
  p.no_bid = nb > 0 ? nb : (ya > 0 ? 1.0 - ya : 0.0);
  p.yes_bid = price_or_zero(p.yes_bid);
  p.yes_ask = price_or_zero(p.yes_ask);
  p.no_bid = price_or_zero(p.no_bid);
  p.no_ask = price_or_zero(p.no_ask);
  // Sizes follow the same identity: the NO ask rests on the YES bid, the NO bid on the YES ask.
  const double ybs = size_or_zero(yes_bid_size), yas = size_or_zero(yes_ask_size);
  const double nbs = size_or_zero(no_bid_size), nas = size_or_zero(no_ask_size);
  p.yes_bid_size = ybs > 0 ? ybs : nas;
  p.yes_ask_size = yas > 0 ? yas : nbs;
  p.no_ask_size = nas > 0 ? nas : ybs;
  p.no_bid_size = nbs > 0 ? nbs : yas;
  p.crossed = p.yes_bid > 0 && p.yes_ask > 0 && p.yes_bid > p.yes_ask + 1e-9;
  return p;
}

MarketSidePrices normalize_prices(const Market& m) {
  return normalize_prices(m.yes_bid, m.yes_ask, m.no_bid, m.no_ask, m.yes_bid_size, m.yes_ask_size, 0, 0);
}

MarketSidePrices normalize_prices_from_row(const nlohmann::json& row) {
  return normalize_prices(row_num(row, "yes_bid"), row_num(row, "yes_ask"), row_num(row, "no_bid"), row_num(row, "no_ask"),
                          row_num(row, "yes_bid_size"), row_num(row, "yes_ask_size"), row_num(row, "no_bid_size"),
                          row_num(row, "no_ask_size"));
}

EdgeCandidate candidate_at(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const std::string& style,
                           double price, const FeeModel& fm, const EdgeSettings& es) {
  EdgeCandidate c;
  c.fair = q;
  c.side = side;
  c.style = style == "maker" ? "maker" : "taker";
  const bool taker = c.style == "taker";
  const double p = q.p_yes, raw = q.p_raw();
  c.fair_probability = std::isfinite(p) ? (side == Side::Yes ? p : 1.0 - p) : NAN;
  c.raw_fair_probability = std::isfinite(raw) ? (side == Side::Yes ? raw : 1.0 - raw) : NAN;
  c.bid = px.bid(side);
  c.ask = px.ask(side);
  c.bid_size = px.bid_size(side);
  c.ask_size = px.ask_size(side);
  c.spread = px.two_sided(side) ? c.ask - c.bid : 0.0;
  c.entry_price = real_price(price) ? price : 0.0;
  c.executable = c.entry_price > 0;
  c.displayed_size = static_cast<int>(std::floor(taker ? c.ask_size : c.bid_size));
  const double sigma = std::isfinite(q.sigma) ? q.sigma : 0.0;
  c.uncertainty_buffer = es.uncertainty_multiplier * sigma;
  c.estimated_slippage = taker ? es.taker_slippage : 0.0;
  if (!c.executable || !std::isfinite(c.fair_probability)) {
    c.executable = false;
    c.raw_edge = c.net_edge = c.executable_edge = c.ev_per_contract = -1;
    c.kelly = 0;
    return c;
  }
  c.entry_fee = fm.fee(c.entry_price, 1, taker);
  c.raw_edge = c.fair_probability - c.entry_price;
  c.net_edge = c.raw_edge - c.entry_fee;
  c.executable_edge = c.net_edge - c.estimated_slippage - c.uncertainty_buffer;
  c.ev_per_contract = c.net_edge - c.estimated_slippage;
  const double cost = c.entry_price + c.entry_fee + c.estimated_slippage;
  c.kelly = cost > 0 && cost < 1 ? (c.fair_probability - cost - c.uncertainty_buffer) / (1.0 - cost) : 0.0;
  return c;
}

EdgeCandidate taker_candidate(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const FeeModel& fm,
                              const EdgeSettings& es) {
  // The executable taker price is the ask for the side being bought, and nothing else: a taker
  // edge measured at the mid is an edge nobody can trade. A one-sided book with an ask can still
  // be lifted, so the candidate exists; whether anyone should is the router's call (it wants a
  // two-sided book), and a crossed book is not a price at all.
  const double ask = px.crossed ? 0.0 : px.ask(side);
  EdgeCandidate c = candidate_at(q, px, side, "taker", ask, fm, es);
  if (!(ask > 0)) c.diagnostics["why_not_executable"] = px.crossed ? "crossed book" : "no offer to lift";
  else if (!px.two_sided(side)) c.diagnostics["one_sided"] = true;
  return c;
}

EdgeCandidate maker_candidate(const ProbabilityQuote& q, const MarketSidePrices& px, Side side, const FeeModel& fm,
                              double tick, const EdgeSettings& es) {
  if (!(tick > 0)) tick = 0.01;
  double price = 0.0;
  if (px.two_sided(side) && !px.crossed) {
    const double bid = px.bid(side), ask = px.ask(side);
    price = ask - bid >= 2 * tick - 1e-9 ? floor_to_tick(bid + tick, tick) : bid;
    price = std::max(tick, std::min(price, ask - tick));
  }
  EdgeCandidate c = candidate_at(q, px, side, "maker", price, fm, es);
  if (!(price > 0)) c.diagnostics["why_not_executable"] = "a maker needs a two-sided book to post into";
  return c;
}

nlohmann::json candidate_to_json(const EdgeCandidate& c) {
  auto num = [](double x) { return std::isfinite(x) ? nlohmann::json(x) : nlohmann::json(nullptr); };
  return nlohmann::json{{"side", side_name(c.side)},
                        {"style", c.style},
                        {"fair_probability", num(c.fair_probability)},
                        {"raw_fair_probability", num(c.raw_fair_probability)},
                        {"entry_price", c.entry_price},
                        {"entry_fee", c.entry_fee},
                        {"estimated_slippage", c.estimated_slippage},
                        {"uncertainty_buffer", c.uncertainty_buffer},
                        {"raw_edge", c.raw_edge},
                        {"net_edge", c.net_edge},
                        {"executable_edge", c.executable_edge},
                        {"ev_per_contract", c.ev_per_contract},
                        {"kelly", c.kelly},
                        {"bid", c.bid},
                        {"ask", c.ask},
                        {"spread", c.spread},
                        {"bid_size", c.bid_size},
                        {"ask_size", c.ask_size},
                        {"displayed_size", c.displayed_size},
                        {"executable", c.executable},
                        {"diagnostics", c.diagnostics}};
}

}  // namespace desk
