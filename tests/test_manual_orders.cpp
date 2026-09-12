#include "desk/manual_orders.h"
#include "desk/risk.h"
#include <cmath>
#include <stdexcept>
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(0)
#include <iostream>
using namespace desk;
int main() {
  Store store(":memory:");ManualOrders queue(store);
  Market m;m.ticker="QUEUE-A-YES";m.event_ticker="QUEUE-A";m.series_ticker="QUEUE";m.exchange_index=0;store.upsert_market(m);
  Series s;s.ticker="QUEUE";s.fee_type="quadratic";s.fee_multiplier=1;store.upsert_series(s);
  const std::string id="11111111-2222-4333-8444-555555555555";
  json input={{"client_request_id",id},{"ticker",m.ticker},{"outcome_side","no"},{"price",.52},{"count",6.},{"post_only",false},{"reviewed_ts",1000}};
  auto r=queue.enqueue(input,1000);CHECK(r["status"]=="queued");CHECK(queue.enqueue(input,1001)["id"]==id);
  bool refused=false;try{auto x=input;x["count"]=15;queue.enqueue(x,1001);}catch(...){refused=true;}CHECK(refused);
  int sends=0;auto lookup=[](const std::string&){return json::object();};
  auto send=[&](const json& p,const std::string& request,long long deadline){++sends;CHECK(request==id&&p["count"]==6&&deadline==1090);return json{{"order_id","fixture"},{"status","submitted"}};};
  queue.process([](auto&,auto&,auto)->json{throw RetryManualOrder("account catching up");},lookup,1002);
  CHECK(queue.view()[0]["status"]=="queued");queue.process(send,lookup,1003);queue.process(send,lookup,1004);CHECK(sends==1);
  CHECK(queue.enqueue(input,1200)["status"]=="submitted"); // a retry returns its old result, never resubmits
  input["client_request_id"]="21111111-2222-4333-8444-555555555555";input["reviewed_ts"]=1200;
  queue.enqueue(input,1200);queue.process(send,lookup,1291);CHECK(sends==1&&queue.view()[0]["status"]=="expired");
  input["client_request_id"]="31111111-2222-4333-8444-555555555555";input["reviewed_ts"]=1300;queue.enqueue(input,1300);
  queue.cancel(input["client_request_id"],1301);queue.process(send,lookup,1302);CHECK(sends==1);
  input["client_request_id"]="41111111-2222-4333-8444-555555555555";input["reviewed_ts"]=1400;queue.enqueue(input,1400);
  auto unknown=[](const std::string&){return json{{"status","unknown"}};};
  queue.process([&](auto&,auto&,auto)->json{++sends;throw std::runtime_error("transport lost");},unknown,1401);
  ManualOrders restored(store);restored.process(send,unknown,1402);CHECK(sends==2&&restored.view()[0]["status"]=="unknown");
  restored.process(send,[](auto&){return json{{"status","submitted"},{"order_id","recovered-exact-client-id"}};},1403);
  CHECK(sends==2&&restored.view()[0]["status"]=="submitted");
  Balance b;b.balance=75;b.portfolio_value=0;
  store.replace_risk_snapshot(b,{}, {},json::array({{{"exchange_index",0},{"balance","3.5512"}},{{"exchange_index",3},{"balance","65.28"}}}),1500);
  auto cash=shard_cash_view(store,m,1501);CHECK(cash["fresh"]==true&&std::fabs(cash["available"].get<double>()-3.5512)<1e-8);
  input["client_request_id"]="51111111-2222-4333-8444-555555555555";input["reviewed_ts"]=1501;queue.enqueue(input,1501);
  cash=shard_cash_view(store,m,1502);CHECK(cash["available"].get<double>()<.5&&cash["queued_reserved"].get<double>()>3.12);
  CHECK(shard_cash_view(store,m,1600)["fresh"]==false);
  std::cout<<"PASS manual queue: idempotency, immutable orders, retry, expiry, cancel, unknown recovery, shard-local reservations\n";
}
