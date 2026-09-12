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
  // Source health is distinct from model validation. A fresh computation can still use stale
  // inputs; the review screen and automatic proposals must be able to distinguish those cases.
  json inputs = json::object();
  double p_raw = -1.0;
  double market_blend_weight = 0.0;
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
  // Optional metadata for the probability adapter (probability.h). A model that says nothing gets
  // the adapter's own rules: version v1, and the per-model settlement check in
  // settlement_verified_for(). Neither changes a probability.
  virtual std::string model_version() const { return ""; }
  virtual std::optional<bool> settlement_verified(const Market& /*m*/) const { return std::nullopt; }
  // How much of the market's own mid belongs in this model's published probability, absent a
  // `market_weight` in the strategy's config block. The default 0.5 is a humility rule for a
  // model that guesses at a quantity the market also guesses at. It is wrong for two kinds of
  // strategy: one that reports somebody else's price (there is nothing to shrink toward), and one
  // whose whole question is whether it disagrees with the market - blending that model's number
  // with the mid answers the question with itself.
  virtual double default_market_weight() const { return 0.5; }
};

class ReferenceFeed;

// Registry of every strategy the desk knows about, in category order. `feed` is the settlement
// reference socket (reference_feed.h); the short-duration commodity models are only built when
// one is supplied, because without it there is no number to price them from.
std::vector<std::unique_ptr<Strategy>> build_strategies(const Config& cfg, ReferenceFeed* feed = nullptr);

}  // namespace desk
