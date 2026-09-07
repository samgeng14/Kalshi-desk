#pragma once
// A strategy turns a market into a fair probability with an uncertainty.
// The engine does everything else: fees, thresholds, sizing, execution.
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "desk/config.h"
#include "desk/store.h"
#include "desk/types.h"

namespace desk {

struct Fair {
  double p = 0.5;      // P(YES)
  double sigma = 0.1;  // model uncertainty on p (probability units)
  std::string note;    // what the number came from, shown on the dashboard
  std::string cluster; // correlation cluster key; empty -> event_ticker
  // Log this number (Brier sample) but never propose on it: the strategy knows its own
  // estimate is not yet validated for this market (e.g. same-day weather before WX-1c).
  bool research_only = false;
  std::string research_reason;
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::string name() const = 0;
  virtual std::string category() const = 0;  // Kalshi category it targets, or "*"
  // Cheap filter run before any data fetch.
  virtual bool covers(const Market& m, const Series& s) const = 0;
  // Called once per cycle before fair() so strategies can fetch external data once.
  virtual void prepare(const std::vector<Market>& /*markets*/, Store& /*store*/) {}
  virtual std::optional<Fair> fair(const Market& m, const Series& s, Store& store) = 0;
  // One line for the dashboard.
  virtual std::string describe() const { return ""; }
};

// Registry of every strategy the desk knows about, in category order.
std::vector<std::unique_ptr<Strategy>> build_strategies(const Config& cfg);

}  // namespace desk
