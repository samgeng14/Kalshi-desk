#include "deskd/gw/poly_wire.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>

#include "deskd/web/contract.h"

namespace deskd::poly {

using json = nlohmann::json;

namespace {

constexpr Micros kCent = 10000;
constexpr Micros kMicrosPerDollar = 1000000;
constexpr std::int64_t kMaxWhole = 1000000000;   // dollars or contracts: an arithmetic bound, not a limit

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool slug_ok(const std::string& s) {
  if (s.empty() || s.size() > 200) return false;
  for (unsigned char c : s)
    if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
  return true;
}
bool id_ok(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (unsigned char c : id)
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
  return true;
}

// A JSON number in units of 10^-dp, when it is one to within float noise: 0.385 at 6 places is
// 385000; 0.0000005 is not a whole micro-dollar.
std::optional<std::int64_t> scaled(const json& v, int dp) {
  double scale = 1;
  for (int k = 0; k < dp; ++k) scale *= 10;
  if (v.is_number_unsigned()) {
    const auto u = v.get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(kMaxWhole)) return std::nullopt;
    return static_cast<std::int64_t>(u) * static_cast<std::int64_t>(scale);
  }
  if (v.is_number_integer()) {
    const auto n = v.get<std::int64_t>();
    if (n > kMaxWhole || n < -kMaxWhole) return std::nullopt;
    return n * static_cast<std::int64_t>(scale);
  }
  if (!v.is_number_float()) return std::nullopt;
  const double d = v.get<double>() * scale;
  if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(kMaxWhole) * scale) return std::nullopt;
  const double r = std::round(d);
  if (std::fabs(d - r) > std::max(1e-3, std::fabs(d) * 1e-15)) return std::nullopt;
  return static_cast<std::int64_t>(r);
}

std::optional<std::int64_t> parse_ms(const std::string& s) { return kalshi::parse_time_ms(s); }

std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// ---- a JSON object read field by field; the first problem is kept --------------------------
struct Rd {
  const json& j;
  std::string where;
  std::string err;
  Rd(const json& o, std::string w) : j(o), where(std::move(w)) {
    if (!j.is_object()) err = where + ": not an object";
  }
  bool ok() const { return err.empty(); }
  void fail(const char* k, const std::string& w) {
    if (err.empty()) err = where + "." + k + ": " + w;
  }
  const json* at(const char* k) const {
    if (!j.is_object()) return nullptr;
    auto it = j.find(k);
    return it == j.end() || it->is_null() ? nullptr : &*it;
  }
  template <class T, class F>
  std::optional<T> opt(const char* k, F read, const char* what) {
    const json* v = at(k);
    if (!v) return std::nullopt;
    auto x = read(*v);
    if (!x) fail(k, std::string("not ") + what + ": " + v->dump().substr(0, 40));
    return x;
  }
  template <class T>
  T need(const char* k, std::optional<T> v) {
    if (!v && ok()) fail(k, "missing");
    return v.value_or(T{});
  }
  std::optional<std::string> ostr(const char* k) {
    return opt<std::string>(k, [](const json& v) { return v.is_string() ? std::optional<std::string>(v.get<std::string>()) : std::nullopt; },
                            "a string");
  }
  std::string str(const char* k) { return need(k, ostr(k)); }
  std::string str_or_empty(const char* k) { return ostr(k).value_or(""); }
  std::optional<bool> obool(const char* k) {
    return opt<bool>(k, [](const json& v) { return v.is_boolean() ? std::optional<bool>(v.get<bool>()) : std::nullopt; },
                     "a boolean");
  }
  bool boolean(const char* k) { return need(k, obool(k)); }
  std::optional<Micros> oamount(const char* k) { return opt<Micros>(k, parse_amount, "an amount"); }
  Micros amount(const char* k) { return need(k, oamount(k)); }
  std::optional<Px> opx(const char* k) { return opt<Px>(k, parse_px, "a price"); }
  Px px(const char* k) { return need(k, opx(k)); }
  std::optional<Qty> oqty(const char* k, bool neg = false) {
    return opt<Qty>(k, [neg](const json& v) { return parse_qty(v, neg); }, "a quantity");
  }
  Qty qty(const char* k, bool neg = false) { return need(k, oqty(k, neg)); }
  std::optional<std::int64_t> otime(const char* k) {
    return opt<std::int64_t>(k, [](const json& v) { return v.is_string() ? parse_ms(v.get<std::string>()) : std::nullopt; },
                             "an RFC 3339 time");
  }
  std::int64_t time(const char* k) { return need(k, otime(k)); }
  // a plain number, or one written as a decimal string ("0.06")
  std::optional<double> onum(const char* k) {
    return opt<double>(
        k,
        [](const json& v) -> std::optional<double> {
          double d = 0;
          if (v.is_number()) {
            d = v.get<double>();
          } else if (v.is_string()) {
            const auto& s = v.get_ref<const std::string&>();
            char* end = nullptr;
            d = std::strtod(s.c_str(), &end);
            if (s.empty() || end != s.c_str() + s.size()) return std::nullopt;
          } else {
            return std::nullopt;
          }
          return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
        },
        "a number");
  }
  // an array (null or absent: none); each element read by f into out, the first failure kept
  template <class T, class F>
  void each(const char* k, std::vector<T>* out, F f, bool required) {
    const json* v = at(k);
    if (!v) {
      if (required && ok() && !(j.is_object() && j.contains(k))) fail(k, "missing");
      return;
    }
    if (!v->is_array()) {
      fail(k, "not an array");
      return;
    }
    for (const auto& e : *v) {
      Rd x(e, where + "." + k + "[" + std::to_string(out->size()) + "]");
      T row = f(x);
      if (!x.ok()) {
        if (ok()) err = x.err;
        return;
      }
      out->push_back(std::move(row));
    }
  }
};

std::optional<json> parse_object(const std::string& body, std::string* why) {
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded()) {
    if (why) *why = "not JSON";
    return std::nullopt;
  }
  if (!j.is_object()) {
    if (why) *why = "not a JSON object";
    return std::nullopt;
  }
  return j;
}

template <class T>
std::optional<T> done(const Rd& r, T value, std::string* why) {
  if (!r.ok()) {
    if (why) *why = r.err;
    return std::nullopt;
  }
  return value;
}

std::optional<OrderState> order_state_of(const std::string& s) {
  static const std::pair<const char*, OrderState> k[] = {
      {"ORDER_STATE_PENDING_NEW", OrderState::PENDING_NEW},     {"ORDER_STATE_NEW", OrderState::NEW},
      {"ORDER_STATE_PARTIALLY_FILLED", OrderState::PARTIALLY_FILLED}, {"ORDER_STATE_FILLED", OrderState::FILLED},
      {"ORDER_STATE_CANCELED", OrderState::CANCELED},           {"ORDER_STATE_REJECTED", OrderState::REJECTED},
      {"ORDER_STATE_EXPIRED", OrderState::EXPIRED},             {"ORDER_STATE_PENDING_CANCEL", OrderState::PENDING_CANCEL}};
  for (const auto& [name, st] : k)
    if (s == name) return st;
  return std::nullopt;
}
std::optional<PmTif> tif_of(const std::string& s) {
  for (PmTif t : {PmTif::IOC, PmTif::FOK, PmTif::GTC, PmTif::GTD})
    if (s == tif_wire(t)) return t;
  return std::nullopt;
}

Market read_market(Rd& r) {
  Market m;
  m.slug = r.str("slug");
  if (r.ok() && !slug_ok(m.slug)) r.fail("slug", "not a slug");
  m.question = r.ostr("question").value_or(r.str_or_empty("title"));
  m.sports_type = r.str_or_empty("sportsMarketType");
  m.tick = r.opx("orderPriceMinTickSize");
  if (m.tick && (*m.tick <= 0 || *m.tick >= kOneDollar)) r.fail("orderPriceMinTickSize", "not inside (0, 1)");
  m.min_qty = r.oqty("minimumTradeQty");
  if (m.min_qty && *m.min_qty <= 0) r.fail("minimumTradeQty", "not above 0");
  m.fee_coeff = r.onum("feeCoefficient");
  if (m.fee_coeff && (*m.fee_coeff < 0 || *m.fee_coeff > 1)) r.fail("feeCoefficient", "not within [0, 1]");
  m.bid = r.opx("bestBidQuote");
  m.ask = r.opx("bestAskQuote");
  m.start_ms = r.otime("gameStartTime");
  if (!m.start_ms) m.start_ms = r.otime("startDate");
  m.end_ms = r.otime("endDate");
  m.updated_ms = r.otime("updatedAt");
  m.active = r.obool("active").value_or(false);
  m.closed = r.obool("closed").value_or(false);
  r.each("marketSides", &m.sides,
         [](Rd& x) {
           SideInfo s;
           s.description = x.str_or_empty("description");
           s.is_long = x.obool("long").value_or(false);
           if (const json* t = x.at("team")) {
             Rd team(*t, x.where + ".team");
             s.abbr = lower(team.str_or_empty("abbreviation"));
             s.name = team.str_or_empty("name");
             s.safe_name = team.str_or_empty("safeName");
             s.alias = team.str_or_empty("alias");
             if (!team.ok() && x.ok()) x.err = team.err;
           }
           return s;
         },
         false);
  std::stable_sort(m.sides.begin(), m.sides.end(), [](const SideInfo& a, const SideInfo& b) { return a.is_long && !b.is_long; });
  return m;
}

Order read_order(Rd& r) {
  Order o;
  o.id = r.str("id");
  if (r.ok() && !id_ok(o.id)) r.fail("id", "not an id");
  o.slug = r.str("marketSlug");
  const std::string side = r.str("side");
  if (r.ok()) {
    if (side == "ORDER_SIDE_BUY") o.action = Action::BUY;
    else if (side == "ORDER_SIDE_SELL") o.action = Action::SELL;
    else r.fail("side", "not ORDER_SIDE_BUY or _SELL");
  }
  const std::string tif = r.str("tif");
  if (r.ok()) {
    if (auto t = tif_of(tif)) o.tif = *t;
    else r.fail("tif", "unknown \"" + tif.substr(0, 40) + "\"");
  }
  const std::string state = r.str("state");
  if (r.ok()) {
    if (auto s = order_state_of(state)) o.state = *s;
    else r.fail("state", "unknown \"" + state.substr(0, 40) + "\"");
  }
  o.price = r.px("price");
  o.quantity = r.qty("quantity");
  o.cum = r.qty("cumQuantity");
  o.leaves = r.qty("leavesQuantity");
  o.avg_px = r.oamount("avgPx");
  o.commission = r.amount("commissionNotionalTotalCollected");
  o.created_ms = r.otime("createTime");
  o.updated_ms = r.otime("updateTime");
  if (!r.ok()) return o;
  // src/poly.cpp reconcile_pm_leg's identity checks: what no order can hold
  if (o.price <= 0 || o.price >= kOneDollar) r.fail("price", "not inside (0, 1)");
  else if (o.quantity <= 0) r.fail("quantity", "not above 0");
  else if (o.cum > o.quantity) r.fail("cumQuantity", "above quantity");
  else if (o.leaves > o.quantity - o.cum) r.fail("leavesQuantity", "above quantity - cumQuantity");
  else if (o.cum > 0 && !o.avg_px) r.fail("avgPx", "missing with cumQuantity above 0");
  else if (o.cum > 0 && (*o.avg_px <= 0 || *o.avg_px >= kMicrosPerDollar)) r.fail("avgPx", "not inside (0, 1)");
  if (o.cum == 0) o.avg_px.reset();   // the old mock's {"value":"0"} before a fill
  return o;
}

Trade read_trade(Rd& r) {
  Trade t;
  t.id = r.str("id");
  if (r.ok() && t.id.empty()) r.fail("id", "empty");
  t.slug = r.str("marketSlug");
  t.price = r.px("price");
  t.qty = r.at("qtyDecimal") ? r.qty("qtyDecimal") : r.qty("qty");
  t.aggressor = r.boolean("isAggressor");
  t.cost_basis = r.oamount("costBasis");
  t.realized = r.oamount("realizedPnl");
  t.ts_ms = r.time("createTime");
  if (r.ok() && (t.price <= 0 || t.price >= kOneDollar)) r.fail("price", "not inside (0, 1)");
  if (r.ok() && t.qty <= 0) r.fail("qtyDecimal", "not above 0");
  return t;
}

Activity read_activity(Rd& r) {
  Activity a;
  a.type = r.str("type");
  if (!r.ok()) return a;
  if (a.type == "ACTIVITY_TYPE_TRADE") {
    a.kind = Activity::Kind::TRADE;
    const json* t = r.at("trade");
    if (!t) {
      r.fail("trade", "missing");
      return a;
    }
    Rd x(*t, r.where + ".trade");
    a.trade = read_trade(x);
    a.ts_ms = a.trade.ts_ms;
    if (!x.ok()) r.err = x.err;
    return a;
  }
  const bool in = a.type == "ACTIVITY_TYPE_ACCOUNT_DEPOSIT" || a.type == "ACTIVITY_TYPE_ACCOUNT_ADVANCED_DEPOSIT";
  if (!in && a.type != "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL") return a;   // OTHER, its type kept
  a.kind = in ? Activity::Kind::DEPOSIT : Activity::Kind::WITHDRAWAL;
  const json* c = r.at("accountBalanceChange");
  if (!c) {
    r.fail("accountBalanceChange", "missing");
    return a;
  }
  Rd x(*c, r.where + ".accountBalanceChange");
  a.id = x.str("transactionId");
  const Micros amount = x.amount("amount");
  a.amount = in ? std::abs(amount) : -std::abs(amount);
  a.ts_ms = x.at("updateTime") ? x.time("updateTime") : x.time("createTime");
  if (!x.ok()) r.err = x.err;
  return a;
}

// nextCursor and eof: the cursor "" on the last page; a page that is not the last names the next
std::string read_cursor(Rd& r) {
  const std::string next = r.str_or_empty("nextCursor");
  const std::optional<bool> eof = r.obool("eof");
  if (eof && *eof) return "";
  if (eof && next.empty() && r.ok()) r.fail("nextCursor", "empty on a page that is not the last");
  return next;
}

void add_q(std::string& q, const char* k, const std::string& v) {
  if (v.empty()) return;
  if (!q.empty()) q += '&';
  q += k;
  q += '=';
  q += kalshi::url_encode(v);
}

WireRequest request(const char* method, std::string path, std::string query = "", Host host = Host::API) {
  WireRequest r;
  r.method = method;
  r.path = std::move(path);
  r.query = std::move(query);
  r.host = host;
  r.auth = host == Host::API;
  return r;
}

json create_body(const CreateOrder& o) {
  json b{{"marketSlug", o.slug},
         {"type", "ORDER_TYPE_LIMIT"},
         {"price", {{"value", price_wire(o.price)}, {"currency", "USD"}}},
         {"quantity", o.count / kOneContract},
         {"tif", tif_wire(o.tif)},
         {"outcomeSide", "OUTCOME_SIDE_YES"},
         {"action", action_wire(o.action)},
         {"manualOrderIndicator", o.manual ? "MANUAL_ORDER_INDICATOR_MANUAL" : "MANUAL_ORDER_INDICATOR_AUTOMATIC"},
         {"synchronousExecution", !o.asynchronous}};
  if (!o.asynchronous) b["maxBlockTime"] = "1";   // the open question's ceiling, were it ever needed
  if (o.post_only) b["participateDontInitiate"] = true;
  if (o.tif == PmTif::GTD) b["expireTime"] = time_wire(o.good_till_ms);
  return b;
}

// What is wrong with the order itself, or ""
std::string shape(const CreateOrder& o) {
  if (!slug_ok(o.slug)) return "a bad market slug \"" + o.slug.substr(0, 40) + "\"";
  if (o.count <= 0 || o.count % kOneContract != 0) return "Polymarket takes whole contracts only";
  if (o.count > kMaxWhole * kOneContract) return "the size is past any order's";
  if (o.price <= 0 || o.price >= kOneDollar) return "the price must be strictly between 0 and 1";
  if (o.tif == PmTif::GTC) return "no GTC: every order lives at most 60 s (plan section 3)";
  if (o.post_only && o.tif != PmTif::GTD) return "post-only (participateDontInitiate) only with GTD";
  if (o.tif == PmTif::GTD && (o.good_till_ms <= 0 || o.good_till_ms % 1000 != 0))
    return "a GTD order expires at a whole second";
  if (o.tif != PmTif::GTD && o.good_till_ms != 0) return "an expiry only with GTD";
  return "";
}

// 0 Sunday .. 6 Saturday, of an ET calendar date "2026-11-05"
int weekday(const std::string& date) {
  const auto t = parse_ms(date + "T00:00:00Z");
  if (!t) return -1;
  return static_cast<int>(((floor_div(*t, 86400000) % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday
}

}  // namespace

// ---- values ------------------------------------------------------------------------------------

std::string price_wire(Px p) {
  char b[24];
  const long long whole = p / 10000, frac = p % 10000;
  if (frac % 10 == 0) std::snprintf(b, sizeof b, "%lld.%03lld", whole, frac / 10);
  else std::snprintf(b, sizeof b, "%lld.%04lld", whole, frac);
  return b;
}

std::optional<Micros> parse_amount(const json& v) {
  if (v.is_object()) {
    auto c = v.find("currency");
    if (c != v.end() && !c->is_null() && !(c->is_string() && c->get<std::string>() == "USD")) return std::nullopt;
    auto it = v.find("value");
    if (it == v.end() || it->is_object()) return std::nullopt;
    return parse_amount(*it);
  }
  if (v.is_string()) return kalshi::parse_dollars(v.get<std::string>());
  return scaled(v, 6);
}

std::optional<Px> parse_px(const json& v) {
  const auto m = parse_amount(v);
  if (!m || *m % 100 != 0 || *m < 0 || *m > kMicrosPerDollar) return std::nullopt;
  return *m / 100;
}

std::optional<Qty> parse_qty(const json& v, bool allow_negative) {
  std::optional<Qty> q = v.is_string() ? kalshi::parse_count(v.get<std::string>(), allow_negative) : scaled(v, 2);
  if (!q || (*q < 0 && !allow_negative) || *q > kMaxWhole * kOneContract || *q < -kMaxWhole * kOneContract)
    return std::nullopt;
  return q;
}

std::string time_wire(std::int64_t ms) {
  const std::time_t s = static_cast<std::time_t>(floor_div(ms, 1000));
  std::tm tm{};
  gmtime_r(&s, &tm);
  char b[32];
  const std::size_t n = std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(b, n);
}

const char* action_wire(Action a) { return a == Action::BUY ? "ORDER_ACTION_BUY" : "ORDER_ACTION_SELL"; }
const char* tif_wire(PmTif t) {
  switch (t) {
    case PmTif::IOC: return "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL";
    case PmTif::FOK: return "TIME_IN_FORCE_FILL_OR_KILL";
    case PmTif::GTC: return "TIME_IN_FORCE_GOOD_TILL_CANCEL";
    case PmTif::GTD: return "TIME_IN_FORCE_GOOD_TILL_DATE";
  }
  return "?";
}
const char* order_state_name(OrderState s) {
  switch (s) {
    case OrderState::PENDING_NEW: return "pending_new";
    case OrderState::NEW: return "new";
    case OrderState::PARTIALLY_FILLED: return "partially_filled";
    case OrderState::FILLED: return "filled";
    case OrderState::CANCELED: return "canceled";
    case OrderState::REJECTED: return "rejected";
    case OrderState::EXPIRED: return "expired";
    case OrderState::PENDING_CANCEL: return "pending_cancel";
  }
  return "?";
}
bool order_open(OrderState s) {
  return s == OrderState::PENDING_NEW || s == OrderState::NEW || s == OrderState::PARTIALLY_FILLED ||
         s == OrderState::PENDING_CANCEL;
}

// ---- requests ----------------------------------------------------------------------------------

CreateOrder from_intent(const PlaceIntent& in, std::int64_t now_ms, std::int64_t gtd_min_lead_ms) {
  CreateOrder o;
  o.slug = in.ticker;
  o.action = in.side == Side::BUY ? Action::BUY : Action::SELL;
  o.price = in.price;
  o.count = in.count;
  o.manual = in.owner == Owner::MAN;
  switch (in.tif) {
    case Tif::IOC: o.tif = PmTif::IOC; break;
    case Tif::FOK: o.tif = PmTif::FOK; break;
    case Tif::POST: {
      o.tif = PmTif::GTD;
      o.post_only = true;
      const std::int64_t asked = in.exp_ms > 0 ? floor_div(in.exp_ms, 1000) * 1000 : (floor_div(now_ms, 1000) + in.ttl_s) * 1000;
      const std::int64_t earliest = (floor_div(now_ms + std::max<std::int64_t>(0, gtd_min_lead_ms), 1000) + 1) * 1000;
      o.good_till_ms = std::max(asked, earliest);
      break;
    }
  }
  return o;
}

std::string check_create(const CreateOrder& o, Px tick, Qty min_qty, std::int64_t now_ms) {
  const std::string bad = shape(o);
  if (!bad.empty()) return bad;
  if (tick <= 0 || tick >= kOneDollar) return "the market's tick is not known";
  if (o.price % tick != 0) return "the price " + price_wire(o.price) + " is off the market's " + price_wire(tick) + " grid";
  if (o.count < min_qty) return "under the market's minimum of " + std::to_string(min_qty / kOneContract) + " contracts";
  if (o.tif == PmTif::GTD && o.good_till_ms <= now_ms) return "the GTD expiry has passed";
  return "";
}

bool build_create(const CreateOrder& o, WireRequest* out, std::string* why) {
  const std::string bad = shape(o);
  if (!bad.empty()) {
    if (why) *why = bad;
    return false;
  }
  WireRequest r = request("POST", "/v1/orders");
  r.body = create_body(o).dump();
  r.write = true;
  r.op = WireOp::CREATE;
  *out = std::move(r);
  return true;
}

bool build_cancel(const std::string& order_id, const std::string& slug, WireRequest* out, std::string* why) {
  if (!id_ok(order_id) || !slug_ok(slug)) {
    if (why) *why = "a cancel names an order id and its market's slug";
    return false;
  }
  WireRequest r = request("POST", "/v1/order/" + order_id + "/cancel");
  r.body = json{{"marketSlug", slug}}.dump();
  r.write = true;
  r.op = WireOp::CANCEL;
  *out = std::move(r);
  return true;
}

WireRequest build_cancel_all() {
  WireRequest r = request("POST", "/v1/orders/open/cancel");
  r.body = "{}";
  r.write = true;
  r.op = WireOp::CANCEL_ALL;
  return r;
}

bool build_preview(const CreateOrder& o, WireRequest* out, std::string* why) {
  const std::string bad = shape(o);
  if (!bad.empty()) {
    if (why) *why = bad;
    return false;
  }
  WireRequest r = request("POST", "/v1/order/preview");
  r.body = json{{"request", create_body(o)}}.dump();
  *out = std::move(r);
  return true;
}

WireRequest build_get_order(const std::string& order_id) { return request("GET", "/v1/order/" + kalshi::url_encode(order_id)); }
WireRequest build_get_open_orders() { return request("GET", "/v1/orders/open"); }
WireRequest build_get_activities(const std::string& cursor, int limit) {
  std::string q;
  if (limit > 0) add_q(q, "limit", std::to_string(limit));
  add_q(q, "cursor", cursor);
  return request("GET", "/v1/portfolio/activities", q);
}
WireRequest build_get_positions(const std::string& cursor, int limit) {
  std::string q;
  if (limit > 0) add_q(q, "limit", std::to_string(limit));
  add_q(q, "cursor", cursor);
  return request("GET", "/v1/portfolio/positions", q);
}
WireRequest build_get_balances() { return request("GET", "/v1/account/balances"); }

bool build_get_markets(const std::vector<std::string>& slugs, WireRequest* out, std::string* why) {
  if (slugs.empty() || slugs.size() > kMarketsPerRequest) {
    if (why) *why = "a markets read names 1 to " + std::to_string(kMarketsPerRequest) + " slugs";
    return false;
  }
  std::string q;
  for (const auto& s : slugs) {
    if (!slug_ok(s)) {
      if (why) *why = "a bad market slug \"" + s.substr(0, 40) + "\"";
      return false;
    }
    add_q(q, "slug", s);
  }
  *out = request("GET", "/v1/markets", q, Host::GATEWAY);
  return true;
}
WireRequest build_get_book(const std::string& slug) {
  return request("GET", "/v1/markets/" + kalshi::url_encode(slug) + "/book", "", Host::GATEWAY);
}
WireRequest build_get_league_events(const std::string& league, int offset, int limit) {
  std::string q;
  if (limit > 0) add_q(q, "limit", std::to_string(limit));
  if (offset > 0) add_q(q, "offset", std::to_string(offset));
  return request("GET", "/v2/leagues/" + kalshi::url_encode(league) + "/events", q, Host::GATEWAY);
}

std::string request_bytes(const WireRequest& r, const std::string& host_header, const net::HeaderList& auth_headers,
                          const std::string& user_agent) {
  net::HeaderList h;
  h.reserve(auth_headers.size() + 3);
  for (const auto& kv : auth_headers) h.push_back(kv);
  h.emplace_back("Accept", "application/json");
  h.emplace_back("User-Agent", user_agent);
  if (!r.body.empty()) h.emplace_back("Content-Type", "application/json");
  return net::request_bytes(r.method, host_header, r.target(), h, r.body, !r.write);
}

// ---- replies -----------------------------------------------------------------------------------

std::optional<LeaguePage> parse_league_events(const std::string& body, const std::string& league, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "league events");
  LeaguePage page;
  std::vector<int> events;   // read for its count; each event's markets go into page
  r.each("events", &events,
         [&](Rd& e) {
           const std::string slug = e.str("slug"), title = e.str_or_empty("title");
           std::vector<Market> ms;
           e.each("markets", &ms, read_market, false);
           for (auto& m : ms) {
             m.event_slug = slug;
             m.event_title = title;
             m.league = league;
             page.markets.push_back(std::move(m));
           }
           return 0;
         },
         true);
  page.events = events.size();
  return done(r, std::move(page), why);
}

std::optional<std::vector<Market>> parse_markets(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "markets");
  std::vector<Market> out;
  r.each("markets", &out, read_market, true);
  return done(r, std::move(out), why);
}

std::optional<Book> parse_book(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "book");
  Book b;
  const json* md = r.at("marketData");
  if (!md) {
    r.fail("marketData", "missing");
    return done(r, b, why);
  }
  Rd d(*md, "book.marketData");
  auto level = [](Rd& x) {
    const Px px = x.px("px");
    const Qty q = x.qty("qty");
    if (x.ok() && (px <= 0 || px >= kOneDollar)) x.fail("px", "not inside (0, 1)");
    if (x.ok() && q <= 0) x.fail("qty", "not above 0");
    return std::make_pair(px, q);
  };
  d.each("bids", &b.bids, level, false);
  d.each("asks", &b.asks, level, false);
  for (std::size_t i = 1; d.ok() && i < b.bids.size(); ++i)
    if (b.bids[i].first >= b.bids[i - 1].first) d.fail("bids", "not best first");
  for (std::size_t i = 1; d.ok() && i < b.asks.size(); ++i)
    if (b.asks[i].first <= b.asks[i - 1].first) d.fail("asks", "not best first");
  return done(d, std::move(b), why);
}

std::optional<Order> parse_order(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd top(*j, "order");
  const json* o = top.at("order");
  if (!o) {
    top.fail("order", "missing");
    return done(top, Order{}, why);
  }
  Rd r(*o, "order");
  Order v = read_order(r);
  return done(r, std::move(v), why);
}

std::optional<std::vector<Order>> parse_open_orders(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "open orders");
  std::vector<Order> out;
  r.each("orders", &out, read_order, true);
  return done(r, std::move(out), why);
}

std::optional<kalshi::Page<Activity>> parse_activities(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "activities");
  kalshi::Page<Activity> page;
  r.each("activities", &page.rows, read_activity, true);
  page.cursor = read_cursor(r);
  return done(r, std::move(page), why);
}

std::optional<kalshi::Page<Position>> parse_positions(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "positions");
  kalshi::Page<Position> page;
  if (const json* ps = r.at("positions")) {
    if (!ps->is_object()) r.fail("positions", "not an object keyed by slug");
    for (auto it = ps->begin(); r.ok() && ps->is_object() && it != ps->end(); ++it) {
      Rd x(it.value(), "positions." + it.key());
      Position p;
      p.slug = it.key();
      if (!slug_ok(p.slug)) x.fail("slug", "not a slug");
      p.net = x.at("netPositionDecimal") ? x.qty("netPositionDecimal", true) : x.qty("netPosition", true);
      p.cost = x.amount("cost");
      p.realized = x.oamount("realized");
      p.cash_value = x.oamount("cashValue");
      p.available = x.oqty("qtyAvailableDecimal");
      p.expired = x.obool("expired");
      p.updated_ms = x.otime("updateTime");
      if (!x.ok()) r.err = x.err;
      else page.rows.push_back(std::move(p));
    }
  } else if (!j->contains("positions")) {
    r.fail("positions", "missing");
  }
  page.cursor = read_cursor(r);
  return done(r, std::move(page), why);
}

std::optional<Balance> parse_balance(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "balances");
  Balance b;
  const json* rows = r.at("balances");
  if (!rows || !rows->is_array()) {
    r.fail("balances", "missing or not an array");
    return done(r, b, why);
  }
  bool found = false;
  for (const auto& row : *rows) {
    if (!row.is_object() || !row.contains("currency") || row["currency"] != "USD") continue;
    Rd x(row, "balances[USD]");
    b.current = x.amount("currentBalance");
    b.buying_power = x.amount("buyingPower");
    b.asset_notional = x.oamount("assetNotional");
    b.asset_available = x.oamount("assetAvailable");
    if (!x.ok()) r.err = x.err;
    found = true;
    break;
  }
  if (!found) r.fail("balances", "no USD row");
  return done(r, b, why);
}

std::optional<CreateAck> parse_create_ack(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "create");
  CreateAck a;
  a.order_id = r.str("id");
  if (r.ok() && !id_ok(a.order_id)) r.fail("id", "not an id");
  r.each("executions", &a.executions,
         [](Rd& x) {
           Execution e;
           e.trade_id = x.str("tradeId");
           if (x.ok() && e.trade_id.empty()) x.fail("tradeId", "empty");
           e.qty = x.qty("lastShares");
           e.price = x.px("lastPx");
           e.fee = x.oamount("commissionNotionalCollected");
           if (const json* o = x.at("order")) {
             Rd s(*o, x.where + ".order");
             if (auto st = s.ostr("state")) {
               e.state = order_state_of(*st);
               if (!e.state) s.fail("state", "unknown \"" + st->substr(0, 40) + "\"");
             }
             if (!s.ok() && x.ok()) x.err = s.err;
           }
           if (x.ok() && e.qty <= 0) x.fail("lastShares", "not above 0");
           if (x.ok() && (e.price <= 0 || e.price >= kOneDollar)) x.fail("lastPx", "not inside (0, 1)");
           return e;
         },
         false);
  return done(r, std::move(a), why);
}

std::optional<std::vector<std::string>> parse_cancel_all(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "cancel all");
  std::vector<std::string> ids;
  const json* v = r.at("canceledOrderIds");
  if (!v && !j->contains("canceledOrderIds")) r.fail("canceledOrderIds", "missing");
  else if (v && !v->is_array()) r.fail("canceledOrderIds", "not an array");
  else if (v)
    for (const auto& id : *v) {
      if (!id.is_string() || !id_ok(id.get<std::string>())) {
        r.fail("canceledOrderIds", "holds something that is not an order id");
        break;
      }
      ids.push_back(id.get<std::string>());
    }
  return done(r, std::move(ids), why);
}

// ---- the ledger's view -------------------------------------------------------------------------

kalshi::OrderRow to_ledger(const Order& o) {
  kalshi::OrderRow row;
  row.order_id = o.id;
  row.ticker = o.slug;
  row.side = o.action == Action::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK;
  const bool open = order_open(o.state);
  row.status = open ? kalshi::OrderStatus::RESTING
               : o.state == OrderState::FILLED ? kalshi::OrderStatus::EXECUTED
                                               : kalshi::OrderStatus::CANCELED;
  row.yes_price = o.price;
  row.filled = o.cum;
  row.remaining = open ? o.leaves : 0;
  row.initial = o.quantity;
  row.taker_fees = o.commission;
  if (o.avg_px && o.cum > 0) {
    // micro-dollars a contract x 1/100 contracts / 100, to the nearest micro-dollar
    const Micros per = o.action == Action::BUY ? *o.avg_px : kMicrosPerDollar - *o.avg_px;
    row.taker_cost = (per * o.cum + kOneContract / 2) / kOneContract;
  }
  row.created_ms = o.created_ms;
  row.updated_ms = o.updated_ms;
  row.exchange_index = kLedgerShard;
  return row;
}

kalshi::FillRow to_ledger(const Trade& t, const std::string& order_id, Action action, double coeff) {
  kalshi::FillRow f;
  f.fill_id = t.id;
  f.trade_id = t.id;
  f.order_id = order_id;
  f.ticker = t.slug;
  f.side = action == Action::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK;
  f.count = t.qty;
  f.yes_price = t.price;
  f.taker = t.aggressor;
  f.fee = t.aggressor ? poly_taker_fee_fill(t.price, t.qty, coeff) : poly_maker_rebate_fill(t.price, t.qty);
  f.ts_ms = t.ts_ms;
  f.exchange_index = kLedgerShard;
  return f;
}

std::string end_why(const Order& o) {
  if (o.state == OrderState::REJECTED) return "EXCHANGE";
  if (o.state == OrderState::EXPIRED) return "EXPIRY";
  return "";
}

// ---- fees -------------------------------------------------------------------------------------

Micros taker_fee_allowance(Side s, Px limit, Qty q, double coeff) {
  if (q <= 0 || !(coeff > 0)) return 0;
  // p(1 - p) rises to 0.50: a BUY fills at or under its limit, a SELL at or over it
  const Px half = kOneDollar / 2;
  const Px worst = s == Side::BUY ? std::min(limit, half) : std::max(limit, half);
  // n fills each rounding up to half a cent past their exact fee, against the whole order's exact
  // fee rounded once (which may have rounded down half a cent): at most (n + 1) / 2 whole cents
  const Qty fills = (q + kOneContract - 1) / kOneContract;
  return poly_taker_fee_fill(worst, q, coeff) + (fills + 1) / 2 * kCent;
}

// ---- what a write's reply means ----------------------------------------------------------------

kalshi::ApiError parse_error(int status, const std::string& body) {
  kalshi::ApiError e;
  e.http = status;
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return e;
  auto code = j.find("code");
  auto msg = j.find("message");
  if (code == j.end() || !code->is_number_integer() || msg == j.end() || !msg->is_string()) return e;
  e.code = std::to_string(code->get<std::int64_t>());
  e.message = msg->get<std::string>();
  auto details = j.find("details");
  if (details != j.end() && details->is_string()) e.details = details->get<std::string>();
  e.venue_body = true;
  return e;
}

kalshi::WriteOutcome classify_write(WireOp op, const net::Exchange& x) {
  kalshi::WriteOutcome w;
  using K = kalshi::WriteOutcome::Kind;
  if (x.fate == net::Exchange::Fate::NOT_SENT) {
    w.kind = K::NOT_SENT;
    w.why = x.why;
    return w;
  }
  if (x.fate == net::Exchange::Fate::UNKNOWN) {
    w.kind = K::UNKNOWN;
    w.why = x.why;
    return w;
  }
  const int st = x.res.status;
  w.http = st;
  if (st >= 200 && st < 300) {
    std::string why;
    bool parsed = true;
    switch (op) {
      case WireOp::CREATE: parsed = parse_create_ack(x.res.body, &why).has_value(); break;
      case WireOp::CANCEL: parsed = parse_object(x.res.body, &why).has_value(); break;
      case WireOp::CANCEL_ALL: parsed = parse_cancel_all(x.res.body, &why).has_value(); break;
      default: break;
    }
    if (parsed) {
      w.kind = K::ACK;
    } else {
      w.kind = K::UNKNOWN;
      w.why = "HTTP " + std::to_string(st) + " the venue took, with a body deskd cannot read" +
              (why.empty() ? std::string() : ": " + why);
    }
    return w;
  }
  if (st >= 500 || st == 408 || st < 200 || (st >= 300 && st < 400)) {
    w.kind = K::UNKNOWN;
    w.why = "HTTP " + std::to_string(st) + " after the write";
    return w;
  }
  // 4xx: a refusal only when the venue itself said so
  const kalshi::ApiError e = parse_error(st, x.res.body);
  if (!e.venue_body && st != 429) {
    w.kind = K::UNKNOWN;
    w.why = "HTTP " + std::to_string(st) + " without the venue's error body (something in between answered)";
    return w;
  }
  w.kind = K::REFUSED;
  w.error = e;
  w.cls = poly::classify_refusal(op, st, e.text());   // WireOp is deskd's: name ours, not reject.h's
  w.why = e.text();
  return w;
}

RejectClass classify_refusal(WireOp op, int status, const std::string& text) {
  const std::string b = lower(text);
  auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
  if (status == 429) return RejectClass::RETRYABLE;
  if (status == 401 || status == 403) return RejectClass::AUTH;
  if (op == WireOp::CANCEL && status == 404) return RejectClass::GONE;
  if (is_post_only_cross(b)) return RejectClass::SOFT_MARKET;
  if (has("market closed") || has("market_closed") || has("market is closed") || has("closed to new orders") ||
      has("market not open") || has("market_not_open") || has("market is not open") || has("trading is paused") ||
      has("trading_paused") || has("trading halted") || has("exchange closed") || has("exchange_closed"))
    return RejectClass::SOFT_MARKET;
  if (has("insufficient_balance") || has("insufficient balance") || has("insufficient funds") ||
      has("insufficient_funds") || has("insufficient collateral") || has("insufficient buying power") ||
      has("insufficient_buying_power"))
    return RejectClass::SOFT_COLLATERAL;
  if (status == 400 || status == 404 || status == 422 || has("invalid")) return RejectClass::HARD_INVALID;
  return RejectClass::UNKNOWN;
}

// ---- maintenance -------------------------------------------------------------------------------

Window maintenance_window(std::int64_t now_ms) {
  // DST changes on a Sunday, so a Thursday keeps one offset all day: its window is its own ET
  // midnight + 2 h to + 6 h
  std::string date = et_date(now_ms);
  for (int i = 0; i < 8; ++i, date = et_date_after(date)) {
    if (weekday(date) != 4) continue;
    const auto noon = parse_ms(date + "T17:00:00Z");   // 12:00 or 13:00 ET that day
    if (!noon) break;
    const std::int64_t midnight = et_midnight_ms(*noon);
    const Window w{midnight + 2 * 3600000, midnight + 6 * 3600000};
    if (w.to_ms > now_ms) return w;
  }
  return Window{};
}

bool in_maintenance(std::int64_t now_ms) {
  const Window w = maintenance_window(now_ms);
  return now_ms >= w.from_ms && now_ms < w.to_ms;
}

}  // namespace deskd::poly
