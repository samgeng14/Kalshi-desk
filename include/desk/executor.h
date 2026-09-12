#pragma once
// Turns proposals into orders and keeps the local books in sync with the exchange.
#include <string>
#include <mutex>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/store.h"

namespace desk {

inline bool is_exit_strategy(const std::string& strategy) {
  return strategy == "exit" || strategy == "target" || strategy == "stop" || strategy == "sys_exit";
}

class Executor {
 public:
  Executor(const Config& cfg, Store& store, KalshiClient& client);

  // Places the order described by the proposal. Updates the proposal row.
  // Returns the exchange order id. Throws on rejection (proposal marked failed).
  std::string submit(Proposal& p, const std::string& client_order_id = "", long long deadline = 0);
  json cancel(const std::string& order_id);
  // Flatten (part of) a position with an immediate-or-cancel order at the touch.
  json close_position(const std::string& ticker, double count = 0);
  // A hand-entered order from the dashboard. Logged as a proposal from strategy "ticket".
  // `decision_id` links it to the router decision it was approved from (decision_log.h).
  Proposal ticket(const OrderRequest& req, const std::string& note, long long deadline = 0,
                  const std::string& decision_id = "");

  // Reduce-only model exit, target, or stop for an existing position: sells our side as a
  // maker (post-only GTC) or taker (IOC). Never adds risk, so the entry caps do not apply.
  std::string submit_exit(Proposal& p);
  // Pull balance, positions, orders, fills, settlements into the store.
  void reconcile(bool full = false);

  void refresh_account();  // all-or-nothing risk snapshot; throws on incomplete reads
  void topup_shards();     // keep configured exchange shards funded from shard 0 (risk.shard_topup)
  bool armed() const { return client_.authenticated(); }

 private:
  std::recursive_mutex order_mu_;
  const Config& cfg_;
  Store& store_;
  KalshiClient& client_;
};

}  // namespace desk
