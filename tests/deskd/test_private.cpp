#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>
#include <chrono>
#include <mutex>
#include <thread>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/net/guard.h"
#include "deskd/priv/kalshi_private.h"
#include "fake_kalshi.h"
using namespace deskd;
using json=nlohmann::json;
namespace {
std::unique_ptr<KalshiSigner> key() {
  auto* k=EVP_PKEY_Q_keygen(nullptr,nullptr,"ED25519"); auto* b=BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(b,k,nullptr,nullptr,0,nullptr,nullptr);
  char* p=nullptr; const auto n=BIO_get_mem_data(b,&p); const std::string pem(p,n);
  BIO_free(b); EVP_PKEY_free(k); std::string why;
  return KalshiSigner::from_pem("private-test",pem,"ed25519",&why);
}
bool until(const std::function<bool()>& fn) {
  const auto end=mono_ms()+4000;
  while(mono_ms()<end) { if(fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  return fn();
}
void parser() {
  const auto now=now_ms(); std::string why;
  json body={{"trade_id","trade-one"},{"order_id","order-one"},{"client_order_id","Msession-1"},
      {"market_ticker","TEST-A"},{"exchange_index",0},{"is_taker",true},{"book_side","ask"},
      {"yes_price_dollars","0.4200"},{"count_fp","1.25"},{"fee_cost","0.021000"},
      {"ts_ms",now},{"post_position_fp","-1.25"},{"subaccount",3}};
  auto parse=[&] { return parse_private_event({{"type","fill"},{"msg",body}},now,&why); };
  auto fill=parse(); CHECK(fill && fill->kind==PrivateEvent::Kind::FILL);
  if(fill) {
    CHECK(fill->fill.trade_id=="trade-one" && fill->fill.fill_id=="trade-one");
    CHECK(fill->cid=="Msession-1" && fill->fill.ticker=="TEST-A");
    CHECK(fill->fill.side==kalshi::BookSide::ASK && fill->fill.count==125);
    CHECK(fill->fill.fee==21000 && fill->post_position==std::optional<Qty>(-125));
    CHECK(fill->fill.ts_ms==now && fill->fill.subaccount==std::optional<int>(3));
  }
  body["count_fp"]="1.251"; CHECK(!parse()); body["count_fp"]="1.25";
  body.erase("trade_id"); CHECK(!parse()); body["trade_id"]="trade-one";
  body.erase("book_side"); CHECK(!parse()); body["book_side"]="ask";
  body["post_position_fp"]="garbage"; CHECK(!parse());
}
void socket() {
  fakex::FakeConfig config; config.rate_limits=false; config.ws_ping_ms=40;
  fakex::FakeKalshi fake(config);
  fake.with_engine([](fakex::Engine& e,std::int64_t now) {
    fakex::MarketSpec m; m.ticker="TEST-A"; e.add_market(m,now);
    e.set_cash(0,0,100*fakex::kMicrosPerDollar,now);
    e.street_add("TEST-A",true,5800,500,now);
  });
  std::string why; CHECK(fake.start(&why)); auto signer=key(); CHECK(signer!=nullptr);
  SendGuard guard(false,nullptr); net::ConnFactory factory(guard);
  const auto endpoint=net::Endpoint::parse("http://127.0.0.1:"+std::to_string(fake.port())+"/trade-api/ws/v2",&why);
  FeedState feed; std::atomic<int> requests{0}; std::mutex mu; std::vector<PrivateEvent> events;
  KalshiPrivate::Options options; options.ping_ms=80; options.pong_ms=500;
  options.backoff_min_ms=20; options.backoff_max_ms=100;
  KalshiPrivate priv(*endpoint,factory,*signer,feed,[&](const auto& event) {
    std::lock_guard<std::mutex> lk(mu); events.push_back(event); return false;
  },[&]{++requests;},options);
  CHECK(priv.fence()==0 && !priv.reconcile(0));
  Worker worker("private-test"); worker.start([&](Worker& w){priv.run(w);});
  CHECK(until([&]{return priv.fence()!=0;}));
  CHECK(feed.view().gap); const auto old=priv.fence(); CHECK(priv.reconcile(old));
  CHECK(until([&]{return feed.view().up && !feed.view().gap;}));
  fake.with_engine([](fakex::Engine& e,std::int64_t now) {
    fakex::NewOrder o; o.ticker="TEST-A"; o.client_order_id="Mprivate-1";
    o.count=125; o.price=4200; o.tif=fakex::Tif::IOC; o.exchange_index=0;
    CHECK(!e.create(o,now).err);
  });
  CHECK(until([&]{std::lock_guard<std::mutex> lk(mu); return events.size()>=2;}));
  CHECK(priv.fence()!=old && !priv.reconcile(old));
  CHECK(feed.view().gap); CHECK(priv.reconcile(priv.fence()));
  CHECK(until([&]{return feed.view().up && !feed.view().gap;}));
  { std::lock_guard<std::mutex> lk(mu);
    bool got=false;
    for(const auto& e:events) if(e.kind==PrivateEvent::Kind::FILL) {
      got=true; CHECK(e.fill.count==125 && e.post_position==std::optional<Qty>(125));
    }
    CHECK(got);
  }
  const auto before=priv.fence(); fake.drop_ws_connections();
  CHECK(until([&]{return priv.fence()!=0 && priv.fence()!=before;}));
  CHECK(feed.view().gap && !priv.reconcile(before));
  CHECK(priv.reconcile(priv.fence()));
  CHECK(until([&]{return !feed.view().gap;}));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(feed.view().up && !feed.view().gap && requests>=2);
  worker.request_stop(); worker.join();
  CHECK(!feed.view().up && priv.fence()==0);
}
}
int main() { ::signal(SIGPIPE,SIG_IGN); parser(); socket(); return finish("deskd-private"); }
