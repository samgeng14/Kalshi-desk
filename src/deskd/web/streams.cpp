#include "deskd/web/streams.h"

namespace deskd {

bool StreamRegistry::open(const std::shared_ptr<Stream>& s, std::string* why) {
  std::shared_ptr<Stream> old;
  {
    std::lock_guard<std::mutex> lk(m_);
    int others = 0;
    for (const auto& kv : by_sid_)
      if (kv.second.stream && kv.first != s->sid) ++others;
    if (others >= max_open_) {
      if (why) *why = std::to_string(others) + " streams open (at most " + std::to_string(max_open_) + ")";
      return false;
    }
    Entry& e = by_sid_[s->sid];
    old = std::move(e.stream);
    e.stream = s;
    e.closed_ms = 0;
  }
  if (old) old->q.close();   // the takeover: the old response ends on its next wake
  return true;
}

void StreamRegistry::closed(const std::shared_ptr<Stream>& s, std::int64_t now_ms) {
  s->q.close();
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_sid_.find(s->sid);
  if (it != by_sid_.end() && it->second.stream == s) {
    it->second.stream.reset();
    it->second.closed_ms = now_ms;
  }
}

std::vector<std::shared_ptr<Stream>> StreamRegistry::open_streams() const {
  std::lock_guard<std::mutex> lk(m_);
  std::vector<std::shared_ptr<Stream>> out;
  for (const auto& kv : by_sid_)
    if (kv.second.stream) out.push_back(kv.second.stream);
  return out;
}

std::shared_ptr<Stream> StreamRegistry::stream_for(const std::string& sid) const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_sid_.find(sid);
  return it == by_sid_.end() ? nullptr : it->second.stream;
}

int StreamRegistry::open_count() const {
  std::lock_guard<std::mutex> lk(m_);
  int n = 0;
  for (const auto& kv : by_sid_)
    if (kv.second.stream) ++n;
  return n;
}

bool StreamRegistry::set_subs(const std::string& sid, Subs subs) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_sid_.find(sid);
  if (it == by_sid_.end() || !it->second.stream) return false;
  it->second.subs = std::move(subs);
  return true;
}

Subs StreamRegistry::subs(const std::string& sid) const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_sid_.find(sid);
  return it == by_sid_.end() ? Subs{} : it->second.subs;
}

std::size_t StreamRegistry::forget_idle(std::int64_t now_ms, std::int64_t after_ms) {
  std::lock_guard<std::mutex> lk(m_);
  std::size_t n = 0;
  for (auto it = by_sid_.begin(); it != by_sid_.end();) {
    if (!it->second.stream && now_ms - it->second.closed_ms > after_ms) {
      it = by_sid_.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}

void StreamRegistry::close_all() {
  for (const auto& s : open_streams()) s->q.close();
}

}  // namespace deskd
