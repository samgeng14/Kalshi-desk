#include "deskd/net/pool.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>

#include "deskd/app/log.h"
#include "deskd/net/http.h"

namespace deskd::net {

namespace {
constexpr std::size_t kHandshakeRing = 256;
}

SparePool::SparePool(ConnFactory& factory, PoolConfig cfg) : factory_(factory), cfg_(std::move(cfg)) {
  cfg_.spares = std::max(0, cfg_.spares);
  cfg_.lead_ms = std::clamp(cfg_.lead_ms, 0, std::max(0, cfg_.idle_replace_ms - 1));
  cfg_.check_ms = std::max(10, cfg_.check_ms);
  cfg_.backoff_min_ms = std::max(1, cfg_.backoff_min_ms);
  cfg_.backoff_max_ms = std::max(cfg_.backoff_min_ms, cfg_.backoff_max_ms);
}

SparePool::~SparePool() { stop(); }

void SparePool::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (running_) return;
  stop_ = false;
  running_ = true;
  th_ = std::thread([this] { loop(); });
}

void SparePool::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  std::deque<std::unique_ptr<Conn>> drop;
  {
    std::lock_guard<std::mutex> lk(mu_);
    drop.swap(ready_);
    running_ = false;
  }
  for (auto& c : drop) c->close(true);
}

void SparePool::record_open(const Conn& c, bool inline_open) {   // under mu_
  ++st_.handshakes;
  ++(inline_open ? st_.opened_inline : st_.opened_background);
  hs_ms_.push_back(c.connect_ms() + c.handshake_ms());
  if (hs_ms_.size() > kHandshakeRing) hs_ms_.pop_front();
}

void SparePool::loop() {
  const std::string label = "pool-" + cfg_.name;
  set_thread_label(label);
#ifdef __APPLE__
  pthread_setname_np(label.substr(0, 63).c_str());
#else
  pthread_setname_np(pthread_self(), label.substr(0, 15).c_str());
#endif
  int backoff_ms = 0;
  std::int64_t next_try_us = 0;
  std::int64_t next_check_us = mono_us() + static_cast<std::int64_t>(cfg_.check_ms) * 1000;
  bool warned = false;
  const std::int64_t retire_us = static_cast<std::int64_t>(cfg_.idle_replace_ms) * 1000;
  const std::int64_t renew_us = static_cast<std::int64_t>(cfg_.idle_replace_ms - cfg_.lead_ms) * 1000;
  for (;;) {
    std::vector<std::unique_ptr<Conn>> retired, dead;
    int want = 0;
    std::uint64_t seen = 0;
    std::int64_t now = mono_us();
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (stop_) break;
      seen = wakeups_;
      for (auto it = ready_.begin(); it != ready_.end();) {
        if (now - (*it)->opened_us() >= retire_us) {
          retired.push_back(std::move(*it));
          it = ready_.erase(it);
          ++st_.idle_retired;
        } else {
          ++it;
        }
      }
      if (now >= next_check_us) {
        for (auto it = ready_.begin(); it != ready_.end();) {
          std::string why;
          if (!(*it)->usable(&why)) {
            st_.last_error = "closed by the venue while ready: " + why;
            dead.push_back(std::move(*it));
            it = ready_.erase(it);
            ++st_.closed_by_venue;
          } else {
            ++it;
          }
        }
        next_check_us = now + static_cast<std::int64_t>(cfg_.check_ms) * 1000;
      }
      int fresh = 0;
      for (const auto& c : ready_)
        if (now - c->opened_us() < renew_us) ++fresh;
      want = cfg_.spares - fresh;
    }
    for (auto& c : retired) c->close(true);
    for (auto& c : dead) c->close(true);
    retired.clear();
    dead.clear();

    if (want > 0 && now >= next_try_us) {
      std::string why;
      const std::uint64_t refused_before = factory_.stats().refused;
      auto c = factory_.open(cfg_.endpoint, cfg_.connect_timeout_ms, &why);
      const bool opened = c != nullptr;
      const bool refused = !opened && factory_.stats().refused > refused_before;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) {
          if (c) c->close(true);
          break;
        }
        if (c) {
          record_open(*c, false);
          ready_.push_back(std::move(c));
          backoff_ms = 0;
          next_try_us = 0;
        } else {
          ++(refused ? st_.refused : st_.open_failures);
          st_.last_error = why;
          backoff_ms = refused ? cfg_.backoff_max_ms
                               : (backoff_ms ? std::min(backoff_ms * 2, cfg_.backoff_max_ms) : cfg_.backoff_min_ms);
          next_try_us = mono_us() + static_cast<std::int64_t>(backoff_ms) * 1000;
        }
      }
      if (!opened) {
        if (!warned) DLOG_WARN("cannot open %s: %s", cfg_.endpoint.url().c_str(), why.c_str());
        warned = true;
      } else {
        if (warned) DLOG_INFO("%s open again", cfg_.endpoint.url().c_str());
        warned = false;
        cv_.notify_all();
        continue;   // open the next one at once
      }
    }

    // sleep until the next thing is due: a look, a renewal or retirement, a retry, or a take()
    std::unique_lock<std::mutex> lk(mu_);
    if (stop_) break;
    now = mono_us();
    std::int64_t wake = next_check_us;
    for (const auto& c : ready_) {
      const std::int64_t renew_at = c->opened_us() + renew_us, retire_at = c->opened_us() + retire_us;
      if (renew_at > now) wake = std::min(wake, renew_at);
      wake = std::min(wake, retire_at);
    }
    if (want > 0 && next_try_us > now) wake = std::min(wake, next_try_us);
    if (wake <= now) continue;
    cv_.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::microseconds(wake)),
                   [&] { return stop_ || wakeups_ != seen; });
  }
}

std::unique_ptr<Conn> SparePool::take(int timeout_ms, bool* miss, std::string* why) {
  if (miss) *miss = false;
  std::unique_lock<std::mutex> lk(mu_);
  ++st_.takes;
  while (!ready_.empty()) {
    auto c = std::move(ready_.back());   // the newest: the furthest from an idle close
    ready_.pop_back();
    ++wakeups_;
    lk.unlock();
    cv_.notify_all();
    std::string w;
    if (c->usable(&w)) {
      lk.lock();
      ++st_.hits;
      return c;
    }
    c->close(true);
    lk.lock();
    ++st_.stale_on_take;
    st_.last_error = "a ready connection was unfit when taken: " + w;
  }
  ++st_.misses;
  ++wakeups_;
  lk.unlock();
  cv_.notify_all();
  if (miss) *miss = true;
  std::string w;
  auto c = factory_.open(cfg_.endpoint, timeout_ms, &w);
  lk.lock();
  if (c) {
    record_open(*c, true);
  } else {
    ++st_.miss_failed;
    st_.last_error = w;
    if (why) *why = w;
  }
  return c;
}

std::size_t SparePool::ready() const {
  std::lock_guard<std::mutex> lk(mu_);
  return ready_.size();
}

bool SparePool::wait_ready(std::size_t n, int timeout_ms) const {
  std::unique_lock<std::mutex> lk(mu_);
  return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return ready_.size() >= n; });
}

PoolStats SparePool::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  PoolStats s = st_;
  s.ready = ready_.size();
  s.handshake_ms.assign(hs_ms_.begin(), hs_ms_.end());
  return s;
}

}  // namespace deskd::net
