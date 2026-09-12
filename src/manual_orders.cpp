#include "desk/manual_orders.h"
#include <cmath>
#include <regex>
#include "desk/fixed.h"

namespace desk {
ManualOrders::ManualOrders(Store& store):store_(store) {
  store_.exec("CREATE TABLE IF NOT EXISTS manual_requests(id TEXT PRIMARY KEY,payload TEXT,ticker TEXT,event_ticker TEXT,status TEXT,created_ts INTEGER,expires_ts INTEGER,updated_ts INTEGER,result TEXT,error TEXT)");
}
json ManualOrders::enqueue(const json& input,long long now) {
  if(!input.is_object())throw std::runtime_error("manual request must be an object");
  const auto id=input.value("client_request_id","");
  if(!std::regex_match(id,std::regex("[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}")))throw std::runtime_error("manual request needs a UUID client_request_id");
  for(const auto& f:input.items())if(f.key()!="client_request_id"&&f.key()!="ticker"&&f.key()!="outcome_side"&&f.key()!="price"&&f.key()!="count"&&f.key()!="post_only"&&f.key()!="note"&&f.key()!="reviewed_ts"&&f.key()!="decision_id")throw std::runtime_error("unsupported queued ticket field: "+f.key());
  const auto decision_id=input.value("decision_id","");
  if(!decision_id.empty()&&!std::regex_match(decision_id,std::regex("d[0-9a-f]{16}")))throw std::runtime_error("decision_id is not a decision id");
  const auto ticker=input.value("ticker","");const auto side=input.value("outcome_side","yes");
  const double price=input.value("price",0.),count=input.value("count",0.);
  if(ticker.empty()||ticker.size()>120||(side!="yes"&&side!="no")||!std::isfinite(price)||!std::isfinite(count)||price<=0||price>=1||count<.01||count>100000||std::fabs(count-floor_contracts(count))>1e-8)throw std::runtime_error("invalid queued ticket");
  const long long reviewed=input.value("reviewed_ts",now);
  json payload={{"reviewed_ts",reviewed},{"ticker",ticker},{"outcome_side",side},{"price",price},{"count",count},{"post_only",input.value("post_only",true)},{"note",input.value("note","manual dashboard review")}};
  if(!decision_id.empty())payload["decision_id"]=decision_id;
  if(payload["note"].get<std::string>().size()>1000)throw std::runtime_error("ticket note too long");
  std::lock_guard<std::mutex> lock(mu_);
  const auto existing=store_.query("SELECT * FROM manual_requests WHERE id=?",{id});
  if(!existing.empty()) {
    if(existing[0]["payload"]!=payload.dump())throw std::runtime_error("request ID already belongs to a different ticket");
    return existing[0];
  }
  if(reviewed>now+5||now>=reviewed+90)throw std::runtime_error("Manual review expired; review a fresh quote");
  if(!decision_id.empty()&&precheck_)precheck_(payload);
  if(store_.kv_get("kill").value_or("0")=="1")throw std::runtime_error("Desk halted; review the risk controls before queueing");
  auto market=store_.get_market(ticker);if(!market)throw std::runtime_error("unknown market "+ticker);
  if(!store_.query("SELECT id FROM manual_requests WHERE event_ticker=? AND status IN ('queued','processing','unknown')",{market->event_ticker}).empty())throw std::runtime_error("A manual request already reserves this event; review it in the queue");
  if(store_.query("SELECT id FROM manual_requests WHERE status IN ('queued','processing','unknown')").size()>=20)throw std::runtime_error("Manual queue is full");
  store_.exec("INSERT INTO manual_requests VALUES(?,?,?,?,'queued',?,?,?,'{}','')",{id,payload.dump(),ticker,market->event_ticker,now,reviewed+90,now});
  return store_.query("SELECT * FROM manual_requests WHERE id=?",{id})[0];
}
json ManualOrders::view() {return store_.query("SELECT * FROM manual_requests ORDER BY created_ts DESC,id DESC LIMIT 100");}
void ManualOrders::finish(const std::string& id,const std::string& status,const json& result,const std::string& error,long long now) {
  store_.exec("UPDATE manual_requests SET status=?,result=?,error=?,updated_ts=? WHERE id=?",{status,result.dump(),error,now,id});
}
json ManualOrders::cancel(const std::string& id,long long now) {
  std::lock_guard<std::mutex> lock(mu_);
  auto r=store_.query("SELECT * FROM manual_requests WHERE id=?",{id});if(r.empty())throw std::runtime_error("unknown manual request");
  if(r[0]["status"]!="queued")throw std::runtime_error("Only an unsent queued request can be canceled here; processing/submitted orders must be reconciled");
  finish(id,"canceled",json::object(),"Canceled before dispatch",now);
  return store_.query("SELECT * FROM manual_requests WHERE id=?",{id})[0];
}
void ManualOrders::process(const Send& send,const Lookup& lookup,long long now) {
  std::unique_lock<std::mutex> worker(worker_mu_,std::try_to_lock);if(!worker.owns_lock())return;
  // Recovery reads the original exchange client ID, never the aggregate position on the ticker.
  for(const auto& r:store_.query("SELECT * FROM manual_requests WHERE status IN ('processing','unknown')")) {
    const auto id=r["id"].get<std::string>();auto found=lookup(id);
    if(found.value("status","")=="submitted")finish(id,"submitted",found,"",now);
    else if(found.value("status","")=="failed")finish(id,"failed",found,found.value("error","Exchange refused"),now);
    else finish(id,"unknown",found,"Delivery not confirmed; reconciliation required. This request is not retried.",now);
  }
  json r;
  {
    std::lock_guard<std::mutex> lock(mu_);
    store_.exec("UPDATE manual_requests SET status='expired',error='Review expired before dispatch; review a fresh quote',updated_ts=? WHERE status='queued' AND expires_ts<=?",{now,now});
    if(store_.kv_get("kill").value_or("0")=="1") {
      store_.exec("UPDATE manual_requests SET status='failed',error='Desk halted before dispatch; no order sent',updated_ts=? WHERE status='queued'",{now});return;
    }
    auto rows=store_.query("SELECT * FROM manual_requests WHERE status='queued' ORDER BY created_ts,id LIMIT 1");if(rows.empty())return;r=rows[0];
    finish(r["id"],"processing",json::object(),"Rechecking price, collateral and ownership",now);
  }
  const auto id=r["id"].get<std::string>();
  try {auto result=send(json::parse(r["payload"].get<std::string>()),id,r["expires_ts"].get<long long>());finish(id,"submitted",result,"",now);}
  catch(const RetryManualOrder& e) {finish(id,"queued",json::object(),e.what(),now);}
  catch(const std::exception& e) {
    const auto result=lookup(id);const auto status=result.value("status","");
    if(status=="submitted")finish(id,"submitted",result,"",now);
    else finish(id,(status=="sending"||status=="unknown")?"unknown":"failed",result,e.what(),now);
  }
}
} // namespace desk
