#pragma once
// store (plan section 7): the only writer of data/ops.sqlite. Rows reach it by message from gw:
// "orders" (an order's whole timeline once it ends: click, web, gw, the wire, the venue's ack,
// the private feed, fills, the cancel, the end), "fills" (each fill once, by trade id) and, from
// strat, "markouts" (each fill's 1/10/60/300 s markout against the mid, per contract) and
// "lip_windows" (each LIP program window's estimated share and dollars, strat/lip.h). The lanes'
// research rows come the same way:
//   "signals"    one table for every IOC lane (C1, X1, N1, N2; the lane column says which): each
//                signal, taken, pulled or in shadow, keyed by its id. id, lane, mode, shadow,
//                ts_ms, tk, ev, side, px, qty, fair, edge, score, src, age_ms, first_seen_ms,
//                touch_since_ms, action, why, cid, mo10, mo60, mo300. The row is sent again once
//                its markouts are in, and the later row replaces it.
//   "scores"     each score state as first seen, by source (ESPN, NCAA): src, league, game_id,
//                state, period, clock, home, away, home_score, away_score, lines, first_seen_ms,
//                prev_poll_ms. A state sent twice keeps its first row.
//   "arb_scans"  S1's finder, one row per event scanned: ts_ms, event, kind, mode, legs, qty,
//                edge, cost, payout_min, fired, why; keyed by event and ts_ms.
//   "baskets"    S1's baskets, keyed by id, sent again on each change of state: id, event, kind,
//                state, legs, qty, edge_plan, edge_real, legging, t0_ms, t_end_ms.
// Every table also keeps the whole row as json. Prices and dollars come as decimal strings
// ("0.4200") and are stored as numbers; an array (legs, lines) is stored as its JSON text. It
// writes them in one transaction every quarter second, in WAL mode, so a reader (research,
// tools/latency_report.py) never blocks it. A full inbox loses the row and gw counts it; a
// database that cannot be opened or written says why in /ops/status and the desk trades on.
//
// Each minute it measures the file (with its WAL) and alerts CRIT, at most hourly, once it passes
// 1 GB (plan section 7: ops.sqlite stays under 1 GB). After each ET midnight it writes the research
// copy, ops-research.sqlite beside it, by VACUUM INTO a .tmp then a rename, so research reads a
// whole copy and never the live file; a failed copy keeps the last one and tries again in 10 min.
// Not yet: the money ledger (LIP payouts against these estimates among it).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class Worker;

struct StoreRecord {
  std::string table;   // "orders", "fills", "markouts", "lip_windows", "signals", "scores", "arb_scans" or "baskets"
  std::string json;
};

class StoreWorker {
 public:
  StoreWorker() : inbox_(8192, 256) {}
  // The database's path; "" (the default) counts rows and writes nothing. Before run().
  void set_path(std::string path) { path_ = std::move(path); }
  const std::string& path() const { return path_; }
  std::string copy_path() const { return path_.empty() ? std::string() : research_path(path_); }
  static std::string research_path(const std::string& db_path);   // the copy beside it
  // alert(level, message) from the store thread; and the size past which it alerts. Before run().
  void set_alert(std::function<void(const std::string&, const std::string&)> alert) { alert_ = std::move(alert); }
  void set_cap_bytes(std::int64_t cap) { cap_bytes_ = cap; }
  std::int64_t size_bytes() const { return size_bytes_.load(); }   // 0 until measured
  std::int64_t copy_ms() const { return copy_ms_.load(); }         // the research copy's time; 0: none
  std::uint64_t copies() const { return copies_.load(); }          // copies this run wrote
  std::string copy_why() const {   // the last copy's failure, "" after a good one
    const auto w = std::atomic_load(&copy_why_);
    return w ? *w : std::string();
  }
  BoundedQueue<StoreRecord>& inbox() { return inbox_; }
  void run(Worker& self);
  std::uint64_t received() const { return received_.load(); }
  std::uint64_t written() const { return written_.load(); }
  std::uint64_t failed() const { return failed_.load(); }
  std::string why() const {   // the last failure, "" while none
    const auto w = std::atomic_load(&why_);
    return w ? *w : std::string();
  }

 private:
  void fail(const std::string& why);
  void measure(std::int64_t now_ms);
  void research_copy(std::int64_t now_ms);
  BoundedQueue<StoreRecord> inbox_;
  std::string path_;
  std::function<void(const std::string&, const std::string&)> alert_;
  std::int64_t cap_bytes_ = 1LL << 30;
  std::int64_t alerted_ms_ = 0, copy_tried_ms_ = 0;
  std::atomic<std::uint64_t> received_{0}, written_{0}, failed_{0}, copies_{0};
  std::atomic<std::int64_t> size_bytes_{0}, copy_ms_{0};
  std::shared_ptr<const std::string> why_;        // std::atomic_load / std::atomic_store
  std::shared_ptr<const std::string> copy_why_;   // the same
};

}  // namespace deskd
