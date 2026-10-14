#include <csignal>
#include <mutex>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kill.h"
#include "deskd/net/kalshi_sender.h"
#include "net_fixture.h"
using namespace deskd;
using Kind=kalshi::WriteOutcome::Kind;
namespace {
bool until(const std::function<bool()>& fn) {
  const auto end=mono_ms()+4000;
  do { if(fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while(mono_ms()<end);
  return fn();
}
// RFC 8032 test vector, not an account credential. The fake checks header presence here;
// signature verification and TLS chain/host checks have their own wire/net suites.
const char* pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";
struct Harness {
  fx::Venue venue;
  net::ConnFactory factory{fx::demo_guard(),fx::trust_test_ca()};
  KillSwitch kill;
  BoundedQueue<KalshiWriteJob> qm{256,64}, qc{4,4};
  std::unique_ptr<KalshiSigner> signer;
  std::unique_ptr<net::SparePool> pm,pc;
  std::unique_ptr<KalshiSender> manual,cancel;
  Worker wm{"send-man-test"},wc{"send-cx-test"};
  std::mutex mu;
  std::vector<KalshiWriteReply> results;
  std::atomic<bool> reject_results{false};
  std::atomic<int> alerts{0};
  explicit Harness(int reply_ms=300) {
    CHECK(venue.ok); std::string why;
    signer=KalshiSigner::from_pem("sender-test",pem,"ed25519",&why); CHECK(signer!=nullptr);
    net::PoolConfig cfg; cfg.endpoint=venue.ep(); cfg.name="manual-test"; cfg.connect_timeout_ms=500;
    pm=std::make_unique<net::SparePool>(factory,cfg); cfg.name="cancel-test";
    pc=std::make_unique<net::SparePool>(factory,cfg);
    auto reply=[this](KalshiWriteReply r) { std::lock_guard<std::mutex> lk(mu); results.push_back(std::move(r)); return !reject_results.load(); };
    auto alert=[this](const std::string&) { ++alerts; };
    KalshiSender::Options options; options.connect_ms=500; options.reply_ms=reply_ms;
    manual=std::make_unique<KalshiSender>(KalshiSender::Role::MANUAL,qm,*pm,*signer,kill,reply,alert,options);
    cancel=std::make_unique<KalshiSender>(KalshiSender::Role::CANCEL,qc,*pc,*signer,kill,reply,alert,options);
  }
  void start() {
    pm->start(); pc->start(); CHECK(pm->wait_ready(1,3000)); CHECK(pc->wait_ready(1,3000));
    wm.start([this](Worker& w){manual->run(w);}); wc.start([this](Worker& w){cancel->run(w);});
  }
  ~Harness() { wm.request_stop(); wc.request_stop(); wm.join(); wc.join(); pm->stop(); pc->stop(); }
  KalshiWriteJob order(std::string cid) {
    KalshiWriteJob j; j.cid=std::move(cid); j.create.client_order_id=j.cid;
    j.create.ticker=fx::T; j.create.side=kalshi::BookSide::BID; j.create.price=4000; j.create.count=100;
    j.create.tif=kalshi::TimeInForce::GTC; j.create.expiration_s=now_ms()/1000+30;
    j.create.post_only=true; j.create.cancel_on_pause=true; j.create.exchange_index=0;
    j.permit=std::make_shared<std::atomic<bool>>(true); j.kill_generation=kill.generation();
    j.valid_until_mono_ms=mono_ms()+10000; return j;
  }
  std::optional<KalshiWriteReply> find(const std::string& cid,WireOp op=WireOp::CREATE,std::size_t index=0) {
    std::lock_guard<std::mutex> lk(mu);
    for(const auto& r:results) if(r.cid==cid && r.op==op) { if(!index) return r; --index; }
    return {};
  }
  KalshiWriteReply wait(const std::string& cid,WireOp op=WireOp::CREATE,std::size_t index=0) {
    CHECK(until([&]{return find(cid,op,index).has_value();})); return find(cid,op,index).value_or(KalshiWriteReply{});
  }
};
void writes_and_faults() {
  Harness h; h.start();
  CHECK(h.qm.push(h.order("Msend-1")));
  const auto first=h.wait("Msend-1"); CHECK(first.outcome.kind==Kind::ACK && first.create);
  CHECK(first.exchange.t_written_us && first.exchange.t_done_us && !first.pool_miss);
  CHECK(h.venue.orders()==1);
  KalshiWriteJob cancel; cancel.kind=KalshiWriteJob::Kind::CANCEL; cancel.cid="Msend-1";
  cancel.oid=first.create ? first.create->order_id : "missing"; cancel.shard=0; cancel.ticker=fx::T;
  CHECK(h.qc.push(cancel,Lane::PRIORITY));
  const auto cx=h.wait("Msend-1",WireOp::CANCEL); CHECK(cx.outcome.kind==Kind::ACK && cx.cancel && cx.cancel->reduced_by==100);

  int id=2;
  for(auto kind:{fakex::HttpFault::Kind::STATUS_AFTER_ACCEPT,fakex::HttpFault::Kind::DROP_AFTER_ACCEPT,
                 fakex::HttpFault::Kind::CUT_BODY,fakex::HttpFault::Kind::STALL_AFTER}) {
    fakex::HttpFault fault; fault.kind=kind; fault.stall_ms=600; h.venue.f->add_fault(fault);
    const auto cid="Msend-"+std::to_string(id++); const auto before=h.venue.f->http_requests();
    CHECK(h.qm.push(h.order(cid))); const auto result=h.wait(cid);
    CHECK(result.outcome.kind==Kind::UNKNOWN && !result.create);
    CHECK(h.venue.has_cid(cid) && h.venue.f->http_requests()==before+1);
  }
  // The duplicate refusal is not proof of rejection of the original request.
  CHECK(h.qm.push(h.order("Msend-1")));
  CHECK(h.wait("Msend-1",WireOp::CREATE,1).outcome.kind==Kind::UNKNOWN);
  CHECK(h.venue.orders()==5 && h.manual->healthy() && !h.kill.active());
  auto cross=h.order("Mcross-1"); cross.create.price=4500;
  CHECK(h.qm.push(cross)); CHECK(h.wait(cross.cid).outcome.kind==Kind::REFUSED);
  CHECK(!h.kill.active());
  fakex::HttpFault rate; rate.kind=fakex::HttpFault::Kind::RATE_LIMIT; h.venue.f->add_fault(rate);
  CHECK(h.qm.push(h.order("Mrate-1"))); auto limited=h.wait("Mrate-1");
  CHECK(limited.outcome.kind==Kind::REFUSED && limited.outcome.cls==RejectClass::RETRYABLE && !h.venue.has_cid("Mrate-1"));
  CHECK(!h.kill.active());
}
void permits_and_kill() {
  Harness h;
  Gateway gw(h.kill,[] {return std::vector<PreflightRow>{};});
  Worker wg{"frozen-gw"}; std::promise<void> entered,release; auto resume=release.get_future().share();
  wg.start([&](Worker& w){gw.run(w);});
  CHECK(gw.post(Probe{[&]{entered.set_value(); resume.wait();}}));
  CHECK(entered.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready);
  // A full cancellation inbox cannot suppress KILL, and a frozen gateway is irrelevant.
  for(int i=0;i<4;++i) { KalshiWriteJob j; j.kind=KalshiWriteJob::Kind::CANCEL; j.cid="bad-cancel"; CHECK(h.qc.push(j,Lane::PRIORITY)); }
  KalshiWriteJob over; CHECK(!h.qc.push(over,Lane::PRIORITY));
  CHECK(h.qm.push(h.order("Mkilled-1"))); h.kill.trip(now_ms(),mono_ms(),"test");
  const auto tripped=h.kill.rearm_ms(); h.start();
  const auto killed=h.wait("Mkilled-1"); CHECK(killed.outcome.kind==Kind::NOT_SENT && killed.local_code=="KILLED");
  const auto cancelled=h.wait("",WireOp::CANCEL_ALL);
  CHECK(cancelled.outcome.kind==Kind::ACK && h.cancel->kill_seen()==h.kill.generation());
  CHECK(h.kill.rearm_ms()>=tripped && h.kill.rearm_ms()>=cancelled.ack_ms+59000);
  CHECK(h.venue.orders()==0 && h.venue.f->http_requests()==1);
  release.set_value(); wg.request_stop(); wg.join();

  Harness fresh;
  auto revoked=fresh.order("Mrevoke-1"); revoked.permit->store(false); CHECK(fresh.qm.push(revoked));
  auto stale=fresh.order("Mstale-1"); stale.valid_until_mono_ms=mono_ms()-1; CHECK(fresh.qm.push(stale));
  auto expired=fresh.order("Mexpiry-1"); expired.create.expiration_s=now_ms()/1000; CHECK(fresh.qm.push(expired));
  auto sys=fresh.order("Ssys-1"); CHECK(fresh.qm.push(sys));
  fresh.start();
  CHECK(fresh.wait(revoked.cid).local_code=="USER"); CHECK(fresh.wait(stale.cid).local_code=="STALE");
  CHECK(fresh.wait(expired.cid).local_code=="EXPIRY"); CHECK(fresh.wait(sys.cid).local_code=="BAD_REQUEST");
  CHECK(fresh.venue.f->http_requests()==0);
}
void lost_result_and_unconfirmed_kill() {
  Harness h; h.reject_results=true; h.start(); CHECK(h.qm.push(h.order("Mlost-1")));
  h.wait("Mlost-1"); CHECK(until([&]{return h.kill.active() && !h.manual->healthy();}));
  h.wait("",WireOp::CANCEL_ALL); CHECK(until([&]{return !h.cancel->healthy();}));
  CHECK(h.venue.orders()==1 && h.alerts>=1);

  Harness cx; cx.start(); fakex::HttpFault fault; fault.kind=fakex::HttpFault::Kind::STATUS_AFTER_ACCEPT;
  fault.method="DELETE"; cx.venue.f->add_fault(fault);
  cx.kill.trip(now_ms(),mono_ms(),"test");
  CHECK(cx.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::UNKNOWN);
  CHECK(!cx.cancel->healthy() && cx.kill.active());
  CHECK(cx.venue.f->http_requests()==1);
  // A second explicit KILL sends again; no automatic retry hides an uncertain operation.
  cx.kill.trip(now_ms(),mono_ms(),"test again");
  CHECK(cx.wait("",WireOp::CANCEL_ALL,1).outcome.kind==Kind::ACK);
  CHECK(cx.venue.f->http_requests()==2);
}
// A refusal that says the desk is wrong trips KILL and stops send-k1 (RO), with the reason kept.
// It comes back only after a cancel-all sent after the failure is confirmed and KILL is re-armed;
// an unconfirmed one keeps both senders stopped until KILL is pressed again (found on demo,
// 2026-10-10: a re-arm never cleared the stop, so every order after it was refused RO).
void hard_refusal_recovers() {
  Harness h; h.start();
  auto wrong=h.order("Mwrong-1"); wrong.create.exchange_index=1;   // the market lives on shard 0
  CHECK(h.qm.push(wrong));
  const auto refused=h.wait("Mwrong-1");
  CHECK(refused.outcome.kind==Kind::REFUSED && reject_trips_kill(refused.outcome.cls));
  CHECK(until([&]{return h.kill.active() && !h.manual->healthy();}));
  // the KILL names the write and the venue's own words
  CHECK(h.manual->fail_reason().rfind("venue refused a create (Mwrong-1) as hard_invalid, HTTP 404: not_found",0)==0 &&
        h.manual->fail_reason().find("; reconciliation required")!=std::string::npos && h.cancel->fail_reason().empty());
  CHECK(h.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::ACK);
  CHECK(until([&]{return h.kill.confirmed_gen()==h.kill.generation();}));
  CHECK(!h.manual->healthy());   // KILL still active: the operator has not re-armed
  CHECK(h.kill.rearm(now_ms(),mono_ms()+61000).empty());
  CHECK(h.manual->healthy() && h.cancel->healthy() && !h.manual->fail_reason().empty());
  CHECK(h.qm.push(h.order("Mafter-1")));
  CHECK(h.wait("Mafter-1").outcome.kind==Kind::ACK);

  // A 429 on the cancel-all was refused before processing: send-cx sends it again by itself.
  Harness rl; rl.start();
  fakex::HttpFault rate; rate.kind=fakex::HttpFault::Kind::RATE_LIMIT; rate.method="DELETE"; rl.venue.f->add_fault(rate);
  CHECK(rl.qm.push(rl.order("Mrest-1"))); CHECK(rl.wait("Mrest-1").outcome.kind==Kind::ACK);
  rl.kill.trip(now_ms(),mono_ms(),"operator");
  CHECK(rl.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::ACK);
  CHECK(rl.venue.f->http_requests()==3 && rl.cancel->healthy() && rl.cancel->fail_reason().empty());
  CHECK(rl.kill.confirmed_gen()==rl.kill.generation());
  CHECK(rl.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {return e.reserved(0,0);})==0);

  Harness u; u.start();
  fakex::HttpFault lost; lost.kind=fakex::HttpFault::Kind::STATUS_AFTER_ACCEPT; lost.method="DELETE";
  u.venue.f->add_fault(lost);
  auto bad=u.order("Mwrong-2"); bad.create.exchange_index=1; CHECK(u.qm.push(bad));
  CHECK(u.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::UNKNOWN);
  CHECK(until([&]{return !u.cancel->healthy() && !u.manual->healthy();}));
  CHECK(u.cancel->fail_reason().find("cancel-all unconfirmed")!=std::string::npos);
  CHECK(u.kill.rearm(now_ms(),mono_ms()+61000).empty());
  CHECK(!u.manual->healthy() && !u.cancel->healthy());   // re-armed, but nothing confirmed the venue is clear
  u.kill.trip(now_ms(),mono_ms(),"operator again");
  CHECK(u.wait("",WireOp::CANCEL_ALL,1).outcome.kind==Kind::ACK);
  CHECK(until([&]{return u.kill.confirmed_gen()==u.kill.generation();}));
  CHECK(!u.manual->healthy());
  CHECK(u.kill.rearm(now_ms(),mono_ms()+125000).empty());
  CHECK(u.manual->healthy() && u.cancel->healthy());
}
// send-k2 (Systematic): S creates in an order group, S cancels, the group's trigger and reset;
// never an M order. KILL's send-cx triggers every registered group after its cancel-all.
void systematic_role() {
  Harness h; GroupRegistry groups; h.cancel->set_groups(&groups);
  BoundedQueue<KalshiWriteJob> qs{64,16};
  net::PoolConfig cfg; cfg.endpoint=h.venue.ep(); cfg.name="sys-test"; cfg.connect_timeout_ms=500;
  net::SparePool ps(h.factory,cfg);
  auto reply=[&h](KalshiWriteReply r) { std::lock_guard<std::mutex> lk(h.mu); h.results.push_back(std::move(r)); return true; };
  KalshiSender::Options options; options.connect_ms=500; options.reply_ms=300;
  KalshiSender sys(KalshiSender::Role::SYSTEMATIC,qs,ps,*h.signer,h.kill,reply,[](const std::string&){},options);
  Worker ws{"send-k2-test"};
  h.start(); ps.start(); CHECK(ps.wait_ready(1,3000));
  ws.start([&](Worker& w){sys.run(w);});
  auto group=[&](const std::string& action,const std::string& id) {
    KalshiWriteJob j; j.kind=KalshiWriteJob::Kind::GROUP; j.group_action=action; j.group_id=id; j.group_limit=5000;
    j.shard=0; j.lane="M1"; j.cid="group-"+action; j.kill_generation=h.kill.generation(); return j;
  };
  CHECK(qs.push(group("create","")));
  const auto made=h.wait("group-create",WireOp::GROUP);
  CHECK(made.outcome.kind==Kind::ACK && !made.group_id.empty() && made.lane=="M1" && made.shard==0);
  const auto gid=made.group_id;
  CHECK(h.venue.f->with_engine([&](fakex::Engine& e,std::int64_t){ const auto* g=e.group(gid); return g && g->limit==5000 && !g->triggered; }));
  groups.add(gid,0);
  auto order=[&](const std::string& cid) { auto j=h.order(cid); j.create.order_group_id=gid; return j; };
  CHECK(qs.push(order("SM1-quote-1")));
  const auto placed=h.wait("SM1-quote-1");
  CHECK(placed.outcome.kind==Kind::ACK && placed.create && h.venue.has_cid("SM1-quote-1"));
  CHECK(qs.push(h.order("Mnot-mine-1")));
  CHECK(h.wait("Mnot-mine-1").local_code=="BAD_REQUEST" && !h.venue.has_cid("Mnot-mine-1"));
  KalshiWriteJob cx; cx.kind=KalshiWriteJob::Kind::CANCEL; cx.cid="SM1-quote-1"; cx.oid=placed.create ? placed.create->order_id : "x";
  cx.shard=0; cx.ticker=fx::T;
  CHECK(qs.push(cx,Lane::PRIORITY));
  CHECK(h.wait("SM1-quote-1",WireOp::CANCEL).outcome.kind==Kind::ACK);
  // trigger: the group refuses new orders (soft, never KILL) until reset
  CHECK(qs.push(group("trigger",gid)));
  CHECK(h.wait("group-trigger",WireOp::GROUP).outcome.kind==Kind::ACK);
  CHECK(qs.push(order("SM1-quote-2")));
  const auto refused=h.wait("SM1-quote-2");
  if(refused.outcome.cls!=RejectClass::SOFT_GROUP) std::fprintf(stderr,"triggered group: %s %s\n",kalshi::write_kind_name(refused.outcome.kind),refused.outcome.why.c_str());
  CHECK(refused.outcome.kind==Kind::REFUSED && refused.outcome.cls==RejectClass::SOFT_GROUP);
  CHECK(!h.kill.active() && sys.healthy());
  CHECK(qs.push(group("reset",gid)));
  CHECK(h.wait("group-reset",WireOp::GROUP).outcome.kind==Kind::ACK);
  CHECK(qs.push(order("SM1-quote-3")));
  CHECK(h.wait("SM1-quote-3").outcome.kind==Kind::ACK);
  // KILL: the cancel-all, then every group triggered; a reset is refused while KILLED
  h.kill.trip(now_ms(),mono_ms(),"test");
  CHECK(h.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::ACK);
  CHECK(until([&]{ return h.venue.f->with_engine([&](fakex::Engine& e,std::int64_t){ const auto* g=e.group(gid); return g && g->triggered; }); }));
  auto late=group("reset",gid); late.cid="group-late"; late.kill_generation=h.kill.generation();
  CHECK(qs.push(late));
  CHECK(h.wait("group-late",WireOp::GROUP).local_code=="KILLED");
  ws.request_stop(); ws.join(); ps.stop();
}
void kill_overtakes_inflight_create() {
  Harness h(1500); h.start();
  fakex::HttpFault fault; fault.kind=fakex::HttpFault::Kind::STALL_AFTER; fault.stall_ms=2000;
  h.venue.f->add_fault(fault); CHECK(h.qm.push(h.order("Mslow-1")));
  CHECK(until([&]{return h.venue.has_cid("Mslow-1");}));
  CHECK(h.qm.push(h.order("Mbehind-1")));
  h.kill.trip(now_ms(),mono_ms(),"inflight test");
  CHECK(h.wait("",WireOp::CANCEL_ALL).outcome.kind==Kind::ACK);
  CHECK(!h.find("Mslow-1")); // cancel-all answered while the Manual sender was still waiting
  CHECK(h.wait("Mslow-1").outcome.kind==Kind::UNKNOWN);
  CHECK(h.wait("Mbehind-1").local_code=="KILLED" && !h.venue.has_cid("Mbehind-1"));
  CHECK(h.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {return e.reserved(0,0);})==0);
}
}
int main() {
  std::signal(SIGPIPE,SIG_IGN);
  writes_and_faults(); permits_and_kill(); lost_result_and_unconfirmed_kill(); hard_refusal_recovers(); systematic_role(); kill_overtakes_inflight_create();
  return finish("deskd-sender");
}
