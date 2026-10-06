#pragma once
// gw, the gateway (plan section 2): the one owner of the risk book, reservations, the cid
// registry, order states, the UNKNOWN table, the write buckets and the Systematic-running flag.
// It never blocks: it reads its two-lane inbox, and everything it says leaves as a message.
//
// Week 0 holds the run state and the queue rules only. There is no venue path yet, so every
// new order is refused, but the lane order and the deletions are the real ones: Kill, Stop and
// cancels overtake new orders and delete the ones still queued behind them.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/queue.h"

namespace deskd {

class KillSwitch;
class Worker;

class Gateway {
 public:
  using PreflightFn = std::function<std::vector<PreflightRow>()>;

  Gateway(KillSwitch& kill, PreflightFn preflight, std::size_t normal_cap = 4096, std::size_t priority_cap = 1024);

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
    std::atomic<std::uint64_t> erased_by_kill{0};     // queued new orders and Starts a Kill deleted
  };
  const Stats& stats() const { return stats_; }

 private:
  void handle(GwMsg& m);
  void publish();

  KillSwitch& kill_;
  PreflightFn preflight_;
  BoundedQueue<GwMsg> inbox_;
  Stats stats_;
  RunView own_;                                   // gw thread only
  std::shared_ptr<const RunView> view_;          // std::atomic_load / std::atomic_store
};

}  // namespace deskd
