#include "check.h"
#include "deskd/gw/manual_admission.h"
#include "deskd/gw/order_ledger.h"
#include "deskd/rest/state.h"
#include <limits>
using namespace deskd;
constexpr std::int64_t now=1791500000123LL;
struct Fixture {
  PlaceIntent p;
  kalshi::MarketRow m;
  MdBook b;
  ManualContext c;
  Fixture() {
    p.cid="Msession-1"; p.ticker="TEST-A"; p.price=4200; p.count=1000;
    m.ticker=p.ticker; m.status="active"; m.exchange_index=2;
    m.open_ms=now-1000; m.close_ms=(now/1000+3600)*1000;
    m.price_ranges={{0,10000,100}}; m.fee_type="quadratic"; m.fee_multiplier=1; m.fee_ms=now;
    b.ticker=p.ticker; b.stale=false; b.bids={{4000,1000}}; b.asks={{4200,1000}};
    c.now_ms=now; c.market=&m; c.book=&b;
    c.read_only=false; c.account_ready=c.public_ready=c.private_ready=c.clock_trusted=true;
    c.publisher_healthy=c.ledger_healthy=c.manual_sender_healthy=c.cancel_sender_healthy=true;
    c.trading_active=c.shard_active=true; c.free=c.shard_free=500000000;
  }
  ManualAdmission admit() { return admit_manual(p,c); }
};
static void basics() {
  Fixture f; auto a=f.admit(); CHECK(a.ok()); CHECK(a.wire.client_order_id==f.p.cid);
  CHECK(a.wire.exchange_index==2); CHECK(a.wire.stp==kalshi::Stp::MAKER);
  CHECK(a.wire.price==4200); CHECK(a.wire.count==1000); CHECK(a.wire.tif==kalshi::TimeInForce::IOC);
  CHECK(a.collateral==4200000); CHECK(a.fee_check>=170520 && a.fee_check<=181000);
  CHECK(a.intent.t_recv_ms==now); CHECK(f.p.t_recv_ms==0); // pure: no mutation of the input
  OrderLedger ledger; CHECK(ledger.insert(a.intent,2,a.collateral));
  CHECK(ledger.reserved(2)==a.collateral); // fees checked, never represented as working collateral
  f.p.slip_ticks=2; CHECK(f.admit().wire.price==4200); // cap already includes UI slippage
  f.p.count=1; CHECK(f.admit().code=="BAD_QTY"); // whole contracts unless the market says otherwise
  f.m.fractional=false; CHECK(f.admit().code=="BAD_QTY");
  f.m.fractional=true; CHECK(f.admit().wire.count==1); CHECK(f.admit().collateral==4200);
  f.p.side=Side::SELL; f.p.price=4000; CHECK(f.admit().collateral==6000);
  f.p.tif=Tif::FOK; CHECK(f.admit().wire.tif==kalshi::TimeInForce::FOK);
  f.m.status="open"; CHECK(f.admit().ok());
  f.c.now_ms+=3600000; CHECK(f.admit().code=="MARKET_CLOSED");
}
static void gates() {
  for(auto member:{&ManualContext::account_ready,&ManualContext::public_ready,&ManualContext::private_ready}) {
    Fixture f; f.c.*member=false; CHECK(f.admit().code=="STALE");
    f.p.override_guard=true; CHECK(f.admit().code=="STALE");
  }
  for(auto member:{&ManualContext::publisher_healthy,&ManualContext::ledger_healthy,
                  &ManualContext::manual_sender_healthy,&ManualContext::cancel_sender_healthy}) {
    Fixture f; f.c.*member=false; CHECK(f.admit().code=="RO");
  }
  Fixture f;
  f.c.unknown_order=true; CHECK(f.admit().code=="STALE"); f.c.unknown_order=false;
  f.c.foreign_order=true; CHECK(f.admit().code=="RO"); f.c.foreign_order=false;
  f.c.killed=true; CHECK(f.admit().code=="KILLED"); f.c.killed=false;
  f.c.duplicate_cid=true; CHECK(f.admit().code=="DUP_CID"); f.c.duplicate_cid=false;
  f.c.read_only=true; CHECK(f.admit().code=="RO"); f.c.read_only=false;
  f.c.shard_active=false; CHECK(f.admit().code=="MARKET_CLOSED"); f.c.shard_active=true;
  f.c.trading_active=false; CHECK(f.admit().code=="MARKET_CLOSED"); f.c.trading_active=true;
  f.b.stale=true; CHECK(f.admit().code=="STALE"); f.b.stale=false;
  f.b.recv_ms=now-900000; CHECK(f.admit().ok()); // quiet books are live via WS heartbeat
  f.m.fee_ms=now-rest::kFeeFreshMs; CHECK(f.admit().code=="RO"); f.m.fee_ms=now;
  f.m.fee_multiplier=std::numeric_limits<double>::quiet_NaN(); CHECK(f.admit().code=="RO");
  f.m.fee_multiplier=1; f.m.fee_type="unrecognized"; CHECK(f.admit().code=="RO");
  f.m.fee_type="quadratic_with_maker_fees"; CHECK(f.admit().ok());
  f.m.exchange_index.reset(); CHECK(f.admit().code=="RO"); f.m.exchange_index=0;
  f.m.open_ms=now+1; CHECK(f.admit().code=="MARKET_CLOSED"); f.m.open_ms=now;
  f.m.close_ms.reset(); CHECK(f.admit().code=="MARKET_CLOSED");
}
static void expiry() {
  Fixture f; f.p.tif=Tif::POST; f.p.price=4100;
  CHECK(f.admit().code=="BAD_REQUEST");
  for(int ttl:{5,15,60,300,1800}) {
    f.p.ttl_s=ttl; auto a=f.admit(); CHECK(a.ok());
    CHECK(a.intent.exp_ms==(now/1000+ttl)*1000); CHECK(a.intent.ttl_s==0);
    CHECK(a.wire.expiration_s==a.intent.exp_ms/1000); CHECK(a.wire.post_only && a.wire.cancel_on_pause);
    CHECK(a.wire.tif==kalshi::TimeInForce::GTC);
  }
  f.p.ttl_s=2; CHECK(f.admit().code=="BAD_REQUEST"); f.p.ttl_s=0;
  f.p.exp_ms=*f.m.close_ms; CHECK(f.admit().ok()); // until-close may be longer than 30 minutes
  f.p.exp_ms+=1000; CHECK(f.admit().code=="BAD_REQUEST"); f.p.exp_ms=*f.m.close_ms;
  f.p.ttl_s=15; CHECK(f.admit().code=="BAD_REQUEST"); f.p.ttl_s=0;
  f.p.exp_ms=now+5000; CHECK(f.admit().code=="BAD_REQUEST"); // not whole seconds
  f.p.exp_ms=(now/1000+5)*1000; CHECK(f.admit().code=="BAD_REQUEST"); // 4.877s absolute
  f.p.exp_ms=(now/1000+6)*1000; CHECK(f.admit().ok());
  f.c.clock_trusted=false; CHECK(f.admit().code=="STALE");
  f.p.exp_ms=0; f.p.tif=Tif::IOC; CHECK(f.admit().ok()); // bad clock blocks POST only
  f.p.ttl_s=5; CHECK(f.admit().code=="BAD_REQUEST");
}
static void prices() {
  Fixture f; f.p.price=4400; CHECK(f.admit().ok());
  f.p.price=4500; CHECK(f.admit().code=="PRICE_BAND");
  f.p.override_guard=true; CHECK(f.admit().ok()); f.p.override_guard=false;
  f.p.price=4150; CHECK(f.admit().code=="OFF_GRID");
  f.p.side=Side::SELL; f.p.price=3800; CHECK(f.admit().ok());
  f.p.price=3700; CHECK(f.admit().code=="PRICE_BAND");
  f.m.price_ranges={{0,1000,10},{1000,9000,100},{9000,10000,10}};
  f.b.bids={{990,100}}; f.b.asks={{1000,100}}; f.p.side=Side::BUY;
  f.p.price=1200; CHECK(f.admit().ok()); f.p.price=1300; CHECK(f.admit().code=="PRICE_BAND");
  f.p.side=Side::SELL; f.p.price=970; CHECK(f.admit().ok());
  f.p.price=960; CHECK(f.admit().code=="PRICE_BAND");
  f.b.bids={{9000,100}}; f.b.asks={{9010,100}};
  f.p.side=Side::BUY; f.p.price=9030; CHECK(f.admit().ok());
  f.p.price=9040; CHECK(f.admit().code=="PRICE_BAND");
  f.p.side=Side::SELL; f.p.price=8800; CHECK(f.admit().ok());
  f.p.price=8700; CHECK(f.admit().code=="PRICE_BAND");
  f.p.price=8800; f.m.price_ranges.clear(); CHECK(f.admit().code=="RO");
  f.m.price_ranges={{0,10000,0}}; CHECK(f.admit().code=="RO");
  f.m.price_ranges={{0,6000,100},{5000,10000,100}}; CHECK(f.admit().code=="RO");
}
// A whole-contract market reduces a fractional position in whole contracts only.
static void r0(Fixture& f,Qty expect) {
  auto r=f.admit(); CHECK(r.ok()); CHECK(r.intent.count==expect); CHECK(r.wire.count==expect); CHECK(r.collateral==0);
}
static void cash_and_reducing() {
  Fixture f; const auto a=f.admit();
  f.c.free=a.required()-1; CHECK(f.admit().code=="NO_CASH"); f.c.free=a.required();
  f.c.shard_free=a.required()-1; CHECK(f.admit().code=="SHARD_CASH");
  f.c.shard_free=a.required(); CHECK(f.admit().ok());
  f.c.free=500000000; f.c.shard_free=3550000; f.p.price=5200; f.p.count=1500; f.p.override_guard=true;
  CHECK(f.admit().code=="SHARD_CASH"); // user's example: venue cash can't replace shard collateral
  f.c.shard_free=500000000; f.p.count=1000; f.p.price=4200; f.p.override_guard=false;
  f.c.position=-350; CHECK(f.admit().collateral==2730000); // collateral is exact on any market
  f.p.reduce_only=true; r0(f,300); f.c.reducing_committed=300; CHECK(f.admit().code=="REDUCE");
  f.c.reducing_committed=0; f.p.reduce_only=false; f.m.fractional=true; CHECK(f.admit().collateral==2730000); // exact 3.5 reducing + 6.5 opening
  f.c.reducing_committed=150; CHECK(f.admit().collateral==3360000);
  f.p.reduce_only=true; auto r=f.admit(); CHECK(r.ok()); CHECK(r.intent.count==200); CHECK(r.collateral==0);
  CHECK(r.wire.reduce_only && r.wire.count==200); CHECK(f.p.count==1000);
  f.c.reducing_committed=350; CHECK(f.admit().code=="REDUCE");
  f.c.reducing_committed=0; f.c.position=350; CHECK(f.admit().code=="REDUCE");
  f.p.side=Side::SELL; f.p.price=4000; r=f.admit(); CHECK(r.ok()); CHECK(r.intent.count==350);
  f.c.shard_free=r.fee_check-1; CHECK(f.admit().code=="SHARD_CASH");
  f.c.shard_free=r.fee_check; CHECK(f.admit().ok()); // no entry capital needed to reduce
  f.p.tif=Tif::FOK; CHECK(f.admit().code=="BAD_REQUEST");
}
static void fees_and_bad_inputs() {
  Fixture f; f.p.override_guard=true; f.p.price=9000;
  CHECK(f.admit().fee_check==185000); // 0.50 fills more expensive than 0.90 limit
  f.p.side=Side::SELL; f.p.price=1000; CHECK(f.admit().fee_check==185000);
  f.p.side=Side::BUY; CHECK(f.admit().fee_check==73000);
  f.m.fee_multiplier=2; CHECK(f.admit().fee_check==136000);
  f.p.cid="Sfoo-123"; CHECK(f.admit().code=="BAD_REQUEST"); f.p.cid="M12345";
  f.p.owner=Owner::SYS; CHECK(f.admit().code=="BAD_REQUEST"); f.p.owner=Owner::MAN;
  f.p.count=0; CHECK(f.admit().code=="BAD_QTY"); f.p.count=1000000001;
  CHECK(f.admit().code=="BAD_QTY"); f.p.count=150; CHECK(f.admit().code=="BAD_QTY");
  f.m.fractional=true; f.p.count=1;
  f.c.position=std::numeric_limits<Qty>::min(); CHECK(f.admit().code=="RO");
  f.c.position=0; f.c.now_ms=std::numeric_limits<std::int64_t>::max(); CHECK(f.admit().code=="BAD_REQUEST");
}
int main() {
  basics(); gates(); expiry(); prices(); cash_and_reducing(); fees_and_bad_inputs();
  return finish("deskd-manual-admission");
}
