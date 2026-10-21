#pragma once
// The gateway's Manual order book (gw thread only; no locks, no I/O). It turns the latest
// complete REST revision, the market's book and the order ledger into admission's context,
// books an accepted order in the same turn, and applies the Manual sender's replies.
//
// Free cash on a shard (what NO_CASH / SHARD_CASH check against):
//   the shard's REST available balance (the venue has already taken its working orders'
//   collateral out of it)
//   - for every ledger order the snapshot cannot fully reflect: its worst case (collateral
//     reserved at admission + its taker-fee allowance)
//   + for such an order the snapshot shows RESTING (acknowledged before the read began): the
//     opening collateral the venue holds for it, so it is never subtracted twice.
// An order is fully reflected once it was terminal before the snapshot's account read began;
// one that never reached the venue (REJ) or ended unfilled reflects at once. This only ever
// errs towards less free cash (e.g. a partly filled resting order's fills count twice until it
// ends and a newer read lands).
//
// Reducing room is conservative the same way: the snapshot position, less the remaining
// quantity of same-side resting orders gw does not own, less the full size of every same-side
// ledger order not yet reflected. A reduce_only right after a fill may therefore be refused
// REDUCE until the next account read (each private event requests one).
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "deskd/gw/manual_admission.h"
#include "deskd/gw/order_ledger.h"
#include "deskd/gw/sys_book.h"
#include "deskd/gw/write_budget.h"
#include "deskd/net/kalshi_sender.h"

namespace deskd {
namespace rest { struct Snapshot; }

struct ManualHealth {
  bool killed = false, read_only = true;
  bool public_feed = false, private_feed = false, publisher = false;
  bool manual_sender = false, cancel_sender = false;
  std::uint64_t kill_generation = 0;
  std::string why;   // why the first owner above that is not ready is not, for the RO refusal
};

// What gw needs from the rest of deskd to run the Manual path. Every function is called on the
// gw thread and must not block: snapshot and book read immutable published state, send and
// cancel push to a bounded inbox (false = full), publish posts to pub.
struct ManualPath {
  std::function<std::shared_ptr<const rest::Snapshot>()> snapshot;
  std::function<std::optional<MdBook>(const std::string&)> book;
  std::function<ManualHealth()> health;          // gw sets killed and kill_generation itself
  std::function<bool(KalshiWriteJob)> send;       // send-k1's inbox
  std::function<bool(KalshiWriteJob)> cancel;     // the cancel sender's inbox
  std::function<void(const std::string&)> publish;   // one ord row, JSON text
  // Systematic (gw/sys_book.h), on when sys_send is set: send-k2's inbox for S creates and
  // order-group jobs and its priority lane for S cancels (Systematic never uses send-k1 or
  // send-cx); its health (manual_sender = send-k2); its lane rows; the daily loss stop; the
  // registry KILL's sender triggers.
  std::function<bool(KalshiWriteJob)> sys_send, sys_cancel;
  std::function<ManualHealth()> sys_health;
  std::function<void(const std::string&)> publish_lane;
  std::function<Money(const std::string& lane)> lane_lip;   // its LIP estimate today, 1/10,000 $
  Money daily_stop = 0;
  // the floor (plan section 4, limit 5): the account's equity (cash and positions, the venue's
  // marks) under it halts Systematic; 0 = none
  Money equity_floor = 0;
  // the account's write rate limit (gw/write_budget.h): Basic until the Advanced upgrade is run
  WriteBudget::Tier write_tier = WriteBudget::basic();
  GroupRegistry* groups = nullptr;
  std::vector<LaneSpec> lanes;   // empty: default_lanes()
  bool live = false;             // the production account: a paper_only lane's orders are refused
  bool check_each_message = false;   // tests and the soak: OrderLedger::check after every gw message
  // Systematic's book across a restart (its day, positions, trade ids, HALT and the markets
  // Manual holds); "" keeps none
  std::string sys_state_file;
  // a market Manual took over, or released: its mkt row again (mkt.man)
  std::function<void(const std::string&)> publish_market;
  // an account read soon (after a transfer, whose cash no private event reports)
  std::function<void()> refresh;
  // a row for store (data/ops.sqlite): table "orders" (an ended order's timeline) or "fills";
  // false = store's inbox is full (the row is lost and counted)
  std::function<bool(const std::string& table, std::string json)> record;
  // an alert on the stream (level, src, msg): Systematic's refusals, so a lane that cannot trade
  // says why on the page, not only in /ops/status
  std::function<void(const std::string&, const std::string&, const std::string&)> alert;
};

class ManualBook {
 public:
  static constexpr std::int64_t kSnapshotMaxAgeMs = 60000;   // an account read older than this is STALE
  static constexpr std::int64_t kSendWindowMs = 1000;        // a job not on the wire by then is dropped

  void set_snapshot(std::shared_ptr<const rest::Snapshot> snapshot);
  void set_book(const MdBook& book);

  kalshi::Micros shard_free(int shard) const;
  kalshi::Micros venue_free() const;   // the sum over the snapshot's shards
  ManualContext context(const PlaceIntent& intent, const ManualHealth& health, std::int64_t now_ms) const;

  struct Placed {
    ManualAdmission admission;            // ok(): booked in the ledger, job ready for send-k1
    std::optional<KalshiWriteJob> job;
  };
  Placed place(const PlaceIntent& intent, const ManualHealth& health, std::int64_t now_ms, std::int64_t mono_now_ms);

  // A cancel before the write left: the sender refuses the job (USER) and its reply ends it CXL.
  bool revoke(const std::string& cid);
  // The Manual sender's reply for a create. false: the reply contradicts the ledger, which is
  // then unhealthy and blocks admission until reconciled.
  bool apply(const KalshiWriteReply& reply);
  // The contract's REJ/CXL why for a reply that ended the order before or at the venue.
  static std::string refusal_code(const KalshiWriteReply& reply);

  const OrderLedger& ledger() const { return ledger_; }
  OrderLedger& ledger() { return ledger_; }
  const rest::Snapshot* snapshot() const { return snapshot_.get(); }

 private:
  bool reflected(const OrderLedger::Order& order) const;
  kalshi::Micros held_in_snapshot(const OrderLedger::Order& order) const;
  std::shared_ptr<const rest::Snapshot> snapshot_;
  std::map<std::string, MdBook> books_;
  std::map<std::string, kalshi::Micros> held_;   // snapshot RESTING order_id -> modeled opening collateral
  OrderLedger ledger_;
  std::map<std::string, kalshi::Micros> fee_allowance_;
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> permits_;
};

}  // namespace deskd
