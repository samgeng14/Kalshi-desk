// deskd as one process against the fake Kalshi over TLS: REST state, market data, the private
// feed, and Manual orders from the page's HTTP routes through gw, send-k1 and send-cx. With
// deskd.manual_orders off every order is refused RO; on, a click fills, rests and cancels, and
// KILL's cancel-all clears the venue.
#include <httplib.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>
#include <nlohmann/json.hpp>
#include "check.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/build_paths.h"
#include "deskd/gw/manual_book.h"
#include "net_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
const std::string TK="KXRUNTIME-26OCT10-A";   // shard 0, 1c grid, closes an hour after the test starts
// RFC 8032 test vector, not an account credential (as in test_sender).
const char* pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";

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
long long sql_count(const std::string& path,const char* q) {
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
  void restart() { d->stop(); d.reset(); launch(); }
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

void off_by_default() {
  Run r(false);
  CHECK(r.ready());
  auto o=r.order("Mrt-off-1","BUY","0.4200",1,"IOC");
  CHECK(o["status"]==409 && o["err"]["code"]=="RO" && !o.contains("ord"));
  CHECK(r.venue.orders()==0);   // nothing reached the venue
  CHECK(r.d->status()["gw"]["manual_orders"]==false);
}

// every event of a type in the stream so far, with its payload
std::vector<json> of(Stream& st,const std::string& type) {
  std::vector<json> out;
  for(const auto& e:st.envelopes()) if(e.value("type","")==type) out.push_back(e);
  return out;
}

void orders_cancel_kill() {
  Run r(true);
  CHECK(r.ready());
  CHECK(r.d->status()["gw"]["manual_orders"]==true);
  Stream st(r.d->port(),"rt-stream-1");
  CHECK(r.until([&]{ return !st.envelopes().empty(); }));
  auto sub=r.post("/api/subscribe",{{"sid","rt-stream-1"},{"ladders",{{{"v","K"},{"tk",TK}}}},{"tops",json::array()}});
  CHECK(sub && sub->status==200);
  // Take: IOC buy 2 at the 0.42 ask
  auto o=r.order("Mrt-take-1","BUY","0.4200",2,"IOC");
  if(o["status"]!=200) std::fprintf(stderr,"take: %s\n",o.dump().c_str());
  CHECK(o["status"]==200 && o["ord"]["st"]=="FILL" && o["ord"]["filled"]==2);
  // The screen fills in from real data: the print on the ladder, the fill, the marked position.
  auto has_print=[&]{
    for(const auto& e:of(st,"book")) {
      const auto& d=e["data"];
      if(d["tk"]==TK && d["last"].is_object() && d["last"]["px"]=="0.4200" && d["last"]["side"]=="BUY" && d["vol"].get<double>()>=2) return true;
    }
    return false;
  };
  CHECK(r.until(has_print));
  auto has_fill=[&]{
    for(const auto& e:of(st,"fill")) if(e["data"]["cid"]=="Mrt-take-1" && e["data"]["qty"]==2 && e["data"]["liq"]=="T") return true;
    return false;
  };
  CHECK(r.until(has_fill));
  auto has_pos=[&]{
    for(const auto& e:of(st,"pos")) {
      const auto& rows=e["data"].is_array() ? e["data"] : json::array({e["data"]});
      for(const auto& p:rows) if(p.value("tk","")==TK && p["pos"]==2 && p["mark"].is_string()) return true;
    }
    return false;
  };
  CHECK(r.until(has_pos));
  // Post: rest 1 at 0.41 for 60 s, then cancel it from the page's route
  o=r.order("Mrt-post-1","BUY","0.4100",1,"POST",60);
  if(o["status"]!=200) std::fprintf(stderr,"post: %s\n",o.dump().c_str());
  CHECK(o["status"]==200 && o["ord"]["st"]=="WORK");
  CHECK(r.resting()==1);
  {   // the order path's latency: two acknowledged creates, click to ack measured
    const auto lat=r.d->status()["latency"];
    CHECK(lat["n"]==2 && lat["click_ack_p50"].is_number() && lat["recv_ack_p90"].is_number() && lat["wire_p50"].is_number());
    if(lat["click_ack_p90"].is_number()) std::printf("click to ack p90 %d ms on the fake\n",lat["click_ack_p90"].get<int>());
  }
  auto cx=r.post("/api/cancel",{{"cids",{"Mrt-post-1"}}});
  CHECK(cx && cx->status==200 && json::parse(cx->body)["n"]==1);
  CHECK(r.until([&]{ return r.resting()==0; }));
  {   // each ended order's timeline and each fill, in data/ops.sqlite (store), in order
    const std::string db=r.dir.path+"/ops.sqlite";
    const char* take="SELECT count(*) FROM orders WHERE cid='Mrt-take-1' AND st='FILL' AND own='MAN' AND tk IS NOT NULL"
                     " AND click_ms<=recv_ms AND recv_ms<=gw_ms AND gw_ms<=sent_ms AND sent_ms<=ack_ms AND fill_ms>0"
                     " AND priv_ms>0 AND done_ms>0 AND w_ms>=0 AND fb_ms>=0 AND rd_ms>=0 AND px=0.42 AND filled=2";
    const char* post="SELECT count(*) FROM orders WHERE cid='Mrt-post-1' AND st='CXL' AND cx_ms>=ack_ms AND cx_ack_ms>=cx_ms"
                     " AND fill_ms IS NULL AND done_ms>0";
    const char* fill="SELECT count(*) FROM fills WHERE cid='Mrt-take-1' AND qty=2 AND px=0.42 AND taker=1 AND own='MAN'";
    if(!r.until([&]{ return sql_count(db,take)==1 && sql_count(db,post)==1 && sql_count(db,fill)==1; })) {
      CHECK(!"the timelines and the fill are in ops.sqlite");
      std::system(("python3 -c \"import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); [print('DBG',r[:26]) for r in c.execute('select * from orders')]; [print('DBG',r[:14]) for r in c.execute('select * from fills')]\" '"+db+"' >&2").c_str());
    }
    // and the take's 1 s markout against the mid (strat writes markouts too)
    CHECK(r.until([&]{
      return sql_count(db,"SELECT count(*) FROM markouts m JOIN fills f ON m.fid=f.fid WHERE f.cid='Mrt-take-1'"
                          " AND m.h='1s' AND m.mid>0 AND m.mo IS NOT NULL")==1;
    }));
    const auto store=r.d->status()["store"];
    CHECK(store["failed"]==0 && store["why"]=="" && store["written"].get<int>()>=4);
    CHECK(r.d->status()["gw"]["records_lost"]==0);
    if(!std::string(DESKD_PYTHON).empty()) {   // the latency report reads it
      const std::string out=r.dir.path+"/latency.txt";
      const std::string cmd=std::string("\"")+DESKD_PYTHON+"\" \""+DESKD_SOURCE_DIR+"/tools/latency_report.py\" --db \""+db+"\" > \""+out+"\"";
      CHECK(std::system(cmd.c_str())==0);
      std::ifstream f(out); const std::string text((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
      CHECK(text.find("sent > ack")!=std::string::npos && text.find("2 orders")!=std::string::npos);
    }
  }
  // KILL: a resting order is cancelled by send-cx's cancel-all, and new orders are refused
  o=r.order("Mrt-post-2","BUY","0.4000",1,"POST",60);
  if(o["status"]!=200) std::fprintf(stderr,"post-2: %s resting %zu\n",o.dump().c_str(),r.resting());
  CHECK(o["status"]==200 && o["ord"]["st"]=="WORK" && r.resting()==1);
  auto k=r.post("/api/kill",json::object());
  CHECK(k && k->status==200);
  CHECK(r.until([&]{ return r.resting()==0; }));
  CHECK(r.d->cx_stats().kills.load()>=1);
  o=r.order("Mrt-take-2","BUY","0.4200",1,"IOC");
  CHECK(o["status"]==409 && o["err"]["code"]=="KILLED");
  // Everything the page received, and its snapshot, checked by contract.js itself.
  if(std::string(DESKD_NODE).empty()) { std::printf("node not found: contract.js check SKIPPED\n"); return; }
  httplib::Client c("127.0.0.1",r.d->port());
  auto snap=c.Get("/api/snapshot?sid=rt-stream-1");
  CHECK(snap && snap->status==200);
  json cap={{"snapshots",json::array()},{"events",st.envelopes()},{"responses",json::array()}};
  if(snap && snap->status==200) {
    const auto sj=json::parse(snap->body);
    cap["snapshots"].push_back(sj);
    bool fills=false, books=false;
    for(const auto& f:sj["fills"]) if(f["cid"]=="Mrt-take-1") fills=true;
    for(const auto& b:sj["books"]) if(b["tk"]==TK && b["last"].is_object()) books=true;
    CHECK(fills && books);
  }
  const std::string file=r.dir.path+"/capture.json";
  std::ofstream(file)<<cap.dump();
  const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
  CHECK(std::system(cmd.c_str())==0);
}
// A create the venue processed but answered 400: send-k1 stops and trips KILL, and the private
// feed's sight of the order contradicts the refusal, so the ledger stops too. Every RO refusal
// and /ops/status name the owner and its reason; after the confirmed cancel-all and a re-arm,
// send-k1 is back and only the ledger (restart deskd) still holds orders.
void stop_reasons() {
  Run r(true);
  CHECK(r.ready());
  fakex::HttpFault hard; hard.kind=fakex::HttpFault::Kind::STATUS_AFTER_ACCEPT; hard.status=400;
  r.venue.f->add_fault(hard);
  auto o=r.order("Mrt-hard-1","BUY","0.4100",1,"POST",60);
  // The 400 and the private feed's sight of the order race: whichever lands second contradicts
  // the first (BAD_REQUEST when the 400 wins, INTERNAL when the order was seen first).
  CHECK(o["err"]["code"]=="BAD_REQUEST" || o["err"]["code"]=="INTERNAL");
  CHECK(r.until([&]{ return r.d->kill_switch().active(); }));
  CHECK(r.until([&]{ return r.resting()==0; }));   // KILL's cancel-all took the order the venue kept
  CHECK(r.until([&]{ return !r.d->status()["gw"]["ledger_why"].get<std::string>().empty(); }));
  auto s=r.d->status();
  CHECK(s["senders"]["k1"]["healthy"]==false);
  CHECK(s["senders"]["k1"]["why"].get<std::string>().rfind("venue refused a create (Mrt-hard-1) as hard_invalid",0)==0);
  CHECK(s["run"]["state"]=="KILLED" && s["run"]["why"].get<std::string>().rfind("venue refused a create (Mrt-hard-1)",0)==0);
  CHECK(s["gw"]["ledger_healthy"]==false);
  CHECK(s["gw"]["ledger_why"].get<std::string>().rfind("Mrt-hard-1: ",0)==0);
  CHECK(r.until([&]{ return r.d->kill_switch().confirmed_gen()==r.d->kill_switch().generation(); }));
  CHECK(r.d->kill_switch().rearm(now_ms(),mono_ms()+61000).empty());
  s=r.d->status();
  CHECK(s["senders"]["k1"]["healthy"]==true && s["senders"]["cx"]["healthy"]==true);
  o=r.order("Mrt-after-1","BUY","0.4200",1,"IOC");
  if(o["status"]!=409) std::fprintf(stderr,"after: %s\n",o.dump().c_str());
  CHECK(o["status"]==409 && o["err"]["code"]=="RO");
  const auto msg=o["err"]["msg"].get<std::string>();
  if(msg.rfind("order ledger stopped: Mrt-hard-1: ",0)!=0) std::fprintf(stderr,"RO msg: %s\n",msg.c_str());
  CHECK(msg.rfind("order ledger stopped: Mrt-hard-1: ",0)==0 && msg.find("restart deskd")!=std::string::npos);
  CHECK(!r.venue.has_cid("Mrt-after-1"));
}
// The 15-minute financial fair end to end: an hour of pyth_value history, a natural-gas window
// whose strike is the candle close the feed saw at its open, and the ladder carries the model's
// fair; a window whose strike the feed did not reproduce gets none.
void financial_fair() {
  const std::string NG="Commodities.Index.NATGAS/USD";
  const std::string W="KXNATGAS15M-26OCT101200-00", BAD="KXNATGAS15M-26OCT101215-15";
  const auto open=now_ms()+6000;   // after deskd is up and its feed has the history
  double s=3.0; std::uint64_t x=1234567;
  std::vector<std::pair<std::int64_t,double>> ticks;
  for(int i=3600;i>=1;--i) {   // a walk, one value a second, ending a second before the open
    x=x*6364136223846793005ULL+1442695040888963407ULL;
    s*=1+((static_cast<double>(x>>11)/9007199254740992.0)-0.5)*0.0006;
    ticks.emplace_back(open-i*1000LL,std::round(s*1e5)/1e5);
  }
  const double strike=ticks.back().second;
  Run r(false,[&](fakex::Engine& e,std::int64_t now) {
    auto spec=fx::spec(W,0); spec.close_ms=open+900000; spec.open_ms=open;
    spec.strike_type="greater_or_equal"; spec.floor_strike=strike;
    e.add_market(spec,now);
    auto bad=spec; bad.ticker=BAD; bad.floor_strike=strike+0.001; e.add_market(bad,now);
    e.street_add(W,false,4000,500,now); e.street_add(W,true,5500,500,now);
  });
  CHECK(r.until([&]{ return r.d->status()["pyth"]["up"]==true; },10000));
  for(const auto& [t,v]:ticks) r.venue.f->pyth(NG,v,t);
  CHECK(r.until([&]{ return r.d->status()["rest"]["ready"]==true; },15000));
  CHECK(r.until([&]{ return now_ms()>open+500; },10000));
  Stream st(r.d->port(),"rt-fair-1");
  CHECK(r.until([&]{ return !st.envelopes().empty(); }));
  auto w=r.post("/api/watch",{{"v","K"},{"tk",W},{"on",true}});
  if(!w || w->status!=200) std::fprintf(stderr,"watch: %s\n",w?w->body.c_str():"no reply");
  CHECK(w && w->status==200);
  auto sub=r.post("/api/subscribe",{{"sid","rt-fair-1"},{"ladders",{{{"v","K"},{"tk",W}}}},{"tops",json::array()}});
  CHECK(sub && sub->status==200);
  std::atomic<bool> stop{false};
  std::thread live([&]{ while(!stop) { r.venue.f->pyth(NG,strike,now_ms()); std::this_thread::sleep_for(std::chrono::milliseconds(300)); } });
  auto fair_on_ladder=[&]{
    for(const auto& e:of(st,"book")) if(e["data"]["tk"]==W && e["data"]["ref"]["fair"].is_string()) return true;
    return false;
  };
  const bool shown=r.until(fair_on_ladder,20000);
  stop=true; live.join();
  const auto fs=r.d->status()["fair"];
  if(!shown) std::fprintf(stderr,"fair: %s\npyth: %s\n",fs.dump().c_str(),r.d->status()["pyth"].dump().c_str());
  CHECK(shown);
  CHECK(fs["replication"]["natgas"]["checked"]==2 && fs["replication"]["natgas"]["matched"]==1);
  CHECK(fs["markets"][W]["fair"].is_string() && fs["markets"][BAD]["fair"].is_null());
  CHECK(r.d->status()["pyth"]["underlyings"][NG]["messages"].get<int>()>=3600);
}
// Systematic on the fake: an order group per lane and shard, a lane switched on, Start, an S order
// in its lane's group, gw's refusals (lane off, wrong owner, inventory, not running), Stop
// cancelling it and triggering the groups (reset again once stopped), and KILL's sender
// triggering them (reset at the re-arm).
void systematic() {
  Run r(false,{},true);
  CHECK(r.ready());
  CHECK(r.d->status()["gw"]["systematic_orders"]==true && r.d->status()["senders"]["k2"]["healthy"]==true);
  Stream st(r.d->port(),"rt-sys-1");
  CHECK(r.until([&]{ return !st.envelopes().empty(); }));
  json responses=json::array();
  auto keep=[&](const char* key,const httplib::Result& res) {
    if(res) responses.push_back({{"key",key},{"status",res->status},{"json",json::parse(res->body,nullptr,false)}});
  };
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  keep("POST /api/lane/{id}",lane);
  CHECK(lane && lane->status==200);
  if(lane && lane->status==200) {
    const auto row=json::parse(lane->body)["lane"];
    CHECK(row["id"]=="M2" && row["on"]==true && row["state"]=="OFF" && row["why"]=="STOP" && row["budget"]=="25.0000");
  }
  auto none=r.post("/api/lane/Z9",{{"on",true}});
  CHECK(none && none->status==404);
  keep("POST /api/lane/{id}",none);
  // a lane no engine trades yet does not switch on (its groups would only hold up GROUPS)
  auto idle=r.post("/api/lane/S1",{{"on",true}});
  CHECK(idle && idle->status==400 && json::parse(idle->body)["err"]["code"]=="BAD_REQUEST");
  keep("POST /api/lane/{id}",idle);
  std::string why;
  const bool ready=r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000);   // M2's groups active
  if(!ready) std::fprintf(stderr,"groups: %s\n",why.c_str());
  CHECK(ready);
  // Start, as an operator would: again while a preflight row (the fake's 1 s Date: CLK) flickers
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  if(start && start->status!=200) std::fprintf(stderr,"start: %s\n",start->body.c_str());
  CHECK(start && start->status==200 && json::parse(start->body)["run"]["state"]=="RUN");
  keep("POST /api/start",start);
  auto sys=[&](const std::string& cid,Px px,Qty count,const std::string& ln="M2") {
    PlaceIntent p; p.cid=cid; p.owner=Owner::SYS; p.lane=ln; p.ticker=TK; p.side=Side::BUY; p.price=px; p.count=count;
    p.tif=Tif::POST; p.ttl_s=30; p.reply=std::make_shared<std::promise<OrderReply>>();
    auto f=p.reply->get_future();
    CHECK(r.d->gw().post(std::move(p)));
    if(f.wait_for(std::chrono::seconds(5))!=std::future_status::ready) return OrderReply{"TIMEOUT","",""};
    return f.get();
  };
  auto a=sys("SM2-rt-1",3900,200);
  if(!a.code.empty()) std::fprintf(stderr,"sys order: %s %s\n",a.code.c_str(),a.msg.c_str());
  CHECK(a.code.empty());
  if(a.code.empty()) {
    const auto ord=json::parse(a.ord);
    CHECK(ord["st"]=="WORK" && ord["own"]=="SYS" && ord["lane"]=="M2");
  }
  const bool grouped=r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
    for(const fakex::Order* o:e.user_orders())
      if(o->client_order_id=="SM2-rt-1") return !o->order_group_id.empty() && e.group(o->order_group_id)!=nullptr;
    return false;
  });
  CHECK(grouped);
  CHECK(sys("SM1-rt-1",3900,100,"M1").code=="BAD_STATE");     // M1 is off
  CHECK(sys("Mrt-sys-1",3900,100).code=="BAD_REQUEST");        // not a Systematic cid
  CHECK(sys("SM2-rt-2",3900,1900).code=="SYS_CEILING");        // 2 working + 19 > 20 contracts
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200 && json::parse(stop->body)["n"]==1);
  keep("POST /api/stop",stop);
  CHECK(r.until([&]{ return r.resting()==0; }));
  CHECK(sys("SM2-rt-3",3900,100).code=="BAD_STATE");           // not running
  auto triggered=[&]{
    return r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      int n=0; for(const auto& [id,g]:e.groups()) if(g.triggered) ++n; return n; });
  };
  // Stop triggered the groups; once stopped gw resets them, ready for the next Start
  CHECK(r.until([&]{ return triggered()==0 && r.d->gw().groups_ready(&why); },20000));
  // KILL: send-cx's cancel-all, then every group triggered; the re-arm resets them
  auto k=r.post("/api/kill",json::object());
  CHECK(k && k->status==200);
  CHECK(r.until([&]{ return triggered()>0; }));
  CHECK(r.until([&]{ return !r.d->gw().groups_ready(&why); }));   // gw saw send-cx's triggers
  CHECK(r.until([&]{ return r.d->kill_switch().confirmed_gen()==r.d->kill_switch().generation(); }));
  CHECK(r.d->kill_switch().rearm(now_ms(),mono_ms()+61000).empty());
  CHECK(r.until([&]{ return triggered()==0 && r.d->gw().groups_ready(&why); },20000));
  CHECK(r.d->status()["gw"]["sys_sent"]==1);
  {   // its writes were counted against the Basic buckets (gw/write_budget.h); none refused
    const auto wb=r.d->status()["gw"]["write_budget"];
    CHECK(wb["cap"]==100 && wb["sys_share"]==0.8 && !wb["tokens"].empty());
    CHECK(r.d->status()["gw"]["sys_rate_refused"]==0 && r.d->status()["gw"]["venue_429"]==0);
  }
  // the lane rows, orders and replies the page got, checked by contract.js itself
  bool lane_on_stream=false;
  for(const auto& e:of(st,"lane")) if(e["data"]["id"]=="M2" && e["data"]["on"]==true) lane_on_stream=true;
  CHECK(lane_on_stream);
  if(std::string(DESKD_NODE).empty()) { std::printf("node not found: contract.js check SKIPPED\n"); return; }
  httplib::Client c("127.0.0.1",r.d->port());
  auto snap=c.Get("/api/snapshot?sid=rt-sys-1");
  CHECK(snap && snap->status==200);
  json cap={{"snapshots",json::array()},{"events",st.envelopes()},{"responses",responses}};
  if(snap && snap->status==200) {
    const auto sj=json::parse(snap->body);
    cap["snapshots"].push_back(sj);
    CHECK(sj["lanes"].size()==5);
  }
  const std::string file=r.dir.path+"/capture-sys.json";
  std::ofstream(file)<<cap.dump();
  const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
  CHECK(std::system(cmd.c_str())==0);
}
// LIP-15 on the fake: lane M1 on, Start, and the maker quotes a 15-minute window one tick inside
// both touches in M1's group; the street hits our bid, the lane is long 10, the maker pulls for a
// second and quotes again skewed; Stop takes every quote down.
void systematic_maker() {
  const std::string W="KXNATGAS15M-26OCT101200-00";
  Run r(false,[&](fakex::Engine& e,std::int64_t now) {
    auto spec=fx::spec(W,0); spec.open_ms=now-60000; spec.close_ms=now+840000;
    spec.strike_type="greater_or_equal"; spec.floor_strike=3.1;
    e.add_market(spec,now);
    e.street_add(W,false,4000,500,now);   // YES bid 0.40 x5
    e.street_add(W,true,5400,500,now);    // NO bid 0.54 x5: YES ask 0.46 x5
    // a liquidity incentive on it: $20 over the window, target 10 a side, half a cent away half
    fakex::Engine::Incentive lip; lip.ticker=W; lip.start_ms=now-60000; lip.end_ms=now+840000;
    lip.reward=200000; lip.target=1000; lip.discount_bps=5000;
    e.add_incentive(lip);
  },true);
  r.relay_pyth("Commodities.Index.NATGAS/USD",3.1);
  CHECK(r.ready());
  Stream st(r.d->port(),"rt-maker-1");
  CHECK(r.until([&]{ return !st.envelopes().empty(); }));
  auto lane=r.post("/api/lane/M1",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  struct Q { Px px; Qty left; fakex::Side side; std::string cid, group; };
  auto quotes=[&]{
    return r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t) {
      std::vector<Q> out;
      for(const fakex::Order* o:e.user_orders())
        if(o->status==fakex::OrdStatus::RESTING && o->client_order_id.rfind("SM1-",0)==0 && o->ticker==W)
          out.push_back({o->price,o->remaining,o->side,o->client_order_id,o->order_group_id});
      return out;
    });
  };
  auto has=[&](fakex::Side side,Px px,Qty left) {
    for(const auto& x:quotes()) if(x.side==side && x.px==px && x.left==left && !x.group.empty()) return true;
    return false;
  };
  const bool quoted=r.until([&]{ return has(fakex::Side::BID,4100,1000) && has(fakex::Side::ASK,4500,1000); },15000);
  if(!quoted) std::fprintf(stderr,"maker: %s\nquotes: %zu\n",r.d->status()["maker"].dump().c_str(),quotes().size());
  CHECK(quoted);
  {   // our LIP share (strat/lip.h): 10 a side at 0.41 / 0.45 inside the street's 5s is the whole
      // target on both sides, so all of each second's reward
    const bool lip=r.until([&]{
      const auto s=r.d->status()["lip"];
      return s["programs"]==1 && s["markets"].contains(W) && s["markets"][W]["share_now"]==1.0 &&
             s["markets"][W]["scored_s"].get<int>()>=1;
    });
    if(!lip) std::fprintf(stderr,"lip: %s\n",r.d->status()["lip"].dump().c_str());
    CHECK(lip);
  }
  // the street sells 10 into our bid
  r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { CHECK(e.street_take(W,fakex::Side::ASK,4100,1000,now)==1000); });
  CHECK(r.until([&]{ const auto v=r.d->gw().sys_view(); return v && v->positions.count("M1|"+W) && v->positions.at("M1|"+W)==1000; }));
  {   // the header's Systematic numbers: long 10 at 0.41 holds $4.10 at worst, under a 70% ceiling
    const auto v=r.d->gw().sys_view();
    CHECK(v->used>=4100000 && v->ceiling>0 && v->eff>=v->used);
  }
  // long 10: the bid stays at 0.41 (its cap is now 0.41), the ask too (its floor 0.43); both re-posted
  CHECK(r.until([&]{ return has(fakex::Side::BID,4100,1000) && has(fakex::Side::ASK,4500,1000); },15000));
  CHECK(r.d->status()["maker"]["places"].get<int>()>=3);
  // the fill on the stream is lane M1's, and its 1 s markout follows (the mid is still 0.43)
  std::string fid;
  CHECK(r.until([&]{
    for(const auto& e:of(st,"fill")) if(e["data"]["lane"]=="M1" && e["data"]["side"]=="BUY") { fid=e["data"]["fid"]; return true; }
    return false; }));
  CHECK(r.until([&]{
    for(const auto& e:of(st,"mo")) if(e["data"]["fid"]==fid && e["data"]["h"]=="1s") return true;
    return false; },10000));
  if(!std::string(DESKD_NODE).empty()) {
    json cap={{"snapshots",json::array()},{"events",st.envelopes()},{"responses",json::array()}};
    const std::string file=r.dir.path+"/capture-maker.json";
    std::ofstream(file)<<cap.dump();
    const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
    CHECK(std::system(cmd.c_str())==0);
  }
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  const bool down=r.until([&]{ return quotes().empty(); });
  if(!down) {
    for(const auto& q:quotes()) std::fprintf(stderr,"left resting: %s %lld x %lld group %s\n",q.cid.c_str(),(long long)q.px,(long long)q.left,q.group.c_str());
    std::fprintf(stderr,"stop: %s\nstatus gw: %s\n",stop->body.c_str(),r.d->status()["gw"].dump().c_str());
  }
  CHECK(down);
}
// BAND 96-99 on the fake: lane M2 on and Start; on a market whose YES bid is 0.97 it joins that
// bid with 5, in M2's group; on one whose YES ask is 0.02 (NO the favourite) it joins the ask.
void systematic_band() {
  const std::string Y="KXHIGHNY-26OCT10-B70", N="KXHIGHNY-26OCT10-B80";
  Run r(false,[&](fakex::Engine& e,std::int64_t now) {
    for(const auto& tk:{Y,N}) { auto spec=fx::spec(tk,0); spec.open_ms=now-3600000; spec.close_ms=now+7200000; e.add_market(spec,now); }
    e.street_add(Y,false,9700,2000,now); e.street_add(Y,true,100,300,now);    // YES 0.97 bid / 0.99 ask
    e.street_add(N,false,100,500,now);   e.street_add(N,true,9800,800,now);   // YES 0.01 bid / 0.02 ask
  },true);
  CHECK(r.ready());
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  auto quoted=[&](const std::string& tk,fakex::Side side,Px px) {
    return r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t) {
      for(const fakex::Order* o:e.user_orders())
        if(o->status==fakex::OrdStatus::RESTING && o->client_order_id.rfind("SM2-",0)==0 && o->ticker==tk &&
           o->side==side && o->price==px && o->remaining==500 && !o->order_group_id.empty()) return true;
      return false;
    });
  };
  const bool both=r.until([&]{ return quoted(Y,fakex::Side::BID,9700) && quoted(N,fakex::Side::ASK,200); },15000);
  if(!both) std::fprintf(stderr,"band: %s\n",r.d->status()["band"].dump().c_str());
  CHECK(both);
  CHECK(r.d->status()["band"]["last"][Y]=="quoting");
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  CHECK(r.until([&]{ return !quoted(Y,fakex::Side::BID,9700) && !quoted(N,fakex::Side::ASK,200); }));
}
// Systematic's races on the fake (plan section 8): a quote hit before its 201 arrives, a fill
// during a Systematic cancel, and Stop overtaking a create still on the wire. After each, gw's
// lane position equals the venue's, the ledger stays healthy and nothing of Systematic rests.
void systematic_races() {
  Run r(false,{},true);
  CHECK(r.ready());
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  auto sys=[&](const std::string& cid,Px px,Qty count) {
    PlaceIntent p; p.cid=cid; p.owner=Owner::SYS; p.lane="M2"; p.ticker=TK; p.side=Side::BUY; p.price=px; p.count=count;
    p.tif=Tif::POST; p.ttl_s=30; p.reply=std::make_shared<std::promise<OrderReply>>();
    auto f=p.reply->get_future();
    CHECK(r.d->gw().post(std::move(p)));
    return f;
  };
  auto venue_pos=[&]{
    return r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      for(const auto& [key,p]:e.positions()) if(key.first==0 && key.second==TK) return p.pos;
      return Qty{0};
    });
  };
  auto gw_pos=[&]{
    const auto v=r.d->gw().sys_view();
    return v && v->positions.count("M2|"+TK) ? v->positions.at("M2|"+TK) : Qty{0};
  };
  auto sys_resting=[&]{
    return r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      int n=0; for(const fakex::Order* o:e.user_orders()) if(o->status==fakex::OrdStatus::RESTING && o->client_order_id.rfind("SM2-",0)==0) ++n; return n; });
  };
  // 1. the street takes 1 of our resting bid before its 201 reaches us
  fakex::HttpFault hit; hit.kind=fakex::HttpFault::Kind::HIT_AFTER_REST; hit.qty=100;
  r.venue.f->add_fault(hit);
  auto a=sys("SM2-race-1",4100,200);
  CHECK(a.wait_for(std::chrono::seconds(5))==std::future_status::ready && a.get().code.empty());
  CHECK(r.until([&]{ return gw_pos()==100 && venue_pos()==100; }));
  // 2. a fill during our cancel: the street takes the other 1 first, the reply reduces by nothing left
  fakex::HttpFault during; during.kind=fakex::HttpFault::Kind::FILL_DURING_CANCEL; during.method="DELETE";
  during.path="/portfolio/events/orders"; during.qty=100;
  r.venue.f->add_fault(during);
  CancelIntent cx; cx.scope=CancelIntent::Scope::CID; cx.cid="SM2-race-1";
  CHECK(r.d->gw().post(cx));
  CHECK(r.until([&]{ return gw_pos()==200 && venue_pos()==200 && sys_resting()==0; }));
  // 3. Stop overtakes a create the venue is still answering: the order is cancelled once its id is known
  fakex::HttpFault slow; slow.kind=fakex::HttpFault::Kind::STALL_AFTER; slow.stall_ms=800;
  r.venue.f->add_fault(slow);
  auto c=sys("SM2-race-3",4000,100);
  CHECK(r.until([&]{ return r.venue.has_cid("SM2-race-3"); }));
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  CHECK(c.wait_for(std::chrono::seconds(5))==std::future_status::ready);
  CHECK(r.until([&]{ return sys_resting()==0; }));
  const auto st=r.d->status();
  CHECK(st["gw"]["ledger_healthy"]==true && st["senders"]["k2"]["healthy"]==true && !r.d->kill_switch().active());
  CHECK(gw_pos()==venue_pos());
}
// A group the venue triggers on its own (its contracts limit): order_group_updates tells gw, which
// blocks the lane (GROUPS fails, its orders are refused) while Systematic runs, and resets the
// group once it is stopped.
void systematic_group_trigger() {
  Run r(false,{},true);
  CHECK(r.ready());
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  const auto gid=r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
    std::string id; for(const auto& [k,g]:e.groups()) id=k; return id; });
  CHECK(!gid.empty());
  r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { CHECK(!e.group_trigger(gid,0,0,now)); });
  CHECK(r.until([&]{ return !r.d->gw().groups_ready(&why); }));
  CHECK(why.find("triggered")!=std::string::npos);
  PlaceIntent p; p.cid="SM2-trig-1"; p.owner=Owner::SYS; p.lane="M2"; p.ticker=TK; p.side=Side::BUY; p.price=3900;
  p.count=100; p.tif=Tif::POST; p.ttl_s=30; p.reply=std::make_shared<std::promise<OrderReply>>();
  auto f=p.reply->get_future();
  CHECK(r.d->gw().post(std::move(p)));
  CHECK(f.wait_for(std::chrono::seconds(5))==std::future_status::ready);
  const auto refused=f.get();
  CHECK(refused.code=="RO" && refused.msg.find("not active")!=std::string::npos);
  CHECK(r.d->status()["run"]["state"]=="RUN");   // never reset while running
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },15000));
  CHECK(!r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t) { return e.group(gid)->triggered; }));
}
// Systematic's book against the venue's own records (the order-safety review, 2026-10-10):
//   - a fill the private feed lost still moves the lane: each REST read's fills count once
//   - a reducing order is never refused for money, past the lane's budget at its full size
//   - a market the lane holds that settles realizes the position, and HALTs a day past the stop
//   - KILL and its re-arm keep the HALT; so does a restart, with the day and the positions
void systematic_book() {
  // three events: the lane's budget is what this test reaches, not one group's $15 cap
  const std::string W="KXRUNTIME-26OCT11-W", V="KXRUNTIME-26OCT12-V";
  Run r(false,[&](fakex::Engine& e,std::int64_t now) {
    for(const auto& tk:{W,V}) { auto spec=fx::spec(tk,0); spec.close_ms=(now/1000+3600)*1000; e.add_market(spec,now); }
    e.street_add(W,false,6000,5000,now); e.street_add(W,true,3800,5000,now);   // YES 0.60 bid / 0.62 ask
    e.street_add(V,false,3000,500,now);  e.street_add(V,true,6800,500,now);    // YES 0.30 bid / 0.32 ask
  },true,[](DeskdConfig& c,RuntimeOptions& o) { c.daily_stop=10000; o.rest_listing_ms=500; });   // a $1.00 stop
  CHECK(r.ready());
  for(const auto& tk:{W,V}) { auto w=r.post("/api/watch",{{"v","K"},{"tk",tk},{"on",true}}); CHECK(w && w->status==200); }
  CHECK(r.until([&]{ return r.d->status()["books_live"]>=3; },15000));
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  int n=0;
  auto sys=[&](const std::string& tk,Side side,Px px,Qty count,bool ioc=false) {
    PlaceIntent p; p.cid="SM2-book-"+std::to_string(++n); p.owner=Owner::SYS; p.lane="M2"; p.ticker=tk; p.side=side;
    p.price=px; p.count=count; p.tif=ioc ? Tif::IOC : Tif::POST; p.ttl_s=ioc ? 0 : 30; p.reduce_only=ioc;
    p.reply=std::make_shared<std::promise<OrderReply>>();
    auto f=p.reply->get_future();
    CHECK(r.d->gw().post(std::move(p)));
    if(f.wait_for(std::chrono::seconds(5))!=std::future_status::ready) return OrderReply{"TIMEOUT","",""};
    auto a=f.get();
    if(!a.code.empty()) std::fprintf(stderr,"sys %s: %s %s\n",tk.c_str(),a.code.c_str(),a.msg.c_str());
    return a;
  };
  auto gw_pos=[&](const std::string& tk) {
    const auto v=r.d->gw().sys_view();
    return v && v->positions.count("M2|"+tk) ? v->positions.at("M2|"+tk) : Qty{0};
  };
  // the street sells into our resting bid
  auto take=[&](const std::string& tk,Px px,Qty q) {
    return r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { return e.street_take(tk,fakex::Side::ASK,px,q,now); });
  };
  // 1. the private feed loses the fill: the next REST read moves the lane anyway
  r.venue.f->drop_ws_messages("fill",1);
  CHECK(sys(TK,Side::BUY,4100,200).code.empty());
  CHECK(take(TK,4100,200)==200);
  CHECK(r.until([&]{ return gw_pos(TK)==200; },10000));
  CHECK(r.d->status()["gw"]["ledger_healthy"]==true);
  // 2. long 20 V at 0.31 and 20 W at 0.61: $0.82 + $6.20 + $12.20 of M2's $25
  // (the dropped fill above leaves a sequence gap the next fill reveals: the private feed then
  // resyncs, and an order sent in that moment is refused STALE; sent again once it is back)
  OrderReply placed;
  CHECK(r.until([&]{ placed=sys(V,Side::BUY,3100,2000); return placed.code!="STALE"; },10000));
  CHECK(placed.code.empty());
  CHECK(take(V,3100,2000)==2000);
  CHECK(r.until([&]{ placed=sys(W,Side::BUY,6100,2000); return placed.code!="STALE"; },10000));
  CHECK(placed.code.empty());
  CHECK(take(W,6100,2000)==2000);
  CHECK(r.until([&]{ return gw_pos(V)==2000 && gw_pos(W)==2000; },10000));
  // 3. selling all 20 W: at its full worst case ($0.41 x 20) the lane would pass $25, but it only
  // reduces (reduce_only waits for the REST read to show the position, as the maker's retry does)
  OrderReply sold;
  CHECK(r.until([&]{ sold=sys(W,Side::SELL,5900,2000,true); return sold.code!="REDUCE"; },10000));
  CHECK(sold.code.empty());
  CHECK(r.until([&]{ return gw_pos(W)==0; },10000));
  CHECK(r.d->status()["run"]["state"]=="RUN");   // the day: -0.20 and the sale's fee, inside the $1.00 stop
  // 4. TK settles NO: the long of 2 at 0.41 realizes -0.82, the day passes -$1.00: HALTED
  r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { e.settle_market(TK,false,now); });
  CHECK(r.until([&]{ return r.d->status()["run"]["state"]=="HALTED"; },15000));
  // the view strat reads follows within a tick of the HALT
  CHECK(r.until([&]{ return gw_pos(TK)==0 && gw_pos(V)==2000 && r.d->gw().sys_view()->day<-1000000; }));
  const auto halted_day=r.d->gw().sys_view()->day;
  auto again=r.post("/api/start",json::object());
  CHECK(again && again->status!=200 && json::parse(again->body)["err"]["code"]=="HALTED");
  // 5. KILL and its re-arm leave it HALTED (until ET midnight)
  CHECK(r.post("/api/kill",json::object()));
  CHECK(r.until([&]{ return r.d->kill_switch().confirmed_gen()==r.d->kill_switch().generation(); }));
  CHECK(r.d->kill_switch().rearm(now_ms(),mono_ms()+61000).empty());
  CHECK(r.until([&]{ return r.d->status()["run"]["state"]=="HALTED"; }));
  // 6. a restart: the HALT, the day and V's position come back
  CHECK(std::filesystem::exists(r.dir.path+"/systematic.json"));
  r.restart();
  CHECK(r.d->status()["run"]["state"]=="HALTED");
  CHECK(r.until([&]{ return gw_pos(V)==2000 && r.d->gw().sys_view()->day<=halted_day+100000; },15000));
  CHECK(gw_pos(TK)==0 && gw_pos(W)==0);
  again=r.post("/api/start",json::object());
  CHECK(again && again->status!=200 && json::parse(again->body)["err"]["code"]=="HALTED");
}
// Manual takes over a market a lane quotes (plan section 4, contract mkt.man): the lane's
// order there is cancelled (why TAKEOVER), its position handed to Manual at the mid, the market
// held (mkt.man) and refused to the lanes until POST /api/release.
void takeover() {
  Run r(true,{},true);
  CHECK(r.ready());
  Stream st(r.d->port(),"rt-take-1");
  CHECK(r.until([&]{ return !st.envelopes().empty(); }));
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  int n=0;
  auto sys=[&](Px px,Qty count) {
    PlaceIntent p; p.cid="SM2-take-"+std::to_string(++n); p.owner=Owner::SYS; p.lane="M2"; p.ticker=TK; p.side=Side::BUY;
    p.price=px; p.count=count; p.tif=Tif::POST; p.ttl_s=30; p.reply=std::make_shared<std::promise<OrderReply>>();
    auto f=p.reply->get_future();
    CHECK(r.d->gw().post(std::move(p)));
    return f.wait_for(std::chrono::seconds(5))==std::future_status::ready ? f.get() : OrderReply{"TIMEOUT","",""};
  };
  auto gw_pos=[&]{
    const auto v=r.d->gw().sys_view();
    return v && v->positions.count("M2|"+TK) ? v->positions.at("M2|"+TK) : Qty{0};
  };
  // the lane holds 2 (its bid at 0.41 taken) and rests another bid at 0.39
  CHECK(sys(4100,200).code.empty());
  r.venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { CHECK(e.street_take(TK,fakex::Side::ASK,4100,200,now)==200); });
  CHECK(r.until([&]{ return gw_pos()==200; }));
  // the risk book: the position's event, losing its cost at worst ($0.82), with the shards' cash
  CHECK(r.until([&]{
    for(const auto& e:of(st,"risk"))
      for(const auto& g:e["data"]["groups"])
        if(g["members"].dump().find(TK)!=std::string::npos && g["worst"]=="-0.8200" && g["best"]=="1.1800" &&
           !e["data"]["shards"].empty()) return true;
    return false; },10000));
  const auto resting=sys(3900,100);
  CHECK(resting.code.empty());
  // a manual order on the market: the lane's bid goes (TAKEOVER), its 2 are handed over
  const auto m=r.order("Mtake-1","BUY","0.3800",1.0,"POST",60);
  CHECK(m["status"]==200);
  CHECK(r.until([&]{ return r.resting()==1; }));   // only the manual order rests
  CHECK(r.until([&]{
    for(const auto& e:of(st,"ord")) if(e["data"]["cid"]=="SM2-take-2" && e["data"]["st"]=="CXL" && e["data"]["why"]=="TAKEOVER") return true;
    return false; }));
  CHECK(r.until([&]{ return gw_pos()==0; }));
  CHECK(r.until([&]{
    for(const auto& e:of(st,"mkt")) if(e["data"]["tk"]==TK && e["data"]["man"]==true) return true;
    return false; }));
  const auto held=r.d->gw().manual_held();
  CHECK(held && held->count(TK));
  // the lanes stay out until it is released
  const auto refused=sys(3900,100);
  CHECK(refused.code=="BAD_STATE" && refused.msg.find("held by Manual")!=std::string::npos);
  auto rel=r.post("/api/release",{{"v","K"},{"tk",TK}});
  CHECK(rel && rel->status==200 && json::parse(rel->body)==json({{"ok",true}}));
  CHECK(r.until([&]{
    for(const auto& e:of(st,"mkt")) if(e["data"]["tk"]==TK && e["data"]["man"]==false) return true;
    return false; }));
  auto again=r.post("/api/release",{{"v","K"},{"tk",TK}});
  CHECK(again && again->status!=200 && json::parse(again->body)["err"]["code"]=="UNKNOWN_MARKET");
  CHECK(sys(3900,100).code.empty());
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  // everything the page got (mkt.man, risk, the TAKEOVER cancel), checked by contract.js itself
  if(!std::string(DESKD_NODE).empty()) {
    json cap={{"snapshots",json::array()},{"events",st.envelopes()},{"responses",json::array()}};
    const std::string file=r.dir.path+"/capture-takeover.json";
    std::ofstream(file)<<cap.dump();
    const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
    CHECK(std::system(cmd.c_str())==0);
  }
}
// SHARD: a lane that is on whose market is listed on a shard without cash fails the preflight,
// naming the shard, its cash and the market; Start is refused until it has cash.
void shard_preflight() {
  const std::string S1="KXRUNTIME-26OCT10-S1";
  Run r(true,[&](fakex::Engine& e,std::int64_t now) {
    auto spec=fx::spec(S1,1); spec.close_ms=(now/1000+3600)*1000; e.add_market(spec,now);   // shard 1: $0
  },true);
  CHECK(r.ready());
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  json row;
  CHECK(r.until([&]{
    auto st=r.post("/api/start",json::object());
    if(!st || st->status==200) return false;
    const auto j=json::parse(st->body);
    // the rows of a Start just after launch can predate the first reads ("stale"): the one that
    // counts names the shard
    for(const auto& x:j.value("preflight",json::array()))
      if(x["id"]=="SHARD" && x["ok"]==false && x["why"].get<std::string>().rfind("S1 ",0)==0) { row=x; return true; }
    return false; },15000));
  CHECK(row.value("why",std::string())=="S1 $0.0000 (M2 "+S1+")");
  // the ticket's MOVE: $100 from S0 to S1 (refused past S0's free cash, and between unknown shards)
  auto too_much=r.post("/api/transfer",{{"from","S0"},{"to","S1"},{"amt","5000.0000"}});
  CHECK(too_much && too_much->status!=200 && json::parse(too_much->body)["err"]["code"]=="NO_CASH");
  auto nowhere=r.post("/api/transfer",{{"from","S0"},{"to","S9"},{"amt","1.0000"}});
  CHECK(nowhere && nowhere->status==400 && json::parse(nowhere->body)["err"]["code"]=="BAD_REQUEST");
  auto mv=r.post("/api/transfer",{{"from","S0"},{"to","S1"},{"amt","100.0000"}});
  CHECK(mv && mv->status==200);
  if(mv && mv->status==200) {
    const auto j=json::parse(mv->body);
    bool s1=false;
    for(const auto& x:j["shards"]) if(x["id"]=="S1" && x["cash"]=="100.0000" && x["free"]=="100.0000") s1=true;
    CHECK(j["ok"]==true && s1);
  } else if(mv) std::fprintf(stderr,"transfer: %s\n",mv->body.c_str());
  CHECK(r.venue.f->with_engine([](fakex::Engine& e,std::int64_t) { return e.cash(0,1); })==100*fx::D);
  // with cash on shard 1 the row passes (Start then goes once every other row does)
  CHECK(r.until([&]{
    auto st=r.post("/api/start",json::object());
    if(!st) return false;
    if(st->status==200) return true;
    for(const auto& x:json::parse(st->body).value("preflight",json::array())) if(x["id"]=="SHARD") return x["ok"]==true;
    return false; },20000));
}
// Orders on the account this run did not place: a previous run's (M-prefixed) and the website's
// (no client id). The page shows them from the REST read; the x on one cancels it by order id,
// and cancel_all with own ANY cancels every one (conformance review: they could not be cancelled
// short of KILL, and they blocked their market).
void foreign_cancels() {
  std::string old_oid, web_oid;
  Run r(true,[&](fakex::Engine& e,std::int64_t now) {
    fakex::NewOrder o; o.ticker=TK; o.side=fakex::Side::BID; o.count=100; o.price=3500; o.client_order_id="Mold-run-1";
    old_oid=e.create(o,now).order_id;
    o.client_order_id.clear(); o.price=3400;
    web_oid=e.create(o,now).order_id;
  });
  CHECK(!old_oid.empty() && !web_oid.empty());
  CHECK(r.ready());
  CHECK(r.resting()==2);
  // the x on the previous run's order: its cid as the page shows it
  auto cx=r.post("/api/cancel",{{"cids",{"Mold-run-1"}}});
  CHECK(cx && cx->status==200);
  if(cx && cx->status==200) CHECK(json::parse(cx->body)["cids"]==json::array({"Mold-run-1"}));
  CHECK(r.until([&]{ return r.resting()==1; }));
  // cancel_all, any owner: the website's order too
  auto all=r.post("/api/cancel_all",{{"v","K"},{"own","ANY"}});
  CHECK(all && all->status==200);
  CHECK(r.until([&]{ return r.resting()==0; }));
  CHECK(r.d->status()["gw"]["ledger_healthy"]==true);
  // and a manual order on the market goes now that nothing foreign rests there
  CHECK(r.until([&]{ return r.order("Mforeign-1","BUY","0.3900",1.0,"POST",60)["status"]==200; },10000));
}
// Paper and live: a demo credential runs without --live, a production one only with it, each on
// its own account's hosts; any mismatch never starts (and dials nothing). A live desk against the
// fake on loopback then trades as the paper one does: the production hosts are never dialled here.
void live_mode() {
  TempDir dir;
  const std::string key=dir.path+"/k.pem";
  { std::ofstream f(key); f<<pem; }
  ::chmod(key.c_str(),0600);
  auto refused=[&](const std::string& env,bool live,const std::string& host,const std::string& want) {
    DeskdConfig c; c.data_dir=dir.path; c.ui_dir=std::string(DESKD_SOURCE_DIR)+"/ui/desk"; c.http_port=0;
    c.env=env; c.key_id="k"; c.pem_path=key; c.key_type="ed25519"; c.live=live; c.manual_orders=true;
    RuntimeOptions o;
    if(!host.empty()) { o.rest_url="https://"+host+"/trade-api/v2"; o.ws_url="https://"+host+"/trade-api/ws/v2"; }
    std::string err;
    auto lock=AccountLock::acquire(dir.path,live?"prod":"demo","deskd-test-live",false,&err);
    Deskd d(c,std::move(lock),o);
    const bool started=d.start(&err);
    if(started) d.stop();
    if(started || err.find(want)==std::string::npos) std::fprintf(stderr,"live_mode: wanted \"%s\", got \"%s\"\n",want.c_str(),err.c_str());
    return !started && err.find(want)!=std::string::npos;
  };
  CHECK(refused("demo",true,"","--live takes a production credential"));
  CHECK(refused("prod",false,"","runs only with --live"));
  CHECK(refused("staging",false,"","must be \"demo\" (paper) or \"prod\" (live)"));
  CHECK(refused("prod",true,"demo-api.kalshi.co","not a production Kalshi host"));
  CHECK(refused("demo",false,"api.elections.kalshi.com","not a demo Kalshi host"));
  CHECK(refused("prod",true,"example.com","not a production Kalshi host"));
  CHECK(std::string(kProdRestUrl)=="https://api.elections.kalshi.com/trade-api/v2");
  CHECK(std::string(kProdWsUrl)=="https://api.elections.kalshi.com/trade-api/ws/v2");

  Run r(true,{},false,[](DeskdConfig& c,RuntimeOptions&) { c.env="prod"; c.live=true; });
  CHECK(r.ready());
  CHECK(r.d->status()["mode"]=="LIVE" && r.d->status()["account"]=="prod");
  // the page names its venue and the paper desk's page for the header's switch
  httplib::Client page("127.0.0.1",r.d->port());
  auto html=page.Get("/");
  CHECK(html && html->status==200);
  if(html) {
    CHECK(html->body.find("<meta name=\"desk-venue\" content=\"sim\">")!=std::string::npos);
    CHECK(html->body.find("<meta name=\"desk-peer\" content=\"http://127.0.0.1:8788/\">")!=std::string::npos);
  }
  auto o=r.order("Mrt-live-1","BUY","0.4200",1,"IOC");
  CHECK(o["status"]==200 && o["ord"]["st"]=="FILL");
}
// The fill model end to end: prints made before deskd started reach it by the REST backfill
// (/markets/trades), GET /api/fill reads the queue ahead from the live book, and its replies are
// contract 1.6's.
void fill_estimates() {
  Run r(true,[](fakex::Engine& e,std::int64_t now) {
    e.street_add(TK,false,4000,1000,now);   // YES bid 0.40 x10 more (x15 in all)
    for(int i=0;i<6;++i) e.street_take(TK,fakex::Side::ASK,4000,100,now);   // six 1-lot sellers hit it: x9 left
    e.street_take(TK,fakex::Side::BID,4200,100,now);   // a 1-lot buyer lifts the ask: x2 left
  });
  CHECK(r.ready());
  json caps=json::array();
  auto fill=[&](const std::string& query) {
    httplib::Client c("127.0.0.1",r.d->port()); c.set_read_timeout(6,0);
    auto res=c.Get("/api/fill?"+query);
    json j=res ? json::parse(res->body,nullptr,false) : json();
    if(res) caps.push_back({{"key","GET /api/fill"},{"status",res->status},{"json",j}});
    j["status"]=res ? res->status : 0;
    return j;
  };
  // the backfill brings the hour before the start: the estimate stands
  json f;
  CHECK(r.until([&]{ f=fill("tk="+TK+"&side=BUY&px=0.4000&qty=1&ttl=60"); return f["status"]==200 && f["fill"]["why"]==""; },15000));
  if(f["status"]!=200) { std::fprintf(stderr,"fill: %s\n",f.dump().c_str()); return; }
  const json& F=f["fill"];
  CHECK(F["windows"].get<int>()>=3000 && F["tape_s"].get<int>()>=3500);
  // joining the 0.40 bid: the nine left are ahead of it; six 1-lots in the hour never clear them
  CHECK(F["at"]["px"]=="0.4000" && F["at"]["ahead"]==9 && F["at"]["p_full"]==0.0);
  // the curve: 0.41 (improves the touch, nobody ahead: the six sellers would have hit it in the
  // windows that held them), the touch, three ticks behind
  CHECK(F["curve"].size()==5 && F["curve"][0]["px"]=="0.4100" && F["curve"][0]["ahead"]==0 &&
        F["curve"][0]["p_full"].get<double>()<0.1);
  // the windows that hold the six sells grow as time passes them (up to 60 of the hour's 3,540)
  CHECK(r.until([&]{ return fill("tk="+TK+"&side=BUY&px=0.4100&qty=1&ttl=60")["fill"]["at"]["p_full"].get<double>()>0; },20000));
  CHECK(F["curve"][1]["px"]=="0.4000" && F["curve"][4]["px"]=="0.3700");
  CHECK(F["p80_px"].is_null());   // nothing rests its way to 80% in this quiet market: take
  CHECK(F["take"]==json({{"px","0.4200"},{"depth",2}}));
  // at or through the ask it crosses: fills now while the depth there covers it
  auto x=fill("tk="+TK+"&side=BUY&px=0.4200&qty=2&ttl=15");
  CHECK(x["status"]==200 && x["fill"]["at"]["p_full"]==1.0 && x["fill"]["at"]["med_s"]==0.0);
  x=fill("tk="+TK+"&side=BUY&px=0.4200&qty=3&ttl=15");
  CHECK(x["status"]==200 && x["fill"]["at"]["p_full"]==0.0 && x["fill"]["at"]["p_any"]==1.0);
  // an ask fills from buyers: one 1-lot in the hour
  x=fill("tk="+TK+"&side=SELL&px=0.4100&qty=1&ttl=60");
  CHECK(x["status"]==200 && x["fill"]["at"]["ahead"]==0 && x["fill"]["at"]["p_full"].get<double>()>0 &&
        x["fill"]["curve"][0]["px"]=="0.4100");
  // refusals in the contract's shape
  x=fill("tk=KXNOPE-1&side=BUY&px=0.4000&qty=1&ttl=15");
  CHECK(x["status"]==404 && x["err"]["code"]=="UNKNOWN_MARKET");
  x=fill("tk="+TK+"&side=BUY&px=1.2000&qty=1&ttl=15");
  CHECK(x["status"]==400 && x["err"]["code"]=="BAD_REQUEST");
  x=fill("tk="+TK+"&side=BUY&px=0.4000&qty=1&ttl=0");
  CHECK(x["status"]==400);
  if(!std::string(DESKD_NODE).empty()) {
    TempDir d;
    const std::string file=d.path+"/fill-capture.json";
    { std::ofstream(file) << json{{"responses",caps}}.dump(); }
    const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
    CHECK(std::system(cmd.c_str())==0);
  }
}
// The equity floor (plan section 4, limit 5): an account under it halts Systematic, which says why
void equity_floor() {
  Run r(false,{},true,[](DeskdConfig& c,RuntimeOptions&) { c.equity_floor=5000*10000; });   // $5,000; the fake holds $2,000
  CHECK(r.ready());
  auto lane=r.post("/api/lane/M2",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(r.until([&]{ return r.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(r.until([&]{ start=r.post("/api/start",json::object()); return start && start->status==200; },10000));
  CHECK(r.until([&]{ return r.d->status()["run"]["state"]=="HALTED"; },10000));
  CHECK(r.d->status()["run"]["why"].get<std::string>().find("equity floor")==0);
}
}  // namespace

int main() {
  std::signal(SIGPIPE,SIG_IGN);
  off_by_default(); orders_cancel_kill(); stop_reasons(); financial_fair(); systematic(); systematic_maker(); systematic_band(); systematic_races(); systematic_group_trigger(); systematic_book(); takeover(); shard_preflight(); foreign_cancels(); live_mode(); fill_estimates(); equity_floor();
  return finish("deskd-runtime-manual");
}
