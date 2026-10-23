#pragma once
// How likely a resting order is to fill, from the market's own trade tape (operator, 2026-10-13:
// Manual wants orders that fill about 80% of the time, not a screen of quotes that never trade;
// Systematic a fill every 5-10 minutes).
//
// A resting YES bid fills from takers who sell YES (prints on the bid side); an ask from takers
// who buy. With `ahead` contracts in front of it (the book's depth at better prices and at its
// own price when it is placed, by price-time priority; 0 for a price that improves the touch) it
// fills completely within T once that side's taker volume in T reaches ahead + size, and partly
// once it passes ahead. The estimate is the fraction of the tape's T-long windows, one starting
// every second over the last hour the tape covers, in which that volume came: the market's real
// bursts and lulls, not a Poisson guess. What it leaves out: cancels ahead of us (they help),
// orders joining at better prices and the touch moving away before the volume comes (they hurt).
// Each Manual and Systematic outcome is recorded against its estimate so the two can be compared.
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "deskd/bus/units.h"

namespace deskd {

struct TapePrint {
  std::int64_t ts_ms = 0;
  Qty qty = 0;
  bool taker_bought_yes = true;   // the aggressor bought YES: a print on the ask side
};

struct FillEstimate {
  double p_full = 0, p_any = 0;   // fills completely / at least partly within the horizon
  double median_s = -1;           // over the windows that filled completely: seconds to do it; -1 = none
  int windows = 0;                // windows the estimate is over; too few and `why` says so
  std::string why;                // "" when the estimate stands
};

// tape: one market's prints, oldest first. fills_from_buyers: true for an ask (YES sell), whose
// fills come from YES-buying takers. covered_from_ms: the time from which the tape is complete
// (a market quiet for an hour is a real zero, not missing data).
FillEstimate estimate_fill(const std::vector<TapePrint>& tape, bool fills_from_buyers, Qty ahead, Qty size,
                           int horizon_s, std::int64_t now_ms, std::int64_t covered_from_ms,
                           std::int64_t lookback_ms = 3600000, int step_ms = 1000);

// Every market's tape for the last hour and a bit, shared: md adds live prints, the REST owner the
// backfill, web and strat ask. One mutex, held only to copy a market's tape.
class FillModel {
 public:
  static constexpr std::int64_t kKeepMs = 2 * 3600000;
  static constexpr int kMinWindows = 120;   // two minutes of windows before an estimate stands

  void add(const std::string& tk, const std::string& trade_id, const TapePrint& p);
  // complete from covered_from_ms: a REST backfill's min_ts, or the live feed's start on a market
  void backfill(const std::string& tk, std::int64_t covered_from_ms,
                const std::vector<std::pair<std::string, TapePrint>>& prints);
  void cover(const std::string& tk, std::int64_t from_ms);   // the live feed began here
  bool needs_backfill(const std::string& tk) const;          // never backfilled
  FillEstimate estimate(const std::string& tk, bool fills_from_buyers, Qty ahead, Qty size, int horizon_s,
                        std::int64_t now_ms) const;
  std::int64_t covered_from(const std::string& tk) const;    // 0 = nothing known

 private:
  struct Tape {
    std::deque<TapePrint> prints;   // by time
    std::set<std::string> ids;
    std::int64_t covered_from_ms = 0;
    bool backfilled = false;
  };
  void insert(Tape& t, const std::string& id, const TapePrint& p);
  void trim(Tape& t, std::int64_t now_ms);
  mutable std::mutex mu_;
  std::map<std::string, Tape> tapes_;
};

}  // namespace deskd
