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
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/queue.h"
#include "deskd/gw/write_budget.h"

namespace deskd {

class KillSwitch;
class Worker;
class ManualBook;
class SysBook;
struct ManualPath;
struct KalshiWriteReply;
struct PrivateEvent;
namespace rest { struct Snapshot; }
namespace kalshi { struct FillRow; }

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
    std::set<std::string> held;                       // markets Manual took over: no lane quotes them
    std::int64_t ts_ms = 0;
  };
  // The daily cost budget of the IOC-style lanes (contract hdr.cost.budget): past it they are CAPPED.
  static constexpr Money kCostBudget = 5 * 10000;
  std::shared_ptr<const SysView> sys_view() const { return std::atomic_load(&sys_view_); }
  // After attach_manual: when the restored Systematic book was saved (0: none). The REST read
  // goes back that far for orders, so the last run's fills can still be told apart by client id.
  std::int64_t sys_saved_ms() const { return sys_saved_ms_; }
  // Any thread: the markets Systematic holds positions in (REST reads them until they settle)
  std::shared_ptr<const std::set<std::string>> sys_held() const { return std::atomic_load(&sys_held_); }
  // Any thread: the markets Manual took over (contract mkt.man): a manual order where Systematic
  // quotes or holds, or that a lane which is on trades, cancels Systematic's orders there (why
  // TAKEOVER), hands the lanes' inventory to Manual at the mid, and keeps the lanes out until
  // POST /api/release (plan section 4)
  std::shared_ptr<const std::set<std::string>> manual_held() const { return std::atomic_load(&man_view_); }

  // Any thread: the order path's latency (stage 4: click-to-ack p90 under 100 ms), over the last
  // 200 creates the venue acknowledged: click (the page's clock) to ack, deskd's receipt to ack,
  // and the write on the wire; ms. ack20_p50 is the contract's hdr.lat.gw_ack_ms (ack - receive,
  // p50 of the last 20).
  struct Latency {
    int n = 0;
    std::optional<int> click_ack_p50, click_ack_p90, recv_ack_p50, recv_ack_p90, wire_p50, wire_p90, ack20_p50;
  };
  std::shared_ptr<const Latency> latency() const { return std::atomic_load(&latency_); }

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
    std::atomic<std::uint64_t> group_reset_fallbacks{0};   // resets taken on their 200 (no socket word)
    std::atomic<std::uint64_t> manual_sent{0};        // Manual orders handed to send-k1
    std::atomic<std::uint64_t> manual_cancels{0};     // Manual cancels handed to the cancel sender
    std::atomic<std::uint64_t> private_applied{0};    // private order updates and fills applied to Manual orders
    std::atomic<std::uint64_t> sys_sent{0};           // Systematic orders handed to send-k2
    std::atomic<std::uint64_t> sys_refused{0};        // Systematic intents refused by gw
    std::atomic<std::uint64_t> sys_cancels{0};        // Systematic cancels handed to send-k2
    std::atomic<std::uint64_t> group_jobs{0};         // order-group creates, triggers and resets
    std::atomic<std::uint64_t> sys_rate_refused{0};   // Systematic creates past its 80% of a shard's write budget
    std::atomic<std::uint64_t> venue_429{0};          // writes the venue refused 429
    std::atomic<std::uint64_t> records_sent{0};       // order timelines and fills handed to store
    std::atomic<std::uint64_t> records_lost{0};       // ...that store's full inbox refused
  };
  const Stats& stats() const { return stats_; }
  // Any thread: each write bucket's tokens as gw last spent from it (shard -> tokens; 0 = the
  // unscoped bucket) and the bucket's size (gw/write_budget.h)
  std::shared_ptr<const std::map<int, double>> write_tokens() const { return std::atomic_load(&write_view_); }
  double write_cap() const { return write_cap_.load(); }

 private:
  void handle(GwMsg& m);
  // applies a KILL or re-arm the switch shows and gw has not seen yet; true when it applied a KILL
  bool sync_kill();
  void publish();
  void place_manual(PlaceIntent& p);
  void manual_reply(const KalshiWriteReply& r);
  void manual_private(const PrivateEvent& e);
  std::vector<std::string> cancel_manual(const CancelIntent& c);
  // Resting orders on the account this run did not place (a previous run's, the website's): a
  // cancel that covers them cancels them by order id on send-cx; their cids as the page shows them
  std::vector<std::string> cancel_foreign(const CancelIntent& c);
  std::map<std::string, std::int64_t> foreign_cx_;   // page cid -> when its cancel was queued
  struct PendingTransfer { std::shared_ptr<std::promise<TransferReply>> reply; int from = -1, to = -1; Money amount = 0; };
  std::map<std::string, PendingTransfer> transfers_;   // "T<n>" -> the page waiting on it
  std::uint64_t transfer_seq_ = 0;
  void transfer(TransferIntent& t);
  void transfer_reply(const KalshiWriteReply& r);
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
  WriteBudget budget_;                                    // gw thread only
  std::shared_ptr<const std::map<int, double>> write_view_;   // std::atomic_load / std::atomic_store
  std::atomic<double> write_cap_{0};
  void spend(int shard, double cost);   // a write sent: counted against its bucket, and published
  // Each order's stamps the ledger does not keep (it has click, recv, sent, ack, done): gw's
  // admission, the create's wire segments, the private feed's first word and first fill, the
  // cancel asked and answered. The whole timeline goes to store when the order ends (plan
  // section 9, "every order logs its timeline"; tools/latency_report.py reads it).
  struct Stamps {
    std::int64_t gw = 0, priv = 0, fill = 0, cx = 0, cx_ack = 0;
    double w_ms = -1, fb_ms = -1, rd_ms = -1;   // start to written, written to first byte, to done
    bool replied = false;   // the create's reply is in (a fill can end an IOC before it): then it is recorded
  };
  std::map<std::string, Stamps> stamps_;   // gw thread only; an order's entry leaves when it is recorded
  // Recorded once the order has ended, its create's reply is in, the private feed has spoken of it
  // (and of its fill, if it filled), and a cancel sent has its reply: the venue's replies and its
  // socket race. `force`: the
  // order ended over 5 s ago, and what has not come is not coming (gw's tick).
  void record_if_done(const std::string& cid, bool force = false);
  std::int64_t timelines_swept_ms_ = 0;
  void record(const char* table, std::string json);
  void note_ledger();

  // ---- Systematic (gw/sys_book.h); gw thread only
  std::unique_ptr<SysBook> sys_;
  std::shared_ptr<const std::pair<bool, std::string>> groups_view_;   // std::atomic_load / std::atomic_store
  std::shared_ptr<const SysView> sys_view_;                          // std::atomic_load / std::atomic_store
  std::shared_ptr<const Latency> latency_;                           // std::atomic_load / std::atomic_store
  struct LatSample { int click_ack = -1, recv_ack = 0, wire = 0; };
  std::deque<LatSample> lat_;                                         // gw thread
  void note_latency(const KalshiWriteReply& r);
  std::map<std::string, std::int64_t> lane_published_;   // lane id -> when its row last went out
  std::set<std::string> lanes_dirty_;
  std::int64_t sys_view_ms_ = 0, groups_paced_ms_ = 0, sys_day_ms_ = 0;
  bool sys_dirty_ = true;
  // Systematic's day is rebuilt from the venue's fills on the first account read after start;
  // until then gw refuses Systematic orders (STALE)
  bool sys_rebuilt_ = false;
  bool sys_save_due_ = false;
  std::int64_t sys_saved_ms_ = 0, sys_save_ms_ = 0;
  std::string sys_halt_why_;
  std::shared_ptr<const std::set<std::string>> sys_held_;   // std::atomic_load / std::atomic_store
  mutable std::map<std::string, Px> last_mark_;             // gw thread: each market's last mid
  std::int64_t reconciled_ms_ = 0;            // the account read last reconciled
  std::string sys_restore_error_;            // a saved book that could not be read blocks Systematic
  void sys_reconcile(const rest::Snapshot& s);
  std::int64_t settled_ms_ = 0;               // the account read that last settled unknown orders
  std::int64_t snapshot_polled_ms_ = 0;       // gw's last look for a new account read
  void settle_uncertain(const rest::Snapshot& s);
  void flush_cancel(const std::string& cid);
  void fill_applied(const kalshi::FillRow& f, std::string cid, std::int64_t at, bool live);
  // private fills that beat their order's 201, kept up to kOrphanWaitMs
  struct Orphan;
  std::vector<Orphan> orphans_;
  static constexpr std::int64_t kOrphanWaitMs = 5000;
  void retry_orphans();
  void reconcile_working(const rest::Snapshot& s);
  std::int64_t fills_asked_ms_ = 0;   // the last account read asked for missing fills
  static constexpr std::int64_t kExpiryBackupMs = 1500;   // plan section 3: TTL + 1.5 s
  static constexpr std::int64_t kGroupResetWaitMs = 5000;  // a reset's socket confirmation, at most
  std::map<std::string, std::int64_t> expiry_asked_;       // backup cancels asked, and when
  void expiry_sweep(std::int64_t now);
  std::set<std::string> man_held_;                                   // gw thread
  std::shared_ptr<const std::set<std::string>> man_view_;           // std::atomic_load / std::atomic_store
  std::map<std::string, std::int64_t> alerted_;                      // "lane|code" -> when last alerted
  void takeover(const std::string& ticker);
  void publish_held();
  int cancel_sys_if(const std::function<bool(const std::string& lane, const std::string& ticker)>& which, const char* why);
  void save_sys();
  void place_sys(PlaceIntent& p);
  void group_reply(const KalshiWriteReply& r);
  void group_event(const std::string& id, const std::string& event);
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
