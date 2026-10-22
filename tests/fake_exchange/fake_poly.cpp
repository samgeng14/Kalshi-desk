#include "fake_poly.h"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <ctime>

#include "rest.h"

namespace fakex {

namespace {
constexpr std::size_t kWireLogMax = 50000;
constexpr long long kPageDefault = 100, kPageMax = 1000;

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// as the fake Kalshi sends it: whole seconds, which deskd's clock check reads (rest::ClockCheck)
std::string http_date() {
  char date[64];
  const std::time_t t = std::time(nullptr);
  std::tm g{};
  gmtime_r(&t, &g);
  std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &g);
  return date;
}

// {value, currency}: a price, or an amount of money (four places, six when it needs them)
json usd(const std::string& value) { return json{{"value", value}, {"currency", "USD"}}; }
json price(Px p) { return usd(px_str(p)); }
json money(Micros m) { return usd(usd4(m)); }
// a quantity as a number, whole contracts as an integer: 5, 2.5
json qty_num(Qty q) {
  return q % kContract == 0 ? json(q / kContract) : json(static_cast<double>(q) / static_cast<double>(kContract));
}
// the same as the ...Decimal fields' strings: "5", "-5", "2.50"
std::string qty_dec(Qty q) { return q % kContract == 0 ? std::to_string(q / kContract) : qty_str(q); }
double dollars(Micros m) { return static_cast<double>(m) / static_cast<double>(kMicrosPerDollar); }

// {code, message}: the gRPC status code beside the HTTP one
std::string error_body(int status, const std::string& message) {
  int code = 2;  // UNKNOWN
  switch (status) {
    case 400: code = 3; break;   // INVALID_ARGUMENT
    case 401: code = 16; break;  // UNAUTHENTICATED
    case 404: code = 5; break;   // NOT_FOUND
    case 405: code = 12; break;  // UNIMPLEMENTED
  }
  return json{{"code", code}, {"message", message}}.dump();
}

// /v1/orders, /v1/order/{id}, /v1/portfolio/... and /v1/account/... are api.polymarket.us's: signed
bool private_path(const std::string& path) {
  return path.compare(0, 9, "/v1/order") == 0 || path.compare(0, 13, "/v1/portfolio") == 0 ||
         path.compare(0, 11, "/v1/account") == 0;
}

std::vector<std::string> segments(const std::string& path) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < path.size()) {
    std::size_t slash = path.find('/', pos);
    if (slash == std::string::npos) slash = path.size();
    if (slash > pos) out.push_back(net::url_decode(path.substr(pos, slash - pos)));
    pos = slash + 1;
  }
  return out;
}

// Every value of `key` in the query, in order (/v1/markets?slug=a&slug=b): net::parse_query keeps
// only the last.
std::vector<std::string> query_all(const std::string& q, const std::string& key) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < q.size()) {
    std::size_t amp = q.find('&', pos);
    if (amp == std::string::npos) amp = q.size();
    const std::string kv = q.substr(pos, amp - pos);
    pos = amp + 1;
    const auto eq = kv.find('=');
    if (eq != std::string::npos && net::url_decode(kv.substr(0, eq)) == key) out.push_back(net::url_decode(kv.substr(eq + 1)));
  }
  return out;
}

// A whole number in [lo, hi]; `dflt` when the query leaves it out. false when it is not one.
bool int_param(const net::Request& r, const char* key, long long dflt, long long lo, long long hi, long long* out) {
  auto it = r.params.find(key);
  if (it == r.params.end()) {
    *out = dflt;
    return true;
  }
  const std::string& s = it->second;
  if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return false;
  *out = std::strtoll(s.c_str(), nullptr, 10);
  return *out >= lo && *out <= hi;
}
}  // namespace

EngineConfig poly_engine_config() {
  EngineConfig c;
  c.cancel_all_echo_ms = 0;
  return c;
}

FakePoly::FakePoly(FakePolyConfig cfg)
    : cfg_(std::move(cfg)),
      engine_(cfg_.engine),
      rng_(cfg_.engine.seed + 1),
      listener_(ListenerConfig{cfg_.port, cfg_.tls_cert_file, cfg_.tls_key_file, cfg_.tls_close_notify,
                               cfg_.tls_handshake_ms}) {
  manual_now_ = cfg_.start_ms ? cfg_.start_ms : wall_ms();
  if (cfg_.auth == AuthMode::VERIFY) {
    const std::string pub = net::unbase64(cfg_.public_key_b64);
    if (pub.size() == 32)
      pubkey_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(pub.data()),
                                            pub.size());
    ERR_clear_error();
  }
}

FakePoly::~FakePoly() {
  stop();
  if (pubkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pubkey_));
}

std::string FakePoly::base() const {
  return std::string(tls() ? "https" : "http") + "://127.0.0.1:" + std::to_string(port());
}

std::int64_t FakePoly::now_locked() const {
  clock_ms_ = std::max(clock_ms_, cfg_.manual_clock ? manual_now_ : wall_ms());
  return clock_ms_;
}
std::int64_t FakePoly::now_ms() const {
  std::lock_guard<std::mutex> lk(mu_);
  return now_locked();
}

bool FakePoly::start(std::string* err) {
  if (running_) return true;
  if (cfg_.auth == AuthMode::VERIFY && !pubkey_) {
    if (err) *err = "AuthMode::VERIFY needs public_key_b64, the key's raw Ed25519 public half (32 bytes)";
    return false;
  }
  stopping_ = false;
  if (!listener_.start([this](int fd) { serve(fd); }, [this](const std::string& line) { log_wire(line); }, err))
    return false;
  running_ = true;
  ticker_ = std::thread([this] { tick_loop(); });
  return true;
}

void FakePoly::stop() {
  if (!running_) return;
  stopping_ = true;
  if (ticker_.joinable()) ticker_.join();
  listener_.stop();
  running_ = false;
}

void FakePoly::tick_loop() {
  while (!stopping_) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!cfg_.manual_clock) {
        engine_.tick(now_locked());
        drain_locked();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.tick_ms));
  }
}

void FakePoly::advance_ms(std::int64_t d) {
  std::lock_guard<std::mutex> lk(mu_);
  manual_now_ += d;
  engine_.tick(now_locked());
  drain_locked();
}

// ---- the venue ----------------------------------------------------------------------------------

void FakePoly::list_market(PolyMarketSpec spec) {
  std::lock_guard<std::mutex> lk(mu_);
  MarketSpec m;
  m.ticker = spec.slug;
  if (!spec.event_slug.empty()) m.event_ticker = spec.event_slug;
  m.ticks = {{spec.tick, kDollar - spec.tick, spec.tick}};
  m.fee_type = FeeType::POLY;
  m.poly_coeff_micro = spec.coeff_micro;
  engine_.add_market(std::move(m), now_locked());
  if (!listing_.count(spec.slug)) listed_.push_back(spec.slug);
  listing_[spec.slug] = std::move(spec);
  drain_locked();
}

void FakePoly::deposit(Micros amount) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::int64_t now = now_locked();
  engine_.set_cash(0, 0, engine_.cash(0, 0) + amount, now);
  activities_.push_back(Activity{
      next_activity_++,
      json{{"type", amount < 0 ? "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL" : "ACTIVITY_TYPE_ACCOUNT_DEPOSIT"},
           {"accountBalanceChange",
            {{"transactionId", uuid4(rng_)}, {"amount", money(amount < 0 ? -amount : amount)}, {"updateTime", rfc3339(now)}}}}});
}

void FakePoly::drain_locked() {
  engine_.drain_events();
  const std::vector<Fill>& fills = engine_.fills();
  for (; fills_seen_ < fills.size(); ++fills_seen_) {
    const Fill& f = fills[fills_seen_];
    activities_.push_back(Activity{next_activity_++,
                                   json{{"type", "ACTIVITY_TYPE_TRADE"},
                                        {"trade",
                                         {{"id", f.fill_id},
                                          {"marketSlug", f.ticker},
                                          {"price", price(f.yes_px)},
                                          {"qtyDecimal", qty_dec(f.count)},
                                          {"isAggressor", f.taker},
                                          {"createTime", rfc3339(f.ts_ms)}}}}});
  }
}

Micros FakePoly::held_value_locked(const std::string& slug, Qty pos) const {
  const auto lv = engine_.levels(slug, pos < 0);  // the held side's bids, best last
  return lv.empty() ? 0 : notional(lv.back().first, pos < 0 ? -pos : pos);
}

// ---- the wire log -------------------------------------------------------------------------------

void FakePoly::log_wire(const std::string& line) {
  std::lock_guard<std::mutex> lk(log_mu_);
  wire_.push_back(line);
  if (wire_.size() > kWireLogMax) wire_.pop_front();
}
std::vector<std::string> FakePoly::wire_log() const {
  std::lock_guard<std::mutex> lk(log_mu_);
  return std::vector<std::string>(wire_.begin(), wire_.end());
}
void FakePoly::clear_wire_log() {
  std::lock_guard<std::mutex> lk(log_mu_);
  wire_.clear();
}

// ---- one connection -----------------------------------------------------------------------------

void FakePoly::serve(int fd) {
  std::string buf;
  for (;;) {
    net::Request req;
    std::string why;
    const net::ReadResult rr = net::read_request(fd, buf, req, cfg_.idle_close_ms, &why);
    if (rr == net::ReadResult::TIMEOUT && !stopping_) {
      ++idle_closes_;
      log_wire("http idle-close");
      return;
    }
    if (rr == net::ReadResult::CLOSED || rr == net::ReadResult::TIMEOUT || stopping_) return;
    if (rr == net::ReadResult::BAD) {
      const std::string body = error_body(400, why.empty() ? "malformed request" : why);
      net::write_all(fd, net::response_head(400, {{"Content-Type", "application/json"},
                                                  {"Content-Length", std::to_string(body.size())},
                                                  {"Connection", "close"}}) +
                             body);
      return;
    }
    ++requests_;
    bool signed_req = false;   // any X-PM-* header, valid or not
    for (const auto& [name, value] : req.headers) {
      (void)value;
      if (name.compare(0, 5, "x-pm-") == 0) signed_req = true;
    }
    Reply rep;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const std::int64_t now = now_locked();
      const std::string denied = private_path(req.path) ? auth_refusal(req, now) : std::string();
      rep = denied.empty() ? route_locked(req, req.path, now) : Reply{401, error_body(401, denied)};
    }
    const std::string data = net::response_head(rep.status, {{"Content-Type", "application/json"},
                                                             {"Date", http_date()},
                                                             {"Content-Length", std::to_string(rep.body.size())},
                                                             {"Connection", req.keep_alive ? "keep-alive" : "close"}}) +
                             rep.body;
    // logged before the write, as on the fake Kalshi: a client that has its reply never sees the
    // entry arrive late
    log_wire("http " + std::to_string(rep.status) + " " + req.method + " " + req.path + (signed_req ? " (signed)" : ""));
    net::write_all(fd, data);
    if (!req.keep_alive) return;
  }
}

std::string FakePoly::auth_refusal(const net::Request& req, std::int64_t now) const {
  if (cfg_.auth == AuthMode::OFF) return "";
  const std::string key = req.header("x-pm-access-key"), ts = req.header("x-pm-timestamp"),
                    sig = req.header("x-pm-signature");
  if (key.empty() || ts.empty() || sig.empty()) return "X-PM-Access-Key, X-PM-Timestamp and X-PM-Signature are required";
  // the venue looks the key up first: a key it does not know never has its signature checked
  if (cfg_.auth == AuthMode::VERIFY && !cfg_.key_id.empty() && key != cfg_.key_id) return "API key not found";
  if (ts.size() > 15 || ts.find_first_not_of("0123456789") != std::string::npos)
    return "X-PM-Timestamp must be milliseconds";
  if (cfg_.auth_max_skew_ms > 0 && std::llabs(std::stoll(ts) - now) > cfg_.auth_max_skew_ms)
    return "X-PM-Timestamp is too far from the exchange clock";
  if (cfg_.auth == AuthMode::VERIFY) {
    // Ed25519 signs the message itself: timestamp + method + path, the path without its query
    const std::string text = ts + req.method + req.path;
    const std::string raw = net::unbase64(sig);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = ctx && raw.size() == 64 &&
                    EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, static_cast<EVP_PKEY*>(pubkey_)) == 1 &&
                    EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
                                     reinterpret_cast<const unsigned char*>(text.data()), text.size()) == 1;
    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
    if (!ok) return "invalid signature";
  }
  return "";
}

// ---- routes -------------------------------------------------------------------------------------

FakePoly::Reply FakePoly::route_locked(const net::Request& req, const std::string& path, std::int64_t now) {
  const std::vector<std::string> seg = segments(path);
  auto get = [&](auto&& f) { return req.method == "GET" ? f() : Reply{405, error_body(405, "GET only")}; };
  if (seg.size() == 4 && seg[0] == "v2" && seg[1] == "leagues" && seg[3] == "events")
    return get([&] { return league_events_locked(req, seg[2], now); });
  if (seg.size() >= 2 && seg[0] == "v1") {
    if (seg[1] == "markets" && seg.size() == 2) return get([&] { return markets_locked(req, now); });
    if (seg[1] == "markets" && seg.size() == 4 && seg[3] == "book") return get([&] { return book_locked(seg[2]); });
    if (seg[1] == "account" && seg.size() == 3 && seg[2] == "balances") return get([&] { return balances_locked(); });
    if (seg[1] == "portfolio" && seg.size() == 3 && seg[2] == "positions")
      return get([&] { return positions_locked(req); });
    if (seg[1] == "portfolio" && seg.size() == 3 && seg[2] == "activities")
      return get([&] { return activities_locked(req); });
    if (seg[1] == "orders" && seg.size() == 3 && seg[2] == "open") return get([&] { return open_orders_locked(); });
    if (seg[1] == "order" && seg.size() == 3) return get([&] { return order_locked(seg[2]); });
  }
  return Reply{404, error_body(404, "no such route")};
}

FakePoly::Reply FakePoly::league_events_locked(const net::Request& req, const std::string& league, std::int64_t now) {
  long long limit = 0, offset = 0;
  if (!int_param(req, "limit", kPageDefault, 1, kPageMax, &limit) || !int_param(req, "offset", 0, 0, LLONG_MAX, &offset))
    return Reply{400, error_body(400, "limit is 1 to 1000 and offset a whole number")};
  // the league's events in the order their first market was listed, each with its markets
  std::vector<std::string> order;
  std::map<std::string, json> events;
  for (const std::string& slug : listed_) {
    const PolyMarketSpec& s = listing_.at(slug);
    if (s.league != league) continue;
    auto it = events.find(s.event_slug);
    if (it == events.end()) {
      order.push_back(s.event_slug);
      it = events.emplace(s.event_slug, json{{"slug", s.event_slug}, {"title", s.event_title}, {"markets", json::array()}})
               .first;
    }
    it->second["markets"].push_back(market_json(s, now));
  }
  json page = json::array();
  for (std::size_t i = static_cast<std::size_t>(std::min<long long>(offset, static_cast<long long>(order.size())));
       i < order.size() && page.size() < static_cast<std::size_t>(limit); ++i)
    page.push_back(events[order[i]]);
  return Reply{200, json{{"events", page}, {"league", {{"slug", league}}}}.dump()};
}

FakePoly::Reply FakePoly::markets_locked(const net::Request& req, std::int64_t now) {
  json out = json::array();
  const std::vector<std::string> slugs = query_all(req.query, "slug");
  if (!slugs.empty()) {
    for (const std::string& slug : slugs) {
      auto it = listing_.find(slug);
      if (it != listing_.end()) out.push_back(market_json(it->second, now));
    }
    return Reply{200, json{{"markets", out}}.dump()};
  }
  long long limit = 0, offset = 0;
  if (!int_param(req, "limit", kPageDefault, 1, kPageMax, &limit) || !int_param(req, "offset", 0, 0, LLONG_MAX, &offset))
    return Reply{400, error_body(400, "limit is 1 to 1000 and offset a whole number")};
  for (std::size_t i = static_cast<std::size_t>(std::min<long long>(offset, static_cast<long long>(listed_.size())));
       i < listed_.size() && out.size() < static_cast<std::size_t>(limit); ++i)
    out.push_back(market_json(listing_.at(listed_[i]), now));
  return Reply{200, json{{"markets", out}}.dump()};
}

FakePoly::Reply FakePoly::book_locked(const std::string& slug) {
  if (!listing_.count(slug)) return Reply{404, error_body(404, "market not found")};
  json bids = json::array(), asks = json::array();
  const auto yes = engine_.levels(slug, false), no = engine_.levels(slug, true);  // each best last
  for (auto it = yes.rbegin(); it != yes.rend(); ++it) bids.push_back(json{{"px", price(it->first)}, {"qty", qty_str(it->second)}});
  // a NO bid at q is a YES offer at 1 - q: the best NO bid is the best offer
  for (auto it = no.rbegin(); it != no.rend(); ++it)
    asks.push_back(json{{"px", price(kDollar - it->first)}, {"qty", qty_str(it->second)}});
  return Reply{200, json{{"marketData", {{"bids", bids}, {"asks", asks}}}}.dump()};
}

FakePoly::Reply FakePoly::balances_locked() {
  Micros held = 0;
  for (const auto& [key, p] : engine_.positions())
    if (key.first == 0 && p.pos != 0) held += held_value_locked(key.second, p.pos);
  const json row{{"currentBalance", dollars(engine_.cash(0, 0))},
                 {"currency", "USD"},
                 {"buyingPower", dollars(engine_.available(0, 0))},
                 {"assetNotional", dollars(held)},
                 {"assetAvailable", dollars(held)}};
  return Reply{200, json{{"balances", json::array({row})}}.dump()};
}

FakePoly::Reply FakePoly::positions_locked(const net::Request& req) {
  long long limit = 0;
  if (!int_param(req, "limit", kPageDefault, 1, kPageMax, &limit))
    return Reply{400, error_body(400, "limit is 1 to 1000")};
  auto c = req.params.find("cursor");
  const std::string after = c == req.params.end() ? "" : c->second;  // the last slug of the page before
  json out = json::object();
  std::string last;
  bool more = false;
  for (const auto& [key, p] : engine_.positions()) {  // by slug
    if (key.first != 0 || p.pos == 0 || (!after.empty() && key.second <= after)) continue;
    if (out.size() == static_cast<std::size_t>(limit)) {
      more = true;
      break;
    }
    out[key.second] = json{{"netPositionDecimal", qty_dec(p.pos)},
                           {"cost", money(p.cost)},
                           {"realized", money(p.realized)},
                           {"cashValue", money(held_value_locked(key.second, p.pos))},
                           {"qtyAvailableDecimal", qty_dec(p.pos < 0 ? -p.pos : p.pos)},
                           {"expired", false},
                           {"updateTime", rfc3339(p.updated_ms)}};
    last = key.second;
  }
  return Reply{200, json{{"positions", out}, {"nextCursor", more ? last : ""}, {"eof", !more}}.dump()};
}

FakePoly::Reply FakePoly::activities_locked(const net::Request& req) {
  long long limit = 0;
  if (!int_param(req, "limit", kPageDefault, 1, kPageMax, &limit))
    return Reply{400, error_body(400, "limit is 1 to 1000")};
  std::uint64_t before = UINT64_MAX;  // the oldest of the page before: this page starts below it
  auto c = req.params.find("cursor");
  if (c != req.params.end() && !c->second.empty() && !read_cursor(c->second, &before))
    return Reply{400, error_body(400, "invalid cursor")};
  json page = json::array();
  std::uint64_t last = 0;
  bool more = false;
  for (auto it = activities_.rbegin(); it != activities_.rend(); ++it) {
    if (it->seq >= before) continue;
    if (page.size() == static_cast<std::size_t>(limit)) {
      more = true;
      break;
    }
    page.push_back(it->body);
    last = it->seq;
  }
  return Reply{200, json{{"activities", page}, {"nextCursor", more ? make_cursor(last) : ""}, {"eof", !more}}.dump()};
}

FakePoly::Reply FakePoly::open_orders_locked() {
  json out = json::array();
  for (const Order* o : engine_.user_orders())
    if (o->status == OrdStatus::RESTING) out.push_back(order_json(*o));
  return Reply{200, json{{"orders", out}}.dump()};
}

FakePoly::Reply FakePoly::order_locked(const std::string& id) {
  const Order* o = engine_.order(id);
  if (!o) return Reply{404, error_body(404, "order not found")};  // the street's orders have no id
  return Reply{200, json{{"order", order_json(*o)}}.dump()};
}

json FakePoly::market_json(const PolyMarketSpec& s, std::int64_t now) const {
  const Market* m = engine_.market(s.slug);
  json j{{"slug", s.slug},
         {"question", s.question},
         {"sportsMarketType", s.sports_type},
         {"orderPriceMinTickSize", static_cast<double>(s.tick) / static_cast<double>(kDollar)},
         {"minimumTradeQty", qty_num(s.min_qty)},
         {"feeCoefficient", static_cast<double>(s.coeff_micro) / 1e6},
         {"active", m && m->state == Market::State::OPEN},
         {"closed", !m || m->state == Market::State::CLOSED}};
  const auto yes = engine_.levels(s.slug, false), no = engine_.levels(s.slug, true);
  if (!yes.empty()) j["bestBidQuote"] = price(yes.back().first);
  if (!no.empty()) j["bestAskQuote"] = price(kDollar - no.back().first);
  if (s.start_ms > 0) j["gameStartTime"] = rfc3339(s.start_ms);
  if (s.end_ms > 0) j["endDate"] = rfc3339(s.end_ms);
  j["updatedAt"] = rfc3339(now);
  json sides = json::array();
  for (const PolySide& side : s.sides) {
    json x{{"description", side.description},
           {"long", side.is_long},
           {"team",
            {{"abbreviation", side.team.abbreviation},
             {"name", side.team.name},
             {"safeName", side.team.safe_name},
             {"alias", side.team.alias},
             {"league", s.league}}}};
    // what the side costs: the long side the YES offer, the short side 1 - the YES bid
    if (side.is_long && !no.empty()) x["price"] = px_str(kDollar - no.back().first);
    if (!side.is_long && !yes.empty()) x["price"] = px_str(kDollar - yes.back().first);
    sides.push_back(std::move(x));
  }
  j["marketSides"] = sides;
  return j;
}

json FakePoly::order_json(const Order& o) const {
  const bool buy = o.side == Side::BID, resting = o.status == OrdStatus::RESTING;
  const char* tif = o.tif == Tif::IOC   ? "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL"
                    : o.tif == Tif::FOK ? "TIME_IN_FORCE_FILL_OR_KILL"
                    : o.expiration_s > 0 ? "TIME_IN_FORCE_GOOD_TILL_DATE"
                                         : "TIME_IN_FORCE_GOOD_TILL_CANCEL";
  const char* state = resting ? (o.filled > 0 ? "ORDER_STATE_PARTIALLY_FILLED" : "ORDER_STATE_NEW")
                      : o.status == OrdStatus::EXECUTED ? "ORDER_STATE_FILLED"
                      : o.reason == "ExpiryCancel"      ? "ORDER_STATE_EXPIRED"
                                                        : "ORDER_STATE_CANCELED";
  json j{{"id", o.order_id},
         {"marketSlug", o.ticker},
         {"type", "ORDER_TYPE_LIMIT"},
         {"side", buy ? "ORDER_SIDE_BUY" : "ORDER_SIDE_SELL"},
         {"tif", tif},
         {"state", state},
         {"price", price(o.price)},
         {"quantity", qty_num(o.initial)},
         {"cumQuantity", qty_num(o.filled)},
         {"leavesQuantity", qty_num(resting ? o.remaining : 0)},
         {"commissionNotionalTotalCollected", money(o.taker_fees + o.maker_fees)},
         {"createTime", rfc3339(o.created_ms)},
         {"updateTime", rfc3339(o.updated_ms)}};
  if (o.filled > 0) {
    // the engine keeps what the side bought cost: a SELL bought NO at 1 - p
    const Micros per = (o.taker_cost + o.maker_cost) * kContract / o.filled;
    j["avgPx"] = money(buy ? per : kMicrosPerDollar - per);
  }
  return j;
}

}  // namespace fakex
