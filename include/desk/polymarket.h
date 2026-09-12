#pragma once
// Polymarket US retail API. This adapter intentionally exposes only GET operations.
#include "desk/http.h"
#include "desk/types.h"

#include <memory>
#include <string>

namespace desk {

// Stable normalized views for cross-venue comparison. Missing prices/sizes are null,
// never zero-cost liquidity. Metadata quotes are indicative until an L2 book is fetched.
json normalize_polymarket_market(const json& response, long long received_ts);
json normalize_polymarket_book(const json& response, const std::string& slug, long long received_ts);
json polymarket_fee_metadata(const json& market);
double polymarket_taker_fee(double theta, double contracts, double price);

// Exposed for deterministic verification with published test keys. Query strings are
// excluded from the signed path, matching the official Python SDK.
std::string polymarket_us_signature(const std::string& secret, const std::string& method,
                                   const std::string& path, long long timestamp_ms);

class PolymarketClient {
 public:
  explicit PolymarketClient(std::string user_agent = "kalshi-desk/0.1",
                            std::string key_id = {}, std::string secret = {});
  ~PolymarketClient();
  PolymarketClient(PolymarketClient&&) noexcept;
  PolymarketClient& operator=(PolymarketClient&&) noexcept;
  PolymarketClient(const PolymarketClient&) = delete;
  PolymarketClient& operator=(const PolymarketClient&) = delete;

  // Environment pair takes precedence. Optional JSON file contains key_id and secret_key;
  // it must be a regular, non-symlink, current-user-owned file with no group/other access.
  static PolymarketClient from_environment(const std::string& user_agent,
                                           const std::string& credential_file = {});
  bool credential_status() const;
  json markets(int limit = 100, int offset = 0) const;
  json market(const std::string& slug) const;
  json book(const std::string& slug) const;
  json account_summary() const;

 private:
  struct Auth;
  Http http_;
  std::unique_ptr<Auth> auth_;
  json get(const std::string& path, const std::string& query = {}, bool authenticated = false) const;
};

}  // namespace desk
