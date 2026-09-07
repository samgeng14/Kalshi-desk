#include "desk/types.h"
#include "desk/log.h"

#include <algorithm>

#include "desk/fixed.h"

namespace desk {

std::string jstr(const json& j, const char* key, const std::string& dflt) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return dflt;
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

double jnum(const json& j, const char* key, double dflt) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return dflt;
  if (it->is_number()) return it->get<double>();
  if (it->is_string()) return parse_num(it->get<std::string>(), dflt);
  return dflt;
}

static std::optional<double> jopt(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return std::nullopt;
  if (it->is_number()) return it->get<double>();
  if (it->is_string()) return parse_num(it->get<std::string>());
  return std::nullopt;
}

std::string series_from_event(const std::string& event_ticker) {
  auto pos = event_ticker.find('-');
  return pos == std::string::npos ? event_ticker : event_ticker.substr(0, pos);
}

Series parse_series(const json& j) {
  Series s;
  s.ticker = jstr(j, "ticker");
  s.category = jstr(j, "category");
  s.title = jstr(j, "title");
  s.fee_type = jstr(j, "fee_type", "quadratic");
  s.frequency = jstr(j, "frequency");
  s.fee_multiplier = jnum(j, "fee_multiplier", 1.0);
  return s;
}

Market parse_market(const json& j) {
  Market m;
  m.ticker = jstr(j, "ticker");
  m.event_ticker = jstr(j, "event_ticker");
  m.series_ticker = series_from_event(m.event_ticker);
  m.title = jstr(j, "title");
  m.subtitle = jstr(j, "subtitle");
  m.yes_sub_title = jstr(j, "yes_sub_title");
  m.no_sub_title = jstr(j, "no_sub_title");
  m.status = jstr(j, "status");
  m.market_type = jstr(j, "market_type");
  m.strike_type = jstr(j, "strike_type");
  m.result = jstr(j, "result");
  m.rules_primary = jstr(j, "rules_primary");
  m.exchange_index = static_cast<int>(jnum(j, "exchange_index", 0));
  m.open_time = jstr(j, "open_time");
  m.close_time = jstr(j, "close_time");
  m.expected_expiration_time = jstr(j, "expected_expiration_time");
  m.expiration_time = jstr(j, "expiration_time");
  m.updated_time = jstr(j, "updated_time");
  m.floor_strike = jopt(j, "floor_strike");
  m.cap_strike = jopt(j, "cap_strike");
  m.yes_bid = jnum(j, "yes_bid_dollars");
  m.yes_ask = jnum(j, "yes_ask_dollars");
  m.no_bid = jnum(j, "no_bid_dollars");
  m.no_ask = jnum(j, "no_ask_dollars");
  m.last = jnum(j, "last_price_dollars");
  m.yes_bid_size = jnum(j, "yes_bid_size_fp");
  m.yes_ask_size = jnum(j, "yes_ask_size_fp");
  m.volume = jnum(j, "volume_fp");
  m.volume_24h = jnum(j, "volume_24h_fp");
  m.open_interest = jnum(j, "open_interest_fp");
  m.liquidity = jnum(j, "liquidity_dollars");
  if (j.contains("custom_strike")) m.custom_strike = j["custom_strike"];
  auto pr = j.find("price_ranges");
  if (pr != j.end() && pr->is_array() && !pr->empty()) {
    double step = jnum((*pr)[0], "step", 0.01);
    if (step > 0) m.tick = step;
  }
  return m;
}

static std::vector<BookLevel> parse_levels(const json& arr) {
  std::vector<BookLevel> out;
  if (!arr.is_array()) return out;
  for (const auto& lv : arr) {
    if (!lv.is_array() || lv.size() < 2) continue;
    BookLevel b;
    b.price = lv[0].is_string() ? parse_num(lv[0].get<std::string>()) : lv[0].get<double>();
    b.qty = lv[1].is_string() ? parse_num(lv[1].get<std::string>()) : lv[1].get<double>();
    out.push_back(b);
  }
  // Kalshi returns levels ascending by price; best bid is the last element.
  std::sort(out.begin(), out.end(),
            [](const BookLevel& a, const BookLevel& b) { return a.price < b.price; });
  return out;
}

Orderbook parse_orderbook(const json& j, const std::string& ticker) {
  Orderbook b;
  b.ticker = ticker;
  const json* ob = &j;
  if (j.contains("orderbook_fp")) ob = &j["orderbook_fp"];
  else if (j.contains("orderbook")) ob = &j["orderbook"];
  if (ob->contains("yes_dollars")) b.yes_bids = parse_levels((*ob)["yes_dollars"]);
  if (ob->contains("no_dollars")) b.no_bids = parse_levels((*ob)["no_dollars"]);
  return b;
}

Trade parse_trade(const json& j) {
  Trade t;
  t.trade_id = jstr(j, "trade_id");
  t.ticker = jstr(j, "ticker");
  t.taker_side = jstr(j, "taker_outcome_side", jstr(j, "taker_side"));
  t.created_time = jstr(j, "created_time");
  t.count = jnum(j, "count_fp", jnum(j, "count"));
  t.yes_price = jnum(j, "yes_price_dollars");
  t.no_price = jnum(j, "no_price_dollars");
  return t;
}

Balance parse_balance(const json& j) {
  Balance b;
  if (j.contains("balance_dollars")) b.balance = jnum(j, "balance_dollars");
  else b.balance = jnum(j, "balance") / 100.0;
  if (j.contains("portfolio_value_dollars")) b.portfolio_value = jnum(j, "portfolio_value_dollars");
  else b.portfolio_value = jnum(j, "portfolio_value") / 100.0;
  b.updated_ts = static_cast<long long>(jnum(j, "updated_ts"));
  return b;
}

Position parse_position(const json& j) {
  Position p;
  p.ticker = jstr(j, "ticker");
  p.position = jnum(j, "position_fp", jnum(j, "position"));
  p.exposure = jnum(j, "market_exposure_dollars");
  p.realized_pnl = jnum(j, "realized_pnl_dollars");
  p.fees_paid = jnum(j, "fees_paid_dollars");
  p.total_traded = jnum(j, "total_traded_dollars");
  p.updated = jstr(j, "last_updated_ts");
  return p;
}

// Kalshi V2 uses bid/ask (YES-book vocabulary) in `side`; older payloads carry yes/no in
// `outcome_side`. Normalize so downstream code only ever sees yes/no: bid = buy YES, ask = sell
// YES = buy NO. Anything else is logged rather than silently treated as NO.
static std::string normalize_side(const std::string& outcome_side, const std::string& side, const char* what) {
  if (outcome_side == "yes" || outcome_side == "no") return outcome_side;
  if (side == "yes" || side == "no") return side;
  if (side == "bid") return "yes";
  if (side == "ask") return "no";
  if (!outcome_side.empty() || !side.empty())
    LOG_WARN("%s: unrecognized side outcome_side=%s side=%s", what, outcome_side.c_str(), side.c_str());
  return outcome_side.empty() ? side : outcome_side;
}

Order parse_order(const json& j) {
  Order o;
  o.order_id = jstr(j, "order_id");
  o.client_order_id = jstr(j, "client_order_id");
  o.ticker = jstr(j, "ticker");
  o.outcome_side = normalize_side(jstr(j, "outcome_side"), jstr(j, "side"), "order");
  o.book_side = jstr(j, "book_side", jstr(j, "side"));
  o.type = jstr(j, "type");
  o.status = jstr(j, "status");
  o.created_time = jstr(j, "created_time");
  o.expiration_time = jstr(j, "expiration_time");
  o.last_update_time = jstr(j, "last_update_time");
  o.yes_price = jnum(j, "yes_price_dollars");
  o.no_price = jnum(j, "no_price_dollars");
  o.initial_count = jnum(j, "initial_count_fp");
  o.fill_count = jnum(j, "fill_count_fp");
  o.remaining_count = jnum(j, "remaining_count_fp");
  o.taker_fees = jnum(j, "taker_fees_dollars");
  o.maker_fees = jnum(j, "maker_fees_dollars");
  return o;
}

Fill parse_fill(const json& j) {
  Fill f;
  f.fill_id = jstr(j, "fill_id");
  f.trade_id = jstr(j, "trade_id");
  f.order_id = jstr(j, "order_id");
  f.ticker = jstr(j, "ticker", jstr(j, "market_ticker"));
  f.outcome_side = normalize_side(jstr(j, "outcome_side"), jstr(j, "side"), "fill");
  f.book_side = jstr(j, "book_side", jstr(j, "side"));
  f.created_time = jstr(j, "created_time");
  f.count = jnum(j, "count_fp", jnum(j, "count"));
  f.yes_price = jnum(j, "yes_price_dollars");
  f.no_price = jnum(j, "no_price_dollars");
  f.fee = jnum(j, "fee_cost");
  auto it = j.find("is_taker");
  f.is_taker = it != j.end() && it->is_boolean() && it->get<bool>();
  f.ts = static_cast<long long>(jnum(j, "ts"));
  return f;
}

Settlement parse_settlement(const json& j) {
  Settlement s;
  s.ticker = jstr(j, "ticker");
  s.event_ticker = jstr(j, "event_ticker");
  s.market_result = jstr(j, "market_result");
  s.settled_time = jstr(j, "settled_time");
  s.yes_count = jnum(j, "yes_count_fp", jnum(j, "yes_count"));
  s.no_count = jnum(j, "no_count_fp", jnum(j, "no_count"));
  s.yes_cost = jnum(j, "yes_total_cost_dollars");
  s.no_cost = jnum(j, "no_total_cost_dollars");
  if (j.contains("revenue_dollars")) s.revenue = jnum(j, "revenue_dollars");
  else s.revenue = jnum(j, "revenue") / 100.0;
  s.fee = jnum(j, "fee_cost");
  return s;
}

OrderResponse parse_order_response(const json& j) {
  OrderResponse r;
  const json* o = &j;
  if (j.contains("order") && j["order"].is_object()) o = &j["order"];
  r.raw = j;
  r.order_id = jstr(*o, "order_id");
  r.client_order_id = jstr(*o, "client_order_id");
  r.fill_count = jnum(*o, "fill_count", jnum(*o, "fill_count_fp"));
  r.remaining_count = jnum(*o, "remaining_count", jnum(*o, "remaining_count_fp"));
  if (o->contains("average_fill_price")) r.avg_fill_price = jnum(*o, "average_fill_price");
  if (o->contains("average_fee_paid")) r.avg_fee = jnum(*o, "average_fee_paid");
  r.ts_ms = static_cast<long long>(jnum(*o, "ts_ms"));
  return r;
}

json series_to_json(const Series& s) {
  return json{{"ticker", s.ticker},       {"category", s.category},
              {"title", s.title},         {"fee_type", s.fee_type},
              {"frequency", s.frequency}, {"fee_multiplier", s.fee_multiplier}};
}

json market_to_json(const Market& m) {
  json j{{"ticker", m.ticker},
         {"event_ticker", m.event_ticker},
         {"series_ticker", m.series_ticker},
         {"title", m.title},
         {"subtitle", m.subtitle},
         {"yes_sub_title", m.yes_sub_title},
         {"no_sub_title", m.no_sub_title},
         {"status", m.status},
         {"strike_type", m.strike_type},
         {"result", m.result},
         {"close_time", m.close_time},
         {"expected_expiration_time", m.expected_expiration_time},
         {"yes_bid", m.yes_bid},
         {"yes_ask", m.yes_ask},
         {"no_bid", m.no_bid},
         {"no_ask", m.no_ask},
         {"last", m.last},
         {"yes_bid_size", m.yes_bid_size},
         {"yes_ask_size", m.yes_ask_size},
         {"volume", m.volume},
         {"volume_24h", m.volume_24h},
         {"open_interest", m.open_interest},
         {"tick", m.tick}, {"exchange_index", m.exchange_index}};
  if (m.floor_strike) j["floor_strike"] = *m.floor_strike;
  if (m.cap_strike) j["cap_strike"] = *m.cap_strike;
  return j;
}

json orderbook_to_json(const Orderbook& b) {
  json yes = json::array(), no = json::array();
  for (const auto& l : b.yes_bids) yes.push_back({l.price, l.qty});
  for (const auto& l : b.no_bids) no.push_back({l.price, l.qty});
  return json{{"ticker", b.ticker}, {"yes_bids", yes}, {"no_bids", no}};
}

}  // namespace desk
