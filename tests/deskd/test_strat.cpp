// strat/strat.h: the strat loop sleeps what its step asks, clamped to 5..250 ms, and each engine
// steps at its own cadence (S1 every 10 ms between a basket's legs, the makers every 250 ms).
#include <atomic>
#include <chrono>
#include <climits>
#include <string>
#include <thread>

#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/strat/strat.h"

using namespace deskd;
namespace {
const std::int64_t T = 7200000;   // the steady clock, ms

void clamp() {
  CHECK(strat_sleep_ms(INT_MIN) == 5 && strat_sleep_ms(-1) == 5 && strat_sleep_ms(0) == 5 && strat_sleep_ms(4) == 5);
  CHECK(strat_sleep_ms(5) == 5 && strat_sleep_ms(10) == 10 && strat_sleep_ms(249) == 249 && strat_sleep_ms(250) == 250);
  CHECK(strat_sleep_ms(251) == 250 && strat_sleep_ms(INT_MAX) == 250);
}

void cadence() {
  StratSteps none;
  CHECK(none.step(T) == 250);   // nothing to run: the longest sleep
  std::string ran;
  int arb_wants = 10;   // like S1: 10 ms while a basket is in flight, else 250
  StratSteps s;
  s.every(250, [&] { ran += "f"; });
  s.every(250, [&] { ran += "m"; });
  s.paced(5, [&] { ran += "a"; return arb_wants; });
  s.every(1000, [&] { ran += "s"; });
  auto pass = [&](std::int64_t at) {
    ran.clear();
    return s.step(at);
  };
  CHECK(pass(T) == 10 && ran == "fmas");   // every step on the first pass, in the order added
  CHECK(pass(T + 10) == 10 && ran == "a");
  CHECK(pass(T + 15) == 5 && ran.empty());   // woken early: nothing due, sleep to the next
  arb_wants = 250;   // the basket is done
  CHECK(pass(T + 20) == 230 && ran == "a");   // the makers' turn at T + 250 comes first
  CHECK(pass(T + 250) == 20 && ran == "fm");
  CHECK(pass(T + 270) == 230 && ran == "a");
  // a late pass (the thread was busy): each interval runs from the pass that ran the step, so
  // the late steps run once, not once for every turn they missed
  CHECK(pass(T + 700) == 250 && ran == "fma");
  CHECK(pass(T + 949) == 1 && ran.empty());
  CHECK(pass(T + 1000) == 250 && ran == "fmas");

  // paced: an ask under its floor is the floor; an ask over 250 ms is kept, the loop waking at 250
  int n = 0, wants = 0;
  StratSteps p;
  p.paced(20, [&] { ++n; return wants; });
  CHECK(p.step(T) == 20 && n == 1);
  CHECK(p.step(T + 19) == 1 && n == 1);
  wants = 1000;
  CHECK(p.step(T + 20) == 250 && n == 2);
  CHECK(p.step(T + 270) == 250 && n == 2 && p.step(T + 1019) == 1 && n == 2);
  CHECK(p.step(T + 1020) == 250 && n == 3);
}

// run_strat on its own thread against the real clock, its step asking `ask` ms every time, until
// it has stepped `want` times or `within_ms` has passed: the steps made and the ms it took
struct Passes {
  int n;
  std::int64_t ms;
};
Passes passes(int ask, int want, std::int64_t within_ms) {
  std::atomic<int> n{0};
  const std::int64_t t0 = mono_ms();
  Worker w("strat");
  w.start([&n, ask](Worker& self) { run_strat(self, [&n, ask] { ++n; return ask; }); });
  while (n < want && mono_ms() - t0 < within_ms) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  w.request_stop();
  w.join();
  return {n.load(), mono_ms() - t0};
}

void loop() {
  const auto busy = passes(0, INT_MAX, 300);
  CHECK(busy.n <= busy.ms / 5 + 2);   // a step asking 0 still sleeps 5 ms: unclamped, a busy loop
  const auto fast = passes(10, 20, 19 * 250);
  CHECK(fast.n >= 20);   // it sleeps what the step asks: at the old fixed 250 ms, 20 steps took 4.75 s
  const auto slow = passes(INT_MAX, 3, static_cast<std::int64_t>(10000 * kSlowdown));
  CHECK(slow.n >= 3);    // a step asking for weeks is stepped again within 250 ms
}
}  // namespace

int main() {
  clamp(); cadence(); loop();
  return finish("deskd-strat");
}
