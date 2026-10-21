#pragma once
// strat (plan section 5): quotes, lanes, models and the LIP share estimate, one maker engine for
// every lane. The strat thread runs each engine at its own cadence: the fair, the makers, the LIP
// meter and the markouts every 250 ms; an engine with work in flight (S1, between a basket's
// legs) asks for its next step sooner. The loop sleeps until the next step is due, clamped to
// 5..250 ms: never a busy loop, and never deaf for longer than today's 250 ms.
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace deskd {

class Worker;

inline constexpr int kStratMinSleepMs = 5, kStratMaxSleepMs = 250;

int strat_sleep_ms(int asked);   // asked clamped to kStratMinSleepMs..kStratMaxSleepMs

// step returns the ms until it wants to run again; run_strat sleeps strat_sleep_ms of it
void run_strat(Worker& self, const std::function<int()>& step = {});

// The strat thread's steps, each with its cadence. step() runs the ones that are due, in the
// order they were added, and returns the ms until the next one is due, at most kStratMaxSleepMs.
// A step's interval runs from the start of the pass that ran it, on the steady clock: the time
// the pass takes comes on top, as it did when the loop slept 250 ms after every pass. Strat
// thread only.
class StratSteps {
 public:
  // fn runs at most every every_ms
  void every(int every_ms, std::function<void()> fn);
  // fn runs at most every min_ms, and returns the ms until it wants its next step; less than
  // min_ms (0, say) means min_ms
  void paced(int min_ms, std::function<int()> fn);
  int step(std::int64_t mono_ms);

 private:
  struct Step {
    int min_ms;
    std::function<int()> fn;
    std::int64_t due = std::numeric_limits<std::int64_t>::min();   // the first pass runs it
  };
  std::vector<Step> steps_;
};

}  // namespace deskd
