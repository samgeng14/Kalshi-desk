#pragma once
// The model fair of Kalshi's 15-minute financial markets (plan section 5, LIP-15's anchor):
// YES pays when the one-minute Pyth candle close at the window's end is at or above the one at
// its start, which Kalshi publishes as the strike (docs/strategy-routing.md). So
//
//   fair = P(S_close >= S0) = Phi(ln(S/S0) / (sigma sqrt tau)),   zero drift
//
// with S the pyth_value now, sigma from the feed's own last hour (strat/terminal_price.h) and tau
// the seconds to the close. sd is half the spread of that probability over sigma +/- 25%.
//
// It is shown only when it can be stood behind (never a placeholder): the feed is fresh, the
// volatility estimate is warm, and this window's published strike is the candle close this feed
// computed at the open, so the number is about the settlement feed. A window that opened before
// deskd was watching cannot be checked and gets no fair. Replication counts every checked window;
// the plan's trading gate is 50 consecutive matches (LIP-15 uses the anchor only after that).
//
// Threading: step() runs on strat; board() and status() are read from any thread.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/units.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/ref/pyth.h"
#include "deskd/strat/terminal_price.h"

namespace deskd {
namespace rest { struct Snapshot; }

struct Underlying {
  std::string key, series;
  std::vector<std::string> patterns;   // pyth_value symbols, exact names first
  int decimals = -1;                   // the strike's stated rounding; -1: read off the strike
};
// The 15-minute series on Pyth. Gold to copper as the old desk read them off each series'
// settlement sources (2026-09-12); platinum and palladium are the plain metal feeds; the FX
// series wait until their pyth_value symbols are seen (their patterns are guesses until then).
const std::vector<Underlying>& financial_underlyings();
const Underlying* underlying_for(const std::string& ticker);

struct ModelFair {
  Px fair = 0, sd = 0;
  std::int64_t ts_ms = 0;
  std::string underlying;
  double spot = 0, strike = 0, sigma = 0, tau_s = 0;
};
using FairBoard = std::map<std::string, ModelFair>;   // by market ticker

struct Replication {
  std::int64_t checked = 0, matched = 0, consecutive = 0;
  std::int64_t last_open_ms = 0;
  double last_strike = 0, last_close = 0;
  bool last_ok = false;
};

class FinancialFair {
 public:
  using SymbolFn = std::function<std::string(const std::vector<std::string>&)>;
  using ViewFn = std::function<std::optional<RefView>(const std::string&, std::int64_t, std::int64_t)>;
  FinancialFair(SymbolFn symbol, ViewFn view, std::int64_t watching_since_ms);
  void step(const rest::Snapshot& snapshot, std::int64_t now_ms);
  std::shared_ptr<const FairBoard> board() const;
  nlohmann::json status() const;
  // Is close the strike, to the contract's rounding? (one unit in the last stated place)
  static bool same_strike(double close, double strike, int decimals);
  // The fair of one market from one view of its underlying; nullopt with why when not shown.
  static std::optional<ModelFair> price(const kalshi::MarketRow& m, const RefView& v, std::int64_t now_ms,
                                        const VolSettings& vol, std::string* why);
 private:
  SymbolFn symbol_;
  ViewFn view_;
  std::int64_t since_ms_;
  VolSettings vol_;
  mutable std::mutex m_;   // board_, replication_, checked_, why_
  std::shared_ptr<const FairBoard> board_;
  std::map<std::string, Replication> replication_;   // by underlying key
  std::map<std::string, bool> checked_;              // market ticker -> its strike matched our close
  std::map<std::string, std::string> why_;           // market ticker -> why it has no fair
};

}  // namespace deskd
