#pragma once
// Server-sent events for GET /api/stream (contract: THE STREAM).
//
// Each message is
//     event: <type>
//     id: <seq>
//     data: {"type":<type>,"seq":<seq>,"ts_ms":<server send time>,"data":{...}}
// followed by a blank line; seq is per stream, 1 for the first message and +1 for every
// message of any type, so the page sees any gap.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace deskd {

// data_json: the event's data object, already serialised (one line; JSON escapes newlines)
std::string sse_frame(const std::string& type, std::uint64_t seq, std::int64_t ts_ms, const std::string& data_json);

// One browser tab's outgoing queue (plan section 2: pub keeps bounded per-stream SSE queues).
// A message takes its seq here, under the queue's lock, and a message offered to a full queue
// is dropped AFTER taking its seq: the next message the page receives skips a number, and the
// page refetches its snapshot. A slow tab therefore costs itself a resync, never pub's time or
// memory, and never another tab's messages.
class StreamQueue {
 public:
  explicit StreamQueue(std::size_t cap) : cap_(cap) {}

  // frames and queues one message; returns its seq, taken even when the message is dropped
  std::uint64_t offer(const std::string& type, std::int64_t ts_ms, const std::string& data_json);
  // waits up to `wait` for messages and returns every queued frame, concatenated in seq order;
  // "" on timeout or once closed
  std::string drain_for(std::chrono::milliseconds wait);

  std::uint64_t last_seq() const;   // the seq of the newest message offered (0 before any)
  std::uint64_t dropped() const;
  std::size_t queued() const;
  void close();
  bool closed() const;

 private:
  const std::size_t cap_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::string> frames_;
  std::uint64_t seq_ = 0;
  std::uint64_t dropped_ = 0;
  bool closed_ = false;
};

}  // namespace deskd
