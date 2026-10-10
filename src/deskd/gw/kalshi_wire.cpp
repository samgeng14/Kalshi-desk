#include "deskd/gw/kalshi_wire.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace deskd::kalshi {

using json = nlohmann::json;

namespace {

bool digits(const std::string& s, std::size_t a, std::size_t b) {
  if (a >= b) return false;
  for (std::size_t i = a; i < b; ++i)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

// sign, whole part, up to max_dp decimals -> value * 10^scale_dp; nullopt on anything else
std::optional<std::int64_t> parse_fixed(const std::string& s, int max_dp, int scale_dp, bool allow_negative) {
  if (s.empty() || s.size() > 32) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    if (!allow_negative) return std::nullopt;
    neg = true;
    i = 1;
  }
  const std::size_t dot = s.find('.', i);
  const std::size_t int_end = dot == std::string::npos ? s.size() : dot;
  if (!digits(s, i, int_end) || int_end - i > 12) return std::nullopt;
  std::int64_t whole = 0;
  for (std::size_t k = i; k < int_end; ++k) whole = whole * 10 + (s[k] - '0');
  std::int64_t frac = 0;
  int dp = 0;
  if (dot != std::string::npos) {
    if (!digits(s, dot + 1, s.size())) return std::nullopt;
    dp = static_cast<int>(s.size() - dot - 1);
    if (dp > max_dp) return std::nullopt;
    for (std::size_t k = dot + 1; k < s.size(); ++k) frac = frac * 10 + (s[k] - '0');
  }
  std::int64_t scale = 1;
  for (int k = 0; k < scale_dp; ++k) scale *= 10;
  for (int k = dp; k < scale_dp; ++k) frac *= 10;
  const std::int64_t v = whole * scale + frac;
  if (neg && v == 0) return std::int64_t{0};
  return neg ? -v : v;
}

// days since 1970-01-01 for a civil date (H. Hinnant's days_from_civil)
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

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
  std::optional<std::string> ostr(const char* k) {
    const json* v = at(k);
    if (!v) return std::nullopt;
    if (!v->is_string()) {
      fail(k, "not a string");
      return std::nullopt;
    }
    return v->get<std::string>();
  }
  std::string str(const char* k) {
    auto v = ostr(k);
    if (!v && ok()) fail(k, "missing");
    return v.value_or("");
  }
  std::string str_or_empty(const char* k) { return ostr(k).value_or(""); }
  std::optional<std::int64_t> oint(const char* k) {
    const json* v = at(k);
    if (!v) return std::nullopt;
    if (!v->is_number_integer()) {
      fail(k, "not an integer");
      return std::nullopt;
    }
    return v->get<std::int64_t>();
  }
  std::int64_t integer(const char* k) {
    auto v = oint(k);
    if (!v && ok()) fail(k, "missing");
    return v.value_or(0);
  }
  std::optional<bool> obool(const char* k) {
    const json* v = at(k);
    if (!v) return std::nullopt;
    if (!v->is_boolean()) {
      fail(k, "not a boolean");
      return std::nullopt;
    }
    return v->get<bool>();
  }
  bool boolean(const char* k) {
    auto v = obool(k);
    if (!v && ok()) fail(k, "missing");
    return v.value_or(false);
  }
  template <class T, class F>
  std::optional<T> oparsed(const char* k, F parse, const char* what) {
    auto s = ostr(k);
    if (!s) return std::nullopt;
    auto v = parse(*s);
    if (!v) fail(k, std::string("not ") + what + ": \"" + s->substr(0, 40) + "\"");
    return v;
  }
  template <class T, class F>
  T parsed(const char* k, F parse, const char* what) {
    auto v = oparsed<T>(k, parse, what);
    if (!v && ok()) fail(k, "missing");
    return v.value_or(T{});
  }
  std::optional<Px> opx(const char* k) { return oparsed<Px>(k, parse_px, "a price"); }
  Px px(const char* k) { return parsed<Px>(k, parse_px, "a price"); }
  std::optional<Qty> ocount(const char* k, bool neg = false) {
    return oparsed<Qty>(k, [neg](const std::string& s) { return parse_count(s, neg); }, "a count");
  }
  Qty count(const char* k, bool neg = false) {
    return parsed<Qty>(k, [neg](const std::string& s) { return parse_count(s, neg); }, "a count");
  }
  std::optional<Micros> odollars(const char* k) { return oparsed<Micros>(k, parse_dollars, "dollars"); }
  Micros dollars(const char* k) { return parsed<Micros>(k, parse_dollars, "dollars"); }
  std::optional<std::int64_t> otime(const char* k) { return oparsed<std::int64_t>(k, parse_time_ms, "an RFC 3339 time"); }
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

std::optional<BookSide> book_side_of(const std::string& s) {
  if (s == "bid") return BookSide::BID;
  if (s == "ask") return BookSide::ASK;
  return std::nullopt;
}

OrderRow read_order(Rd& r) {
  OrderRow o;
  o.order_id = r.str("order_id");
  o.client_order_id = r.str_or_empty("client_order_id");   // "" for an order placed on the website
  o.ticker = r.str("ticker");
  const std::string side = r.str("book_side");
  if (r.ok()) {
    auto bs = book_side_of(side);
    if (!bs) r.fail("book_side", "not bid or ask");
    else o.side = *bs;
  }
  const std::string status = r.str("status");
  if (r.ok()) {
    if (status == "resting") o.status = OrderStatus::RESTING;
    else if (status == "canceled") o.status = OrderStatus::CANCELED;
    else if (status == "executed") o.status = OrderStatus::EXECUTED;
    else r.fail("status", "not resting, canceled or executed: \"" + status + "\"");
  }
  o.yes_price = r.px("yes_price_dollars");
  o.filled = r.count("fill_count_fp");
  o.remaining = r.count("remaining_count_fp");
  o.initial = r.count("initial_count_fp");
  o.taker_fees = r.dollars("taker_fees_dollars");
  o.maker_fees = r.dollars("maker_fees_dollars");
  o.taker_cost = r.dollars("taker_fill_cost_dollars");
  o.maker_cost = r.dollars("maker_fill_cost_dollars");
  o.expiration_ms = r.otime("expiration_time");
  o.created_ms = r.otime("created_time");
  o.updated_ms = r.otime("last_update_time");
  if (auto s = r.ostr("self_trade_prevention_type")) {
    if (*s == "taker_at_cross") o.stp = Stp::TAKER_AT_CROSS;
    else if (*s == "maker") o.stp = Stp::MAKER;
    else r.fail("self_trade_prevention_type", "unknown value \"" + *s + "\"");
  }
  o.order_group_id = r.str_or_empty("order_group_id");
  o.cancel_on_pause = r.obool("cancel_order_on_pause").value_or(false);
  if (auto v = r.oint("subaccount_number")) o.subaccount = static_cast<int>(*v);
  if (auto v = r.oint("exchange_index")) o.exchange_index = static_cast<int>(*v);
  return o;
}

FillRow read_fill(Rd& r) {
  FillRow f;
  f.fill_id = r.str("fill_id");
  f.trade_id = r.str("trade_id");
  f.order_id = r.str("order_id");
  f.ticker = r.str("ticker");
  const std::string side = r.str("book_side");
  if (r.ok()) {
    auto bs = book_side_of(side);
    if (!bs) r.fail("book_side", "not bid or ask");
    else f.side = *bs;
  }
  f.count = r.count("count_fp");
  f.yes_price = r.px("yes_price_dollars");
  f.taker = r.boolean("is_taker");
  f.fee = r.dollars("fee_cost");
  f.exchange_index = static_cast<int>(r.integer("exchange_index"));
  if (auto v = r.oint("subaccount_number")) f.subaccount = static_cast<int>(*v);
  if (auto t = r.otime("created_time")) {
    f.ts_ms = *t;
  } else if (auto ts = r.oint("ts")) {
    f.ts_ms = *ts * 1000;
  } else if (r.ok()) {
    r.fail("created_time", "missing, and no ts either");
  }
  return f;
}

PositionRow read_position(Rd& r) {
  PositionRow p;
  p.ticker = r.str("ticker");
  p.exchange_index = static_cast<int>(r.integer("exchange_index"));
  p.position = r.count("position_fp", true);
  p.exposure = r.dollars("market_exposure_dollars");
  p.realized = r.dollars("realized_pnl_dollars");
  p.fees = r.dollars("fees_paid_dollars");
  p.traded = r.dollars("total_traded_dollars");
  p.updated_ms = r.otime("last_updated_ts");
  return p;
}

MarketRow read_market(Rd& r) {
  MarketRow m;
  m.ticker = r.str("ticker");
  m.event_ticker = r.str_or_empty("event_ticker");
  m.status = r.str("status");
  m.market_type = r.str_or_empty("market_type");
  m.price_level_structure = r.str_or_empty("price_level_structure");
  m.title = r.str_or_empty("title");
  m.subtitle = r.str_or_empty("subtitle");
  m.yes_sub_title = r.str_or_empty("yes_sub_title");
  m.category = r.str_or_empty("category");
  m.result = r.str_or_empty("result");
  if (auto v = r.oint("exchange_index")) m.exchange_index = static_cast<int>(*v);
  m.yes_bid = r.opx("yes_bid_dollars");
  m.yes_ask = r.opx("yes_ask_dollars");
  m.no_bid = r.opx("no_bid_dollars");
  m.no_ask = r.opx("no_ask_dollars");
  m.last = r.opx("last_price_dollars");
  m.yes_bid_size = r.ocount("yes_bid_size_fp");
  m.yes_ask_size = r.ocount("yes_ask_size_fp");
  m.volume = r.ocount("volume_fp");
  m.volume_24h = r.ocount("volume_24h_fp");
  m.open_interest = r.ocount("open_interest_fp");
  m.close_ms = r.otime("close_time");
  m.open_ms = r.otime("open_time");
  m.expected_expiration_ms = r.otime("expected_expiration_time");
  m.expiration_ms = r.otime("expiration_time");
  if (const json* pr = r.at("price_ranges")) {
    if (!pr->is_array()) {
      r.fail("price_ranges", "not an array");
    } else {
      for (const auto& e : *pr) {
        Rd x(e, r.where + ".price_ranges[]");
        PriceRange g;
        g.start = x.px("start");
        g.end = x.px("end");
        g.step = x.px("step");
        if (!x.ok()) {
          if (r.ok()) r.err = x.err;
          break;
        }
        m.price_ranges.push_back(g);
      }
    }
  }
  return m;
}

template <class T, class F>
std::optional<Page<T>> read_page(const std::string& body, const char* key, F read_row, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Page<T> page;
  auto it = j->find(key);
  if (it == j->end() || !it->is_array()) {
    // the venue writes [] for an empty list; null is read as empty too
    if (it == j->end() || !it->is_null()) {
      if (why) *why = std::string(key) + ": missing or not an array";
      return std::nullopt;
    }
  } else {
    for (const auto& row : *it) {
      Rd r(row, std::string(key) + "[" + std::to_string(page.rows.size()) + "]");
      T v = read_row(r);
      if (!r.ok()) {
        if (why) *why = r.err;
        return std::nullopt;
      }
      page.rows.push_back(std::move(v));
    }
  }
  auto c = j->find("cursor");
  if (c != j->end() && !c->is_null()) {
    if (!c->is_string()) {
      if (why) *why = "cursor: not a string";
      return std::nullopt;
    }
    page.cursor = c->get<std::string>();
  }
  return page;
}

void add_q(std::string& q, const char* k, const std::string& v) {
  if (v.empty()) return;
  if (!q.empty()) q += '&';
  q += k;
  q += '=';
  q += url_encode(v);
}
void add_q(std::string& q, const char* k, std::int64_t v, bool present) {
  if (present) add_q(q, k, std::to_string(v));
}

bool ticker_ok(const std::string& t) {
  if (t.empty() || t.size() > 200) return false;
  for (unsigned char c : t)
    if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
  return true;
}
bool id_ok(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (unsigned char c : id)
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
  return true;
}

WireRequest get(const std::string& route, std::string query = "", bool auth = true) {
  WireRequest r;
  r.method = "GET";
  r.path = std::string(kBasePath) + route;
  r.query = std::move(query);
  r.auth = auth;
  return r;
}

void list_query(const ListQuery& q, std::string& s, bool fills, bool positions) {
  add_q(s, "ticker", q.ticker);
  add_q(s, "event_ticker", q.event_ticker);
  if (!fills && !positions) add_q(s, "status", q.status);
  if (fills) add_q(s, "order_id", q.order_id);
  if (!positions) {
    add_q(s, "min_ts", q.min_ts, q.min_ts > 0);
    add_q(s, "max_ts", q.max_ts, q.max_ts > 0);
  }
  if (positions) {
    add_q(s, "count_filter", q.count_filter);
    add_q(s, "settlement_status", q.settlement_status);
  }
  add_q(s, "limit", q.limit, q.limit > 0);
  add_q(s, "cursor", q.cursor);
  add_q(s, "exchange_index", q.exchange_index, q.exchange_index >= 0);
  add_q(s, "subaccount", q.subaccount, q.subaccount >= 0);
}

}  // namespace

// ---- values ------------------------------------------------------------------------------------

std::string price_wire(Px p) {
  char b[24];
  std::snprintf(b, sizeof b, "%lld.%04lld", static_cast<long long>(p / 10000), static_cast<long long>(p % 10000));
  return b;
}
std::string count_wire(Qty q) {
  char b[32];
  const long long a = q < 0 ? -static_cast<long long>(q) : static_cast<long long>(q);
  std::snprintf(b, sizeof b, "%s%lld.%02lld", q < 0 ? "-" : "", a / 100, a % 100);
  return b;
}
std::optional<Micros> parse_dollars(const std::string& s) { return parse_fixed(s, 6, 6, true); }
std::optional<Px> parse_px(const std::string& s) {
  auto m = parse_fixed(s, 6, 6, false);
  if (!m || *m % 100 != 0 || *m > 1000000) return std::nullopt;
  return *m / 100;
}
std::optional<Money> parse_money_wire(const std::string& s) {
  auto m = parse_fixed(s, 6, 6, true);
  if (!m || *m % 100 != 0) return std::nullopt;
  return *m / 100;
}
std::optional<Qty> parse_count(const std::string& s, bool allow_negative) { return parse_fixed(s, 2, 2, allow_negative); }

std::optional<std::int64_t> parse_time_ms(const std::string& s) {
  // YYYY-MM-DDTHH:MM:SS[.f+](Z|+HH:MM|-HH:MM)
  if (s.size() < 20) return std::nullopt;
  auto num = [&](std::size_t a, std::size_t n, int* out) {
    if (!digits(s, a, a + n)) return false;
    *out = std::stoi(s.substr(a, n));
    return true;
  };
  int Y, M, D, h, m, sec;
  if (!num(0, 4, &Y) || s[4] != '-' || !num(5, 2, &M) || s[7] != '-' || !num(8, 2, &D) ||
      (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !num(11, 2, &h) || s[13] != ':' || !num(14, 2, &m) ||
      s[16] != ':' || !num(17, 2, &sec))
    return std::nullopt;
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return std::nullopt;
  std::size_t i = 19;
  std::int64_t ms = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    const std::size_t f0 = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == f0) return std::nullopt;
    int scale = 100;
    for (std::size_t k = f0; k < i && k < f0 + 3; ++k) {
      ms += (s[k] - '0') * scale;
      scale /= 10;
    }
  }
  if (i >= s.size()) return std::nullopt;
  int off_min = 0;
  if (s[i] == 'Z' || s[i] == 'z') {
    if (i + 1 != s.size()) return std::nullopt;
  } else if (s[i] == '+' || s[i] == '-') {
    int oh, om;
    if (s.size() != i + 6 || !num(i + 1, 2, &oh) || s[i + 3] != ':' || !num(i + 4, 2, &om)) return std::nullopt;
    off_min = (oh * 60 + om) * (s[i] == '-' ? -1 : 1);
  } else {
    return std::nullopt;
  }
  const std::int64_t days = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
  const std::int64_t secs = days * 86400 + h * 3600 + m * 60 + sec - off_min * 60;
  return secs * 1000 + ms;
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

const char* book_side_name(BookSide s) { return s == BookSide::BID ? "bid" : "ask"; }
const char* tif_name(TimeInForce t) {
  switch (t) {
    case TimeInForce::FOK: return "fill_or_kill";
    case TimeInForce::GTC: return "good_till_canceled";
    case TimeInForce::IOC: return "immediate_or_cancel";
  }
  return "?";
}
const char* stp_name(Stp s) { return s == Stp::MAKER ? "maker" : "taker_at_cross"; }
const char* order_status_name(OrderStatus s) {
  switch (s) {
    case OrderStatus::RESTING: return "resting";
    case OrderStatus::CANCELED: return "canceled";
    case OrderStatus::EXECUTED: return "executed";
  }
  return "?";
}

// ---- requests ----------------------------------------------------------------------------------

std::string check_create(const CreateOrder& o) {
  if (!ticker_ok(o.ticker)) return "a bad ticker \"" + o.ticker.substr(0, 40) + "\"";
  if (o.count <= 0) return "the count must be above 0";
  if (o.price <= 0 || o.price >= kOneDollar) return "the price must be strictly between 0 and 1";
  if (o.client_order_id.empty()) return "every order carries a client_order_id";
  if (!id_ok(o.client_order_id)) return "a client_order_id may hold letters, digits, - and _ only, 1-128 of them";
  if (!o.order_group_id.empty() && !id_ok(o.order_group_id)) return "a bad order_group_id";
  if (o.exchange_index < 0) return "a write names its shard (exchange_index): an auto-routed one is billed to every shard";
  if (o.subaccount < -1 || o.subaccount > 63) return "subaccount must be 0-63";
  if (o.reduce_only && o.tif != TimeInForce::IOC) return "reduce_only only with immediate_or_cancel";
  if (o.expiration_s != 0 && o.tif != TimeInForce::GTC) return "expiration_time only with good_till_canceled";
  if (o.expiration_s < 0) return "a negative expiration_time";
  if (o.post_only && o.tif != TimeInForce::GTC) return "post_only only with good_till_canceled";
  if (o.tif == TimeInForce::GTC && o.expiration_s == 0) return "no GTC without an expiration_time (plan section 3)";
  return "";
}

CreateOrder from_intent(const PlaceIntent& in, int exchange_index, std::int64_t now_ms) {
  CreateOrder o;
  o.ticker = in.ticker;
  o.side = in.side == Side::BUY ? BookSide::BID : BookSide::ASK;
  o.count = in.count;
  o.price = in.price;
  o.client_order_id = in.cid;
  o.exchange_index = exchange_index;
  o.stp = in.owner == Owner::MAN ? Stp::MAKER : Stp::TAKER_AT_CROSS;
  o.reduce_only = in.reduce_only;
  switch (in.tif) {
    case Tif::IOC: o.tif = TimeInForce::IOC; break;
    case Tif::FOK: o.tif = TimeInForce::FOK; break;
    case Tif::POST:
      o.tif = TimeInForce::GTC;
      o.post_only = true;
      o.cancel_on_pause = true;
      o.expiration_s = in.exp_ms > 0 ? in.exp_ms / 1000 : now_ms / 1000 + in.ttl_s;
      break;
  }
  return o;
}

bool build_create(const CreateOrder& o, WireRequest* out, std::string* why) {
  const std::string bad = check_create(o);
  if (!bad.empty()) {
    if (why) *why = bad;
    return false;
  }
  json b{{"ticker", o.ticker},
         {"side", book_side_name(o.side)},
         {"count", count_wire(o.count)},
         {"price", price_wire(o.price)},
         {"time_in_force", tif_name(o.tif)},
         {"self_trade_prevention_type", stp_name(o.stp)},
         {"client_order_id", o.client_order_id},
         {"exchange_index", o.exchange_index}};
  if (o.post_only) b["post_only"] = true;
  if (o.reduce_only) b["reduce_only"] = true;
  if (o.cancel_on_pause) b["cancel_order_on_pause"] = true;
  if (o.expiration_s > 0) b["expiration_time"] = o.expiration_s;
  if (!o.order_group_id.empty()) b["order_group_id"] = o.order_group_id;
  if (o.subaccount >= 0) b["subaccount"] = o.subaccount;
  WireRequest r;
  r.method = "POST";
  r.path = std::string(kBasePath) + "/portfolio/events/orders";
  r.body = b.dump();
  r.write = true;
  r.op = WireOp::CREATE;
  *out = std::move(r);
  return true;
}

bool build_cancel(const std::string& order_id, int exchange_index, const std::string& market_ticker, int subaccount,
                  WireRequest* out, std::string* why) {
  if (!id_ok(order_id)) {
    if (why) *why = "a bad order_id";
    return false;
  }
  if (exchange_index < 0) {
    if (why) *why = "a cancel names its shard (exchange_index)";
    return false;
  }
  if (!market_ticker.empty() && !ticker_ok(market_ticker)) {
    if (why) *why = "a bad market_ticker";
    return false;
  }
  if (subaccount < -1 || subaccount > 63) {
    if (why) *why = "subaccount must be 0-63";
    return false;
  }
  WireRequest r;
  r.method = "DELETE";
  r.path = std::string(kBasePath) + "/portfolio/events/orders/" + order_id;
  add_q(r.query, "exchange_index", exchange_index, true);
  add_q(r.query, "market_ticker", market_ticker);
  add_q(r.query, "subaccount", subaccount, subaccount >= 0);
  r.write = true;
  r.op = WireOp::CANCEL;
  *out = std::move(r);
  return true;
}

bool build_batch_cancel(const std::vector<BatchCancelItem>& items, WireRequest* out, std::string* why) {
  if (items.empty() || items.size() > kBatchCancelMax) {
    if (why) *why = "a batch cancel takes 1 to " + std::to_string(kBatchCancelMax) + " orders";
    return false;
  }
  json arr = json::array();
  for (const auto& it : items) {
    if (!id_ok(it.order_id) || it.exchange_index < 0 || (!it.market_ticker.empty() && !ticker_ok(it.market_ticker)) ||
        it.subaccount < -1 || it.subaccount > 63) {
      if (why) *why = "a bad batch cancel item (order_id, its shard, ticker or subaccount)";
      return false;
    }
    json o{{"order_id", it.order_id}, {"exchange_index", it.exchange_index}};
    if (!it.market_ticker.empty()) o["market_ticker"] = it.market_ticker;
    if (it.subaccount >= 0) o["subaccount"] = it.subaccount;
    arr.push_back(std::move(o));
  }
  WireRequest r;
  r.method = "DELETE";
  r.path = std::string(kBasePath) + "/portfolio/events/orders/batched";
  r.body = json{{"orders", arr}}.dump();
  r.write = true;
  r.op = WireOp::BATCH_CANCEL;
  *out = std::move(r);
  return true;
}

WireRequest build_cancel_all(int subaccount) {
  WireRequest r;
  r.method = "DELETE";
  r.path = std::string(kBasePath) + "/portfolio/events/orders";
  add_q(r.query, "subaccount", subaccount, subaccount >= 0);
  r.write = true;
  r.op = WireOp::CANCEL_ALL;
  return r;
}

WireRequest build_get_order(const std::string& order_id) { return get("/portfolio/orders/" + url_encode(order_id)); }
WireRequest build_get_orders(const ListQuery& q) {
  std::string s;
  list_query(q, s, false, false);
  return get("/portfolio/orders", s);
}
WireRequest build_get_fills(const ListQuery& q) {
  std::string s;
  list_query(q, s, true, false);
  return get("/portfolio/fills", s);
}
WireRequest build_get_positions(const ListQuery& q) {
  std::string s;
  list_query(q, s, false, true);
  return get("/portfolio/positions", s);
}
WireRequest build_get_balance(int exchange_index, int subaccount) {
  std::string s;
  add_q(s, "subaccount", subaccount, subaccount >= 0);
  add_q(s, "exchange_index", exchange_index, exchange_index >= 0);
  return get("/portfolio/balance", s);
}
WireRequest build_get_market(const std::string& ticker) { return get("/markets/" + url_encode(ticker), "", false); }
WireRequest build_get_series(const std::string& ticker) { return get("/series/" + url_encode(ticker), "", false); }
WireRequest build_get_markets(const MarketsQuery& q) {
  std::string s;
  add_q(s, "limit", q.limit, q.limit > 0);
  add_q(s, "cursor", q.cursor);
  add_q(s, "event_ticker", q.event_ticker);
  add_q(s, "series_ticker", q.series_ticker);
  add_q(s, "status", q.status);
  add_q(s, "tickers", q.tickers);
  add_q(s, "min_close_ts", q.min_close_ts, q.min_close_ts > 0);
  add_q(s, "max_close_ts", q.max_close_ts, q.max_close_ts > 0);
  add_q(s, "mve_filter", q.mve_filter);
  return get("/markets", s, false);
}
WireRequest build_get_orderbook(const std::string& ticker, int depth) {
  std::string s;
  add_q(s, "depth", depth, depth > 0);
  return get("/markets/" + url_encode(ticker) + "/orderbook", s, false);
}
WireRequest build_get_exchange_status() { return get("/exchange/status", "", false); }

std::string request_bytes(const WireRequest& r, const std::string& host_header,
                          const std::vector<std::pair<std::string, std::string>>& auth_headers,
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

std::string ApiError::text() const { return code + " " + message + " " + details; }

ApiError parse_error(int status, const std::string& body) {
  ApiError e;
  e.http = status;
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return e;
  auto it = j.find("error");
  if (it == j.end()) return e;
  if (it->is_string()) {   // the 429's {"error": "too many requests"} (rate_limits.md)
    e.venue_body = status == 429;
    e.message = it->get<std::string>();
    return e;
  }
  if (!it->is_object()) return e;
  const json& o = *it;
  auto s = [&](const char* k) {
    auto f = o.find(k);
    return f != o.end() && f->is_string() ? f->get<std::string>() : std::string();
  };
  e.code = s("code");
  e.message = s("message");
  e.details = s("details");
  e.venue_body = !e.code.empty() || !e.message.empty();
  return e;
}

std::optional<CreateAck> parse_create_ack(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "create");
  CreateAck a;
  a.order_id = r.str("order_id");
  a.client_order_id = r.str_or_empty("client_order_id");
  a.fill_count = r.count("fill_count");
  a.remaining = r.count("remaining_count");
  a.avg_fill_price = r.odollars("average_fill_price");
  a.avg_fee = r.odollars("average_fee_paid");
  a.ts_ms = r.integer("ts_ms");
  return done(r, a, why);
}

std::optional<CancelAck> parse_cancel_ack(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "cancel");
  CancelAck a;
  a.order_id = r.str("order_id");
  a.client_order_id = r.str_or_empty("client_order_id");
  a.reduced_by = r.count("reduced_by");
  a.ts_ms = r.integer("ts_ms");
  return done(r, a, why);
}

std::optional<std::vector<BatchCancelRow>> parse_batch_cancel(const std::string& body, std::string* why) {
  auto page = read_page<BatchCancelRow>(
      body, "orders",
      [](Rd& r) {
        BatchCancelRow b;
        b.order_id = r.str("order_id");
        b.client_order_id = r.str_or_empty("client_order_id");
        b.reduced_by = r.count("reduced_by");
        b.ts_ms = r.oint("ts_ms");
        if (const json* e = r.at("error")) {
          if (!e->is_object()) {
            r.fail("error", "not an object");
          } else {
            ApiError ae = parse_error(0, json{{"error", *e}}.dump());
            ae.venue_body = true;
            b.error = ae;
          }
        }
        return b;
      },
      why);
  if (!page) return std::nullopt;
  return page->rows;
}

std::optional<OrderRow> parse_order(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  auto it = j->find("order");
  if (it == j->end() || !it->is_object()) {
    if (why) *why = "order: missing or not an object";
    return std::nullopt;
  }
  Rd r(*it, "order");
  OrderRow o = read_order(r);
  return done(r, o, why);
}
std::optional<Page<OrderRow>> parse_orders(const std::string& body, std::string* why) {
  return read_page<OrderRow>(body, "orders", read_order, why);
}
std::optional<Page<FillRow>> parse_fills(const std::string& body, std::string* why) {
  return read_page<FillRow>(body, "fills", read_fill, why);
}
std::optional<Page<PositionRow>> parse_positions(const std::string& body, std::string* why) {
  return read_page<PositionRow>(body, "market_positions", read_position, why);
}

std::optional<Balance> parse_balance(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "balance");
  Balance b;
  const std::int64_t cents = r.integer("balance");
  if (auto d = r.ostr("balance_dollars")) {
    auto m = parse_money_wire(*d);
    if (!m) r.fail("balance_dollars", "not dollars on the 1/10,000 grid: \"" + d->substr(0, 40) + "\"");
    else b.available = *m;
  } else {
    b.available = cents * 100;
  }
  if (auto pv = r.oint("portfolio_value")) b.portfolio_value = *pv * 100;
  b.updated_ts = r.integer("updated_ts");
  if (const json* bd = r.at("balance_breakdown")) {
    if (!bd->is_array()) {
      r.fail("balance_breakdown", "not an array");
    } else {
      for (const auto& e : *bd) {
        Rd x(e, "balance.balance_breakdown[]");
        const int idx = static_cast<int>(x.integer("exchange_index"));
        const std::string v = x.str("balance");
        auto m = x.ok() ? parse_money_wire(v) : std::nullopt;
        if (x.ok() && !m) x.fail("balance", "not dollars on the 1/10,000 grid");
        if (!x.ok()) {
          if (r.ok()) r.err = x.err;
          break;
        }
        b.by_shard.emplace_back(idx, *m);
      }
    }
  }
  return done(r, b, why);
}

std::optional<SeriesRow> parse_series(const std::string& body, std::string* why) {
  auto j=parse_object(body,why);
  if(!j || !j->contains("series")) { if(why) *why="series: missing object"; return {}; }
  Rd r(j->at("series"),"series"); SeriesRow row;
  row.ticker=r.str("ticker"); row.category=r.str_or_empty("category"); row.fee_type=r.str("fee_type");
  const auto* m=r.at("fee_multiplier");
  if(!m || !m->is_number()) r.fail("fee_multiplier","missing or not numeric");
  else {
    row.fee_multiplier=m->get<double>();
    if(!std::isfinite(row.fee_multiplier) || row.fee_multiplier<0 || row.fee_multiplier>1000)
      r.fail("fee_multiplier","outside supported numeric range");
  }
  if(row.ticker.empty()) r.fail("ticker","empty");
  if(row.fee_type.empty()) r.fail("fee_type","empty");
  // Unknown fee models remain explicit. The gateway must not trade them by assuming QUAD.
  return done(r,std::move(row),why);
}
std::optional<MarketRow> parse_market(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  auto it = j->find("market");
  if (it == j->end() || !it->is_object()) {
    if (why) *why = "market: missing or not an object";
    return std::nullopt;
  }
  Rd r(*it, "market");
  MarketRow m = read_market(r);
  return done(r, m, why);
}
std::optional<Page<MarketRow>> parse_markets(const std::string& body, std::string* why) {
  return read_page<MarketRow>(body, "markets", read_market, why);
}

std::optional<Orderbook> parse_orderbook(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  auto it = j->find("orderbook_fp");
  if (it == j->end() || !it->is_object()) {
    if (why) *why = "orderbook_fp: missing or not an object";
    return std::nullopt;
  }
  Orderbook ob;
  auto side = [&](const char* k, std::vector<std::pair<Px, Qty>>& out) -> bool {
    auto s = it->find(k);
    if (s == it->end() || s->is_null()) return true;   // an empty side may be left out
    if (!s->is_array()) {
      if (why) *why = std::string("orderbook_fp.") + k + ": not an array";
      return false;
    }
    for (const auto& lv : *s) {
      if (!lv.is_array() || lv.size() != 2 || !lv[0].is_string() || !lv[1].is_string()) {
        if (why) *why = std::string("orderbook_fp.") + k + ": a level is not [price, count]";
        return false;
      }
      auto p = parse_px(lv[0].get<std::string>());
      auto q = parse_count(lv[1].get<std::string>());
      if (!p || !q) {
        if (why) *why = std::string("orderbook_fp.") + k + ": a bad price or count";
        return false;
      }
      out.emplace_back(*p, *q);
    }
    return true;
  };
  if (!side("yes_dollars", ob.yes) || !side("no_dollars", ob.no)) return std::nullopt;
  return ob;
}

std::optional<ExchangeStatus> parse_exchange_status(const std::string& body, std::string* why) {
  auto j = parse_object(body, why);
  if (!j) return std::nullopt;
  Rd r(*j, "exchange_status");
  ExchangeStatus s;
  s.exchange_active = r.boolean("exchange_active");
  s.trading_active = r.boolean("trading_active");
  if (const json* arr = r.at("exchange_index_statuses")) {
    if (!arr->is_array()) {
      r.fail("exchange_index_statuses", "not an array");
    } else {
      for (const auto& e : *arr) {
        Rd x(e, "exchange_status.exchange_index_statuses[]");
        ShardStatus sh;
        sh.exchange_index = static_cast<int>(x.integer("exchange_index"));
        sh.exchange_active = x.boolean("exchange_active");
        sh.trading_active = x.boolean("trading_active");
        sh.transfers_active = x.obool("intra_exchange_transfers_active");
        if (!x.ok()) {
          if (r.ok()) r.err = x.err;
          break;
        }
        s.shards.push_back(sh);
      }
    }
  }
  return done(r, s, why);
}

// ---- what a write's reply means ----------------------------------------------------------------

const char* write_kind_name(WriteOutcome::Kind k) {
  switch (k) {
    case WriteOutcome::Kind::ACK: return "ACK";
    case WriteOutcome::Kind::REFUSED: return "REFUSED";
    case WriteOutcome::Kind::UNKNOWN: return "UNKNOWN";
    case WriteOutcome::Kind::NOT_SENT: return "NOT_SENT";
  }
  return "?";
}

WriteOutcome classify_write(WireOp op, const net::Exchange& x) {
  WriteOutcome w;
  using K = WriteOutcome::Kind;
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
    bool parsed = false;
    switch (op) {
      case WireOp::CREATE: parsed = st == 201 && parse_create_ack(x.res.body, &why).has_value(); break;
      case WireOp::CANCEL: parsed = st == 200 && parse_cancel_ack(x.res.body, &why).has_value(); break;
      case WireOp::BATCH_CANCEL: parsed = st == 200 && parse_batch_cancel(x.res.body, &why).has_value(); break;
      case WireOp::CANCEL_ALL: parsed = st == 204 || st == 200; break;
      case WireOp::OTHER: parsed = true; break;
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
  const ApiError e = parse_error(st, x.res.body);
  if (!e.venue_body && st != 429) {
    w.kind = K::UNKNOWN;
    w.why = "HTTP " + std::to_string(st) + " without the venue's error body (something in between answered)";
    return w;
  }
  w.kind = K::REFUSED;
  w.error = e;
  w.cls = classify_refusal(op, st, e.text());
  w.why = e.text();
  return w;
}

}  // namespace deskd::kalshi
