// gw's lane gates, retirement and per-event cap end to end, on test_manual_gw's harness with
// Systematic attached (send-k2, order groups, lanes) against the fake Kalshi over TLS:
//   - a gated lane (LaneSpec::gate_why) is refused BAD_REQUEST on, naming its gate, and stays
//     off; its orders are refused LANE_OFF and never reach the venue (decisions 12)
//   - a lane whose retire rule fires (its fill from the account read, its markouts from strat as
//     LaneMarkout) is retired: its working order cancelled with why RETIRED, a CRIT alert, LaneSet
//     on refused, its orders LANE_OFF; across a restart it stays so, until deskctl lane-reset
//   - the loss rule after a restart waits for a mark on every position the lane holds and for
//     the first account read, and retires on a mark that really loses
//   - a lane's event cap (LaneSpec::event_cap) counts what it holds and works in one event, and
//     never refuses a reducing order
//   - the budgets of the lanes on may not sum past their venue's 70% ceiling (decisions 1): a
//     lane set on past it is refused BAD_REQUEST naming the lanes and the overflow, and a Start
//     whose lanes no longer fit is refused PREFLIGHT, its CASH row failing
// The ord and lane rows gw published, and the replies the web would send, checked by
// contract.js itself (node): RETIRED is a contract WHY.
#include <algorithm>
#include <csignal>
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/build_paths.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kill.h"
#include "deskd/gw/manual_book.h"
#include "deskd/gw/sys_book.h"
#include "deskd/net/kalshi_sender.h"
#include "deskd/rest/state.h"
#include "deskd/web/contract.h"
#include "net_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
// RFC 8032 test vector, not an account credential (as in test_sender).
const char* pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";
const std::string EVENT="KXTEST-26OCT06";   // fx::T's event

std::shared_ptr<const rest::Snapshot> snapshot(std::int64_t now) {
  auto s=std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m;
  m.ticker=fx::T; m.event_ticker=EVENT; m.status="active"; m.exchange_index=0;
  m.open_ms=now-60000; m.close_ms=(now/1000+3600)*1000;
  m.price_ranges={{0,10000,100}}; m.fee_type="quadratic"; m.fee_multiplier=1; m.fee_ms=now;
  s->markets[m.ticker]=m;
  s->balance.available=1000*10000; s->balances[0]=1000*10000;   // one shard: one order group a lane
  s->exchange.exchange_active=s->exchange.trading_active=true;
  s->exchange.shards={{0,true,true,std::nullopt}};
  s->clock.post_trusted=true;
  s->ready=s->account_ready=s->listing_ready=true;
  s->account_began_ms=now-2000; s->account_ms=now-1000;
  return s;
}

LaneSpec lane_spec(const std::string& id) {
  LaneSpec l{id, "TEST " + id, "REST", 10 * 10000, 2000, 5000, {}, std::nullopt};
  return l;
}

struct Desk {
  fx::Venue venue;
  net::ConnFactory factory{fx::demo_guard(),fx::trust_test_ca()};
  KillSwitch kill;
  BoundedQueue<KalshiWriteJob> qm{256,64}, qc{64,64}, qs{256,64};
  std::unique_ptr<KalshiSigner> signer;
  std::unique_ptr<net::SparePool> pm,pc,ps;
  std::unique_ptr<KalshiSender> manual,cancel,sys;
  std::unique_ptr<Gateway> gw;
  Worker wm{"send-k1-test"},wc{"send-cx-test"},ws{"send-k2-test"},wg{"gw-test"};
  std::shared_ptr<const rest::Snapshot> snap=snapshot(now_ms());
  MdBook book;
  std::mutex mu;
  std::vector<json> rows;               // under mu: {"type": "ord"|"lane", "data": row}, as published
  std::vector<std::string> alerts;      // under mu: "LEVEL SRC msg"
  // read=false: no account read complete yet (account() brings one)
  Desk(std::vector<LaneSpec> lanes, const std::string& state_file, bool live=false, bool read=true) {
    CHECK(venue.ok); std::string why;
    signer=KalshiSigner::from_pem("sys-gate-test",pem,"ed25519",&why); CHECK(signer!=nullptr);
    net::PoolConfig cfg; cfg.endpoint=venue.ep(); cfg.name="k1-test"; cfg.connect_timeout_ms=500;
    pm=std::make_unique<net::SparePool>(factory,cfg); cfg.name="cx-test";
    pc=std::make_unique<net::SparePool>(factory,cfg); cfg.name="k2-test";
    ps=std::make_unique<net::SparePool>(factory,cfg);
    gw=std::make_unique<Gateway>(kill,[]{ return std::vector<PreflightRow>{{"KEY",true,""}}; });
    auto reply=[this](KalshiWriteReply r) { return gw->post(ManualWrite{std::make_shared<const KalshiWriteReply>(std::move(r))}); };
    auto alert=[](const std::string&) {};
    KalshiSender::Options options; options.connect_ms=500; options.reply_ms=400;
    manual=std::make_unique<KalshiSender>(KalshiSender::Role::MANUAL,qm,*pm,*signer,kill,reply,alert,options);
    cancel=std::make_unique<KalshiSender>(KalshiSender::Role::CANCEL,qc,*pc,*signer,kill,reply,alert,options);
    sys=std::make_unique<KalshiSender>(KalshiSender::Role::SYSTEMATIC,qs,*ps,*signer,kill,reply,alert,options);
    book.ticker=fx::T; book.stale=false; book.bids={{4000,500}}; book.asks={{4200,300}};
    ManualPath path;
    path.snapshot=[this]{ return std::atomic_load(&snap); };
    path.book=[this](const std::string& tk) -> std::optional<MdBook> { if(tk==book.ticker) return book; return std::nullopt; };
    path.health=[this]{ ManualHealth h; h.read_only=false; h.public_feed=h.private_feed=h.publisher=true;
                        h.manual_sender=manual->healthy(); h.cancel_sender=cancel->healthy(); return h; };
    path.send=[this](KalshiWriteJob j) { return qm.push(std::move(j)); };
    path.cancel=[this](KalshiWriteJob j) { return qc.push(std::move(j),Lane::PRIORITY); };
    path.sys_send=[this](KalshiWriteJob j) { return qs.push(std::move(j)); };
    path.sys_cancel=[this](KalshiWriteJob j) { return qs.push(std::move(j),Lane::PRIORITY); };
    path.sys_health=[this]{ ManualHealth h; h.read_only=false; h.public_feed=h.private_feed=h.publisher=true;
                            h.manual_sender=h.cancel_sender=sys->healthy(); return h; };
    path.publish=[this](const std::string& row) { std::lock_guard<std::mutex> lk(mu); rows.push_back({{"type","ord"},{"data",json::parse(row)}}); };
    path.publish_lane=[this](const std::string& row) { std::lock_guard<std::mutex> lk(mu); rows.push_back({{"type","lane"},{"data",json::parse(row)}}); };
    path.alert=[this](const std::string& lvl, const std::string& src, const std::string& msg) {
      std::lock_guard<std::mutex> lk(mu); alerts.push_back(lvl+" "+src+" "+msg); };
    path.lanes=std::move(lanes);
    path.sys_state_file=state_file;
    path.live=live;   // only gw's paper_only rule reads it: the venue here is the fake's either way
    path.check_each_message=true;
    gw->attach_manual(path);
    if(!read) account(1000*10000,false);
    for(auto* p:{pm.get(),pc.get(),ps.get()}) { p->start(); CHECK(p->wait_ready(1,3000)); }
    wm.start([this](Worker& w){manual->run(w);}); wc.start([this](Worker& w){cancel->run(w);});
    ws.start([this](Worker& w){sys->run(w);}); wg.start([this](Worker& w){gw->run(w);});
  }
  ~Desk() {
    for(auto* w:{&wg,&wm,&wc,&ws}) w->request_stop();
    for(auto* w:{&wg,&wm,&wc,&ws}) w->join();
    pm->stop(); pc->stop(); ps->stop();
    CHECK(gw->stats().invariant_violations==0);
  }
  LaneReply lane(const std::string& id,bool on) {
    auto p=std::make_shared<std::promise<LaneReply>>(); auto f=p->get_future();
    CHECK(gw->post(LaneSet{id,on,p}));
    if(f.wait_for(std::chrono::seconds(2))!=std::future_status::ready) { CHECK(!"gw answered"); return {"TIMEOUT","","",0}; }
    return f.get();
  }
  ControlReply start() {
    auto p=std::make_shared<std::promise<ControlReply>>(); auto f=p->get_future();
    CHECK(gw->post(Start{p}));
    if(f.wait_for(std::chrono::seconds(2))!=std::future_status::ready) { CHECK(!"gw answered"); return {"TIMEOUT","",{},0,{}}; }
    return f.get();
  }
  ControlReply stop() {
    auto p=std::make_shared<std::promise<ControlReply>>(); auto f=p->get_future();
    CHECK(gw->post(Stop{"test",p}));
    if(f.wait_for(std::chrono::seconds(2))!=std::future_status::ready) { CHECK(!"gw answered"); return {"TIMEOUT","",{},0,{}}; }
    return f.get();
  }
  // a new account read with this much cash (and no positions: the equity), or none complete yet
  void account(Money cash,bool ready=true) {
    auto s=std::make_shared<rest::Snapshot>(*std::atomic_load(&snap));
    s->balance.available=cash; s->balances[0]=cash; s->account_ready=s->ready=ready;
    s->account_began_ms=now_ms(); s->account_ms=now_ms()+1; ++s->revision;
    std::atomic_store(&snap,std::shared_ptr<const rest::Snapshot>(s));
  }
  // fn on the gw thread, the one that reads `book`
  template<class F> auto on_gw(F fn) {
    std::promise<decltype(fn())> p; auto f=p.get_future();
    CHECK(gw->post(Probe{[&]{ p.set_value(fn()); }})); return f.get();
  }
  // gw's view of a lane's day (real - fees + unrl), micro-dollars; built after the retire rules
  // have looked at the same marks
  std::optional<std::int64_t> lane_day(const std::string& id) {
    const auto v=gw->sys_view();
    if(!v || !v->lane_day.count(id)) return std::nullopt;
    return v->lane_day.at(id);
  }
  std::set<std::string> lanes_on() {
    const auto v=gw->sys_view();
    return v ? v->lanes_on : std::set<std::string>{"?"};
  }
  // a lane that is on, with its order group active (one shard here)
  bool lane_ready(const std::string& id) {
    if(!lane(id,true).code.empty()) return false;
    std::string why;
    return until([&]{ return gw->groups_ready(&why) && why.empty(); },8000);
  }
  OrderReply place(const std::string& lane,const std::string& cid,Side side,Px px,Qty n,Tif tif=Tif::POST) {
    PlaceIntent p; p.cid="S"+lane+"-"+cid; p.owner=Owner::SYS; p.lane=lane; p.ticker=fx::T; p.side=side; p.price=px;
    p.count=n; p.tif=tif; p.ttl_s=tif==Tif::POST ? 60 : 0;
    p.reply=std::make_shared<std::promise<OrderReply>>(); auto f=p.reply->get_future();
    CHECK(gw->post(std::move(p)));
    if(f.wait_for(std::chrono::seconds(4))!=std::future_status::ready) { CHECK(!"gw answered"); return {"TIMEOUT","",""}; }
    return f.get();
  }
  // an account read begun now: the venue's fills of every order so far (the private feed is not
  // wired here; gw takes Systematic's fills from the read)
  void read_account() {
    auto fills=venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      std::vector<kalshi::FillRow> out;
      for(const auto& f:e.fills()) {
        kalshi::FillRow y; y.fill_id=f.fill_id; y.trade_id=f.trade_id; y.order_id=f.order_id; y.ticker=f.ticker;
        y.side=f.side==fakex::Side::BID ? kalshi::BookSide::BID : kalshi::BookSide::ASK; y.count=f.count;
        y.yes_price=f.yes_px; y.taker=f.taker; y.fee=f.fee; y.ts_ms=f.ts_ms; y.exchange_index=f.exchange_index;
        out.push_back(y);
      }
      return out;
    });
    auto s=std::make_shared<rest::Snapshot>(*std::atomic_load(&snap));
    s->fills=fills; s->account_began_ms=now_ms(); s->account_ms=now_ms()+1; ++s->revision;
    std::atomic_store(&snap,std::shared_ptr<const rest::Snapshot>(s));
  }
  json last(const std::string& type,const std::string& key,const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for(auto it=rows.rbegin();it!=rows.rend();++it) if((*it)["type"]==type && (*it)["data"][key]==id) return (*it)["data"];
    return json();
  }
  int alerts_with(const std::string& prefix) {
    std::lock_guard<std::mutex> lk(mu);
    int n=0; for(const auto& a:alerts) if(a.rfind(prefix,0)==0) ++n;
    return n;
  }
  bool resting(const std::string& cid) {
    return venue.f->with_engine([&](fakex::Engine& e,std::int64_t) {
      for(const fakex::Order* o:e.user_orders()) if(o->client_order_id==cid && o->status==fakex::OrdStatus::RESTING) return true;
      return false; });
  }
  bool until(const std::function<bool()>& fn,int ms=4000) {
    const auto end=mono_ms()+ms;
    do { if(fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while(mono_ms()<end);
    return fn();
  }
  // every row published so far as stream envelopes, and the replies given, for contract_check.cjs
  void capture(json& cap) {
    std::lock_guard<std::mutex> lk(mu);
    for(const auto& r:rows)
      cap["events"].push_back({{"type",r["type"]},{"seq",cap["events"].size()+1},{"ts_ms",now_ms()},{"data",r["data"]}});
  }
};

json contract_capture() { return {{"snapshots",json::array()},{"events",json::array()},{"responses",json::array()}}; }
// the web's reply to POST /api/lane/{id} for gw's answer (web/server.cpp)
void lane_response(json& cap,const LaneReply& r) {
  if(r.code.empty()) cap["responses"].push_back({{"key","POST /api/lane/{id}"},{"status",200},{"json",{{"ok",true},{"lane",json::parse(r.lane)}}}});
  else cap["responses"].push_back({{"key","POST /api/lane/{id}"},{"status",400},{"json",{{"ok",false},{"err",{{"code",r.code},{"msg",r.msg}}}}}});
}
// the web's reply to POST /api/start for gw's answer (web/server.cpp control_reply)
void start_response(json& cap,const ControlReply& r) {
  if(r.code.empty()) {
    cap["responses"].push_back({{"key","POST /api/start"},{"status",200},{"json",{{"ok",true},{"run",run_json(r.run)},{"n",r.n}}}});
    return;
  }
  json body=err_body(r.code,r.msg);
  if(r.code=="PREFLIGHT") body["preflight"]=preflight_json(r.preflight);
  cap["responses"].push_back({{"key","POST /api/start"},{"status",err_http(r.code)},{"json",body}});
}
void contract_check(const json& cap,const std::string& file) {
  if(std::string(DESKD_NODE).empty()) { std::printf("node not found: contract.js check SKIPPED\n"); return; }
  std::ofstream(file)<<cap.dump();
  const std::string cmd=std::string("\"")+DESKD_NODE+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+file+"\"";
  CHECK(std::system(cmd.c_str())==0);
}

// A lane its evidence gate keeps off: never on, never trading. And no lane refusal is FORBIDDEN
// (the X-Desk token's code): D1 PAPER's on a production account is LANE_OFF too.
void gated() {
  TempDir dir;
  auto g=lane_spec("G1"); g.gate_why="no gate file gates/G1.json";
  Desk d({g,lane_spec("R1"),paper_lane()},dir.file("systematic.json"),true);
  json cap=contract_capture();
  auto r=d.lane("G1",true);
  lane_response(cap,r);
  CHECK(r.code=="BAD_REQUEST" && r.msg=="lane G1 is gated: no gate file gates/G1.json");
  r=d.lane("G1",false);   // off is always allowed, and its row says why it is off
  lane_response(cap,r);
  CHECK(r.code.empty());
  if(r.code.empty()) {
    const auto row=json::parse(r.lane);
    CHECK(row["on"]==false && row["state"]=="OFF" && row["why"]=="no gate file gates/G1.json");
  }
  CHECK(d.start().code.empty());   // Systematic runs; the gated lane still trades nothing
  const auto o=d.place("G1","gate-1",Side::BUY,3900,100);
  CHECK(o.code=="LANE_OFF" && o.msg=="lane G1 is gated: no gate file gates/G1.json" && o.ord.empty());
  CHECK(d.gw->stats().sys_sent==0 && !d.venue.has_cid("SG1-gate-1"));
  CHECK(d.place("R1","open-1",Side::BUY,3900,100).code=="BAD_STATE");   // an ungated lane that is off: as before
  const auto paper=d.place("D1","paper-1",Side::BUY,3900,100);           // on from the start, demo only
  CHECK(paper.code=="LANE_OFF" && paper.msg=="lane D1 trades only on the demo account");
  CHECK(d.gw->stats().sys_sent==0);
  d.capture(cap);
  contract_check(cap,dir.file("capture-gated.json"));
}

// A lane retired by its rule (MARKOUT after 1 trade, 50 marked at 300 s, mean <= 0): working
// order cancelled RETIRED, CRIT, refused on and LANE_OFF; kept across a restart; cleared only by
// deskctl lane-reset with gw stopped.
void retired() {
  TempDir dir;
  auto l=lane_spec("R1");
  l.retire_rule=LaneSpec::RetireRule::MARKOUT; l.retire_trades=1; l.retire_h="300s";
  const std::string file=dir.file("systematic.json");
  json cap=contract_capture();
  {
    Desk d({l},file);
    CHECK(d.lane_ready("R1"));
    CHECK(d.start().code.empty());
    auto o=d.place("R1","post-1",Side::BUY,3900,100);   // rests under the 0.40 bid
    CHECK(o.code.empty() && d.resting("SR1-post-1"));
    o=d.place("R1","take-1",Side::BUY,4200,100,Tif::IOC);   // takes 1 at the 0.42 ask: its one trade
    CHECK(o.code.empty() && json::parse(o.ord.empty() ? "{}" : o.ord)["st"]=="FILL");
    d.read_account();
    CHECK(d.until([&]{ return d.last("lane","id","R1")["fills"]==1; }));
    // 49 marked at its horizon, one at another and one fid again: not enough to read the mean
    for(int i=0;i<49;++i) CHECK(d.gw->post(LaneMarkout{"R1","f"+std::to_string(i),"300s",-100,100}));
    CHECK(d.gw->post(LaneMarkout{"R1","f49","60s",-100,100}));
    CHECK(d.gw->post(LaneMarkout{"R1","f0","300s",-100,100}));
    CHECK(d.until([&]{ return d.gw->inbox().size()==0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // sys_tick has looked, more than once
    CHECK(d.alerts_with("CRIT")==0 && d.resting("SR1-post-1"));
    // the 50th: a mean net markout of -1c over 50 marked; retired
    CHECK(d.gw->post(LaneMarkout{"R1","f49","300s",-100,100}));
    CHECK(d.until([&]{ return d.last("ord","cid","SR1-post-1")["st"]=="CXL"; }));
    CHECK(d.last("ord","cid","SR1-post-1")["why"]=="RETIRED");
    CHECK(d.until([&]{ return !d.resting("SR1-post-1"); }));
    CHECK(d.alerts_with("CRIT R1 lane R1 retired: 1 trades, mean net 300s markout -0.0100 over 50 marked")==1);
    CHECK(d.until([&]{ const auto row=d.last("lane","id","R1"); return row["state"]=="OFF" && row["on"]==false; }));
    CHECK(d.last("lane","id","R1")["why"]=="retired: 1 trades, mean net 300s markout -0.0100 over 50 marked");
    auto r=d.lane("R1",true);
    lane_response(cap,r);
    CHECK(r.code=="BAD_REQUEST" && r.msg.rfind("lane R1 is retired: 1 trades",0)==0);
    CHECK(r.msg.find("deskctl lane-reset R1") != std::string::npos);
    const auto sent=d.gw->stats().sys_sent.load();
    o=d.place("R1","post-2",Side::SELL,4500,100);   // even a reducing one
    CHECK(o.code=="LANE_OFF" && o.msg.rfind("lane R1 is retired: ",0)==0 && o.ord.empty());
    CHECK(d.gw->stats().sys_sent==sent && !d.venue.has_cid("SR1-post-2"));
    CHECK(d.until([&]{ std::ifstream in(file); const auto j=json::parse(in,nullptr,false);
                       return !j.is_discarded() && j["life"]["R1"]["retired"]==true; }));
    d.capture(cap);
  }
  contract_check(cap,dir.file("capture-retired.json"));
  {   // a restart: still retired
    Desk d({l},file);
    CHECK(d.until([&]{ return d.last("lane","id","R1")["why"]=="retired: 1 trades, mean net 300s markout -0.0100 over 50 marked"; }));
    const auto r=d.lane("R1",true);
    CHECK(r.code=="BAD_REQUEST" && r.msg.rfind("lane R1 is retired: ",0)==0);
  }
  // deskctl lane-reset, gw stopped (it takes the account lock a running deskd holds)
  const std::string cfg=dir.file("desk.json");
  std::ofstream(cfg)<<json{{"env","demo"},{"deskd",{{"lock_dir",dir.path}}}}.dump();
  const std::string cmd=std::string("\"")+DESKCTL_BIN+"\" lane-reset R1 --config \""+cfg+"\" --data-dir \""+dir.path+
                        "\" >\""+dir.file("reset.out")+"\" 2>&1";
  CHECK(std::system(cmd.c_str())==0);
  {
    std::ifstream in(dir.file("reset.out"));
    const std::string out((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    CHECK(out.find("lane R1: retired at ")!=std::string::npos && out.find("lane R1 reset")!=std::string::npos);
  }
  {
    Desk d({l},file);
    const auto r=d.lane("R1",true);   // a fresh life: it may be turned on again
    CHECK(r.code.empty());
    if(r.code.empty()) {
      const auto row=json::parse(r.lane);
      CHECK(row["on"]==true && row["why"]=="STOP");
    }
  }
}

// The loss rule after a restart: C1 holds positions from yesterday, md has no book for them yet
// and gw's last mids went with the old process. A position with no mark leaves the rule
// undecided (not $24 lost), and the rule waits for the first account read (the fills the saved
// book missed, the held markets that settled since); then a mark that really loses $20 retires.
void loss_at_boot() {
  TempDir dir;
  const auto& lanes=default_lanes();
  const LaneSpec c1=*std::find_if(lanes.begin(),lanes.end(),[](const LaneSpec& l) { return l.id=="C1"; });
  const std::string U="KXTEST-26OCT06-B";   // no book, and not in the account read: never settled
  // yesterday's book: C1 long n at px in tk, its loss limit $20
  auto saved=[&](const std::string& file,const std::string& tk,Qty n,Px px) {
    const std::int64_t yesterday=et_midnight_ms(now_ms())-86400000;
    SysBook b({c1});
    b.roll(yesterday);
    CHECK(b.fill("C1","SC1-held",tk,Side::BUY,n,px,0,yesterday+1,"tr-held"));
    std::ofstream(file)<<b.to_json(SysBook::Halt{},yesterday+2).dump();
  };
  {   // 30 at 0.80 in U: $24 of cost, unmarked
    const std::string file=dir.file("unmarked.json");
    saved(file,U,3000,8000);
    Desk d({c1},file);
    CHECK(d.until([&]{ return d.lane_day("C1")==-24000000; }));   // its day prices it worthless
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // sys_tick has looked, more than once
    CHECK(d.alerts_with("CRIT")==0 && d.last("lane","id","C1")["why"]=="");
    // U at 0.41 (fx::T's book, now U's): down $11.70, inside its $20
    d.on_gw([&]{ d.book.ticker=U; return true; });
    CHECK(d.until([&]{ return d.lane_day("C1")==-11700000; }));
    CHECK(d.alerts_with("CRIT")==0);
    // U at 0.10: down $21
    d.on_gw([&]{ d.book.bids={{900,500}}; d.book.asks={{1100,300}}; return true; });
    const std::string why="down $21.0000 over its life, its limit $20.0000";
    CHECK(d.until([&]{ return d.alerts_with("CRIT C1 lane C1 retired: "+why)==1; }));
    CHECK(d.until([&]{ return d.last("lane","id","C1")["why"]=="retired: "+why; }));
    CHECK(d.until([&]{ std::ifstream in(file); const auto j=json::parse(in,nullptr,false);
                       return !j.is_discarded() && j["life"]["C1"]["retired_why"]==why; }));
  }
  {   // 50 at 0.90 in fx::T, marked at 0.41: down $24.50, but no account read yet
    const std::string file=dir.file("unread.json");
    saved(file,fx::T,5000,9000);
    Desk d({c1},file,false,false);
    CHECK(d.until([&]{ return d.lane_day("C1")==-24500000; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(d.alerts_with("CRIT")==0);
    d.account(1000*10000);
    CHECK(d.until([&]{ return d.alerts_with("CRIT C1 lane C1 retired: down $24.5000 over its life, its limit $20.0000")==1; }));
  }
}

// LaneSpec::event_cap: the lane's worst case in one event, held and working; a reducing order
// never refused.
void event_cap() {
  TempDir dir;
  auto l=lane_spec("E1"); l.event_cap=1200000;   // $1.20
  Desk d({l},dir.file("systematic.json"));
  CHECK(d.lane_ready("E1"));
  CHECK(d.start().code.empty());
  auto o=d.place("E1","take-1",Side::BUY,4200,200,Tif::IOC);   // long 2 at 0.42: $0.84 at worst
  CHECK(o.code.empty());
  d.read_account();
  CHECK(d.until([&]{ return d.last("lane","id","E1")["open"]=="0.8400"; }));
  o=d.place("E1","buy-1",Side::BUY,3900,100);   // $0.84 + $0.39 > $1.20
  CHECK(o.code=="SYS_CEILING" && o.msg=="lane E1's worst case in event "+EVENT+" past 1.2000");
  CHECK(d.place("E1","buy-2",Side::BUY,2000,100).code.empty());   // $0.84 + $0.20
  o=d.place("E1","buy-3",Side::BUY,2000,100);                      // and the one working: past it
  CHECK(o.code=="SYS_CEILING" && o.msg.find("event "+EVENT)!=std::string::npos);
  CHECK(d.place("E1","sell-1",Side::SELL,4500,100).code.empty());  // reducing: never refused for money
}

// Decisions 1: the budgets of the lanes on, venue by venue (a KP lane's on both), may not sum past
// the venue's 70% ceiling. On past it is refused BAD_REQUEST, naming the lanes and the overflow,
// and the lane stays off; at it is fine; off always goes. A Start whose lanes no longer fit (the
// equity fell) is refused PREFLIGHT, its CASH row (free cash for the lanes) failing with the same
// words. A venue whose ceiling gw does not know fits no budget: Kalshi without an account read,
// Polymarket while gw reads no PM account.
void budgets() {
  TempDir dir;
  auto lane=[](const std::string& id,Money budget,const std::string& venue) {
    auto l=lane_spec(id); l.budget=budget*10000; l.venue=venue; return l; };
  Desk d({lane("A",40,"K"),lane("B",30,"K"),lane("C",31,"K"),lane("Q",5,"P"),lane("W",5,"KP")},dir.file("systematic.json"));
  json cap=contract_capture();
  auto set=[&](const std::string& id,bool on) { auto r=d.lane(id,on); lane_response(cap,r); return r; };
  d.account(100*10000);   // $100 of equity: a $70 ceiling
  CHECK(set("A",true).code.empty());
  auto r=set("C",true);   // $71
  CHECK(r.code=="BAD_REQUEST" && r.msg=="lane C stays off: the lane budgets on K, A 40.0000 + C 31.0000 = 71.0000, "
                                         "pass K's 70% ceiling 70.0000 by 1.0000");
  CHECK(set("B",true).code.empty());   // $70: at the ceiling
  CHECK(set("B",true).code.empty());   // on already: nothing more to fit
  r=set("W",true);        // a KP lane counts on Kalshi too: $75
  CHECK(r.code=="BAD_REQUEST" && r.msg=="lane W stays off: the lane budgets on K, A 40.0000 + B 30.0000 + W 5.0000 = 75.0000, "
                                         "pass K's 70% ceiling 70.0000 by 5.0000");
  CHECK(set("B",false).code.empty());
  r=set("W",true);        // and on Polymarket, whose ceiling gw does not know yet
  CHECK(r.code=="BAD_REQUEST" && r.msg=="lane W stays off: the lane budgets on P, W 5.0000 = 5.0000, need P's 70% ceiling, "
                                         "and gw reads no Polymarket account yet");
  r=set("Q",true);
  CHECK(r.code=="BAD_REQUEST" && r.msg.rfind("lane Q stays off: the lane budgets on P, Q 5.0000 = 5.0000, need P's",0)==0);
  CHECK(set("B",true).code.empty());
  CHECK(d.until([&]{ return d.lanes_on()==std::set<std::string>({"A","B"}); }));
  // the equity falls to $90: a $63 ceiling the lanes on no longer fit, so Start is refused
  d.account(90*10000);
  auto st=d.start();
  start_response(cap,st);
  const std::string over="the lane budgets on K, A 40.0000 + B 30.0000 = 70.0000, pass K's 70% ceiling 63.0000 by 7.0000";
  CHECK(st.code=="PREFLIGHT" && st.msg=="preflight fails: CASH ("+over+")" && st.run.state==RunState::STOP);
  CHECK(st.preflight.size()==2 && st.preflight[0].id=="KEY" && st.preflight[0].ok);
  CHECK(st.preflight.size()==2 && st.preflight[1].id=="CASH" && !st.preflight[1].ok && st.preflight[1].why==over);
  CHECK(set("B",false).code.empty());   // $40 fits: Start goes
  st=d.start();
  start_response(cap,st);
  CHECK(st.code.empty() && st.run.state==RunState::RUN);
  CHECK(d.stop().code.empty());
  // no account read complete: the ceiling is unknown, so nothing more goes on
  d.account(1000*10000,false);
  r=set("C",true);
  CHECK(r.code=="BAD_REQUEST" && r.msg=="lane C stays off: the lane budgets on K, A 40.0000 + C 31.0000 = 71.0000, "
                                         "need K's 70% ceiling, and no account read has come yet");
  st=d.start();
  start_response(cap,st);
  CHECK(st.code=="PREFLIGHT" && st.msg.find("no account read has come yet")!=std::string::npos);
  d.account(1000*10000);   // $700
  CHECK(set("C",true).code.empty() && set("B",true).code.empty());
  CHECK(d.start().code.empty());
  CHECK(d.until([&]{ return d.lanes_on()==std::set<std::string>({"A","B","C"}); }));
  d.capture(cap);
  contract_check(cap,dir.file("capture-budgets.json"));
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);   // as deskd's runtime does process-wide (app/runtime.h)
  gated(); retired(); loss_at_boot(); event_cap(); budgets();
  return finish("deskd-sys-gate");
}
