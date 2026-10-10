// deskd as one process against the fake Kalshi over TLS: REST state, market data, the private
// feed, and Manual orders from the page's HTTP routes through gw, send-k1 and send-cx. With
// deskd.manual_orders off every order is refused RO; on, a click fills, rests and cancels, and
// KILL's cancel-all clears the venue.
#include <httplib.h>
#include <sys/stat.h>
#include <csignal>
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

struct Run {
  fx::Venue venue;
  TempDir dir;
  std::unique_ptr<Deskd> d;
  explicit Run(bool manual_orders,std::function<void(fakex::Engine&,std::int64_t)> more={},bool systematic=false) {
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
    DeskdConfig c;
    c.data_dir=dir.path; c.ui_dir=std::string(DESKD_SOURCE_DIR)+"/ui/desk"; c.http_port=0;
    c.env="demo"; c.key_id="runtime-test"; c.pem_path=key; c.key_type="ed25519";
    c.manual_orders=manual_orders;
    c.systematic_orders=systematic;
    RuntimeOptions o;
    const std::string base="https://127.0.0.1:"+std::to_string(venue.port());
    o.rest_url=base+"/trade-api/v2"; o.ws_url=base+"/trade-api/ws/v2"; o.factory=fx::trust_test_ca();
    o.rest_poll_ms=700; o.rest_gap_ms=0;   // reconciles (and clock samples at varied sub-second phases) come quickly
    o.fair_watch_since_ms=now_ms()-600000;   // a window opening now is one the fair can check
    std::string err;
    auto lock=AccountLock::acquire(dir.path,"demo","deskd-test-runtime",false,&err);
    d=std::make_unique<Deskd>(c,std::move(lock),o);
    if(!d->start(&err)) { std::fprintf(stderr,"deskd did not start: %s\n",err.c_str()); std::exit(2); }
  }
  ~Run() { d->stop(); }
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
  auto cx=r.post("/api/cancel",{{"cids",{"Mrt-post-1"}}});
  CHECK(cx && cx->status==200 && json::parse(cx->body)["n"]==1);
  CHECK(r.until([&]{ return r.resting()==0; }));
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
  CHECK(s["senders"]["k1"]["why"].get<std::string>().find("invalid or unauthorized")!=std::string::npos);
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
  },true);
  CHECK(r.ready());
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
  auto stop=r.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  const bool down=r.until([&]{ return quotes().empty(); });
  if(!down) {
    for(const auto& q:quotes()) std::fprintf(stderr,"left resting: %s %lld x %lld group %s\n",q.cid.c_str(),(long long)q.px,(long long)q.left,q.group.c_str());
    std::fprintf(stderr,"stop: %s\nstatus gw: %s\n",stop->body.c_str(),r.d->status()["gw"].dump().c_str());
  }
  CHECK(down);
}
}  // namespace

int main() {
  std::signal(SIGPIPE,SIG_IGN);
  off_by_default(); orders_cancel_kill(); stop_reasons(); financial_fair(); systematic(); systematic_maker();
  return finish("deskd-runtime-manual");
}
