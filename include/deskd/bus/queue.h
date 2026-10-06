#pragma once
// The bounded two-lane queue every deskd worker reads its inbox from.
//
// This is the only place deskd holds a mutex (plan section 2): every piece of state has one
// owner thread, and others reach it by message. A lock here is held for a deque push or pop,
// never across I/O, which removes the bug class behind the 2026-09-23 deadlock (a lock held
// across an HTTP call) and the kill_reason_ race.
//
// - Two lanes. pop takes the priority lane first, so a Kill, Stop, cancel or private event
//   pushed behind 4,000 new orders is the next thing the owner sees.
// - Each lane is fixed-size. A push to a full lane is dropped, counted, and marks a gap, which
//   the reader takes with take_gap(): it must then resync (a page refetches its snapshot)
//   rather than trust that nothing was lost. Nothing ever blocks a producer.
// - erase_if lets the owner delete queued messages a later one made stale: draining a cancel,
//   Stop or Kill deletes the matching new orders still queued behind it, so a click followed at
//   once by Esc never rests.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace deskd {

enum class Lane : std::uint8_t { PRIORITY, NORMAL };

template <class T>
class BoundedQueue {
 public:
  BoundedQueue(std::size_t normal_cap, std::size_t priority_cap)
      : normal_cap_(normal_cap), priority_cap_(priority_cap) {}
  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // false when the lane is full (the message is dropped and a gap marked) or the queue closed
  bool push(T v, Lane lane = Lane::NORMAL) {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return false;
      auto& q = lane == Lane::PRIORITY ? prio_ : normal_;
      const std::size_t cap = lane == Lane::PRIORITY ? priority_cap_ : normal_cap_;
      if (q.size() >= cap) {
        ++dropped_;
        gap_ = true;
        return false;
      }
      q.push_back(std::move(v));
    }
    cv_.notify_one();
    return true;
  }

  // the next message, priority lane first; nullopt after `wait` with nothing queued, or once the
  // queue is closed and empty
  std::optional<T> pop_for(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, wait, [this] { return closed_ || !prio_.empty() || !normal_.empty(); });
    return take_locked();
  }

  std::optional<T> try_pop() {
    std::lock_guard<std::mutex> lk(m_);
    return take_locked();
  }

  // deletes the queued messages of one lane that match; returns how many, and moves them into
  // *erased when given (a deleted request someone waits on still needs its answer)
  template <class Pred>
  std::size_t erase_if(Lane lane, Pred pred, std::vector<T>* erased = nullptr) {
    std::lock_guard<std::mutex> lk(m_);
    auto& q = lane == Lane::PRIORITY ? prio_ : normal_;
    std::size_t n = 0;
    for (auto it = q.begin(); it != q.end();) {
      if (pred(*it)) {
        if (erased) erased->push_back(std::move(*it));
        it = q.erase(it);
        ++n;
      } else {
        ++it;
      }
    }
    return n;
  }

  // true once after any drop since the previous call
  bool take_gap() {
    std::lock_guard<std::mutex> lk(m_);
    const bool g = gap_;
    gap_ = false;
    return g;
  }
  std::uint64_t dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return dropped_;
  }
  std::size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return prio_.size() + normal_.size();
  }
  std::size_t size(Lane lane) const {
    std::lock_guard<std::mutex> lk(m_);
    return lane == Lane::PRIORITY ? prio_.size() : normal_.size();
  }
  // refuses further pushes and wakes every waiter; what is queued can still be popped
  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
    }
    cv_.notify_all();
  }
  bool closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
  }

 private:
  std::optional<T> take_locked() {
    auto& q = !prio_.empty() ? prio_ : normal_;
    if (q.empty()) return std::nullopt;
    std::optional<T> v(std::move(q.front()));
    q.pop_front();
    return v;
  }

  const std::size_t normal_cap_, priority_cap_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> prio_, normal_;
  std::uint64_t dropped_ = 0;
  bool gap_ = false;
  bool closed_ = false;
};

}  // namespace deskd
