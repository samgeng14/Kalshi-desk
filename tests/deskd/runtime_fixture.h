#pragma once
// The runtime suites' harness (decisions 13): deskd as one process against the fake Kalshi over
// TLS, with its own data dir and account lock under $TMPDIR, the page's stream on a thread of its
// own, and reads of its ops.sqlite. test_runtime_manual.cpp and each lane's runtime suite
// (test_runtime_college.cpp, ...) include it.
#include <httplib.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "check.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/build_paths.h"
#include "net_fixture.h"

namespace rt {
using namespace deskd;
using json = nlohmann::json;

const std::string TK="KXRUNTIME-26OCT10-A";   // shard 0, 1c grid, closes an hour after the test starts
// RFC 8032 test vector, not an account credential (as in test_sender).
const char* const pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";

// The page's stream, on its own thread (as test_web's reader): the envelopes so far.
struct Stream {
  std::thread th;
  std::mutex m;
  std::string buf;
  std::atomic<bool> stop{false};
  Stream(int port, const std::string& sid) {
    th = std::thread([this, port, sid] {
      httplib::Client c("127.0.0.1", port);
      c.set_read_timeout(15, 0);
      c.Get("/api/stream?sid=" + sid, httplib::Headers{},
            [](const httplib::Response&) { return true; },
            [this](const char* data, std::size_t len) {
              std::lock_guard<std::mutex> lk(m);
              buf.append(data, len);
              return !stop.load();
            });
    });
  }
  ~Stream() { stop = true; th.join(); }
  std::vector<json> envelopes() {
    std::string t;
    { std::lock_guard<std::mutex> lk(m); t = buf; }
    std::vector<json> out;
    std::size_t at = 0;
    while (true) {
      const auto end = t.find("\n\n", at);
      if (end == std::string::npos) break;
      const std::string block = t.substr(at, end - at);
      at = end + 2;
      const auto d = block.find("data: ");
      if (d == std::string::npos) continue;
      const auto le = block.find('\n', d);
      auto j = json::parse(block.substr(d + 6, le == std::string::npos ? std::string::npos : le - d - 6), nullptr, false);
      if (!j.is_discarded()) out.push_back(j);
    }
    return out;
  }
};

// one integer from a query on a sqlite file; -1 when it cannot be read
inline long long sql_count(const std::string& path,const char* q) {
  sqlite3* db=nullptr; long long n=-1;
  if(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK) {
    sqlite3_stmt* st=nullptr;
    if(sqlite3_prepare_v2(db,q,-1,&st,nullptr)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW) n=sqlite3_column_int64(st,0);
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return n;
}

struct Run {
  fx::Venue venue;
  TempDir dir;
  std::unique_ptr<Deskd> d;
  DeskdConfig c;
  RuntimeOptions o;
  explicit Run(bool manual_orders,std::function<void(fakex::Engine&,std::int64_t)> more={},bool systematic=false,
               std::function<void(DeskdConfig&,RuntimeOptions&)> tune={}) {
    CHECK(venue.ok);
    venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) {
      auto spec=fx::spec(TK,0); spec.close_ms=(now/1000+3600)*1000;
      e.add_market(spec,now);
      e.street_add(TK,false,4000,500,now);   // YES bid 0.40 x5
      e.street_add(TK,true,5800,300,now);    // NO bid 0.58 x3: YES ask 0.42 x3
      if(more) more(e,now);
    });
    const std::string key=dir.path+"/test-key.pem";
    { std::ofstream f(key); f<<pem; }
    ::chmod(key.c_str(),0600);
    c.data_dir=dir.path; c.ui_dir=std::string(DESKD_SOURCE_DIR)+"/ui/desk"; c.http_port=0;
    c.env="demo"; c.key_id="runtime-test"; c.pem_path=key; c.key_type="ed25519";
    c.manual_orders=manual_orders;
    c.systematic_orders=systematic;
    c.band_series={"KXRUNTIME","KXHIGHNY"};   // M2 trades the test's own market too
    const std::string base="https://127.0.0.1:"+std::to_string(venue.port());
    o.rest_url=base+"/trade-api/v2"; o.ws_url=base+"/trade-api/ws/v2"; o.factory=fx::trust_test_ca();
    o.rest_poll_ms=700; o.rest_gap_ms=0;   // reconciles (and clock samples at varied sub-second phases) come quickly
    o.fair_watch_since_ms=now_ms()-600000;   // a window opening now is one the fair can check
    o.check_each_message=true;               // plan section 8: the ledger's bounds after every gw message
    if(tune) tune(c,o);
    launch();
  }
  void launch() {
    std::string err;
    auto lock=AccountLock::acquire(dir.path,c.live?"prod":"demo","deskd-test-runtime",false,&err);
    d=std::make_unique<Deskd>(c,std::move(lock),o);
    if(!d->start(&err)) { std::fprintf(stderr,"deskd did not start: %s\n",err.c_str()); std::exit(2); }
  }
  // a new deskd process on the same data directory and venue
  void restart() { invariants_held(); d->stop(); d.reset(); launch(); }
  // every run: no gw message left the ledger outside its bounds
  void invariants_held() {
    const auto gw=d->status()["gw"];
    if(gw["invariant_violations"]!=0) std::fprintf(stderr,"invariant: %s\n",gw["invariant_why"].get<std::string>().c_str());
    CHECK(gw["invariant_violations"]==0);
  }
  // the venue's pyth_value relay for one underlying, every 200 ms (LIP-15 quotes only while its
  // underlying's reference is fresh)
  std::atomic<bool> relaying{false};
  std::thread relay;
  void relay_pyth(const std::string& symbol,double value) {
    relaying=true;
    relay=std::thread([this,symbol,value] {
      while(relaying) { venue.f->pyth(symbol,value,now_ms()); std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    });
  }
  ~Run() {
    relaying=false;
    if(relay.joinable()) relay.join();
    invariants_held();
    d->stop();
  }
  httplib::Result post(const std::string& path,const json& body) {
    httplib::Client c("127.0.0.1",d->port()); c.set_read_timeout(6,0);
    return c.Post(path,httplib::Headers{{"X-Desk",d->token()}},body.dump(),"application/json");
  }
  bool until(const std::function<bool()>& f,int ms=8000) {
    const auto end=mono_ms()+ms;
    do { if(f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); } while(mono_ms()<end);
    return f();
  }
  // up: REST reconciled, both Kalshi feeds up, the market pinned, fee-ready and its book live
  bool ready() {
    if(!until([&]{ return d->status()["rest"]["ready"]==true; },10000)) {
      std::fprintf(stderr,"rest: %s\n",d->status()["rest"].dump().c_str()); return false;
    }
    auto w=post("/api/watch",{{"v","K"},{"tk",TK},{"on",true}});
    if(!w || w->status!=200) { std::fprintf(stderr,"watch: %d %s\n",w?w->status:0,w?w->body.c_str():""); return false; }
    CHECK(json::parse(w->body)==json({{"ok",true},{"pin",true}}));
    return until([&]{
      const auto s=d->status();
      return s["rest"]["ready"]==true && s["rest"]["fee_ready_live"]>=1 && s["feeds"]["KX"]["up"]==true &&
             s["feeds"]["KXP"]["up"]==true && s["feeds"]["KXP"]["gap"]==false && s["books_live"]>=1 &&
             s["clock"]["post_trusted"]==true;
    },30000) || (std::fprintf(stderr,"not ready: rest %s feeds %s\n",d->status()["rest"].dump().c_str(),
                              d->status()["feeds"].dump().c_str()),false);
  }
  json order(const std::string& cid,const std::string& side,const std::string& px,double qty,
             const std::string& tif,json ttl=nullptr) {
    json b={{"cid",cid},{"v","K"},{"tk",TK},{"side",side},{"px",px},{"qty",qty},{"tif",tif},
            {"ttl_s",ttl},{"override",false},{"touch",nullptr},{"t_click_ms",now_ms()}};
    auto r=post("/api/order",b);
    if(!r) return json{{"status",0}};
    json j=json::parse(r->body,nullptr,false); j["status"]=r->status; return j;
  }
  std::size_t resting() {
    return venue.f->with_engine([](fakex::Engine& e,std::int64_t){
      std::size_t n=0; for(const fakex::Order* o:e.user_orders()) if(o->status==fakex::OrdStatus::RESTING) ++n; return n; });
  }
};

// every event of a type in the stream so far, with its payload
inline std::vector<json> of(Stream& st,const std::string& type) {
  std::vector<json> out;
  for(const auto& e:st.envelopes()) if(e.value("type","")==type) out.push_back(e);
  return out;
}

}  // namespace rt
