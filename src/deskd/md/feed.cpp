#include "deskd/md/feed.h"

#include <chrono>
#include <thread>

#include "deskd/app/worker.h"

namespace deskd {

void run_feed_placeholder(Worker& self, FeedState& feed) {
  feed.set(false, false);
  while (!self.stopping()) {
    self.beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace deskd
