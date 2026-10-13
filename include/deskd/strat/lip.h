#pragma once
// Our share of Kalshi's liquidity incentive (LIP) on a market, estimated each second from the
// book and our resting orders (plan section 5, "our share each second"), and the dollars that
// share would earn over the program's window. An ESTIMATE until it is reconciled with the
// payouts the account receives: the venue publishes the program's terms
// (GET /incentive_programs: reward, target size, discount), not its scoring code. The rule here:
//   - each second is a snapshot; a side scores the resting size from its best price outward
//     until target_size is reached (the level that reaches it only in part);
//   - each contract counts discount^(cents from the best price), discount = discount_bps / 10,000;
//   - a snapshot pays only if both sides reach target_size (scripts/incentive_audit.py);
//   - its reward, period_reward / the window's seconds, is split evenly between the two sides,
//     each by its share of the side's score;
//   - our contracts at a level stand behind everyone else's there (we joined last): when the
//     target cuts a level short, ours are the ones cut first.
// The YES bids are one side; the NO bids (YES asks) the other.
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "deskd/bus/units.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/kalshi_md.h"

namespace deskd {

struct LipOwn {   // one of our resting orders
  bool bid = true;   // a YES bid; false: a YES ask
  Px price = 0;
  Qty left = 0;
};

struct LipShare {
  bool scores = false;          // both sides reach the target size
  double bid = 0, ask = 0;      // our share of each side's score
  double share() const { return scores ? (bid + ask) / 2 : 0; }
};

// book: the venue's book, our orders in it; ours: our resting orders on this market
LipShare lip_share(const MdBook& book, const std::vector<LipOwn>& ours, Qty target, int discount_bps);

// Each market's program window: seconds seen, seconds that scored, our mean share, and the
// estimated dollars. step() once a second from strat; a window that has ended comes back once, as
// a row for store's lip_windows.
class LipMeter {
 public:
  void sample(const std::string& tk, const kalshi::IncentiveRow& program, const MdBook& book,
              const std::vector<LipOwn>& ours, std::int64_t now_ms);
  std::vector<nlohmann::json> finished(std::int64_t now_ms);   // windows past their end
  nlohmann::json status() const;                                // any thread

 private:
  struct Window {
    kalshi::IncentiveRow program;
    std::int64_t seconds = 0, scored = 0, quoted = 0;   // seen, both sides at target, with ours in
    double share_sum = 0, est = 0;                      // est in 1/10,000 $
    double last_share = 0;
    std::int64_t last_ms = 0;
  };
  static nlohmann::json row(const std::string& tk, const Window& w);
  mutable std::mutex mu_;
  std::map<std::string, Window> windows_;   // "ticker|start_ms"
};

}  // namespace deskd
