#include "deskd/store/store.h"

#include <chrono>

#include "deskd/app/worker.h"

namespace deskd {

void StoreWorker::run(Worker& self) {
  while (!self.stopping()) {
    auto r = inbox_.pop_for(std::chrono::milliseconds(250));
    if (r) received_.fetch_add(1, std::memory_order_relaxed);
    self.beat();
  }
}

}  // namespace deskd
