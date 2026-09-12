#pragma once
// Read-only cross-venue research. This service owns public-data clients and has no executor.
// Saving a mapping or changing the analysis budget cannot submit orders or transfer money.
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/store.h"

namespace desk {
class PolymarketClient;
json crossvenue_defaults();
json crossvenue_settings(const json& patch, const json& previous);
std::string crossvenue_contract_hash(const json& market);
json crossvenue_evaluate(const json& pair, const json& kalshi, const json& polymarket,
                         const json& settings, long long now);
json crossvenue_candidates(const json& kalshi, const json& polymarket, int limit = 30);

class CrossVenueDesk {
 public:
  CrossVenueDesk(const Config& cfg, Store& store);
  ~CrossVenueDesk();
  void start();
  void stop();
  json view();
  json configure(const json& body);
  json save_pair(const json& body);
 private:
  void refresh();
  void persist();
  json kalshi_market(const Market& m);
  json spread_stats(const std::string& id, double basis, long long now);
  const Config& cfg_;
  Store& store_;
  KalshiClient kalshi_;
  std::unique_ptr<PolymarketClient> poly_;
  json settings_, mappings_ = json::array(), poly_markets_ = json::array();
  json status_ = json::object(), account_ = json::object();
  std::map<std::string, json> kalshi_cache_, poly_cache_;
  std::map<std::string, long long> last_record_;
  long long discovery_ts_ = 0, account_ts_ = 0;
  int discovery_offset_ = 0;
  size_t pair_cursor_ = 0;
  std::mutex mu_;
  std::condition_variable wake_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};
}  // namespace desk
