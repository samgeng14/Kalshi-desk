#pragma once
// Dedicated Manual and cancel workers. A job is attempted once on a fresh pooled connection;
// neither an uncertain write nor a duplicate cid is retried. All callbacks must be nonblocking.
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include "deskd/bus/queue.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/pool.h"

namespace deskd {
class KillSwitch;
class KalshiSigner;
class Worker;
struct KalshiWriteJob {
  enum class Kind { CREATE, CANCEL } kind = Kind::CREATE;
  kalshi::CreateOrder create;
  std::string cid, oid, ticker;
  int shard = -1, subaccount = -1;
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
  std::int64_t entered_ms = 0, sent_ms = 0, ack_ms = 0;
  net::Exchange exchange; // monotonic write/first-byte/done timeline; never logged with its body
  bool pool_miss = false;
};
class KalshiSender {
 public:
  enum class Role { MANUAL, CANCEL };
  struct Options { int connect_ms = 1000, reply_ms = 1500; };
  using ReplyFn = std::function<bool(KalshiWriteReply)>;
  using AlertFn = std::function<void(const std::string&)>;
  // Construct before threads start: registers a KILL wake with the inbox. The inbox, pool,
  // signer and switch must outlive this worker; the caller starts/stops its separate pool.
  KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
               const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert);
  KalshiSender(Role role, BoundedQueue<KalshiWriteJob>& inbox, net::SparePool& pool,
               const KalshiSigner& signer, KillSwitch& kill, ReplyFn reply, AlertFn alert, Options options);
  void run(Worker& self);
  bool healthy() const { return healthy_.load(); }
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
  std::atomic<bool> healthy_{true};
  std::atomic<std::uint64_t> kill_seen_{0}, attempted_{0};
  std::string admission(const KalshiWriteJob& job, const Worker& self) const;
  KalshiWriteReply execute(kalshi::WireRequest request, const std::string& cid,
                          Worker& self, const KalshiWriteJob* job);
  void deliver(KalshiWriteReply reply);
  void take_kill(Worker& self);
  void fail_closed(const std::string& why);
};
}
