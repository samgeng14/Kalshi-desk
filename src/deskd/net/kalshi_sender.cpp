#include "deskd/net/kalshi_sender.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
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
namespace {
WireOp op_of(KalshiWriteJob::Kind k) {
  using JK=KalshiWriteJob::Kind;
  return k==JK::CREATE ? WireOp::CREATE : k==JK::CANCEL ? WireOp::CANCEL : k==JK::TRANSFER ? WireOp::TRANSFER : WireOp::GROUP;
}
}  // namespace
std::string KalshiSender::admission(const KalshiWriteJob& j,const Worker& self) const {
  if(self.stopping()) return "STOP";
  if(j.kind==KalshiWriteJob::Kind::TRANSFER) {   // the ticket's MOVE: send-k1, never during KILL
    if(role_!=Role::MANUAL || j.cid.empty()) return "BAD_REQUEST";
    if(kill_.active() || j.kill_generation!=kill_.generation()) return "KILLED";
    return healthy() ? "" : "RO";
  }
  if(j.kind==KalshiWriteJob::Kind::CANCEL)
    return !j.cid.empty() && (role_==Role::CANCEL || (role_==Role::SYSTEMATIC && j.cid.front()=='S')) ? "" : "BAD_REQUEST";
  if(j.kind==KalshiWriteJob::Kind::GROUP) {
    if(role_!=Role::SYSTEMATIC || j.lane.empty()) return "BAD_REQUEST";
    // a trigger always goes; a create or reset never during KILL, nor from a stopped sender
    if(j.group_action=="trigger") return "";
    if(kill_.active() || j.kill_generation!=kill_.generation()) return "KILLED";
    return healthy() ? "" : "RO";
  }
  const char own=role_==Role::MANUAL ? 'M' : role_==Role::SYSTEMATIC ? 'S' : 0;
  if(!own || j.cid.empty() || j.cid.front()!=own || j.create.client_order_id!=j.cid)
    return "BAD_REQUEST";
  if(kill_.active() || j.kill_generation!=kill_.generation()) return "KILLED";
  if(!healthy()) return "RO";
  if(!j.permit || !j.permit->load()) return "USER";
  if(j.valid_until_mono_ms<=mono_ms()) return "STALE";
  if(j.create.expiration_s && j.create.expiration_s<=now_ms()/1000+1) return "EXPIRY";
  return "";
}
bool KalshiSender::healthy() const {
  if(!failed_.load()) return true;
  return !kill_.active() && kill_.confirmed_gen()>=failed_gen_.load();
}
std::string KalshiSender::fail_reason() const {
  std::lock_guard<std::mutex> lk(why_m_);
  return why_;
}
void KalshiSender::fail_closed(const std::string& why) {
  { std::lock_guard<std::mutex> lk(why_m_); why_=why; }
  // A Manual failure always sends a fresh cancel-all, KILL active or not, so recovery waits on
  // one that went out after it. The cancel sender never re-trips itself: an unconfirmed
  // cancel-all would retry without end; the operator presses KILL again instead.
  if(role_==Role::MANUAL || !kill_.active()) kill_.trip(now_ms(),mono_ms(),why);
  // Never lowered: a newer failure needs a newer cancel-all.
  for(auto g=kill_.generation(), old=failed_gen_.load(); g>old && !failed_gen_.compare_exchange_weak(old,g);) {}
  if(role_==Role::CANCEL && kill_.confirmed_gen()>=failed_gen_.load()) failed_gen_.fetch_add(1);
  failed_.store(true);
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
    if(request.op==WireOp::GROUP && job && job->group_action=="create") {
      if(auto id=kalshi::parse_create_order_group(r.exchange.res.body,&why)) r.group_id=*id;
      else { r.outcome.kind=kalshi::WriteOutcome::Kind::UNKNOWN; r.outcome.why="order group created without a readable id"; }
    }
  }
  if(r.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED && r.outcome.cls==RejectClass::DUPLICATE) {
    // A duplicate is evidence an earlier request may have landed. Never free the reservation
    // or silently resend under a fresh cid; the gateway must locate the original by cid.
    r.outcome.kind=kalshi::WriteOutcome::Kind::UNKNOWN;
  }
  // a refused transfer (not enough free cash, transfers paused) is the operator's to read, not a
  // fault in the order path
  if(r.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED && reject_trips_kill(r.outcome.cls) && request.op!=WireOp::TRANSFER)
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
  if(stats_) {
    stats_->kills.fetch_add(1); stats_->last_kill_ms.store(now_ms()); stats_->kill_gen_seen.store(generation);
  }
  auto reply=execute(kalshi::build_cancel_all(),"",self,nullptr);
  // A 429 is refused before the venue processed anything, so sending again is safe; a cancel-all
  // costs a whole batch's write tokens, so a KILL right after a burst of clicks can meet one.
  // Waits honour Retry-After and stop as soon as a newer KILL takes over.
  for(int attempt=0; attempt<5 && reply.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED &&
      reply.outcome.cls==RejectClass::RETRYABLE && kill_.generation()==generation && !self.stopping(); ++attempt) {
    std::int64_t wait=250LL<<attempt;
    const auto after=reply.exchange.res.header("retry-after");
    if(!after.empty() && after.size()<3 && std::all_of(after.begin(),after.end(),[](unsigned char c){return std::isdigit(c);}))
      wait=std::max<std::int64_t>(wait,std::min<std::int64_t>(5000,std::stoll(after)*1000));
    DLOG_WARN("KILL cancel-all answered 429; sending again in %lld ms",static_cast<long long>(wait));
    const auto until=mono_ms()+wait;
    while(mono_ms()<until && kill_.generation()==generation && !self.stopping()) {
      self.beat(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(kill_.generation()!=generation || self.stopping()) break;
    reply=execute(kalshi::build_cancel_all(),"",self,nullptr);
  }
  if(reply.outcome.kind==kalshi::WriteOutcome::Kind::ACK) kill_.cancel_all_done(now_ms(),mono_ms(),generation);
  else if(kill_.generation()==generation)   // a newer KILL is about to send its own
    fail_closed("KILL cancel-all unconfirmed ("+(reply.outcome.why.empty() ? reply.local_code : reply.outcome.why).substr(0,120)+
                "); reconcile and press KILL again");
  deliver(std::move(reply));
  // Then every order group: a triggered group refuses new orders until gw resets it at re-arm.
  if(groups_) for(const auto& [id,shard]:groups_->all()) {
    kalshi::WireRequest request; std::string why;
    if(!kalshi::build_order_group_action(id,"trigger",shard,-1,&request,&why)) continue;
    auto g=execute(std::move(request),"",self,nullptr);
    g.group_action="trigger"; g.group_id=id; g.shard=shard;
    if(g.outcome.kind!=kalshi::WriteOutcome::Kind::ACK && !(g.outcome.kind==kalshi::WriteOutcome::Kind::REFUSED && g.outcome.cls==RejectClass::GONE))
      DLOG_WARN("KILL: order group %s on shard %d not triggered: %s",id.c_str(),shard,g.outcome.why.c_str());
    deliver(std::move(g));
  }
}
void KalshiSender::run(Worker& self) {
  while(!self.stopping()) {
    self.beat(); take_kill(self);
    auto job=inbox_.pop_for(std::chrono::milliseconds(100));
    self.beat(); take_kill(self);
    if(!job) continue;
    kalshi::WireRequest request; std::string why;
    const auto code=admission(*job,self);
    using JK=KalshiWriteJob::Kind;
    bool built=false;
    if(code.empty()) {
      if(job->kind==JK::CREATE) built=kalshi::build_create(job->create,&request,&why);
      else if(job->kind==JK::CANCEL) built=kalshi::build_cancel(job->oid,job->shard,job->ticker,job->subaccount,&request,&why);
      else if(job->kind==JK::TRANSFER) built=kalshi::build_transfer(job->shard,job->transfer_to,job->amount,&request,&why);
      else if(job->group_action=="create") built=kalshi::build_create_order_group(job->group_limit,job->shard,job->subaccount,&request,&why);
      else built=kalshi::build_order_group_action(job->group_id,job->group_action,job->shard,job->subaccount,&request,&why);
    }
    KalshiWriteReply r;
    if(!built) {
      r.cid=job->cid; r.entered_ms=now_ms();
      r.op=op_of(job->kind);
      r.outcome.kind=kalshi::WriteOutcome::Kind::NOT_SENT;
      r.local_code=code.empty() ? "BAD_REQUEST" : code; r.outcome.why=why;
    } else r=execute(std::move(request),job->cid,self,&*job);
    if(job->kind==JK::GROUP) {
      r.group_action=job->group_action; r.lane=job->lane; r.shard=job->shard;
      if(r.group_id.empty()) r.group_id=job->group_id;
    }
    deliver(std::move(r));
  }
  // Queued jobs never disappear just because shutdown beat the worker to them.
  while(auto job=inbox_.try_pop()) {
    KalshiWriteReply r; r.cid=job->cid;
    r.op=op_of(job->kind);
    r.group_action=job->group_action; r.group_id=job->group_id; r.lane=job->lane; r.shard=job->shard;
    r.local_code="STOP"; r.outcome.kind=kalshi::WriteOutcome::Kind::NOT_SENT;
    deliver(std::move(r));
  }
}
}
