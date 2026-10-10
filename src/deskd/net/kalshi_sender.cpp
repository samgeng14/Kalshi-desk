#include "deskd/net/kalshi_sender.h"
#include <algorithm>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kill.h"

namespace deskd {
KalshiSender::KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
    const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert)
    : KalshiSender(role,inbox,pool,signer,kill,std::move(reply),std::move(alert),Options{}) {}
KalshiSender::KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
    const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert, Options options)
    : role_(role),inbox_(inbox),pool_(pool),signer_(signer),kill_(kill),reply_(std::move(reply)),
      alert_(std::move(alert)),options_(options) {
  options_.connect_ms=std::clamp(options_.connect_ms,1,2000);
  options_.reply_ms=std::clamp(options_.reply_ms,1,2000);
  kill_.on_trip([&inbox]{inbox.kick();});
}
std::string KalshiSender::admission(const KalshiWriteJob& j,const Worker& self) const {
  if(self.stopping()) return "STOP";
  if(j.kind==KalshiWriteJob::Kind::CANCEL) return role_==Role::CANCEL && !j.cid.empty() ? "" : "BAD_REQUEST";
  if(role_!=Role::MANUAL || j.cid.empty() || j.cid.front()!='M' || j.create.client_order_id!=j.cid)
    return "BAD_REQUEST";
  if(kill_.active() || j.kill_generation!=kill_.generation()) return "KILLED";
  if(!healthy()) return "RO";
  if(!j.permit || !j.permit->load()) return "USER";
  if(j.valid_until_mono_ms<=mono_ms()) return "STALE";
  if(j.create.expiration_s && j.create.expiration_s<=now_ms()/1000+1) return "EXPIRY";
  return "";
}
void KalshiSender::fail_closed(const std::string& why) {
  healthy_.store(false);
  if(!kill_.active()) kill_.trip(now_ms(),mono_ms(),why);
  if(alert_) alert_(why);
}
void KalshiSender::deliver(KalshiWriteReply reply) {
  bool queued=false;
  try { queued=reply_ && reply_(std::move(reply)); } catch(...) { queued=false; }
  if(!queued) fail_closed("sender result queue lost an order update; reconcile before trading");
}
KalshiWriteReply KalshiSender::execute(kalshi::WireRequest request,const std::string& cid,
                                      Worker& self,const KalshiWriteJob* job) {
  KalshiWriteReply r; r.cid=cid; r.op=request.op; r.entered_ms=now_ms();
  r.outcome.kind=kalshi::WriteOutcome::Kind::NOT_SENT;
  auto refused=[&](const std::string& code,const std::string& why) {
    r.local_code=code; r.outcome.why=why; return r;
  };
  if(job) { const auto code=admission(*job,self); if(!code.empty()) return refused(code,"sender admission refused"); }
  self.beat();
  std::string why;
  auto conn=pool_.take(options_.connect_ms,&r.pool_miss,&why);
  self.beat();
  if(!conn) return refused("EXCHANGE","no guarded venue connection");
  // A cancel, KILL or deadline can overtake the handshake. Re-check before any bytes leave.
  if(job) { const auto code=admission(*job,self); if(!code.empty()) return refused(code,"permit expired before send"); }
  const auto& ep=pool_.config().endpoint;
  const std::string base=kalshi::kBasePath;
  if(request.path.compare(0,base.size(),base)!=0) return refused("INTERNAL","unexpected write path");
  request.path.replace(0,base.size(),ep.base_path);
  const auto headers=signer_.headers(now_ms(),request.method,request.path);
  if(headers.size()!=3) {
    fail_closed("venue request signing failed");
    return refused("RO","request signing failed");
  }
  const auto bytes=kalshi::request_bytes(request,ep.host_header(),headers);
  if(job) { const auto code=admission(*job,self); if(!code.empty()) return refused(code,"permit revoked while signing"); }
  r.sent_ms=now_ms(); ++attempted_;
  r.exchange=net::exchange(*conn,bytes,net::mono_us()+1000LL*options_.reply_ms,false,[&]{self.beat();});
  conn.reset(); // one write per connection, regardless of a keep-alive response
  r.ack_ms=now_ms(); self.beat();
  r.outcome=kalshi::classify_write(request.op,r.exchange);
  if(r.outcome.kind==kalshi::WriteOutcome::Kind::ACK) {
    if(request.op==WireOp::CREATE) r.create=kalshi::parse_create_ack(r.exchange.res.body,&why);
    if(request.op==WireOp::CANCEL) r.cancel=kalshi::parse_cancel_ack(r.exchange.res.body,&why);
  }
  if(r.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED && r.outcome.cls==RejectClass::DUPLICATE) {
    // A duplicate is evidence an earlier request may have landed. Never free the reservation
    // or silently resend under a fresh cid; the gateway must locate the original by cid.
    r.outcome.kind=kalshi::WriteOutcome::Kind::UNKNOWN;
  }
  if(r.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED && reject_trips_kill(r.outcome.cls))
    fail_closed("venue rejected a write as invalid or unauthorized; reconciliation required");
  return r;
}
void KalshiSender::take_kill(Worker& self) {
  if(role_!=Role::CANCEL) return;
  const auto generation=kill_.generation();
  if(!generation || generation==kill_seen_.load()) return;
  // Independent of gw and of every inbox slot. A second click advances the generation and
  // explicitly retries an unconfirmed cancel-all; uncertain writes are never silently retried.
  kill_seen_.store(generation);
  auto reply=execute(kalshi::build_cancel_all(),"",self,nullptr);
  if(reply.outcome.kind==kalshi::WriteOutcome::Kind::ACK) kill_.cancel_all_done(now_ms(),mono_ms());
  else fail_closed("KILL cancel-all unconfirmed; reconcile and press KILL again");
  deliver(std::move(reply));
}
void KalshiSender::run(Worker& self) {
  while(!self.stopping()) {
    self.beat(); take_kill(self);
    auto job=inbox_.pop_for(std::chrono::milliseconds(100));
    self.beat(); take_kill(self);
    if(!job) continue;
    kalshi::WireRequest request; std::string why;
    const auto code=admission(*job,self);
    const bool built=code.empty() && (job->kind==KalshiWriteJob::Kind::CREATE ?
        kalshi::build_create(job->create,&request,&why) :
        kalshi::build_cancel(job->oid,job->shard,job->ticker,job->subaccount,&request,&why));
    if(!built) {
      KalshiWriteReply r; r.cid=job->cid; r.entered_ms=now_ms();
      r.op=job->kind==KalshiWriteJob::Kind::CREATE ? WireOp::CREATE : WireOp::CANCEL;
      r.outcome.kind=kalshi::WriteOutcome::Kind::NOT_SENT;
      r.local_code=code.empty() ? "BAD_REQUEST" : code; r.outcome.why=why;
      deliver(std::move(r));
    } else deliver(execute(std::move(request),job->cid,self,&*job));
  }
  // Queued jobs never disappear just because shutdown beat the worker to them.
  while(auto job=inbox_.try_pop()) {
    KalshiWriteReply r; r.cid=job->cid;
    r.op=job->kind==KalshiWriteJob::Kind::CREATE ? WireOp::CREATE : WireOp::CANCEL;
    r.local_code="STOP"; r.outcome.kind=kalshi::WriteOutcome::Kind::NOT_SENT;
    deliver(std::move(r));
  }
}
}
