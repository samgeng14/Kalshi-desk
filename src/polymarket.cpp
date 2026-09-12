#include "desk/polymarket.h"
#include "desk/timeutil.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace desk {
namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
const char* kPublic = "https://gateway.polymarket.us";
const char* kPrivate = "https://api.polymarket.us";
long long now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string string_at(const json& j, const char* k) {
  return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : "";
}
bool bool_at(const json& j, const char* k, bool fallback = false) {
  return j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : fallback;
}
std::optional<double> number(const json& j) {
  double d;
  if (j.is_number()) d = j.get<double>();
  else if (j.is_string()) {
    auto s = j.get<std::string>();
    if (s.empty() || s.size() > 64 || s.find_first_not_of("0123456789.-+") != std::string::npos) return {};
    size_t n = 0;
    try { d = std::stod(s, &n); } catch (...) { return {}; }
    if (n != s.size()) return {};
  } else return {};
  return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}
std::optional<double> usd(const json& j) {
  if (!j.is_object() || string_at(j, "currency") != "USD" || !j.contains("value")) return {};
  return number(j["value"]);
}
json optional_number(std::optional<double> n) { return n ? json(*n) : json(nullptr); }
json empty_quotes() {
  json q = json::object();
  for (auto k : {"yes_bid", "yes_ask", "no_bid", "no_ask", "yes_bid_size", "yes_ask_size", "no_bid_size", "no_ask_size"}) q[k] = nullptr;
  return q;
}
Key key_from_secret(const std::string& secret) {
  // Strict canonical base64, accepting only the 32-byte seed or 64-byte seed+public key.
  if ((secret.size() != 44 && secret.size() != 88) || secret.find_first_not_of(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != std::string::npos)
    throw std::runtime_error("Invalid Polymarket US secret key encoding");
  std::vector<unsigned char> decoded(secret.size());
  int n = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(secret.data()), static_cast<int>(secret.size()));
  if (n > 0) { if (secret.back() == '=') --n; if (secret[secret.size() - 2] == '=') --n; }
  std::vector<unsigned char> canonical(secret.size() + 1);
  if (n != 32 && n != 64) {
    OPENSSL_cleanse(decoded.data(), decoded.size());
    throw std::runtime_error("Polymarket US secret key must decode to 32 or 64 bytes");
  }
  int len = EVP_EncodeBlock(canonical.data(), decoded.data(), n);
  bool valid = len == static_cast<int>(secret.size()) && std::equal(secret.begin(), secret.end(), canonical.begin());
  OPENSSL_cleanse(canonical.data(), canonical.size());
  Key key(valid ? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, decoded.data(), 32) : nullptr, EVP_PKEY_free);
  OPENSSL_cleanse(decoded.data(), decoded.size());
  if (!key) throw std::runtime_error("Invalid Polymarket US Ed25519 key");
  return key;
}
std::string sign(EVP_PKEY* key, const std::string& method, const std::string& path, long long ts) {
  if (path.empty() || path.front() != '/' || path.find_first_of("\r\n#") != std::string::npos ||
      method.empty() || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos || ts <= 0)
    throw std::runtime_error("Invalid Polymarket US signing request");
  std::string message = std::to_string(ts) + method + path.substr(0, path.find('?'));
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  unsigned char signature[64]; size_t len = sizeof(signature);
  if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key) != 1 ||
      EVP_DigestSign(ctx.get(), signature, &len, reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1 || len != 64)
    throw std::runtime_error("Polymarket US signing failed");
  unsigned char encoded[89];
  int written = EVP_EncodeBlock(encoded, signature, static_cast<int>(len));
  return std::string(reinterpret_cast<char*>(encoded), written);
}
std::string safe_slug(const std::string& slug) {
  if (slug.empty() || slug.size() > 240 || slug.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
    throw std::runtime_error("Invalid Polymarket US market slug");
  return slug;
}
}  // namespace

json polymarket_fee_metadata(const json& m) {
  // Source: https://docs.polymarket.us/fees (verified 2026-09-08, effective 2026-07-01).
  // Use the market's explicit coefficient; an absent field never silently means zero.
  auto theta = m.contains("feeCoefficient") ? number(m["feeCoefficient"]) : std::nullopt;
  bool known = theta && *theta >= 0 && *theta <= 1;
  return {{"known", known}, {"theta", known ? json(*theta) : json(nullptr)},
          {"formula", "theta * contracts * price * (1 - price)"},
          {"rounding", "nearest_cent_ties_even"}, {"source", "https://docs.polymarket.us/fees"},
          {"coefficient_source", "market.feeCoefficient"}, {"verified_date", "2026-09-08"},
          {"rebates_assumed", false}};
}

double polymarket_taker_fee(double theta, double contracts, double price) {
  if (!std::isfinite(theta) || !std::isfinite(contracts) || !std::isfinite(price) || theta < 0 ||
      theta > 1 || contracts < 0 || contracts > 1e12 || price <= 0 || price >= 1)
    throw std::runtime_error("Invalid Polymarket US fee inputs");
  double cents = theta * contracts * price * (1 - price) * 100;
  double lo = std::floor(cents), frac = cents - lo;
  double rounded = frac > .5 + 1e-9 ? lo + 1 : (frac < .5 - 1e-9 ? lo : (std::fmod(lo, 2) == 0 ? lo : lo + 1));
  return rounded / 100;
}

json normalize_polymarket_market(const json& response, long long received_ts) {
  const json& m = response.is_object() && response.contains("market") ? response["market"] : response;
  if (!m.is_object() || string_at(m, "slug").empty()) throw std::runtime_error("Malformed Polymarket US market response");
  const auto slug = safe_slug(string_at(m, "slug"));
  std::string yes_label, no_label, yes_team, no_team;
  if (m.contains("marketSides") && m["marketSides"].is_array()) for (const auto& s : m["marketSides"]) {
    if (!s.is_object() || !s.contains("long") || !s["long"].is_boolean()) continue;
    auto& label = s["long"].get<bool>() ? yes_label : no_label;
    auto& team = s["long"].get<bool>() ? yes_team : no_team;
    label = string_at(s, "description");
    if (s.contains("team") && s["team"].is_object()) team = string_at(s["team"], "name");
  }
  json quotes = empty_quotes();
  for (auto field : {"bestBidQuote", "bestAskQuote"}) {
    auto p = m.contains(field) ? usd(m[field]) : std::nullopt;
    if (!p || *p <= 0 || *p >= 1) continue;
    bool bid = std::string(field) == "bestBidQuote";
    quotes[bid ? "yes_bid" : "yes_ask"] = *p;
    quotes[bid ? "no_ask" : "no_bid"] = 1 - *p;
  }
  const auto end_ts = parse_iso(string_at(m, "endDate"));
  bool active = bool_at(m, "active") && !bool_at(m, "closed") && !bool_at(m, "archived");
  std::string raw_status = string_at(m, "status");
  auto ep3_status = string_at(m, "ep3Status");
  if ((!raw_status.empty() && raw_status != "MARKET_STATUS_OPEN") || (!ep3_status.empty() && ep3_status != "OPEN")) active = false;
  json row = {{"venue", "polymarket_us"}, {"id", string_at(m, "id")}, {"slug", slug},
    {"title", string_at(m, "question").empty() ? string_at(m, "title") : string_at(m, "question")},
    {"subtitle", string_at(m, "title")}, {"description", string_at(m, "description")},
    {"rules", string_at(m, "description") + (string_at(m,"rulesDisclaimer").empty()?"":"\n" + string_at(m,"rulesDisclaimer"))}, {"rules_disclaimer", string_at(m, "rulesDisclaimer")},
    {"url", "https://polymarket.us/market/" + slug}, {"category", string_at(m, "category")},
    {"market_type", string_at(m, "marketType")}, {"yes_label", yes_label}, {"no_label", no_label},
    {"yes_team", yes_team}, {"no_team", no_team}, {"outcomes_known", !yes_label.empty() && !no_label.empty()},
    {"close_ts", end_ts}, {"end_ts", end_ts}, {"close_ts_source", "endDate"},
    {"settlement_ts", 0}, {"settlement_time_known", false},
    {"game_start_ts", parse_iso(string_at(m, "gameStartTime"))},
    {"status", active ? "open" : "unavailable"}, {"raw_status", raw_status}, {"quotes", quotes},
    {"quote_ts", 0}, {"source_ts", parse_iso(string_at(m, "updatedAt"))},
    {"fetched_ts", received_ts}, {"quote_kind", "indicative_metadata"}, {"fees", polymarket_fee_metadata(m)},
    {"rules_verified", false}};
  row["minimum_quantity"] = m.contains("minimumTradeQty") ? optional_number(number(m["minimumTradeQty"])) : json(nullptr);
  row["tick_size"] = m.contains("orderPriceMinTickSize") ? optional_number(number(m["orderPriceMinTickSize"])) : json(nullptr);
  return row;
}

json normalize_polymarket_book(const json& response, const std::string& slug, long long received_ts) {
  json out = {{"venue", "polymarket_us"}, {"slug", slug}, {"quotes", empty_quotes()},
              {"book", {{"yes_bids", json::array()}, {"yes_asks", json::array()}, {"no_bids", json::array()}, {"no_asks", json::array()}}},
              {"source_ts", 0}, {"quote_ts", received_ts}, {"state", "unknown"}, {"valid", false}, {"error", ""}};
  auto fail = [&](const char* error) { out["valid"] = false; out["error"] = error; out["quotes"] = empty_quotes();
    for (auto& item : out["book"].items()) item.value() = json::array(); return out; };
  if (!response.is_object() || !response.contains("marketData") || !response["marketData"].is_object()) return fail("Missing L2 marketData");
  const auto& b = response["marketData"];
  if (string_at(b, "marketSlug") != slug) return fail("L2 response market does not match requested slug");
  out["state"] = string_at(b, "state");
  out["source_ts"] = parse_iso(string_at(b, "transactTime"));
  if (out["source_ts"].get<long long>() <= 0 || out["source_ts"].get<long long>() > received_ts + 5) return fail("Missing or future exchange quote timestamp");
  if (out["state"] != "MARKET_STATE_OPEN") return fail("Market book is not open");
  for (auto field : {"bids", "offers"}) {
    // Empty protobuf arrays can be omitted, but malformed levels invalidate the entire snapshot.
    if (!b.contains(field)) continue;
    if (!b[field].is_array()) return fail("Malformed L2 levels");
    bool bid = std::string(field) == "bids";
    std::map<double, double> levels;
    for (const auto& l : b[field]) {
      if (!l.is_object() || !l.contains("px") || !l.contains("qty")) return fail("Malformed L2 level");
      auto p = usd(l["px"]), q = number(l["qty"]);
      if (!p || !q || *p <= 0 || *p >= 1 || *q < 0 || *q > 1e12) return fail("Invalid L2 price, currency, or quantity");
      if (*q == 0) continue;
      levels[*p] += *q;
    }
    std::vector<std::pair<double, double>> sorted(levels.begin(), levels.end());
    if (bid) std::reverse(sorted.begin(), sorted.end());
    for (const auto& l : sorted) {
      out["book"][bid ? "yes_bids" : "yes_asks"].push_back({{"price", l.first}, {"qty", l.second}});
      out["book"][bid ? "no_asks" : "no_bids"].push_back({{"price", 1-l.first}, {"qty", l.second}});
    }
    if (sorted.empty()) continue;
    auto& q = out["quotes"];
    q[bid ? "yes_bid" : "yes_ask"] = sorted[0].first;
    q[bid ? "no_ask" : "no_bid"] = 1 - sorted[0].first;
    q[bid ? "yes_bid_size" : "yes_ask_size"] = sorted[0].second;
    q[bid ? "no_ask_size" : "no_bid_size"] = sorted[0].second;
  }
  const auto& q = out["quotes"];
  if (q["yes_bid"].is_number() && q["yes_ask"].is_number() && q["yes_bid"].get<double>() >= q["yes_ask"].get<double>()) return fail("Crossed or locked L2 snapshot");
  out["valid"] = true;
  out["quote_kind"] = "level2";
  return out;
}

struct PolymarketClient::Auth {
  std::string id;
  Key key;
  Auth(std::string i, const std::string& secret) : id(std::move(i)), key(key_from_secret(secret)) {}
};
PolymarketClient::PolymarketClient(std::string ua, std::string id, std::string secret) : http_(std::move(ua), 8) {
  if (id.empty() != secret.empty()) { OPENSSL_cleanse(secret.data(), secret.size()); throw std::runtime_error("Both Polymarket US credential fields are required"); }
  try {
    if (!id.empty()) {
      if (id.size() > 128 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::runtime_error("Invalid Polymarket US key identifier");
      auth_ = std::make_unique<Auth>(std::move(id), secret);
    }
  } catch (...) { OPENSSL_cleanse(secret.data(), secret.size()); throw; }
  OPENSSL_cleanse(secret.data(), secret.size());
}
PolymarketClient::~PolymarketClient() = default;
PolymarketClient::PolymarketClient(PolymarketClient&&) noexcept = default;
PolymarketClient& PolymarketClient::operator=(PolymarketClient&&) noexcept = default;
bool PolymarketClient::credential_status() const { return static_cast<bool>(auth_); }
std::string polymarket_us_signature(const std::string& secret, const std::string& method, const std::string& path, long long timestamp_ms) {
  auto key = key_from_secret(secret); return sign(key.get(), method, path, timestamp_ms);
}

PolymarketClient PolymarketClient::from_environment(const std::string& ua, const std::string& file) {
  const char* id = std::getenv("POLYMARKET_US_KEY_ID");
  const char* key = std::getenv("POLYMARKET_US_SECRET_KEY");
  if ((id && *id) || (key && *key)) return PolymarketClient(ua, id ? id : "", key ? key : "");
  if (file.empty()) return PolymarketClient(ua);
  int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("Cannot open Polymarket US credential file");
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0077) != 0 || st.st_size <= 0 || st.st_size > 4096) {
    ::close(fd); throw std::runtime_error("Polymarket US credential file must be owner-only (0600), regular, and owned by the current user");
  }
  std::string body(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < body.size()) { auto n = ::read(fd, body.data() + offset, body.size() - offset); if (n <= 0) break; offset += static_cast<size_t>(n); }
  ::close(fd);
  // Never include parser exceptions (which can embed credential fragments) in user-visible errors.
  json j = json::parse(body, nullptr, false);
  OPENSSL_cleanse(body.data(), body.size());
  if (offset != body.size() || !j.is_object() || !j.contains("key_id") || !j["key_id"].is_string() ||
      !j.contains("secret_key") || !j["secret_key"].is_string()) throw std::runtime_error("Invalid Polymarket US credential file format");
  std::string secret = std::move(j["secret_key"].get_ref<std::string&>());
  return PolymarketClient(ua, j["key_id"].get<std::string>(), std::move(secret));
}

json PolymarketClient::get(const std::string& path, const std::string& query, bool authenticated) const {
  std::map<std::string, std::string> headers;
  if (authenticated) {
    if (!auth_) throw std::runtime_error("Polymarket US account is not connected");
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    headers = {{"X-PM-Access-Key", auth_->id}, {"X-PM-Timestamp", std::to_string(ts)}, {"X-PM-Signature", sign(auth_->key.get(), "GET", path, ts)}};
  }
  HttpResponse r;
  try { r = http_.get(std::string(authenticated ? kPrivate : kPublic) + path + query, headers); }
  catch (...) { throw std::runtime_error("Polymarket US network request failed"); }
  if (!r.ok()) throw std::runtime_error("Polymarket US GET failed (HTTP " + std::to_string(r.status) + ")");
  auto parsed = json::parse(r.body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) throw std::runtime_error("Malformed Polymarket US JSON response");
  return parsed;
}
json PolymarketClient::markets(int limit, int offset) const {
  if (limit < 1 || limit > 500 || offset < 0 || offset > 100000) throw std::runtime_error("Invalid Polymarket US pagination");
  auto j = get("/v1/markets", "?limit=" + std::to_string(limit) + "&offset=" + std::to_string(offset) + "&active=true&closed=false");
  if (!j.contains("markets") || !j["markets"].is_array()) throw std::runtime_error("Missing Polymarket US market list");
  auto ts = now_seconds(); json rows = json::array();
  for (const auto& m : j["markets"]) rows.push_back(normalize_polymarket_market(m, ts));
  return {{"markets", rows}, {"limit", limit}, {"offset", offset}, {"fetched_ts", ts}};
}
json PolymarketClient::market(const std::string& slug) const {
  auto j = get("/v1/market/slug/" + safe_slug(slug));
  auto result = normalize_polymarket_market(j, now_seconds());
  if (result["slug"] != slug) throw std::runtime_error("Polymarket US market response does not match requested slug");
  return result;
}
json PolymarketClient::book(const std::string& slug) const {
  auto j = get("/v1/markets/" + safe_slug(slug) + "/book");
  return normalize_polymarket_book(j, slug, now_seconds());
}
json PolymarketClient::account_summary() const {
  if (!auth_) return {{"connected", false}, {"read_only", true}, {"balances", json::array()}, {"positions", json::array()}};
  json balances = json::array(), positions = json::array();
  auto b = get("/v1/account/balances", "", true);
  if (!b.contains("balances") || !b["balances"].is_array()) throw std::runtime_error("Malformed Polymarket US balances");
  for (const auto& x : b["balances"]) {
    json row = {{"currency", string_at(x, "currency")}, {"updated_ts", parse_iso(string_at(x, "lastUpdated"))}};
    for (auto k : {"currentBalance", "buyingPower", "assetNotional", "assetAvailable", "openOrders", "unsettledFunds", "balanceReservation"}) row[k] = x.contains(k) ? optional_number(number(x[k])) : json(nullptr);
    balances.push_back(std::move(row));
  }
  std::string cursor; bool complete = false;
  for (int page = 0; page < 20; ++page) {
    auto p = get("/v1/portfolio/positions", "?limit=100" + (cursor.empty() ? "" : "&cursor=" + url_encode(cursor)), true);
    if (!p.contains("positions") || !p["positions"].is_object()) throw std::runtime_error("Malformed Polymarket US positions");
    for (const auto& x : p["positions"].items()) {
      const auto& v = x.value();
      json row = {{"slug", x.key()}, {"updated_ts", parse_iso(string_at(v, "updateTime"))}, {"expired", bool_at(v, "expired")}};
      for (auto k : {"netPosition", "qtyAvailable", "qtyBought", "qtySold"}) row[k] = v.contains(k) ? optional_number(number(v[k])) : json(nullptr);
      for (auto k : {"cost", "realized", "cashValue"}) row[k] = v.contains(k) ? optional_number(usd(v[k])) : json(nullptr);
      positions.push_back(std::move(row));
    }
    auto next = string_at(p, "nextCursor");
    if (bool_at(p, "eof") || next.empty()) { complete = true; break; }
    if (next == cursor) break;
    cursor = std::move(next);
  }
  json available = nullptr, cash = nullptr;
  for (const auto& row : balances) if (row.value("currency", "") == "USD") {
    available = row.value("buyingPower", json(nullptr));
    cash = row.value("currentBalance", json(nullptr));
    break;
  }
  return {{"connected", true}, {"read_only", true}, {"balances", balances}, {"positions", positions},
          {"available_cash", available}, {"balance_dollars", cash},
          {"positions_complete", complete}, {"fetched_ts", now_seconds()}};
}
}  // namespace desk
