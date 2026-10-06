#include "deskd/app/worker.h"

#include <pthread.h>

#include <chrono>
#include <exception>

#include "deskd/app/log.h"

namespace deskd {

Worker::Worker(std::string name) : name_(std::move(name)) {}

Worker::~Worker() {
  request_stop();
  join();
}

void Worker::start(std::function<void(Worker&)> body) {
  running_.store(true);
  thread_ = std::thread([this, body = std::move(body)] {
#ifdef __APPLE__
    pthread_setname_np(name_.c_str());  // shows in Activity Monitor, sample(1) and lldb
#else
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
#endif
    set_thread_label(name_);
    beat();
    try {
      body(*this);
    } catch (const std::exception& e) {
      // An owner that dies stops beating: ops then withholds the heartbeat (for gw, md-k and
      // priv-k) and the supervisor restarts the whole desk, which is the recovery we want.
      DLOG_ERR("worker ended by an exception: %s", e.what());
    } catch (...) {
      DLOG_ERR("worker ended by an unknown exception");
    }
    running_.store(false);
  });
}

void Worker::join() {
  if (thread_.joinable()) thread_.join();
}

void Worker::beat() {
  using namespace std::chrono;
  progress_.fetch_add(1, std::memory_order_relaxed);
  last_beat_ms_.store(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(),
                      std::memory_order_relaxed);
}

}  // namespace deskd
