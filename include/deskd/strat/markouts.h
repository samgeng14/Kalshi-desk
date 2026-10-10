#pragma once
// Markouts (contract S.mo; plan section 5's lane rules read them): how far the mid moved our way
// 1 s, 10 s and 60 s after a fill, per contract, + in our favour (a SELL sign-flipped).
//
// A fill reaches deskd through the REST read, seconds after the venue time it carries, so the
// mid at fill + h is read from a history: md records each market's mid as it changes (at most
// one sample a market every 200 ms; a stale book records "unknown"; 3 minutes kept, plus the mid
// in force before them). The mid at fill + h is the last sample at or before it; with none, or
// an unknown one, that horizon is skipped, never guessed.
//
// Threading: mid() from md, fill() from the REST owner, step() and status() from strat; one lock.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"

namespace deskd {

class Markouts {
 public:
  void mid(const std::string& ticker, std::int64_t at_ms, Px mid);   // mid < 0: the book is stale
  // lane: "" for Manual and foreign fills
  void fill(const std::string& fid, const std::string& lane, const std::string& ticker, Side side, Px px, Qty qty,
            std::int64_t ts_ms);
  // the markouts due by now, each once: {fid, h, mo, mid} rows for the stream
  std::vector<nlohmann::json> step(std::int64_t now_ms);
  // per lane and horizon: fills marked and the contract-weighted mean markout
  nlohmann::json status() const;
  // one lane's markouts at a horizon ("1s", "10s", "60s"): fills marked and the mean, per contract
  struct LaneMean { std::int64_t fills = 0; Money mean = 0; };
  LaneMean lane_mean(const std::string& lane, const std::string& horizon) const;
  std::optional<Px> mid_at(const std::string& ticker, std::int64_t at_ms) const;

 private:
  std::optional<Px> mid_at_locked(const std::string& ticker, std::int64_t at_ms) const;
  struct Pending {
    std::string fid, lane, ticker;
    Side side = Side::BUY;
    Px px = 0;
    Qty qty = 0;
    std::int64_t ts_ms = 0;
    int done = 0;   // horizons already published (1 s, 10 s, 60 s)
  };
  struct Mean {
    std::int64_t n = 0;
    std::int64_t contracts = 0;   // 1/100
    std::int64_t sum = 0;         // Money x Qty
  };
  mutable std::mutex m_;
  std::map<std::string, std::deque<std::pair<std::int64_t, Px>>> mids_;
  std::vector<Pending> pending_;
  std::map<std::string, bool> seen_;
  std::map<std::string, std::map<std::string, Mean>> means_;   // lane -> horizon -> mean
};

}  // namespace deskd
