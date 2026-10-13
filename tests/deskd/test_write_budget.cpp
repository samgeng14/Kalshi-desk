// gw/write_budget: Kalshi's write buckets per shard, worked by hand (Basic: 100 tokens a second,
// holding 1 s; create 10, cancel 2), and Systematic's 80% of each.
#include <cmath>
#include "check.h"
#include "deskd/gw/write_budget.h"

using namespace deskd;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void basic() {
  WriteBudget w;
  const std::int64_t t = 1791000000000;
  CHECK(near(w.cap(), 100) && near(w.tokens(2, t), 100));   // a bucket starts full
  // Systematic creates down to a fifth: 8 of them (100 -> 20), the ninth is refused
  int sent = 0;
  while (w.sys_may(2, WriteBudget::kCreate, t)) { w.spend(2, WriteBudget::kCreate, t); ++sent; }
  CHECK(sent == 8 && near(w.tokens(2, t), 20) && w.sys_refused() == 1);
  // ...room for one more once 10 tokens refill: 100 ms at 100 a second
  CHECK(w.sys_wait_ms(2, WriteBudget::kCreate, t) == 100);
  CHECK(!w.sys_may(2, WriteBudget::kCreate, t + 99) && w.sys_may(2, WriteBudget::kCreate, t + 100));
  // Manual and the cancels spend what Systematic left (30 by then), and are never refused here
  w.spend(2, WriteBudget::kCreate, t + 100);
  for (int i = 0; i < 10; ++i) w.spend(2, WriteBudget::kCancel, t + 100);
  CHECK(near(w.tokens(2, t + 100), 0) && w.sys_refused() == 2);
  // shards are separate buckets: shard 1 and the unscoped one are untouched
  CHECK(near(w.tokens(1, t), 100) && near(w.tokens(0, t), 100) && w.sys_may(1, WriteBudget::kCreate, t));
  // refills to the cap and no further
  CHECK(near(w.tokens(2, t + 600), 50) && near(w.tokens(2, t + 5000), 100));
  // a 429 says the venue's bucket is empty, whatever ours thought
  w.venue_refused(2, t + 5000);
  CHECK(near(w.tokens(2, t + 5000), 0) && w.venue_refusals() == 1 && w.sys_wait_ms(2, WriteBudget::kCreate, t + 5000) == 300);
  // a burst past empty (KILL's cancels) has to refill from below zero, but never below one bucket
  for (int i = 0; i < 200; ++i) w.spend(0, WriteBudget::kCancel, t + 5000);
  CHECK(near(w.tokens(0, t + 5000), -100) && near(w.tokens(0, t + 6000), 0));
  const auto all = w.all(t + 6000);
  CHECK(all.size() == 3 && all.count(0) && all.count(1) && all.count(2));
}

void advanced() {
  WriteBudget w(WriteBudget::advanced());
  const std::int64_t t = 1791000000000;
  CHECK(near(w.cap(), 900));   // 300 a second, holding 3 s
  int sent = 0;
  while (w.sys_may(3, WriteBudget::kCreate, t)) { w.spend(3, WriteBudget::kCreate, t); ++sent; }
  CHECK(sent == 72 && near(w.tokens(3, t), 180));   // down to 180, a fifth of 900
}

}  // namespace

int main() {
  basic();
  advanced();
  return finish("deskd-write_budget");
}
