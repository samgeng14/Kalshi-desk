#include "deskd/strat/strat.h"

#include <chrono>
#include <thread>

#include "deskd/app/worker.h"

namespace deskd {

void run_strat(Worker& self, const std::function<void()>& step) {
  while (!self.stopping()) {
    self.beat();
    if (step) step();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace deskd
