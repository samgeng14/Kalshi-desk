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
#include <set>
#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/queue.h"

namespace deskd {

class KillSwitch;
class Worker;
class ManualBook;
class SysBook;
struct ManualPath;
struct KalshiWriteReply;
struct PrivateEvent;
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
  // Any thread: the cids gw's order book owns. Their rows come from gw alone (contract ORDER
  // REVISIONS: the page keeps the highest rev), so the REST publisher leaves them out.
  std::shared_ptr<const std::set<std::string>> manual_cids() const { return std::atomic_load(&owned_); }
  // Why the Manual order ledger stopped ("" while healthy): any thread, for /ops/status.
  std::string ledger_why() const {
    const auto why = std::atomic_load(&ledger_why_);
    return why ? *why : std::string();
  }

  // Any thread: the GROUPS preflight row (every lane that is on has an active order group on every
  // shard), as gw last saw it.
  bool groups_ready(std::string* why) const {
    const auto g = std::atomic_load(&groups_view_);
    if (why) *why = g ? g->second : "Systematic is not attached";
    return g && g->first;
  }
  // Any thread: what strat needs from gw's book, republished as it changes (at most every 100 ms).
  struct SysOrder {
    std::string cid, lane, ticker;
    Side side = Side::BUY;
    Px price = 0;
    Qty count = 0, left = 0, filled = 0;
    OrdState state = OrdState::PEND;
    bool terminal = false, cancel_requested = false;
    std::int64_t done_ms = 0;
  };
  struct SysView {
    RunState run = RunState::STOP;
    std::set<std::string> lanes_on;
    std::map<std::string, SysOrder> orders;           // working ones, and those ended in the last 2 minutes
    std::map<std::string, Qty> positions;             // "lane|ticker" -> the lane's signed position
    // the header's Systematic numbers (contract hdr.day.sys, hdr.sys.K, hdr.cost), micro-dollars:
    // its day (real net of fees + unrl at the mids), its fees today, its used worst case, the 70%
    // ceiling and the effective ceiling min(ceiling, used + free)
    std::int64_t day = 0, fees = 0, used = 0, ceiling = 0, eff = 0;
    std::map<std::string, std::int64_t> lane_day;     // each lane's day, micro-dollars
    std::int64_t ts_ms = 0;
  };
  // The daily cost budget of the IOC-style lanes (contract hdr.cost.budget): past it they are CAPPED.
  static constexpr Money kCostBudget = 5 * 10000;
  std::shared_ptr<const SysView> sys_view() const { return std::atomic_load(&sys_view_); }

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
    std::atomic<std::uint64_t> private_applied{0};    // private order updates and fills applied to Manual orders
    std::atomic<std::uint64_t> sys_sent{0};           // Systematic orders handed to send-k2
    std::atomic<std::uint64_t> sys_refused{0};        // Systematic intents refused by gw
    std::atomic<std::uint64_t> sys_cancels{0};        // Systematic cancels handed to send-k2
    std::atomic<std::uint64_t> group_jobs{0};         // order-group creates, triggers and resets
  };
  const Stats& stats() const { return stats_; }

 private:
  void handle(GwMsg& m);
  // applies a KILL or re-arm the switch shows and gw has not seen yet; true when it applied a KILL
  bool sync_kill();
  void publish();
  void place_manual(PlaceIntent& p);
  void manual_reply(const KalshiWriteReply& r);
  void manual_private(const PrivateEvent& e);
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
  std::set<std::string> cancel_sent_;            // cids whose cancel went to the cancel sender
  std::map<std::string, std::string> cancel_why_;   // a cancel gw itself asked for: STOP, LANE_OFF, HALT
  std::shared_ptr<const std::set<std::string>> owned_;   // std::atomic_load / std::atomic_store
  std::shared_ptr<const std::string> ledger_why_;        // std::atomic_load / std::atomic_store
  void note_ledger();

  // ---- Systematic (gw/sys_book.h); gw thread only
  std::unique_ptr<SysBook> sys_;
  std::shared_ptr<const std::pair<bool, std::string>> groups_view_;   // std::atomic_load / std::atomic_store
  std::shared_ptr<const SysView> sys_view_;                          // std::atomic_load / std::atomic_store
  std::map<std::string, std::int64_t> lane_published_;   // lane id -> when its row last went out
  std::set<std::string> lanes_dirty_;
  std::int64_t sys_view_ms_ = 0, groups_paced_ms_ = 0, sys_day_ms_ = 0;
  bool sys_dirty_ = true;
  void place_sys(PlaceIntent& p);
  void group_reply(const KalshiWriteReply& r);
  void group_job(const std::string& lane, int shard, const std::string& action);
  int cancel_sys(const std::function<bool(const std::string& lane)>& which, const char* why);
  void trigger_groups();
  void reset_groups();
  void sys_tick(std::int64_t now);
  void halt(const std::string& why);
  std::set<int> shards() const;
  std::optional<Px> mark(const std::string& ticker) const;
  std::string lane_row(const std::string& id) const;
  // Systematic's used worst case (open positions + working reservations) and its ceilings
  void sys_money(std::int64_t* used, std::int64_t* ceiling, std::int64_t* eff) const;   // micro-dollars
  std::int64_t sys_fees() const;
  void sync_snapshot();
};

}  // namespace deskd
