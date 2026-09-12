#pragma once
#include <functional>
#include <mutex>
#include "desk/store.h"

namespace desk {
// Accepted manual instructions, not exchange orders. A request owns its immutable limit,
// quantity and deadline. Unknown delivery is never retried as a new order.
struct RetryManualOrder : std::runtime_error { using std::runtime_error::runtime_error; };
class ManualOrders {
 public:
  explicit ManualOrders(Store& store);
  json enqueue(const json& payload, long long now);
  json view();
  json cancel(const std::string& id, long long now);
  using Send=std::function<json(const json&,const std::string&,long long)>;
  using Lookup=std::function<json(const std::string&)>;
  void process(const Send& send,const Lookup& lookup,long long now);
  // A request that names a decision_id (the Trades board's order dialog) is checked against the
  // decision before it is accepted; the full revalidation runs again just before it is sent.
  using Precheck=std::function<void(const json&)>;
  void set_precheck(Precheck p){precheck_=std::move(p);}
 private:
  Precheck precheck_;
  Store& store_;
  std::mutex mu_;
  std::mutex worker_mu_;
  void finish(const std::string& id,const std::string& status,const json& result,const std::string& error,long long now);
};
} // namespace desk
