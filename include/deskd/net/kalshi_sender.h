#pragma once
// Dedicated Manual and cancel workers. A job is attempted once on a fresh pooled connection;
// neither an uncertain write nor a duplicate cid is retried. All callbacks must be nonblocking.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <optional>
#include "deskd/bus/queue.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/pool.h"
#include "deskd/net/send.h"

namespace deskd {
class KillSwitch;
class KalshiSigner;
class Worker;
struct KalshiWriteJob {
  enum class Kind { CREATE, CANCEL, GROUP, TRANSFER } kind = Kind::CREATE;
  kalshi::CreateOrder create;
  std::string cid, oid, ticker;
  int shard = -1, subaccount = -1;
  // GROUP (Systematic's sender only): "create" with group_limit, or "trigger" / "reset" of
  // group_id; lane says whose group it is, for the reply.
  std::string group_action, group_id, lane;
  Qty group_limit = 0;
  // TRANSFER (the Manual sender only): free cash from shard to transfer_to (shard is the source)
  int transfer_to = -1;
  Money amount = 0;
  // Required for CREATE. The gateway revokes this permit on a cancel, stale book or KILL.
  // Even a permit still true cannot outlive its deadline or cross a KILL generation.
  std::shared_ptr<std::atomic<bool>> permit;
  std::uint64_t kill_generation = 0;
  std::int64_t valid_until_mono_ms = 0;
};
struct KalshiWriteReply {
  std::string cid, local_code;
  WireOp op = WireOp::OTHER;
  kalshi::WriteOutcome outcome;
  std::optional<kalshi::CreateAck> create;
  std::optional<kalshi::CancelAck> cancel;
  std::string group_action, group_id, lane;   // GROUP: what was asked; create's ack fills group_id
  int shard = -1;   // the job's
  std::int64_t entered_ms = 0, sent_ms = 0, ack_ms = 0;
  net::Exchange exchange; // monotonic write/first-byte/done timeline; never logged with its body
  bool pool_miss = false;
};
// Every order group's id and shard, for KILL: send-cx triggers each one after its cancel-all,
// without asking gw (plan section 2: send-cx keeps its own copy of every order-group id).
class GroupRegistry {
 public:
  void add(const std::string& id, int shard) { std::lock_guard<std::mutex> lk(m_); ids_[id] = shard; }
  void remove(const std::string& id) { std::lock_guard<std::mutex> lk(m_); ids_.erase(id); }
  std::map<std::string, int> all() const { std::lock_guard<std::mutex> lk(m_); return ids_; }
 private:
  mutable std::mutex m_;
  std::map<std::string, int> ids_;
};
class KalshiSender {
 public:
  // MANUAL: send-k1, M creates. CANCEL: send-cx, cancels and KILL. SYSTEMATIC: send-k2, S creates,
  // S cancels and order-group actions; never KILL's cancel-all.
  enum class Role { MANUAL, CANCEL, SYSTEMATIC };
  struct Options { int connect_ms = 1000, reply_ms = 1500; };
  using ReplyFn = std::function<bool(KalshiWriteReply)>;
  using AlertFn = std::function<void(const std::string&)>;
  // Construct before threads start: registers a KILL wake with the inbox. The inbox, pool,
  // signer and switch must outlive this worker; the caller starts/stops its separate pool.
  KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
               const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert);
  KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
               const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert, Options options);
  // CANCEL role: also report each KILL generation taken (before its cancel-all goes out) into
  // stats, which the web's KILL route waits on. Call before run().
  void set_stats(SenderStats* stats) { stats_ = stats; }
  // CANCEL role: the groups KILL triggers after its cancel-all. Call before run().
  void set_groups(GroupRegistry* groups) { groups_ = groups; }
  void run(Worker& self);
  // A failure trips KILL and stops this sender (RO) until the venue confirms a cancel-all sent
  // after it and KILL is re-armed: the operator has seen it and the venue holds none of our orders.
  bool healthy() const;
  std::string fail_reason() const;   // the last failure ("" if none); kept after recovery
  std::uint64_t kill_seen() const { return kill_seen_.load(); }
  std::uint64_t attempted() const { return attempted_.load(); }
 private:
  Role role_;
  BoundedQueue<KalshiWriteJob>& inbox_;
  net::SparePool& pool_;
  const KalshiSigner& signer_;
  KillSwitch& kill_;
  ReplyFn reply_;
  AlertFn alert_;
  Options options_;
  std::atomic<bool> failed_{false};
  std::atomic<std::uint64_t> failed_gen_{0};
  mutable std::mutex why_m_;
  std::string why_;
  SenderStats* stats_ = nullptr;
  GroupRegistry* groups_ = nullptr;
  std::atomic<std::uint64_t> kill_seen_{0}, attempted_{0};
  std::string admission(const KalshiWriteJob& job, const Worker& self) const;
  KalshiWriteReply execute(kalshi::WireRequest request, const std::string& cid,
                          Worker& self, const KalshiWriteJob* job);
  void deliver(KalshiWriteReply reply);
  void take_kill(Worker& self);
  void fail_closed(const std::string& why);
};
}
