#pragma once
// gw, the gateway (plan section 2): the one owner of the risk book, reservations, the cid
// registry, order states, the UNKNOWN table, the write buckets and the Systematic-running flag.
// It never blocks: it reads its two-lane inbox, and everything it says leaves as a message.
//
// Week 0 holds the run state and the queue rules only. There is no venue path yet, so every
// new order is refused, but the lane order and the deletions are the real ones: KILL, Stop and
// cancels overtake new orders and delete the ones still queued behind them.
//
// KILL and re-arm reach gw through the kill switch, not its inbox: gw compares the switch's
// generations before it handles each message (and the trip kicks its inbox awake), so a KILL
// deletes every queued new order and Start before the next one is looked at, and a re-arm is
// applied on this thread before any later Start sees the switch clear.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <memory>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/queue.h"

namespace deskd {

class KillSwitch;
class Worker;
class ManualBook;
struct ManualPath;
struct KalshiWriteReply;
namespace rest { struct Snapshot; }

class Gateway {
 public:
  using PreflightFn = std::function<std::vector<PreflightRow>()>;

  // registers its inbox's kick with kill.on_trip: construct it before any thread can trip kill,
  // and keep kill alive longer than it
  Gateway(KillSwitch& kill, PreflightFn preflight, std::size_t normal_cap = 4096, std::size_t priority_cap = 1024);
  ~Gateway();

  // Turns on the Manual order path (gw/manual_book.h). Call before run(); without it every new
  // order is refused RO, as in week 0.
  void attach_manual(const ManualPath& path);
  // gw thread only (inside a Probe): the Manual book, or nullptr
  ManualBook* manual_book() { return manual_.get(); }

  // queues m in its lane (lane_of); false when that lane is full: dropped, gap marked
  bool post(GwMsg m);
  BoundedQueue<GwMsg>& inbox() { return inbox_; }

  void run(Worker& self);   // the gw thread

  // KILLED from the kill switch (it never waits for gw), else gw's own state as last published
  RunView run_view() const;

  struct Stats {
    std::atomic<std::uint64_t> handled{0};
    std::atomic<std::uint64_t> place_refused{0};
    std::atomic<std::uint64_t> erased_by_cancel{0};   // queued new orders a cancel deleted
    std::atomic<std::uint64_t> erased_by_stop{0};     // queued SYS orders and Starts a Stop deleted
    std::atomic<std::uint64_t> erased_by_kill{0};     // queued new orders and Starts a KILL deleted
    std::atomic<std::uint64_t> kills_seen{0};         // KILL generations gw has applied
    std::atomic<std::uint64_t> manual_sent{0};        // Manual orders handed to send-k1
    std::atomic<std::uint64_t> manual_cancels{0};     // Manual cancels handed to the cancel sender
  };
  const Stats& stats() const { return stats_; }

 private:
  void handle(GwMsg& m);
  // applies a KILL or re-arm the switch shows and gw has not seen yet; true when it applied a KILL
  bool sync_kill();
  void publish();
  void place_manual(PlaceIntent& p);
  void manual_reply(const KalshiWriteReply& r);
  std::vector<std::string> cancel_manual(const CancelIntent& c);
  void send_cancel(const std::string& cid);
  void publish_order(const std::string& cid);
  std::string order_json(const std::string& cid) const;

  KillSwitch& kill_;
  PreflightFn preflight_;
  BoundedQueue<GwMsg> inbox_;
  Stats stats_;
  RunView own_;                                   // gw thread only
  std::uint64_t kill_gen_seen_ = 0, kill_state_seen_ = 0;   // gw thread only
  std::shared_ptr<const RunView> view_;          // std::atomic_load / std::atomic_store
  std::unique_ptr<ManualPath> path_;             // the rest: gw thread only
  std::unique_ptr<ManualBook> manual_;
  std::shared_ptr<const rest::Snapshot> snapshot_seen_;
  std::map<std::string, OrderPromise> pending_;  // web threads waiting on a Manual order's write
};

}  // namespace deskd
