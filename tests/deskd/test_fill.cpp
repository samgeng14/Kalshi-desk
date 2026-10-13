// strat/fill_model: how likely a resting order is to fill, worked by hand on tapes whose answer
// is known, then the shared model's de-duplication, coverage and learning rule.
#include <cmath>
#include "check.h"
#include "deskd/strat/fill_model.h"

using namespace deskd;

namespace {

const std::int64_t T0 = 1791000000000;    // the tape's start
const std::int64_t HOUR = 3600000;
const Qty ONE = kOneContract;

// one YES-selling taker print of `q` every `every_s` seconds for an hour (they fill bids)
std::vector<TapePrint> steady(int every_s, Qty q = ONE, bool buyers = false) {
  std::vector<TapePrint> t;
  for (std::int64_t ms = T0 + every_s * 1000LL; ms <= T0 + HOUR; ms += every_s * 1000LL) t.push_back({ms, q, buyers});
  return t;
}
bool near(double a, double b, double tol = 0.02) { return std::fabs(a - b) <= tol; }

void by_hand() {
  const std::int64_t now = T0 + HOUR;
  // a print every 10 s: a 15 s window always holds one, a 5 s window half the time
  auto e = estimate_fill(steady(10), false, 0, ONE, 15, now, T0);
  CHECK(e.why.empty() && e.windows == 3600 - 15 + 1 && e.p_full == 1.0 && e.p_any == 1.0);
  CHECK(near(e.median_s, 5.5, 0.6));   // the next print is 1-10 s away, uniformly
  e = estimate_fill(steady(10), false, 0, ONE, 5, now, T0);
  CHECK(near(e.p_full, 0.5));
  // one contract ahead of a 1-lot needs two prints in 15 s: half the windows (the first print
  // only takes the contract ahead, so even a part fill needs the second)
  e = estimate_fill(steady(10), false, ONE, ONE, 15, now, T0);
  CHECK(near(e.p_full, 0.5) && near(e.p_any, 0.5));
  // 2-lot prints: a 3-lot behind nobody fills completely once two prints come (half), partly always
  e = estimate_fill(steady(10, 2 * ONE), false, 0, 3 * ONE, 15, now, T0);
  CHECK(near(e.p_full, 0.5) && e.p_any == 1.0);
  // the other side's flow never fills a bid
  e = estimate_fill(steady(10, ONE, true), false, 0, ONE, 15, now, T0);
  CHECK(e.p_full == 0 && e.p_any == 0 && e.median_s < 0 && e.windows > 0);
  // ...and fills an ask
  e = estimate_fill(steady(10, ONE, true), true, 0, ONE, 15, now, T0);
  CHECK(e.p_full == 1.0);
  // a quiet hour is a real zero, not missing data
  e = estimate_fill({}, false, 0, ONE, 60, now, T0);
  CHECK(e.why.empty() && e.p_full == 0 && e.windows == 3600 - 60 + 1);
  // the tape covers less than one window: learning, no number
  e = estimate_fill(steady(10), false, 0, ONE, 60, now, now - 30000);
  CHECK(!e.why.empty() && e.windows == 0);
  // only the last hour counts: an old burst does not make today look busy
  auto t = steady(10);
  std::vector<TapePrint> old;
  for (int i = 0; i < 500; ++i) old.push_back({T0 - HOUR + i, 10 * ONE, false});
  old.insert(old.end(), t.begin(), t.end());
  e = estimate_fill(old, false, 50 * ONE, ONE, 15, now, T0 - HOUR);
  CHECK(e.p_full == 0);
}

void model() {
  FillModel m;
  const std::int64_t now = T0 + HOUR;
  // a market never seen: nothing known, a backfill wanted
  CHECK(m.covered_from("K1") == 0 && m.needs_backfill("K1"));
  auto e = m.estimate("K1", false, 0, ONE, 15, now);
  CHECK(!e.why.empty());
  // the live feed from 90 s ago: windows exist but too few to stand
  m.cover("K1", now - 90000);
  for (std::int64_t ms = now - 90000 + 5000; ms <= now; ms += 5000) m.add("K1", "live-" + std::to_string(ms), {ms, ONE, false});
  e = m.estimate("K1", false, 0, ONE, 15, now);
  CHECK(e.windows > 0 && e.windows < FillModel::kMinWindows && e.why.find("learning") == 0 && e.p_full == 1.0);
  // the REST backfill: the hour before, overlapping the live prints (same trade ids count once)
  std::vector<std::pair<std::string, TapePrint>> back;
  for (std::int64_t ms = now - HOUR + 5000; ms <= now; ms += 5000)
    back.push_back({ms > now - 90000 ? "live-" + std::to_string(ms) : "rest-" + std::to_string(ms), {ms, ONE, false}});
  m.backfill("K1", now - HOUR, back);
  CHECK(!m.needs_backfill("K1") && m.covered_from("K1") == now - HOUR);
  e = m.estimate("K1", false, 0, ONE, 15, now);
  CHECK(e.why.empty() && e.windows == 3600 - 15 + 1 && e.p_full == 1.0);
  // de-duplicated: a 1-lot ahead needs two prints in 5 s; one every 5 s gives that never
  e = m.estimate("K1", false, ONE, ONE, 5, now);
  CHECK(e.p_full == 0);
  // a print out of order lands in its place
  m.add("K1", "late", {now - 2500, ONE, false});
  e = m.estimate("K1", false, ONE, ONE, 5, now);
  CHECK(e.p_full > 0);
}

}  // namespace

int main() {
  by_hand();
  model();
  return finish("deskd-fill");
}
