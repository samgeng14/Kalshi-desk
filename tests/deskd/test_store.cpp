// The store thread (store/store.h): rows reach ops.sqlite, the research copy is written once per
// ET day beside it (a restart the same day writes none) and holds the rows, and a database past
// its size cap alerts once. The lanes' tables (signals, scores, arb_scans, baskets) read back
// what was written, a resent row replacing or keeping the first as its table says.
#include <sqlite3.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/app/worker.h"
#include "deskd/store/store.h"

using namespace deskd;

namespace {

bool until(const std::function<bool()>& f, int ms = 5000) {
  for (int i = 0; i < ms / 20; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return f();
}

long long count(const std::string& path, const char* table) {
  sqlite3* db = nullptr;
  long long n = -1;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
      n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return n;
}

using Rows = std::vector<std::vector<std::string>>;

// each row of `sql` as its columns' text ("NULL" for none)
Rows rows(const std::string& path, const std::string& sql) {
  Rows out;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
      while (sqlite3_step(st) == SQLITE_ROW) {
        std::vector<std::string> r;
        for (int i = 0; i < sqlite3_column_count(st); ++i) {
          const auto* t = sqlite3_column_text(st, i);
          r.push_back(t ? reinterpret_cast<const char*>(t) : "NULL");
        }
        out.push_back(std::move(r));
      }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return out;
}

void lane_tables() {
  TempDir d;
  const std::string path = d.path + "/ops.sqlite";
  StoreWorker s;
  s.set_path(path);
  Worker w("store");
  w.start([&](Worker& x) { s.run(x); });
  // a shadow N1 signal, then the same signal again once its markouts are in; and a taken C1 one
  s.inbox().push(StoreRecord{"signals", R"({"id":"N1-KXNBAGAME-26OCT21LALBOS-1","lane":"N1","mode":"MODEL","shadow":true,)"
                                        R"("ts_ms":1000,"tk":"KXNBAGAME-26OCT21LALBOS-LAL","ev":"KXNBAGAME-26OCT21LALBOS",)"
                                        R"("side":"BUY","px":"0.4200","qty":5,"fair":"0.5300","edge":"0.0900",)"
                                        R"("score":"54-50 Q3","src":"ESPN","age_ms":1200,"first_seen_ms":900,)"
                                        R"("action":"SHADOW","why":"lane off","mo10":null,"mo60":null,"mo300":null})"});
  s.inbox().push(StoreRecord{"signals", R"({"id":"N1-KXNBAGAME-26OCT21LALBOS-1","lane":"N1","mode":"MODEL","shadow":true,)"
                                        R"("ts_ms":1000,"tk":"KXNBAGAME-26OCT21LALBOS-LAL","ev":"KXNBAGAME-26OCT21LALBOS",)"
                                        R"("side":"BUY","px":"0.4200","qty":5,"fair":"0.5300","edge":"0.0900",)"
                                        R"("score":"54-50 Q3","src":"ESPN","age_ms":1200,"first_seen_ms":900,)"
                                        R"("action":"SHADOW","why":"lane off","mo10":"0.0100","mo60":"-0.0250","mo300":null})"});
  s.inbox().push(StoreRecord{"signals", R"({"id":"C1-KXNCAAWBGAME-26NOV04UCLAUSC-1","lane":"C1","mode":"COLLEGE","shadow":false,)"
                                        R"("ts_ms":2000,"tk":"KXNCAAWBGAME-26NOV04UCLAUSC-UCLA","ev":"KXNCAAWBGAME-26NOV04UCLAUSC",)"
                                        R"("side":"SELL","px":"0.6100","qty":3.5,"fair":"0.3900","edge":"0.2200","score":"40-31 H2",)"
                                        R"("src":"NCAA","age_ms":800,"first_seen_ms":1500,"touch_since_ms":1100,"action":"TAKE",)"
                                        R"("why":"","cid":"SC1-7"})"});
  // a score state, the same state sent again (kept as first seen), then the next state
  s.inbox().push(StoreRecord{"scores", R"({"src":"ESPN","league":"womens-college-basketball","game_id":"401","state":"in",)"
                                       R"("period":2,"clock":"4:12","home":"UCLA","away":"USC","home_score":40,"away_score":31,)"
                                       R"("lines":[[22,18],[17,13]],"first_seen_ms":1500,"prev_poll_ms":1000})"});
  s.inbox().push(StoreRecord{"scores", R"({"src":"ESPN","league":"womens-college-basketball","game_id":"401","state":"in",)"
                                       R"("period":2,"clock":"3:58","home":"UCLA","away":"USC","home_score":40,"away_score":31,)"
                                       R"("lines":[[22,18],[17,13]],"first_seen_ms":1500,"prev_poll_ms":4500})"});
  s.inbox().push(StoreRecord{"scores", R"({"src":"ESPN","league":"womens-college-basketball","game_id":"401","state":"in",)"
                                       R"("period":2,"clock":"3:40","home":"UCLA","away":"USC","home_score":42,"away_score":31,)"
                                       R"("lines":[[22,18],[19,13]],"first_seen_ms":4500,"prev_poll_ms":1500})"});
  // the same event scanned twice; a basket as it starts, then as it completes
  s.inbox().push(StoreRecord{"arb_scans", R"({"ts_ms":60000,"event":"KXHIGHNY-26OCT21","kind":"YES_COVER","mode":"scan",)"
                                          R"("legs":[{"tk":"KXHIGHNY-26OCT21-B60","side":"BUY","px":"0.3000"},)"
                                          R"({"tk":"KXHIGHNY-26OCT21-B62","side":"BUY","px":"0.6400"}],"qty":10,)"
                                          R"("edge":"0.0400","cost":"9.4000","payout_min":"10.0000","fired":false,"why":"scan mode"})"});
  s.inbox().push(StoreRecord{"arb_scans", R"({"ts_ms":120000,"event":"KXHIGHNY-26OCT21","kind":"","mode":"scan",)"
                                          R"("legs":[],"qty":0,"edge":null,"cost":null,"payout_min":null,"fired":false,"why":"no lock"})"});
  s.inbox().push(StoreRecord{"baskets", R"({"id":"S1-1","event":"KXHIGHNY-26OCT21","kind":"NO_SET","state":"REST",)"
                                        R"("legs":[{"tk":"KXHIGHNY-26OCT21-B60","side":"SELL","px":"0.0300","filled":4}],)"
                                        R"("qty":4,"edge_plan":"0.1200","edge_real":null,"legging":"0.0000","t0_ms":70000})"});
  s.inbox().push(StoreRecord{"baskets", R"({"id":"S1-1","event":"KXHIGHNY-26OCT21","kind":"NO_SET","state":"COMPLETE",)"
                                        R"("legs":[{"tk":"KXHIGHNY-26OCT21-B60","side":"SELL","px":"0.0300","filled":4}],)"
                                        R"("qty":4,"edge_plan":"0.1200","edge_real":"0.1100","legging":"-0.0100",)"
                                        R"("t0_ms":70000,"t_end_ms":70400})"});
  // C1's own table is gone: its rows are signals now
  s.inbox().push(StoreRecord{"c1_signals", R"({"ts_ms":1})"});
  CHECK(until([&] { return s.written() == 10 && s.failed() == 1; }));
  w.request_stop();
  w.join();
  CHECK(s.why().empty());

  CHECK((rows(path, "SELECT id, lane, mode, shadow, ts_ms, tk, ev, side, px, qty, fair, edge, score, src, age_ms, first_seen_ms, "
                    "touch_since_ms, action, why, cid, mo10, mo60, mo300 FROM signals ORDER BY ts_ms") ==
         Rows{{"N1-KXNBAGAME-26OCT21LALBOS-1", "N1", "MODEL", "1", "1000", "KXNBAGAME-26OCT21LALBOS-LAL",
               "KXNBAGAME-26OCT21LALBOS", "BUY", "0.42", "5.0", "0.53", "0.09", "54-50 Q3", "ESPN", "1200", "900", "NULL",
               "SHADOW", "lane off", "NULL", "0.01", "-0.025", "NULL"},
              {"C1-KXNCAAWBGAME-26NOV04UCLAUSC-1", "C1", "COLLEGE", "0", "2000", "KXNCAAWBGAME-26NOV04UCLAUSC-UCLA",
               "KXNCAAWBGAME-26NOV04UCLAUSC", "SELL", "0.61", "3.5", "0.39", "0.22", "40-31 H2", "NCAA", "800", "1500", "1100",
               "TAKE", "", "SC1-7", "NULL", "NULL", "NULL"}}));
  CHECK((rows(path, "SELECT typeof(px), typeof(shadow), typeof(mo10), json FROM signals WHERE lane = 'N1'") ==
         Rows{{"real", "integer", "real",
               R"({"action":"SHADOW","age_ms":1200,"edge":"0.0900","ev":"KXNBAGAME-26OCT21LALBOS","fair":"0.5300",)"
               R"("first_seen_ms":900,"id":"N1-KXNBAGAME-26OCT21LALBOS-1","lane":"N1","mo10":"0.0100","mo300":null,)"
               R"("mo60":"-0.0250","mode":"MODEL","px":"0.4200","qty":5,"score":"54-50 Q3","shadow":true,"side":"BUY",)"
               R"("src":"ESPN","tk":"KXNBAGAME-26OCT21LALBOS-LAL","ts_ms":1000,"why":"lane off"})"}}));

  CHECK((rows(path, "SELECT src, league, game_id, state, period, clock, home, away, home_score, away_score, lines, first_seen_ms, "
                    "prev_poll_ms FROM scores ORDER BY first_seen_ms") ==
         Rows{{"ESPN", "womens-college-basketball", "401", "in", "2", "4:12", "UCLA", "USC", "40", "31", "[[22,18],[17,13]]",
               "1500", "1000"},
              {"ESPN", "womens-college-basketball", "401", "in", "2", "3:40", "UCLA", "USC", "42", "31", "[[22,18],[19,13]]",
               "4500", "1500"}}));

  CHECK((rows(path, "SELECT ts_ms, event, kind, mode, legs, qty, edge, cost, payout_min, fired, why FROM arb_scans ORDER BY ts_ms") ==
         Rows{{"60000", "KXHIGHNY-26OCT21", "YES_COVER", "scan",
               R"([{"px":"0.3000","side":"BUY","tk":"KXHIGHNY-26OCT21-B60"},{"px":"0.6400","side":"BUY","tk":"KXHIGHNY-26OCT21-B62"}])",
               "10.0", "0.04", "9.4", "10.0", "0", "scan mode"},
              {"120000", "KXHIGHNY-26OCT21", "", "scan", "[]", "0.0", "NULL", "NULL", "NULL", "0", "no lock"}}));

  CHECK((rows(path, "SELECT id, event, kind, state, legs, qty, edge_plan, edge_real, legging, t0_ms, t_end_ms FROM baskets") ==
         Rows{{"S1-1", "KXHIGHNY-26OCT21", "NO_SET", "COMPLETE",
               R"([{"filled":4,"px":"0.0300","side":"SELL","tk":"KXHIGHNY-26OCT21-B60"}])", "4.0", "0.12", "0.11", "-0.01",
               "70000", "70400"}}));
}

void copy_and_cap() {
  TempDir d;
  const std::string path = d.path + "/ops.sqlite";
  CHECK(StoreWorker::research_path(path) == d.path + "/ops-research.sqlite");
  std::mutex mu;   // alerts come from the store thread
  std::vector<std::pair<std::string, std::string>> alerts;
  {
    StoreWorker s;
    s.set_path(path);
    s.set_cap_bytes(1);   // any database is past it
    s.set_alert([&](const std::string& lvl, const std::string& msg) {
      std::lock_guard<std::mutex> lk(mu);
      alerts.emplace_back(lvl, msg);
    });
    Worker w("store");
    s.inbox().push(StoreRecord{"fills", R"({"fid":"t-1","cid":"M-1","tk":"KXT-1","side":"BUY","qty":1,"px":"0.4200","ts_ms":1})"});
    w.start([&](Worker& x) { s.run(x); });
    CHECK(until([&] { return s.written() == 1; }));
    // no copy yet today: the first round writes one, with the row in it
    CHECK(until([&] { return s.copies() == 1; }));
    CHECK(s.copy_why().empty() && s.copy_ms() > 0 && s.copy_path() == StoreWorker::research_path(path));
    CHECK(count(s.copy_path(), "fills") == 1);
    // measured, and past the cap: one CRIT, not one a round
    CHECK(until([&] { return s.size_bytes() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    {
      std::lock_guard<std::mutex> lk(mu);
      CHECK(alerts.size() == 1 && alerts[0].first == "CRIT" && alerts[0].second.find("past its") != std::string::npos);
    }
    w.request_stop();
    w.join();
  }
  {   // a restart the same ET day: the copy an earlier run wrote counts
    StoreWorker s;
    s.set_path(path);
    Worker w("store");
    w.start([&](Worker& x) { s.run(x); });
    s.inbox().push(StoreRecord{"fills", R"({"fid":"t-2","cid":"M-2","tk":"KXT-1","side":"BUY","qty":1,"px":"0.4300","ts_ms":2})"});
    CHECK(until([&] { return s.written() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(s.copies() == 0 && s.copy_ms() > 0);
    CHECK(count(path, "fills") == 2 && count(s.copy_path(), "fills") == 1);
    w.request_stop();
    w.join();
  }
  {   // no database path: nothing measured, nothing copied
    StoreWorker s;
    Worker w("store");
    w.start([&](Worker& x) { s.run(x); });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(s.copies() == 0 && s.size_bytes() == 0 && s.copy_path().empty());
    w.request_stop();
    w.join();
  }
}

}  // namespace

int main() {
  lane_tables();
  copy_and_cap();
  return finish("deskd-store");
}
