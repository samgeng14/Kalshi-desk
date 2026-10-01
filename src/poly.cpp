#include "desk/poly.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "desk/auth.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/risk.h"
#include "desk/timeutil.h"

namespace desk {
namespace {
constexpr const char* kAlgoKey = "poly_algo:v1";
constexpr long long kQuoteFresh = 90;   // a Polymarket quote older than this is not a price

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
double dval(const json& j, const char* k, double dflt = 0) {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return dflt;
  const json& v = j[k];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) return parse_num(v.get<std::string>(), dflt);
  if (v.is_object() && v.contains("value")) return dval(v, "value", dflt);
  return dflt;
}
std::string sval(const json& j, const char* k, const std::string& dflt = "") {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return dflt;
  const json& v = j[k];
  return v.is_string() ? v.get<std::string>() : v.dump();
}
double amount(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || !j[k].is_object()) return 0;
  return parse_num(sval(j[k], "value", "0"));
}
long long iso_ts(const std::string& s) { return s.empty() ? 0 : parse_iso(s); }
std::string fmt3(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
double round_tick(double price, double tick) { return tick > 0 ? std::round(price / tick) * tick : price; }
double round4(double x) { return std::round(x * 10000.0) / 10000.0; }
// "Chicago WS", "San Jose St.", "Los Angeles R" -> normalized tokens.
std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : lower(s)) {
    if (std::isalnum(static_cast<unsigned char>(ch))) cur += ch;
    else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(cur);
  for (auto& w : out) if (w == "st") w = "state";
  return out;
}
}  // namespace

// ---- auth and client ----------------------------------------------------------------------------------

PolyAuth::PolyAuth(std::string key_id, const std::string& secret_b64) : key_id_(std::move(key_id)) {
  std::string raw = Signer::unbase64(trim(secret_b64));
  if (raw.size() != 32 && raw.size() != 64) throw std::runtime_error("Polymarket secret must decode to 32 or 64 bytes");
  pkey_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(raw.data()), 32);
  std::fill(raw.begin(), raw.end(), '\0');
  if (!pkey_) throw std::runtime_error("could not load the Polymarket Ed25519 key");
}
PolyAuth::~PolyAuth() { if (pkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_)); }

std::string PolyAuth::sign(const std::string& text) const {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) throw std::runtime_error("EVP_MD_CTX_new");
  unsigned char sig[64];
  size_t len = sizeof sig;
  bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, static_cast<EVP_PKEY*>(pkey_)) == 1 &&
            EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(text.data()), text.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok) throw std::runtime_error("Ed25519 signing failed");
  return Signer::base64(sig, len);
}

std::map<std::string, std::string> PolyAuth::headers(const std::string& method, const std::string& path, long long timestamp_ms) const {
  std::string ts = std::to_string(timestamp_ms);
  return {{"X-PM-Access-Key", key_id_}, {"X-PM-Timestamp", ts}, {"X-PM-Signature", sign(ts + method + path)},
          {"Content-Type", "application/json"}};
}

std::string PolyAuth::public_key_b64() const {
  unsigned char pub[32];
  size_t len = sizeof pub;
  if (EVP_PKEY_get_raw_public_key(static_cast<EVP_PKEY*>(pkey_), pub, &len) != 1) return "";
  return Signer::base64(pub, len);
}

bool PolyAuth::verify(const std::string& public_key_b64, const std::string& text, const std::string& sig_b64) {
  std::string pub = Signer::unbase64(public_key_b64), sig = Signer::unbase64(sig_b64);
  if (pub.size() != 32 || sig.size() != 64) return false;
  EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(pub.data()), 32);
  if (!k) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
            EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                             reinterpret_cast<const unsigned char*>(text.data()), text.size()) == 1;
  if (ctx) EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(k);
  return ok;
}

PolyClient::PolyClient(std::string api_base, std::string gateway_base, std::shared_ptr<PolyAuth> auth, std::string user_agent)
    : api_base_(std::move(api_base)), gateway_base_(std::move(gateway_base)), auth_(std::move(auth)), http_(std::move(user_agent), 20) {}

json PolyClient::call(const std::string& method, const std::string& path, const std::vector<std::pair<std::string, std::string>>& query,
                      const json* body, bool authenticated) {
  if (authenticated && !auth_) throw std::runtime_error("Polymarket: no API key configured");
  std::string url = (authenticated ? api_base_ : gateway_base_) + path;
  bool first = true;
  for (const auto& [k, v] : query) {
    url += (first ? "?" : "&") + k + "=" + url_encode(v);
    first = false;
  }
  HttpResponse r;
  int attempts = method == "GET" ? 3 : 1;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (authenticated) headers = auth_->headers(method, path, now_ms());
    {
      // 20 requests a second per key: keep this process well under it.
      std::lock_guard<std::mutex> lk(mu_);
      long long now = now_ms();
      if (now - last_call_ms_ < 80) std::this_thread::sleep_for(std::chrono::milliseconds(80 - (now - last_call_ms_)));
      last_call_ms_ = now_ms();
    }
    if (method == "GET") r = http_.get(url, headers);
    else if (method == "POST") r = http_.post(url, body ? body->dump() : "{}", headers);
    else if (method == "DELETE") r = http_.del(url, headers);
    else throw std::runtime_error("unsupported method " + method);
    if (r.ok()) break;
    bool retryable = r.status == 429 || r.status >= 500;
    if (!retryable || attempt == attempts) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(600 * attempt));
  }
  if (!r.ok()) throw ApiError(r.status, r.body, "Polymarket " + method + " " + path + " -> HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 300));
  if (r.body.empty()) return json::object();
  json j = json::parse(r.body, nullptr, false);
  return j.is_discarded() ? json::object() : j;
}

// ---- parsing ----------------------------------------------------------------------------------------------

PolyMarket parse_poly_market(const json& j, const std::string& league, const std::string& event_slug, const std::string& event_title) {
  PolyMarket m;
  m.slug = sval(j, "slug");
  m.event_slug = event_slug;
  m.event_title = event_title;
  m.question = sval(j, "question", sval(j, "title"));
  m.league = league;
  m.sports_type = sval(j, "sportsMarketType");
  m.tick = dval(j, "orderPriceMinTickSize", 0.01);
  if (m.tick <= 0) m.tick = 0.01;
  m.min_qty = dval(j, "minimumTradeQty", 1);
  if (m.min_qty <= 0) m.min_qty = 1;
  m.fee_coeff = dval(j, "feeCoefficient", 0.06);
  m.bid = amount(j, "bestBidQuote");
  m.ask = amount(j, "bestAskQuote");
  m.start_ts = iso_ts(sval(j, "gameStartTime", sval(j, "startDate")));
  m.end_ts = iso_ts(sval(j, "endDate"));
  m.updated_ts = iso_ts(sval(j, "updatedAt"));
  m.active = j.value("active", true);
  m.closed = j.value("closed", false);
  for (const auto& s : j.value("marketSides", json::array())) {
    PolySide side;
    side.description = sval(s, "description");
    side.is_long = s.value("long", false);
    side.team_id = static_cast<int>(dval(s, "teamId"));
    if (s.contains("team") && s["team"].is_object()) {
      const json& t = s["team"];
      side.abbr = lower(sval(t, "abbreviation"));
      side.name = sval(t, "name");
      side.safe_name = sval(t, "safeName");
      side.alias = sval(t, "alias");
    }
    m.sides.push_back(side);
  }
  std::stable_sort(m.sides.begin(), m.sides.end(), [](const PolySide& a, const PolySide& b) { return a.is_long && !b.is_long; });
  return m;
}

PolyPosition parse_poly_position(const std::string& slug, const json& j) {
  PolyPosition p;
  p.slug = slug;
  p.net = j.contains("netPositionDecimal") ? parse_num(sval(j, "netPositionDecimal", "0")) : parse_num(sval(j, "netPosition", "0"));
  p.cost = amount(j, "cost");
  p.realized = amount(j, "realized");
  p.unrealized = amount(j, "cashValue");
  p.available = j.contains("qtyAvailableDecimal") ? parse_num(sval(j, "qtyAvailableDecimal", "0")) : std::fabs(p.net);
  p.expired = j.value("expired", false);
  p.update_ts = iso_ts(sval(j, "updateTime"));
  return p;
}

PolyBalance parse_poly_balance(const json& j) {
  PolyBalance b;
  const json* row = nullptr;
  if (j.contains("balances") && j["balances"].is_array())
    for (const auto& x : j["balances"]) if (sval(x, "currency", "USD") == "USD") { row = &x; break; }
  if (!row && j.contains("balances") && j["balances"].is_array() && !j["balances"].empty()) row = &j["balances"][0];
  if (!row) row = &j;
  b.current_balance = dval(*row, "currentBalance");
  b.buying_power = dval(*row, "buyingPower");
  b.asset_notional = dval(*row, "assetNotional");
  b.asset_available = dval(*row, "assetAvailable");
  b.updated_ts = now_s();
  return b;
}

PolyOrder parse_poly_order(const json& j) {
  PolyOrder o;
  o.id = sval(j, "id");
  o.slug = sval(j, "marketSlug");
  o.side = sval(j, "side");
  if (o.side.rfind("ORDER_SIDE_", 0) == 0) o.side = o.side.substr(11);
  o.type = sval(j, "type");
  o.tif = sval(j, "tif");
  o.intent = sval(j, "intent");
  o.state = sval(j, "state");
  if (o.state.rfind("ORDER_STATE_", 0) == 0) o.state = lower(o.state.substr(12));
  o.price = amount(j, "price");
  o.quantity = dval(j, "quantity");
  o.cum = dval(j, "cumQuantity");
  o.leaves = dval(j, "leavesQuantity");
  o.avg_px = amount(j, "avgPx");
  o.commission = amount(j, "commissionNotionalTotalCollected");
  o.created_ts = iso_ts(sval(j, "createTime", sval(j, "createdAt")));
  o.updated_ts = iso_ts(sval(j, "updateTime", sval(j, "updatedAt")));
  return o;
}

PolyTrade parse_poly_trade(const json& j) {
  PolyTrade t;
  t.id = sval(j, "id");
  t.slug = sval(j, "marketSlug");
  t.state = sval(j, "state");
  t.price = amount(j, "price");
  t.qty = j.contains("qtyDecimal") ? parse_num(sval(j, "qtyDecimal", "0")) : parse_num(sval(j, "qty", "0"));
  t.cost_basis = amount(j, "costBasis");
  t.realized = amount(j, "realizedPnl");
  t.aggressor = j.value("isAggressor", false);
  t.ts = iso_ts(sval(j, "createTime"));
  return t;
}

json poly_order_body(const PolyOrderRequest& r) {
  std::string tif = r.tif == "IOC" ? "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL" : r.tif == "FOK" ? "TIME_IN_FORCE_FILL_OR_KILL" : "TIME_IN_FORCE_GOOD_TILL_CANCEL";
  json b{{"marketSlug", r.slug}, {"type", "ORDER_TYPE_LIMIT"}, {"price", {{"value", fmt3(r.price)}, {"currency", "USD"}}},
         {"quantity", r.quantity}, {"tif", tif}, {"outcomeSide", "OUTCOME_SIDE_YES"},
         {"action", r.action == "SELL" ? "ORDER_ACTION_SELL" : "ORDER_ACTION_BUY"},
         {"manualOrderIndicator", r.manual ? "MANUAL_ORDER_INDICATOR_MANUAL" : "MANUAL_ORDER_INDICATOR_AUTOMATIC"},
         {"synchronousExecution", true}, {"maxBlockTime", "3"}};
  if (r.post_only) b["participateDontInitiate"] = true;
  return b;
}

double poly_fee(double p, bool taker, double coeff) {
  if (p <= 0 || p >= 1) return 0;
  return (taker ? coeff : -0.0125) * p * (1 - p);
}

std::string poly_league_for_series(const std::string& s) {
  static const std::map<std::string, std::string> m = {
      {"KXNFLGAME", "nfl"}, {"KXNCAAFGAME", "cfb"}, {"KXMLBGAME", "mlb"}, {"KXNBAGAME", "nba"}, {"KXNHLGAME", "nhl"},
      {"KXWNBAGAME", "wnba"}, {"KXEPLGAME", "epl"}, {"KXLALIGAGAME", "lal"}, {"KXSERIEAGAME", "sea"},
      {"KXBUNDESLIGAGAME", "bun"}, {"KXLIGUE1GAME", "lg1"}, {"KXMLSGAME", "mls"}, {"KXUCLGAME", "ucl"},
      {"KXNCAAMBGAME", "cbb"}, {"KXNCAAWBGAME", "wcbb"}};
  auto it = m.find(s);
  return it == m.end() ? "" : it->second;
}

bool parse_kalshi_game_event(const std::string& ev, std::string* date, std::string* away, std::string* home, std::string* hhmm) {
  // KXNFLGAME-26SEP09NESEA, KXMLBGAME-26SEP112010CLEMIN (with a time), KXNCAAFGAME-26SEP19PURUCLA.
  size_t dash = ev.find('-');
  if (dash == std::string::npos || ev.size() < dash + 8) return false;
  std::string rest = ev.substr(dash + 1);
  static const std::map<std::string, int> mon = {{"JAN", 1}, {"FEB", 2}, {"MAR", 3}, {"APR", 4}, {"MAY", 5}, {"JUN", 6},
                                                 {"JUL", 7}, {"AUG", 8}, {"SEP", 9}, {"OCT", 10}, {"NOV", 11}, {"DEC", 12}};
  if (rest.size() < 7 || !std::isdigit(static_cast<unsigned char>(rest[0])) || !std::isdigit(static_cast<unsigned char>(rest[1]))) return false;
  auto mi = mon.find(rest.substr(2, 3));
  if (mi == mon.end() || !std::isdigit(static_cast<unsigned char>(rest[5])) || !std::isdigit(static_cast<unsigned char>(rest[6]))) return false;
  char buf[16];
  std::snprintf(buf, sizeof buf, "20%s-%02d-%s", rest.substr(0, 2).c_str(), mi->second, rest.substr(5, 2).c_str());
  if (date) *date = buf;
  std::string teams = rest.substr(7);
  if (hhmm) *hhmm = "";
  // An optional HHMM (Eastern time) before the team codes: MLB and other daily leagues carry it.
  if (teams.size() >= 4 && std::isdigit(static_cast<unsigned char>(teams[0])) && std::isdigit(static_cast<unsigned char>(teams[1])) &&
      std::isdigit(static_cast<unsigned char>(teams[2])) && std::isdigit(static_cast<unsigned char>(teams[3]))) {
    if (hhmm) *hhmm = teams.substr(0, 4);
    teams = teams.substr(4);
  }
  if (away) *away = "";
  if (home) *home = "";
  // The split between the two codes is not marked; the caller resolves it against the market
  // tickers of the event, so only the concatenation is returned here (in `away`).
  if (away) *away = teams;
  return !teams.empty();
}

int poly_team_score(const std::string& kalshi_code, const std::string& kalshi_name, const PolySide& side) {
  std::string code = lower(kalshi_code);
  if (!code.empty() && code == side.abbr) return 3;
  std::vector<std::string> kw = words(kalshi_name), nw = words(side.name), sw = words(side.safe_name), aw = words(side.alias);
  if (kw.empty()) return 0;
  auto same = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return false;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
  };
  if (same(kw, sw) || same(kw, nw) || same(kw, aw)) return 2;
  int overlap = 0;
  for (const auto& w : kw) {
    if (w.size() < 4) continue;
    if (std::find(nw.begin(), nw.end(), w) != nw.end() || std::find(sw.begin(), sw.end(), w) != sw.end() ||
        std::find(aw.begin(), aw.end(), w) != aw.end()) ++overlap;
  }
  return overlap > 0 ? 1 : 0;
}

PolyConfig poly_config(const nlohmann::json& p) {
  PolyConfig c;
  if (!p.is_object()) return c;
  c.key_id = p.value("key_id", "");
  c.secret_path = p.value("secret_path", "");
  c.enabled = p.value("enabled", !c.key_id.empty() && !c.secret_path.empty());
  c.api_base = p.value("api_base", c.api_base);
  c.gateway_base = p.value("gateway_base", c.gateway_base);
  if (p.contains("leagues") && p["leagues"].is_array()) {
    c.leagues.clear();
    for (const auto& l : p["leagues"]) if (l.is_string()) c.leagues.push_back(l.get<std::string>());
  }
  c.max_order_dollars = p.value("max_order_dollars", c.max_order_dollars);
  c.max_total_dollars = p.value("max_total_dollars", c.max_total_dollars);
  c.price_band = p.value("price_band", c.price_band);
  c.refresh_seconds = p.value("refresh_seconds", c.refresh_seconds);
  c.universe_refresh_seconds = p.value("universe_refresh_seconds", c.universe_refresh_seconds);
  c.account_refresh_seconds = p.value("account_refresh_seconds", c.account_refresh_seconds);
  return c;
}

std::shared_ptr<PolyClient> make_poly_client(const PolyConfig& pc, const std::string& user_agent) {
  if (!pc.enabled) return nullptr;
  std::shared_ptr<PolyAuth> auth;
  if (!pc.key_id.empty() && !pc.secret_path.empty()) {
    std::string path = pc.secret_path;
    if (!path.empty() && path[0] == '~') {
      const char* home = std::getenv("HOME");
      if (home) path = std::string(home) + path.substr(1);
    }
    std::ifstream in(path);
    if (!in) {
      LOG_WARN("polymarket: secret file %s not readable; public data only", path.c_str());
    } else {
      std::stringstream ss;
      ss << in.rdbuf();
      try {
        auth = std::make_shared<PolyAuth>(pc.key_id, trim(ss.str()));
      } catch (const std::exception& e) {
        LOG_WARN("polymarket: %s; public data only", e.what());
      }
    }
  }
  return std::make_shared<PolyClient>(pc.api_base, pc.gateway_base, auth, user_agent);
}

// ---- the desk --------------------------------------------------------------------------------------------

PolyDesk::PolyDesk(const Config& cfg, Store& store, std::shared_ptr<PolyClient> client, Executor* exec, Hooks hooks)
    : cfg_(cfg), pc_(poly_config(cfg.strategy_params.is_object() && cfg.strategy_params.contains("polymarket")
                                     ? cfg.strategy_params["polymarket"] : json::object())),
      store_(store), client_(std::move(client)), exec_(exec), hooks_(std::move(hooks)) {
  store_.exec("CREATE TABLE IF NOT EXISTS poly_markets(slug TEXT PRIMARY KEY, event_slug TEXT, event_title TEXT, question TEXT, league TEXT, "
              "sports_type TEXT, tick REAL, min_qty REAL, fee_coeff REAL, bid REAL, ask REAL, start_ts INTEGER, end_ts INTEGER, quote_ts INTEGER, "
              "active INTEGER, closed INTEGER, sides TEXT, updated_ts INTEGER)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_quotes(id INTEGER PRIMARY KEY, ts INTEGER, slug TEXT, bid REAL, ask REAL)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_poly_quotes ON poly_quotes(slug, ts)");
  // Both venues' books for the same outcome, written in the same instant. Storing each side on
  // its own clock and joining later does not work: Polymarket was polled about every five
  // minutes against Kalshi's five seconds, so of 2,889 recorded Polymarket quotes only 713 had a
  // Kalshi quote within fifteen seconds, and only 68 usable consecutive pairs survived. That is
  // not enough to answer whether one venue leads the other, which is the question that decides
  // whether any of this is a single-leg trade on Kalshi rather than a two-legged arb.
  store_.exec("CREATE TABLE IF NOT EXISTS pair_ticks(id INTEGER PRIMARY KEY, ts INTEGER, slug TEXT, "
              "kalshi_ticker TEXT, pm_bid REAL, pm_ask REAL, k_bid REAL, k_ask REAL, "
              "k_bid_size REAL, k_ask_size REAL, k_quote_ts INTEGER, same_side INTEGER)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_pair_ticks ON pair_ticks(kalshi_ticker, ts)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_pairs(kalshi_ticker TEXT PRIMARY KEY, kalshi_event TEXT, kalshi_series TEXT, kalshi_team TEXT, "
              "slug TEXT, league TEXT, pm_team TEXT, pm_yes_is_kalshi_yes INTEGER, score INTEGER, matched_by TEXT, updated_ts INTEGER)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_positions(slug TEXT PRIMARY KEY, net REAL, cost REAL, realized REAL, unrealized REAL, "
              "available REAL, expired INTEGER, update_ts INTEGER, updated_ts INTEGER)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_orders(id TEXT PRIMARY KEY, slug TEXT, side TEXT, type TEXT, tif TEXT, intent TEXT, state TEXT, "
              "price REAL, quantity REAL, cum REAL, leaves REAL, avg_px REAL, commission REAL, note TEXT, origin TEXT, created_ts INTEGER, updated_ts INTEGER)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_trades(id TEXT PRIMARY KEY, slug TEXT, state TEXT, price REAL, qty REAL, cost_basis REAL, realized REAL, "
              "aggressor INTEGER, ts INTEGER)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_account(ts INTEGER PRIMARY KEY, current_balance REAL, buying_power REAL, asset_notional REAL, asset_available REAL)");
  store_.exec("CREATE TABLE IF NOT EXISTS poly_flows(id TEXT PRIMARY KEY, ts INTEGER, kind TEXT, amount REAL)");
  store_.exec("CREATE TABLE IF NOT EXISTS xvenue_trades(id INTEGER PRIMARY KEY, kalshi_ticker TEXT, kalshi_event TEXT, slug TEXT, league TEXT, "
              "route TEXT, mode TEXT, state TEXT, contracts REAL, kalshi_side TEXT, kalshi_style TEXT, kalshi_price REAL, kalshi_filled REAL, "
              "kalshi_cost REAL, kalshi_fee REAL, kalshi_order_id TEXT, kalshi_proposal_id INTEGER, pm_action TEXT, pm_style TEXT, pm_price REAL, "
              "pm_filled REAL, pm_cost REAL, pm_fee REAL, pm_order_id TEXT, locked REAL, expected_profit REAL, realized_pnl REAL, opened_ts INTEGER, "
              "kalshi_filled_ts INTEGER, pm_filled_ts INTEGER, closed_ts INTEGER, reason TEXT, note TEXT, day TEXT, pm_yes_is_kalshi_yes INTEGER, pm_mid_yes REAL)");
  algo_state_ = {{"running", false}, {"paper", true}, {"halted", false}, {"halt_reason", ""}, {"day", ""}, {"log", json::array()},
                 {"last_step_ts", 0},
                 {"settings", {{"min_lock", 0.01}, {"min_return", 0.01}, {"max_pair_dollars", 20.0}, {"max_total_dollars", 100.0},
                               {"max_pairs", 5}, {"contracts", 5.0}, {"kalshi_style", "maker"}, {"pm_style", "taker"},
                               {"kalshi_ttl_seconds", 240.0}, {"hedge_ttl_seconds", 120.0}, {"daily_loss_stop", 5.0},
                               {"min_hours_to_start", 0.5}, {"max_spread", 0.06}}}};
  if (auto saved = store_.kv_get(kAlgoKey)) {
    json parsed = json::parse(*saved, nullptr, false);
    if (parsed.is_object()) {
      json settings = algo_state_["settings"];
      if (parsed.contains("settings") && parsed["settings"].is_object()) settings.update(parsed["settings"]);
      algo_state_.update(parsed);
      algo_state_["settings"] = settings;
    }
  }
  if (algo_state_.value("running", false) && !algo_state_.value("paper", true) && (!armed() || !exec_)) {
    algo_state_["running"] = false;
    algo_log("warn", "pair rule stopped: live mode needs the Polymarket key and the Kalshi executor");
  }
  // The universe from the last run, so pairs exist before the first league fetch completes.
  for (const auto& r : store_.query("SELECT * FROM poly_markets WHERE closed=0")) {
    PolyMarket m;
    m.slug = r.value("slug", ""); m.event_slug = r.value("event_slug", ""); m.event_title = r.value("event_title", "");
    m.question = r.value("question", ""); m.league = r.value("league", ""); m.sports_type = r.value("sports_type", "");
    m.tick = r.value("tick", 0.01); m.min_qty = r.value("min_qty", 1.0); m.fee_coeff = r.value("fee_coeff", 0.06);
    m.bid = r.value("bid", 0.0); m.ask = r.value("ask", 0.0); m.start_ts = r.value("start_ts", 0LL); m.end_ts = r.value("end_ts", 0LL);
    m.quote_ts = r.value("quote_ts", 0LL); m.active = r.value("active", 1LL) != 0; m.closed = r.value("closed", 0LL) != 0;
    json sides = json::parse(r.value("sides", "[]"), nullptr, false);
    if (sides.is_array())
      for (const auto& s : sides) {
        PolySide side;
        side.description = s.value("description", ""); side.abbr = s.value("abbr", ""); side.name = s.value("name", "");
        side.safe_name = s.value("safe_name", ""); side.alias = s.value("alias", ""); side.is_long = s.value("is_long", false);
        side.team_id = s.value("team_id", 0);
        m.sides.push_back(side);
      }
    if (!m.slug.empty()) {
      markets_[m.slug] = m;
      stored_quote_[m.slug] = StoredQuote{m.bid, m.ask, m.active, m.closed, m.quote_ts};
    }
  }
  for (const auto& r : store_.query("SELECT * FROM poly_pairs")) {
    PolyPair p;
    p.kalshi_ticker = r.value("kalshi_ticker", ""); p.kalshi_event = r.value("kalshi_event", ""); p.kalshi_series = r.value("kalshi_series", "");
    p.kalshi_team = r.value("kalshi_team", ""); p.slug = r.value("slug", ""); p.league = r.value("league", ""); p.pm_team = r.value("pm_team", "");
    p.pm_yes_is_kalshi_yes = r.value("pm_yes_is_kalshi_yes", 1LL) != 0; p.score = static_cast<int>(r.value("score", 0LL));
    p.matched_by = r.value("matched_by", "");
    if (!p.kalshi_ticker.empty()) pairs_[p.kalshi_ticker] = p;
  }
  algo_persist();
}

void PolyDesk::algo_persist() { store_.kv_set(kAlgoKey, algo_state_.dump()); }

void PolyDesk::algo_log(const std::string& level, const std::string& msg) {
  algo_state_["log"].push_back(json{{"ts", now_s()}, {"level", level}, {"msg", msg}});
  while (algo_state_["log"].size() > 150) algo_state_["log"].erase(algo_state_["log"].begin());
  if (level != "info") store_.log_event(level == "trade" ? "info" : level, "POLY: " + msg);
  if (level == "error") LOG_ERR("poly: %s", msg.c_str());
  else LOG_INFO("poly: %s", msg.c_str());
}

std::optional<PolyMarket> PolyDesk::market(const std::string& slug) {
  Lock lk(mu_);   // the background thread inserts and erases markets_ under this lock
  auto it = markets_.find(slug);
  if (it == markets_.end()) return std::nullopt;
  return it->second;
}

std::map<std::string, PolyPair> PolyDesk::pairs_by_ticker() {
  Lock lk(mu_);
  return pairs_;
}

std::optional<PolyDesk::Quote> PolyDesk::quote_for(const std::string& kalshi_ticker, long long now) {
  Lock lk(mu_);
  auto p = pairs_.find(kalshi_ticker);
  if (p == pairs_.end()) return std::nullopt;
  auto m = markets_.find(p->second.slug);
  if (m == markets_.end() || !m->second.two_sided() || now - m->second.quote_ts > kQuoteFresh) return std::nullopt;
  Quote q;
  q.slug = p->second.slug;
  q.ts = m->second.quote_ts;
  q.flipped = !p->second.pm_yes_is_kalshi_yes;
  if (q.flipped) { q.bid = 1 - m->second.ask; q.ask = 1 - m->second.bid; }
  else { q.bid = m->second.bid; q.ask = m->second.ask; }
  return q;
}

// ---- refreshes ------------------------------------------------------------------------------------------

PolyDesk::~PolyDesk() { stop_background(); }

void PolyDesk::start_background() {
  if (!pc_.enabled || !client_ || bg_running_.load()) return;
  bg_stop_ = false;
  bg_running_ = true;
  bg_ = std::thread([this] {
    LOG_INFO("polymarket: refreshing on its own thread");
    while (!bg_stop_.load()) {
      // Each call checks its own cadence and returns at once when it is not due. None of them may
      // end the thread: a failure is logged and the next pass tries again.
      auto guard = [&](const char* what, auto&& fn) {
        try { fn(); } catch (const std::exception& e) {
          LOG_WARN("polymarket %s: %s", what, e.what());
          Lock lk(mu_);
          last_error_ = std::string(what) + ": " + e.what();
        } catch (...) {
          LOG_WARN("polymarket %s: unknown exception", what);
        }
      };
      // Each call gets the time it actually runs at: the universe refresh alone can take seconds,
      // and quote rows stamped with the pass's start would read older than they are.
      guard("universe", [&] { refresh_universe(now_s()); });
      guard("match", [&] { match(now_s()); });
      guard("quotes", [&] { refresh_quotes(now_s()); });
      guard("account", [&] { refresh_account(now_s()); });
      for (int i = 0; i < 10 && !bg_stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}

void PolyDesk::stop_background() {
  bg_stop_ = true;
  if (bg_.joinable()) bg_.join();
  bg_running_ = false;
  try {
    persist_quote_times();
  } catch (const std::exception& e) {
    LOG_WARN("polymarket: quote times not written at stop: %s", e.what());
  }
}

std::map<std::string, long long> PolyDesk::quote_poll_times() {
  Lock lk(mu_);
  return quote_poll_ts_;
}

void PolyDesk::persist_quote_times() {
  std::vector<std::pair<std::string, long long>> todo;
  {
    Lock lk(mu_);
    for (const auto& [slug, ts] : quote_poll_ts_) {
      auto st = stored_quote_.find(slug);
      if (st == stored_quote_.end() || st->second.quote_ts < ts) todo.emplace_back(slug, ts);
    }
  }
  if (todo.empty()) return;
  store_.transaction([&] {
    for (const auto& [slug, ts] : todo)
      store_.exec("UPDATE poly_markets SET quote_ts=? WHERE slug=? AND quote_ts<?", {ts, slug, ts});
  });
  Lock lk(mu_);
  for (const auto& [slug, ts] : todo)
    if (auto st = stored_quote_.find(slug); st != stored_quote_.end()) st->second.quote_ts = std::max(st->second.quote_ts, ts);
}

void PolyDesk::refresh_universe(long long now) {
  if (!pc_.enabled || !client_) return;
  {
    Lock lk(mu_);   // the background thread and the dashboard's sync can both get here
    if (now - universe_ts_ < pc_.universe_refresh_seconds) return;
    universe_ts_ = now;
  }
  std::vector<PolyMarket> found;
  std::string err;
  std::set<std::string> failed_leagues;   // their markets are kept, not treated as over
  for (const auto& lg : pc_.leagues) {
    for (int page = 0; page < 3; ++page) {
      json j;
      try {
        j = client_->call("GET", "/v2/leagues/" + lg + "/events", {{"limit", "200"}, {"offset", std::to_string(page * 200)}});
      } catch (const std::exception& e) {
        err = std::string(lg) + ": " + e.what();
        failed_leagues.insert(lg);
        LOG_WARN("poly league %s: %s", lg.c_str(), e.what());
        break;
      }
      const json& evs = j.value("events", json::array());
      for (const auto& ev : evs) {
        std::string es = sval(ev, "slug"), et = sval(ev, "title");
        for (const auto& mj : ev.value("markets", json::array())) {
          std::string type = sval(mj, "sportsMarketType");
          bool winner = type == "soccer_team_full_time_winner" || (type.find("_full_game_winner") != std::string::npos);
          if (!winner) continue;
          PolyMarket m = parse_poly_market(mj, lg, es, et);
          if (!m.slug.empty() && !m.closed) found.push_back(m);
        }
      }
      if (evs.size() < 200) break;
    }
  }
  Lock lk(mu_);
  last_error_ = err;
  std::set<std::string> seen;
  for (auto& m : found) {
    seen.insert(m.slug);
    auto it = markets_.find(m.slug);
    if (it != markets_.end()) { m.quote_ts = it->second.quote_ts; if (m.bid <= 0 && m.ask <= 0) { m.bid = it->second.bid; m.ask = it->second.ask; } }
    else if (m.bid > 0 || m.ask > 0) m.quote_ts = now;
    markets_[m.slug] = m;
    json sides = json::array();
    for (const auto& s : m.sides)
      sides.push_back({{"description", s.description}, {"abbr", s.abbr}, {"name", s.name}, {"safe_name", s.safe_name}, {"alias", s.alias},
                       {"is_long", s.is_long}, {"team_id", s.team_id}});
    store_.exec("INSERT INTO poly_markets(slug,event_slug,event_title,question,league,sports_type,tick,min_qty,fee_coeff,bid,ask,start_ts,end_ts,"
                "quote_ts,active,closed,sides,updated_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(slug) DO UPDATE SET "
                "event_slug=excluded.event_slug,event_title=excluded.event_title,question=excluded.question,league=excluded.league,"
                "sports_type=excluded.sports_type,tick=excluded.tick,min_qty=excluded.min_qty,fee_coeff=excluded.fee_coeff,start_ts=excluded.start_ts,"
                "end_ts=excluded.end_ts,active=excluded.active,closed=excluded.closed,sides=excluded.sides,updated_ts=excluded.updated_ts",
                {m.slug, m.event_slug, m.event_title, m.question, m.league, m.sports_type, m.tick, m.min_qty, m.fee_coeff, m.bid, m.ask,
                 m.start_ts, m.end_ts, m.quote_ts, static_cast<long long>(m.active ? 1 : 0), static_cast<long long>(m.closed ? 1 : 0),
                 sides.dump(), now});
    // This statement sets active and closed (and on a new row bid, ask and quote_ts), so what
    // refresh_quotes believes the row holds is no longer known: its next pass writes the row.
    stored_quote_.erase(m.slug);
  }
  // Markets that left the league feeds are over unless something is still held there.
  std::set<std::string> keep;
  for (const auto& p : positions_) keep.insert(p.slug);
  for (auto it = markets_.begin(); it != markets_.end();) {
    // Not listed because its league's fetch failed is not the same as over: an open pair may still
    // need that market to hedge.
    if (!seen.count(it->first) && !keep.count(it->first) && !found.empty() &&
        !failed_leagues.count(it->second.league)) {
      store_.exec("UPDATE poly_markets SET closed=1, updated_ts=? WHERE slug=?", {now, it->first});
      stored_quote_.erase(it->first);
      quote_poll_ts_.erase(it->first);
      it = markets_.erase(it);
    } else ++it;
  }
  match_ts_ = 0;   // re-match on the new universe
}

void PolyDesk::refresh_quotes(long long now) {
  if (!pc_.enabled || !client_) return;
  {
    Lock lk(mu_);
    if (now - quotes_ts_ < pc_.refresh_seconds) return;
    quotes_ts_ = now;
  }
  std::vector<std::string> slugs;
  {
    Lock lk(mu_);
    std::set<std::string> want;
    for (const auto& [t, p] : pairs_) want.insert(p.slug);
    for (const auto& p : positions_) want.insert(p.slug);
    for (const auto& r : store_.query("SELECT DISTINCT slug FROM poly_orders WHERE state IN ('new','partially_filled','pending_new')"))
      want.insert(r.value("slug", ""));
    for (const auto& r : store_.query("SELECT DISTINCT slug FROM xvenue_trades WHERE state NOT IN ('settled','closed','canceled')"))
      want.insert(r.value("slug", ""));
    slugs.assign(want.begin(), want.end());
  }
  if (slugs.empty()) return;
  for (size_t i = 0; i < slugs.size(); i += 40) {
    std::vector<std::pair<std::string, std::string>> q;
    for (size_t k = i; k < slugs.size() && k < i + 40; ++k) q.push_back({"slug", slugs[k]});
    json j;
    try {
      j = client_->call("GET", "/v1/markets", q);
    } catch (const std::exception& e) {
      Lock lk(mu_);
      last_error_ = e.what();
      LOG_WARN("poly quotes: %s", e.what());
      return;
    }
    // The batch's writes are collected under mu_ and run after it, in one transaction (2026-10-01).
    // They were one autocommit each, ~3 per slug every 10 s, and the poly_markets UPDATE ran for
    // every slug on every pass because quote_ts always changed: 4.5% of the desk's write-weighted
    // stack samples on 09-28. Now the UPDATE runs when bid, ask, active or closed differ from what
    // the row holds, or its quote_ts is 60 s behind (quote_ts lives in markets_, and quote_poll_ts_
    // keeps what the row would have held, for the xvenue strategy); the per-slug SELECT MAX(ts)
    // probe is a map, read from the table once per slug. The transaction never spans the HTTP call
    // and takes no lock of ours (Store::transaction), and our maps move only once it has committed,
    // as the rows they describe did.
    std::vector<std::pair<const char*, std::vector<SqlValue>>> writes;
    std::map<std::string, long long> logged, polled;
    std::map<std::string, StoredQuote> stored;
    {
      Lock lk(mu_);
      for (const auto& mj : j.value("markets", json::array())) {
        std::string slug = sval(mj, "slug");
        auto it = markets_.find(slug);
        if (it == markets_.end()) continue;
        PolyMarket& m = it->second;
        double bid = amount(mj, "bestBidQuote"), ask = amount(mj, "bestAskQuote");
        bool changed = std::fabs(bid - m.bid) > 1e-9 || std::fabs(ask - m.ask) > 1e-9;
        m.bid = bid; m.ask = ask; m.quote_ts = now;
        m.active = mj.value("active", m.active); m.closed = mj.value("closed", m.closed);
        m.updated_ts = iso_ts(sval(mj, "updatedAt"));
        auto lg = quote_logged_ts_.find(slug);
        if (lg == quote_logged_ts_.end()) {
          long long seeded = 0;
          json lr = store_.query("SELECT MAX(ts) AS t FROM poly_quotes WHERE slug=?", {slug});
          if (!lr.empty() && !lr[0]["t"].is_null()) seeded = lr[0].value("t", 0LL);
          lg = quote_logged_ts_.emplace(slug, seeded).first;
        }
        const long long last = lg->second;
        if ((changed && now - last >= 5) || now - last >= 60) {
          writes.push_back({"INSERT INTO poly_quotes(ts,slug,bid,ask) VALUES(?,?,?,?)", {now, slug, bid, ask}});
          logged[slug] = now;
          // Take Kalshi's side of the same outcome now, not later. The market row carries the book
          // from this cycle's refresh; k_quote_ts records how old that was, so an analysis can
          // discard a pair where one side was stale instead of silently trusting it.
          json mp = store_.query("SELECT kalshi_ticker, pm_yes_is_kalshi_yes FROM poly_pairs WHERE slug=?", {slug});
          if (!mp.empty()) {
            const std::string kt = mp[0].value("kalshi_ticker", "");
            if (auto km = store_.get_market(kt))
              if (km->yes_bid > 0 && km->yes_ask > 0 && km->yes_ask < 1)
                writes.push_back({"INSERT INTO pair_ticks(ts,slug,kalshi_ticker,pm_bid,pm_ask,k_bid,k_ask,"
                                  "k_bid_size,k_ask_size,k_quote_ts,same_side) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                                  {now, slug, kt, bid, ask, km->yes_bid, km->yes_ask,
                                   static_cast<double>(km->yes_bid_size), static_cast<double>(km->yes_ask_size),
                                   parse_iso(km->updated_time),
                                   static_cast<long long>(mp[0].value("pm_yes_is_kalshi_yes", 1))}});
          }
        }
        auto st = stored_quote_.find(slug);
        if (st == stored_quote_.end() || st->second.bid != bid || st->second.ask != ask ||
            st->second.active != m.active || st->second.closed != m.closed || now - st->second.quote_ts >= 60) {
          writes.push_back({"UPDATE poly_markets SET bid=?, ask=?, quote_ts=?, active=?, closed=? WHERE slug=?",
                            {bid, ask, now, static_cast<long long>(m.active ? 1 : 0),
                             static_cast<long long>(m.closed ? 1 : 0), slug}});
          stored[slug] = StoredQuote{bid, ask, m.active, m.closed, now};
        }
        polled[slug] = now;
      }
    }
    if (!writes.empty())
      store_.transaction([&] {
        for (const auto& [sql, params] : writes) store_.exec(sql, params);
      });
    Lock lk(mu_);
    for (const auto& [slug, ts] : logged) quote_logged_ts_[slug] = ts;
    for (const auto& [slug, q] : stored) stored_quote_[slug] = q;
    for (const auto& [slug, ts] : polled) quote_poll_ts_[slug] = ts;
    last_error_.clear();
  }
}

// How long to stop asking after the credential is refused.
static constexpr long long kPolyAuthBackoffSeconds = 900;

void PolyDesk::refresh_account(long long now, bool force) {
  if (!pc_.enabled || !armed()) return;
  {
    Lock lk(mu_);
    if (!force && now - account_ts_ < pc_.account_refresh_seconds) return;
    account_ts_ = now;
  }
  json bj, pj, oj, aj;
  // An order sent or cancelled while this snapshot is in flight makes it stale on arrival: it would
  // mark the new order done and stamp pre-trade positions as fresh. Such a snapshot is dropped and
  // fetched again on the next pass.
  const long long gen_before = order_gen_.load();
  long long since = std::atoll(store_.kv_get("poly_trades_since").value_or("0").c_str());
  try {
    bj = client_->call("GET", "/v1/account/balances", {}, nullptr, true);
    pj = client_->call("GET", "/v1/portfolio/positions", {{"limit", "200"}}, nullptr, true);
    oj = client_->call("GET", "/v1/orders/open", {}, nullptr, true);
    aj = client_->call("GET", "/v1/portfolio/activities", {{"limit", "100"}}, nullptr, true);
  } catch (const std::exception& e) {
    Lock lk(mu_);
    last_error_ = e.what();
    // A credential that is wrong is wrong on the next cycle too. Retrying it every cycle cost
    // four HTTP round trips inside the engine lock, every cycle, for hours - 234 consecutive
    // 401s on 2026-09-12 - while the cycle was already overrunning the quote freshness bar.
    // Back off on an auth failure and say so once, rather than logging the same line forever.
    const std::string what = e.what();
    // By the status the venue returned, not by searching the message: the message carries the
    // response body, and "401" or "API key" can appear in a body that is not an auth failure.
    const auto* api = dynamic_cast<const ApiError*>(&e);
    if (api ? (api->status == 401 || api->status == 403) : what.find("API key") != std::string::npos) {
      if (++account_auth_failures_ == 1 || account_auth_failures_ % 60 == 0)
        LOG_WARN("poly account: %s (auth failure %d; backing off to %ds - fix the key or disable "
                 "polymarket in config)", what.c_str(), account_auth_failures_,
                 static_cast<int>(kPolyAuthBackoffSeconds));
      account_ts_ = now + kPolyAuthBackoffSeconds - pc_.account_refresh_seconds;
      return;
    }
    account_auth_failures_ = 0;
    LOG_WARN("poly account: %s", what.c_str());
    return;
  }
  account_auth_failures_ = 0;
  Lock lk(mu_);
  if (order_gen_.load() != gen_before) {
    account_ts_ = 0;
    return;
  }
  balance_ = parse_poly_balance(bj);
  store_.exec("INSERT OR REPLACE INTO poly_account(ts,current_balance,buying_power,asset_notional,asset_available) VALUES(?,?,?,?,?)",
              {now, balance_.current_balance, balance_.buying_power, balance_.asset_notional, balance_.asset_available});
  positions_.clear();
  store_.exec("DELETE FROM poly_positions");
  const json& pos = pj.value("positions", json::object());
  auto take_position = [&](const std::string& slug, const json& x) {
    PolyPosition p = parse_poly_position(slug, x);
    if (p.net == 0 || p.slug.empty()) return;
    positions_.push_back(p);
    store_.exec("INSERT INTO poly_positions(slug,net,cost,realized,unrealized,available,expired,update_ts,updated_ts) VALUES(?,?,?,?,?,?,?,?,?)",
                {p.slug, p.net, p.cost, p.realized, p.unrealized, p.available, static_cast<long long>(p.expired ? 1 : 0), p.update_ts, now});
  };
  if (pos.is_object()) for (auto it = pos.begin(); it != pos.end(); ++it) take_position(it.key(), it.value());
  else if (pos.is_array()) for (const auto& x : pos) take_position(sval(x, "marketSlug"), x);
  std::set<std::string> open;
  for (const auto& x : oj.value("orders", json::array())) {
    PolyOrder o = parse_poly_order(x);
    if (o.id.empty()) continue;
    open.insert(o.id);
    store_.exec("INSERT INTO poly_orders(id,slug,side,type,tif,intent,state,price,quantity,cum,leaves,avg_px,commission,note,origin,created_ts,updated_ts) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,'','',?,?) ON CONFLICT(id) DO UPDATE SET state=excluded.state,price=excluded.price,"
                "quantity=excluded.quantity,cum=excluded.cum,leaves=excluded.leaves,avg_px=excluded.avg_px,commission=excluded.commission,updated_ts=excluded.updated_ts",
                {o.id, o.slug, o.side, o.type, o.tif, o.intent, o.state.empty() ? std::string("new") : o.state, o.price, o.quantity, o.cum, o.leaves,
                 o.avg_px, o.commission, o.created_ts, now});
  }
  // What the desk thought was open and the exchange no longer lists is done.
  for (const auto& r : store_.query("SELECT id FROM poly_orders WHERE state IN ('new','partially_filled','pending_new')"))
    if (!open.count(r.value("id", "")))
      store_.exec("UPDATE poly_orders SET state='done', leaves=0, updated_ts=? WHERE id=?", {now, r.value("id", "")});
  long long maxts = since;
  for (const auto& a : aj.value("activities", json::array())) {
    std::string type = sval(a, "type");
    if (type == "ACTIVITY_TYPE_TRADE" && a.contains("trade")) {
      PolyTrade t = parse_poly_trade(a["trade"]);
      if (t.id.empty()) continue;
      bool known = !store_.query("SELECT 1 AS x FROM poly_trades WHERE id=?", {t.id}).empty();
      store_.exec("INSERT OR REPLACE INTO poly_trades(id,slug,state,price,qty,cost_basis,realized,aggressor,ts) VALUES(?,?,?,?,?,?,?,?,?)",
                  {t.id, t.slug, t.state, t.price, t.qty, t.cost_basis, t.realized, static_cast<long long>(t.aggressor ? 1 : 0), t.ts});
      if (!known && since > 0)
        store_.log_event("info", "POLY FILL " + t.slug + " x" + fmt2(t.qty) + " @ " + fmt3(t.price) + (t.aggressor ? " T" : " M") +
                                     (t.realized != 0 ? " realized " + fmt2(t.realized) : ""));
      maxts = std::max(maxts, t.ts);
    } else if ((type == "ACTIVITY_TYPE_ACCOUNT_DEPOSIT" || type == "ACTIVITY_TYPE_ACCOUNT_ADVANCED_DEPOSIT" ||
                type == "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL") && a.contains("accountBalanceChange")) {
      const json& c = a["accountBalanceChange"];
      std::string id = sval(c, "transactionId");
      double amt = amount(c, "amount");
      if (amt == 0) amt = dval(c, "amount");
      if (type == "ACTIVITY_TYPE_ACCOUNT_WITHDRAWAL") amt = -std::fabs(amt); else amt = std::fabs(amt);
      long long ts = iso_ts(sval(c, "updateTime", sval(c, "createTime")));
      if (!id.empty()) store_.exec("INSERT OR REPLACE INTO poly_flows(id,ts,kind,amount) VALUES(?,?,?,?)", {id, ts, type.substr(14), amt});
    }
  }
  if (maxts > 0) store_.kv_set("poly_trades_since", std::to_string(maxts));
  last_error_.clear();
}

// ---- matching -------------------------------------------------------------------------------------------

void PolyDesk::match(long long now) {
  if (!pc_.enabled) return;
  Lock lk(mu_);
  if (match_ts_ > 0 && now - match_ts_ < 120) return;
  match_ts_ = now;
  // Polymarket events by league: slug -> markets of that event.
  std::map<std::string, std::vector<const PolyMarket*>> by_event;
  for (const auto& [slug, m] : markets_) by_event[m.league + "|" + m.event_slug].push_back(&m);
  std::map<std::string, PolyPair> found;
  std::map<std::string, std::pair<std::string, long long>> slug_owner;   // Polymarket slug -> (Kalshi event, start gap)
  static const char* series_list[] = {"KXNFLGAME", "KXNCAAFGAME", "KXMLBGAME", "KXNBAGAME", "KXNHLGAME", "KXWNBAGAME", "KXEPLGAME",
                                      "KXLALIGAGAME", "KXSERIEAGAME", "KXBUNDESLIGAGAME", "KXLIGUE1GAME", "KXMLSGAME", "KXUCLGAME",
                                      "KXNCAAMBGAME", "KXNCAAWBGAME"};
  for (const char* series : series_list) {
    std::string league = poly_league_for_series(series);
    if (league.empty()) continue;
    std::map<std::string, std::vector<Market>> kevents;
    for (const auto& m : store_.open_markets(series)) kevents[m.event_ticker].push_back(m);
    // Daily leagues (a series between the same two teams runs on consecutive days) need the exact
    // game date; the others tolerate a day of slack for the odd ticker dated on the wrong side of
    // midnight.
    const bool daily = league == "mlb" || league == "nba" || league == "nhl" || league == "wnba";
    for (const auto& [ev, kms] : kevents) {
      std::string date, codes, dummy, hhmm;
      if (!parse_kalshi_game_event(ev, &date, &codes, &dummy, &hhmm)) continue;
      // The ticker's date (and time, when it has one) are US Eastern.
      long long knoon = parse_iso(date + "T12:00:00Z");
      long long kstart = 0;
      if (hhmm.size() == 4) {
        long long local = parse_iso(date + "T" + hhmm.substr(0, 2) + ":" + hhmm.substr(2, 2) + ":00Z");
        kstart = local - us_eastern_offset(local);
      }
      // Team markets of the event (the draw market, when there is one, has no counterpart).
      std::vector<const Market*> teams;
      for (const auto& m : kms) {
        std::string code = m.ticker.substr(m.ticker.rfind('-') + 1);
        if (code == "TIE" || code == "DRAW") continue;
        teams.push_back(&m);
      }
      if (teams.size() != 2) continue;
      // Candidate Polymarket events: same league, same Eastern game date (within three hours of
      // the ticker time when it has one, so a doubleheader's two games stay apart). Ties between
      // candidates go to the closest start.
      const std::vector<const PolyMarket*>* best_ev = nullptr;
      int best_total = 0;
      long long best_gap = 0;
      std::vector<std::pair<const PolySide*, const PolyMarket*>> best_map(2, {nullptr, nullptr});
      for (const auto& [key, pms] : by_event) {
        if (key.rfind(league + "|", 0) != 0 || pms.empty()) continue;
        long long pstart = pms[0]->start_ts;
        long long gap = 0;
        if (pstart > 0) {
          if (kstart > 0) {
            gap = std::llabs(pstart - kstart);
            if (gap > 3 * 3600) continue;
          } else {
            std::string pdate = date_of(pstart, us_eastern_offset(pstart));
            gap = std::llabs(pstart - knoon);
            if (pdate != date && (daily || gap > 36 * 3600)) continue;
          }
        } else if (daily) continue;
        // Every side of every winner market in the event, by team.
        std::vector<std::pair<const PolySide*, const PolyMarket*>> sides;
        for (const PolyMarket* pm : pms)
          for (const auto& s : pm->sides)
            if (!s.abbr.empty() || !s.name.empty()) sides.push_back({&s, pm});
        int total = 0;
        std::vector<std::pair<const PolySide*, const PolyMarket*>> map(2, {nullptr, nullptr});
        std::set<std::string> used;
        for (size_t i = 0; i < 2; ++i) {
          const Market& km = *teams[i];
          std::string code = km.ticker.substr(km.ticker.rfind('-') + 1);
          std::string name = km.yes_sub_title.empty() ? km.title : km.yes_sub_title;
          if (name.size() > 5 && name.compare(name.size() - 5, 5, " wins") == 0) name = name.substr(0, name.size() - 5);
          int bs = 0;
          std::pair<const PolySide*, const PolyMarket*> pick{nullptr, nullptr};
          for (const auto& sp : sides) {
            std::string ident = sp.first->abbr + "|" + sp.first->name;
            if (used.count(ident)) continue;
            int sc = poly_team_score(code, name, *sp.first);
            // Soccer: a team's own "will X win" market carries X as the long side; prefer it.
            if (sc > 0 && sp.second->sports_type == "soccer_team_full_time_winner" && !sp.first->is_long) sc = 0;
            if (sc > bs) { bs = sc; pick = sp; }
          }
          if (bs == 0) { total = 0; break; }
          used.insert(pick.first->abbr + "|" + pick.first->name);
          map[i] = pick;
          total += bs;
        }
        if (map[0].first && map[1].first && (total > best_total || (total == best_total && best_ev && gap < best_gap))) {
          best_total = total; best_gap = gap; best_ev = &pms; best_map = map;
        }
      }
      if (!best_ev || best_total < 4) continue;   // each team at least a name match
      // One Polymarket market for one Kalshi event: when two Kalshi events claim the same market
      // (the ticker dated across midnight), the closer start keeps it.
      {
        const std::string slug0 = best_map[0].second->slug;
        auto prev = slug_owner.find(slug0);
        if (prev != slug_owner.end()) {
          if (prev->second.second <= best_gap) continue;
          for (auto it = found.begin(); it != found.end();) it = it->second.kalshi_event == prev->second.first ? found.erase(it) : std::next(it);
        }
        slug_owner[slug0] = {ev, best_gap};
      }
      for (size_t i = 0; i < 2; ++i) {
        const Market& km = *teams[i];
        PolyPair p;
        p.kalshi_ticker = km.ticker;
        p.kalshi_event = ev;
        p.kalshi_series = series;
        p.kalshi_team = km.ticker.substr(km.ticker.rfind('-') + 1);
        p.slug = best_map[i].second->slug;
        p.league = league;
        p.pm_team = best_map[i].first->name.empty() ? best_map[i].first->description : best_map[i].first->name;
        p.pm_yes_is_kalshi_yes = best_map[i].first->is_long;
        p.score = best_total;
        p.matched_by = best_total >= 6 ? "code" : "name";
        found[km.ticker] = p;
      }
    }
  }
  pairs_ = found;
  // One transaction: this runs on the Polymarket thread now, and a reader (the xvenue model, the
  // quote refresh) must never see the table emptied and half refilled.
  store_.transaction([&] {
    store_.exec("DELETE FROM poly_pairs");
    for (const auto& [t, p] : pairs_)
      store_.exec("INSERT INTO poly_pairs(kalshi_ticker,kalshi_event,kalshi_series,kalshi_team,slug,league,pm_team,pm_yes_is_kalshi_yes,score,matched_by,updated_ts) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                  {p.kalshi_ticker, p.kalshi_event, p.kalshi_series, p.kalshi_team, p.slug, p.league, p.pm_team,
                   static_cast<long long>(p.pm_yes_is_kalshi_yes ? 1 : 0), static_cast<long long>(p.score), p.matched_by, now});
  });
}

// ---- rails and orders --------------------------------------------------------------------------------------

void PolyDesk::rails(const PolyOrderRequest& r, const PolyMarket& m, double* cost_out) {
  if (!pc_.enabled) throw std::runtime_error("Polymarket is switched off (params.polymarket)");
  if (!armed()) throw std::runtime_error("no Polymarket API key configured; cannot send");
  if (store_.kv_get("kill").value_or("0") == "1") throw std::runtime_error("kill switch is on");
  if (r.action != "BUY" && r.action != "SELL") throw std::runtime_error("action must be BUY or SELL");
  if (!std::isfinite(r.price) || r.price <= 0 || r.price >= 1) throw std::runtime_error("price must be between 0 and 1");
  if (!std::isfinite(r.quantity) || r.quantity < m.min_qty - 1e-9) throw std::runtime_error("quantity must be at least " + fmt2(m.min_qty));
  if (m.min_qty >= 1 && std::fabs(r.quantity - std::round(r.quantity)) > 1e-9) throw std::runtime_error(m.slug + " trades whole contracts only");
  if (std::fabs(r.price / m.tick - std::round(r.price / m.tick)) > 1e-6) throw std::runtime_error("price is not on the " + fmt3(m.tick) + " tick");
  if (m.closed || !m.active) throw std::runtime_error(m.slug + " is not open for trading");
  double mid = m.mid();
  if (mid > 0 && std::fabs(r.price - mid) > pc_.price_band + 1e-9)
    throw std::runtime_error("price " + fmt3(r.price) + " is more than " + fmt2(pc_.price_band) + " from the mid " + fmt3(mid) + " (params.polymarket.price_band)");
  // Cost: a buyer pays the price, a seller posts $1 less the price (the exchange's margin).
  double cost = r.quantity * (r.action == "BUY" ? r.price : 1 - r.price);
  if (cost_out) *cost_out = cost;
  if (!r.manual) {
    // A SELL/BUY on this netted account can close a hand position instead of creating
    // the hedge. Automatic pair legs never consume pre-existing opposite inventory.
    for (const auto& p : positions_)
      if (p.slug == r.slug && ((p.net > 1e-9 && r.action == "SELL") || (p.net < -1e-9 && r.action == "BUY")))
        throw std::runtime_error("automatic hedge would reduce existing opposite Polymarket exposure; leave manual positions untouched");
    if (!store_.query("SELECT id FROM poly_orders WHERE slug=? AND state IN ('new','partially_filled','pending_new','pending_cancel') LIMIT 1", {r.slug}).empty())
      throw std::runtime_error("automatic hedge deferred: another Polymarket order is working in this market");
  }
  // Reducing an existing position is never capped.
  for (const auto& p : positions_)
    if (p.slug == r.slug && ((p.net > 0 && r.action == "SELL") || (p.net < 0 && r.action == "BUY")) && r.quantity <= std::fabs(p.net) + 1e-9)
      return;
  if (cost > pc_.max_order_dollars + 1e-9)
    throw std::runtime_error("order cost $" + fmt2(cost) + " is over the Polymarket order cap $" + fmt2(pc_.max_order_dollars));
  double total = cost;
  for (const auto& p : positions_) total += std::fabs(p.cost);
  for (const auto& o : store_.query("SELECT side, price, leaves FROM poly_orders WHERE state IN ('new','partially_filled','pending_new')"))
    total += o.value("leaves", 0.0) * (o.value("side", "") == "SELL" ? 1 - o.value("price", 0.0) : o.value("price", 0.0));
  if (total > pc_.max_total_dollars + 1e-9)
    throw std::runtime_error("Polymarket exposure would be $" + fmt2(total) + ", over the cap $" + fmt2(pc_.max_total_dollars));
  if (balance_.updated_ts <= 0) throw std::runtime_error("Polymarket account not synced yet; sync before trading");
  if (now_s() - balance_.updated_ts > 180) throw std::runtime_error("Polymarket account snapshot is stale; sync before trading");
  if (cost > balance_.buying_power + 1e-9) throw std::runtime_error("buying power $" + fmt2(balance_.buying_power) + " is under the order cost $" + fmt2(cost));
}

json PolyDesk::preview(const PolyOrderRequest& req) {
  if (!armed()) throw std::runtime_error("no Polymarket API key configured");
  json body{{"request", poly_order_body(req)}};
  return client_->call("POST", "/v1/order/preview", {}, &body, true);
}

json PolyDesk::ticket(const PolyOrderRequest& req_in, const std::string& note) {
  ++order_gen_;
  Lock lk(mu_);
  auto m = market(req_in.slug);
  if (!m) throw std::runtime_error("unknown Polymarket market " + req_in.slug);
  PolyOrderRequest req = req_in;
  req.price = round_tick(req.price, m->tick);
  if (m->min_qty >= 1) req.quantity = std::floor(req.quantity + 1e-9);
  double cost = 0;
  rails(req, *m, &cost);
  json body = poly_order_body(req);
  LOG_INFO("poly order -> %s", body.dump().c_str());
  json r;
  try {
    r = client_->call("POST", "/v1/orders", {}, &body, true);
  } catch (const ApiError& e) {
    store_.log_event("error", "POLY REJECTED " + req.slug + " " + req.action + " x" + fmt2(req.quantity) + " @ " + fmt3(req.price) + ": " + e.body.substr(0, 160));
    throw;
  }
  std::string id = sval(r, "id");
  double filled = 0, notional = 0, fee = 0;
  std::string state = "new";
  for (const auto& ex : r.value("executions", json::array())) {
    double q = parse_num(sval(ex, "lastShares", "0"));
    if (!sval(ex, "tradeId").empty() && q > 0) { filled += q; notional += q * amount(ex, "lastPx"); }
    fee += amount(ex, "commissionNotionalCollected");
    if (ex.contains("order") && ex["order"].is_object()) {
      std::string st = sval(ex["order"], "state");
      if (!st.empty()) state = lower(st.rfind("ORDER_STATE_", 0) == 0 ? st.substr(12) : st);
    }
  }
  double avg = filled > 0 ? notional / filled : 0;
  double leaves = std::max(0.0, req.quantity - filled);
  if (req.tif != "GTC" && state == "new") state = filled >= req.quantity - 1e-9 ? "filled" : "canceled";
  store_.exec("INSERT OR REPLACE INTO poly_orders(id,slug,side,type,tif,intent,state,price,quantity,cum,leaves,avg_px,commission,note,origin,created_ts,updated_ts) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {id, req.slug, req.action, std::string("LIMIT"), req.tif, std::string(), state, req.price, req.quantity, filled, leaves, avg, fee, note,
               note.rfind("pair", 0) == 0 ? std::string("pair") : std::string("ticket"), now_s(), now_s()});
  store_.log_event("info", "POLY order " + req.slug + " " + req.action + " x" + fmt2(req.quantity) + " @ " + fmt3(req.price) + " ($" + fmt2(cost) + ")" +
                               (filled > 0 ? " filled " + fmt2(filled) + " @ " + fmt3(avg) : (req.tif == "GTC" ? " resting" : " unfilled")));
  account_ts_ = 0;
  hooks_.request_reconcile();
  return json{{"order_id", id}, {"fill_count", filled}, {"remaining_count", leaves}, {"average_fill_price", avg}, {"fee", fee},
              {"cost", cost}, {"state", state}};
}

json PolyDesk::cancel(const std::string& order_id, const std::string& slug) {
  ++order_gen_;
  Lock lk(mu_);
  if (!armed()) throw std::runtime_error("no Polymarket API key configured");
  std::string s = slug;
  if (s.empty()) {
    json r = store_.query("SELECT slug FROM poly_orders WHERE id=?", {order_id});
    if (!r.empty()) s = r[0].value("slug", "");
  }
  json body{{"marketSlug", s}};
  json r = client_->call("POST", "/v1/order/" + order_id + "/cancel", {}, &body, true);
  store_.exec("UPDATE poly_orders SET state='canceled', leaves=0, updated_ts=? WHERE id=?", {now_s(), order_id});
  store_.log_event("info", "POLY canceled " + order_id.substr(0, 8));
  account_ts_ = 0;
  return json{{"order_id", order_id}, {"canceled", true}};
}

int PolyDesk::cancel_all(const std::string& why) {
  ++order_gen_;
  Lock lk(mu_);
  if (!pc_.enabled || !armed()) return 0;
  json open = store_.query("SELECT id, slug FROM poly_orders WHERE state IN ('new','partially_filled','pending_new')");
  if (open.empty()) return 0;
  try {
    json body = json::object();
    client_->call("POST", "/v1/orders/open/cancel", {}, &body, true);
    store_.exec("UPDATE poly_orders SET state='canceled', leaves=0, updated_ts=? WHERE state IN ('new','partially_filled','pending_new')", {now_s()});
    store_.log_event("warn", "POLY: pulled " + std::to_string(open.size()) + " resting order(s): " + why);
    account_ts_ = 0;
    return static_cast<int>(open.size());
  } catch (const std::exception& e) {
    LOG_WARN("poly cancel all: %s", e.what());
    return 0;
  }
}

json PolyDesk::book(const std::string& slug) {
  if (!client_) throw std::runtime_error("Polymarket is not configured");
  json j = client_->call("GET", "/v1/markets/" + slug + "/book");
  const json& md = j.contains("marketData") ? j["marketData"] : j;
  json bids = json::array(), asks = json::array();
  for (const auto& l : md.value("bids", json::array())) bids.push_back({amount(l, "px"), parse_num(sval(l, "qty", "0"))});
  for (const auto& l : md.value("asks", json::array())) asks.push_back({amount(l, "px"), parse_num(sval(l, "qty", "0"))});
  return json{{"slug", slug}, {"bids", bids}, {"asks", asks},
              {"best_bid", bids.empty() ? 0.0 : bids[0][0].get<double>()}, {"best_ask", asks.empty() ? 0.0 : asks[0][0].get<double>()},
              {"bid_size", bids.empty() ? 0.0 : bids[0][1].get<double>()}, {"ask_size", asks.empty() ? 0.0 : asks[0][1].get<double>()}};
}

json PolyDesk::close_position(const std::string& slug) {
  PolyPosition pos;
  bool have = false;
  {
    Lock lk(mu_);
    for (const auto& p : positions_) if (p.slug == slug) { pos = p; have = true; }
  }
  if (!have || pos.net == 0) throw std::runtime_error("no Polymarket position in " + slug);
  auto m = market(slug);
  if (!m) throw std::runtime_error("unknown Polymarket market " + slug);
  PolyOrderRequest r;
  r.slug = slug;
  r.quantity = std::fabs(pos.net);
  r.action = pos.net > 0 ? "SELL" : "BUY";
  r.price = pos.net > 0 ? m->bid : m->ask;
  if (r.price <= 0) throw std::runtime_error("no " + std::string(pos.net > 0 ? "bid" : "offer") + " to close into");
  r.tif = "IOC";
  return ticket(r, "close position");
}

// ---- pairs ----------------------------------------------------------------------------------------------------

json PolyDesk::lock_math(const PolyPair& pr, const Market& km, const Series& ks, const PolyMarket& pm, long long now) {
  // Everything in Kalshi-YES terms for the Kalshi team: Polymarket's book flipped when the pair
  // is on Polymarket's NO side.
  double pbid = pr.pm_yes_is_kalshi_yes ? pm.bid : 1 - pm.ask, pask = pr.pm_yes_is_kalshi_yes ? pm.ask : 1 - pm.bid;
  double kbid = km.yes_bid, kask = km.yes_ask, tick = km.tick > 0 ? km.tick : .01;
  FeeModel kf = FeeModel::from_series(ks);
  bool k_two = kbid > 0 && kask > 0 && kask > kbid && kask < 1, p_two = pbid > 0 && pask > 0 && pask > pbid && pask < 1;
  bool k_fresh = km.quote_ts > 0 && now - km.quote_ts <= 120, p_fresh = pm.quote_ts > 0 && now - pm.quote_ts <= kQuoteFresh;
  json routes = json::array();
  auto add = [&](const std::string& route, const std::string& ks_, const std::string& ps_, double kprice, double pprice, bool k_taker, bool p_taker,
                 double gross, double capital) {
    double kfee = kf.fee(kprice, 1, k_taker), pfee = poly_fee(p_taker ? pprice : pprice, p_taker, pm.fee_coeff);
    double profit = gross - kfee - pfee;
    routes.push_back({{"route", route}, {"kalshi_style", ks_}, {"pm_style", ps_}, {"kalshi_price", round4(kprice)}, {"pm_price", round4(pprice)},
                      {"kalshi_fee", round4(kfee)}, {"pm_fee", round4(pfee)}, {"gross", round4(gross)}, {"profit", round4(profit)},
                      {"capital", round4(capital)}, {"return", capital > 0 ? round4(profit / capital) : 0.0}});
  };
  if (k_two && p_two) {
    // Route A: buy the team on Kalshi (YES), sell it on Polymarket. Capital: Kalshi price + the
    // $1 - price margin a Polymarket short posts.
    for (bool kt : {true, false})
      for (bool pt : {true, false}) {
        double kprice = kt ? kask : std::min(kask - tick, kbid + tick);
        double pprice = pt ? pbid : pask;   // sell at the bid (taker) or offer at the ask (maker)
        add("buy_kalshi_sell_pm", kt ? "taker" : "maker", pt ? "taker" : "maker", kprice, pprice, kt, pt, pprice - kprice, kprice + (1 - pprice));
      }
    // Route B: sell the team on Kalshi (buy NO), buy it on Polymarket. Kalshi NO price = 1 - YES bid.
    for (bool kt : {true, false})
      for (bool pt : {true, false}) {
        double no_price = kt ? 1 - kbid : std::min(1 - kbid - tick, (1 - kask) + tick);   // buy NO at its ask, or a tick over its bid
        double pprice = pt ? pask : pbid;
        double gross = 1 - no_price - pprice;
        add("sell_kalshi_buy_pm", kt ? "taker" : "maker", pt ? "taker" : "maker", no_price, pprice, kt, pt, gross, no_price + pprice);
      }
  }
  // The executable lock. The Kalshi leg always goes first and the Polymarket leg is the hedge, so
  // a route is a lock only when that hedge crosses (pm taker): taker/taker is instant, maker/taker
  // rests the Kalshi leg and hits Polymarket when it fills. A resting Polymarket leg would leave
  // the Kalshi fill naked until it filled, and two resting legs are just the spread on a wide book,
  // so those routes stay in `routes` for the eye and never become `best`. A resting leg is only
  // plausible on a book that actually trades near it, hence the spread gate (`tight`): the rule
  // and the board skip pairs whose books are wider than the pair rule's max_spread on either venue.
  double max_spread = algo_state_.is_object() && algo_state_.contains("settings") ? algo_state_["settings"].value("max_spread", 0.06) : 0.06;
  double kspread = k_two ? kask - kbid : 1.0, pspread = p_two ? pask - pbid : 1.0;
  bool tight = k_two && p_two && kspread <= max_spread + 1e-9 && pspread <= max_spread + 1e-9;
  json best = nullptr, instant = nullptr;
  for (const auto& r : routes) {
    bool kt = r["kalshi_style"] == "taker", pt = r["pm_style"] == "taker";
    if (!pt) continue;
    if (kt) { if (instant.is_null() || r["profit"].get<double>() > instant["profit"].get<double>()) instant = r; }
    else if (best.is_null() || r["profit"].get<double>() > best["profit"].get<double>()) best = r;
  }
  if (!instant.is_null() && (best.is_null() || instant["profit"].get<double>() > 0 || instant["profit"].get<double>() > best["profit"].get<double>())) best = instant;
  // Would the pair rule take it: tight, and the executable lock clears its floors.
  const json& st = algo_state_.is_object() && algo_state_.contains("settings") ? algo_state_["settings"] : json::object();
  double min_lock = st.value("min_lock", 0.01), min_return = st.value("min_return", 0.01);
  bool qualifies = tight && k_fresh && p_fresh && !best.is_null() && best["profit"].get<double>() + 1e-9 >= min_lock &&
                   best["capital"].get<double>() > 0 && best["profit"].get<double>() / best["capital"].get<double>() + 1e-9 >= min_return;
  return json{{"kalshi_ticker", pr.kalshi_ticker}, {"kalshi_event", pr.kalshi_event}, {"kalshi_team", pr.kalshi_team}, {"slug", pr.slug},
              {"league", pr.league}, {"pm_team", pr.pm_team}, {"pm_yes_is_kalshi_yes", pr.pm_yes_is_kalshi_yes}, {"matched_by", pr.matched_by},
              {"title", km.title}, {"kalshi_bid", kbid}, {"kalshi_ask", kask}, {"kalshi_bid_size", km.yes_bid_size}, {"kalshi_ask_size", km.yes_ask_size},
              {"pm_bid", round4(pbid)}, {"pm_ask", round4(pask)}, {"pm_raw_bid", pm.bid}, {"pm_raw_ask", pm.ask}, {"pm_tick", pm.tick},
              {"kalshi_quote_ts", km.quote_ts}, {"pm_quote_ts", pm.quote_ts}, {"fresh", k_fresh && p_fresh}, {"two_sided", k_two && p_two},
              {"kalshi_spread", round4(kspread)}, {"pm_spread", round4(pspread)}, {"tight", tight}, {"max_spread", max_spread},
              {"qualifies", qualifies}, {"min_lock", min_lock}, {"min_return", min_return},
              {"kalshi_mid", k_two ? .5 * (kbid + kask) : 0.0}, {"pm_mid", p_two ? .5 * (pbid + pask) : 0.0},
              {"gap", k_two && p_two ? round4(.5 * (kbid + kask) - .5 * (pbid + pask)) : 0.0},
              {"start_ts", pm.start_ts}, {"hours_to_start", pm.start_ts > 0 ? (pm.start_ts - now) / 3600.0 : -1.0},
              {"close_time", km.close_time}, {"routes", routes}, {"best", best}, {"instant", instant}};
}

json PolyDesk::pairs(long long now) {
  Lock lk(mu_);
  json out = json::array();
  std::set<std::string> busy;
  for (const auto& r : store_.query("SELECT DISTINCT kalshi_event FROM xvenue_trades WHERE state IN ('kalshi_working','hedging','locked','unhedged')"))
    busy.insert(r.value("kalshi_event", ""));
  for (const auto& [t, pr] : pairs_) {
    auto km = store_.get_market(t);
    auto pm = markets_.find(pr.slug);
    if (!km || pm == markets_.end()) continue;
    Series ks;
    if (auto s = store_.get_series(km->series_ticker)) ks = *s;
    else { ks.ticker = km->series_ticker; ks.fee_type = "quadratic_with_maker_fees"; }
    json row = lock_math(pr, *km, ks, pm->second, now);
    row["busy"] = busy.count(pr.kalshi_event) > 0;
    out.push_back(row);
  }
  // Tradable books first (fresh, two-sided, tight), then by the executable lock.
  std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
    bool ta = a.value("tight", false) && a.value("fresh", false), tb = b.value("tight", false) && b.value("fresh", false);
    if (ta != tb) return ta;
    double pa = a["best"].is_null() ? -1 : a["best"]["profit"].get<double>(), pb = b["best"].is_null() ? -1 : b["best"]["profit"].get<double>();
    return pa > pb;
  });
  return out;
}

// ---- the pair rule ----------------------------------------------------------------------------------------------

json PolyDesk::xtrades(const std::string& states_sql) {
  return store_.query("SELECT * FROM xvenue_trades WHERE state IN (" + states_sql + ") ORDER BY id");
}

void PolyDesk::upsert_xtrade(json& t) {
  auto s = [&](const char* k) { return SqlValue(t.value(k, std::string())); };
  auto n = [&](const char* k) { return SqlValue(t.value(k, 0.0)); };
  auto i = [&](const char* k) { return SqlValue(t.value(k, 0LL)); };
  if (t.value("id", 0LL) == 0) {
    const long long new_id = store_.insert("INSERT INTO xvenue_trades(kalshi_ticker,kalshi_event,slug,league,route,mode,state,contracts,kalshi_side,kalshi_style,kalshi_price,"
                "kalshi_filled,kalshi_cost,kalshi_fee,kalshi_order_id,kalshi_proposal_id,pm_action,pm_style,pm_price,pm_filled,pm_cost,pm_fee,pm_order_id,"
                "locked,expected_profit,realized_pnl,opened_ts,kalshi_filled_ts,pm_filled_ts,closed_ts,reason,note,day,pm_yes_is_kalshi_yes,pm_mid_yes) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {s("kalshi_ticker"), s("kalshi_event"), s("slug"), s("league"), s("route"), s("mode"), s("state"), n("contracts"), s("kalshi_side"),
                 s("kalshi_style"), n("kalshi_price"), n("kalshi_filled"), n("kalshi_cost"), n("kalshi_fee"), s("kalshi_order_id"), i("kalshi_proposal_id"),
                 s("pm_action"), s("pm_style"), n("pm_price"), n("pm_filled"), n("pm_cost"), n("pm_fee"), s("pm_order_id"), n("locked"), n("expected_profit"),
                 n("realized_pnl"), i("opened_ts"), i("kalshi_filled_ts"), i("pm_filled_ts"), i("closed_ts"), s("reason"), s("note"), s("day"),
                 SqlValue(static_cast<long long>(t.value("pm_yes_is_kalshi_yes", true) ? 1 : 0)), n("pm_mid_yes")});
    t["id"] = new_id;
    return;
  }
  store_.exec("UPDATE xvenue_trades SET state=?,contracts=?,kalshi_price=?,kalshi_filled=?,kalshi_cost=?,kalshi_fee=?,kalshi_order_id=?,kalshi_proposal_id=?,"
              "pm_action=?,pm_style=?,pm_price=?,pm_filled=?,pm_cost=?,pm_fee=?,pm_order_id=?,locked=?,expected_profit=?,realized_pnl=?,kalshi_filled_ts=?,"
              "pm_filled_ts=?,closed_ts=?,reason=?,note=? WHERE id=?",
              {s("state"), n("contracts"), n("kalshi_price"), n("kalshi_filled"), n("kalshi_cost"), n("kalshi_fee"), s("kalshi_order_id"), i("kalshi_proposal_id"),
               s("pm_action"), s("pm_style"), n("pm_price"), n("pm_filled"), n("pm_cost"), n("pm_fee"), s("pm_order_id"), n("locked"), n("expected_profit"),
               n("realized_pnl"), i("kalshi_filled_ts"), i("pm_filled_ts"), i("closed_ts"), s("reason"), s("note"), i("id")});
}

double PolyDesk::algo_pnl_today(long long now) {
  std::string day = algo_state_.value("day", "");
  long long since = day.size() >= 10 ? parse_iso(day + "T00:00:00Z") - tz_offset_seconds(cfg_.risk.day_tz, now) : 0;
  std::string mode = algo_state_.value("paper", true) ? "paper" : "live";
  double realized = 0;
  for (const auto& r : store_.query("SELECT COALESCE(SUM(realized_pnl),0) AS r FROM xvenue_trades WHERE state IN ('settled','closed') AND closed_ts>=? AND mode=?",
                                     {since, mode}))
    realized = r.value("r", 0.0);
  return realized;
}

json PolyDesk::configure_algo(const json& body) {
  Lock lk(mu_);
  if (!body.is_object()) throw std::invalid_argument("request must be an object");
  json& st = algo_state_;
  if (body.contains("settings") && body["settings"].is_object()) {
    json settings = st["settings"];
    for (auto it = body["settings"].begin(); it != body["settings"].end(); ++it) {
      const std::string k = it.key();
      if (!settings.contains(k)) throw std::invalid_argument("unknown pair setting: " + k);
      if (k == "kalshi_style" || k == "pm_style") {
        if (!it.value().is_string() || (it.value() != "maker" && it.value() != "taker")) throw std::invalid_argument(k + " must be maker or taker");
        settings[k] = it.value();
        continue;
      }
      if (!it.value().is_number() || !std::isfinite(it.value().get<double>())) throw std::invalid_argument(k + " must be a number");
      settings[k] = it.value();
    }
    auto range = [&](const char* k, double lo, double hi) {
      double x = settings.value(k, 0.0);
      if (x < lo || x > hi) throw std::invalid_argument(std::string(k) + " outside [" + fmt2(lo) + ", " + fmt2(hi) + "]");
    };
    range("min_lock", 0, .5); range("min_return", 0, 1); range("max_pair_dollars", .5, 10000); range("max_total_dollars", .5, 100000);
    range("max_pairs", 1, 100); range("contracts", 1, 10000); range("kalshi_ttl_seconds", 10, 3600); range("hedge_ttl_seconds", 5, 3600);
    range("daily_loss_stop", .1, 10000); range("min_hours_to_start", 0, 240); range("max_spread", .01, .5);
    st["settings"] = settings;
    algo_log("info", "pair rule settings changed");
  }
  if (body.contains("paper")) {
    if (!body["paper"].is_boolean()) throw std::invalid_argument("paper must be boolean");
    if (!body["paper"].get<bool>() && (!armed() || !exec_)) throw std::invalid_argument("live needs the Polymarket key and the Kalshi executor");
    if (st["paper"] != body["paper"]) { st["paper"] = body["paper"]; algo_log("warn", std::string("pair rule mode -> ") + (body["paper"].get<bool>() ? "paper" : "LIVE")); }
  }
  if (body.contains("clear_halt") && body["clear_halt"].is_boolean() && body["clear_halt"].get<bool>()) {
    st["halted"] = false; st["halt_reason"] = "";
    st["loss_base"] = std::min(0.0, algo_pnl_today(now_s()));
    algo_log("warn", "pair rule halt cleared by hand");
  }
  if (body.contains("running")) {
    if (!body["running"].is_boolean()) throw std::invalid_argument("running must be boolean");
    bool want = body["running"].get<bool>();
    if (want && !st.value("paper", true) && (!armed() || !exec_)) throw std::invalid_argument("live needs the Polymarket key and the Kalshi executor");
    if (want != st.value("running", false)) {
      st["running"] = want;
      algo_log("warn", want ? std::string("pair rule STARTED (") + (st.value("paper", true) ? "paper" : "LIVE") + "): lock >= $" +
                                  fmt2(st["settings"].value("min_lock", 0.0)) + " a contract, " + fmt2(st["settings"].value("contracts", 0.0)) + " contracts a pair"
                            : "pair rule STOPPED: no new pairs; open legs keep being managed");
    }
  }
  algo_persist();
  return view();
}

bool PolyDesk::send_kalshi_leg(json& t, long long now, std::string* why) {
  if (!exec_) { if (why) *why = "no Kalshi executor"; return false; }
  auto m = store_.get_market(t.value("kalshi_ticker", ""));
  if (!m) { if (why) *why = "Kalshi market not in the store"; return false; }
  Proposal p;
  p.ticker = m->ticker;
  p.event_ticker = m->event_ticker;
  p.series_ticker = m->series_ticker;
  p.strategy = "xvenue";
  p.mode = "bot";
  p.outcome_side = t.value("kalshi_side", "yes");
  p.exec_style = t.value("kalshi_style", "maker");
  p.status = "pending";
  p.price = t.value("kalshi_price", 0.0);
  p.count = t.value("contracts", 0.0);
  p.yes_bid = m->yes_bid;
  p.yes_ask = m->yes_ask;
  p.cluster = risk_cluster(store_, *m);
  Series ks;
  if (auto s = store_.get_series(m->series_ticker)) ks = *s;
  p.fee_per = FeeModel::from_series(ks).fee(p.price, 1, p.exec_style == "taker");
  p.net_edge = t.value("locked", 0.0);
  p.p_model = p.outcome_side == "yes" ? t.value("pm_mid_yes", 0.5) : 1 - t.value("pm_mid_yes", 0.5);
  p.sigma = 0.01;
  p.note = "pair leg 1 (" + t.value("route", "") + "): lock " + fmt2(t.value("locked", 0.0)) + " a contract against Polymarket " + t.value("slug", "");
  store_.insert_proposal(p);
  t["kalshi_proposal_id"] = p.id;
  try {
    exec_->submit(p);
  } catch (const ApiError& e) {
    if (why) *why = "Kalshi refused (" + std::to_string(e.status) + "): " + e.body.substr(0, 120);
    return false;
  } catch (const std::exception& e) {
    if (why) *why = e.what();
    p.status = "failed";
    p.note += " | not sent: " + std::string(e.what());
    store_.update_proposal(p);
    return false;
  }
  hooks_.request_reconcile();
  t["kalshi_order_id"] = p.order_id;
  json o = store_.query("SELECT fill_count, remaining_count FROM orders WHERE order_id=?", {p.order_id});
  double fill = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
  if (fill > 0) {
    t["kalshi_filled"] = fill;
    t["kalshi_filled_ts"] = now;
    t["kalshi_fee"] = round4(FeeModel::from_series(ks).fee(p.price, fill, p.exec_style == "taker"));
    t["kalshi_cost"] = round4(fill * p.price + t.value("kalshi_fee", 0.0));
  }
  return true;
}

bool PolyDesk::send_pm_leg(json& t, long long now, std::string* why) {
  PolyOrderRequest r;
  r.slug = t.value("slug", "");
  r.action = t.value("pm_action", "SELL");
  r.quantity = t.value("kalshi_filled", 0.0) - t.value("pm_filled", 0.0);
  r.price = t.value("pm_price", 0.0);
  r.post_only = t.value("pm_style", "taker") == "maker";
  r.tif = r.post_only ? "GTC" : "IOC";
  r.manual = false;
  auto m = market(r.slug);
  if (!m) { if (why) *why = "Polymarket market unknown"; return false; }
  r.quantity = m->min_qty >= 1 ? std::floor(r.quantity + 1e-9) : r.quantity;
  if (r.quantity < m->min_qty) { if (why) *why = "nothing left to hedge"; return false; }
  const std::string pending_key = "poly_pair_send_pending:" + r.slug;
  if (store_.kv_get(pending_key).value_or("") != "") {
    if (why) *why = "prior Polymarket submission outcome is unknown; reconcile before another automatic order";
    return false;
  }
  try {
    refresh_account(now, true);
    if (!last_error_.empty()) throw std::runtime_error("cannot verify Polymarket account ownership: " + last_error_);
    double checked_cost = 0;
    rails(r, *m, &checked_cost);
    // Persist intent before the network write. A timeout/5xx without an order ID cannot be
    // retried safely: the venue may have accepted it, so this market stays blocked for automation.
    store_.kv_set(pending_key, json{{"trade_id", t.value("id", 0LL)}, {"ts", now}, {"quantity", r.quantity},
                                    {"action", r.action}, {"price", r.price}}.dump());
    json res = ticket(r, "pair leg 2: " + t.value("kalshi_ticker", ""));
    t["pm_order_id"] = res.value("order_id", "");
    if (t.value("pm_order_id", "").empty()) throw std::runtime_error("Polymarket response did not identify the submitted order");
    double fc = res.value("fill_count", 0.0);
    if (fc > 0) {
      double px = res.value("average_fill_price", 0.0) > 0 ? res.value("average_fill_price", 0.0) : r.price;
      t["pm_filled"] = t.value("pm_filled", 0.0) + fc;
      t["pm_fee"] = round4(t.value("pm_fee", 0.0) + res.value("fee", 0.0));
      t["pm_cost"] = round4(t.value("pm_cost", 0.0) + fc * (r.action == "BUY" ? px : 1 - px));
      if (t.value("pm_filled_ts", 0LL) == 0) t["pm_filled_ts"] = now;
    }
    const double actual_px = res.value("average_fill_price", 0.0) > 0 ? res.value("average_fill_price", 0.0) : r.price;
    // One transaction holding the store lock throughout (Store::transaction), so no other thread's
    // statement can land inside it.
    store_.transaction([&] {
      store_.kv_set("poly_pair_accounted:" + t.value("pm_order_id", ""),
                    json{{"cum", fc}, {"cost", fc * (r.action == "BUY" ? actual_px : 1 - actual_px)},
                         {"fee", res.value("fee", 0.0)}}.dump());
      upsert_xtrade(t);
      store_.kv_set(pending_key, "");
    });
    return true;
  } catch (const ApiError& e) {
    if (e.status >= 400 && e.status < 500 && e.status != 408) store_.kv_set(pending_key, "");
    if (why) *why = e.what();
    return false;
  } catch (const std::exception& e) {
    if (why) *why = e.what();
    return false;
  }
}

bool PolyDesk::reconcile_pm_leg(json& t, long long now, bool cancel_working, std::string* why) {
  const std::string id = t.value("pm_order_id", ""), slug = t.value("slug", "");
  if (id.empty()) return true;
  json checkpoint = t;
  try {
    json response = client_->call("GET", "/v1/order/" + id, {}, nullptr, true);
    if (!response.contains("order") || !response["order"].is_object()) throw std::runtime_error("order response missing");
    PolyOrder o = parse_poly_order(response["order"]);
    if (o.id != id || o.slug != slug || o.side != t.value("pm_action", "") || !std::isfinite(o.cum) ||
        !std::isfinite(o.leaves) || !std::isfinite(o.commission) || o.cum < 0 || o.leaves < 0 ||
        o.quantity <= 0 || o.cum > o.quantity + 1e-9 || (o.cum > 0 && !(o.avg_px > 0 && o.avg_px < 1)))
      throw std::runtime_error("order identity or cumulative fill is invalid");
    const std::string key = "poly_pair_accounted:" + id;
    json accounted = json::object();
    if (auto saved = store_.kv_get(key)) accounted = json::parse(*saved);
    else throw std::runtime_error("legacy hedge has no fill-accounting baseline; reconcile it before replacing");
    double prior = accounted.value("cum", 0.0);
    if (o.cum + 1e-9 < prior) throw std::runtime_error("cumulative fill moved backwards; awaiting reconciliation");
    double cost = o.cum * (o.side == "BUY" ? o.avg_px : 1 - o.avg_px);
    t["pm_filled"] = t.value("pm_filled", 0.0) + o.cum - prior;
    t["pm_cost"] = round4(t.value("pm_cost", 0.0) + cost - accounted.value("cost", 0.0));
    t["pm_fee"] = round4(t.value("pm_fee", 0.0) + o.commission - accounted.value("fee", 0.0));
    if (o.cum > 0 && t.value("pm_filled_ts", 0LL) == 0) t["pm_filled_ts"] = now;
    store_.transaction([&] {
      store_.exec("UPDATE poly_orders SET state=?,cum=?,leaves=?,avg_px=?,commission=?,updated_ts=? WHERE id=?",
                  {o.state, o.cum, o.leaves, o.avg_px, o.commission, now, id});
      store_.kv_set(key, json{{"cum", o.cum}, {"cost", cost}, {"fee", o.commission}}.dump());
      upsert_xtrade(t);
    });
    checkpoint = t;
    bool terminal = o.state == "filled" || o.state == "canceled" || o.state == "cancelled" || o.state == "rejected" || o.state == "expired";
    if (terminal && o.leaves <= 1e-9) return true;
    if (cancel_working) {
      cancel(id, slug);
      // A cancellation acknowledgement can race a fill. Get the final cumulative result
      // before reducing the replacement quantity or clearing the previous order ID.
      return reconcile_pm_leg(t, now, false, why);
    }
    if (why) *why = "prior Polymarket hedge is still working; no duplicate submitted";
    return false;
  } catch (const std::exception& e) {
    t = checkpoint;
    if (why) *why = "Polymarket hedge outcome not confirmed: " + std::string(e.what());
    return false;
  }
}

void PolyDesk::manage_pairs(long long now) {
  const json settings = algo_state_["settings"];
  const bool paper = algo_state_.value("paper", true);
  const std::string mode = paper ? "paper" : "live";
  bool killed = hooks_.killed();
  std::map<std::string, Position> kpos;
  for (const auto& p : store_.positions()) kpos[p.ticker] = p;
  for (auto t : store_.query("SELECT * FROM xvenue_trades WHERE mode=? AND state IN ('kalshi_working','hedging','locked','unhedged') ORDER BY id", {mode})) {
    const std::string kt = t.value("kalshi_ticker", ""), state = t.value("state", "");
    auto km = store_.get_market(kt);
    auto pm = markets_.find(t.value("slug", ""));
    long long id = t.value("id", 0LL);
    // Settlement: the Kalshi result decides both legs (same outcome on both venues).
    if (km && !km->result.empty() && (state == "locked" || state == "unhedged")) {
      if (!paper && t.value("pm_filled", 0.0) > 0) {
        t["reason"] = "Kalshi finalized; awaiting independent Polymarket settlement confirmation. Pair payout is not realized yet.";
        upsert_xtrade(t);
        continue;
      }
      bool kwon = (km->result == "yes") == (t.value("kalshi_side", "yes") == "yes");
      double kpay = kwon ? t.value("kalshi_filled", 0.0) : 0.0;
      // Polymarket leg: BUY YES pays $1 when the Kalshi team wins in YES terms; SELL YES pays $1
      // when it does not. The pair's pm_action is in Polymarket's own YES terms, so map through
      // the side relationship.
      bool pm_yes_is_ky = t["pm_yes_is_kalshi_yes"].is_boolean() ? t["pm_yes_is_kalshi_yes"].get<bool>() : t.value("pm_yes_is_kalshi_yes", 1LL) != 0;
      bool pm_long_kalshi_yes = (t.value("pm_action", "SELL") == "BUY") == pm_yes_is_ky;
      bool team_won = km->result == "yes";
      double ppay = ((pm_long_kalshi_yes && team_won) || (!pm_long_kalshi_yes && !team_won)) ? t.value("pm_filled", 0.0) : 0.0;
      double realized = kpay + ppay - t.value("kalshi_cost", 0.0) - t.value("pm_cost", 0.0) - t.value("pm_fee", 0.0);
      t["state"] = "settled"; t["closed_ts"] = now; t["realized_pnl"] = round4(realized);
      t["reason"] = std::string("settled: Kalshi ") + km->result + (t.value("pm_filled", 0.0) > 0 ? "" : " (unhedged)");
      upsert_xtrade(t);
      algo_log("trade", kt + " pair settled -> " + (realized >= 0 ? "+" : "") + fmt2(realized));
      continue;
    }
    if (state == "kalshi_working") {
      double fill = t.value("kalshi_filled", 0.0);
      bool still = false;
      if (!paper) {
        json o = store_.query("SELECT status, fill_count, remaining_count FROM orders WHERE order_id=?", {t.value("kalshi_order_id", "")});
        double fc = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
        still = !o.empty() && o[0].value("status", "") == "resting" && o[0].value("remaining_count", 0.0) > 0;
        // Pair ownership comes from this leg's order, never from the account's aggregate
        // position: an external/manual purchase must not trigger an automatic PM hedge.
        fill = std::max(fill, fc);
        bool expired = now - t.value("opened_ts", 0LL) >= static_cast<long long>(settings.value("kalshi_ttl_seconds", 240.0));
        if (still && (expired || killed)) {
          try { exec_->cancel(t.value("kalshi_order_id", "")); still = false; } catch (const std::exception& e) { algo_log("warn", kt + ": could not pull the Kalshi leg: " + e.what()); continue; }
          json o2 = store_.query("SELECT fill_count FROM orders WHERE order_id=?", {t.value("kalshi_order_id", "")});
          if (!o2.empty()) fill = std::max(fill, o2[0].value("fill_count", 0.0));
        }
        if (still) continue;
      } else {
        // Paper: a maker leg fills when the touch trades through it, a taker leg at once.
        if (km) {
          bool yes = t.value("kalshi_side", "yes") == "yes";
          double px = t.value("kalshi_price", 0.0);
          double ask_side = yes ? km->yes_ask : 1 - km->yes_bid;
          if (t.value("kalshi_style", "maker") == "taker" || (ask_side > 0 && ask_side <= px + 1e-9)) fill = t.value("contracts", 0.0);
          else if (now - t.value("opened_ts", 0LL) >= static_cast<long long>(settings.value("kalshi_ttl_seconds", 240.0))) fill = 0;
          else continue;
        }
      }
      if (fill <= 0) {
        t["state"] = "canceled"; t["closed_ts"] = now; t["reason"] = "Kalshi leg never filled";
        upsert_xtrade(t);
        pair_backoff_[kt] = now + 300;   // not re-posted for five minutes: the book walked past it once
        algo_log("info", kt + ": Kalshi leg pulled unfilled");
        continue;
      }
      Series ks;
      if (auto s = store_.get_series(km ? km->series_ticker : "")) ks = *s;
      bool taker = t.value("kalshi_style", "maker") == "taker";
      t["kalshi_filled"] = fill;
      if (t.value("kalshi_filled_ts", 0LL) == 0) t["kalshi_filled_ts"] = now;
      t["kalshi_fee"] = round4(FeeModel::from_series(ks).fee(t.value("kalshi_price", 0.0), fill, taker));
      t["kalshi_cost"] = round4(fill * t.value("kalshi_price", 0.0) + t.value("kalshi_fee", 0.0));
      t["state"] = "hedging";
      algo_log("trade", kt + ": Kalshi leg filled " + fmt2(fill) + " @ " + fmt2(t.value("kalshi_price", 0.0)) + "; hedging on Polymarket");
      upsert_xtrade(t);
      // fall through to hedging in this same cycle
    }
    if (t.value("state", "") == "hedging") {
      long long since = t.value("kalshi_filled_ts", 0LL) > 0 ? t.value("kalshi_filled_ts", 0LL) : t.value("opened_ts", 0LL);
      bool ttl = now - since >= static_cast<long long>(settings.value("hedge_ttl_seconds", 120.0));
      if (!paper && !t.value("pm_order_id", "").empty()) {
        std::string why;
        if (!reconcile_pm_leg(t, now, ttl || killed, &why)) {
          t["reason"] = why; upsert_xtrade(t); continue;
        }
        if (ttl && t.value("pm_style", "taker") == "maker") t["pm_style"] = "taker";
      }
      double need = t.value("kalshi_filled", 0.0) - t.value("pm_filled", 0.0);
      if (need <= 1e-9) {
        t["state"] = "locked";
        upsert_xtrade(t);
        continue;
      }
      if (pm == markets_.end() || !pm->second.two_sided() || now - pm->second.quote_ts > kQuoteFresh) { upsert_xtrade(t); continue; }
      // Re-price the Polymarket leg to the live touch in Polymarket's own terms.
      bool flip = !(t["pm_yes_is_kalshi_yes"].is_boolean() ? t["pm_yes_is_kalshi_yes"].get<bool>() : t.value("pm_yes_is_kalshi_yes", 1LL) != 0);
      std::string action = t.value("pm_action", "SELL");
      double touch = action == "SELL" ? pm->second.bid : pm->second.ask;
      double maker_px = action == "SELL" ? pm->second.ask : pm->second.bid;
      bool taker = t.value("pm_style", "taker") == "taker";
      double px = taker ? touch : maker_px;
      // The lock at this price, per contract, in Kalshi-YES terms.
      double pm_yes_px = flip ? 1 - px : px;
      double gross = t.value("route", "") == "buy_kalshi_sell_pm" ? pm_yes_px - t.value("kalshi_price", 0.0)
                                                                   : 1 - t.value("kalshi_price", 0.0) - pm_yes_px;
      double lock_now = gross - t.value("kalshi_fee", 0.0) / std::max(1e-9, t.value("kalshi_filled", 0.0)) - poly_fee(px, taker, pm->second.fee_coeff);
      if (lock_now < -0.02 && !ttl) { upsert_xtrade(t); continue; }   // the book moved away; wait a little for it to come back
      if (lock_now < -0.05) {
        t["state"] = "unhedged"; t["reason"] = "Polymarket moved away: hedging now would lose " + fmt2(-lock_now) + " a contract";
        upsert_xtrade(t);
        algo_log("warn", kt + ": left unhedged, " + t.value("reason", ""));
        continue;
      }
      t["pm_price"] = round4(px);
      if (paper) {
        t["pm_filled"] = t.value("kalshi_filled", 0.0);
        t["pm_fee"] = round4(need * poly_fee(px, taker, pm->second.fee_coeff));
        t["pm_cost"] = round4(t.value("pm_cost", 0.0) + need * (action == "BUY" ? px : 1 - px));
        t["pm_filled_ts"] = now;
        t["locked"] = round4(lock_now);
        t["state"] = "locked";
        upsert_xtrade(t);
        algo_log("trade", kt + ": paper hedge " + action + " " + fmt2(need) + " @ " + fmt3(px) + " on Polymarket; locked " + fmt2(lock_now) + " a contract");
        continue;
      }
      if (killed) { upsert_xtrade(t); continue; }
      std::string why;
      double before = t.value("pm_filled", 0.0);
      if (!send_pm_leg(t, now, &why)) {
        algo_log("warn", kt + ": Polymarket leg not sent: " + why);
        if (ttl) { t["state"] = "unhedged"; t["reason"] = "hedge not sent: " + why; algo_log("warn", kt + ": left unhedged"); }
        upsert_xtrade(t);
        continue;
      }
      if (t.value("pm_filled", 0.0) > before) t["locked"] = round4(lock_now);
      if (t.value("pm_filled", 0.0) >= t.value("kalshi_filled", 0.0) - 1e-9) {
        t["state"] = "locked";
        algo_log("trade", kt + ": hedged " + fmt2(t.value("pm_filled", 0.0)) + " on Polymarket @ " + fmt3(px) + "; locked " + fmt2(lock_now) + " a contract");
      } else if (!taker) {
        // The next cycle reconciles this exact order before considering a replacement.
        t["reason"] = "Polymarket hedge working; awaiting its cumulative fill";
      } else if (ttl) {
        t["state"] = "unhedged"; t["reason"] = "hedge did not fill inside the TTL";
        algo_log("warn", kt + ": left unhedged: " + t.value("reason", ""));
      }
      upsert_xtrade(t);
      continue;
    }
    (void)id;
  }
}

void PolyDesk::enter_pairs(long long now) {
  const json settings = algo_state_["settings"];
  const bool paper = algo_state_.value("paper", true);
  const std::string mode = paper ? "paper" : "live";
  if (!paper && (!armed() || !exec_ || hooks_.killed())) return;
  std::set<std::string> busy;
  double open_dollars = 0;
  int open_n = 0;
  for (const auto& r : store_.query("SELECT kalshi_event, kalshi_cost, pm_cost, contracts, kalshi_price, state FROM xvenue_trades WHERE mode=? AND state IN ('kalshi_working','hedging','locked','unhedged')", {mode})) {
    busy.insert(r.value("kalshi_event", ""));
    ++open_n;
    open_dollars += r.value("state", "") == "kalshi_working" ? r.value("contracts", 0.0) * r.value("kalshi_price", 0.0) * 2
                                                              : r.value("kalshi_cost", 0.0) + r.value("pm_cost", 0.0);
  }
  if (open_n >= settings.value("max_pairs", 5)) return;
  const std::string want_ks = settings.value("kalshi_style", "maker"), want_ps = settings.value("pm_style", "taker");
  json rows = pairs(now);
  int sent = 0;
  for (const auto& row : rows) {
    if (sent >= 1) break;
    if (!row.value("fresh", false) || !row.value("two_sided", false) || !row.value("tight", false)) continue;
    if (busy.count(row.value("kalshi_event", ""))) continue;
    auto bo = pair_backoff_.find(row.value("kalshi_ticker", ""));
    if (bo != pair_backoff_.end() && bo->second > now) continue;
    if (row.value("hours_to_start", -1.0) >= 0 && row.value("hours_to_start", -1.0) < settings.value("min_hours_to_start", 0.5)) continue;
    // The route with the configured styles.
    json pick = nullptr;
    for (const auto& r : row["routes"])
      if (r["kalshi_style"] == want_ks && r["pm_style"] == want_ps && (pick.is_null() || r["profit"].get<double>() > pick["profit"].get<double>())) pick = r;
    if (pick.is_null()) continue;
    double profit = pick["profit"].get<double>(), capital = pick["capital"].get<double>();
    if (profit + 1e-9 < settings.value("min_lock", 0.01)) continue;
    if (capital <= 0 || profit / capital + 1e-9 < settings.value("min_return", 0.01)) continue;
    double contracts = std::floor(std::min(settings.value("contracts", 5.0), settings.value("max_pair_dollars", 20.0) / capital));
    contracts = std::min(contracts, std::floor((settings.value("max_total_dollars", 100.0) - open_dollars) / capital));
    if (want_ks == "taker") contracts = std::min(contracts, std::floor(row.value("kalshi_ask_size", 0.0)));
    if (contracts < 1) continue;
    json t = {{"kalshi_ticker", row["kalshi_ticker"]}, {"kalshi_event", row["kalshi_event"]}, {"slug", row["slug"]}, {"league", row["league"]},
              {"route", pick["route"]}, {"mode", mode}, {"state", "kalshi_working"}, {"contracts", contracts},
              {"kalshi_side", pick["route"] == "buy_kalshi_sell_pm" ? "yes" : "no"}, {"kalshi_style", want_ks},
              {"kalshi_price", pick["kalshi_price"]}, {"kalshi_filled", 0.0}, {"kalshi_cost", 0.0}, {"kalshi_fee", 0.0}, {"kalshi_order_id", ""},
              {"kalshi_proposal_id", 0}, {"pm_action", std::string()}, {"pm_style", want_ps}, {"pm_price", pick["pm_price"]}, {"pm_filled", 0.0},
              {"pm_cost", 0.0}, {"pm_fee", 0.0}, {"pm_order_id", ""}, {"locked", pick["profit"]}, {"expected_profit", round4(profit * contracts)},
              {"realized_pnl", 0.0}, {"opened_ts", now}, {"kalshi_filled_ts", 0}, {"pm_filled_ts", 0}, {"closed_ts", 0}, {"reason", ""},
              {"note", "pair " + row.value("title", std::string()) + " vs Polymarket " + row.value("pm_team", std::string())},
              {"day", algo_state_.value("day", "")}, {"pm_yes_is_kalshi_yes", row["pm_yes_is_kalshi_yes"]}, {"pm_mid_yes", row["pm_mid"]}};
    // Polymarket action in Polymarket's own YES terms: route A sells the Kalshi team there, route
    // B buys it; flip when the team is Polymarket's NO side.
    bool sell_team = pick["route"] == "buy_kalshi_sell_pm";
    bool flip = !row.value("pm_yes_is_kalshi_yes", true);
    t["pm_action"] = (sell_team != flip) ? "SELL" : "BUY";
    // pm_price in Polymarket's own terms as well.
    double pm_px = pick["pm_price"].get<double>();
    t["pm_price"] = round4(flip ? 1 - pm_px : pm_px);
    if (!paper) {
      std::string why;
      if (!send_kalshi_leg(t, now, &why)) {
        algo_log("warn", row.value("kalshi_ticker", std::string()) + ": Kalshi leg not sent: " + why);
        pair_backoff_[row.value("kalshi_ticker", std::string())] = now + 300;
        continue;
      }
    }
    upsert_xtrade(t);
    algo_log("trade", row.value("kalshi_ticker", std::string()) + ": pair opened (" + pick["route"].get<std::string>() + "), " + fmt2(contracts) +
                          " contracts, Kalshi " + t.value("kalshi_side", "") + " @ " + fmt2(t.value("kalshi_price", 0.0)) + " " + want_ks +
                          ", lock " + fmt2(profit) + " a contract (" + fmt2(100 * profit / capital) + "% on $" + fmt2(capital * contracts) + ")" + (paper ? " (paper)" : ""));
    ++sent;
    busy.insert(row.value("kalshi_event", ""));
  }
}

void PolyDesk::step(long long now) {
  if (!pc_.enabled) return;
  Lock lk(mu_);
  json& st = algo_state_;
  if (now <= st.value("last_step_ts", 0LL)) return;
  st["last_step_ts"] = now;
  std::string day = trading_day(cfg_.risk.day_tz, now);
  if (st.value("day", "") != day) {
    bool first = st.value("day", "").empty();
    st["day"] = day;
    st["loss_base"] = 0.0;
    if (!first && st.value("halted", false)) { st["halted"] = false; st["halt_reason"] = ""; algo_log("info", "pair rule: new day, loss stop cleared"); }
  }
  manage_pairs(now);
  double today = algo_pnl_today(now), base = st.value("loss_base", 0.0), stop = st["settings"].value("daily_loss_stop", 5.0);
  if (st.value("running", false) && !st.value("halted", false) && today - base <= -stop) {
    st["halted"] = true;
    st["halt_reason"] = "daily loss stop: today " + fmt2(today) + " <= -" + fmt2(stop);
    algo_log("warn", st["halt_reason"].get<std::string>());
  }
  if (st.value("running", false) && !st.value("halted", false)) enter_pairs(now);
  algo_persist();
}

json PolyDesk::send_pair(const std::string& kalshi_ticker, const std::string& route, double contracts, const std::string& kalshi_style,
                         const std::string& pm_style, long long now) {
  Lock lk(mu_);
  if (!exec_ || !armed()) throw std::runtime_error("a pair needs the Kalshi executor and the Polymarket key");
  json rows = pairs(now);
  json row = nullptr;
  for (const auto& r : rows) if (r["kalshi_ticker"] == kalshi_ticker) row = r;
  if (row.is_null()) throw std::runtime_error("no Polymarket pair for " + kalshi_ticker);
  if (!row.value("fresh", false) || !row.value("two_sided", false)) throw std::runtime_error("one of the two books is stale or one-sided");
  json pick = nullptr;
  for (const auto& r : row["routes"])
    if (r["route"] == route && r["kalshi_style"] == kalshi_style && r["pm_style"] == pm_style) pick = r;
  if (pick.is_null()) throw std::runtime_error("unknown route/style");
  if (contracts < 1) throw std::runtime_error("contracts must be at least 1");
  bool sell_team = route == "buy_kalshi_sell_pm", flip = !row.value("pm_yes_is_kalshi_yes", true);
  double pm_px = pick["pm_price"].get<double>();
  json t = {{"kalshi_ticker", kalshi_ticker}, {"kalshi_event", row["kalshi_event"]}, {"slug", row["slug"]}, {"league", row["league"]},
            {"route", route}, {"mode", "live"}, {"state", "kalshi_working"}, {"contracts", contracts},
            {"kalshi_side", sell_team ? "yes" : "no"}, {"kalshi_style", kalshi_style}, {"kalshi_price", pick["kalshi_price"]},
            {"kalshi_filled", 0.0}, {"kalshi_cost", 0.0}, {"kalshi_fee", 0.0}, {"kalshi_order_id", ""}, {"kalshi_proposal_id", 0},
            {"pm_action", (sell_team != flip) ? "SELL" : "BUY"}, {"pm_style", pm_style}, {"pm_price", round4(flip ? 1 - pm_px : pm_px)},
            {"pm_filled", 0.0}, {"pm_cost", 0.0}, {"pm_fee", 0.0}, {"pm_order_id", ""}, {"locked", pick["profit"]},
            {"expected_profit", round4(pick["profit"].get<double>() * contracts)}, {"realized_pnl", 0.0}, {"opened_ts", now},
            {"kalshi_filled_ts", 0}, {"pm_filled_ts", 0}, {"closed_ts", 0}, {"reason", ""},
            {"note", "hand pair " + row.value("title", std::string())}, {"day", trading_day(cfg_.risk.day_tz, now)},
            {"pm_yes_is_kalshi_yes", row["pm_yes_is_kalshi_yes"]}, {"pm_mid_yes", row["pm_mid"]}};
  std::string why;
  if (!send_kalshi_leg(t, now, &why)) throw std::runtime_error("Kalshi leg not sent: " + why);
  upsert_xtrade(t);
  algo_log("trade", kalshi_ticker + ": hand pair sent (" + route + "), " + fmt2(contracts) + " contracts, lock " + fmt2(pick["profit"].get<double>()) + " a contract");
  return t;
}

// ---- views ----------------------------------------------------------------------------------------------------

json PolyDesk::summary() {
  Lock lk(mu_);
  double unreal = 0, cost = 0;
  for (const auto& p : positions_) { unreal += p.unrealized; cost += std::fabs(p.cost); }
  return json{{"enabled", pc_.enabled}, {"armed", armed()}, {"equity", balance_.current_balance + balance_.asset_notional},
              {"cash", balance_.current_balance}, {"buying_power", balance_.buying_power}, {"unrealized", unreal}, {"positions", positions_.size()},
              {"cost", cost}, {"account_ts", balance_.updated_ts}, {"quotes_ts", quotes_ts_}, {"universe_ts", universe_ts_},
              {"pairs", pairs_.size()}, {"markets", markets_.size()}, {"last_error", last_error_},
              {"algo_running", algo_state_.value("running", false)}, {"algo_paper", algo_state_.value("paper", true)},
              {"algo_halted", algo_state_.value("halted", false)}};
}

json PolyDesk::pnl() {
  Lock lk(mu_);
  long long now = now_s();
  std::string day = trading_day(cfg_.risk.day_tz, now);
  long long day_start = parse_iso(day + "T00:00:00Z") - tz_offset_seconds(cfg_.risk.day_tz, now);
  bool synced = balance_.updated_ts > 0;
  double equity = balance_.current_balance + balance_.asset_notional;
  double base = 0;
  long long base_ts = 0;
  for (const auto& r : store_.query("SELECT ts, current_balance + asset_notional AS eq FROM poly_account WHERE ts>=? ORDER BY ts LIMIT 1", {day_start})) {
    base = r.value("eq", 0.0);
    base_ts = r.value("ts", 0LL);
  }
  double in_today = 0, in_total = 0;
  for (const auto& f : store_.query("SELECT ts, amount FROM poly_flows")) {
    in_total += f.value("amount", 0.0);
    if (base_ts > 0 && f.value("ts", 0LL) > base_ts) in_today += f.value("amount", 0.0);
  }
  double unreal = 0, realized = 0;
  for (const auto& p : positions_) { unreal += p.unrealized; realized += p.realized; }
  double trades_realized = 0;
  for (const auto& r : store_.query("SELECT COALESCE(SUM(realized),0) AS r FROM poly_trades")) trades_realized = r.value("r", 0.0);
  return json{{"synced", synced}, {"equity", equity}, {"unrealized", unreal},
              {"today", synced && base_ts > 0 ? equity - base - in_today : 0.0},
              {"total", synced && in_total != 0 ? equity - in_total : trades_realized + unreal},
              {"baseline_equity", base}, {"baseline_ts", base_ts}, {"moved_in_today", in_today}, {"moved_in_total", in_total},
              {"trading_pnl", trades_realized + unreal}, {"day", day}};
}

json PolyDesk::view() {
  Lock lk(mu_);
  long long now = now_s();
  json positions = json::array();
  for (const auto& p : positions_) {
    auto m = markets_.find(p.slug);
    positions.push_back({{"slug", p.slug}, {"net", p.net}, {"side", p.net > 0 ? "long YES" : "short YES"}, {"cost", p.cost},
                         {"realized", p.realized}, {"unrealized", p.unrealized}, {"available", p.available}, {"expired", p.expired},
                         {"question", m != markets_.end() ? m->second.question : std::string()},
                         {"bid", m != markets_.end() ? m->second.bid : 0.0}, {"ask", m != markets_.end() ? m->second.ask : 0.0}});
  }
  json markets = json::array();
  for (const auto& [slug, m] : markets_) {
    bool paired = false;
    for (const auto& [t, p] : pairs_) if (p.slug == slug) { paired = true; break; }
    if (!paired && m.quote_ts == 0) continue;
    markets.push_back({{"slug", slug}, {"league", m.league}, {"question", m.question}, {"event_title", m.event_title}, {"bid", m.bid}, {"ask", m.ask},
                       {"tick", m.tick}, {"fee_coeff", m.fee_coeff}, {"start_ts", m.start_ts}, {"quote_ts", m.quote_ts}, {"paired", paired},
                       {"long_side", m.sides.empty() ? std::string() : (m.sides[0].name.empty() ? m.sides[0].description : m.sides[0].name)},
                       {"short_side", m.sides.size() < 2 ? std::string() : (m.sides[1].name.empty() ? m.sides[1].description : m.sides[1].name)}});
  }
  json algo = algo_state_;
  algo["open"] = store_.query("SELECT * FROM xvenue_trades WHERE state IN ('kalshi_working','hedging','locked','unhedged') ORDER BY id DESC");
  algo["history"] = store_.query("SELECT * FROM xvenue_trades WHERE state IN ('settled','closed','canceled') ORDER BY id DESC LIMIT 100");
  algo["pnl_today"] = algo_pnl_today(now);
  double locked = 0;
  for (const auto& t : algo["open"]) if (t.value("state", "") == "locked") locked += t.value("locked", 0.0) * t.value("pm_filled", 0.0);
  algo["locked_open"] = round4(locked);
  return json{{"enabled", pc_.enabled}, {"armed", armed()}, {"last_error", last_error_}, {"quotes_ts", quotes_ts_}, {"universe_ts", universe_ts_},
              {"account_ts", balance_.updated_ts},
              {"balance", {{"cash", balance_.current_balance}, {"buying_power", balance_.buying_power}, {"asset_notional", balance_.asset_notional},
                           {"asset_available", balance_.asset_available}, {"equity", balance_.current_balance + balance_.asset_notional}}},
              {"rails", {{"max_order_dollars", pc_.max_order_dollars}, {"max_total_dollars", pc_.max_total_dollars}, {"price_band", pc_.price_band}}},
              {"leagues", pc_.leagues}, {"positions", positions}, {"markets", markets}, {"pairs", pairs(now)},
              {"orders", store_.query("SELECT * FROM poly_orders WHERE state IN ('new','partially_filled','pending_new') ORDER BY created_ts DESC")},
              {"recent_orders", store_.query("SELECT * FROM poly_orders ORDER BY updated_ts DESC LIMIT 30")},
              {"trades", store_.query("SELECT * FROM poly_trades ORDER BY ts DESC LIMIT 50")}, {"algo", algo}};
}

}  // namespace desk
