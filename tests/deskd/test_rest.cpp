#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>
#include <fstream>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/build_paths.h"
#include "deskd/net/guard.h"
#include "deskd/rest/state.h"
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
      m.close_ms=now+3600000; e.add_market(m,now);
    }
    e.set_cash(0,0,500*fakex::kMicrosPerDollar,now);
    e.set_cash(0,1,75*fakex::kMicrosPerDollar,now);
  });
  std::string why; CHECK(fake.start(&why));
  auto signer=key(); CHECK(signer!=nullptr);
  SendGuard guard(false,nullptr); net::ConnFactory factory(guard);
  const auto endpoint=net::Endpoint::parse("http://127.0.0.1:"+std::to_string(fake.port())+"/trade-api/v2",&why);
  rest::Options opts; opts.page_size=2;
  rest::State state(factory,*endpoint,signer.get(),{},opts);
  CHECK(state.refresh_once());
  auto s=state.snapshot();
  if (!s->ready) std::fprintf(stderr,"REST: %s\n",s->error.c_str());
  CHECK(s->ready && s->markets.size()==5 && s->account_ms>0);
  CHECK(s->balances.count(1) && s->balances.at(1)==75*10000);
  state.set_pins({"TEST-1"}); CHECK(state.snapshot()->pins.count("TEST-1"));
  CHECK(state.snapshot()->live.count("TEST-1"));
  fakex::HttpFault fault; fault.method="GET"; fault.path="/portfolio/balance";
  fault.kind=fakex::HttpFault::Kind::DROP_BEFORE_REPLY; fake.add_fault(fault);
  CHECK(!state.refresh_once());
  CHECK(!state.snapshot()->account_ready && state.snapshot()->balances==s->balances);
  CHECK(state.refresh_once());
  state.set_reconcile_guard([] { return std::uint64_t{7}; }, [](std::uint64_t) { return false; });
  CHECK(!state.refresh_once() && !state.snapshot()->account_ready);
  CHECK(state.snapshot()->error.find("private feed changed") != std::string::npos);
  state.set_reconcile_guard([] { return std::uint64_t{8}; }, [](std::uint64_t fence) { return fence == 8; });
  CHECK(state.refresh_once());
  opts.max_pages=1;
  rest::State capped(factory,*endpoint,signer.get(),{},opts);
  CHECK(!capped.refresh_once());
  CHECK(!capped.snapshot()->listing_ready && capped.snapshot()->markets.empty());
}
}
int main() {
  ::signal(SIGPIPE,SIG_IGN);
  clock_checks(); json capture={{"events",json::array()},{"responses",json::array()}};
  discovery(capture); reconcile();
  TempDir dir; std::ofstream out(dir.file("capture.json")); out<<capture.dump(); out.close();
  const std::string command="node '"+std::string(DESKD_SOURCE_DIR)+"/tests/deskd/contract_check.cjs' '"+dir.file("capture.json")+"'";
  CHECK(std::system(command.c_str())==0);
  return finish("deskd-rest");
}
