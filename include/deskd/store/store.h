#pragma once
// store (plan section 7): the only writer of data/ops.sqlite. Rows reach it by message from gw:
// "orders" (an order's whole timeline once it ends: click, web, gw, the wire, the venue's ack,
// the private feed, fills, the cancel, the end), "fills" (each fill once, by trade id) and, from
// strat, "markouts" (each fill's 1/10/60 s markout against the mid, per contract). It
// writes them in one transaction every quarter second, in WAL mode, so a reader (research,
// tools/latency_report.py) never blocks it. A full inbox loses the row and gw counts it; a
// database that cannot be opened or written says why in /ops/status and the desk trades on.
// Not yet: the money ledger, LIP windows, the nightly copy.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class Worker;

struct StoreRecord {
  std::string table;   // "orders", "fills" or "markouts"
  std::string json;
};

class StoreWorker {
 public:
  StoreWorker() : inbox_(8192, 256) {}
  // The database's path; "" (the default) counts rows and writes nothing. Before run().
  void set_path(std::string path) { path_ = std::move(path); }
  const std::string& path() const { return path_; }
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
  BoundedQueue<StoreRecord> inbox_;
  std::string path_;
  std::atomic<std::uint64_t> received_{0}, written_{0}, failed_{0};
  std::shared_ptr<const std::string> why_;   // std::atomic_load / std::atomic_store
};

}  // namespace deskd
