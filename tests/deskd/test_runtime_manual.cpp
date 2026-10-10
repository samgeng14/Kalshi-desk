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
  explicit Run(bool manual_orders) {
    CHECK(venue.ok);
    venue.f->with_engine([](fakex::Engine& e,std::int64_t now) {
      auto spec=fx::spec(TK,0); spec.close_ms=(now/1000+3600)*1000;
      e.add_market(spec,now);
      e.street_add(TK,false,4000,500,now);   // YES bid 0.40 x5
      e.street_add(TK,true,5800,300,now);    // NO bid 0.58 x3: YES ask 0.42 x3
    });
    const std::string key=dir.path+"/test-key.pem";
    { std::ofstream f(key); f<<pem; }
    ::chmod(key.c_str(),0600);
    DeskdConfig c;
    c.data_dir=dir.path; c.ui_dir=std::string(DESKD_SOURCE_DIR)+"/ui/desk"; c.http_port=0;
    c.env="demo"; c.key_id="runtime-test"; c.pem_path=key; c.key_type="ed25519";
    c.manual_orders=manual_orders;
    RuntimeOptions o;
    const std::string base="https://127.0.0.1:"+std::to_string(venue.port());
    o.rest_url=base+"/trade-api/v2"; o.ws_url=base+"/trade-api/ws/v2"; o.factory=fx::trust_test_ca();
    o.rest_poll_ms=700; o.rest_gap_ms=0;   // reconciles (and clock samples at varied sub-second phases) come quickly
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
      return s["rest"]["ready"]==true && s["rest"]["fee_ready_live"]==1 && s["feeds"]["KX"]["up"]==true &&
             s["feeds"]["KXP"]["up"]==true && s["feeds"]["KXP"]["gap"]==false && s["books_live"]==1 &&
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
}  // namespace

int main() {
  std::signal(SIGPIPE,SIG_IGN);
  off_by_default(); orders_cancel_kill(); stop_reasons();
  return finish("deskd-runtime-manual");
}
