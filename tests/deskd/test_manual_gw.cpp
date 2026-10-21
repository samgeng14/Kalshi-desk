// The Manual path end to end without the web: gw admits against a REST revision and a book,
// reserves, hands the write to a real KalshiSender, and answers the waiting web thread once the
// fake Kalshi (over TLS) has: fills, a resting POST and its cancel, refusals before and at the
// venue, a lost reply (UNKNOWN blocks the market), a full sender queue, KILL.
#include <csignal>
#include <mutex>
#include <nlohmann/json.hpp>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kill.h"
#include "deskd/gw/manual_book.h"
#include "deskd/net/kalshi_sender.h"
#include "deskd/rest/state.h"
#include "net_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
// RFC 8032 test vector, not an account credential (as in test_sender).
const char* pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";

std::shared_ptr<const rest::Snapshot> snapshot(std::int64_t now) {
  auto s=std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m;
  m.ticker=fx::T; m.status="active"; m.exchange_index=0;
  m.open_ms=now-60000; m.close_ms=(now/1000+3600)*1000;
  m.price_ranges={{0,10000,100}}; m.fee_type="quadratic"; m.fee_multiplier=1; m.fee_ms=now;
  s->markets[m.ticker]=m;
  s->balances[0]=1000*10000; s->balances[2]=1000*10000;
  s->exchange.exchange_active=s->exchange.trading_active=true;
  s->exchange.shards={{0,true,true,std::nullopt},{2,true,true,std::nullopt}};
  s->clock.post_trusted=true;
  s->ready=s->account_ready=s->listing_ready=true;
  s->account_began_ms=now-2000; s->account_ms=now-1000;
  return s;
}

struct Desk {
  fx::Venue venue;
  net::ConnFactory factory{fx::demo_guard(),fx::trust_test_ca()};
  KillSwitch kill;
  BoundedQueue<KalshiWriteJob> qm{256,64}, qc{64,64};
  std::unique_ptr<KalshiSigner> signer;
  std::unique_ptr<net::SparePool> pm,pc;
  std::unique_ptr<KalshiSender> manual,cancel;
  std::unique_ptr<Gateway> gw;
  Worker wm{"send-k1-test"},wc{"send-cx-test"},wg{"gw-test"};
  std::shared_ptr<const rest::Snapshot> snap=snapshot(now_ms());
  MdBook book;
  std::atomic<bool> send_full{false};
  std::mutex mu;
  std::vector<json> published;
  Desk() {
    CHECK(venue.ok); std::string why;
    signer=KalshiSigner::from_pem("manual-gw-test",pem,"ed25519",&why); CHECK(signer!=nullptr);
    net::PoolConfig cfg; cfg.endpoint=venue.ep(); cfg.name="k1-test"; cfg.connect_timeout_ms=500;
    pm=std::make_unique<net::SparePool>(factory,cfg); cfg.name="cx-test";
    pc=std::make_unique<net::SparePool>(factory,cfg);
    gw=std::make_unique<Gateway>(kill,[]{return std::vector<PreflightRow>{};});
    auto reply=[this](KalshiWriteReply r) { return gw->post(ManualWrite{std::make_shared<const KalshiWriteReply>(std::move(r))}); };
    auto alert=[](const std::string&) {};
    KalshiSender::Options options; options.connect_ms=500; options.reply_ms=400;
    manual=std::make_unique<KalshiSender>(KalshiSender::Role::MANUAL,qm,*pm,*signer,kill,reply,alert,options);
    cancel=std::make_unique<KalshiSender>(KalshiSender::Role::CANCEL,qc,*pc,*signer,kill,reply,alert,options);
    book.ticker=fx::T; book.stale=false; book.bids={{4000,500}}; book.asks={{4200,300}};
    ManualPath path;
    path.snapshot=[this]{ return std::atomic_load(&snap); };
    path.book=[this](const std::string& tk) -> std::optional<MdBook> { if(tk==book.ticker) return book; return std::nullopt; };
    path.health=[this]{ ManualHealth h; h.read_only=false; h.public_feed=h.private_feed=h.publisher=true;
                        h.manual_sender=manual->healthy(); h.cancel_sender=cancel->healthy(); return h; };
    path.send=[this](KalshiWriteJob j) { return !send_full.load() && qm.push(std::move(j)); };
    path.cancel=[this](KalshiWriteJob j) { return qc.push(std::move(j),Lane::PRIORITY); };
    path.publish=[this](const std::string& row) { std::lock_guard<std::mutex> lk(mu); published.push_back(json::parse(row)); };
    gw->attach_manual(path);
    pm->start(); pc->start(); CHECK(pm->wait_ready(1,3000)); CHECK(pc->wait_ready(1,3000));
    wm.start([this](Worker& w){manual->run(w);}); wc.start([this](Worker& w){cancel->run(w);});
    wg.start([this](Worker& w){gw->run(w);});
  }
  ~Desk() { for(auto* w:{&wg,&wm,&wc}) w->request_stop(); for(auto* w:{&wg,&wm,&wc}) w->join(); pm->stop(); pc->stop(); }
  PlaceIntent intent(const std::string& cid,Side side,Px px,Qty n,Tif tif=Tif::IOC,int ttl=0) {
    PlaceIntent p; p.cid=cid; p.ticker=fx::T; p.side=side; p.price=px; p.count=n; p.tif=tif; p.ttl_s=ttl; return p;
  }
  OrderReply place(PlaceIntent p) {
    p.reply=std::make_shared<std::promise<OrderReply>>(); auto f=p.reply->get_future();
    CHECK(gw->post(std::move(p)));
    if(f.wait_for(std::chrono::seconds(4))!=std::future_status::ready) { CHECK(!"gw answered"); return {"TIMEOUT","",""}; }
    return f.get();
  }
  CancelReply cancel_cid(const std::string& cid) {
    CancelIntent c; c.scope=CancelIntent::Scope::CID; c.cid=cid;
    c.reply=std::make_shared<std::promise<CancelReply>>(); auto f=c.reply->get_future();
    CHECK(gw->post(std::move(c))); CHECK(f.wait_for(std::chrono::seconds(2))==std::future_status::ready); return f.get();
  }
  template<class F> auto on_gw(F fn) {
    std::promise<decltype(fn())> p; auto f=p.get_future();
    CHECK(gw->post(Probe{[&]{ p.set_value(fn()); }})); return f.get();
  }
  json last_row(const std::string& cid) {
    std::lock_guard<std::mutex> lk(mu);
    for(auto it=published.rbegin();it!=published.rend();++it) if((*it)["cid"]==cid) return *it;
    return json();
  }
  bool until(const std::function<bool()>& fn) {
    const auto end=mono_ms()+4000;
    do { if(fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while(mono_ms()<end);
    return fn();
  }
};

void fills_and_rests() {
  Desk d;
  // Take: IOC buy 2 at the 0.42 ask (3 offered) fills at once.
  auto r=d.place(d.intent("Mgwtest-1",Side::BUY,4200,200));
  CHECK(r.code.empty()); auto ord=json::parse(r.ord.empty() ? "{}" : r.ord);
  CHECK(ord["st"]=="FILL" && ord["filled"]==2 && ord["left"]==0 && !ord["oid"].is_null());
  CHECK(ord["t"]["sent"].is_number() && ord["t"]["ack"].is_number());
  // Post: a resting buy at 0.41 for 60 s, then cancelled.
  r=d.place(d.intent("Mgwtest-2",Side::BUY,4100,100,Tif::POST,60));
  CHECK(r.code.empty()); ord=json::parse(r.ord.empty() ? "{}" : r.ord);
  CHECK(ord["st"]=="WORK" && ord["left"]==1 && ord["exp_ms"].is_number());
  CHECK(d.venue.has_cid("Mgwtest-2"));
  auto cx=d.cancel_cid("Mgwtest-2"); CHECK(cx.cids==std::vector<std::string>{"Mgwtest-2"});
  CHECK(d.until([&]{ return d.last_row("Mgwtest-2")["st"]=="CXL"; }));
  CHECK(d.last_row("Mgwtest-2")["why"]=="USER");
  // The cancelled POST holds nothing; the IOC's 2 filled contracts keep their cost until the
  // private feed delivers the executions (a cumulative fill_count never books a position).
  CHECK(d.on_gw([&]{ return d.gw->manual_book()->ledger().find("Mgwtest-2")->reservation(); })==0);
  CHECK(d.on_gw([&]{ return d.gw->manual_book()->ledger().reserved(0); })==4200LL*200);
  CHECK(d.gw->stats().manual_sent==2 && d.gw->stats().manual_cancels==1);
  // A crossing POST is sent post_only; the venue's refusal ends it CXL POST_CROSS, not an error.
  r=d.place(d.intent("Mgwtest-3",Side::BUY,4200,100,Tif::POST,15));
  CHECK(r.code.empty()); ord=json::parse(r.ord.empty() ? "{}" : r.ord);
  CHECK(ord["st"]=="CXL" && ord["why"]=="POST_CROSS");
}

void refusals() {
  Desk d;
  auto r=d.place(d.intent("Mgwtest-1",Side::BUY,4500,100));   // three ticks through the 0.42 ask
  CHECK(r.code=="PRICE_BAND" && r.ord.empty());
  auto o=d.intent("Mgwtest-2",Side::BUY,4500,100); o.override_guard=true;
  r=d.place(o); CHECK(r.code.empty());                      // Shift+click sends it
  r=d.place(d.intent("Mgwtest-2",Side::BUY,4200,100)); CHECK(r.code=="DUP_CID");
  r=d.place(d.intent("Mgwtest-3",Side::BUY,4200,150)); CHECK(r.code=="BAD_QTY");   // whole contracts here
  auto sys=d.intent("Sx-1",Side::BUY,4200,100); sys.owner=Owner::SYS; sys.lane="x";
  r=d.place(sys); CHECK(r.code=="RO");                      // no Systematic path yet
  // A full send-k1 inbox: refused RO, the order ends REJ and frees its reservation at once.
  const auto before=d.on_gw([&]{ return d.gw->manual_book()->shard_free(0); });
  d.send_full=true;
  r=d.place(d.intent("Mgwtest-4",Side::BUY,4000,100)); CHECK(r.code=="RO");
  CHECK(json::parse(r.ord.empty() ? "{}" : r.ord)["st"]=="REJ");
  d.send_full=false;
  CHECK(d.on_gw([&]{ return d.gw->manual_book()->shard_free(0); })==before);
  // $0.50 on shard 0, less the unreflected override order's worst case, cannot pay for 0.42 plus
  // its fee allowance, though shard 2 has $1000: SHARD_CASH before anything is sent.
  auto s=std::make_shared<rest::Snapshot>(*d.snap); s->balances[0]=5000; std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  r=d.place(d.intent("Mgwtest-5",Side::BUY,4200,100)); CHECK(r.code=="SHARD_CASH");
  // KILL: refused before anything is sent.
  d.kill.trip(now_ms(),mono_ms(),"test");
  r=d.place(d.intent("Mgwtest-6",Side::BUY,4000,100)); CHECK(r.code=="KILLED");
}

void unknown_blocks() {
  Desk d;
  fakex::HttpFault fault; fault.kind=fakex::HttpFault::Kind::DROP_AFTER_ACCEPT; d.venue.f->add_fault(fault);
  auto r=d.place(d.intent("Mgwtest-1",Side::BUY,4000,100,Tif::POST,60));
  CHECK(r.code=="EXCHANGE"); auto ord=json::parse(r.ord.empty() ? "{}" : r.ord);
  CHECK(ord["st"]=="PEND" && ord["why"]=="");
  CHECK(d.venue.has_cid("Mgwtest-1"));   // it does rest at the venue: the reservation stays
  r=d.place(d.intent("Mgwtest-2",Side::BUY,4000,100)); CHECK(r.code=="STALE");   // the market is blocked
  CHECK(r.msg.find("order Mgwtest-1 ")==0);                                        // and says by which order
  // a cancel of the uncertain order is recorded; nothing can be sent without its order id
  auto cx=d.cancel_cid("Mgwtest-1"); CHECK(cx.cids.size()==1);
  CHECK(d.gw->stats().manual_cancels==0);
}

// The private feed is not wired here, so only the account read can settle an unknown order: its
// own row (it rested) does at once; its absence from a complete read begun after its deadline
// (it never reached the book) does then, not before. Either way the market takes orders again
// (2026-10-13 on demo: an order stuck PEND refused the market's every later order as STALE).
void unknown_settled_by_account_read() {
  Desk d;
  fakex::HttpFault fault; fault.kind=fakex::HttpFault::Kind::DROP_AFTER_ACCEPT; d.venue.f->add_fault(fault);
  auto r=d.place(d.intent("Mgwset-1",Side::BUY,4000,100,Tif::POST,60));
  CHECK(r.code=="EXCHANGE");
  CHECK(d.place(d.intent("Mgwset-2",Side::BUY,4000,100)).code=="STALE");
  auto venue_row=[&](const std::string& cid) {   // the order's row as the account read gets it
    return d.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) {
      kalshi::OrderRow out;
      for (const fakex::Order* o : e.user_orders())
        if (o->client_order_id==cid) {
          out.order_id=o->order_id; out.client_order_id=o->client_order_id; out.ticker=o->ticker;
          out.yes_price=o->price; out.initial=o->initial; out.filled=o->filled; out.remaining=o->remaining;
          out.exchange_index=o->exchange_index; out.updated_ms=o->updated_ms;
        }
      return out;
    });
  };
  auto row=venue_row("Mgwset-1");
  CHECK(!row.order_id.empty());
  auto s=std::make_shared<rest::Snapshot>(*snapshot(now_ms()));
  s->orders.push_back(row); s->account_ms=now_ms(); ++s->revision;
  std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  CHECK(d.until([&]{ return d.last_row("Mgwset-1")["st"]=="WORK"; }));
  CHECK(d.last_row("Mgwset-1")["oid"]==row.order_id);
  r=d.place(d.intent("Mgwset-3",Side::BUY,4000,100));   // an IOC that does not cross: unblocked
  CHECK(r.code.empty());

  // cancelled while its outcome was unknown: the cancel waits for the order id, and goes the
  // moment the account read binds it (review 2026-10-14: it never went, and the order rested on)
  d.venue.f->add_fault(fault);
  r=d.place(d.intent("Mgwset-6",Side::BUY,3900,100,Tif::POST,60));
  CHECK(r.code=="EXCHANGE");
  auto cx=d.cancel_cid("Mgwset-6"); CHECK(cx.cids.size()==1);
  CHECK(d.gw->stats().manual_cancels==0);
  s=std::make_shared<rest::Snapshot>(*snapshot(now_ms()));
  s->orders.push_back(venue_row("Mgwset-6")); s->account_ms=now_ms()+1; s->revision=5;
  std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  CHECK(d.until([&]{ return d.gw->stats().manual_cancels==1; }));
  CHECK(d.until([&]{ return d.last_row("Mgwset-6")["st"]=="CXL"; }));
  CHECK(d.until([&]{ return d.venue.f->with_engine([](fakex::Engine& e, std::int64_t) {
    for (const fakex::Order* o : e.user_orders())
      if (o->client_order_id=="Mgwset-6") return o->status!=fakex::OrdStatus::RESTING;
    return false; }); }));

  // never reached the book: an IOC whose request the venue dropped unread
  fault.kind=fakex::HttpFault::Kind::DROP_BEFORE_REPLY; d.venue.f->add_fault(fault);
  r=d.place(d.intent("Mgwset-4",Side::BUY,4000,100));
  CHECK(r.code=="EXCHANGE" && !d.venue.has_cid("Mgwset-4"));
  const auto t=d.last_row("Mgwset-4")["t"]["sent"];
  const std::int64_t sent=t.is_number() ? t.get<std::int64_t>() : 0;
  CHECK(sent>0);
  // a read that began too soon after it proves nothing
  s=std::make_shared<rest::Snapshot>(*snapshot(now_ms()));
  s->orders_from_ms=sent-60000; s->fills_from_ms=sent-60000;
  s->account_began_ms=sent+1000; s->account_ms=sent+2000; s->revision=10;
  std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  CHECK(d.place(d.intent("Mgwset-5",Side::BUY,4000,100)).code=="STALE");
  // one begun after its deadline (an IOC's: two minutes) and complete, without it
  s=std::make_shared<rest::Snapshot>(*s);
  s->account_began_ms=sent+121000; s->account_ms=sent+122000; s->revision=11;
  std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  CHECK(d.until([&]{ return d.last_row("Mgwset-4")["st"]=="CXL"; }));
  CHECK(d.last_row("Mgwset-4")["why"]=="IOC_REST");
}
// Plan section 3: a client cancel at the expiry + 1.5 s backs up the exchange's own ("nothing
// lives 2 s past expiry"). A venue whose expiry runs a minute late leaves a 5 s post working; gw
// cancels it, once, about 1.5 s after the expiry, and it ends EXP (why EXPIRY).
void expiry_backup() {
  Desk d;
  d.venue.f->with_engine([](fakex::Engine& e, std::int64_t) { e.config().expiry_delay_ms = 60000; });
  auto r=d.place(d.intent("Mgwexp-1",Side::BUY,3900,100,Tif::POST,5));
  CHECK(r.code.empty());
  auto ord=json::parse(r.ord.empty() ? "{}" : r.ord);
  CHECK(ord["st"]=="WORK" && ord["exp_ms"].is_number());
  const std::int64_t exp=ord["exp_ms"].is_number() ? ord["exp_ms"].get<std::int64_t>() : 0;
  const auto end=mono_ms()+10000;
  auto ended=[&]{ const auto st=d.last_row("Mgwexp-1")["st"]; return st=="EXP" || st=="CXL"; };
  while(mono_ms()<end && !ended()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto row=d.last_row("Mgwexp-1");
  // ended by our cancel at its expiry: EXP, why EXPIRY (the contract's word for it)
  CHECK(row["st"]=="EXP" && row["why"]=="EXPIRY");
  const std::int64_t done=row["t"]["done"].is_number() ? row["t"]["done"].get<std::int64_t>() : 0;
  CHECK(done>=exp+1500 && done<exp+2000);   // nothing lives 2 s past its expiry
  CHECK(d.gw->stats().manual_cancels==1);
  CHECK(!d.venue.f->with_engine([](fakex::Engine& e, std::int64_t) {
    for (const fakex::Order* o : e.user_orders())
      if (o->client_order_id=="Mgwexp-1" && o->status==fakex::OrdStatus::RESTING) return true;
    return false; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK(d.gw->stats().manual_cancels==1);   // once
}
// The private feed lost an update (it is not wired here at all): a resting post is taken in full
// by the street. The account read shows the order executed, so gw ends it FILL and frees its
// reservation; its fill is still missing 5 s on, so the next read's fill row is taken (avg price),
// once, and a third read changes nothing.
void missed_fill_from_account_read() {
  Desk d;
  auto r=d.place(d.intent("Mgwmiss-1",Side::BUY,4100,100,Tif::POST,60));   // the best bid, inside 0.40/0.42
  CHECK(r.code.empty());
  CHECK(d.last_row("Mgwmiss-1")["st"]=="WORK");
  const auto took=d.venue.f->with_engine([](fakex::Engine& e,std::int64_t now) { return e.street_take(fx::T,fakex::Side::ASK,4100,100,now); });
  CHECK(took==100);
  auto rows=[&] {
    return d.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      std::pair<kalshi::OrderRow,std::vector<kalshi::FillRow>> out;
      for (const fakex::Order* o : e.user_orders())
        if (o->client_order_id=="Mgwmiss-1") {
          auto& x=out.first;
          x.order_id=o->order_id; x.client_order_id=o->client_order_id; x.ticker=o->ticker; x.yes_price=o->price;
          x.initial=o->initial; x.filled=o->filled; x.remaining=o->remaining; x.exchange_index=o->exchange_index;
          x.updated_ms=o->updated_ms;
          x.status=o->remaining==0 ? kalshi::OrderStatus::EXECUTED : kalshi::OrderStatus::RESTING;
        }
      for (const auto& f : e.fills())
        if (f.client_order_id=="Mgwmiss-1") {
          kalshi::FillRow y; y.fill_id=f.fill_id; y.trade_id=f.trade_id; y.order_id=f.order_id; y.ticker=f.ticker;
          y.side=kalshi::BookSide::BID; y.count=f.count; y.yes_price=f.yes_px; y.taker=f.taker; y.fee=f.fee;
          y.ts_ms=f.ts_ms; y.exchange_index=f.exchange_index;
          out.second.push_back(y);
        }
      return out;
    });
  };
  const auto [row,fills]=rows();
  CHECK(row.status==kalshi::OrderStatus::EXECUTED && fills.size()==1);
  auto publish=[&](std::int64_t rev) {   // an account read begun now, after the order was sent
    auto s=std::make_shared<rest::Snapshot>(*snapshot(now_ms()));
    s->orders.push_back(row); s->fills=fills; s->account_began_ms=now_ms(); s->account_ms=now_ms()+1; s->revision=rev;
    std::atomic_store(&d.snap,std::shared_ptr<const rest::Snapshot>(s));
  };
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // the read must begin past the send
  publish(20);
  CHECK(d.until([&]{ return d.last_row("Mgwmiss-1")["st"]=="FILL"; }));
  CHECK(d.last_row("Mgwmiss-1")["avg"].is_null());   // its fill is not in the ledger yet
  std::this_thread::sleep_for(std::chrono::milliseconds(5200));
  publish(21);
  CHECK(d.until([&]{ return d.last_row("Mgwmiss-1")["avg"]=="0.4100"; }));
  const auto rev=d.last_row("Mgwmiss-1")["rev"];
  publish(22);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(d.last_row("Mgwmiss-1")["rev"]==rev);   // the same fill again is a duplicate: nothing moves
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);   // as deskd's runtime does process-wide (app/runtime.h)
  fills_and_rests(); refusals(); unknown_blocks(); unknown_settled_by_account_read(); expiry_backup(); missed_fill_from_account_read();
  return finish("deskd-manual-gw");
}
