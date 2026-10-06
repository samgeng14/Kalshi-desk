#include "rest.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>

namespace fakex {

namespace {

const char* outcome(Side s) { return s == Side::BID ? "yes" : "no"; }
const char* book_side(Side s) { return s == Side::BID ? "bid" : "ask"; }

RestReply ok(int status, const json& j) { return {status, j.dump()}; }
RestReply bad(const std::string& message) { return rest_error(400, "invalid_parameters", message); }
RestReply refused(const Refusal& r) { return rest_error(r.http, r.code, r.message, r.details); }

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep))
    if (!cur.empty()) out.push_back(cur);
  return out;
}

// A query integer: nullopt when absent; *bad set when present and not an integer.
std::optional<long long> qint(const net::Request& r, const char* key, bool* bad) {
  auto it = r.params.find(key);
  if (it == r.params.end()) return std::nullopt;
  const std::string& s = it->second;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0') {
    *bad = true;
    return std::nullopt;
  }
  return v;
}
std::string qstr(const net::Request& r, const char* key) {
  auto it = r.params.find(key);
  return it == r.params.end() ? std::string() : it->second;
}

// limit (1-1000, default 100) and cursor, the venue's paging (get-orders.md, get-fills.md).
bool paging(const net::Request& r, int* limit, std::uint64_t* after, RestReply* err) {
  bool b = false;
  const auto l = qint(r, "limit", &b);
  if (b || (l && (*l < 1 || *l > 1000))) {
    *err = bad("limit must be an integer from 1 to 1000");
    return false;
  }
  *limit = l ? static_cast<int>(*l) : 100;
  *after = 0;
  const std::string c = qstr(r, "cursor");
  if (!c.empty() && !read_cursor(c, after)) {
    *err = bad("invalid cursor");
    return false;
  }
  return true;
}

// subaccount: 0-63 (create-order-v2.md).
bool subaccount_of(const net::Request& r, std::optional<int>* sub, RestReply* err) {
  bool b = false;
  const auto s = qint(r, "subaccount", &b);
  if (b || (s && (*s < 0 || *s > 63))) {
    *err = bad("subaccount must be 0-63");
    return false;
  }
  *sub = s ? std::optional<int>(static_cast<int>(*s)) : std::nullopt;
  return true;
}

std::string hex(const std::string& s) {
  std::string out;
  char b[3];
  for (unsigned char c : s) {
    std::snprintf(b, sizeof b, "%02x", c);
    out += b;
  }
  return out;
}
bool unhex(const std::string& s, std::string* out) {
  if (s.size() % 2) return false;
  out->clear();
  for (std::size_t i = 0; i < s.size(); i += 2) {
    char* end = nullptr;
    const std::string pair = s.substr(i, 2);
    const long v = std::strtol(pair.c_str(), &end, 16);
    if (*end != '\0') return false;
    out->push_back(static_cast<char>(v));
  }
  return true;
}

Micros avg_per_contract(Micros total, Qty count) {
  // micro-dollars per contract, rounded half up: total over count/100 contracts
  return count == 0 ? 0 : (total * 100 * 2 + count) / (2 * count);
}

// ---- create ---------------------------------------------------------------------------------------

RestReply create_order(Engine& e, const json& b, std::int64_t now, std::string* created) {
  if (!b.is_object()) return bad("the body must be a JSON object");
  // Required: ticker, side, count, price, time_in_force, self_trade_prevention_type
  // (CreateOrderV2Request). Wrong types are refused, not coerced.
  auto str = [&](const char* k, std::string* out, bool required) -> bool {
    auto it = b.find(k);
    if (it == b.end() || it->is_null()) return !required;
    if (!it->is_string()) return false;
    *out = it->get<std::string>();
    return true;
  };
  auto flag = [&](const char* k, bool* out) -> bool {
    auto it = b.find(k);
    if (it == b.end() || it->is_null()) return true;
    if (!it->is_boolean()) return false;
    *out = it->get<bool>();
    return true;
  };
  auto integer = [&](const char* k, long long* out, bool* present) -> bool {
    auto it = b.find(k);
    *present = false;
    if (it == b.end() || it->is_null()) return true;
    if (!it->is_number_integer()) return false;
    *out = it->get<long long>();
    *present = true;
    return true;
  };
  NewOrder n;
  std::string side, count, price, tif, stp;
  if (!str("ticker", &n.ticker, true) || n.ticker.empty()) return bad("ticker is required");
  if (!str("side", &side, true) || (side != "bid" && side != "ask")) return bad("side must be bid or ask");
  n.side = side == "bid" ? Side::BID : Side::ASK;
  if (!str("count", &count, true)) return bad("count is required, a string with up to 2 decimals");
  auto c = parse_count_req(count);
  if (!c || *c <= 0) return bad("count must be a positive fixed-point string with up to 2 decimals");
  n.count = *c;
  if (!str("price", &price, true)) return bad("price is required, a dollar string");
  auto p = parse_price_req(price);
  if (!p) return bad("price must be a dollar string with 2 to 4 decimals");
  n.price = *p;
  if (!str("time_in_force", &tif, true)) return bad("time_in_force is required");
  if (tif == "fill_or_kill") {
    n.tif = Tif::FOK;
  } else if (tif == "good_till_canceled") {
    n.tif = Tif::GTC;
  } else if (tif == "immediate_or_cancel") {
    n.tif = Tif::IOC;
  } else {
    return bad("time_in_force must be fill_or_kill, good_till_canceled or immediate_or_cancel");
  }
  if (!str("self_trade_prevention_type", &stp, true) || (stp != "taker_at_cross" && stp != "maker"))
    return bad("self_trade_prevention_type must be taker_at_cross or maker");
  n.stp = stp == "maker" ? Stp::MAKER : Stp::TAKER_AT_CROSS;
  if (!str("client_order_id", &n.client_order_id, false)) return bad("client_order_id must be a string");
  if (!str("order_group_id", &n.order_group_id, false)) return bad("order_group_id must be a string");
  if (!flag("post_only", &n.post_only) || !flag("reduce_only", &n.reduce_only) ||
      !flag("cancel_order_on_pause", &n.cancel_on_pause))
    return bad("post_only, reduce_only and cancel_order_on_pause are booleans");
  long long v = 0;
  bool present = false;
  if (!integer("expiration_time", &v, &present) || (present && v <= 0))
    return bad("expiration_time must be whole Unix seconds");
  if (present) n.expiration_s = v;
  if (!integer("subaccount", &v, &present) || (present && (v < 0 || v > 63))) return bad("subaccount must be 0-63");
  if (present) n.subaccount = static_cast<int>(v);
  if (!integer("exchange_index", &v, &present) || (present && v < -1)) return bad("exchange_index must be -1 or a shard");
  if (present) n.exchange_index = static_cast<int>(v);

  const CreateResult r = e.create(n, now);
  if (r.err) return refused(r.err);
  *created = r.order_id;
  json j{{"order_id", r.order_id},
         {"fill_count", qty_str(r.fill_count)},
         {"remaining_count", qty_str(r.remaining)},
         {"ts_ms", r.ts_ms}};
  if (!r.client_order_id.empty()) j["client_order_id"] = r.client_order_id;
  // "Only present when fill_count > 0" (CreateOrderV2Response)
  if (r.fill_count > 0) {
    j["average_fill_price"] = usd4(avg_per_contract(r.fill_cost, r.fill_count));
    j["average_fee_paid"] = usd4(avg_per_contract(r.fees, r.fill_count));
  }
  return ok(201, j);
}

// ---- cancels --------------------------------------------------------------------------------------

RestReply cancel_one(Engine& e, const net::Request& r, const std::string& id, std::int64_t now) {
  bool b = false;
  const auto idx = qint(r, "exchange_index", &b);
  std::optional<int> sub;
  RestReply err;
  if (b || (idx && *idx < -1)) return bad("exchange_index must be -1 or a shard");
  if (!subaccount_of(r, &sub, &err)) return err;
  const CancelResult c = e.cancel(id, qstr(r, "market_ticker"), idx ? static_cast<int>(*idx) : -2, sub.value_or(0), now);
  if (c.err) return refused(c.err);
  return ok(200, json{{"order_id", c.order_id},
                      {"client_order_id", c.client_order_id},
                      {"reduced_by", qty_str(c.reduced_by)},
                      {"ts_ms", c.ts_ms}});
}

RestReply cancel_batch(Engine& e, const json& b, std::int64_t now) {
  if (!b.is_object() || !b.contains("orders") || !b["orders"].is_array() || b["orders"].empty())
    return bad("orders must be a non-empty array");
  json out = json::array();
  for (const auto& o : b["orders"]) {
    if (!o.is_object() || !o.contains("order_id") || !o["order_id"].is_string()) return bad("each order needs an order_id");
    const std::string id = o["order_id"].get<std::string>();
    const int idx = o.contains("exchange_index") && o["exchange_index"].is_number_integer() ? o["exchange_index"].get<int>() : -2;
    const int sub = o.contains("subaccount") && o["subaccount"].is_number_integer() ? o["subaccount"].get<int>() : 0;
    const std::string ticker = o.value("market_ticker", std::string());
    const CancelResult c = e.cancel(id, ticker, idx, sub, now);
    if (c.err) {
      json err{{"code", c.err.code}, {"message", c.err.message}};
      out.push_back(json{{"order_id", id}, {"reduced_by", "0.00"}, {"ts_ms", nullptr}, {"client_order_id", nullptr},
                         {"error", err}});
    } else {
      out.push_back(json{{"order_id", id},
                         {"reduced_by", qty_str(c.reduced_by)},
                         {"ts_ms", c.ts_ms},
                         {"client_order_id", c.client_order_id},
                         {"error", nullptr}});
    }
  }
  return ok(200, json{{"orders", out}});
}

// ---- reads ----------------------------------------------------------------------------------------

RestReply list_orders(Engine& e, const net::Request& r) {
  int limit;
  std::uint64_t after;
  RestReply err;
  if (!paging(r, &limit, &after, &err)) return err;
  std::optional<int> sub;
  if (!subaccount_of(r, &sub, &err)) return err;
  bool b = false;
  const auto min_ts = qint(r, "min_ts", &b), max_ts = qint(r, "max_ts", &b), idx = qint(r, "exchange_index", &b);
  if (b || (idx && *idx < 0)) return bad("min_ts, max_ts and exchange_index are integers");
  const std::string status = qstr(r, "status"), ticker = qstr(r, "ticker");
  if (!status.empty() && status != "resting" && status != "canceled" && status != "executed")
    return bad("status must be resting, canceled or executed");
  const auto events = split(qstr(r, "event_ticker"), ',');
  if (events.size() > 10) return bad("at most 10 event tickers");
  json orders = json::array();
  std::uint64_t last = 0;
  bool more = false;
  const auto& all = e.user_orders();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {  // newest first
    const Order& o = **it;
    if (after && o.seq >= after) continue;
    if (sub && o.subaccount != *sub) continue;
    if (idx && o.exchange_index != *idx) continue;
    if (!ticker.empty() && o.ticker != ticker) continue;
    if (!status.empty() && status != status_name(o.status)) continue;
    if (min_ts && o.created_ms / 1000 < *min_ts) continue;
    if (max_ts && o.created_ms / 1000 > *max_ts) continue;
    if (!events.empty()) {
      const Market* m = e.market(o.ticker);
      if (!m || std::find(events.begin(), events.end(), m->spec.event_ticker) == events.end()) continue;
    }
    if (static_cast<int>(orders.size()) == limit) {
      more = true;
      break;
    }
    orders.push_back(rest_order_json(o, e.config().user_id));
    last = o.seq;
  }
  return ok(200, json{{"orders", orders}, {"cursor", more ? make_cursor(last) : ""}});
}

json fill_json(const Fill& f) {
  return json{{"fill_id", f.fill_id},
              {"exchange_index", f.exchange_index},
              {"trade_id", f.trade_id},
              {"order_id", f.order_id},
              {"ticker", f.ticker},
              {"market_ticker", f.ticker},
              {"outcome_side", outcome(f.side)},
              {"book_side", book_side(f.side)},
              {"count_fp", qty_str(f.count)},
              {"yes_price_dollars", px_str(f.yes_px)},
              {"no_price_dollars", px_str(kDollar - f.yes_px)},
              {"is_taker", f.taker},
              {"created_time", rfc3339(f.ts_ms)},
              {"fee_cost", usd6(f.fee)},
              {"subaccount_number", f.subaccount},
              {"ts", f.ts_ms / 1000}};
}

RestReply list_fills(Engine& e, const net::Request& r) {
  int limit;
  std::uint64_t after;
  RestReply err;
  if (!paging(r, &limit, &after, &err)) return err;
  std::optional<int> sub;
  if (!subaccount_of(r, &sub, &err)) return err;
  bool b = false;
  const auto min_ts = qint(r, "min_ts", &b), max_ts = qint(r, "max_ts", &b), idx = qint(r, "exchange_index", &b);
  if (b || (idx && *idx < 0)) return bad("min_ts, max_ts and exchange_index are integers");
  const auto tickers = split(qstr(r, "ticker"), ',');
  if (tickers.size() > 100) return bad("at most 100 tickers");
  const std::string order_id = qstr(r, "order_id");
  json fills = json::array();
  std::uint64_t last = 0;
  bool more = false;
  const auto& all = e.fills();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const Fill& f = *it;
    if (after && f.seq >= after) continue;
    if (sub && f.subaccount != *sub) continue;
    if (idx && f.exchange_index != *idx) continue;
    if (!tickers.empty() && std::find(tickers.begin(), tickers.end(), f.ticker) == tickers.end()) continue;
    if (!order_id.empty() && f.order_id != order_id) continue;
    if (min_ts && f.ts_ms / 1000 < *min_ts) continue;
    if (max_ts && f.ts_ms / 1000 > *max_ts) continue;
    if (static_cast<int>(fills.size()) == limit) {
      more = true;
      break;
    }
    fills.push_back(fill_json(f));
    last = f.seq;
  }
  return ok(200, json{{"fills", fills}, {"cursor", more ? make_cursor(last) : ""}});
}

RestReply list_positions(Engine& e, const net::Request& r) {
  bool b = false;
  const auto l = qint(r, "limit", &b);
  if (b || (l && (*l < 1 || *l > 1000))) return bad("limit must be an integer from 1 to 1000");
  const int limit = l ? static_cast<int>(*l) : 100;
  std::string after;
  const std::string cursor = qstr(r, "cursor");
  if (!cursor.empty() && (cursor[0] != 'p' || !unhex(cursor.substr(1), &after))) return bad("invalid cursor");
  std::optional<int> sub;
  RestReply err;
  if (!subaccount_of(r, &sub, &err)) return err;
  const auto idx = qint(r, "exchange_index", &b);
  if (b || (idx && *idx < 0)) return bad("exchange_index must be a shard");
  const auto filters = split(qstr(r, "count_filter"), ',');
  for (const auto& f : filters)
    if (f != "position" && f != "total_traded") return bad("count_filter takes position and total_traded");
  const std::string settled = qstr(r, "settlement_status");
  if (!settled.empty() && settled != "unsettled" && settled != "settled" && settled != "all")
    return bad("settlement_status must be unsettled, settled or all");
  const std::string ticker = qstr(r, "ticker"), event = qstr(r, "event_ticker");
  struct Row {
    std::string ticker, event;
    const Position* p;
  };
  std::vector<Row> rows;
  for (const auto& [k, p] : e.positions()) {
    if (k.first != sub.value_or(0)) continue;  // positions default to the primary account
    if (idx && p.exchange_index != *idx) continue;
    if (!ticker.empty() && k.second != ticker) continue;
    const Market* m = e.market(k.second);
    const std::string ev = m ? m->spec.event_ticker : std::string();
    if (!event.empty() && ev != event) continue;
    if (settled == "settled") continue;  // the fake settles nothing
    if (!filters.empty()) {
      bool keep = false;
      for (const auto& f : filters) keep = keep || (f == "position" ? p.pos != 0 : p.volume != 0);
      if (!keep) continue;
    }
    rows.push_back({k.second, ev, &p});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ticker < b.ticker; });
  json mp = json::array();
  std::map<std::string, std::vector<const Position*>> by_event;
  std::string last;
  bool more = false;
  for (const Row& row : rows) {
    by_event[row.event].push_back(row.p);
    if (!after.empty() && row.ticker <= after) continue;
    if (static_cast<int>(mp.size()) == limit) {
      more = true;
      continue;
    }
    const Position& p = *row.p;
    mp.push_back(json{{"ticker", row.ticker},
                      {"exchange_index", p.exchange_index},
                      {"total_traded_dollars", usd4(p.traded)},
                      {"position_fp", qty_str(p.pos)},
                      {"market_exposure_dollars", usd4(p.cost)},
                      {"realized_pnl_dollars", usd4(p.realized)},
                      {"fees_paid_dollars", usd4(p.fees)},
                      {"last_updated_ts", rfc3339(p.updated_ms)}});
    last = row.ticker;
  }
  json ep = json::array();
  for (const auto& [ev, ps] : by_event) {
    Micros cost = 0, realized = 0, fees = 0;
    Qty shares = 0;  // "total shares traded" (get-positions.md)
    for (const Position* p : ps) {
      cost += p->cost;
      realized += p->realized;
      fees += p->fees;
      shares += p->volume;
    }
    ep.push_back(json{{"event_ticker", ev},
                      {"total_cost_dollars", usd4(cost)},
                      {"total_cost_shares_fp", qty_str(shares)},
                      {"event_exposure_dollars", usd4(cost)},
                      {"realized_pnl_dollars", usd4(realized)},
                      {"fees_paid_dollars", usd4(fees)}});
  }
  return ok(200, json{{"market_positions", mp}, {"event_positions", ep}, {"cursor", more ? "p" + hex(last) : ""}});
}

RestReply balance(Engine& e, const net::Request& r) {
  std::optional<int> sub;
  RestReply err;
  if (!subaccount_of(r, &sub, &err)) return err;
  bool b = false;
  const auto idx = qint(r, "exchange_index", &b);
  if (b || (idx && *idx < 0)) return bad("exchange_index must be a shard");
  const int s = sub.value_or(0);
  Micros avail = 0, value = 0;
  json breakdown = json::array();
  for (const int shard : e.shards()) {
    const Micros a = e.available(s, shard);
    breakdown.push_back(json{{"exchange_index", shard}, {"balance", usd4(a)}});
    if (idx && *idx != shard) continue;
    avail += a;
    value += e.position_value(s, shard);
  }
  // balance and portfolio_value in cents (get-balance.md); the fake floors to the cent
  auto cents = [](Micros m) { return m >= 0 ? m / 10000 : -((-m + 9999) / 10000); };
  return ok(200, json{{"balance", cents(avail)},
                      {"balance_dollars", usd4(avail)},
                      {"portfolio_value", cents(value)},
                      {"updated_ts", e.cash_updated_ms()},
                      {"balance_breakdown", breakdown}});
}

// ---- order groups ---------------------------------------------------------------------------------

// contracts_limit (whole contracts) or contracts_limit_fp; "if both are provided, they must match"
bool group_limit_of(const json& b, Qty* limit, RestReply* err) {
  std::optional<Qty> a, f;
  if (b.is_object() && b.contains("contracts_limit") && !b["contracts_limit"].is_null()) {
    if (!b["contracts_limit"].is_number_integer() || b["contracts_limit"].get<long long>() < 1) {
      *err = bad("contracts_limit must be a whole number of at least 1");
      return false;
    }
    a = b["contracts_limit"].get<long long>() * kContract;
  }
  if (b.is_object() && b.contains("contracts_limit_fp") && !b["contracts_limit_fp"].is_null()) {
    auto v = b["contracts_limit_fp"].is_string() ? parse_count_req(b["contracts_limit_fp"].get<std::string>()) : std::nullopt;
    if (!v || *v < kContract) {
      *err = bad("contracts_limit_fp must be a count string of at least 1.00");
      return false;
    }
    f = *v;
  }
  if (!a && !f) {
    *err = bad("contracts_limit or contracts_limit_fp is required");
    return false;
  }
  if (a && f && *a != *f) {
    *err = bad("contracts_limit and contracts_limit_fp do not match");
    return false;
  }
  *limit = a ? *a : *f;
  if (*limit > 1000000 * kContract) {
    *err = bad("contracts_limit is at most 1,000,000");
    return false;
  }
  return true;
}

// subaccount and exchange_index for a group call: both default to 0 (order-groups/*.md)
bool group_route(const net::Request& r, int* sub, int* shard, RestReply* err) {
  std::optional<int> s;
  if (!subaccount_of(r, &s, err)) return false;
  bool b = false;
  const auto idx = qint(r, "exchange_index", &b);
  if (b || (idx && *idx < 0)) {
    *err = bad("exchange_index must be a shard");
    return false;
  }
  *sub = s.value_or(0);
  *shard = idx ? static_cast<int>(*idx) : 0;
  return true;
}

RestReply groups_route(Engine& e, const net::Request& r, const std::vector<std::string>& seg, const json& b,
                       std::int64_t now) {
  RestReply err;
  if (seg.size() == 2 && r.method == "GET") {
    std::optional<int> sub;
    if (!subaccount_of(r, &sub, &err)) return err;
    json out = json::array();
    for (const auto& [id, g] : e.groups()) {
      if (sub && g.subaccount != *sub) continue;
      out.push_back(json{{"id", id}, {"contracts_limit_fp", qty_str(g.limit)}, {"is_auto_cancel_enabled", true},
                         {"exchange_index", g.exchange_index}});
    }
    return ok(200, json{{"order_groups", out}});
  }
  if (seg.size() == 3 && seg[2] == "create") {
    if (r.method != "POST") return rest_error(405, "method_not_allowed", "use POST");
    Qty limit;
    if (!group_limit_of(b, &limit, &err)) return err;
    int sub = 0, shard = 0;
    if (b.contains("subaccount")) {
      if (!b["subaccount"].is_number_integer() || b["subaccount"].get<int>() < 0 || b["subaccount"].get<int>() > 63)
        return bad("subaccount must be 0-63");
      sub = b["subaccount"].get<int>();
    }
    if (b.contains("exchange_index")) {
      if (!b["exchange_index"].is_number_integer() || b["exchange_index"].get<int>() < 0)
        return bad("exchange_index must be a shard");
      shard = b["exchange_index"].get<int>();  // "default": 0 (create-order-group.md)
    }
    const std::string id = e.group_create(sub, shard, limit, now);
    return ok(201, json{{"order_group_id", id}, {"subaccount", sub}, {"exchange_index", shard}});
  }
  if (seg.size() == 3) {
    const std::string& id = seg[2];
    if (r.method == "GET") {
      std::optional<int> sub;
      if (!subaccount_of(r, &sub, &err)) return err;
      const Group* g = e.group(id);
      if (!g || (sub && g->subaccount != *sub)) return rest_error(404, "not_found", "order group not found");
      return ok(200, json{{"is_auto_cancel_enabled", true},
                          {"contracts_limit_fp", qty_str(g->limit)},
                          {"orders", g->orders},
                          {"exchange_index", g->exchange_index}});
    }
    if (r.method == "DELETE") {
      int sub, shard;
      if (!group_route(r, &sub, &shard, &err)) return err;
      const Refusal f = e.group_delete(id, sub, shard, now);
      return f ? refused(f) : ok(200, json::object());
    }
    return rest_error(405, "method_not_allowed", "GET or DELETE");
  }
  if (seg.size() == 4) {
    if (r.method != "PUT") return rest_error(405, "method_not_allowed", "use PUT");
    const std::string& id = seg[2];
    int sub, shard;
    if (!group_route(r, &sub, &shard, &err)) return err;
    Refusal f;
    if (seg[3] == "trigger") {
      f = e.group_trigger(id, sub, shard, now);
    } else if (seg[3] == "reset") {
      f = e.group_reset(id, sub, shard, now);
    } else if (seg[3] == "limit") {
      Qty limit;
      if (!group_limit_of(b, &limit, &err)) return err;
      f = e.group_limit(id, sub, shard, limit, now);
    } else {
      return rest_error(404, "not_found", "no such route");
    }
    return f ? refused(f) : ok(200, json::object());
  }
  return rest_error(404, "not_found", "no such route");
}

json market_json(const Engine& e, const Market& m) {
  const auto yes = e.levels(m.spec.ticker, false), no = e.levels(m.spec.ticker, true);
  const Px bid = yes.empty() ? 0 : yes.back().first, ask = no.empty() ? kDollar : kDollar - no.back().first;
  json ranges = json::array();
  for (const auto& r : m.spec.ticks)
    ranges.push_back(json{{"start", px_str(r.start)}, {"end", px_str(r.end)}, {"step", px_str(r.step)}});
  const char* status = m.state == Market::State::OPEN ? "active" : m.state == Market::State::PAUSED ? "paused" : "closed";
  json j{{"ticker", m.spec.ticker},
         {"event_ticker", m.spec.event_ticker},
         {"market_type", "binary"},
         {"status", status},
         {"exchange_index", m.spec.exchange_index},
         {"yes_bid_dollars", px_str(bid)},
         {"yes_ask_dollars", px_str(ask)},
         {"no_bid_dollars", px_str(kDollar - ask)},
         {"no_ask_dollars", px_str(kDollar - bid)},
         {"last_price_dollars", px_str(m.last_px)},
         {"yes_bid_size_fp", qty_str(yes.empty() ? 0 : yes.back().second)},
         {"yes_ask_size_fp", qty_str(no.empty() ? 0 : no.back().second)},
         {"volume_fp", qty_str(m.volume)},
         {"open_interest_fp", qty_str(m.open_interest)},
         {"price_level_structure", m.spec.price_level_structure},
         {"price_ranges", ranges}};
  if (m.spec.close_ms > 0) j["close_time"] = rfc3339(m.spec.close_ms);
  return j;
}

}  // namespace

RestReply rest_error(int status, const std::string& code, const std::string& message, const std::string& details) {
  json e{{"code", code}, {"message", message}};
  if (!details.empty()) e["details"] = details;
  return {status, json{{"error", e}}.dump()};
}

std::string make_cursor(std::uint64_t key) {
  char b[32];
  std::snprintf(b, sizeof b, "c%llx", static_cast<unsigned long long>(key));
  return b;
}

bool read_cursor(const std::string& c, std::uint64_t* key) {
  if (c.size() < 2 || c[0] != 'c') return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(c.c_str() + 1, &end, 16);
  if (*end != '\0' || v == 0) return false;
  *key = v;
  return true;
}

// The fields get-orders.md lists, and only those: the deprecated `side` and `action` are left out,
// so deskd cannot come to rely on them.
json rest_order_json(const Order& o, const std::string& user_id) {
  return json{{"order_id", o.order_id},
              {"user_id", user_id},
              {"client_order_id", o.client_order_id},
              {"ticker", o.ticker},
              {"outcome_side", outcome(o.side)},
              {"book_side", book_side(o.side)},
              {"type", "limit"},
              {"status", status_name(o.status)},
              {"yes_price_dollars", px_str(o.price)},
              {"no_price_dollars", px_str(kDollar - o.price)},
              {"fill_count_fp", qty_str(o.filled)},
              {"remaining_count_fp", qty_str(o.status == OrdStatus::RESTING ? o.remaining : 0)},
              {"initial_count_fp", qty_str(o.initial)},
              {"taker_fees_dollars", usd4(o.taker_fees)},
              {"maker_fees_dollars", usd4(o.maker_fees)},
              {"taker_fill_cost_dollars", usd4(o.taker_cost)},
              {"maker_fill_cost_dollars", usd4(o.maker_cost)},
              {"expiration_time", o.expiration_s > 0 ? json(rfc3339(o.expiration_s * 1000)) : json(nullptr)},
              {"created_time", rfc3339(o.created_ms)},
              {"last_update_time", rfc3339(o.updated_ms)},
              {"self_trade_prevention_type", stp_name(o.stp)},
              {"order_group_id", o.order_group_id.empty() ? json(nullptr) : json(o.order_group_id)},
              {"cancel_order_on_pause", o.cancel_on_pause},
              {"subaccount_number", o.subaccount},
              {"exchange_index", o.exchange_index}};
}

RestReply rest_route(Engine& e, const net::Request& r, const std::string& path, const json& body, std::int64_t now,
                     std::string* created) {
  const auto seg = split(path, '/');
  const bool has_body = r.method == "POST" || r.method == "PUT" || (r.method == "DELETE" && !r.body.empty());
  if (has_body && !r.body.empty() && body.is_discarded()) return bad("the body is not JSON");
  if (seg.empty()) return rest_error(404, "not_found", "no such route");
  if (seg[0] == "portfolio" && seg.size() >= 2) {
    if (seg[1] == "events" && seg.size() >= 3 && seg[2] == "orders") {
      if (seg.size() == 3) {
        if (r.method == "POST") return create_order(e, body, now, created);
        if (r.method == "DELETE") {
          std::optional<int> sub;
          RestReply err;
          if (!subaccount_of(r, &sub, &err)) return err;
          e.cancel_all(sub, now);  // "Omitting defaults to all subaccounts"
          return {204, ""};
        }
        return rest_error(405, "method_not_allowed", "POST or DELETE");
      }
      if (seg.size() == 4 && r.method == "DELETE")
        return seg[3] == "batched" ? cancel_batch(e, body, now) : cancel_one(e, r, seg[3], now);
      return rest_error(405, "method_not_allowed", "DELETE");
    }
    if (seg[1] == "orders" && r.method == "GET") {
      if (seg.size() == 2) return list_orders(e, r);
      if (seg.size() == 3) {
        const Order* o = e.order(seg[2]);
        if (!o) return rest_error(404, "not_found", "order not found");
        return ok(200, json{{"order", rest_order_json(*o, e.config().user_id)}});
      }
    }
    if (seg.size() == 2 && r.method == "GET") {
      if (seg[1] == "fills") return list_fills(e, r);
      if (seg[1] == "positions") return list_positions(e, r);
      if (seg[1] == "balance") return balance(e, r);
    }
    if (seg[1] == "order_groups") return groups_route(e, r, seg, body, now);
    if (seg[1] == "intra_exchange_instance_transfer" && seg.size() == 2 && r.method == "POST") {
      // the old desk's body (src/client.cpp transfer_between_shards): amount in centicents
      if (!body.is_object() || body.value("source", "") != "event_contract" ||
          body.value("destination", "") != "event_contract")
        return bad("source and destination must be event_contract");
      if (!body.contains("amount") || !body["amount"].is_number_integer() ||
          !body.contains("source_exchange_shard") || !body["source_exchange_shard"].is_number_integer() ||
          !body.contains("destination_exchange_shard") || !body["destination_exchange_shard"].is_number_integer())
        return bad("amount, source_exchange_shard and destination_exchange_shard are integers");
      std::string id;
      const Refusal f = e.transfer(0, body["source_exchange_shard"].get<int>(), body["destination_exchange_shard"].get<int>(),
                                   body["amount"].get<long long>() * 100, now, &id);
      if (f) return refused(f);
      return ok(200, json{{"transfer_id", id}});
    }
    return rest_error(404, "not_found", "no such route");
  }
  if (seg[0] == "markets" && r.method == "GET" && seg.size() >= 2) {
    const Market* m = e.market(seg[1]);
    if (!m) return rest_error(404, "not_found", "market not found");
    if (seg.size() == 2) return ok(200, json{{"market", market_json(e, *m)}});
    if (seg.size() == 3 && seg[2] == "orderbook") {
      bool b = false;
      const auto depth = qint(r, "depth", &b);
      if (b || (depth && (*depth < 0 || *depth > 100))) return bad("depth must be 0-100");
      // ascending, the best last: what the venue sent the old desk (src/types.cpp parse_levels)
      auto side = [&](bool no_side) {
        auto lv = e.levels(m->spec.ticker, no_side);
        if (depth && *depth > 0 && static_cast<long long>(lv.size()) > *depth) lv.erase(lv.begin(), lv.end() - *depth);
        json a = json::array();
        for (const auto& [px, q] : lv) a.push_back(json::array({px_str(px), qty_str(q)}));
        return a;
      };
      return ok(200, json{{"orderbook_fp", json{{"yes_dollars", side(false)}, {"no_dollars", side(true)}}}});
    }
  }
  if (seg[0] == "exchange" && seg.size() == 2 && seg[1] == "status" && r.method == "GET")
    return ok(200, json{{"exchange_active", true}, {"trading_active", true}});
  return rest_error(404, "not_found", "no such route");
}

}  // namespace fakex
