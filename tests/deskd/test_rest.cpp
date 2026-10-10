#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>
#include <fstream>
#include <thread>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/build_paths.h"
#include "deskd/net/guard.h"
#include "deskd/rest/state.h"
#include "deskd/web/contract.h"
#include "fake_kalshi.h"

using namespace deskd;
using json = nlohmann::json;
namespace {
std::unique_ptr<KalshiSigner> key() {
  auto* k = EVP_PKEY_Q_keygen(nullptr,nullptr,"ED25519");
  auto* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio,k,nullptr,nullptr,0,nullptr,nullptr);
  char* p = nullptr; const auto n = BIO_get_mem_data(bio,&p);
  const std::string pem(p,n); BIO_free(bio); EVP_PKEY_free(k);
  std::string why; return KalshiSigner::from_pem("rest-test",pem,"ed25519",&why);
}
void clock_checks() {
  const auto t = *kalshi::parse_time_ms("2026-10-07T00:00:00Z");
  rest::ClockCheck c;
  CHECK(!c.estimate(t).offset_ms);
  c.observe("Wed, 07 Oct 2026 00:00:00 GMT",t+100,t+120,20);
  CHECK(!c.estimate(t+120).post_trusted); // one Date sample cannot certify <500ms
  c.observe("Wed, 07 Oct 2026 00:00:00 GMT",t+850,t+870,20);
  auto e = c.estimate(t+870);
  CHECK(e.post_trusted && e.samples == 2 && e.uncertainty_ms && *e.uncertainty_ms < 150);
  CHECK(!c.estimate(t+70000).post_trusted);
  c.observe("Wed, 07 Oct 2026 00:00:01 GMT",t+900,t+2000,20);
  CHECK(!c.estimate(t+2000).offset_ms); // local clock stepped
  c.observe("bad date",t+2100,t+2110,10);
  CHECK(!c.estimate(t+2110).post_trusted);
}
void series_fees() {
  std::string why;
  json j={{"series",{{"ticker","TEST"},{"category","Sports"},{"fee_type","quadratic_with_maker_fees"},
                     {"fee_multiplier",1.5}}}};
  auto parsed=kalshi::parse_series(j.dump(),&why);
  CHECK(parsed && parsed->fee_multiplier==1.5 && parsed->category=="Sports");
  CHECK(!kalshi::build_get_series("TEST").auth && kalshi::build_get_series("TEST").path=="/trade-api/v2/series/TEST");
  j["series"]["fee_multiplier"]="1.5"; CHECK(!kalshi::parse_series(j.dump(),&why));
  j["series"]["fee_multiplier"]=-1; CHECK(!kalshi::parse_series(j.dump(),&why));
  j["series"]["fee_multiplier"]=nullptr; CHECK(!kalshi::parse_series(j.dump(),&why));
  j["series"]["fee_multiplier"]=0; CHECK(kalshi::parse_series(j.dump(),&why));
  kalshi::MarketRow m; m.fee_type="quadratic"; m.fee_multiplier=0; m.fee_ms=now_ms();
  CHECK(rest::fee_ready(m,m.fee_ms));
  CHECK(rest::market_json(m,false,false,m.fee_ms)["fee"]["type"]=="FREE");
  CHECK(!rest::fee_ready(m,m.fee_ms+rest::kFeeFreshMs));
  CHECK(!rest::fee_ready(m,m.fee_ms-1));
  m.fee_type="future_unknown_model"; CHECK(!rest::fee_ready(m,m.fee_ms));
}
void discovery(json& capture) {
  const auto t = *kalshi::parse_time_ms("2026-10-07T18:00:00Z");
  rest::Snapshot s;
  for (int i=0; i<5; ++i) {
    kalshi::MarketRow m;
    m.ticker = "KXHIGHNY-26OCT07-B"+std::to_string(70+i);
    m.event_ticker="KXHIGHNY-26OCT07"; m.title="New York temperature";
    m.status="active"; m.open_ms=t-1000; m.close_ms=t+16*3600000;
    m.yes_bid=4000; m.yes_ask=4200; m.volume_24h=10000+i;
    m.price_ranges={{0,10000,100}};
    s.markets[m.ticker]=m;
  }
  auto m = s.markets.begin()->second;
  CHECK(rest::outcome_date(m)=="2026-10-07"); // close is tomorrow in ET
  CHECK(rest::market_json(m,false,true,t)["shard"].is_null());
  m.ticker="KXNBA-26OCT07-A"; m.event_ticker="KXNBA-26OCT07"; m.category="Sports";
  m.expected_expiration_ms=t+3600000; s.markets[m.ticker]=m;
  auto j=rest::universe(s,{{"day","T"},{"limit",2}},t);
  CHECK(j["total"]==6 && j["rows"].size()==2 && j["cats"]["WEATHER"]==5);
  CHECK(j["rows"][0]["cat"]=="SPORTS");
  capture["responses"].push_back({{"key","GET /api/universe"},{"status",200},{"json",j}});
  const auto same=rest::universe(s,{{"day","T"},{"grev",j["grev"]}},t);
  CHECK(same["groups"].is_null());
  CHECK(rest::universe(s,{{"day","T1"}},t)["total"]==0);
  CHECK(rest::universe(s,{{"day","T"},{"v","P"}},t)["total"]==0);
  const auto filtered=rest::universe(s,{{"day","T"},{"cat","WEATHER"},{"offset",4},{"limit",2}},t);
  CHECK(filtered["total"]==5 && filtered["rows"].size()==1);
  {   // a model's fair: the row carries it and the better take's edge after the taker fee
    kalshi::MarketRow fm; fm.fee_multiplier=1.0;
    // buy at 0.48 against 0.50: 2c less 0.07 x 0.48 x 0.52 = 1.7472c, rounded up to 1.75c
    CHECK(rest::take_edge(fm,Px{5100},Px{4800},5000)==std::optional<Money>(25));
    CHECK(rest::take_edge(fm,Px{5300},Px{5600},5000)==std::optional<Money>(300-175));   // selling at 0.53
    CHECK(!rest::take_edge(fm,std::nullopt,std::nullopt,5000));
    auto with=rest::universe(s,{{"day","T"},{"cat","SPORTS"}},t,[&](const std::string& tk) -> std::optional<Px> {
      return tk==m.ticker ? std::optional<Px>(5000) : std::nullopt; });
    CHECK(with["rows"].size()==1 && with["rows"][0]["fair"]=="0.5000");
    capture["responses"].push_back({{"key","GET /api/universe"},{"status",200},{"json",with}});
  }
  for (const auto& p:s.markets)
    capture["events"].push_back({{"type","mkt"},{"seq",1},{"ts_ms",t},{"data",rest::market_json(p.second,false,true,t)}});
  kalshi::OrderRow o;
  o.order_id="oid-foreign"; o.ticker=m.ticker; o.initial=100; o.remaining=100; o.yes_price=4500;
  auto order=rest_order_json(o,t);
  CHECK(order["own"]=="EXT" && order["cid"].get<std::string>().rfind("X-",0)==0);
  capture["events"].push_back({{"type","ord"},{"seq",1},{"ts_ms",t},{"data",order}});
  kalshi::PositionRow p; p.ticker=m.ticker; p.position=-150; p.exposure=900000;
  const auto position=rest_position_json(p);
  CHECK(position["avg"]=="0.4000" && position["maxl"]=="0.9000" && position["if_no"]=="0.6000");
  capture["events"].push_back({{"type","pos"},{"seq",1},{"ts_ms",t},{"data",position}});
}
void reconcile() {
  fakex::FakeConfig cfg; cfg.rate_limits=false;
  fakex::FakeKalshi fake(cfg);
  fake.with_engine([](fakex::Engine& e,std::int64_t now) {
    for (int i=0;i<5;++i) {
      fakex::MarketSpec m; m.ticker="TEST-"+std::to_string(i); m.exchange_index=i%2;
      m.fee_type=fakex::FeeType::QUADRATIC_WITH_MAKER_FEES; m.fee_mult_milli=1500;
      m.close_ms=now+3600000; e.add_market(m,now);
    }
    e.set_cash(0,0,500*fakex::kMicrosPerDollar,now);
    e.set_cash(0,1,75*fakex::kMicrosPerDollar,now);
  });
  std::string why; CHECK(fake.start(&why));
  auto signer=key(); CHECK(signer!=nullptr);
  SendGuard guard(false,nullptr); net::ConnFactory factory(guard);
  const auto endpoint=net::Endpoint::parse("http://127.0.0.1:"+std::to_string(fake.port())+"/trade-api/v2",&why);
  rest::Options opts; opts.page_size=2; opts.request_gap_ms=0; opts.backoff_429_ms=20;
  rest::State state(factory,*endpoint,signer.get(),{},opts);
  CHECK(state.refresh_once());
  auto s=state.snapshot();
  if (!s->ready) std::fprintf(stderr,"REST: %s\n",s->error.c_str());
  CHECK(s->ready && s->markets.size()==5 && s->account_ms>0);
  CHECK(s->balances.count(1) && s->balances.at(1)==75*10000);
  state.set_pins({"TEST-1"}); CHECK(state.snapshot()->pins.count("TEST-1"));
  CHECK(state.snapshot()->live.count("TEST-1"));
  fakex::HttpFault metadata_fault; metadata_fault.method="GET"; metadata_fault.path="/series/TEST";
  metadata_fault.kind=fakex::HttpFault::Kind::DROP_BEFORE_REPLY; fake.add_fault(metadata_fault);
  CHECK(state.refresh_once()); // missing fee metadata is per-market, not a fabricated cash/account failure
  CHECK(!state.snapshot()->metadata_error.empty());
  CHECK(!rest::fee_ready(state.snapshot()->markets.at("TEST-1"),now_ms()));
  CHECK(state.refresh_once());
  CHECK(rest::fee_ready(state.snapshot()->markets.at("TEST-1"),now_ms()));
  CHECK(state.snapshot()->metadata_error.empty());
  const auto meta=rest::market_json(state.snapshot()->markets.at("TEST-1"),true,true,now_ms());
  CHECK(meta["fee"]["type"]=="QUAD_MAKER" && meta["fee"]["mult"]==1.5);
  fake.clear_wire_log(); CHECK(state.refresh_once());
  for(const auto& entry:fake.wire_log()) CHECK(entry.find("/series/")==std::string::npos); // fresh cache
  // A failed account read keeps the last good one while it is young, and says so.
  fakex::HttpFault fault; fault.method="GET"; fault.path="/portfolio/balance";
  fault.kind=fakex::HttpFault::Kind::DROP_BEFORE_REPLY; fake.add_fault(fault);
  const auto good=state.snapshot();
  CHECK(state.refresh_once());
  CHECK(state.snapshot()->account_ready && state.snapshot()->balances==s->balances);
  CHECK(state.snapshot()->account_ms==good->account_ms && state.snapshot()->account_began_ms==good->account_began_ms);
  CHECK(state.snapshot()->error.rfind("account: ",0)==0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));   // a whole refresh can take under 1 ms
  CHECK(state.refresh_once() && state.snapshot()->error.empty() && state.snapshot()->account_ms>good->account_ms);
  {   // an older one is not kept; nor is a listing
    auto strict=opts; strict.account_keep_ms=-1; strict.listing_keep_ms=-1; strict.listing_poll_ms=100;
    rest::State old(factory,*endpoint,signer.get(),{},strict);
    CHECK(old.refresh_once());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    fake.add_fault(fault);
    CHECK(!old.refresh_once() && !old.snapshot()->account_ready && old.snapshot()->listing_ready);
    fakex::HttpFault listing_fault; listing_fault.method="GET"; listing_fault.path="/markets";
    listing_fault.kind=fakex::HttpFault::Kind::DROP_BEFORE_REPLY; fake.add_fault(listing_fault);
    std::this_thread::sleep_for(std::chrono::milliseconds(110));   // listing_poll_ms is at least 100
    const bool listed=old.refresh_once();
    if(listed || !old.snapshot()->account_ready || old.snapshot()->listing_ready) std::fprintf(stderr,"listing fault: %d %d %d %s\n",listed,(int)old.snapshot()->account_ready,(int)old.snapshot()->listing_ready,old.snapshot()->error.c_str());
    CHECK(!listed && old.snapshot()->account_ready && !old.snapshot()->listing_ready);
    CHECK(!old.snapshot()->markets.empty());   // the rows stay for the screen; admission waits on ready
    CHECK(old.refresh_once());
  }
  // A private event during the read: the pages are discarded, the last consistent read stays.
  const auto consistent=state.snapshot();
  state.set_reconcile_guard([] { return std::uint64_t{7}; }, [](std::uint64_t) { return false; });
  CHECK(state.refresh_once() && state.snapshot()->account_ready);
  CHECK(state.snapshot()->error.find("private feed changed") != std::string::npos);
  CHECK(state.snapshot()->account_ms==consistent->account_ms &&
        state.snapshot()->account_began_ms==consistent->account_began_ms &&
        state.snapshot()->balances==consistent->balances);
  {   // with no consistent read yet, the account is not ready
    rest::State fresh(factory,*endpoint,signer.get(),{},opts);
    fresh.set_reconcile_guard([] { return std::uint64_t{7}; }, [](std::uint64_t) { return false; });
    CHECK(!fresh.refresh_once() && !fresh.snapshot()->account_ready);
  }
  state.set_reconcile_guard([] { return std::uint64_t{8}; }, [](std::uint64_t fence) { return fence == 8; });
  CHECK(state.refresh_once());
  {   // the listing leaves out multivariate combos; a venue that refuses the filter gets the plain listing
    kalshi::MarketsQuery mq; mq.limit=200; mq.status="open"; mq.mve_filter="exclude";
    CHECK(kalshi::build_get_markets(mq).query.find("mve_filter=exclude")!=std::string::npos);
    rest::State lister(factory,*endpoint,signer.get(),{},opts);
    CHECK(lister.refresh_once() && lister.snapshot()->listing_ready);
    fakex::HttpFault refuse; refuse.method="GET"; refuse.path="/markets";
    refuse.kind=fakex::HttpFault::Kind::STATUS_AFTER_ACCEPT; refuse.status=400; fake.add_fault(refuse);
    const auto before=fake.http_requests();
    rest::State fallback(factory,*endpoint,signer.get(),{},opts);
    CHECK(fallback.refresh_once() && fallback.snapshot()->listing_ready && !fallback.snapshot()->markets.empty());
    CHECK(fallback.snapshot()->markets.size()==lister.snapshot()->markets.size());
    CHECK(fake.http_requests()>before);
  }
  {   // Kalshi's 429 is refused before processing: the same read waits and goes again
    fakex::HttpFault limited; limited.method="GET"; limited.path="/markets"; limited.kind=fakex::HttpFault::Kind::RATE_LIMIT;
    fake.add_fault(limited); fake.add_fault(limited);
    rest::State patient(factory,*endpoint,signer.get(),{},opts);
    CHECK(patient.refresh_once() && patient.snapshot()->listing_ready);
    for(int i=0;i<=opts.retries_429;++i) fake.add_fault(limited);
    rest::State gives_up(factory,*endpoint,signer.get(),{},opts);
    CHECK(!gives_up.refresh_once());
    CHECK(gives_up.snapshot()->error.find("HTTP 429")!=std::string::npos);
    CHECK(gives_up.refresh_once());   // the faults are spent: the next poll loads it
  }
  opts.max_pages=1;
  rest::State capped(factory,*endpoint,signer.get(),{},opts);
  CHECK(!capped.refresh_once());
  CHECK(!capped.snapshot()->listing_ready && capped.snapshot()->markets.empty());

  // The exchange already deducts collateral from its available balance. The UI must not
  // deduct it again. Exercise the real REST parser/projection against an actual resting order.
  fake.with_engine([](fakex::Engine& e,std::int64_t now) {
    fakex::NewOrder o; o.ticker="TEST-0"; o.client_order_id="Mcash-1";
    o.price=4000; o.count=1000; o.exchange_index=0; o.tif=fakex::Tif::GTC; o.post_only=true;
    o.expiration_s=now/1000+60;
    CHECK(!e.create(o,now).err);
  });
  CHECK(state.refresh_once());
  auto cash=rest_cash_json(*state.snapshot(),false);
  CHECK(cash["shards"][0]["free"]=="496.0000" && cash["shards"][0]["cash"]=="500.0000");
  CHECK(cash["total"]=="575.0000" && cash["free"]=="571.0000");
}
void cash_projection(json& capture) {
  rest::Snapshot s; s.balances={{0,920000},{2,50000}};
  kalshi::PositionRow p; p.ticker="TEST-A"; p.exchange_index=0; p.position=500;
  s.positions.push_back(p);
  kalshi::OrderRow a; a.order_id="high"; a.ticker=p.ticker; a.exchange_index=0;
  a.initial=500; a.remaining=500; a.side=kalshi::BookSide::ASK; a.yes_price=6000;
  auto b=a; b.order_id="low"; b.yes_price=5500;
  s.orders={a,b}; // low SELL gets the five reducing contracts; high SELL reserves $2
  auto cash=rest_cash_json(s,false);
  CHECK(cash["shards"][0]["cash"]=="94.0000" && cash["shards"][0]["free"]=="92.0000");
  CHECK(cash["total"]=="99.0000" && cash["free"]=="97.0000");
  HdrInputs hdr; hdr.cash_k=cash;
  capture["events"].push_back({{"type","hdr"},{"seq",1},{"ts_ms",now_ms()},{"data",hdr_json(hdr)}});
  {   // Systematic's part: day.man is the account's day less day.sys; the stop counts sys only
    HdrInputs h=hdr; h.daily_stop=300000; h.day_man=-50000;
    h.sys=HdrInputs::Sys{-120000,7000,50000,45000,690000,600000};
    const auto j=hdr_json(h);
    CHECK(j["day"]["total"]=="-5.0000" && j["day"]["sys"]=="-12.0000" && j["day"]["man"]=="7.0000");
    CHECK(j["stop"]["left"]=="18.0000" && j["stop"]["limit"]=="30.0000" && j["stop"]["hit"]==false);
    CHECK(j["sys"]["K"]==json({{"used","4.5000"},{"ceiling","69.0000"},{"eff","60.0000"}}));
    CHECK(j["cost"]==json({{"used","0.7000"},{"budget","5.0000"}}));
    h.sys->day=-310000;
    CHECK(hdr_json(h)["stop"]["hit"]==true && hdr_json(h)["stop"]["left"]=="-1.0000");
    capture["events"].push_back({{"type","hdr"},{"seq",2},{"ts_ms",now_ms()},{"data",hdr_json(h)}});
  }
  // Equivalent BUY orders against NO inventory allocate the reducing room highest first.
  s.positions[0].position=-500; s.orders[0].side=s.orders[1].side=kalshi::BookSide::BID;
  cash=rest_cash_json(s,false); CHECK(cash["shards"][0]["cash"]=="94.7500");
  // Fractional collateral is rounded upward at the UI boundary, never down to whole lots.
  s.positions.clear(); s.orders.resize(1); s.orders[0].remaining=1; s.orders[0].yes_price=5555;
  cash=rest_cash_json(s,false); CHECK(cash["shards"][0]["cash"]=="92.0056");
  s.orders[0].exchange_index.reset();
  CHECK(rest_cash_json(s,false)["ro"]==true);
  s.markets["TEST-A"].exchange_index=0; // actual market metadata can identify a missing order shard
  CHECK(rest_cash_json(s,false)["ro"]==false);
}
}
int main() {
  ::signal(SIGPIPE,SIG_IGN);
  clock_checks(); series_fees(); json capture={{"events",json::array()},{"responses",json::array()}};
  discovery(capture); reconcile(); cash_projection(capture);
  TempDir dir; std::ofstream out(dir.file("capture.json")); out<<capture.dump(); out.close();
  const std::string command="node '"+std::string(DESKD_SOURCE_DIR)+"/tests/deskd/contract_check.cjs' '"+dir.file("capture.json")+"'";
  CHECK(std::system(command.c_str())==0);
  return finish("deskd-rest");
}
