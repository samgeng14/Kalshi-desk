#include "deskd/strat/strat.h"

#include <chrono>
#include <thread>

#include "deskd/app/worker.h"

namespace deskd {

void run_strat(Worker& self) {
  while (!self.stopping()) {
    self.beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace deskd
