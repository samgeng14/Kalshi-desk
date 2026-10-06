#pragma once
// store (plan section 7): the only writer of data/ops.sqlite (orders with timelines, fills,
// markouts, the ledger, LIP windows). Records reach it by message; if it falls behind they wait
// in the intents journal and an alert fires. Week 0 has the owner and its bounded inbox only:
// nothing is written yet.
#include <atomic>
#include <cstdint>
#include <string>

#include "deskd/bus/queue.h"

namespace deskd {

class Worker;

struct StoreRecord {
  std::string table;
  std::string json;
};

class StoreWorker {
 public:
  StoreWorker() : inbox_(8192, 256) {}
  BoundedQueue<StoreRecord>& inbox() { return inbox_; }
  void run(Worker& self);
  std::uint64_t received() const { return received_.load(); }

 private:
  BoundedQueue<StoreRecord> inbox_;
  std::atomic<std::uint64_t> received_{0};
};

}  // namespace deskd
