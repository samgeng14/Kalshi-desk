#pragma once
// Kalshi's write rate limit, one token bucket per shard (plan section 3, "Write budget"), so
// Systematic's quoting never spends what Manual and the cancels need.
//
// The venue (getting_started/rate_limits.md, /account/endpoint_costs 2026-10-05): a create costs
// 10 tokens, a cancel 2, a batch cancel 2 an order, cancel-all 2, anything else 10. A single order
// write naming a nonzero shard is billed to that shard's bucket only; shard 0, batches, cancel-all
// and order groups to the unscoped one (exchange_sharding.md), which is bucket 0 here. Basic
// refills 100 tokens a second and holds 1 s of them; Advanced 300, holding 3 s.
//
// Systematic may use 80%: its create is refused RATE here once it would leave the bucket under a
// fifth, and the maker backs that side off as for any refusal. Manual's writes and every cancel are
// never refused here (the venue decides; its 429 is refused before processing), only counted, so
// Systematic sees what they spent. A 429 empties that bucket: the venue's count is the truth.
// gw thread only.
#include <cstdint>
#include <map>
#include <string>

namespace deskd {

class WriteBudget {
 public:
  struct Tier {
    double per_s = 100, burst_s = 1;
  };
  static Tier basic() { return {100, 1}; }
  static Tier advanced() { return {300, 3}; }
  static constexpr double kCreate = 10, kCancel = 2, kOther = 10;
  static constexpr double kSysShare = 0.8;

  explicit WriteBudget(Tier tier = basic()) : tier_(tier) {}
  double cap() const { return tier_.per_s * tier_.burst_s; }
  // Systematic may send a write of `cost` now (counted when not); spend() once it is sent
  bool sys_may(int shard, double cost, std::int64_t now_ms);
  void spend(int shard, double cost, std::int64_t now_ms);   // every write sent; never refused
  void venue_refused(int shard, std::int64_t now_ms);        // a 429 on this shard's bucket
  double tokens(int shard, std::int64_t now_ms);
  // how long until Systematic could spend `cost` on this shard, if nothing else spends
  std::int64_t sys_wait_ms(int shard, double cost, std::int64_t now_ms);
  int sys_refused() const { return sys_refused_; }
  int venue_refusals() const { return venue_refused_; }
  std::map<int, double> all(std::int64_t now_ms);

 private:
  struct Bucket {
    double tokens = 0;
    std::int64_t at_ms = 0;
  };
  Bucket& at(int shard, std::int64_t now_ms);   // refilled to now
  Tier tier_;
  std::map<int, Bucket> buckets_;
  int sys_refused_ = 0, venue_refused_ = 0;
};

}  // namespace deskd
