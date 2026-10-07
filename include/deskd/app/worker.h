#pragma once
// A worker is one named thread that owns its state (plan section 2). Its loop calls beat()
// every time it comes round, idle or not, so a progress counter that stops moving means the
// loop is stuck: ops withholds the supervisor's heartbeat when a watched owner stops moving.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace deskd {

class Worker {
 public:
  explicit Worker(std::string name);
  ~Worker();  // asks the loop to stop and joins it
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  const std::string& name() const { return name_; }

  // runs body(*this) on a new thread named name()
  void start(std::function<void(Worker&)> body);
  void request_stop() { stop_.store(true, std::memory_order_relaxed); }
  bool stopping() const { return stop_.load(std::memory_order_relaxed); }
  void join();
  bool running() const { return running_.load(); }

  void beat();  // one more unit of progress
  std::uint64_t progress() const { return progress_.load(std::memory_order_relaxed); }
  std::int64_t last_beat_ms() const { return last_beat_ms_.load(std::memory_order_relaxed); }   // steady clock

 private:
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> progress_{0};
  std::atomic<std::int64_t> last_beat_ms_{0};
};

}  // namespace deskd
