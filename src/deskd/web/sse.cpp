#include "deskd/web/sse.h"

namespace deskd {

std::string sse_frame(const std::string& type, std::uint64_t seq, std::int64_t ts_ms, const std::string& data_json) {
  const std::string s = std::to_string(seq);
  std::string f;
  f.reserve(type.size() * 2 + s.size() * 2 + data_json.size() + 96);
  f += "event: ";
  f += type;
  f += "\nid: ";
  f += s;
  f += "\ndata: {\"type\":\"";
  f += type;
  f += "\",\"seq\":";
  f += s;
  f += ",\"ts_ms\":";
  f += std::to_string(ts_ms);
  f += ",\"data\":";
  f += data_json;
  f += "}\n\n";
  return f;
}

std::uint64_t StreamQueue::offer(const std::string& type, std::int64_t ts_ms, const std::string& data_json) {
  std::uint64_t seq;
  {
    std::lock_guard<std::mutex> lk(m_);
    seq = ++seq_;
    if (closed_) return seq;
    if (frames_.size() >= cap_) {
      ++dropped_;
      return seq;
    }
    frames_.push_back(sse_frame(type, seq, ts_ms, data_json));
  }
  cv_.notify_one();
  return seq;
}

std::string StreamQueue::drain_for(std::chrono::milliseconds wait) {
  std::unique_lock<std::mutex> lk(m_);
  cv_.wait_for(lk, wait, [this] { return closed_ || !frames_.empty(); });
  std::string out;
  if (closed_) return out;
  std::size_t n = 0;
  for (const auto& f : frames_) n += f.size();
  out.reserve(n);
  for (auto& f : frames_) out += f;
  frames_.clear();
  return out;
}

std::uint64_t StreamQueue::last_seq() const {
  std::lock_guard<std::mutex> lk(m_);
  return seq_;
}
std::uint64_t StreamQueue::dropped() const {
  std::lock_guard<std::mutex> lk(m_);
  return dropped_;
}
std::size_t StreamQueue::queued() const {
  std::lock_guard<std::mutex> lk(m_);
  return frames_.size();
}
void StreamQueue::close() {
  {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true;
    frames_.clear();
  }
  cv_.notify_all();
}
bool StreamQueue::closed() const {
  std::lock_guard<std::mutex> lk(m_);
  return closed_;
}

}  // namespace deskd
