#include "desk/store.h"
#include "desk/timeutil.h"

#include <sqlite3.h>

#include <stdexcept>

#include "desk/log.h"

namespace desk {

namespace {

const char* SCHEMA = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS series(
  ticker TEXT PRIMARY KEY, category TEXT, title TEXT, fee_type TEXT, fee_multiplier REAL,
  frequency TEXT, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS markets(
  ticker TEXT PRIMARY KEY, event_ticker TEXT, series_ticker TEXT, title TEXT, subtitle TEXT,
  yes_sub_title TEXT, no_sub_title TEXT, status TEXT, market_type TEXT, strike_type TEXT,
  floor_strike REAL, cap_strike REAL, open_time TEXT, close_time TEXT,
  expected_expiration_time TEXT, expiration_time TEXT, result TEXT, rules_primary TEXT,
  tick REAL, yes_bid REAL, yes_ask REAL, no_bid REAL, no_ask REAL, last REAL,
  volume REAL, open_interest REAL, custom_strike TEXT, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS market_routing(ticker TEXT PRIMARY KEY, exchange_index INTEGER);
CREATE TABLE IF NOT EXISTS event_meta(event_ticker TEXT PRIMARY KEY, title TEXT, series_ticker TEXT,
  mutually_exclusive INTEGER, exhaustive INTEGER, checked_ts INTEGER);
CREATE TABLE IF NOT EXISTS ext_wp(id INTEGER PRIMARY KEY, ts INTEGER, event_ticker TEXT, espn_id TEXT, league TEXT,
  state TEXT, period INTEGER, clock TEXT, home_score INTEGER, away_score INTEGER, wp_home REAL, wp_leadtime REAL,
  mu_home REAL, mkt_home REAL, mkt_away REAL, home_ticker TEXT, away_ticker TEXT);
CREATE INDEX IF NOT EXISTS idx_ext_wp_event ON ext_wp(event_ticker, ts);
CREATE TABLE IF NOT EXISTS cashflows(id TEXT PRIMARY KEY, ts INTEGER, kind TEXT, amount REAL);
CREATE TABLE IF NOT EXISTS realized(ticker TEXT PRIMARY KEY, realized_pnl REAL, fees_paid REAL, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS brackets(ticker TEXT PRIMARY KEY, tp_price REAL, sl_price REAL, trail REAL, mode TEXT,
  tp_dollars REAL, sl_dollars REAL, note TEXT, created_ts INTEGER, updated_ts INTEGER, alerted TEXT);
CREATE INDEX IF NOT EXISTS idx_markets_series ON markets(series_ticker, status);
CREATE INDEX IF NOT EXISTS idx_markets_event ON markets(event_ticker);
CREATE TABLE IF NOT EXISTS quotes(
  id INTEGER PRIMARY KEY, ts INTEGER, ticker TEXT, yes_bid REAL, yes_ask REAL, no_bid REAL,
  no_ask REAL, last REAL, yes_bid_size REAL, yes_ask_size REAL, volume REAL, open_interest REAL);
CREATE INDEX IF NOT EXISTS idx_quotes_ticker ON quotes(ticker, ts);
CREATE TABLE IF NOT EXISTS books(id INTEGER PRIMARY KEY, ts INTEGER, ticker TEXT, book TEXT);
CREATE INDEX IF NOT EXISTS idx_books_ticker ON books(ticker, ts);
CREATE TABLE IF NOT EXISTS fair_values(
  ticker TEXT PRIMARY KEY, p REAL, sigma REAL, note TEXT, ts INTEGER);
CREATE TABLE IF NOT EXISTS model_fairs(
  id INTEGER PRIMARY KEY, ts INTEGER, ticker TEXT, strategy TEXT, p REAL, sigma REAL,
  yes_bid REAL, yes_ask REAL);
CREATE INDEX IF NOT EXISTS idx_model_fairs ON model_fairs(ticker, strategy, ts);
CREATE TABLE IF NOT EXISTS proposals(
  id INTEGER PRIMARY KEY, ts INTEGER, updated_ts INTEGER, ticker TEXT, event_ticker TEXT,
  series_ticker TEXT, strategy TEXT, mode TEXT, outcome_side TEXT, exec_style TEXT,
  status TEXT, order_id TEXT, note TEXT, cluster TEXT, price REAL, count REAL, p_model REAL,
  sigma REAL, fee_per REAL, net_edge REAL, yes_bid REAL, yes_ask REAL,
  entry_kind TEXT DEFAULT 'new', approval_required INTEGER DEFAULT 0, required_edge REAL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_proposals_status ON proposals(status, ts);
CREATE TABLE IF NOT EXISTS trade_decisions(
  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, decision_id TEXT, event TEXT, ticker TEXT,
  event_ticker TEXT, side TEXT, category TEXT, model_name TEXT, model_version TEXT, p_yes REAL,
  fair_probability REAL, sigma REAL, bid REAL, ask REAL, entry_price REAL, fee REAL, slippage REAL,
  uncertainty_buffer REAL, raw_edge REAL, executable_edge REAL, route TEXT, verdict TEXT, style TEXT,
  count REAL, reasons_json TEXT, inputs_json TEXT, quote_json TEXT, model_ts INTEGER, book_ts INTEGER,
  proposal_id INTEGER, note TEXT, source_decision_id TEXT DEFAULT '');
CREATE INDEX IF NOT EXISTS idx_trade_decisions_id ON trade_decisions(decision_id, id);
CREATE INDEX IF NOT EXISTS idx_trade_decisions_ticker ON trade_decisions(ticker, ts);
CREATE INDEX IF NOT EXISTS idx_trade_decisions_proposal ON trade_decisions(proposal_id);
CREATE TABLE IF NOT EXISTS orders(
  order_id TEXT PRIMARY KEY, client_order_id TEXT, ticker TEXT, outcome_side TEXT,
  book_side TEXT, type TEXT, status TEXT, yes_price REAL, no_price REAL, initial_count REAL,
  fill_count REAL, remaining_count REAL, taker_fees REAL, maker_fees REAL, created_time TEXT,
  last_update_time TEXT, proposal_id INTEGER, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS fills(
  fill_id TEXT PRIMARY KEY, trade_id TEXT, order_id TEXT, ticker TEXT, outcome_side TEXT,
  book_side TEXT, count REAL, yes_price REAL, no_price REAL, fee REAL, is_taker INTEGER,
  created_time TEXT, ts INTEGER);
CREATE INDEX IF NOT EXISTS idx_fills_ticker ON fills(ticker);
CREATE TABLE IF NOT EXISTS positions(
  ticker TEXT PRIMARY KEY, position REAL, exposure REAL, realized_pnl REAL, fees_paid REAL,
  total_traded REAL, updated TEXT, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS settlements(
  ticker TEXT, event_ticker TEXT, market_result TEXT, yes_count REAL, no_count REAL,
  yes_cost REAL, no_cost REAL, revenue REAL, fee REAL, settled_time TEXT,
  PRIMARY KEY(ticker, settled_time));
CREATE TABLE IF NOT EXISTS account(ts INTEGER PRIMARY KEY, balance REAL, portfolio_value REAL);
CREATE TABLE IF NOT EXISTS hypotheses(
  id INTEGER PRIMARY KEY, key TEXT UNIQUE, category TEXT, title TEXT, statement TEXT,
  data_needed TEXT, test TEXT, status TEXT, notes TEXT, updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS events_log(id INTEGER PRIMARY KEY, ts INTEGER, level TEXT, msg TEXT);
CREATE TABLE IF NOT EXISTS ext_prices(id INTEGER PRIMARY KEY, ts INTEGER, symbol TEXT, price REAL);
CREATE INDEX IF NOT EXISTS idx_ext_prices ON ext_prices(symbol, ts);
CREATE TABLE IF NOT EXISTS ext_weather(id INTEGER PRIMARY KEY, ts INTEGER, series TEXT, date TEXT,
  fc_max REAL, fc_min REAL, obs_max REAL, obs_min REAL, have_obs INTEGER);
CREATE TABLE IF NOT EXISTS ext_sports(id INTEGER PRIMARY KEY, ts INTEGER, event_ticker TEXT, espn_id TEXT,
  state TEXT, minute REAL, home TEXT, away TEXT, home_score INTEGER, away_score INTEGER, red_home INTEGER,
  red_away INTEGER, sot_home REAL, sot_away REAL, poss_home REAL, lam_home REAL, lam_away REAL,
  p_home REAL, p_draw REAL, p_away REAL, mkt_home REAL, mkt_draw REAL, mkt_away REAL);
CREATE INDEX IF NOT EXISTS idx_ext_sports ON ext_sports(event_ticker, ts);
CREATE TABLE IF NOT EXISTS match_state(event_ticker TEXT PRIMARY KEY, espn_id TEXT, phase TEXT,
  phase_ts INTEGER, home_score INTEGER, away_score INTEGER, minute REAL, period INTEGER,
  red_home INTEGER, red_away INTEGER, status_name TEXT, state TEXT, data_ts INTEGER,
  updated_ts INTEGER);
CREATE TABLE IF NOT EXISTS rate_fits(id INTEGER PRIMARY KEY, ts INTEGER, event_ticker TEXT,
  model_version TEXT, calibration TEXT, phase TEXT, minute REAL, home_score INTEGER,
  away_score INTEGER, p_home REAL, p_draw REAL, p_away REAL, lam_home REAL, lam_away REAL,
  residual REAL, converged INTEGER);
CREATE INDEX IF NOT EXISTS idx_rate_fits ON rate_fits(event_ticker, ts);
CREATE TABLE IF NOT EXISTS contract_universe(ticker TEXT PRIMARY KEY, event_ticker TEXT,
  series_ticker TEXT, local_date TEXT, tz TEXT, settle_source TEXT, settle_product TEXT,
  phase TEXT, close_ts INTEGER, expiration_ts INTEGER, refreshed_ts INTEGER);
CREATE INDEX IF NOT EXISTS idx_universe_phase ON contract_universe(phase, local_date);
CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY, source TEXT, subject TEXT,
  kind TEXT, valid_ts INTEGER, init_ts INTEGER, published_ts INTEGER, received_ts INTEGER,
  payload TEXT);
CREATE INDEX IF NOT EXISTS idx_obs ON observations(source, subject, received_ts);
CREATE INDEX IF NOT EXISTS idx_obs_valid ON observations(source, subject, valid_ts);
CREATE TABLE IF NOT EXISTS manual_requests(id TEXT PRIMARY KEY,payload TEXT,ticker TEXT,event_ticker TEXT,status TEXT,created_ts INTEGER,expires_ts INTEGER,updated_ts INTEGER,result TEXT,error TEXT);
)SQL";

class Stmt {
 public:
  Stmt(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) + " :: " + sql);
  }
  ~Stmt() {
    if (st_) sqlite3_finalize(st_);
  }
  void bind(const std::vector<SqlValue>& params) {
    int i = 1;
    for (const auto& p : params) {
      std::visit(
          [&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) sqlite3_bind_null(st_, i);
            else if constexpr (std::is_same_v<T, long long>) sqlite3_bind_int64(st_, i, v);
            else if constexpr (std::is_same_v<T, double>) sqlite3_bind_double(st_, i, v);
            else sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT);
          },
          p);
      ++i;
    }
  }
  bool step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errstr(rc));
  }
  json row() {
    json r = json::object();
    int n = sqlite3_column_count(st_);
    for (int c = 0; c < n; ++c) {
      const char* name = sqlite3_column_name(st_, c);
      switch (sqlite3_column_type(st_, c)) {
        case SQLITE_INTEGER: r[name] = static_cast<long long>(sqlite3_column_int64(st_, c)); break;
        case SQLITE_FLOAT: r[name] = sqlite3_column_double(st_, c); break;
        case SQLITE_TEXT:
          r[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st_, c)));
          break;
        default: r[name] = nullptr;
      }
    }
    return r;
  }
  sqlite3_stmt* st_ = nullptr;
};

double num(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null()) return 0;
  if (it->is_number()) return it->get<double>();
  return 0;
}
std::string str(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null()) return "";
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}
long long ll(const json& r, const char* k) { return static_cast<long long>(num(r, k)); }

}  // namespace

Store::Store(const std::string& path) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
    throw std::runtime_error("cannot open db " + path + ": " + sqlite3_errmsg(db_));
  sqlite3_busy_timeout(db_, 5000);
  migrate();
}

Store::~Store() {
  if (db_) sqlite3_close(db_);
}

void Store::migrate() {
  char* err = nullptr;
  if (sqlite3_exec(db_, SCHEMA, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "?";
    sqlite3_free(err);
    throw std::runtime_error("schema: " + e);
  }
  // Additive migrations: SQLite has no ADD COLUMN IF NOT EXISTS, so ignore "duplicate column".
  for (const char* sql : {"ALTER TABLE ext_weather ADD COLUMN ens_mean REAL",
                          "ALTER TABLE ext_weather ADD COLUMN ens_sd REAL",
                          "ALTER TABLE ext_wp ADD COLUMN home_ticker TEXT",
                          "ALTER TABLE ext_wp ADD COLUMN away_ticker TEXT",
                          "ALTER TABLE markets ADD COLUMN rules_secondary TEXT",
                          "ALTER TABLE markets ADD COLUMN yes_bid_size REAL",
                          "ALTER TABLE markets ADD COLUMN yes_ask_size REAL",
                          "ALTER TABLE proposals ADD COLUMN entry_kind TEXT DEFAULT 'new'",
                          "ALTER TABLE proposals ADD COLUMN approval_required INTEGER DEFAULT 0",
                          "ALTER TABLE proposals ADD COLUMN required_edge REAL DEFAULT 0",
                          "ALTER TABLE proposals ADD COLUMN decision_id TEXT DEFAULT ''",
                          "ALTER TABLE orders ADD COLUMN decision_id TEXT DEFAULT ''",
                          "ALTER TABLE trade_decisions ADD COLUMN source_decision_id TEXT DEFAULT ''"}) {
    char* e2 = nullptr;
    sqlite3_exec(db_, sql, nullptr, nullptr, &e2);
    if (e2) sqlite3_free(e2);
  }
}

json Store::query(const std::string& sql, const std::vector<SqlValue>& params) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Stmt s(db_, sql);
  s.bind(params);
  json rows = json::array();
  while (s.step()) rows.push_back(s.row());
  return rows;
}

void Store::exec(const std::string& sql, const std::vector<SqlValue>& params) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Stmt s(db_, sql);
  s.bind(params);
  s.step();
}

// ---- reference data -------------------------------------------------------------

void Store::upsert_series(const Series& s) {
  exec(
      "INSERT INTO series(ticker,category,title,fee_type,fee_multiplier,frequency,updated_ts) "
      "VALUES(?,?,?,?,?,?,?) ON CONFLICT(ticker) DO UPDATE SET category=excluded.category,"
      "title=excluded.title,fee_type=excluded.fee_type,fee_multiplier=excluded.fee_multiplier,"
      "frequency=excluded.frequency,updated_ts=excluded.updated_ts",
      {s.ticker, s.category, s.title, s.fee_type, s.fee_multiplier, s.frequency, now_s()});
}

std::optional<Series> Store::get_series(const std::string& ticker) {
  json rows = query("SELECT * FROM series WHERE ticker=?", {ticker});
  if (rows.empty()) return std::nullopt;
  const json& r = rows[0];
  Series s;
  s.ticker = str(r, "ticker");
  s.category = str(r, "category");
  s.title = str(r, "title");
  s.fee_type = str(r, "fee_type");
  s.fee_multiplier = num(r, "fee_multiplier");
  s.frequency = str(r, "frequency");
  return s;
}

void Store::replace_risk_snapshot(const Balance& balance, const std::vector<Position>& ps,
                                  const std::vector<Order>& os, const json& shards, long long ts) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  exec("BEGIN IMMEDIATE");
  try {
    insert_account(ts,balance.balance,balance.portfolio_value);
    replace_positions(ps);
    exec("UPDATE orders SET status='done' WHERE status='resting'");
    for(const auto& o:os) upsert_order(o,0);
    kv_set("portfolio_sync_ts",std::to_string(ts));
    kv_set("shard_balances",shards.dump());
    exec("COMMIT");
  } catch(...) { exec("ROLLBACK"); throw; }
}

void Store::upsert_market(const Market& m) {
  exec("INSERT INTO market_routing VALUES(?,?) ON CONFLICT(ticker) DO UPDATE SET exchange_index=excluded.exchange_index",
       {m.ticker, static_cast<long long>(m.exchange_index)});
  exec(
      "INSERT INTO markets(ticker,event_ticker,series_ticker,title,subtitle,yes_sub_title,"
      "no_sub_title,status,market_type,strike_type,floor_strike,cap_strike,open_time,close_time,"
      "expected_expiration_time,expiration_time,result,rules_primary,rules_secondary,tick,yes_bid,yes_ask,no_bid,"
      "no_ask,last,volume,open_interest,custom_strike,updated_ts,yes_bid_size,yes_ask_size) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(ticker) DO UPDATE SET status=excluded.status,result=excluded.result,"
      "yes_bid=excluded.yes_bid,yes_ask=excluded.yes_ask,no_bid=excluded.no_bid,"
      "no_ask=excluded.no_ask,last=excluded.last,volume=excluded.volume,"
      "open_interest=excluded.open_interest,close_time=excluded.close_time,"
      "expected_expiration_time=excluded.expected_expiration_time,rules_primary=excluded.rules_primary,"
      "rules_secondary=excluded.rules_secondary,updated_ts=excluded.updated_ts,"
      "yes_bid_size=excluded.yes_bid_size,yes_ask_size=excluded.yes_ask_size",
      {m.ticker, m.event_ticker, m.series_ticker, m.title, m.subtitle, m.yes_sub_title,
       m.no_sub_title, m.status, m.market_type, m.strike_type,
       m.floor_strike ? SqlValue(*m.floor_strike) : SqlValue(nullptr),
       m.cap_strike ? SqlValue(*m.cap_strike) : SqlValue(nullptr), m.open_time, m.close_time,
       m.expected_expiration_time, m.expiration_time, m.result, m.rules_primary, m.rules_secondary, m.tick, m.yes_bid,
       m.yes_ask, m.no_bid, m.no_ask, m.last, m.volume, m.open_interest,
       m.custom_strike.is_null() ? std::string("") : m.custom_strike.dump(), now_s(), m.yes_bid_size, m.yes_ask_size});
}

static Market market_from_row(const json& r) {
  Market m;
  m.ticker = str(r, "ticker");
  m.event_ticker = str(r, "event_ticker");
  m.series_ticker = str(r, "series_ticker");
  m.title = str(r, "title");
  m.subtitle = str(r, "subtitle");
  m.yes_sub_title = str(r, "yes_sub_title");
  m.no_sub_title = str(r, "no_sub_title");
  m.status = str(r, "status");
  m.market_type = str(r, "market_type");
  m.strike_type = str(r, "strike_type");
  if (!r["floor_strike"].is_null()) m.floor_strike = num(r, "floor_strike");
  if (!r["cap_strike"].is_null()) m.cap_strike = num(r, "cap_strike");
  m.open_time = str(r, "open_time");
  m.close_time = str(r, "close_time");
  m.expected_expiration_time = str(r, "expected_expiration_time");
  m.expiration_time = str(r, "expiration_time");
  m.result = str(r, "result");
  m.rules_primary = str(r, "rules_primary");
  m.rules_secondary = str(r, "rules_secondary");
  m.tick = num(r, "tick") > 0 ? num(r, "tick") : 0.01;
  m.yes_bid = num(r, "yes_bid");
  m.yes_ask = num(r, "yes_ask");
  m.no_bid = num(r, "no_bid");
  m.no_ask = num(r, "no_ask");
  m.last = num(r, "last");
  m.volume = num(r, "volume");
  m.open_interest = num(r, "open_interest");
  m.yes_bid_size = num(r, "yes_bid_size");
  m.yes_ask_size = num(r, "yes_ask_size");
  m.quote_ts = static_cast<long long>(num(r, "updated_ts"));
  std::string cs = str(r, "custom_strike");
  if (!cs.empty()) m.custom_strike = json::parse(cs, nullptr, false);
  return m;
}

std::optional<Market> Store::get_market(const std::string& ticker) {
  json rows = query("SELECT * FROM markets WHERE ticker=?", {ticker});
  if (rows.empty()) return std::nullopt;
  Market m = market_from_row(rows[0]);
  auto route = query("SELECT exchange_index FROM market_routing WHERE ticker=?", {ticker});
  if (!route.empty()) m.exchange_index = route[0]["exchange_index"].get<int>();
  return m;
}

std::vector<Market> Store::markets_in_event(const std::string& event_ticker) {
  std::vector<Market> out;
  for (const auto& r : query("SELECT * FROM markets WHERE event_ticker=? ORDER BY ticker", {event_ticker}))
    out.push_back(market_from_row(r));
  auto routes = query("SELECT ticker,exchange_index FROM market_routing WHERE ticker IN "
                      "(SELECT ticker FROM markets WHERE event_ticker=?)", {event_ticker});
  for (auto& m : out)
    for (const auto& r : routes)
      if (r.value("ticker", "") == m.ticker) m.exchange_index = static_cast<int>(r.value("exchange_index", 0.0));
  return out;
}

std::vector<Market> Store::open_markets(const std::string& series_ticker) {
  json rows = series_ticker.empty()
                  ? query("SELECT * FROM markets WHERE status IN ('active','open') ORDER BY ticker")
                  : query("SELECT * FROM markets WHERE status IN ('active','open') AND series_ticker=? "
                          "ORDER BY ticker",
                          {series_ticker});
  std::vector<Market> out;
  const long long now = now_s();
  for (const auto& r : rows) {
    Market m = market_from_row(r);
    // The open-market feed no longer contains yesterday's closed contracts, so their cached
    // status can stay "active" forever. Keep the rows for history, never in the live list.
    long long closes = parse_iso(m.close_time);
    if (closes <= 0) closes = parse_iso(m.expected_expiration_time);
    if (closes > 0 && closes <= now) continue;
    out.push_back(std::move(m));
  }
  return out;
}

void Store::insert_quote(long long ts, const Market& m) {
  exec(
      "INSERT INTO quotes(ts,ticker,yes_bid,yes_ask,no_bid,no_ask,last,yes_bid_size,yes_ask_size,"
      "volume,open_interest) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
      {ts, m.ticker, m.yes_bid, m.yes_ask, m.no_bid, m.no_ask, m.last, m.yes_bid_size, m.yes_ask_size,
       m.volume, m.open_interest});
}

void Store::insert_book(long long ts, const Orderbook& b) {
  exec("INSERT INTO books(ts,ticker,book) VALUES(?,?,?)", {ts, b.ticker, orderbook_to_json(b).dump()});
}

// ---- fair values ------------------------------------------------------------------

void Store::set_fair(const FairValue& f) {
  exec(
      "INSERT INTO fair_values(ticker,p,sigma,note,ts) VALUES(?,?,?,?,?) ON CONFLICT(ticker) DO UPDATE "
      "SET p=excluded.p,sigma=excluded.sigma,note=excluded.note,ts=excluded.ts",
      {f.ticker, f.p, f.sigma, f.note, now_s()});
}

void Store::clear_fair(const std::string& ticker) {
  exec("DELETE FROM fair_values WHERE ticker=?", {ticker});
}

std::vector<FairValue> Store::fairs() {
  std::vector<FairValue> out;
  for (const auto& r : query("SELECT * FROM fair_values ORDER BY ts DESC")) {
    FairValue f;
    f.ticker = str(r, "ticker");
    f.p = num(r, "p");
    f.sigma = num(r, "sigma");
    f.note = str(r, "note");
    f.ts = ll(r, "ts");
    out.push_back(f);
  }
  return out;
}

void Store::insert_model_fair(long long ts, const std::string& ticker, const std::string& strategy,
                              double p, double sigma, double yes_bid, double yes_ask) {
  exec("INSERT INTO model_fairs(ts,ticker,strategy,p,sigma,yes_bid,yes_ask) VALUES(?,?,?,?,?,?,?)",
       {ts, ticker, strategy, p, sigma, yes_bid, yes_ask});
}

// ---- proposals -------------------------------------------------------------------

json Proposal::to_json() const {
  return json{{"id", id},
              {"ts", ts},
              {"updated_ts", updated_ts},
              {"ticker", ticker},
              {"event_ticker", event_ticker},
              {"series_ticker", series_ticker},
              {"strategy", strategy},
              {"mode", mode},
              {"outcome_side", outcome_side},
              {"exec_style", exec_style},
              {"status", status},
              {"order_id", order_id},
              {"note", note},
              {"cluster", cluster},
              {"price", price},
              {"count", count},
              {"p_model", p_model},
              {"sigma", sigma},
              {"fee_per", fee_per},
              {"net_edge", net_edge},
              {"yes_bid", yes_bid},
              {"yes_ask", yes_ask},
              {"entry_kind", entry_kind},
              {"approval_required", approval_required},
              {"required_edge", required_edge},
              {"decision_id", decision_id}};
}

Proposal Store::proposal_from_row(const json& r) {
  Proposal p;
  p.id = ll(r, "id");
  p.ts = ll(r, "ts");
  p.updated_ts = ll(r, "updated_ts");
  p.ticker = str(r, "ticker");
  p.event_ticker = str(r, "event_ticker");
  p.series_ticker = str(r, "series_ticker");
  p.strategy = str(r, "strategy");
  p.mode = str(r, "mode");
  p.outcome_side = str(r, "outcome_side");
  p.exec_style = str(r, "exec_style");
  p.status = str(r, "status");
  p.order_id = str(r, "order_id");
  p.note = str(r, "note");
  p.cluster = str(r, "cluster");
  p.price = num(r, "price");
  p.count = num(r, "count");
  p.p_model = num(r, "p_model");
  p.sigma = num(r, "sigma");
  p.fee_per = num(r, "fee_per");
  p.net_edge = num(r, "net_edge");
  p.yes_bid = num(r, "yes_bid");
  p.yes_ask = num(r, "yes_ask");
  p.entry_kind = str(r, "entry_kind");
  if (p.entry_kind.empty()) p.entry_kind = "new";
  p.approval_required = ll(r, "approval_required") != 0;
  p.required_edge = num(r, "required_edge");
  p.decision_id = str(r, "decision_id");
  return p;
}

long long Store::insert_proposal(Proposal& p) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (p.ts == 0) p.ts = now_s();
  p.updated_ts = now_s();
  exec(
      "INSERT INTO proposals(ts,updated_ts,ticker,event_ticker,series_ticker,strategy,mode,"
      "outcome_side,exec_style,status,order_id,note,cluster,price,count,p_model,sigma,fee_per,"
      "net_edge,yes_bid,yes_ask,entry_kind,approval_required,required_edge,decision_id) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {p.ts, p.updated_ts, p.ticker, p.event_ticker, p.series_ticker, p.strategy, p.mode,
       p.outcome_side, p.exec_style, p.status, p.order_id, p.note, p.cluster, p.price, p.count,
       p.p_model, p.sigma, p.fee_per, p.net_edge, p.yes_bid, p.yes_ask, p.entry_kind,
       static_cast<long long>(p.approval_required ? 1 : 0), p.required_edge, p.decision_id});
  p.id = static_cast<long long>(sqlite3_last_insert_rowid(db_));
  return p.id;
}

bool Store::proposal_terminal(const std::string& s) {
  return s == "filled" || s == "canceled" || s == "expired" || s == "failed" || s == "rejected";
}

bool Store::proposal_transition_ok(const std::string& from, const std::string& to) {
  if (from.empty() || from == to) return true;          // creation, and idempotent rewrites
  if (proposal_terminal(from)) return false;            // nothing comes back from the dead
  if (to == "submitted") return from == "pending" || from == "unknown";   // only something live can be sent
  // A cancel is asked for on an order that is working, and working means submitted (or an
  // uncertain send). Executor::cancel writes submitted -> cancel_pending before it calls the
  // exchange; refusing that move (as this once did) left the proposal claiming it still rested
  // and turned the whole "cancel outcome unknown" state into dead code. From cancel_pending the
  // live-to-terminal rule below lets it end canceled or filled (a fill that raced the cancel).
  // Never from pending: a pending proposal has no order, so nothing could ever resolve it.
  if (to == "cancel_pending") return from == "submitted" || from == "unknown";
  if (to == "unknown") return from == "pending" || from == "submitted";
  if (to == "pending") return false;                    // a proposal is only pending when created
  return true;                                          // every terminal end is reachable from live
}

void Store::update_proposal(const Proposal& p) {
  // Reject an illegal move rather than writing it. The alternative is a corrupted row that looks
  // ordinary, and the failure it causes turns up later somewhere else entirely.
  json before = query("SELECT status FROM proposals WHERE id=?", {p.id});
  if (!before.empty()) {
    std::string from = before[0].value("status", "");
    if (!proposal_transition_ok(from, p.status)) {
      log_event("error", "refused illegal proposal transition " + from + " -> " + p.status +
                             " on " + p.ticker + " (proposal " + std::to_string(p.id) + ")");
      return;
    }
  }
  exec(
      "UPDATE proposals SET updated_ts=?,status=?,order_id=?,note=?,price=?,count=?,p_model=?,"
      "sigma=?,fee_per=?,net_edge=?,yes_bid=?,yes_ask=?,exec_style=?,mode=?,entry_kind=?,"
      "approval_required=?,required_edge=?,decision_id=? WHERE id=?",
      {now_s(), p.status, p.order_id, p.note, p.price, p.count, p.p_model, p.sigma, p.fee_per,
       p.net_edge, p.yes_bid, p.yes_ask, p.exec_style, p.mode, p.entry_kind,
       static_cast<long long>(p.approval_required ? 1 : 0), p.required_edge, p.decision_id, p.id});
}

std::optional<Proposal> Store::get_proposal(long long id) {
  json rows = query("SELECT * FROM proposals WHERE id=?", {id});
  if (rows.empty()) return std::nullopt;
  return proposal_from_row(rows[0]);
}

std::vector<Proposal> Store::proposals(const std::string& status, int limit) {
  json rows = status.empty()
                  ? query("SELECT * FROM proposals ORDER BY id DESC LIMIT ?", {static_cast<long long>(limit)})
                  : query("SELECT * FROM proposals WHERE status=? ORDER BY id DESC LIMIT ?",
                          {status, static_cast<long long>(limit)});
  std::vector<Proposal> out;
  for (const auto& r : rows) out.push_back(proposal_from_row(r));
  return out;
}

std::optional<Proposal> Store::find_open_proposal(const std::string& ticker, const std::string& side,
                                                  const std::string& strategy) {
  json rows = query(
      "SELECT * FROM proposals WHERE ticker=? AND outcome_side=? AND strategy=? AND status IN "
      "('pending','submitted','cancel_pending') ORDER BY id DESC LIMIT 1",
      {ticker, side, strategy});
  if (rows.empty()) return std::nullopt;
  return proposal_from_row(rows[0]);
}

int Store::expire_proposals(long long older_than_ts) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  exec("UPDATE proposals SET status='expired',updated_ts=? WHERE status='pending' AND updated_ts<?",
       {now_s(), older_than_ts});
  return sqlite3_changes(db_);
}

// ---- trading records ----------------------------------------------------------------

void Store::upsert_order(const Order& o, long long proposal_id) {
  exec(
      "INSERT INTO orders(order_id,client_order_id,ticker,outcome_side,book_side,type,status,"
      "yes_price,no_price,initial_count,fill_count,remaining_count,taker_fees,maker_fees,"
      "created_time,last_update_time,proposal_id,updated_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(order_id) DO UPDATE SET status=excluded.status,fill_count=excluded.fill_count,"
      "remaining_count=excluded.remaining_count,taker_fees=excluded.taker_fees,"
      "maker_fees=excluded.maker_fees,last_update_time=excluded.last_update_time,"
      "proposal_id=CASE WHEN excluded.proposal_id>0 THEN excluded.proposal_id ELSE orders.proposal_id END,"
      "updated_ts=excluded.updated_ts",
      {o.order_id, o.client_order_id, o.ticker, o.outcome_side, o.book_side, o.type, o.status,
       o.yes_price, o.no_price, o.initial_count, o.fill_count, o.remaining_count, o.taker_fees,
       o.maker_fees, o.created_time, o.last_update_time, proposal_id, now_s()});
  // The decision behind the order, straight on its row: order -> decision without a join.
  if (proposal_id > 0)
    exec("UPDATE orders SET decision_id=(SELECT COALESCE(decision_id,'') FROM proposals WHERE id=?) "
         "WHERE order_id=? AND (decision_id IS NULL OR decision_id='')", {proposal_id, o.order_id});
}

std::vector<Order> Store::orders(const std::string& status) {
  json rows = status.empty() ? query("SELECT * FROM orders ORDER BY created_time DESC LIMIT 500")
                             : query("SELECT * FROM orders WHERE status=? ORDER BY created_time DESC", {status});
  std::vector<Order> out;
  for (const auto& r : rows) {
    Order o;
    o.order_id = str(r, "order_id");
    o.client_order_id = str(r, "client_order_id");
    o.ticker = str(r, "ticker");
    o.outcome_side = str(r, "outcome_side");
    o.book_side = str(r, "book_side");
    o.type = str(r, "type");
    o.status = str(r, "status");
    o.yes_price = num(r, "yes_price");
    o.no_price = num(r, "no_price");
    o.initial_count = num(r, "initial_count");
    o.fill_count = num(r, "fill_count");
    o.remaining_count = num(r, "remaining_count");
    o.taker_fees = num(r, "taker_fees");
    o.maker_fees = num(r, "maker_fees");
    o.created_time = str(r, "created_time");
    o.last_update_time = str(r, "last_update_time");
    out.push_back(o);
  }
  return out;
}

long long Store::proposal_for_order(const std::string& order_id) {
  json rows = query("SELECT proposal_id FROM orders WHERE order_id=?", {order_id});
  if (rows.empty()) return 0;
  return ll(rows[0], "proposal_id");
}

void Store::upsert_fill(const Fill& f) {
  exec(
      "INSERT OR REPLACE INTO fills(fill_id,trade_id,order_id,ticker,outcome_side,book_side,count,"
      "yes_price,no_price,fee,is_taker,created_time,ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {f.fill_id.empty() ? f.trade_id + ":" + f.order_id : f.fill_id, f.trade_id, f.order_id, f.ticker,
       f.outcome_side, f.book_side, f.count, f.yes_price, f.no_price, f.fee,
       static_cast<long long>(f.is_taker ? 1 : 0), f.created_time, f.ts});
}

void Store::replace_positions(const std::vector<Position>& ps) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  exec("DELETE FROM positions");
  for (const auto& p : ps)
    exec(
        "INSERT INTO positions(ticker,position,exposure,realized_pnl,fees_paid,total_traded,updated,"
        "updated_ts) VALUES(?,?,?,?,?,?,?,?)",
        {p.ticker, p.position, p.exposure, p.realized_pnl, p.fees_paid, p.total_traded, p.updated, now_s()});
}

std::vector<Position> Store::positions() {
  std::vector<Position> out;
  for (const auto& r : query("SELECT * FROM positions ORDER BY ticker")) {
    Position p;
    p.ticker = str(r, "ticker");
    p.position = num(r, "position");
    p.exposure = num(r, "exposure");
    p.realized_pnl = num(r, "realized_pnl");
    p.fees_paid = num(r, "fees_paid");
    p.total_traded = num(r, "total_traded");
    p.updated = str(r, "updated");
    out.push_back(p);
  }
  return out;
}

void Store::upsert_settlement(const Settlement& s) {
  exec(
      "INSERT OR REPLACE INTO settlements(ticker,event_ticker,market_result,yes_count,no_count,"
      "yes_cost,no_cost,revenue,fee,settled_time) VALUES(?,?,?,?,?,?,?,?,?,?)",
      {s.ticker, s.event_ticker, s.market_result, s.yes_count, s.no_count, s.yes_cost, s.no_cost,
       s.revenue, s.fee, s.settled_time});
}

void Store::insert_account(long long ts, double balance, double portfolio_value) {
  exec("INSERT OR REPLACE INTO account(ts,balance,portfolio_value) VALUES(?,?,?)",
       {ts, balance, portfolio_value});
}

std::optional<Balance> Store::last_account() {
  json rows = query("SELECT * FROM account ORDER BY ts DESC LIMIT 1");
  if (rows.empty()) return std::nullopt;
  Balance b;
  b.balance = num(rows[0], "balance");
  b.portfolio_value = num(rows[0], "portfolio_value");
  b.updated_ts = ll(rows[0], "ts");
  return b;
}

// ---- hypotheses ----------------------------------------------------------------------

void Store::upsert_hypothesis(const Hypothesis& h) {
  exec(
      "INSERT INTO hypotheses(key,category,title,statement,data_needed,test,status,notes,updated_ts) "
      "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET category=excluded.category,"
      "title=excluded.title,statement=excluded.statement,data_needed=excluded.data_needed,"
      "test=excluded.test,status=CASE WHEN excluded.status<>'' THEN excluded.status ELSE hypotheses.status END,"
      "notes=CASE WHEN excluded.notes<>'' THEN excluded.notes ELSE hypotheses.notes END,"
      "updated_ts=excluded.updated_ts",
      {h.key, h.category, h.title, h.statement, h.data_needed, h.test, h.status, h.notes, now_s()});
}

std::vector<Hypothesis> Store::hypotheses() {
  std::vector<Hypothesis> out;
  for (const auto& r : query("SELECT * FROM hypotheses ORDER BY category, id")) {
    Hypothesis h;
    h.id = ll(r, "id");
    h.key = str(r, "key");
    h.category = str(r, "category");
    h.title = str(r, "title");
    h.statement = str(r, "statement");
    h.data_needed = str(r, "data_needed");
    h.test = str(r, "test");
    h.status = str(r, "status");
    h.notes = str(r, "notes");
    h.updated_ts = ll(r, "updated_ts");
    out.push_back(h);
  }
  return out;
}

// ---- kv ---------------------------------------------------------------------------------

void Store::kv_set(const std::string& k, const std::string& v) {
  exec("INSERT OR REPLACE INTO kv(key,value) VALUES(?,?)", {k, v});
}

std::optional<std::string> Store::kv_get(const std::string& k) {
  json rows = query("SELECT value FROM kv WHERE key=?", {k});
  if (rows.empty()) return std::nullopt;
  return str(rows[0], "value");
}

// ---- events ------------------------------------------------------------------------------

void Store::log_event(const std::string& level, const std::string& msg) {
  exec("INSERT INTO events_log(ts,level,msg) VALUES(?,?,?)", {now_s(), level, msg});
  static int n = 0;
  if (++n % 200 == 0)
    exec("DELETE FROM events_log WHERE id < (SELECT MAX(id) FROM events_log) - 5000");
}

json Store::events(int limit) {
  return query("SELECT id,ts,level,msg FROM events_log ORDER BY id DESC LIMIT ?", {static_cast<long long>(limit)});
}

}  // namespace desk
