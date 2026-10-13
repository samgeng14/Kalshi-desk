#include "deskd/store/store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include <nlohmann/json.hpp>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"

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
CREATE TABLE IF NOT EXISTS markouts(
  fid TEXT NOT NULL, h TEXT NOT NULL, mo REAL, mid REAL, json TEXT NOT NULL, PRIMARY KEY(fid, h));
)SQL";

const char* kOrder =
    "INSERT OR REPLACE INTO orders(cid,own,lane,tk,side,tif,px,qty,filled,st,why,shard,click_ms,recv_ms,gw_ms,"
    "sent_ms,ack_ms,priv_ms,fill_ms,cx_ms,cx_ack_ms,done_ms,w_ms,fb_ms,rd_ms,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
const char* kMarkout = "INSERT OR IGNORE INTO markouts(fid,h,mo,mid,json) VALUES(?,?,?,?,?)";
const char* kFill =
    "INSERT OR IGNORE INTO fills(fid,cid,oid,tk,side,qty,px,fee,taker,ts_ms,recv_ms,shard,own,lane,json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

// binds j[key] as what it holds: a string, a number, a bool, or NULL; a decimal string ("0.4200")
// in a REAL column binds as its number
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
  } else {
    sqlite3_bind_null(st, i);
  }
}
const json& at(const json& j, const char* k) {
  static const json null;
  const auto it = j.find(k);
  return it == j.end() ? null : *it;
}

struct Db {
  sqlite3* db = nullptr;
  sqlite3_stmt *order = nullptr, *fill = nullptr, *markout = nullptr;
  ~Db() {
    sqlite3_finalize(order);
    sqlite3_finalize(fill);
    sqlite3_finalize(markout);
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
  if (sqlite3_prepare_v2(d.db, kOrder, -1, &d.order, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v2(d.db, kFill, -1, &d.fill, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v2(d.db, kMarkout, -1, &d.markout, nullptr) != SQLITE_OK) {
    *why = path + ": " + d.err();
    return false;
  }
  return true;
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
  const auto text = j.dump();
  sqlite3_bind_text(st, ++i, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

void write_fill(sqlite3_stmt* st, const json& j) {
  int i = 0;
  for (const char* k : {"fid", "cid", "oid", "tk", "side"}) bind(st, ++i, at(j, k));
  bind(st, ++i, at(j, "qty"));
  bind(st, ++i, at(j, "px"), true);
  bind(st, ++i, at(j, "fee"), true);
  for (const char* k : {"taker", "ts_ms", "recv_ms", "shard", "own", "lane"}) bind(st, ++i, at(j, k));
  const auto text = j.dump();
  sqlite3_bind_text(st, ++i, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

void write_markout(sqlite3_stmt* st, const json& j) {
  int i = 0;
  bind(st, ++i, at(j, "fid"));
  bind(st, ++i, at(j, "h"));
  bind(st, ++i, at(j, "mo"), true);
  bind(st, ++i, at(j, "mid"), true);
  const auto text = j.dump();
  sqlite3_bind_text(st, ++i, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

}  // namespace

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
      sqlite3_stmt* st = r.table == "orders" ? d.order : r.table == "fills" ? d.fill : r.table == "markouts" ? d.markout : nullptr;
      if (!st || !j.is_object()) { ++bad; continue; }
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
      if (r.table == "orders") write_order(st, j);
      else if (r.table == "fills") write_fill(st, j);
      else write_markout(st, j);
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
  while (!self.stopping()) {
    if (auto r = inbox_.pop_for(std::chrono::milliseconds(250))) {
      received_.fetch_add(1, std::memory_order_relaxed);
      batch.push_back(std::move(*r));
    }
    if (std::chrono::steady_clock::now() >= next || batch.size() >= 512) {
      flush();
      next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
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
