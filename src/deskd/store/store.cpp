#include "deskd/store/store.h"

#include <sqlite3.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <nlohmann/json.hpp>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/web/contract.h"

namespace deskd {

namespace {

using json = nlohmann::json;

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS orders(
  cid TEXT PRIMARY KEY, own TEXT, lane TEXT, tk TEXT, side TEXT, tif TEXT, px REAL, qty REAL,
  filled REAL, st TEXT, why TEXT, shard INTEGER,
  click_ms INTEGER, recv_ms INTEGER, gw_ms INTEGER, sent_ms INTEGER, ack_ms INTEGER,
  priv_ms INTEGER, fill_ms INTEGER, cx_ms INTEGER, cx_ack_ms INTEGER, done_ms INTEGER,
  w_ms REAL, fb_ms REAL, rd_ms REAL, json TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS orders_done ON orders(done_ms);
CREATE TABLE IF NOT EXISTS fills(
  fid TEXT PRIMARY KEY, cid TEXT, oid TEXT, tk TEXT, side TEXT, qty REAL, px REAL, fee REAL,
  taker INTEGER, ts_ms INTEGER, recv_ms INTEGER, shard INTEGER, own TEXT, lane TEXT, json TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS fills_ts ON fills(ts_ms);
CREATE TABLE IF NOT EXISTS lip_windows(
  tk TEXT NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER, seconds INTEGER, scored_s INTEGER, quoted_s INTEGER,
  mean_share REAL, est REAL, reward REAL, target REAL, discount_bps INTEGER, json TEXT NOT NULL,
  PRIMARY KEY(tk, start_ms));
CREATE TABLE IF NOT EXISTS markouts(
  fid TEXT NOT NULL, h TEXT NOT NULL, mo REAL, mid REAL, json TEXT NOT NULL, PRIMARY KEY(fid, h));
CREATE TABLE IF NOT EXISTS signals(
  id TEXT PRIMARY KEY, lane TEXT, mode TEXT, shadow INTEGER, ts_ms INTEGER, tk TEXT, ev TEXT, side TEXT,
  px REAL, qty REAL, fair REAL, edge REAL, score TEXT, src TEXT, age_ms INTEGER, first_seen_ms INTEGER,
  touch_since_ms INTEGER, action TEXT, why TEXT, cid TEXT, mo10 REAL, mo60 REAL, mo300 REAL, json TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS signals_lane ON signals(lane, ts_ms);
CREATE TABLE IF NOT EXISTS scores(
  src TEXT NOT NULL, league TEXT NOT NULL, game_id TEXT NOT NULL, state TEXT, period INTEGER, clock TEXT,
  home TEXT, away TEXT, home_score INTEGER, away_score INTEGER, lines TEXT, first_seen_ms INTEGER NOT NULL,
  prev_poll_ms INTEGER, json TEXT NOT NULL, PRIMARY KEY(src, league, game_id, first_seen_ms));
CREATE TABLE IF NOT EXISTS arb_scans(
  ts_ms INTEGER NOT NULL, event TEXT NOT NULL, kind TEXT, mode TEXT, legs TEXT, qty REAL, edge REAL, cost REAL,
  payout_min REAL, fired INTEGER, why TEXT, json TEXT NOT NULL, PRIMARY KEY(event, ts_ms));
CREATE INDEX IF NOT EXISTS arb_scans_ts ON arb_scans(ts_ms);
CREATE TABLE IF NOT EXISTS baskets(
  id TEXT PRIMARY KEY, event TEXT, kind TEXT, state TEXT, legs TEXT, qty REAL, edge_plan REAL, edge_real REAL,
  legging REAL, t0_ms INTEGER, t_end_ms INTEGER, json TEXT NOT NULL);
)SQL";

const char* kOrder =
    "INSERT OR REPLACE INTO orders(cid,own,lane,tk,side,tif,px,qty,filled,st,why,shard,click_ms,recv_ms,gw_ms,"
    "sent_ms,ack_ms,priv_ms,fill_ms,cx_ms,cx_ack_ms,done_ms,w_ms,fb_ms,rd_ms,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
const char* kLip =
    "INSERT OR REPLACE INTO lip_windows(tk,start_ms,end_ms,seconds,scored_s,quoted_s,mean_share,est,reward,target,"
    "discount_bps,json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
const char* kMarkout = "INSERT OR IGNORE INTO markouts(fid,h,mo,mid,json) VALUES(?,?,?,?,?)";
const char* kFill =
    "INSERT OR IGNORE INTO fills(fid,cid,oid,tk,side,qty,px,fee,taker,ts_ms,recv_ms,shard,own,lane,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
// a signal's row is sent again once its markouts are in: the later row replaces it
const char* kSignal =
    "INSERT OR REPLACE INTO signals(id,lane,mode,shadow,ts_ms,tk,ev,side,px,qty,fair,edge,score,src,age_ms,"
    "first_seen_ms,touch_since_ms,action,why,cid,mo10,mo60,mo300,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
// a score state is kept as first seen
const char* kScore =
    "INSERT OR IGNORE INTO scores(src,league,game_id,state,period,clock,home,away,home_score,away_score,lines,"
    "first_seen_ms,prev_poll_ms,json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
const char* kArbScan =
    "INSERT OR REPLACE INTO arb_scans(ts_ms,event,kind,mode,legs,qty,edge,cost,payout_min,fired,why,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
// a basket's row is sent again on each change of state: the later row replaces it
const char* kBasket =
    "INSERT OR REPLACE INTO baskets(id,event,kind,state,legs,qty,edge_plan,edge_real,legging,t0_ms,t_end_ms,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";

// binds j[key] as what it holds: a string, a number, a bool, an array or object as its JSON text
// (the whole row, for each table's json column), or NULL; a decimal string ("0.4200") in a REAL
// column binds as its number
void bind(sqlite3_stmt* st, int i, const json& j, bool real = false) {
  if (j.is_string()) {
    const auto& s = j.get_ref<const std::string&>();
    if (real) {
      char* end = nullptr;
      const double v = std::strtod(s.c_str(), &end);
      if (end && *end == 0 && !s.empty()) { sqlite3_bind_double(st, i, v); return; }
    }
    sqlite3_bind_text(st, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
  } else if (j.is_boolean()) {
    sqlite3_bind_int(st, i, j.get<bool>() ? 1 : 0);
  } else if (j.is_number_integer()) {
    sqlite3_bind_int64(st, i, j.get<std::int64_t>());
  } else if (j.is_number()) {
    sqlite3_bind_double(st, i, j.get<double>());
  } else if (j.is_structured()) {
    const auto text = j.dump();
    sqlite3_bind_text(st, i, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(st, i);
  }
}
const json& at(const json& j, const char* k) {
  static const json null;
  const auto it = j.find(k);
  return it == j.end() ? null : *it;
}

void write_order(sqlite3_stmt* st, const json& j) {
  const auto& t = at(j, "t");
  const auto& w = at(j, "wire");
  int i = 0;
  for (const char* k : {"cid", "own", "lane", "tk", "side", "tif"}) bind(st, ++i, at(j, k));
  bind(st, ++i, at(j, "px"), true);
  bind(st, ++i, at(j, "qty"));
  bind(st, ++i, at(j, "filled"));
  bind(st, ++i, at(j, "st"));
  bind(st, ++i, at(j, "why"));
  bind(st, ++i, at(j, "shard"));
  for (const char* k : {"click", "recv", "gw", "sent", "ack", "priv", "fill", "cx", "cx_ack", "done"}) bind(st, ++i, at(t, k));
  for (const char* k : {"w_ms", "fb_ms", "rd_ms"}) bind(st, ++i, at(w, k));
  bind(st, ++i, j);
}

void write_fill(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"fid", "cid", "oid", "tk", "side"}) bind(st, ++i, at(j, k));
  bind(st, ++i, at(j, "qty"));
  bind(st, ++i, at(j, "px"), true);
  bind(st, ++i, at(j, "fee"), true);
  for (const char* k : {"taker", "ts_ms", "recv_ms", "shard", "own", "lane"}) bind(st, ++i, at(j, k));
  bind(st, ++i, j);
}

void write_markout(sqlite3_stmt* st, const json& j) {
  int i = 0;
  bind(st, ++i, at(j, "fid"));
  bind(st, ++i, at(j, "h"));
  bind(st, ++i, at(j, "mo"), true);
  bind(st, ++i, at(j, "mid"), true);
  bind(st, ++i, j);
}

void write_lip(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"tk", "start_ms", "end_ms", "seconds", "scored_s", "quoted_s", "mean_share"}) bind(st, ++i, at(j, k));
  bind(st, ++i, at(j, "est"), true);
  bind(st, ++i, at(j, "reward"), true);
  bind(st, ++i, at(j, "target"));
  bind(st, ++i, at(j, "discount_bps"));
  bind(st, ++i, j);
}

void write_signal(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"id", "lane", "mode", "shadow", "ts_ms", "tk", "ev", "side"}) bind(st, ++i, at(j, k));
  for (const char* k : {"px", "qty", "fair", "edge"}) bind(st, ++i, at(j, k), true);
  for (const char* k : {"score", "src", "age_ms", "first_seen_ms", "touch_since_ms", "action", "why", "cid"}) bind(st, ++i, at(j, k));
  for (const char* k : {"mo10", "mo60", "mo300"}) bind(st, ++i, at(j, k), true);
  bind(st, ++i, j);
}

void write_score(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"src", "league", "game_id", "state", "period", "clock", "home", "away", "home_score", "away_score",
                        "lines", "first_seen_ms", "prev_poll_ms"})
    bind(st, ++i, at(j, k));
  bind(st, ++i, j);
}

void write_arb_scan(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"ts_ms", "event", "kind", "mode", "legs"}) bind(st, ++i, at(j, k));
  for (const char* k : {"qty", "edge", "cost", "payout_min"}) bind(st, ++i, at(j, k), true);
  for (const char* k : {"fired", "why"}) bind(st, ++i, at(j, k));
  bind(st, ++i, j);
}

void write_basket(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"id", "event", "kind", "state", "legs"}) bind(st, ++i, at(j, k));
  for (const char* k : {"qty", "edge_plan", "edge_real", "legging"}) bind(st, ++i, at(j, k), true);
  for (const char* k : {"t0_ms", "t_end_ms"}) bind(st, ++i, at(j, k));
  bind(st, ++i, j);
}

// the tables a StoreRecord may name: each one's statement, and how a row's json binds to it
struct Table {
  const char* name;
  const char* sql;
  void (*write)(sqlite3_stmt*, const json&);
};
const Table kTables[] = {
    {"orders", kOrder, write_order},
    {"fills", kFill, write_fill},
    {"markouts", kMarkout, write_markout},
    {"lip_windows", kLip, write_lip},
    {"signals", kSignal, write_signal},
    {"scores", kScore, write_score},
    {"arb_scans", kArbScan, write_arb_scan},
    {"baskets", kBasket, write_basket},
};
constexpr std::size_t kNTables = sizeof kTables / sizeof kTables[0];

struct Db {
  sqlite3* db = nullptr;
  sqlite3_stmt* st[kNTables] = {};   // kTables' statements, in its order
  ~Db() {
    for (auto* s : st) sqlite3_finalize(s);
    if (db) sqlite3_close(db);
  }
  std::string err() const { return db ? sqlite3_errmsg(db) : "no database"; }
};

bool open(Db& d, const std::string& path, std::string* why) {
  if (sqlite3_open_v2(path.c_str(), &d.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    *why = "cannot open " + path + ": " + d.err();
    return false;
  }
  sqlite3_busy_timeout(d.db, 2000);
  char* msg = nullptr;
  const std::string setup = std::string("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") + kSchema;
  if (sqlite3_exec(d.db, setup.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    *why = path + ": " + (msg ? msg : d.err());
    sqlite3_free(msg);
    return false;
  }
  for (std::size_t i = 0; i < kNTables; ++i)
    if (sqlite3_prepare_v2(d.db, kTables[i].sql, -1, &d.st[i], nullptr) != SQLITE_OK) {
      *why = path + ": " + d.err();
      return false;
    }
  return true;
}

std::int64_t file_bytes(const std::string& path) {
  struct stat s {};
  return ::stat(path.c_str(), &s) == 0 ? static_cast<std::int64_t>(s.st_size) : 0;
}

std::int64_t file_mtime_ms(const std::string& path) {
  struct stat s {};
  if (::stat(path.c_str(), &s) != 0) return 0;
  return static_cast<std::int64_t>(s.st_mtime) * 1000;   // whole seconds: st_mtim is Linux-only (macOS: st_mtimespec)
}

}  // namespace

std::string StoreWorker::research_path(const std::string& db_path) {
  const auto dot = db_path.rfind(".sqlite");
  return (dot == std::string::npos ? db_path : db_path.substr(0, dot)) + "-research.sqlite";
}

void StoreWorker::measure(std::int64_t now) {
  const std::int64_t bytes = file_bytes(path_) + file_bytes(path_ + "-wal");
  size_bytes_.store(bytes);
  if (bytes > cap_bytes_ && (alerted_ms_ == 0 || now - alerted_ms_ >= 3600000)) {
    alerted_ms_ = now;
    char msg[200];
    std::snprintf(msg, sizeof msg, "ops.sqlite is %.0f MB, past its %.0f MB cap (plan section 7): move old rows out",
                  bytes / 1048576.0, cap_bytes_ / 1048576.0);
    DLOG_WARN("store: %s", msg);
    if (alert_) alert_("CRIT", msg);
  }
}

void StoreWorker::research_copy(std::int64_t now) {
  const std::string dst = research_path(path_), tmp = dst + ".tmp";
  copy_tried_ms_ = now;
  std::remove(tmp.c_str());
  sqlite3* src = nullptr;
  std::string why;
  // its own read-only connection: WAL lets the copy read a consistent snapshot while rows go on
  if (sqlite3_open_v2(path_.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    why = std::string("open: ") + (src ? sqlite3_errmsg(src) : "out of memory");
  } else {
    sqlite3_busy_timeout(src, 2000);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(src, "VACUUM INTO ?", -1, &st, nullptr) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, tmp.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE)
      why = std::string("VACUUM INTO: ") + sqlite3_errmsg(src);
    sqlite3_finalize(st);
  }
  if (src) sqlite3_close(src);
  if (why.empty() && std::rename(tmp.c_str(), dst.c_str()) != 0) why = "cannot rename " + tmp + " to " + dst;
  if (!why.empty()) {
    std::remove(tmp.c_str());
    DLOG_WARN("store: the research copy failed (%s); the last one stays", why.c_str());
    std::atomic_store(&copy_why_, std::make_shared<const std::string>(why));
    return;
  }
  copy_ms_.store(now);
  copies_.fetch_add(1);
  std::atomic_store(&copy_why_, std::make_shared<const std::string>());
  DLOG_INFO("store: research copy written to %s", dst.c_str());
}

void StoreWorker::fail(const std::string& why) {
  const auto last = std::atomic_load(&why_);
  if (!last || *last != why) DLOG_ERR("store: %s", why.c_str());
  std::atomic_store(&why_, std::make_shared<const std::string>(why));
}

void StoreWorker::run(Worker& self) {
  Db d;
  bool ok = false;
  if (!path_.empty()) {
    std::string why;
    ok = open(d, path_, &why);
    if (!ok) fail(why);
  }
  std::vector<StoreRecord> batch;
  auto flush = [&] {
    if (batch.empty()) return;
    if (!ok) {
      if (!path_.empty()) failed_.fetch_add(batch.size());
      batch.clear();
      return;
    }
    std::uint64_t done = 0, bad = 0;
    sqlite3_exec(d.db, "BEGIN", nullptr, nullptr, nullptr);
    for (const auto& r : batch) {
      const auto j = json::parse(r.json, nullptr, false);
      const auto t = std::find_if(std::begin(kTables), std::end(kTables), [&](const Table& x) { return r.table == x.name; });
      if (t == std::end(kTables) || !j.is_object()) { ++bad; continue; }
      sqlite3_stmt* st = d.st[t - kTables];
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
      t->write(st, j);
      if (sqlite3_step(st) == SQLITE_DONE) ++done;
      else { ++bad; fail("writing " + r.table + ": " + d.err()); }
    }
    if (sqlite3_exec(d.db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
      fail("commit: " + d.err());
      sqlite3_exec(d.db, "ROLLBACK", nullptr, nullptr, nullptr);
      bad += done;
      done = 0;
    }
    written_.fetch_add(done);
    failed_.fetch_add(bad);
    batch.clear();
  };
  auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  std::int64_t measured_ms = 0;
  if (ok) copy_ms_.store(file_mtime_ms(research_path(path_)));   // the copy an earlier run wrote
  while (!self.stopping()) {
    if (auto r = inbox_.pop_for(std::chrono::milliseconds(250))) {
      received_.fetch_add(1, std::memory_order_relaxed);
      batch.push_back(std::move(*r));
    }
    if (std::chrono::steady_clock::now() >= next || batch.size() >= 512) {
      flush();
      next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
      const auto now = now_ms();
      if (ok && now - measured_ms >= 60000) {
        measured_ms = now;
        measure(now);
      }
      // the research copy: none since this ET day began (and none failed in the last 10 minutes)
      if (ok && copy_ms_.load() < et_midnight_ms(now) && now - copy_tried_ms_ >= 600000) research_copy(now);
    }
    self.beat();
  }
  while (auto r = inbox_.try_pop()) {   // what gw handed over before the stop is kept
    received_.fetch_add(1, std::memory_order_relaxed);
    batch.push_back(std::move(*r));
  }
  flush();
}

}  // namespace deskd
