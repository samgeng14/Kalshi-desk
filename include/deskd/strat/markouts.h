#pragma once
// Markouts (contract S.mo; plan section 5's lane rules read them): how far the mid moved our way
// 1 s, 10 s, 60 s and 300 s after a fill, per contract, + in our favour (a SELL sign-flipped),
// and the same net of the fill's fee (plan 5(d): "60 s and 300 s markouts net of the fee").
//
// A fill reaches deskd through the REST read, seconds after the venue time it carries, so the
// mid at fill + h is read from a history: md records each market's mid as it changes (at most
// one sample a market every 200 ms; a stale book records "unknown"; 330 s kept, plus the mid
// in force before them). The mid at fill + h is the last sample at or before it; with none, or
// an unknown one, that horizon is skipped, never guessed.
//
// A market's key is any string: a Kalshi ticker, or "P|slug" for a Polymarket market. Every row
// goes to the store; the stream carries only the contract's horizons (MO_H: 1s, 10s, 60s), so a
// 300s row is stored and read by the lanes' rules, never published.
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

// one fill's markout at one horizon
struct MarkoutRow {
  std::string fid, lane, h;   // lane: "" for Manual and foreign fills; h: "1s", "10s", "60s" or "300s"
  Money mo = 0;               // per contract, + in our favour
  Money net = 0;              // mo less the fill's fee per contract, rounded down (a rebate adds)
  Px mid = 0;                 // the mid at fill + h
  Qty qty = 0;                // the fill's size
  bool on_stream() const;     // h is one of the contract's MO_H: the row is published
  nlohmann::json pub() const;     // {fid, h, mo, mid}: contract S.mo
  nlohmann::json store() const;   // the store's markouts row: pub() plus lane, net and qty
};

class Markouts {
 public:
  void mid(const std::string& key, std::int64_t at_ms, Px mid);   // mid < 0: the book is stale
  // lane: "" for Manual and foreign fills; fee: the fill's whole fee (negative: a rebate)
  void fill(const std::string& fid, const std::string& lane, const std::string& key, Side side, Px px, Qty qty,
            std::int64_t ts_ms, Money fee = 0);
  // the markouts due by now, each once
  std::vector<MarkoutRow> step(std::int64_t now_ms);
  // per lane and horizon: fills marked and the contract-weighted mean markout, gross and net
  nlohmann::json status() const;
  // one lane's markouts at a horizon ("1s", "10s", "60s", "300s"): fills marked and the mean, per
  // contract, gross and net of fees
  struct LaneMean { std::int64_t fills = 0; Money mean = 0, net = 0; };
  LaneMean lane_mean(const std::string& lane, const std::string& horizon) const;
  std::optional<Px> mid_at(const std::string& key, std::int64_t at_ms) const;

 private:
  std::optional<Px> mid_at_locked(const std::string& key, std::int64_t at_ms) const;
  struct Pending {
    std::string fid, lane, key;
    Side side = Side::BUY;
    Px px = 0;
    Qty qty = 0;
    std::int64_t ts_ms = 0;
    Money fee = 0;
    int done = 0;   // horizons already marked (1 s, 10 s, 60 s, 300 s)
  };
  struct Mean {
    std::int64_t n = 0;
    std::int64_t contracts = 0;   // 1/100
    std::int64_t sum = 0;         // Money x Qty
    std::int64_t net = 0;         // Money x Qty, each row's net
  };
  mutable std::mutex m_;
  std::map<std::string, std::deque<std::pair<std::int64_t, Px>>> mids_;
  std::vector<Pending> pending_;
  std::map<std::string, bool> seen_;
  std::map<std::string, std::map<std::string, Mean>> means_;   // lane -> horizon -> mean
};

}  // namespace deskd
