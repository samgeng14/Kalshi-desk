#include "fake_poly.h"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <set>

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
    case 429: code = 8; break;   // RESOURCE_EXHAUSTED
    case 500: code = 13; break;  // INTERNAL
    case 503: code = 14; break;  // UNAVAILABLE
  }
  return json{{"code", code}, {"message", message}}.dump();
}

const char* tif_wire(Tif t, bool gtd) {
  return t == Tif::IOC ? "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL"
         : t == Tif::FOK ? "TIME_IN_FORCE_FILL_OR_KILL"
         : gtd           ? "TIME_IN_FORCE_GOOD_TILL_DATE"
                         : "TIME_IN_FORCE_GOOD_TILL_CANCEL";
}

// A create's price, {value: "0.385", currency: "USD"}: up to four places. nullopt otherwise.
std::optional<Px> read_price(const json& v) {
  if (!v.is_object() || v.size() != 2 || !v.contains("value") || !v["value"].is_string() || v.value("currency", "") != "USD")
    return std::nullopt;
  return parse_fixed(v["value"].get<std::string>(), 4, false);
}

// A create's quantity: a number (5, 5.0) or a decimal string ("5", "5.00"), to the hundredth.
// nullopt for anything else; whether it is whole and positive is the caller's.
std::optional<Qty> read_qty(const json& v) {
  if (v.is_number_integer()) {
    const long long n = v.get<long long>();
    if (n < -1000000000LL || n > 1000000000LL) return std::nullopt;
    return static_cast<Qty>(n) * kContract;
  }
  if (v.is_number_float()) {
    const double c = v.get<double>() * kContract;
    if (!(std::fabs(c) < 1e11) || std::fabs(c - std::round(c)) > 1e-6) return std::nullopt;
    return static_cast<Qty>(std::llround(c));
  }
  if (v.is_string()) return parse_fixed(v.get<std::string>(), 2, true);
  return std::nullopt;
}

// expireTime: "2026-10-06T15:31:00Z", or with a fraction of zeros, in UTC: whole seconds since the
// epoch. false for anything else, a time between two seconds among it.
bool read_whole_second(const std::string& s, std::int64_t* out) {
  static const char kShape[] = "dddd-dd-ddTdd:dd:dd";
  if (s.size() < 20) return false;
  for (std::size_t i = 0; i < 19; ++i)
    if (kShape[i] == 'd' ? !std::isdigit(static_cast<unsigned char>(s[i])) : s[i] != kShape[i]) return false;
  std::size_t i = 19;
  if (s[i] == '.') {
    const std::size_t from = ++i;
    while (i < s.size() && s[i] == '0') ++i;
    if (i == from) return false;
  }
  if (i + 1 != s.size() || s[i] != 'Z') return false;
  auto num = [&](std::size_t at, std::size_t n) { return std::atoi(s.substr(at, n).c_str()); };
  std::tm tm{};
  tm.tm_year = num(0, 4) - 1900;
  tm.tm_mon = num(5, 2) - 1;
  tm.tm_mday = num(8, 2);
  tm.tm_hour = num(11, 2);
  tm.tm_min = num(14, 2);
  tm.tm_sec = num(17, 2);
  if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
      tm.tm_sec > 59)
    return false;
  *out = static_cast<std::int64_t>(timegm(&tm));
  return true;
}

// The fields of a create the fake knows (src/poly.cpp poly_order_body, and expireTime for GTD)
const std::set<std::string>& create_fields() {
  static const std::set<std::string> f = {"marketSlug", "type", "price", "quantity", "tif", "outcomeSide", "action",
                                          "manualOrderIndicator", "participateDontInitiate", "synchronousExecution",
                                          "maxBlockTime", "expireTime"};
  return f;
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

HttpFault poly_fault(HttpFault::Kind kind, const std::string& method, const std::string& path) {
  HttpFault f;
  f.kind = kind;
  f.method = method;
  f.path = path;
  return f;
}

FakePoly::FakePoly(FakePolyConfig cfg)
    : cfg_(std::move(cfg)),
      engine_(cfg_.engine),
      rng_(cfg_.engine.seed + 1),
      listener_(ListenerConfig{cfg_.port, cfg_.tls_cert_file, cfg_.tls_key_file, cfg_.tls_close_notify,
                               cfg_.tls_handshake_ms}) {
  manual_now_ = cfg_.start_ms ? cfg_.start_ms : wall_ms();
  tokens_ = cfg_.requests_per_s;
  tokens_ms_ = now_locked();
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
        const std::int64_t now = now_locked();
        take_due_locked(now);
        engine_.tick(now);
        drain_locked();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.tick_ms));
  }
}

void FakePoly::advance_ms(std::int64_t d) {
  std::lock_guard<std::mutex> lk(mu_);
  manual_now_ += d;
  const std::int64_t now = now_locked();
  take_due_locked(now);
  engine_.tick(now);
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

std::string FakePoly::engine_id(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return engine_id_locked(id);
}
std::string FakePoly::engine_id_locked(const std::string& id) const {
  auto it = submitted_.find(id);
  if (it != submitted_.end()) return it->second.engine_id;
  return by_engine_.count(id) || !engine_.order(id) ? "" : id;  // ours go by our id
}

json FakePoly::sent(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = submitted_.find(id);
  return it == submitted_.end() ? json() : it->second.sent;
}

void FakePoly::take_due_locked(std::int64_t now) {
  // in the order accepted; a delay changed meanwhile can make a later one due first
  for (std::size_t i = 0; i < pending_.size();) {
    Submitted& s = submitted_.at(pending_[i]);
    if (s.due_ms > now) {
      ++i;
      continue;
    }
    take_locked(s, now);  // out of pending_
  }
}

CreateResult FakePoly::take_locked(Submitted& s, std::int64_t now) {
  pending_.erase(std::remove(pending_.begin(), pending_.end(), s.id), pending_.end());
  const CreateResult r = engine_.create(s.order, now);
  s.updated_ms = now;
  if (r.err) {
    s.state = Submitted::State::REJECTED;
  } else {
    s.state = Submitted::State::TAKEN;
    s.engine_id = r.order_id;
    by_engine_[r.order_id] = s.id;
  }
  drain_locked();
  return r;
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

// ---- faults and settings ------------------------------------------------------------------------

void FakePoly::add_fault(HttpFault f) {
  std::lock_guard<std::mutex> lk(mu_);
  faults_.push_back(std::move(f));
}
void FakePoly::clear_faults() {
  std::lock_guard<std::mutex> lk(mu_);
  faults_.clear();
}
std::size_t FakePoly::faults_pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return faults_.size();
}
void FakePoly::set_rate_limits(bool on, double requests_per_s) {
  std::lock_guard<std::mutex> lk(mu_);
  cfg_.rate_limits = on;
  cfg_.requests_per_s = requests_per_s;
  tokens_ = requests_per_s;
  tokens_ms_ = now_locked();
}
void FakePoly::set_async_delay_ms(int ms) {
  std::lock_guard<std::mutex> lk(mu_);
  cfg_.async_delay_ms = ms;
}
void FakePoly::set_maintenance(std::int64_t from_ms, std::int64_t to_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  maintenance_from_ = from_ms;
  maintenance_to_ = to_ms;
}
void FakePoly::drop_http_connections() { listener_.drop_http(); }

bool FakePoly::take_fault_locked(const net::Request& req, const json& body, HttpFault* f) {
  const std::string slug = body.is_object() && body.contains("marketSlug") && body["marketSlug"].is_string()
                               ? body["marketSlug"].get<std::string>()
                               : "";
  for (auto it = faults_.begin(); it != faults_.end(); ++it) {
    if (it->method != req.method || req.path.compare(0, it->path.size(), it->path) != 0) continue;
    if (!it->ticker.empty() && it->ticker != slug) continue;
    *f = *it;
    if (--it->times <= 0) faults_.erase(it);
    return true;
  }
  return false;
}

bool FakePoly::rate_ok_locked(std::int64_t now) {
  if (!cfg_.rate_limits) return true;
  if (now > tokens_ms_)
    tokens_ = std::min(cfg_.requests_per_s, tokens_ + cfg_.requests_per_s * static_cast<double>(now - tokens_ms_) / 1000.0);
  tokens_ms_ = std::max(tokens_ms_, now);
  if (tokens_ < 1) return false;
  tokens_ -= 1;
  return true;
}

bool FakePoly::in_maintenance_locked(std::int64_t now) const {
  return now >= maintenance_from_ && now < maintenance_to_;
}

void FakePoly::pull_touch_locked(const json& create, Qty qty, std::int64_t now) {
  auto str = [&](const char* k) {
    return create.is_object() && create.contains(k) && create[k].is_string() ? create[k].get<std::string>() : "";
  };
  const std::string slug = str("marketSlug"), action = str("action");
  const std::optional<Px> px = create.is_object() && create.contains("price") ? read_price(create["price"]) : std::nullopt;
  Qty pulled = 0;
  if (px && (action == "ORDER_ACTION_BUY" || action == "ORDER_ACTION_SELL"))  // else refused as it would be anyway
    pulled = engine_.street_pull_touch(slug, action == "ORDER_ACTION_BUY" ? Side::BID : Side::ASK, *px, qty, now);
  log_wire("http pull-touch " + slug + " " + qty_str(pulled));
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
    const std::string what = req.method + " " + req.path + (signed_req ? " (signed)" : "");
    // the head of a reply: a 429 says when to come back
    auto head = [&](int status, std::vector<std::pair<std::string, std::string>> h) {
      if (status == 429 && cfg_.retry_after_s > 0) h.emplace_back("Retry-After", std::to_string(cfg_.retry_after_s));
      return net::response_head(status, h);
    };

    const json body = req.body.empty() ? json() : json::parse(req.body, nullptr, false);
    HttpFault f;
    bool faulted;
    {
      std::lock_guard<std::mutex> lk(mu_);
      faulted = take_fault_locked(req, body, &f);
    }
    using K = HttpFault::Kind;
    auto is = [&](K k) { return faulted && f.kind == k; };
    if (is(K::RATE_LIMIT)) {
      const std::string e = error_body(429, "too many requests");
      net::write_all(fd, head(429, {{"Content-Type", "application/json"}, {"Content-Length", std::to_string(e.size())}}) + e);
      log_wire("http 429 " + what + " (fault)");
      if (!req.keep_alive) return;
      continue;
    }
    if (is(K::DROP_BEFORE_REPLY)) {
      log_wire("http drop-before-reply " + what);
      return;
    }
    if (is(K::STALL_BEFORE)) std::this_thread::sleep_for(std::chrono::milliseconds(f.stall_ms));

    Reply rep;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const std::int64_t now = now_locked();
      const std::string denied = private_path(req.path) ? auth_refusal(req, now) : std::string();
      if (in_maintenance_locked(now)) {
        rep = Reply{503, error_body(503, "the exchange is down for maintenance")};
      } else if (!denied.empty()) {
        rep = Reply{401, error_body(401, denied)};
      } else if (!rate_ok_locked(now)) {
        rep = Reply{429, error_body(429, "too many requests")};
      } else {
        // the street's fill lands before the cancel does; a pull of the touch before the create
        if (is(K::FILL_DURING_CANCEL) && req.method == "POST") {
          const std::vector<std::string> seg = segments(req.path);
          const std::string eid = seg.size() == 4 ? engine_id_locked(seg[2]) : "";
          if (!eid.empty()) engine_.street_hit(eid, f.qty, now);
        }
        if (is(K::PULL_TOUCH) && req.method == "POST") pull_touch_locked(body, f.qty, now);
        std::string created;
        rep = route_locked(req, req.path, body, now, &created);
        // the engine takes the create before the reply: its fills are in the account first
        if ((is(K::FILL_BEFORE_REPLY) || is(K::HIT_AFTER_REST)) && !created.empty()) {
          Submitted& s = submitted_.at(created);
          if (s.state == Submitted::State::PENDING) take_locked(s, now);
          const Order* o = s.engine_id.empty() ? nullptr : engine_.order(s.engine_id);
          if (is(K::HIT_AFTER_REST) && o && o->in_book) engine_.street_hit(s.engine_id, f.qty, now);
        }
        drain_locked();
      }
    }
    if (is(K::STALL_AFTER)) std::this_thread::sleep_for(std::chrono::milliseconds(f.stall_ms));
    if (is(K::STATUS_AFTER_ACCEPT)) rep = Reply{f.status, error_body(f.status, "internal server error")};

    const bool close_after = !req.keep_alive || is(K::CLOSE_AFTER_REPLY) || is(K::CUT_BODY) || is(K::DROP_AFTER_ACCEPT);
    if (is(K::DROP_AFTER_ACCEPT)) {
      log_wire("http drop-after-accept " + std::to_string(rep.status) + " " + what);
      return;
    }
    std::vector<std::pair<std::string, std::string>> h = {{"Content-Type", "application/json"}, {"Date", http_date()}};
    if (is(K::CHUNKED))
      h.emplace_back("Transfer-Encoding", "chunked");
    else
      h.emplace_back("Content-Length", std::to_string(rep.body.size()));
    h.emplace_back("Connection", close_after ? "close" : "keep-alive");
    std::string data = head(rep.status, std::move(h));
    std::string label = "http " + std::to_string(rep.status) + " " + what;
    if (is(K::CHUNKED)) {
      data += net::chunked_body(rep.body, f.chunk_bytes);
      label += " (chunked)";
    } else if (is(K::CUT_BODY)) {
      const std::size_t cut = f.cut_bytes ? std::min(f.cut_bytes, rep.body.size() ? rep.body.size() - 1 : 0)
                                          : rep.body.size() / 2;
      data += rep.body.substr(0, cut);
      label = "http cut " + std::to_string(rep.status) + " " + what;
    } else {
      data += rep.body;
    }
    // logged before the write, as on the fake Kalshi: a client that has its reply never sees the
    // entry arrive late
    log_wire(label);
    net::write_all(fd, data);
    if (close_after) return;
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

FakePoly::Reply FakePoly::route_locked(const net::Request& req, const std::string& path, const json& body,
                                       std::int64_t now, std::string* created) {
  const std::vector<std::string> seg = segments(path);
  auto get = [&](auto&& f) { return req.method == "GET" ? f() : Reply{405, error_body(405, "GET only")}; };
  // a write's body is JSON, or nothing
  auto post = [&](auto&& f) {
    if (req.method != "POST") return Reply{405, error_body(405, "POST only")};
    if (body.is_discarded()) return Reply{400, error_body(400, "the body is not JSON")};
    return f();
  };
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
    if (seg[1] == "order" && seg.size() == 3 && seg[2] == "preview" && req.method == "POST")
      return post([&] { return preview_locked(body, now); });
    if (seg[1] == "order" && seg.size() == 3) return get([&] { return order_locked(seg[2]); });
    if (seg[1] == "orders" && seg.size() == 2) return post([&] { return create_locked(body, now, created); });
    if (seg[1] == "order" && seg.size() == 4 && seg[3] == "cancel")
      return post([&] { return cancel_locked(seg[2], body, now); });
    if (seg[1] == "orders" && seg.size() == 4 && seg[2] == "open" && seg[3] == "cancel")
      return post([&] { return cancel_all_locked(body, now); });
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
  for (const std::string& id : open_ids_locked()) out.push_back(order_view_locked(id));
  return Reply{200, json{{"orders", out}}.dump()};
}

FakePoly::Reply FakePoly::order_locked(const std::string& id) {
  json o = order_view_locked(id);
  if (o.is_null()) return Reply{404, error_body(404, "order not found")};
  return Reply{200, json{{"order", std::move(o)}}.dump()};
}

// ---- writes -------------------------------------------------------------------------------------

std::string FakePoly::read_create_locked(const json& b, std::int64_t now, NewOrder* out, bool* sync, int* status) const {
  *status = 400;
  if (!b.is_object()) return "the order must be a JSON object";
  for (auto it = b.begin(); it != b.end(); ++it)
    if (!create_fields().count(it.key())) return "unknown field " + it.key();
  auto str = [&](const char* k) { return b.contains(k) && b[k].is_string() ? b[k].get<std::string>() : std::string(); };
  const std::string slug = str("marketSlug"), tif = str("tif"), action = str("action"),
                    manual = str("manualOrderIndicator");
  if (slug.empty()) return "marketSlug is required";
  if (str("type") != "ORDER_TYPE_LIMIT") return "type must be ORDER_TYPE_LIMIT";
  if (action != "ORDER_ACTION_BUY" && action != "ORDER_ACTION_SELL")
    return "action must be ORDER_ACTION_BUY or ORDER_ACTION_SELL";
  if (str("outcomeSide") != "OUTCOME_SIDE_YES") return "outcomeSide must be OUTCOME_SIDE_YES";
  if (manual != "MANUAL_ORDER_INDICATOR_MANUAL" && manual != "MANUAL_ORDER_INDICATOR_AUTOMATIC")
    return "manualOrderIndicator must be MANUAL_ORDER_INDICATOR_MANUAL or MANUAL_ORDER_INDICATOR_AUTOMATIC";
  for (const char* k : {"participateDontInitiate", "synchronousExecution"})
    if (b.contains(k) && !b[k].is_boolean()) return std::string(k) + " must be true or false";
  if (b.contains("maxBlockTime") && !b["maxBlockTime"].is_string() && !b["maxBlockTime"].is_number())
    return "maxBlockTime must be a number of seconds";
  NewOrder o;
  if (tif == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL")
    o.tif = Tif::IOC;
  else if (tif == "TIME_IN_FORCE_FILL_OR_KILL")
    o.tif = Tif::FOK;
  else if (tif == "TIME_IN_FORCE_GOOD_TILL_CANCEL" || tif == "TIME_IN_FORCE_GOOD_TILL_DATE")
    o.tif = Tif::GTC;
  else
    return "tif must be TIME_IN_FORCE_IMMEDIATE_OR_CANCEL, _FILL_OR_KILL, _GOOD_TILL_CANCEL or _GOOD_TILL_DATE";
  const bool gtd = tif == "TIME_IN_FORCE_GOOD_TILL_DATE";
  if (b.contains("expireTime") != gtd)
    return gtd ? "expireTime is required with TIME_IN_FORCE_GOOD_TILL_DATE" : "expireTime goes only with TIME_IN_FORCE_GOOD_TILL_DATE";
  o.post_only = b.value("participateDontInitiate", false);
  if (o.post_only && o.tif != Tif::GTC)
    return "participateDontInitiate goes only with TIME_IN_FORCE_GOOD_TILL_CANCEL or _GOOD_TILL_DATE";
  const std::optional<Px> px = b.contains("price") ? read_price(b["price"]) : std::nullopt;
  if (!px) return "price must be {value, currency USD}, the value a decimal string";
  const std::optional<Qty> q = b.contains("quantity") ? read_qty(b["quantity"]) : std::nullopt;
  if (!q) return "quantity must be a number";
  auto it = listing_.find(slug);
  const Market* m = engine_.market(slug);
  if (it == listing_.end() || !m) {
    *status = 404;
    return "market not found";
  }
  if (!on_grid(m->spec.ticks, *px)) return "price must be between 0 and 1 on the market's tick";
  if (*q <= 0 || *q % kContract != 0 || *q < it->second.min_qty)
    return "quantity must be whole contracts, at least the market's minimumTradeQty";
  if (gtd) {
    std::int64_t exp_s = 0;
    if (!b["expireTime"].is_string() || !read_whole_second(b["expireTime"].get<std::string>(), &exp_s))
      return "expireTime must be RFC 3339 in UTC, a whole second";
    // the engine's own test, made here so that it is a 400 and not a REJECTED order
    const std::int64_t lead_s = engine_.config().min_expiry_lead_s;
    if (exp_s * 1000 <= now + lead_s * 1000)
      return lead_s > 0 ? "expireTime must be more than " + std::to_string(lead_s) + " s ahead" : "expireTime must be ahead";
    o.expiration_s = exp_s;
  }
  o.ticker = slug;
  o.side = action == "ORDER_ACTION_BUY" ? Side::BID : Side::ASK;
  o.count = *q;
  o.price = *px;
  o.stp = Stp::NONE;  // the venue lists no self-trade prevention
  *sync = b.value("synchronousExecution", false);
  *out = o;
  return "";
}

FakePoly::Reply FakePoly::create_locked(const json& body, std::int64_t now, std::string* created) {
  Submitted sub;
  int status = 400;
  const std::string why = read_create_locked(body, now, &sub.order, &sub.sync, &status);
  if (!why.empty()) return Reply{status, error_body(status, why)};
  sub.id = uuid4(rng_);
  sub.sent = body;
  sub.created_ms = sub.updated_ms = now;
  sub.due_ms = sub.sync ? now : now + cfg_.async_delay_ms;
  const std::string id = sub.id;
  Submitted& s = submitted_.emplace(id, std::move(sub)).first->second;
  if (!s.sync) {
    pending_.push_back(id);
    *created = id;
    take_due_locked(now);  // no delay: the engine has it before the reply, which still says nothing of it
    return Reply{200, json{{"id", id}, {"executions", json::array()}}.dump()};
  }
  // synchronous: the engine's answer is the reply's
  const std::size_t before = engine_.fills().size();
  const CreateResult r = take_locked(s, now);
  if (r.err) {
    submitted_.erase(id);
    return Reply{r.err.http, error_body(r.err.http, r.err.details.empty() ? r.err.message : r.err.details)};
  }
  *created = id;
  const Order* o = engine_.order(r.order_id);
  const json state = o ? order_json(*o)["state"] : json("ORDER_STATE_NEW");
  json executions = json::array();
  const std::vector<Fill>& fills = engine_.fills();
  for (std::size_t i = before; i < fills.size(); ++i) {
    const Fill& f = fills[i];
    if (f.order_id != r.order_id) continue;  // the maker of a self-trade is another order
    executions.push_back(json{{"tradeId", f.fill_id},
                              {"lastShares", qty_str(f.count)},
                              {"lastPx", price(f.yes_px)},
                              {"commissionNotionalCollected", money(f.fee)},
                              {"order", {{"state", state}}}});
  }
  return Reply{200, json{{"id", id}, {"executions", executions}}.dump()};
}

FakePoly::Reply FakePoly::preview_locked(const json& body, std::int64_t now) {
  if (!body.is_object() || body.size() != 1 || !body.contains("request"))
    return Reply{400, error_body(400, "the body must be {request: <order>}")};
  NewOrder o;
  bool sync = false;
  int status = 400;
  const std::string why = read_create_locked(body["request"], now, &o, &sync, &status);
  if (!why.empty()) return Reply{status, error_body(status, why)};
  const int bps = listing_.at(o.ticker).coeff_micro / 100;  // 0.06 -> 600
  return Reply{200, json{{"order", {{"id", ""}, {"commissionsBasisPoints", std::to_string(bps)}}}}.dump()};
}

FakePoly::Reply FakePoly::cancel_locked(const std::string& id, const json& body, std::int64_t now) {
  if (!body.is_object() || !body.contains("marketSlug") || !body["marketSlug"].is_string())
    return Reply{400, error_body(400, "marketSlug is required")};
  const std::string slug = body["marketSlug"].get<std::string>();
  const Reply gone{404, error_body(404, "order not found")};
  std::string eid = id;
  auto it = submitted_.find(id);
  if (it != submitted_.end()) {
    Submitted& s = it->second;
    if (s.order.ticker != slug) return gone;
    if (s.state == Submitted::State::PENDING) {  // the engine never sees it
      s.state = Submitted::State::CANCELED;
      s.updated_ms = now;
      pending_.erase(std::remove(pending_.begin(), pending_.end(), id), pending_.end());
      return Reply{200, "{}"};
    }
    if (s.state != Submitted::State::TAKEN) return gone;
    eid = s.engine_id;
  } else if (by_engine_.count(id)) {
    return gone;  // ours go by our id
  }
  const Order* o = engine_.order(eid);
  if (!o || o->ticker != slug) return gone;
  const CancelResult r = engine_.cancel(eid, slug, -2, 0, now);  // a filled or cancelled order: 404
  if (r.err) return Reply{r.err.http, error_body(r.err.http, r.err.message)};
  return Reply{200, "{}"};
}

FakePoly::Reply FakePoly::cancel_all_locked(const json& body, std::int64_t now) {
  if (!body.is_null() && !body.is_object()) return Reply{400, error_body(400, "the body must be a JSON object")};
  json ids = json::array();
  for (const std::string& id : open_ids_locked()) ids.push_back(id);
  for (const std::string& id : pending_) {
    Submitted& s = submitted_.at(id);
    s.state = Submitted::State::CANCELED;
    s.updated_ms = now;
  }
  pending_.clear();
  engine_.cancel_all(0, now);
  return Reply{200, json{{"canceledOrderIds", ids}}.dump()};
}

std::vector<std::string> FakePoly::open_ids_locked() const {
  std::vector<std::pair<std::int64_t, std::string>> open;  // (created, id)
  for (const Order* o : engine_.user_orders()) {
    if (o->status != OrdStatus::RESTING) continue;
    auto it = by_engine_.find(o->order_id);
    if (it == by_engine_.end())
      open.emplace_back(o->created_ms, o->order_id);
    else
      open.emplace_back(submitted_.at(it->second).created_ms, it->second);
  }
  for (const std::string& id : pending_) open.emplace_back(submitted_.at(id).created_ms, id);
  std::stable_sort(open.begin(), open.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> out;
  for (auto& [ms, id] : open) {
    (void)ms;
    out.push_back(std::move(id));
  }
  return out;
}

json FakePoly::order_view_locked(const std::string& id) const {
  auto it = submitted_.find(id);
  if (it != submitted_.end()) {
    if (it->second.state != Submitted::State::TAKEN) return submitted_json(it->second);
    const Order* o = engine_.order(it->second.engine_id);
    return o ? order_json(*o) : json();
  }
  if (by_engine_.count(id)) return json();  // ours go by our id
  const Order* o = engine_.order(id);       // the street's orders have no id
  return o ? order_json(*o) : json();
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
  // one made over the wire goes by our id, and was created when the venue accepted it
  auto ours = by_engine_.find(o.order_id);
  const Submitted* sub = ours == by_engine_.end() ? nullptr : &submitted_.at(ours->second);
  const char* state = resting ? (o.filled > 0 ? "ORDER_STATE_PARTIALLY_FILLED" : "ORDER_STATE_NEW")
                      : o.status == OrdStatus::EXECUTED ? "ORDER_STATE_FILLED"
                      : o.reason == "ExpiryCancel"      ? "ORDER_STATE_EXPIRED"
                                                        : "ORDER_STATE_CANCELED";
  json j{{"id", sub ? sub->id : o.order_id},
         {"marketSlug", o.ticker},
         {"type", "ORDER_TYPE_LIMIT"},
         {"side", buy ? "ORDER_SIDE_BUY" : "ORDER_SIDE_SELL"},
         {"tif", tif_wire(o.tif, o.expiration_s > 0)},
         {"state", state},
         {"price", price(o.price)},
         {"quantity", qty_num(o.initial)},
         {"cumQuantity", qty_num(o.filled)},
         {"leavesQuantity", qty_num(resting ? o.remaining : 0)},
         {"commissionNotionalTotalCollected", money(o.taker_fees + o.maker_fees)},
         {"createTime", rfc3339(sub ? sub->created_ms : o.created_ms)},
         {"updateTime", rfc3339(o.updated_ms)}};
  if (o.filled > 0) {
    // the engine keeps what the side bought cost: a SELL bought NO at 1 - p
    const Micros per = (o.taker_cost + o.maker_cost) * kContract / o.filled;
    j["avgPx"] = money(buy ? per : kMicrosPerDollar - per);
  }
  return j;
}

json FakePoly::submitted_json(const Submitted& s) const {
  const bool pending = s.state == Submitted::State::PENDING;
  const char* state = pending                                    ? "ORDER_STATE_PENDING_NEW"
                      : s.state == Submitted::State::REJECTED    ? "ORDER_STATE_REJECTED"
                                                                 : "ORDER_STATE_CANCELED";
  return json{{"id", s.id},
              {"marketSlug", s.order.ticker},
              {"type", "ORDER_TYPE_LIMIT"},
              {"side", s.order.side == Side::BID ? "ORDER_SIDE_BUY" : "ORDER_SIDE_SELL"},
              {"tif", tif_wire(s.order.tif, s.order.expiration_s > 0)},
              {"state", state},
              {"price", price(s.order.price)},
              {"quantity", qty_num(s.order.count)},
              {"cumQuantity", 0},
              {"leavesQuantity", qty_num(pending ? s.order.count : 0)},
              {"commissionNotionalTotalCollected", money(0)},
              {"createTime", rfc3339(s.created_ms)},
              {"updateTime", rfc3339(s.updated_ms)}};
}

}  // namespace fakex
