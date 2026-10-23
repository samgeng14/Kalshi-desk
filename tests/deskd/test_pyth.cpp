// Kalshi's pyth_value channel (plan section 5, LIP-15): the reducer's rules, then the socket
// against the fake.
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/net/guard.h"
#include "deskd/ref/pyth.h"
#include "fake_kalshi.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
std::unique_ptr<KalshiSigner> key() {
  auto* k=EVP_PKEY_Q_keygen(nullptr,nullptr,"ED25519"); auto* b=BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(b,k,nullptr,nullptr,0,nullptr,nullptr);
  char* p=nullptr; const auto n=BIO_get_mem_data(b,&p); const std::string pem(p,n);
  BIO_free(b); EVP_PKEY_free(k); std::string why;
  return KalshiSigner::from_pem("pyth-test",pem,"ed25519",&why);
}
bool until(const std::function<bool()>& fn) {
  const auto end=mono_ms()+5000;
  while(mono_ms()<end) { if(fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  return fn();
}
const std::string NG="Commodities.Index.NATGAS/USD";
const std::int64_t T0=1791000000000;   // 2026-10-03

void reducer() {
  PythValues v; std::string why;
  auto msg=[](json value,std::int64_t ts) { return json{{"underlying_ticker",NG},{"value_usd",value},{"source_ts_ms",ts}}; };
  CHECK(v.value(msg(3.1234,T0),T0+40,&why));
  CHECK(v.value(msg("3.1240",T0+400),T0+440,&why));   // a decimal string reads the same
  const auto& s=v.series().at(NG);
  CHECK(s.ticks.size()==2 && s.last()->value==3.124 && s.last()->source_ms==T0+400 && s.messages==2);
  CHECK(v.value(msg(3.2,T0+400),T0+500,&why) && s.duplicates==1 && s.ticks.size()==2);   // same source time
  CHECK(v.value(msg(3.2,T0+300),T0+520,&why) && s.out_of_order==1 && s.ticks.size()==2);
  CHECK(v.value(msg(3.124,T0+800),T0+840,&why) && s.unchanged==1);
  // malformed: counted and skipped, nothing guessed
  CHECK(!v.value(json{{"underlying_ticker",NG},{"value_usd",0}},T0+900,&why) && v.malformed()==1);
  CHECK(!v.value(json{{"value_usd",3.1}},T0+900,&why) && v.malformed()==2);
  CHECK(!v.value(msg("3.1x",T0+900),T0+900,&why) && v.malformed()==3);
  CHECK(!v.value(msg(3.1,1234),T0+900,&why) && v.malformed()==4);   // seconds, not ms
  CHECK(s.ticks.size()==3);
  // fresh: within max(2 s, 8 x the median gap) and 20 s
  CHECK(s.median_gap_ms()==400 && v.gap_limit_ms(s)==3200);
  CHECK(v.fresh(s,T0+840+3200) && !v.fresh(s,T0+840+3201));
  PythValues slow; std::int64_t t=T0;
  for(int i=0;i<10;++i) { t+=5000; CHECK(slow.value(msg(3.0+i*0.001,t),t,&why)); }
  const auto& ss=slow.series().at(NG);
  CHECK(slow.gap_limit_ms(ss)==40000 && slow.fresh(ss,t+20000) && !slow.fresh(ss,t+20001));   // the 20 s cap
  // history: 90 minutes by our clock
  PythValues h; std::int64_t at=T0;
  for(int i=0;i<200;++i) { at+=60000; CHECK(h.value(msg(3.0,at),at,&why)); }
  CHECK(h.series().at(NG).ticks.size()==91);
}

void socket() {
  fakex::FakeConfig config; config.rate_limits=false;
  fakex::FakeKalshi fake(config);
  std::string why; CHECK(fake.start(&why)); auto signer=key(); CHECK(signer!=nullptr);
  SendGuard guard(false,nullptr); net::ConnFactory factory(guard);
  const auto endpoint=net::Endpoint::parse("http://127.0.0.1:"+std::to_string(fake.port())+"/trade-api/ws/v2",&why);
  FeedState feed;
  KalshiPyth::Options o; o.backoff_min_ms=20; o.backoff_max_ms=100;
  KalshiPyth pyth(*endpoint,factory,*signer,feed,o);
  Worker w("pyth-test"); w.start([&](Worker& self){pyth.run(self);});
  CHECK(until([&]{ return feed.view().up; }));
  CHECK(pyth.symbol_for({"natgas"}).empty() && !pyth.view(NG,0,now_ms()));
  const auto now=now_ms();
  fake.pyth(NG,3.1234,now-30); fake.pyth(NG,3.1236,now-20); fake.pyth("Metal.Index.GOLD/USD",2650.5,now-10);
  CHECK(until([&]{ return pyth.symbol_for({"gold"})=="Metal.Index.GOLD/USD"; }));
  CHECK(pyth.symbol_for({"Commodities.Index.NATGAS/USD","natgas"})==NG && pyth.symbol_for({"NATGAS"})==NG);
  auto v=pyth.view(NG,0,now_ms());
  CHECK(v && v->value==3.1236 && v->source_ms==now-20 && v->fresh && v->ticks.size()==2);
  if(v && v->ticks.size()==2) CHECK(v->ticks[0]==std::make_pair(now-30,3.1234));
  CHECK(pyth.view(NG,-1,now_ms())->ticks.empty());
  const auto st=pyth.status(now_ms());
  CHECK(st["up"]==true && st["underlyings"][NG]["messages"]==2 && st["underlyings"][NG]["fresh"]==true);
  // a dropped socket: the feed reads down, reconnects, and the values it held stay (not fresh)
  fake.drop_ws_connections();
  CHECK(until([&]{ return pyth.status(now_ms())["connects"].get<int>()>=2 && feed.view().up; }));
  fake.pyth(NG,3.1240,now_ms());
  CHECK(until([&]{ auto x=pyth.view(NG,-1,now_ms()); return x && x->value==3.124; }));
  w.request_stop(); w.join();
  CHECK(!feed.view().up);
}
}  // namespace

int main() {
  signal(SIGPIPE,SIG_IGN);
  reducer(); socket();
  return finish("deskd-pyth");
}
