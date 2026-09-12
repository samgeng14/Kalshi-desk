#include "desk/perps.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>

#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/timeutil.h"

namespace desk {
namespace {
constexpr const char* kAlgoKey = "perps_algo:v1";
constexpr long long kQuoteLogEvery = 60;

double d(const json& j, const char* k, double dflt = 0) { return jnum(j, k, dflt); }
std::string s(const json& j, const char* k, const std::string& dflt = "") { return jstr(j, k, dflt); }
double ticker_price(const json& j, const char* k, long long* ts_ms = nullptr) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_object()) return 0;
  if (ts_ms) *ts_ms = static_cast<long long>(d(*it, "ts_ms"));
  return d(*it, "price");
}
std::string uuid4() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned long long> dist;
  unsigned long long a = dist(rng), b = dist(rng);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08llx-%04llx-4%03llx-%04llx-%012llx", a & 0xffffffffULL, (a >> 32) & 0xffff,
                (a >> 48) & 0xfff, 0x8000ULL | (b & 0x3fff), (b >> 16) & 0xffffffffffffULL);
  return buf;
}
double round_tick(double price, double tick) { return tick > 0 ? std::round(price / tick) * tick : price; }
std::string fmt4(double v) { char b[32]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
}  // namespace

// ---- parsing -------------------------------------------------------------------------------

PerpMarket parse_perp_market(const json& j) {
  PerpMarket m;
  m.ticker = s(j, "ticker");
  m.title = s(j, "title");
  m.status = s(j, "status");
  m.asset_class = s(j, "asset_class");
  m.tick = d(j, "tick_size", 0.0001);
  m.contract_size = d(j, "contract_size");
  // Kalshi sends perp prices as fixed-point integers scaled by 10^4 and uses INT64_MAX to mean
  // "no order on this side". Parsed as a number that is 922337203685477.6, which is not a price:
  // recorded straight into perp_quotes it made the basis read 6.8e16 bps on a dozen instruments,
  // and `r.price = m->ask` would have posted an offer there. Absent is absent.
  auto side_price = [](double v) { return (std::isfinite(v) && v > 0 && v < 1e9) ? v : 0.0; };
  m.bid = side_price(d(j, "bid"));
  m.ask = side_price(d(j, "ask"));
  m.quote_received_ts_ms = now_ms();   // ours: the venue sends no clock with the book
  m.last = d(j, "price");
  m.mark = ticker_price(j, "settlement_mark_price", &m.mark_ts_ms);
  m.liquidation_mark = ticker_price(j, "liquidation_mark_price");
  m.reference = ticker_price(j, "reference_price", &m.reference_ts_ms);
  m.leverage = d(j, "leverage_estimate");
  m.volume_24h = d(j, "volume_24h");
  m.open_interest = d(j, "open_interest");
  m.fractional = j.value("fractional_trading_enabled", false);
  if (j.contains("schedule") && j["schedule"].is_object()) m.is_open = j["schedule"].value("is_open", true);
  return m;
}

PerpBook parse_perp_book(const json& j, const std::string& ticker) {
  PerpBook b;
  b.ticker = ticker;
  const json* ob = &j;
  if (j.contains("orderbook") && j["orderbook"].is_object()) ob = &j["orderbook"];
  auto levels = [](const json& arr) {
    std::vector<BookLevel> out;
    if (!arr.is_array()) return out;
    for (const auto& l : arr)
      if (l.is_array() && l.size() >= 2)
        out.push_back({parse_num(l[0].is_string() ? l[0].get<std::string>() : l[0].dump()),
                       parse_num(l[1].is_string() ? l[1].get<std::string>() : l[1].dump())});
    return out;
  };
  b.bids = levels(ob->value("bids", json::array()));
  b.asks = levels(ob->value("asks", json::array()));
  // The exchange lists both sides from the touch outward; the desk keeps the touch last so the
  // event-contract habits (`.back()` is best) carry over.
  std::sort(b.bids.begin(), b.bids.end(), [](const BookLevel& x, const BookLevel& y) { return x.price < y.price; });
  std::sort(b.asks.begin(), b.asks.end(), [](const BookLevel& x, const BookLevel& y) { return x.price > y.price; });
  return b;
}

PerpPosition parse_perp_position(const json& j) {
  PerpPosition p;
  p.subaccount = static_cast<int>(d(j, "subaccount"));
  p.ticker = s(j, "market_ticker", s(j, "ticker"));
  p.position = d(j, "position");
  p.entry_price = d(j, "entry_price");
  p.unrealized_pnl = d(j, "unrealized_pnl");
  p.margin_used = d(j, "margin_used");
  p.fees = d(j, "fees");
  p.is_portfolio = j.value("is_portfolio", false);
  return p;
}

PerpBalance parse_perp_balance(const json& j) {
  PerpBalance b;
  b.settled_funds = d(j, "settled_funds");
  for (const auto& sub : j.value("subaccount_balances", json::array())) {
    b.position_value += d(sub, "position_value");
    b.equity += d(sub, "account_equity");
    b.maintenance_margin += d(sub, "maintenance_margin");
    b.initial_margin += d(sub, "initial_margin");
    b.resting_margin += d(sub, "resting_orders_margin");
    b.available += d(sub, "available_balance");
  }
  // Equity is reported per subaccount for retail members; a self-clearing member sees 0 there and
  // the sum of settled funds and position value is the honest fallback.
  if (b.equity <= 0) b.equity = b.settled_funds + b.position_value;
  b.updated_ts = now_s();
  return b;
}

PerpOrder parse_perp_order(const json& j) {
  PerpOrder o;
  o.order_id = s(j, "order_id");
  o.client_order_id = s(j, "client_order_id");
  o.ticker = s(j, "ticker");
  o.side = s(j, "side");
  o.status = s(j, "status");
  o.price = d(j, "price");
  o.fill_count = d(j, "fill_count");
  o.remaining_count = d(j, "remaining_count");
  o.created_time = s(j, "created_time");
  o.last_update_time = s(j, "last_update_time");
  o.last_update_reason = s(j, "last_update_reason");
  o.order_source = s(j, "order_source");
  o.order_reason = s(j, "order_reason");
  return o;
}

PerpFill parse_perp_fill(const json& j) {
  PerpFill f;
  f.fill_id = s(j, "fill_id");
  f.order_id = s(j, "order_id");
  f.ticker = s(j, "ticker");
  f.side = s(j, "side");
  f.count = d(j, "count");
  f.price = d(j, "price");
  f.entry_price = d(j, "entry_price");
  f.fees = d(j, "fees");
  f.realized_pnl = d(j, "realized_pnl");
  f.is_taker = j.value("is_taker", false);
  f.created_time = s(j, "created_time");
  f.order_source = s(j, "order_source");
  f.ts = parse_iso(f.created_time);
  return f;
}

json perp_order_body(const PerpOrderRequest& r) {
  json b{{"ticker", r.ticker}, {"side", r.side}, {"count", fmt_fp(r.count)}, {"price", fmt_dollars(r.price)},
         {"time_in_force", r.tif}, {"post_only", r.post_only}, {"self_trade_prevention_type", "taker_at_cross"},
         {"subaccount", 0}};
  if (r.reduce_only) b["reduce_only"] = true;
  if (!r.client_order_id.empty()) b["client_order_id"] = r.client_order_id;
  return b;
}

PerpsConfig perps_config(const nlohmann::json& p) {
  PerpsConfig c;
  if (!p.is_object()) return c;
  c.enabled = p.value("enabled", c.enabled);
  c.max_order_notional = p.value("max_order_notional", c.max_order_notional);
  c.max_total_notional = p.value("max_total_notional", c.max_total_notional);
  c.max_leverage = p.value("max_leverage", c.max_leverage);
  c.price_band_pct = p.value("price_band_pct", c.price_band_pct);
  c.refresh_seconds = p.value("refresh_seconds", c.refresh_seconds);
  c.account_refresh_seconds = p.value("account_refresh_seconds", c.account_refresh_seconds);
  if (p.contains("watch") && p["watch"].is_array())
    for (const auto& w : p["watch"]) if (w.is_string()) c.watch.push_back(w.get<std::string>());
  if (p.contains("algo") && p["algo"].is_object()) {
    const json& a = p["algo"];
    c.algo_entry_bps = a.value("entry_bps", c.algo_entry_bps);
    c.algo_exit_bps = a.value("exit_bps", c.algo_exit_bps);
    c.algo_stop_bps = a.value("stop_bps", c.algo_stop_bps);
    c.algo_max_hold_minutes = a.value("max_hold_minutes", c.algo_max_hold_minutes);
    c.algo_contracts = a.value("contracts", c.algo_contracts);
    c.algo_notional_dollars = a.value("notional_dollars", c.algo_notional_dollars);
    c.algo_daily_loss_stop = a.value("daily_loss_stop", c.algo_daily_loss_stop);
    if (a.contains("markets") && a["markets"].is_array()) {
      c.algo_markets.clear();
      for (const auto& m : a["markets"]) if (m.is_string()) c.algo_markets.push_back(m.get<std::string>());
    }
  }
  return c;
}

// ---- the desk ------------------------------------------------------------------------------

PerpsDesk::PerpsDesk(const Config& cfg, Store& store, KalshiClient& client)
    : cfg_(cfg), pc_(perps_config(cfg.strategy_params.is_object() && cfg.strategy_params.contains("perps")
                                     ? cfg.strategy_params["perps"] : json::object())),
      store_(store), client_(client) {
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_markets(ticker TEXT PRIMARY KEY, title TEXT, status TEXT, asset_class TEXT, tick REAL, "
      "contract_size REAL, bid REAL, ask REAL, last REAL, mark REAL, liquidation_mark REAL, reference REAL, leverage REAL, "
      "volume_24h REAL, open_interest REAL, is_open INTEGER, fractional INTEGER, funding_rate REAL, next_funding TEXT, updated_ts INTEGER)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_quotes(id INTEGER PRIMARY KEY, ts INTEGER, ticker TEXT, bid REAL, ask REAL, mark REAL, "
      "reference REAL, funding_rate REAL)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_perp_quotes ON perp_quotes(ticker, ts)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_positions(ticker TEXT PRIMARY KEY, subaccount INTEGER, position REAL, entry_price REAL, "
      "unrealized_pnl REAL, margin_used REAL, fees REAL, is_portfolio INTEGER, updated_ts INTEGER)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_orders(order_id TEXT PRIMARY KEY, client_order_id TEXT, ticker TEXT, side TEXT, status TEXT, "
      "price REAL, fill_count REAL, remaining_count REAL, created_time TEXT, last_update_time TEXT, last_update_reason TEXT, "
      "order_source TEXT, order_reason TEXT, note TEXT, origin TEXT, updated_ts INTEGER)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_fills(fill_id TEXT PRIMARY KEY, order_id TEXT, ticker TEXT, side TEXT, count REAL, price REAL, "
      "entry_price REAL, fees REAL, realized_pnl REAL, is_taker INTEGER, created_time TEXT, ts INTEGER, order_source TEXT)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_account(ts INTEGER PRIMARY KEY, settled_funds REAL, position_value REAL, equity REAL, "
      "maintenance_margin REAL, initial_margin REAL, available REAL)");
  store_.exec(
      "CREATE TABLE IF NOT EXISTS perp_algo_trades(id INTEGER PRIMARY KEY, ticker TEXT, side TEXT, mode TEXT, state TEXT, count REAL, "
      "entry_price REAL, entry_basis_bps REAL, opened_ts INTEGER, filled_ts INTEGER, order_id TEXT, exit_order_id TEXT, exit_price REAL, "
      "closed_ts INTEGER, realized_pnl REAL, fees REAL, reason TEXT, mark REAL, unrealized REAL, day TEXT)");
  algo_state_ = {{"running", false}, {"paper", true}, {"halted", false}, {"halt_reason", ""}, {"day", ""},
                 {"log", json::array()}, {"last_step_ts", 0},
                 {"settings", {{"entry_bps", pc_.algo_entry_bps}, {"exit_bps", pc_.algo_exit_bps}, {"stop_bps", pc_.algo_stop_bps},
                               {"max_hold_minutes", pc_.algo_max_hold_minutes}, {"contracts", pc_.algo_contracts},
                               {"daily_loss_stop", pc_.algo_daily_loss_stop}, {"entry_ttl_seconds", 90.0},
                               // Dollars of notional per entry. A contract count cannot be one setting
                               // across this book: a WLD contract is $0.40 and a ZEC contract is $11, so
                               // "12 contracts" is $4.82 in one and $132 in the other. 0 falls back to
                               // the contract count, which is what a settings row written earlier holds.
                               {"notional_dollars", pc_.algo_notional_dollars},
                               {"cooldown_minutes", 30.0},
                               // How old the book may be when an entry is priced from it. The
                               // validator refuses a settings key it does not already hold, and
                               // the range check reads 0.0 for an absent one, so this default has
                               // to exist here or every perps settings save would be rejected.
                               {"book_max_age_seconds", 2.0},
                               {"markets", pc_.algo_markets}}}};
  if (auto saved = store_.kv_get(kAlgoKey)) {
    json parsed = json::parse(*saved, nullptr, false);
    if (parsed.is_object()) {
      json settings = algo_state_["settings"];
      if (parsed.contains("settings") && parsed["settings"].is_object()) settings.update(parsed["settings"]);
      algo_state_.update(parsed);
      algo_state_["settings"] = settings;
    }
  }
  if (!armed() && algo_state_.value("running", false) && !algo_state_.value("paper", true)) {
    algo_state_["running"] = false;
    algo_log("warn", "perps rule stopped: live mode needs an API key");
  }
  if (algo_state_.value("running", false)) algo_log("info", "perps rule resumed after a restart");
  algo_persist();
}

void PerpsDesk::algo_persist() { store_.kv_set(kAlgoKey, algo_state_.dump()); }

void PerpsDesk::algo_log(const std::string& level, const std::string& msg) {
  algo_state_["log"].push_back(json{{"ts", now_s()}, {"level", level}, {"msg", msg}});
  while (algo_state_["log"].size() > 150) algo_state_["log"].erase(algo_state_["log"].begin());
  if (level != "info") store_.log_event(level == "trade" ? "info" : level, "PERPS: " + msg);
  if (level == "error") LOG_ERR("perps: %s", msg.c_str());
  else LOG_INFO("perps: %s", msg.c_str());
}

std::optional<PerpMarket> PerpsDesk::market(const std::string& ticker) {
  auto it = markets_cache_.find(ticker);
  if (it != markets_cache_.end()) return it->second;
  return std::nullopt;
}

void PerpsDesk::refresh_public(long long now) {
  if (!pc_.enabled) return;
  if (now - public_ts_ < pc_.refresh_seconds) return;
  public_ts_ = now;
  // The exchange is read with the lock released; the caches are swapped under it.
  json j;
  try {
    j = client_.call("GET", "/margin/markets", {{"status", "active"}});
  } catch (const std::exception& e) {
    Lock lk(mu_);
    last_error_ = e.what();
    LOG_WARN("perps markets: %s", e.what());
    return;
  }
  std::set<std::string> fund;
  {
    Lock lk(mu_);
    std::set<std::string> want(pc_.watch.begin(), pc_.watch.end());
    for (const auto& mj : j.value("markets", json::array())) {
      PerpMarket m = parse_perp_market(mj);
      if (m.ticker.empty()) continue;
      if (!want.empty() && !want.count(m.ticker)) continue;
      PerpMarket* prev = markets_cache_.count(m.ticker) ? &markets_cache_[m.ticker] : nullptr;
      bool changed = !prev || prev->bid != m.bid || prev->ask != m.ask || prev->reference != m.reference;
      long long last_log = 0;
      json lr = store_.query("SELECT MAX(ts) AS t FROM perp_quotes WHERE ticker=?", {m.ticker});
      if (!lr.empty() && !lr[0]["t"].is_null()) last_log = lr[0].value("t", 0LL);
      double funding = funding_cache_.count(m.ticker) ? funding_cache_[m.ticker].rate : 0.0;
      if ((changed && now - last_log >= 5) || now - last_log >= kQuoteLogEvery)
        store_.exec("INSERT INTO perp_quotes(ts,ticker,bid,ask,mark,reference,funding_rate) VALUES(?,?,?,?,?,?,?)",
                    {now, m.ticker, m.bid, m.ask, m.mark, m.reference, funding});
      markets_cache_[m.ticker] = m;
      store_.exec(
          "INSERT INTO perp_markets(ticker,title,status,asset_class,tick,contract_size,bid,ask,last,mark,liquidation_mark,reference,"
          "leverage,volume_24h,open_interest,is_open,fractional,funding_rate,next_funding,updated_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
          "ON CONFLICT(ticker) DO UPDATE SET title=excluded.title,status=excluded.status,asset_class=excluded.asset_class,tick=excluded.tick,"
          "contract_size=excluded.contract_size,bid=excluded.bid,ask=excluded.ask,last=excluded.last,mark=excluded.mark,"
          "liquidation_mark=excluded.liquidation_mark,reference=excluded.reference,leverage=excluded.leverage,volume_24h=excluded.volume_24h,"
          "open_interest=excluded.open_interest,is_open=excluded.is_open,fractional=excluded.fractional,updated_ts=excluded.updated_ts",
          {m.ticker, m.title, m.status, m.asset_class, m.tick, m.contract_size, m.bid, m.ask, m.last, m.mark, m.liquidation_mark,
           m.reference, m.leverage, m.volume_24h, m.open_interest, static_cast<long long>(m.is_open ? 1 : 0),
           static_cast<long long>(m.fractional ? 1 : 0), funding,
           funding_cache_.count(m.ticker) ? funding_cache_[m.ticker].next_funding_time : std::string(), now});
    }
    // Funding estimates for the markets the rule watches and anything held, every five minutes.
    const json& algo_markets = algo_state_["settings"]["markets"];
    if (algo_markets.is_array())
      for (const auto& t : algo_markets) if (t.is_string()) fund.insert(t.get<std::string>());
    for (const auto& p : positions_cache_) fund.insert(p.ticker);
    for (auto it = fund.begin(); it != fund.end();) {
      if (!markets_cache_.count(*it) || now - funding_ts_[*it] < 300) it = fund.erase(it);
      else { funding_ts_[*it] = now; ++it; }
    }
    last_error_.clear();
  }
  for (const auto& t : fund) {
    try {
      json f = client_.call("GET", "/margin/funding_rates/estimate", {{"ticker", t}});
      PerpFunding pf;
      pf.ticker = t;
      pf.rate = d(f, "funding_rate");
      pf.mark = d(f, "mark_price");
      pf.computed_time = s(f, "computed_time");
      pf.next_funding_time = s(f, "next_funding_time");
      Lock lk(mu_);
      funding_cache_[t] = pf;
      store_.exec("UPDATE perp_markets SET funding_rate=?, next_funding=? WHERE ticker=?", {pf.rate, pf.next_funding_time, t});
    } catch (const std::exception& e) {
      LOG_WARN("perps funding %s: %s", t.c_str(), e.what());
    }
  }
}

void PerpsDesk::refresh_account(long long now, bool force) {
  if (!pc_.enabled || !armed()) return;
  if (!force && now - account_ts_ < pc_.account_refresh_seconds) return;
  account_ts_ = now;
  json bj, pj, oj, fj, tiers, rj = json::object();
  long long since = std::atoll(store_.kv_get("perps_fills_since").value_or("0").c_str());
  bool want_tiers = now - fee_ts_ > 3600;
  try {
    bj = client_.call("GET", "/margin/balance", {{"compute_available_balance", "true"}});
    pj = client_.call("GET", "/margin/positions");
    oj = client_.call("GET", "/margin/orders", {{"status", "resting"}, {"limit", "200"}});
    std::map<std::string, std::string> q{{"limit", "200"}};
    if (since > 0) q["min_ts"] = std::to_string(since - 60);
    fj = client_.call("GET", "/margin/fills", q);
  } catch (const std::exception& e) {
    Lock lk(mu_);
    last_error_ = e.what();
    LOG_WARN("perps account: %s", e.what());
    return;
  }
  bool any_position = false;
  for (const auto& x : pj.value("positions", json::array())) if (d(x, "position") != 0) any_position = true;
  if (any_position) {
    try {
      rj = client_.call("GET", "/margin/risk");
    } catch (const std::exception& e) {
      LOG_WARN("perps risk: %s", e.what());
    }
  }
  if (want_tiers) {
    try {
      tiers = client_.call("GET", "/margin/fee_tiers");
    } catch (const std::exception& e) {
      LOG_WARN("perps fee tiers: %s", e.what());
    }
  }
  Lock lk(mu_);
  balance_cache_ = parse_perp_balance(bj);
  store_.exec("INSERT OR REPLACE INTO perp_account(ts,settled_funds,position_value,equity,maintenance_margin,initial_margin,available) "
              "VALUES(?,?,?,?,?,?,?)",
              {now, balance_cache_.settled_funds, balance_cache_.position_value, balance_cache_.equity,
               balance_cache_.maintenance_margin, balance_cache_.initial_margin, balance_cache_.available});
  store_.kv_set("perps_balance", json{{"ts", now}, {"settled_funds", balance_cache_.settled_funds},
                                      {"position_value", balance_cache_.position_value}, {"equity", balance_cache_.equity},
                                      {"maintenance_margin", balance_cache_.maintenance_margin},
                                      {"initial_margin", balance_cache_.initial_margin}, {"available", balance_cache_.available}}.dump());
  positions_cache_.clear();
  store_.exec("DELETE FROM perp_positions");
  std::map<std::string, std::pair<double, double>> risk;   // ticker -> (liquidation price, leverage)
  for (const auto& x : rj.value("positions", json::array()))
    risk[s(x, "market_ticker")] = {ticker_price(x, "estimated_liquidation_price") > 0 ? ticker_price(x, "estimated_liquidation_price") : d(x, "estimated_liquidation_price"),
                                   d(x, "position_leverage")};
  for (const auto& x : pj.value("positions", json::array())) {
    PerpPosition p = parse_perp_position(x);
    if (p.position == 0 || p.ticker.empty()) continue;
    if (risk.count(p.ticker)) { p.liquidation_price = risk[p.ticker].first; p.position_leverage = risk[p.ticker].second; }
    positions_cache_.push_back(p);
    store_.exec("INSERT INTO perp_positions(ticker,subaccount,position,entry_price,unrealized_pnl,margin_used,fees,is_portfolio,updated_ts) "
                "VALUES(?,?,?,?,?,?,?,?,?)",
                {p.ticker, static_cast<long long>(p.subaccount), p.position, p.entry_price, p.unrealized_pnl, p.margin_used, p.fees,
                 static_cast<long long>(p.is_portfolio ? 1 : 0), now});
  }
  std::set<std::string> resting;
  for (const auto& x : oj.value("orders", json::array())) {
    PerpOrder o = parse_perp_order(x);
    if (o.order_id.empty()) continue;
    resting.insert(o.order_id);
    store_.exec("INSERT INTO perp_orders(order_id,client_order_id,ticker,side,status,price,fill_count,remaining_count,created_time,"
                "last_update_time,last_update_reason,order_source,order_reason,note,origin,updated_ts) VALUES(?,?,?,?,'resting',?,?,?,?,?,?,?,?,'','',?) "
                "ON CONFLICT(order_id) DO UPDATE SET status='resting',price=excluded.price,fill_count=excluded.fill_count,"
                "remaining_count=excluded.remaining_count,last_update_time=excluded.last_update_time,"
                "last_update_reason=excluded.last_update_reason,updated_ts=excluded.updated_ts",
                {o.order_id, o.client_order_id, o.ticker, o.side, o.price, o.fill_count, o.remaining_count, o.created_time,
                 o.last_update_time, o.last_update_reason, o.order_source, o.order_reason, now});
  }
  // Whatever the desk thought was resting and the exchange no longer lists is done: filled,
  // canceled by hand, or canceled by the exchange (post-only cross, margin). The fills below say
  // how much of it filled; the rest was canceled.
  for (const auto& r : store_.query("SELECT order_id FROM perp_orders WHERE status='resting'"))
    if (!resting.count(r.value("order_id", "")))
      store_.exec("UPDATE perp_orders SET status='done', remaining_count=0, updated_ts=? WHERE order_id=?", {now, r.value("order_id", "")});
  long long maxts = since;
  std::set<std::string> touched;
  for (const auto& x : fj.value("fills", json::array())) {
    PerpFill f = parse_perp_fill(x);
    if (f.fill_id.empty()) continue;
    bool known = !store_.query("SELECT 1 AS x FROM perp_fills WHERE fill_id=?", {f.fill_id}).empty();
    store_.exec("INSERT OR REPLACE INTO perp_fills(fill_id,order_id,ticker,side,count,price,entry_price,fees,realized_pnl,is_taker,"
                "created_time,ts,order_source) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {f.fill_id, f.order_id, f.ticker, f.side, f.count, f.price, f.entry_price, f.fees, f.realized_pnl,
                 static_cast<long long>(f.is_taker ? 1 : 0), f.created_time, f.ts, f.order_source});
    if (!known) {
      touched.insert(f.order_id);
      if (since > 0)
        store_.log_event("info", "PERPS FILL " + f.ticker + " " + (f.side == "bid" ? "buy" : "sell") + " x" + fmt_fp(f.count) + " @ " +
                                     fmt4(f.price) + (f.is_taker ? " T" : " M") + (f.realized_pnl != 0 ? " realized " + fmt2(f.realized_pnl) : ""));
    }
    maxts = std::max(maxts, f.ts);
  }
  // An order's filled count is the larger of what the exchange's order row said and what its
  // fills add up to; the two are never summed, so a partial fill seen both ways counts once.
  for (const auto& oid : touched)
    store_.exec("UPDATE perp_orders SET fill_count=MAX(fill_count,(SELECT COALESCE(SUM(count),0) FROM perp_fills WHERE perp_fills.order_id=?)), "
                "updated_ts=? WHERE order_id=?", {oid, now, oid});
  if (maxts > 0) store_.kv_set("perps_fills_since", std::to_string(maxts));
  if (want_tiers) {
    if (tiers.is_object()) { fee_tiers_ = tiers; fee_ts_ = now; }
    else fee_ts_ = now - 3000;   // try again in ten minutes, not every cycle
  }
  last_error_.clear();
}

// ---- rails and orders ------------------------------------------------------------------------

void PerpsDesk::rails(const PerpOrderRequest& r, const PerpMarket& m, double* notional_out) {
  if (!pc_.enabled) throw std::runtime_error("perps are switched off in the config (params.perps.enabled)");
  if (!armed()) throw std::runtime_error("no API key configured; cannot send");
  if (store_.kv_get("kill").value_or("0") == "1") throw std::runtime_error("kill switch is on");
  if (r.side != "bid" && r.side != "ask") throw std::runtime_error("side must be bid (buy) or ask (sell)");
  if (!std::isfinite(r.count) || r.count < 0.01) throw std::runtime_error("count must be at least 0.01 contracts");
  if (!m.fractional && std::fabs(r.count - std::round(r.count)) > 1e-9)
    throw std::runtime_error(m.ticker + " trades whole contracts only");
  if (!std::isfinite(r.price) || r.price <= 0) throw std::runtime_error("price must be positive");
  if (!m.is_open || m.status != "active") throw std::runtime_error(m.ticker + " is not open for trading");
  double ref = m.mark > 0 ? m.mark : m.mid();
  if (ref > 0 && std::fabs(r.price - ref) / ref * 100.0 > pc_.price_band_pct)
    throw std::runtime_error("price " + fmt4(r.price) + " is more than " + fmt2(pc_.price_band_pct) + "% from the mark " + fmt4(ref) +
                             " (params.perps.price_band_pct)");
  double notional = r.count * r.price;
  if (notional_out) *notional_out = notional;
  if (r.reduce_only) return;   // taking risk off is never capped
  if (notional > pc_.max_order_notional + 1e-9)
    throw std::runtime_error("order notional $" + fmt2(notional) + " is over the perps order cap $" + fmt2(pc_.max_order_notional));
  double total = notional;
  for (const auto& p : positions_cache_) {
    auto pm = market(p.ticker);
    total += std::fabs(p.position) * (pm ? (pm->mark > 0 ? pm->mark : pm->mid()) : p.entry_price);
  }
  for (const auto& o : store_.query("SELECT price, remaining_count FROM perp_orders WHERE status='resting'"))
    total += o.value("remaining_count", 0.0) * o.value("price", 0.0);
  if (total > pc_.max_total_notional + 1e-9)
    throw std::runtime_error("total perps notional would be $" + fmt2(total) + ", over the cap $" + fmt2(pc_.max_total_notional));
  if (balance_cache_.equity > 0 && total / balance_cache_.equity > pc_.max_leverage + 1e-9)
    throw std::runtime_error("leverage would be " + fmt2(total / balance_cache_.equity) + "x on $" + fmt2(balance_cache_.equity) +
                             " of perps equity, over the " + fmt2(pc_.max_leverage) + "x cap");
  if (balance_cache_.updated_ts <= 0) throw std::runtime_error("perps account not synced yet; sync before trading");
  if (now_s() - balance_cache_.updated_ts > 180) throw std::runtime_error("perps account snapshot is stale; sync before trading");
}

json PerpsDesk::ticket(const PerpOrderRequest& req_in, const std::string& note) {
  Lock lk(mu_);
  auto m = market(req_in.ticker);
  if (!m) throw std::runtime_error("unknown perp market " + req_in.ticker);
  PerpOrderRequest req = req_in;
  req.price = round_tick(req.price, m->tick);
  req.count = floor_contracts(req.count);
  if (req.client_order_id.empty()) req.client_order_id = uuid4();
  double notional = 0;
  rails(req, *m, &notional);
  json body = perp_order_body(req);
  LOG_INFO("perp order -> %s", body.dump().c_str());
  json r;
  try {
    r = client_.call("POST", "/margin/orders", {}, &body);
  } catch (const ApiError& e) {
    store_.log_event("error", "PERPS REJECTED " + req.ticker + " " + req.side + " x" + fmt_fp(req.count) + " @ " + fmt4(req.price) + ": " +
                                  e.body.substr(0, 160));
    throw;
  }
  last_write_ts_ = now_s();
  PerpOrder o;
  o.order_id = s(r, "order_id");
  o.client_order_id = req.client_order_id;
  o.ticker = req.ticker;
  o.side = req.side;
  o.price = req.price;
  o.fill_count = d(r, "fill_count");
  o.remaining_count = d(r, "remaining_count");
  o.status = o.remaining_count > 0 ? "resting" : "done";
  o.created_time = now_iso();
  store_.exec("INSERT OR REPLACE INTO perp_orders(order_id,client_order_id,ticker,side,status,price,fill_count,remaining_count,created_time,"
              "last_update_time,last_update_reason,order_source,order_reason,note,origin,updated_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {o.order_id, o.client_order_id, o.ticker, o.side, o.status, o.price, o.fill_count, o.remaining_count, o.created_time,
               o.created_time, std::string(), std::string("user"), std::string(), note, note.rfind("algo", 0) == 0 ? "algo" : "ticket",
               now_s()});
  store_.log_event("info", "PERPS order " + req.ticker + " " + (req.side == "bid" ? "buy" : "sell") + " x" + fmt_fp(req.count) + " @ " +
                               fmt4(req.price) + " ($" + fmt2(notional) + (req.reduce_only ? ", reduce-only" : "") + ")" +
                               (o.fill_count > 0 ? " filled " + fmt_fp(o.fill_count) + (d(r, "average_fill_price") > 0 ? " @ " + fmt4(d(r, "average_fill_price")) : "") : " resting"));
  account_ts_ = 0;   // read the account next cycle
  return json{{"order_id", o.order_id}, {"fill_count", o.fill_count}, {"remaining_count", o.remaining_count},
              {"average_fill_price", d(r, "average_fill_price")}, {"average_fee_paid", d(r, "average_fee_paid")}, {"notional", notional},
              {"status", o.status}};
}

json PerpsDesk::cancel(const std::string& order_id) {
  Lock lk(mu_);
  if (!armed()) throw std::runtime_error("no API key configured");
  json r = client_.call("DELETE", "/margin/orders/" + order_id, {{"subaccount", "0"}});
  store_.exec("UPDATE perp_orders SET status='canceled', updated_ts=? WHERE order_id=?", {now_s(), order_id});
  store_.log_event("info", "PERPS canceled " + order_id.substr(0, 8) + " (reduced by " + s(r, "reduced_by", "?") + ")");
  account_ts_ = 0;
  return r;
}

int PerpsDesk::cancel_all(const std::string& why) {
  Lock lk(mu_);
  if (!pc_.enabled || !armed()) return 0;
  int n = 0;
  for (const auto& r : store_.query("SELECT order_id, ticker FROM perp_orders WHERE status='resting'")) {
    try {
      cancel(r.value("order_id", ""));
      ++n;
    } catch (const std::exception& e) {
      LOG_WARN("perps cancel %s: %s", r.value("order_id", "").c_str(), e.what());
    }
  }
  if (n > 0) store_.log_event("warn", "PERPS: pulled " + std::to_string(n) + " resting order(s): " + why);
  return n;
}

json PerpsDesk::book(const std::string& ticker) {
  json j = client_.call("GET", "/margin/markets/" + ticker + "/orderbook", {{"depth", "10"}});
  PerpBook b = parse_perp_book(j, ticker);
  json bids = json::array(), asks = json::array();
  for (const auto& l : b.bids) bids.push_back({l.price, l.qty});
  for (const auto& l : b.asks) asks.push_back({l.price, l.qty});
  return json{{"ticker", ticker}, {"bids", bids}, {"asks", asks}, {"best_bid", b.best_bid()}, {"best_ask", b.best_ask()},
              {"bid_size", b.bid_size()}, {"ask_size", b.ask_size()}};
}

json PerpsDesk::close_position(const std::string& ticker) {
  PerpPosition pos;
  bool have = false;
  {
    Lock lk(mu_);
    for (const auto& p : positions_cache_) if (p.ticker == ticker) { pos = p; have = true; }
  }
  if (!have || pos.position == 0) throw std::runtime_error("no perps position in " + ticker);
  json b = book(ticker);
  PerpOrderRequest r;
  r.ticker = ticker;
  r.count = std::fabs(pos.position);
  r.side = pos.position > 0 ? "ask" : "bid";
  r.price = pos.position > 0 ? b.value("best_bid", 0.0) : b.value("best_ask", 0.0);
  if (r.price <= 0) throw std::runtime_error("no " + std::string(pos.position > 0 ? "bid" : "offer") + " to close into");
  r.reduce_only = true;
  r.tif = "immediate_or_cancel";
  return ticket(r, "close position");
}

json PerpsDesk::transfer(const std::string& source, const std::string& destination, double dollars) {
  if (!armed()) throw std::runtime_error("no API key configured");
  auto ok = [](const std::string& x) { return x == "event_contract" || x == "margined"; };
  if (!ok(source) || !ok(destination) || source == destination) throw std::runtime_error("source and destination must be event_contract and margined");
  if (!std::isfinite(dollars) || dollars <= 0 || dollars > 1000) throw std::runtime_error("transfer must be between $0.01 and $1000");
  json body{{"source", source}, {"destination", destination}, {"amount", static_cast<long long>(std::llround(dollars * 10000.0))},
            {"source_exchange_shard", 0}, {"destination_exchange_shard", 0}};
  json r = client_.call("POST", "/portfolio/intra_exchange_instance_transfer", {}, &body);
  std::string id = r.value("transfer_id", "");
  store_.log_event("warn", "moved $" + fmt2(dollars) + " " + source + " -> " + destination + " (transfer " + id + "); asynchronous");
  account_ts_ = 0;
  return json{{"transfer_id", id}, {"note", "asynchronous; both balances update on the next sync"}};
}

// ---- the basis-reversion rule ---------------------------------------------------------------------

double PerpsDesk::algo_pnl_today(long long now) {
  std::string day = algo_state_.value("day", "");
  long long since = day.size() >= 10 ? parse_iso(day + "T00:00:00Z") - tz_offset_seconds(cfg_.risk.day_tz, now) : 0;
  double realized = 0, unrealized = 0;
  for (const auto& r : store_.query("SELECT COALESCE(SUM(realized_pnl),0) AS r FROM perp_algo_trades WHERE state='closed' AND closed_ts>=? AND mode=?",
                                     {since, algo_state_.value("paper", true) ? std::string("paper") : std::string("live")}))
    realized = r.value("r", 0.0);
  for (const auto& r : store_.query("SELECT COALESCE(SUM(unrealized),0) AS u FROM perp_algo_trades WHERE state='open' AND mode=?",
                                     {algo_state_.value("paper", true) ? std::string("paper") : std::string("live")}))
    unrealized = r.value("u", 0.0);
  return realized + unrealized;
}

json PerpsDesk::configure_algo(const json& body) {
  Lock lk(mu_);
  if (!body.is_object()) throw std::invalid_argument("request must be an object");
  json& st = algo_state_;
  if (body.contains("settings") && body["settings"].is_object()) {
    json settings = st["settings"];
    for (auto it = body["settings"].begin(); it != body["settings"].end(); ++it) {
      const std::string k = it.key();
      if (k == "markets") {
        if (!it.value().is_array()) throw std::invalid_argument("markets must be a list of tickers");
        settings["markets"] = it.value();
        continue;
      }
      if (!settings.contains(k)) throw std::invalid_argument("unknown perps setting: " + k);
      if (!it.value().is_number() || !std::isfinite(it.value().get<double>())) throw std::invalid_argument(k + " must be a number");
      settings[k] = it.value();
    }
    auto range = [&](const char* k, double lo, double hi) {
      double x = settings.value(k, 0.0);
      if (x < lo || x > hi) throw std::invalid_argument(std::string(k) + " outside [" + fmt2(lo) + ", " + fmt2(hi) + "]");
    };
    range("entry_bps", 1, 500); range("exit_bps", 0, 400); range("stop_bps", 1, 2000); range("max_hold_minutes", 1, 1440);
    range("contracts", 0.01, 1000); range("daily_loss_stop", 0.01, 10000); range("entry_ttl_seconds", 10, 3600);
  range("notional_dollars", 0, 10000);
    range("cooldown_minutes", 0, 1440);
    // Below ~0.2s no snapshot would ever qualify and the strategy would silently stop
    // trading; above a minute the gate stops meaning anything.
    range("book_max_age_seconds", 0.2, 60);
    if (settings.value("exit_bps", 0.0) >= settings.value("entry_bps", 0.0)) throw std::invalid_argument("exit_bps must be below entry_bps");
    st["settings"] = settings;
    algo_log("info", "perps rule settings changed");
  }
  if (body.contains("paper")) {
    if (!body["paper"].is_boolean()) throw std::invalid_argument("paper must be boolean");
    if (!body["paper"].get<bool>() && !armed()) throw std::invalid_argument("live needs an API key");
    if (st["paper"] != body["paper"]) { st["paper"] = body["paper"]; algo_log("warn", std::string("perps rule mode -> ") + (body["paper"].get<bool>() ? "paper" : "LIVE")); }
  }
  if (body.contains("clear_halt") && body["clear_halt"].is_boolean() && body["clear_halt"].get<bool>()) {
    st["halted"] = false; st["halt_reason"] = "";
    st["loss_base"] = std::min(0.0, algo_pnl_today(now_s()));
    algo_log("warn", "perps rule halt cleared by hand");
  }
  if (body.contains("running")) {
    if (!body["running"].is_boolean()) throw std::invalid_argument("running must be boolean");
    bool want = body["running"].get<bool>();
    if (want && !st.value("paper", true) && !armed()) throw std::invalid_argument("live needs an API key");
    if (want != st.value("running", false)) {
      st["running"] = want;
      algo_log("warn", want ? std::string("perps rule STARTED (") + (st.value("paper", true) ? "paper" : "LIVE") + "): entry " +
                                  fmt2(st["settings"].value("entry_bps", 0.0)) + " bps, exit " + fmt2(st["settings"].value("exit_bps", 0.0)) +
                                  " bps, stop " + fmt2(st["settings"].value("stop_bps", 0.0)) + " bps, " +
                                  fmt2(st["settings"].value("contracts", 1.0)) + " contracts"
                            : "perps rule STOPPED: no new entries; open trades keep their exit rules");
    }
  }
  algo_persist();
  return view();
}

void PerpsDesk::step(long long now) {
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
    if (!first && st.value("halted", false)) { st["halted"] = false; st["halt_reason"] = ""; algo_log("info", "perps rule: new day, loss stop cleared"); }
  }
  const json settings = st["settings"];
  const bool paper = st.value("paper", true);
  const std::string mode = paper ? "paper" : "live";
  // How old the book may be when an entry is priced from it. Two seconds by default: the loop runs
  // every few seconds, and anything older is a price the exchange has already moved past.
  const long long book_max_age_ms =
      static_cast<long long>(1000.0 * settings.value("book_max_age_seconds", 2.0));
  const double entry_bps = settings.value("entry_bps", 25.0), exit_bps = settings.value("exit_bps", 5.0),
               stop_bps = settings.value("stop_bps", 60.0), hold = settings.value("max_hold_minutes", 120.0),
               contracts = settings.value("contracts", 1.0), ttl = settings.value("entry_ttl_seconds", 90.0),
               cooldown = settings.value("cooldown_minutes", 30.0),
               notional_dollars = settings.value("notional_dollars", 0.0);
  std::map<std::string, PerpPosition> real;
  for (const auto& p : positions_cache_) real[p.ticker] = p;
  bool killed = store_.kv_get("kill").value_or("0") == "1";
  // Fee rates, bps of notional, from the account's tier when known.
  auto fee_bps = [&](const std::string& t, bool taker) {
    const char* key = taker ? "taker_fee_rates" : "maker_fee_rates";
    if (fee_tiers_.is_object() && fee_tiers_.contains(key) && fee_tiers_[key].is_object() && fee_tiers_[key].contains(t))
      return fee_tiers_[key][t].get<double>() * 1e4;
    return taker ? 12.0 : 5.0;
  };

  // 1. Manage what the rule holds.
  for (auto t : store_.query("SELECT * FROM perp_algo_trades WHERE mode=? AND state IN ('entering','open') ORDER BY id", {mode})) {
    const std::string ticker = t.value("ticker", ""), side = t.value("side", "");
    auto m = market(ticker);
    if (!m) continue;
    double mid = m->mid(), basis = m->basis_bps();
    bool is_long = side == "bid";
    double count = t.value("count", 0.0);
    long long id = t.value("id", 0LL);
    if (t.value("state", "") == "entering") {
      // A resting maker entry. Live: the exchange's order row is the truth. Paper: it fills if
      // the touch moves through the posted price.
      bool filled = false;
      double fill_px = t.value("entry_price", 0.0);
      if (!paper) {
        json o = store_.query("SELECT status, fill_count, remaining_count FROM perp_orders WHERE order_id=?", {t.value("order_id", "")});
        double fc = o.empty() ? 0.0 : o[0].value("fill_count", 0.0);
        bool resting = !o.empty() && o[0].value("status", "") == "resting";
        if (fc > 0 && (!resting || o[0].value("remaining_count", 0.0) <= 0)) { filled = true; count = fc; }
        else if (fc > 0) { filled = true; count = fc; try { cancel(t.value("order_id", "")); } catch (...) {} }
        else if (!resting && now - t.value("opened_ts", 0LL) > 15) {
          store_.exec("UPDATE perp_algo_trades SET state='canceled', closed_ts=?, reason='entry never filled' WHERE id=?", {now, id});
          continue;
        } else if (now - t.value("opened_ts", 0LL) > ttl || killed) {
          try { cancel(t.value("order_id", "")); } catch (const std::exception& e) { algo_log("warn", ticker + ": could not pull the resting entry: " + e.what()); continue; }
          store_.exec("UPDATE perp_algo_trades SET state='canceled', closed_ts=?, reason='entry pulled unfilled' WHERE id=?", {now, id});
          algo_log("info", ticker + ": maker entry pulled after " + std::to_string(now - t.value("opened_ts", 0LL)) + "s unfilled");
          continue;
        }
      } else {
        if ((is_long && m->ask > 0 && m->ask <= fill_px + 1e-12) || (!is_long && m->bid > 0 && m->bid >= fill_px - 1e-12)) filled = true;
        else if (now - t.value("opened_ts", 0LL) > ttl) {
          store_.exec("UPDATE perp_algo_trades SET state='canceled', closed_ts=?, reason='paper entry never filled' WHERE id=?", {now, id});
          continue;
        }
      }
      if (!filled) continue;
      double fee = count * fill_px * fee_bps(ticker, false) / 1e4;
      store_.exec("UPDATE perp_algo_trades SET state='open', filled_ts=?, count=?, fees=?, mark=?, unrealized=? WHERE id=?",
                  {now, count, fee, mid, (is_long ? mid - fill_px : fill_px - mid) * count - fee, id});
      algo_log("trade", ticker + ": " + (is_long ? "bought " : "sold ") + fmt_fp(count) + " @ " + fmt4(fill_px) + " as a maker (basis " +
                            fmt2(t.value("entry_basis_bps", 0.0)) + " bps)");
      t["state"] = "open"; t["filled_ts"] = now; t["count"] = count;
    }
    // Open: mark it, then the exit rules. Basis reverted, basis ran away, time is up, or the
    // exchange says the position is gone (a liquidation, a hand close).
    double entry = t.value("entry_price", 0.0);
    double exit_px = is_long ? m->bid : m->ask;
    double gross = (is_long ? exit_px - entry : entry - exit_px) * count;
    double exit_fee = count * exit_px * fee_bps(ticker, true) / 1e4;
    double unreal = gross - t.value("fees", 0.0) - exit_fee;
    store_.exec("UPDATE perp_algo_trades SET mark=?, unrealized=? WHERE id=?", {mid, unreal, id});
    std::string reason;
    long long since = t.value("filled_ts", 0LL) > 0 ? t.value("filled_ts", 0LL) : t.value("opened_ts", 0LL);
    if (!paper && !real.count(ticker)) reason = "position gone on the exchange (closed elsewhere or liquidated)";
    else if ((is_long && basis >= -exit_bps) || (!is_long && basis <= exit_bps)) reason = "basis reverted to " + fmt2(basis) + " bps";
    else if ((is_long && basis <= -stop_bps) || (!is_long && basis >= stop_bps)) reason = "basis ran to " + fmt2(basis) + " bps against the position";
    else if (now - since >= static_cast<long long>(hold * 60)) reason = "time stop after " + std::to_string((now - since) / 60) + " min";
    if (reason.empty() || (killed && !paper)) continue;
    if (!paper && real.count(ticker)) {
      try {
        // Reduce-only into the touch. The rails do not cap a reduce-only order.
        PerpOrderRequest r;
        r.ticker = ticker; r.count = std::min(count, std::fabs(real[ticker].position)); r.side = is_long ? "ask" : "bid";
        r.price = exit_px; r.reduce_only = true; r.tif = "immediate_or_cancel"; r.client_order_id = uuid4();
        json res = ticket(r, "algo exit: " + reason);
        double fc = res.value("fill_count", 0.0);
        if (fc <= 0) { algo_log("info", ticker + ": exit IOC did not fill (" + reason + "); retrying next cycle"); continue; }
        double px = res.value("average_fill_price", 0.0) > 0 ? res.value("average_fill_price", 0.0) : exit_px;
        double fee = fc * px * fee_bps(ticker, true) / 1e4;
        double realized = (is_long ? px - entry : entry - px) * fc - t.value("fees", 0.0) * (fc / std::max(count, 1e-9)) - fee;
        if (fc >= count - 1e-9) {
          store_.exec("UPDATE perp_algo_trades SET state='closed', closed_ts=?, exit_price=?, exit_order_id=?, realized_pnl=?, fees=fees+?, "
                      "reason=?, unrealized=0 WHERE id=?", {now, px, res.value("order_id", ""), realized, fee, reason, id});
          algo_log("trade", ticker + " closed: " + reason + " -> " + (realized >= 0 ? "+" : "") + fmt2(realized));
        } else {
          store_.exec("UPDATE perp_algo_trades SET count=count-?, realized_pnl=realized_pnl+?, fees=fees+? WHERE id=?", {fc, realized, fee, id});
          algo_log("trade", ticker + ": partial exit " + fmt_fp(fc) + " of " + fmt_fp(count) + " (" + reason + ")");
        }
      } catch (const std::exception& e) {
        algo_log("warn", ticker + ": exit not sent: " + e.what());
      }
    } else if (!paper) {
      // Gone on the exchange: value it from the fills the exchange reported for this order id chain.
      double realized = 0;
      for (const auto& f : store_.query("SELECT realized_pnl, fees FROM perp_fills WHERE ticker=? AND ts>=?", {ticker, t.value("opened_ts", 0LL)}))
        realized += f.value("realized_pnl", 0.0) - f.value("fees", 0.0);
      store_.exec("UPDATE perp_algo_trades SET state='closed', closed_ts=?, realized_pnl=?, reason=?, unrealized=0 WHERE id=?",
                  {now, realized, reason, id});
      algo_log("warn", ticker + " closed outside the rule: " + reason + " -> " + fmt2(realized));
    } else {
      double realized = unreal;
      store_.exec("UPDATE perp_algo_trades SET state='closed', closed_ts=?, exit_price=?, realized_pnl=?, fees=fees+?, reason=?, unrealized=0 WHERE id=?",
                  {now, exit_px, realized, exit_fee, reason + " (paper)", id});
      algo_log("trade", ticker + " paper closed: " + reason + " -> " + (realized >= 0 ? "+" : "") + fmt2(realized));
    }
  }

  // 2. The loss stop.
  double today = algo_pnl_today(now), base = st.value("loss_base", 0.0), stop = settings.value("daily_loss_stop", 3.0);
  if (st.value("running", false) && !st.value("halted", false) && today - base <= -stop) {
    st["halted"] = true;
    st["halt_reason"] = "daily loss stop: today " + fmt2(today) + " <= -" + fmt2(stop);
    algo_log("warn", st["halt_reason"].get<std::string>());
  }

  // 3. Entries: post against a basis that has stretched past the fees with room to spare.
  if (st.value("running", false) && !st.value("halted", false) && !(killed && !paper) && (paper || armed())) {
    std::set<std::string> held;
    for (const auto& r : store_.query("SELECT ticker FROM perp_algo_trades WHERE mode=? AND state IN ('entering','open')", {mode}))
      held.insert(r.value("ticker", ""));
    // A market just closed out (any reason) sits out the cooldown: a stop followed by an
    // immediate re-entry against the same move is the loop that pays fees twice and learns nothing.
    std::map<std::string, long long> last_close, last_pull;
    for (const auto& r : store_.query("SELECT ticker, MAX(closed_ts) AS t FROM perp_algo_trades WHERE mode=? AND state='closed' GROUP BY ticker", {mode}))
      last_close[r.value("ticker", "")] = r.value("t", 0LL);
    // An entry pulled unfilled waits one TTL before it is posted again: the touch it was
    // joining did not trade, and re-posting every cycle is churn, not patience.
    for (const auto& r : store_.query("SELECT ticker, MAX(closed_ts) AS t FROM perp_algo_trades WHERE mode=? AND state='canceled' GROUP BY ticker", {mode}))
      last_pull[r.value("ticker", "")] = r.value("t", 0LL);
    for (const auto& tj : settings.value("markets", json::array())) {
      if (!tj.is_string()) continue;
      const std::string ticker = tj.get<std::string>();
      if (held.count(ticker)) continue;
      if (!paper && real.count(ticker)) continue;   // a hand position in this market: leave it alone
      if (last_close.count(ticker) && now - last_close[ticker] < static_cast<long long>(cooldown * 60)) continue;
      if (last_pull.count(ticker) && now - last_pull[ticker] < static_cast<long long>(ttl)) continue;
      auto m = market(ticker);
      if (!m || !m->is_open || m->bid <= 0 || m->ask <= 0 || m->reference <= 0) continue;
      if (now * 1000 - m->reference_ts_ms > 120000) continue;   // a stale index is not a basis
      // A stale BOOK is not a basis either, and it is the half that decides whether a post-only
      // price crosses. Only the index was gated here, so an old bid/ask could both fabricate a
      // basis that had already closed and price an order the exchange then refused. Said once per
      // market per run, like the unsized warning, so a quiet feed cannot flood the log.
      const long long book_age_ms = m->quote_received_ts_ms > 0 ? now * 1000 - m->quote_received_ts_ms : -1;
      if (book_age_ms < 0 || book_age_ms > book_max_age_ms) {
        if (stale_book_.insert(ticker).second)
          algo_log("warn", ticker + ": book snapshot is " +
                               (book_age_ms < 0 ? std::string("unstamped") : fmt2(book_age_ms / 1000.0) + "s old") +
                               ", over the " + fmt2(book_max_age_ms / 1000.0) +
                               "s entry limit; no order is priced from it");
        continue;
      }
      stale_book_.erase(ticker);
      double basis = m->basis_bps();
      double needed = fee_bps(ticker, false) + fee_bps(ticker, true) + 5.0;   // maker in, taker out, and something for the desk
      if (std::fabs(basis) < std::max(entry_bps, needed)) continue;
      // Past the stop is not a reversion candidate; it is the exchange telling you the index
      // and the perp have parted for a reason (a halt, a stale index, a squeeze).
      if (std::fabs(basis) >= stop_bps) continue;
      PerpOrderRequest r;
      r.ticker = ticker;
      // Rich perp: sell at the offer (post-only, joining the best ask). Cheap perp: buy at the bid.
      r.side = basis > 0 ? "ask" : "bid";
      r.price = basis > 0 ? m->ask : m->bid;
      // Size in dollars when asked to: the same setting then means the same exposure in every
      // instrument, which is what lets one rule run the whole book.
      if (notional_dollars > 0) {
        double want = notional_dollars / r.price;
        r.count = m->fractional ? floor_contracts(want) : std::floor(want);
        if (r.count < (m->fractional ? 0.01 : 1.0)) {
          // Said once per market per run, not once per cycle: the entry loop runs every few
          // seconds and a market that cannot be sized cannot be sized on the next pass either,
          // so logging it each time would push everything else out of a 150-line log in a minute.
          if (unsized_.insert(ticker).second)
            algo_log("warn", ticker + ": $" + fmt2(notional_dollars) + " does not buy one contract at " +
                                 fmt4(r.price) + "; it will not trade until the size is raised");
          continue;
        }
        unsized_.erase(ticker);
      } else {
        r.count = contracts;
      }
      r.post_only = true;
      r.tif = "good_till_canceled";
      r.client_order_id = uuid4();
      std::string order_id;
      if (!paper) {
        try {
          json res = ticket(r, "algo entry: basis " + fmt2(basis) + " bps");
          order_id = res.value("order_id", "");
          if (res.value("fill_count", 0.0) > 0 && res.value("remaining_count", 0.0) <= 0) {
            // A post-only order that filled at once was a maker fill at the touch (the exchange
            // rejects a post-only that would cross), so the trade is open from this cycle on.
            store_.exec("INSERT INTO perp_algo_trades(ticker,side,mode,state,count,entry_price,entry_basis_bps,opened_ts,filled_ts,order_id,"
                        "exit_order_id,exit_price,closed_ts,realized_pnl,fees,reason,mark,unrealized,day) VALUES(?,?,?,'open',?,?,?,?,?,?,'',0,0,0,?,'',?,0,?)",
                        {ticker, r.side, mode, res.value("fill_count", 0.0), r.price, basis, now, now, order_id,
                         res.value("fill_count", 0.0) * r.price * fee_bps(ticker, false) / 1e4, m->mid(), day});
            algo_log("trade", ticker + ": " + (r.side == "bid" ? "bought " : "sold ") + fmt_fp(res.value("fill_count", 0.0)) + " @ " +
                                  fmt4(r.price) + " at once against a basis of " + fmt2(basis) + " bps");
            continue;
          }
        } catch (const std::exception& e) {
          algo_log("warn", ticker + ": entry not sent: " + e.what());
          continue;
        }
      }
      store_.exec("INSERT INTO perp_algo_trades(ticker,side,mode,state,count,entry_price,entry_basis_bps,opened_ts,filled_ts,order_id,"
                  "exit_order_id,exit_price,closed_ts,realized_pnl,fees,reason,mark,unrealized,day) VALUES(?,?,?,'entering',?,?,?,?,0,?,'',0,0,0,0,'',?,0,?)",
                  {ticker, r.side, mode, r.count, r.price, basis, now, order_id, m->mid(), day});
      algo_log("trade", ticker + ": posted " + (r.side == "bid" ? "buy " : "sell ") + fmt_fp(r.count) + " @ " + fmt4(r.price) +
                            " against a basis of " + fmt2(basis) + " bps" + (paper ? " (paper)" : ""));
    }
  }
  algo_persist();
}

// ---- views --------------------------------------------------------------------------------------------

json PerpsDesk::summary() {
  Lock lk(mu_);
  double unreal = 0, notional = 0;
  for (const auto& p : positions_cache_) {
    unreal += p.unrealized_pnl;
    auto m = market(p.ticker);
    notional += std::fabs(p.position) * (m ? (m->mark > 0 ? m->mark : m->mid()) : p.entry_price);
  }
  return json{{"enabled", pc_.enabled}, {"armed", armed()}, {"equity", balance_cache_.equity}, {"settled_funds", balance_cache_.settled_funds},
              {"unrealized", unreal}, {"positions", positions_cache_.size()}, {"notional", notional},
              {"maintenance_margin", balance_cache_.maintenance_margin}, {"available", balance_cache_.available},
              {"account_ts", balance_cache_.updated_ts}, {"public_ts", public_ts_}, {"last_error", last_error_},
              {"algo_running", algo_state_.value("running", false)}, {"algo_paper", algo_state_.value("paper", true)},
              {"algo_halted", algo_state_.value("halted", false)}};
}

json PerpsDesk::pnl() {
  Lock lk(mu_);
  long long now = now_s();
  std::string day = trading_day(cfg_.risk.day_tz, now);
  long long day_start = parse_iso(day + "T00:00:00Z") - tz_offset_seconds(cfg_.risk.day_tz, now);
  double equity = balance_cache_.equity;
  bool synced = balance_cache_.updated_ts > 0;
  double base = 0;
  long long base_ts = 0;
  for (const auto& r : store_.query("SELECT ts, equity FROM perp_account WHERE ts>=? ORDER BY ts LIMIT 1", {day_start})) {
    base = r.value("equity", 0.0);
    base_ts = r.value("ts", 0LL);
  }
  // Cash moved between the instances: the event-contract side logs them with its own sign, so
  // the perps side is the negative of that.
  double in_today = 0, in_total = 0;
  for (const auto& f : store_.query("SELECT ts, amount FROM cashflows WHERE kind IN ('to_margined','from_margined')")) {
    double amt = -f.value("amount", 0.0);
    in_total += amt;
    if (f.value("ts", 0LL) > base_ts && base_ts > 0) in_today += amt;
  }
  double unreal = 0;
  for (const auto& p : positions_cache_) unreal += p.unrealized_pnl;
  double fills_realized = 0, fills_fees = 0;
  for (const auto& r : store_.query("SELECT COALESCE(SUM(realized_pnl),0) AS r, COALESCE(SUM(fees),0) AS f FROM perp_fills")) {
    fills_realized = r.value("r", 0.0);
    fills_fees = r.value("f", 0.0);
  }
  return json{{"synced", synced}, {"equity", equity}, {"unrealized", unreal},
              {"today", synced && base_ts > 0 ? equity - base - in_today : 0.0},
              {"total", synced && in_total != 0 ? equity - in_total : fills_realized - fills_fees + unreal},
              {"baseline_equity", base}, {"baseline_ts", base_ts}, {"moved_in_today", in_today}, {"moved_in_total", in_total},
              {"trading_pnl", fills_realized - fills_fees + unreal}, {"fees_total", fills_fees}, {"day", day}};
}

json PerpsDesk::view() {
  Lock lk(mu_);
  long long now = now_s();
  json markets = json::array();
  for (const auto& [t, m] : markets_cache_) {
    auto f = funding_cache_.find(t);
    markets.push_back({{"ticker", t}, {"title", m.title}, {"status", m.status}, {"is_open", m.is_open}, {"tick", m.tick},
                       {"contract_size", m.contract_size}, {"bid", m.bid}, {"ask", m.ask}, {"last", m.last}, {"mark", m.mark},
                       {"liquidation_mark", m.liquidation_mark}, {"reference", m.reference}, {"mid", m.mid()}, {"basis_bps", m.basis_bps()},
                       {"spread_bps", m.bid > 0 && m.ask > 0 ? (m.ask - m.bid) / m.mid() * 1e4 : 0.0}, {"leverage", m.leverage},
                       {"volume_24h", m.volume_24h}, {"open_interest", m.open_interest}, {"fractional", m.fractional},
                       {"funding_rate", f == funding_cache_.end() ? json(nullptr) : json(f->second.rate)},
                       {"next_funding", f == funding_cache_.end() ? "" : f->second.next_funding_time},
                       {"reference_age_s", m.reference_ts_ms > 0 ? (now * 1000 - m.reference_ts_ms) / 1000 : -1},
                       {"book_age_ms", m.quote_received_ts_ms > 0 ? now * 1000 - m.quote_received_ts_ms : -1}});
  }
  json positions = json::array();
  double total_notional = 0, total_unreal = 0;
  for (const auto& p : positions_cache_) {
    auto m = market(p.ticker);
    double mark = m ? (m->mark > 0 ? m->mark : m->mid()) : p.entry_price;
    double notional = std::fabs(p.position) * mark;
    total_notional += notional;
    total_unreal += p.unrealized_pnl;
    positions.push_back({{"ticker", p.ticker}, {"position", p.position}, {"side", p.position > 0 ? "long" : "short"},
                         {"entry_price", p.entry_price}, {"mark", mark}, {"notional", notional}, {"unrealized_pnl", p.unrealized_pnl},
                         {"margin_used", p.margin_used}, {"fees", p.fees}, {"is_portfolio", p.is_portfolio},
                         {"bid", m ? m->bid : 0.0}, {"ask", m ? m->ask : 0.0},
                         {"liquidation_price", p.liquidation_price}, {"position_leverage", p.position_leverage},
                         {"leverage", m && m->leverage > 0 ? m->leverage : 0.0}});
  }
  json algo = algo_state_;
  algo["positions"] = store_.query("SELECT * FROM perp_algo_trades WHERE state IN ('entering','open') ORDER BY id DESC");
  algo["history"] = store_.query("SELECT * FROM perp_algo_trades WHERE state IN ('closed','canceled') ORDER BY id DESC LIMIT 100");
  algo["pnl_today"] = algo_pnl_today(now);
  json fees = json::object();
  if (fee_tiers_.is_object()) fees = fee_tiers_;
  return json{{"enabled", pc_.enabled}, {"armed", armed()}, {"last_error", last_error_}, {"public_ts", public_ts_},
              {"account_ts", balance_cache_.updated_ts},
              {"balance", {{"settled_funds", balance_cache_.settled_funds}, {"position_value", balance_cache_.position_value},
                           {"equity", balance_cache_.equity}, {"maintenance_margin", balance_cache_.maintenance_margin},
                           {"initial_margin", balance_cache_.initial_margin}, {"resting_margin", balance_cache_.resting_margin},
                           {"available", balance_cache_.available}}},
              {"rails", {{"max_order_notional", pc_.max_order_notional}, {"max_total_notional", pc_.max_total_notional},
                         {"max_leverage", pc_.max_leverage}, {"price_band_pct", pc_.price_band_pct}}},
              {"markets", markets}, {"positions", positions}, {"total_notional", total_notional}, {"total_unrealized", total_unreal},
              {"leverage_now", balance_cache_.equity > 0 ? total_notional / balance_cache_.equity : 0.0},
              {"orders", store_.query("SELECT * FROM perp_orders WHERE status='resting' ORDER BY created_time DESC")},
              {"recent_orders", store_.query("SELECT * FROM perp_orders ORDER BY updated_ts DESC LIMIT 30")},
              {"fills", store_.query("SELECT * FROM perp_fills ORDER BY ts DESC LIMIT 50")},
              {"fees", fees}, {"algo", algo},
              {"basis_history", store_.query("SELECT ticker, ts, bid, ask, mark, reference FROM perp_quotes WHERE ts>? ORDER BY ts", {now - 6 * 3600})}};
}

}  // namespace desk
