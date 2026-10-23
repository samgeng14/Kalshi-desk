#include "deskd/strat/strat.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "deskd/app/worker.h"

namespace deskd {

int strat_sleep_ms(int asked) { return std::clamp(asked, kStratMinSleepMs, kStratMaxSleepMs); }

void run_strat(Worker& self, const std::function<int()>& step) {
  while (!self.stopping()) {
    self.beat();
    const int asked = step ? step() : kStratMaxSleepMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(strat_sleep_ms(asked)));
  }
}

void StratSteps::every(int every_ms, std::function<void()> fn) {
  steps_.push_back(Step{every_ms, [fn = std::move(fn)] { fn(); return 0; }});
}

void StratSteps::paced(int min_ms, std::function<int()> fn) { steps_.push_back(Step{min_ms, std::move(fn)}); }

int StratSteps::step(std::int64_t mono_ms) {
  std::int64_t next = mono_ms + kStratMaxSleepMs;
  for (auto& s : steps_) {
    if (mono_ms >= s.due) s.due = mono_ms + std::max(s.min_ms, s.fn());
    next = std::min(next, s.due);
  }
  return static_cast<int>(next - mono_ms);
}

}  // namespace deskd
