#pragma once
// The open SSE streams by sid, and each sid's subscription (contract: LIFECYCLE).
//
// - GET /api/stream creates the sid or takes it over: a second stream with the same sid closes
//   the first and inherits its slot.
// - At most max_open sids have a stream open (4 by default, of 16 web threads: an SSE stream
//   holds its thread for as long as it is open).
// - A sid's subscription survives a disconnect for 60 s (forget_idle), so a reconnect keeps it.
//
// The lock is held for map work only, never across a write to a socket.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/web/sse.h"

namespace deskd {

struct MarketRef {
  Venue v = Venue::K;
  std::string tk;
};

// POST /api/subscribe: up to 4 ladders at FULL depth, the watchlist at TOP ("ALL" = every OPEN
// market). A sid starts with nothing until the page sends its first subscribe.
struct Subs {
  std::vector<MarketRef> ladders;
  bool tops_all = false;
  std::vector<MarketRef> tops;
};

struct Stream {
  Stream(std::string sid_, std::size_t cap, std::int64_t opened) : sid(std::move(sid_)), q(cap), opened_ms(opened) {}
  const std::string sid;
  StreamQueue q;
  const std::int64_t opened_ms;   // steady clock
};

class StreamRegistry {
 public:
  explicit StreamRegistry(int max_open) : max_open_(max_open) {}

  // Makes s the open stream of its sid. Refused (false, *why set) when max_open other sids are
  // open. A stream already open for the sid is closed: the takeover.
  bool open(const std::shared_ptr<Stream>& s, std::string* why);
  // the connection of s ended; its sid keeps its subscription until forget_idle (both times on
  // the steady clock)
  void closed(const std::shared_ptr<Stream>& s, std::int64_t mono_ms);

  std::vector<std::shared_ptr<Stream>> open_streams() const;
  std::shared_ptr<Stream> stream_for(const std::string& sid) const;   // null unless open
  int open_count() const;

  bool set_subs(const std::string& sid, Subs subs);   // false unless sid has an open stream
  Subs subs(const std::string& sid) const;

  // forgets sids whose stream closed more than after_ms ago; returns how many
  std::size_t forget_idle(std::int64_t now_ms, std::int64_t after_ms);
  void close_all();   // shutdown: every stream ends its response

 private:
  struct Entry {
    std::shared_ptr<Stream> stream;   // null while disconnected
    Subs subs;
    std::int64_t closed_ms = 0;
  };
  const int max_open_;
  mutable std::mutex m_;
  std::map<std::string, Entry> by_sid_;
};

}  // namespace deskd
