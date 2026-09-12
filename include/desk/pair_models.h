#pragma once
#include <deque>
#include <map>
#include <mutex>
#include "desk/store.h"

namespace desk {
struct PairSample {
  long long ts=0;
  double kb=0,ka=0,pb=0,pa=0;
};
// Pure causal forecasts. Prior observations must be strictly before current.ts.
// Prices on both venues refer to the same outcome, after mapping PM YES/NO.
json pair_model_signals(const std::deque<PairSample>& prior, const PairSample& current,
                        double roundtrip_cost, double settlement_edge);

// A separate paper ledger: cannot call an Executor, transfer collateral or own live positions.
class PairModelLab {
 public:
  explicit PairModelLab(Store& store);
  json view();
  json configure(const json& body);
  bool running();
  // rows contain mapped books, BOTH depth snapshots, freshness and a score/period regime key.
  void step(const json& rows,long long now,double existing_cap);
 private:
  void persist();
  Store& store_;
  std::mutex mu_;
  json state_, rows_=json::array();
  std::map<std::string,std::deque<PairSample>> history_;
  std::map<std::string,std::string> regimes_;
};
} // namespace desk
