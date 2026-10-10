#pragma once
// Gateway-thread-only order state. This has no sockets, locks or retries. Admission and the
// execution/position ledger are separate: cumulative venue counts never move a position.
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "deskd/gw/kalshi_wire.h"

namespace deskd {
class OrderLedger {
 public:
  struct Order {
    PlaceIntent intent;
    int shard = -1;
    std::string oid, why;
    OrdState state = OrdState::PEND;
    Qty filled = 0, executions = 0, left = 0;
    kalshi::Micros execution_cost = 0, fees = 0, original_reserve = 0;
    bool uncertain = false, cancel_requested = false, cancel_unverified = false;
    bool refused = false;   // ended by rejected(): never reached the venue, so no venue evidence may follow
    std::int64_t sent_ms = 0, ack_ms = 0, done_ms = 0, update_ms = 0;
    std::uint64_t rev = 1;
    bool terminal() const;
    // Keep the cost of fills reported cumulatively but not yet delivered as executions.
    kalshi::Micros reservation() const;
    nlohmann::json json() const;
  };
  enum class FillResult { APPLIED, DUPLICATE, UNMAPPED, INVALID };
  struct History {
    std::string ticker;
    int shard = -1;
    std::int64_t since_ms = 0, began_ms = 0, completed_ms = 0;
    bool orders_complete = false, fills_complete = false;
    std::vector<kalshi::OrderRow> orders;
    std::vector<kalshi::FillRow> fills;
  };
  // Call only after admission, with a conservative all-in reservation and resolved expiry.
  // A cid can never be reused, including after rejection or terminal completion.
  bool insert(PlaceIntent intent, int shard, kalshi::Micros reserve);
  bool sent(const std::string& cid, std::int64_t at);
  bool unknown(const std::string& cid);
  // REJ with code as why; except POST_CROSS (the venue cancelled a crossing post-only) and USER
  // (cancelled before it was sent), which the contract reports as CXL with that why.
  bool rejected(const std::string& cid, const std::string& code, std::int64_t at);
  bool acknowledged(const std::string& cid, const kalshi::CreateAck& ack, std::int64_t at);
  bool observed(const kalshi::OrderRow& row, std::int64_t at);
  // APPLIED is the ONLY outcome that permits the caller to book this execution in positions.
  // An orphan waits for mapping/reconciliation. Conflicting duplicate executions fail closed.
  FillResult fill(const kalshi::FillRow& row, const std::string& cid, std::int64_t at);
  bool request_cancel(const std::string& cid);
  bool cancelled(const std::string& cid, const kalshi::CancelAck& ack,
                 const std::string& reason, std::int64_t at);
  // A complete history read begun after the deadline is required to prove absence. Mere
  // absence from an open-orders snapshot, timeout, or local expiry is never sufficient.
  bool resolve_absent(const std::string& cid, const History& history);
  const Order* find(const std::string& cid) const;
  bool blocks(const std::string& ticker, int shard) const;
  kalshi::Micros reserved(int shard) const;
  std::vector<std::string> fills_due(std::int64_t now) const;
  bool healthy() const { return healthy_; }
  const std::map<std::string, Order>& orders() const { return orders_; }
 private:
  bool bind(Order& order, const std::string& oid);
  bool invalid();
  std::map<std::string, Order> orders_;
  std::map<std::string, std::string> by_oid_;
  std::map<std::string, kalshi::FillRow> fills_;
  std::map<std::string, std::int64_t> missing_since_;
  bool healthy_ = true;
  void missing(Order& order, std::int64_t at);
};
}
