#include <fstream>
#include "check.h"
#include "deskd/build_paths.h"
#include "deskd/gw/order_ledger.h"
using namespace deskd;
using J=nlohmann::json;
constexpr std::int64_t t=1791500000000LL;
static J capture={{"responses",J::array()}};
static PlaceIntent intent(std::string cid="Mledger-1",Tif tif=Tif::POST) {
  PlaceIntent p; p.cid=std::move(cid); p.ticker="TEST-A"; p.price=4200; p.count=1000;
  p.tif=tif; p.exp_ms=tif==Tif::POST ? t+15000 : 0; p.t_recv_ms=t; p.t_click_ms=t-1; return p;
}
static kalshi::FillRow execution(std::string trade="trade-1",Qty qty=250) {
  kalshi::FillRow f; f.fill_id="REST-"+trade; f.trade_id=std::move(trade); f.order_id="oid-1";
  f.ticker="TEST-A"; f.count=qty; f.yes_price=4200; f.fee=17000; f.exchange_index=0; f.ts_ms=t+2;
  return f;
}
static kalshi::CreateAck ack(Qty filled=0,Qty left=1000) {
  kalshi::CreateAck a; a.order_id="oid-1"; a.client_order_id="Mledger-1";
  a.fill_count=filled; a.remaining=left; a.ts_ms=t+1; return a;
}
static kalshi::OrderRow row(Qty filled,Qty left,kalshi::OrderStatus status,std::int64_t at=t+3) {
  kalshi::OrderRow r; r.order_id="oid-1"; r.client_order_id="Mledger-1"; r.ticker="TEST-A";
  r.initial=1000; r.filled=filled; r.remaining=left; r.status=status; r.yes_price=4200;
  r.exchange_index=0; r.updated_ms=at; return r;
}
static void record(const OrderLedger& l,const std::string& cid="Mledger-1") {
  const auto* o=l.find(cid); CHECK(o!=nullptr); if(!o) return;
  CHECK(o->filled>=o->executions && o->filled<=o->intent.count);
  CHECK(o->left>=0 && o->left<=o->intent.count-o->filled);
  CHECK(!o->terminal() || o->left==0);
  CHECK(o->reservation()>=0 && o->reservation()<=o->original_reserve);
  capture["responses"].push_back({{"key","POST /api/order"},{"status",200},{"json",{{"ok",true},{"ord",o->json()}}}});
}
static OrderLedger ledger(Tif tif=Tif::POST) {
  OrderLedger l; CHECK(l.insert(intent("Mledger-1",tif),0,4200000)); CHECK(l.sent("Mledger-1",t+1)); return l;
}
static void races() {
  auto l=ledger(); record(l);
  CHECK(l.fill(execution(),"Mledger-1",t+2)==OrderLedger::FillResult::APPLIED);
  CHECK(l.find("Mledger-1")->executions==250 && l.reserved(0)==3150000); record(l);
  CHECK(l.acknowledged("Mledger-1",ack(),t+4)); // stale 201 cannot undo the earlier fill
  CHECK(l.find("Mledger-1")->filled==250 && l.find("Mledger-1")->left==750);
  CHECK(l.unknown("Mledger-1") && !l.blocks("TEST-A",0)); record(l);
  auto duplicate=execution(); duplicate.fill_id="WS-id"; duplicate.ts_ms=t;
  CHECK(l.fill(duplicate,"Mledger-1",t+5)==OrderLedger::FillResult::DUPLICATE);
  CHECK(l.find("Mledger-1")->executions==250);
  CHECK(l.request_cancel("Mledger-1"));
  CHECK(l.cancelled("Mledger-1",{"oid-1","Mledger-1",500,t+6},"USER",t+6));
  CHECK(l.find("Mledger-1")->filled==500 && l.find("Mledger-1")->executions==250);
  CHECK(l.find("Mledger-1")->state==OrdState::CXL && l.reserved(0)==1050000);
  CHECK(l.fills_due(t+5005).empty()); CHECK(l.fills_due(t+5006).size()==1); record(l);
  CHECK(l.fill(execution("trade-2"),"Mledger-1",t+5007)==OrderLedger::FillResult::APPLIED);
  CHECK(l.reserved(0)==0 && l.fills_due(t+9000).empty()); record(l);
  CHECK(l.observed(row(250,750,kalshi::OrderStatus::RESTING,t+2),t+6000));
  CHECK(l.find("Mledger-1")->state==OrdState::CXL && l.find("Mledger-1")->filled==500); record(l);
  CHECK(!l.insert(intent(),0,4200000));
}
static void cumulative_is_not_execution() {
  auto l=ledger(Tif::IOC);
  CHECK(l.acknowledged("Mledger-1",ack(250,0),t+2));
  CHECK(l.find("Mledger-1")->state==OrdState::CXL && l.find("Mledger-1")->why=="IOC_REST");
  CHECK(l.find("Mledger-1")->executions==0 && l.reserved(0)==1050000); record(l);
  CHECK(l.observed(row(250,0,kalshi::OrderStatus::CANCELED),t+3));
  CHECK(l.reserved(0)==1050000);
  CHECK(l.fill(execution(),"",t+4)==OrderLedger::FillResult::APPLIED);
  CHECK(l.reserved(0)==0); record(l);

  auto full=ledger(Tif::FOK); CHECK(full.acknowledged("Mledger-1",ack(1000,0),t+2));
  CHECK(full.find("Mledger-1")->state==OrdState::FILL && full.reserved(0)==4200000); record(full);
  CHECK(full.fill(execution("full",1000),"Mledger-1",t+3)==OrderLedger::FillResult::APPLIED);
  CHECK(full.reserved(0)==0); record(full);
  auto zero=ledger(Tif::FOK); CHECK(zero.acknowledged("Mledger-1",ack(0,0),t+2));
  CHECK(zero.find("Mledger-1")->why=="FOK" && zero.reserved(0)==0); record(zero);
}
static void zero_cancel_needs_read() {
  auto l=ledger(); CHECK(l.request_cancel("Mledger-1"));
  CHECK(l.cancelled("Mledger-1",{"oid-1","Mledger-1",0,t+2},"USER",t+2));
  CHECK(l.reserved(0)==4200000 && l.blocks("TEST-A",0)); record(l);
  CHECK(l.acknowledged("Mledger-1",ack(),t+3)); // a late create response is not a cancel proof
  CHECK(l.reserved(0)==4200000 && l.blocks("TEST-A",0)); record(l);
  CHECK(l.observed(row(1000,0,kalshi::OrderStatus::EXECUTED),t+4));
  CHECK(!l.blocks("TEST-A",0) && l.reserved(0)==4200000); record(l);
  CHECK(l.fill(execution("full",1000),"",t+5)==OrderLedger::FillResult::APPLIED);
  CHECK(l.reserved(0)==0); record(l);
  auto partial=ledger();
  CHECK(partial.cancelled("Mledger-1",{"oid-1","Mledger-1",0,t+2},"USER",t+2));
  CHECK(partial.acknowledged("Mledger-1",ack(250,750),t+3));
  CHECK(partial.blocks("TEST-A",0) && partial.reserved(0)==4200000); record(partial);
  CHECK(partial.find("Mledger-1")->left==750);
  CHECK(partial.acknowledged("Mledger-1",ack(1000,0),t+4));
  CHECK(!partial.blocks("TEST-A",0) && partial.reserved(0)==4200000); record(partial);
}
static void unknown_table_and_proof() {
  OrderLedger l;
  for(int i=0;i<5000;++i) {
    const auto cid="Mbulk-"+std::to_string(i);
    auto p=intent(cid); p.ticker="T-"+std::to_string(i); p.tif=Tif::IOC; p.exp_ms=0;
    CHECK(l.insert(p,i%4,4200000)); CHECK(l.sent(cid,t+1)); CHECK(l.unknown(cid));
  }
  CHECK(l.reserved(0)==1250LL*4200000 && l.blocks("T-4999",3));
  OrderLedger::History h; h.ticker="T-4999"; h.shard=3; h.since_ms=t-4999;
  h.began_ms=t+120001; h.completed_ms=t+120002; h.orders_complete=true; h.fills_complete=true;
  auto bad=h; bad.orders_complete=false; CHECK(!l.resolve_absent("Mbulk-4999",bad));
  bad=h; bad.fills_complete=false; CHECK(!l.resolve_absent("Mbulk-4999",bad));
  bad=h; ++bad.since_ms; CHECK(!l.resolve_absent("Mbulk-4999",bad));
  bad=h; --bad.began_ms; CHECK(!l.resolve_absent("Mbulk-4999",bad));
  bad=h; bad.shard=0; CHECK(!l.resolve_absent("Mbulk-4999",bad));
  bad=h; auto r=row(0,1000,kalshi::OrderStatus::RESTING); r.client_order_id="Mbulk-4999";
  bad.orders.push_back(r); CHECK(!l.resolve_absent("Mbulk-4999",bad));
  CHECK(l.resolve_absent("Mbulk-4999",h)); CHECK(!l.blocks("T-4999",3));
  CHECK(l.reserved(3)==1249LL*4200000 && l.blocks("T-4998",2)); record(l,"Mbulk-4999");
  auto post=ledger(); CHECK(post.unknown("Mledger-1"));
  h.ticker="TEST-A"; h.shard=0; h.began_ms=t+19999; h.completed_ms=t+30000;
  CHECK(!post.resolve_absent("Mledger-1",h)); ++h.began_ms;
  CHECK(post.resolve_absent("Mledger-1",h)); CHECK(post.find("Mledger-1")->state==OrdState::EXP); record(post);
}
static void touch_and_edge() {
  auto p=intent("Sedge-1"); p.owner=Owner::SYS; p.lane="M1"; p.side=Side::BUY;
  p.touch=4100; p.fair=4350;
  OrderLedger l; CHECK(l.insert(p,0,1));
  const auto j=l.find("Sedge-1")->json();
  CHECK(j["touch"]=="0.4100" && j["edge"]=="0.0150" && j["own"]=="SYS" && j["lane"]=="M1");   // bid 0.42 under fair 0.435
  auto s=intent("Sedge-2"); s.side=Side::SELL; s.fair=4350;
  CHECK(l.insert(s,0,1) && l.find("Sedge-2")->json()["edge"]=="-0.0150" && l.find("Sedge-2")->json()["touch"].is_null());
  CHECK(!intent().touch && !intent().fair);
}
static void bad_evidence() {
  auto l=ledger(); auto f=execution();
  CHECK(l.fill(f,"",t+1)==OrderLedger::FillResult::UNMAPPED && l.healthy() && l.why_unhealthy().empty());
  CHECK(l.fill(f,"Mledger-1",t+2)==OrderLedger::FillResult::APPLIED);
  f.count=251; CHECK(l.fill(f,"Mledger-1",t+3)==OrderLedger::FillResult::INVALID);
  CHECK(!l.healthy() && l.find("Mledger-1")->executions==250 && l.reserved(0)==3150000);
  CHECK(l.why_unhealthy().find("fill")!=std::string::npos && l.why_unhealthy().find("arrived twice")!=std::string::npos);
  CHECK(!l.insert(intent("Mnew-1"),0,1));
  auto other=ledger(); auto a=ack(); a.fill_count=1001;
  CHECK(!other.acknowledged("Mledger-1",a,t+2) && !other.healthy()); CHECK(other.reserved(0)==4200000);
  CHECK(other.why_unhealthy().rfind("Mledger-1: acknowledged filled 1001",0)==0);
  auto reject=ledger(); CHECK(reject.rejected("Mledger-1","SHARD_CASH",t+2));
  CHECK(reject.reserved(0)==0); record(reject);
  CHECK(!reject.acknowledged("Mledger-1",ack(),t+3) && !reject.healthy());
  auto uncertain=ledger(); CHECK(uncertain.unknown("Mledger-1"));
  CHECK(!uncertain.rejected("Mledger-1","EXCHANGE",t+2)); CHECK(uncertain.reserved(0)==4200000);
  auto malformed=ledger();
  CHECK(!malformed.observed(row(250,0,kalshi::OrderStatus::EXECUTED),t+3));
  CHECK(!malformed.healthy() && malformed.reserved(0)==4200000);
  CHECK(malformed.why_unhealthy().find("is executed with")!=std::string::npos);
  // the first contradiction is the one kept
  CHECK(!malformed.observed(row(250,0,kalshi::OrderStatus::EXECUTED),t+4));
  CHECK(malformed.why_unhealthy().find("Mledger-1:")==0);
}
static void fractional_and_permutations() {
  auto p=intent(); p.count=3; p.side=Side::SELL;
  OrderLedger l; CHECK(l.insert(p,2,101)); auto f=execution("fraction",1);
  f.exchange_index=2; f.side=kalshi::BookSide::ASK; f.fee=1;
  CHECK(l.fill(f,p.cid,t+1)==OrderLedger::FillResult::APPLIED);
  CHECK(l.reserved(2)==68 && l.find(p.cid)->json()["avg"]=="0.4200"); record(l);
  std::vector<int> ops{0,1,2,3};
  do {
    auto book=ledger();
    for(auto op:ops) {
      if(op==0) CHECK(book.acknowledged("Mledger-1",ack(1000,0),t+10));
      if(op==1) CHECK(book.fill(execution("one",500),"Mledger-1",t+10)==OrderLedger::FillResult::APPLIED);
      if(op==2) CHECK(book.fill(execution("two",500),"Mledger-1",t+10)==OrderLedger::FillResult::APPLIED);
      if(op==3) CHECK(book.observed(row(1000,0,kalshi::OrderStatus::EXECUTED),t+10));
      record(book);
    }
    CHECK(book.find("Mledger-1")->state==OrdState::FILL && book.reserved(0)==0);
    CHECK(book.find("Mledger-1")->executions==1000 && book.find("Mledger-1")->fees==34000);
  } while(std::next_permutation(ops.begin(),ops.end()));
  ops={0,1,2,3,4};
  do {
    auto book=ledger(); CHECK(book.request_cancel("Mledger-1"));
    for(auto op:ops) {
      if(op==0) CHECK(book.acknowledged("Mledger-1",ack(),t+10));
      if(op==1) CHECK(book.fill(execution("one",250),"Mledger-1",t+10)==OrderLedger::FillResult::APPLIED);
      if(op==2) CHECK(book.fill(execution("two",250),"Mledger-1",t+10)==OrderLedger::FillResult::APPLIED);
      if(op==3) CHECK(book.observed(row(500,0,kalshi::OrderStatus::CANCELED),t+10));
      if(op==4) CHECK(book.cancelled("Mledger-1",{"oid-1","Mledger-1",500,t+10},"USER",t+10));
      record(book);
    }
    CHECK(book.find("Mledger-1")->state==OrdState::CXL && book.reserved(0)==0);
    CHECK(book.find("Mledger-1")->executions==500 && book.find("Mledger-1")->fees==34000);
  } while(std::next_permutation(ops.begin(),ops.end()));
}
int main() {
  races(); cumulative_is_not_execution(); zero_cancel_needs_read(); unknown_table_and_proof();
  bad_evidence(); touch_and_edge(); fractional_and_permutations();
  TempDir dir;
  const std::string path=dir.file("order-ledger-contract.json");
  { std::ofstream out(path); out<<capture; }
  if(std::string(DESKD_NODE).empty()) CHECK(false);
  else {
    const std::string cmd="\""+std::string(DESKD_NODE)+"\" \""+DESKD_SOURCE_DIR+"/tests/deskd/contract_check.cjs\" \""+path+"\"";
    CHECK(std::system(cmd.c_str())==0);
  }
  return finish("deskd-order-ledger");
}
