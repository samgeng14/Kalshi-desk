#include "deskd/gw/write_budget.h"

#include <algorithm>
#include <cmath>

namespace deskd {

WriteBudget::Bucket& WriteBudget::at(int shard, std::int64_t now_ms) {
  auto it = buckets_.find(shard);
  if (it == buckets_.end()) it = buckets_.emplace(shard, Bucket{cap(), now_ms}).first;   // a new bucket is full
  auto& b = it->second;
  if (now_ms > b.at_ms) {
    b.tokens = std::min(cap(), b.tokens + tier_.per_s * static_cast<double>(now_ms - b.at_ms) / 1000.0);
    b.at_ms = now_ms;
  }
  return b;
}

bool WriteBudget::sys_may(int shard, double cost, std::int64_t now_ms) {
  if (at(shard, now_ms).tokens - cost >= cap() * (1 - kSysShare)) return true;
  ++sys_refused_;
  return false;
}

void WriteBudget::spend(int shard, double cost, std::int64_t now_ms) {
  auto& b = at(shard, now_ms);
  b.tokens = std::max(-cap(), b.tokens - cost);   // a burst past empty still has to refill
}

void WriteBudget::venue_refused(int shard, std::int64_t now_ms) {
  ++venue_refused_;
  auto& b = at(shard, now_ms);
  b.tokens = std::min(b.tokens, 0.0);
}

double WriteBudget::tokens(int shard, std::int64_t now_ms) { return at(shard, now_ms).tokens; }

std::int64_t WriteBudget::sys_wait_ms(int shard, double cost, std::int64_t now_ms) {
  const double short_by = cap() * (1 - kSysShare) + cost - at(shard, now_ms).tokens;
  if (short_by <= 0) return 0;
  return static_cast<std::int64_t>(std::ceil(short_by / tier_.per_s * 1000.0));
}

std::map<int, double> WriteBudget::all(std::int64_t now_ms) {
  std::map<int, double> out;
  for (auto& [shard, b] : buckets_) out[shard] = at(shard, now_ms).tokens;
  return out;
}

}  // namespace deskd
