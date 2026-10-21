// The store thread (store/store.h): rows reach ops.sqlite, the research copy is written once per
// ET day beside it (a restart the same day writes none) and holds the rows, and a database past
// its size cap alerts once.
#include <sqlite3.h>

#include <chrono>
#include <mutex>
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
  copy_and_cap();
  return finish("deskd-store");
}
