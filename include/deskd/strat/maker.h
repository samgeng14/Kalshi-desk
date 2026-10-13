#pragma once
// The maker engine (plan section 5): one engine for every quoting lane, LIP-15 first.
//
//   fair    microprice, (bid x ask size + ask x bid size) / (bid size + ask size), or the mid
//           when the spread is over 3 ticks; a validated anchor (the 15-minute model once its
//           strike replication gate has passed) is averaged in 50/50, and quotes are pulled if
//           the two differ by over 3c
//   edge    e = 0.5c + the maker fee with an anchor, 1c without; inventory skew 0.1c a contract
//   quotes  post-only both sides; the YES bid at the best bid or one tick better, never above
//           fair - e - 0.1c x q, and the ask likewise; both sides in minutes 0-10 of the window,
//           only the reducing side after; an IOC flatten from T-60 s while |q| > 10
//   pulls   the book stale, the private feed down, the reference not fresh (the underlying's
//           Pyth gap, gate or no gate), the anchor too far from the microprice, and (owner) a
//           fill of ours or a 0.5 sigma move of the underlying while quotes rest
//   book    the lane's own resting quotes are taken out of the book first: the touch, the
//           sizes and the microprice are everyone else's (else a quote one tick inside the
//           touch moves the touch, and the next decision chases it)
//   flatten the T-60 s IOC goes ahead of the pulls, and of the lane pause, the event cap and the
//           pause after our fill: those stop new risk, never the last minute's reduction
//
// quote() is pure; MakerEngine owns a lane's working quotes through gw (strat thread only).
#include <cstdint>
#include <functional>
#include <future>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "deskd/bus/msg.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/kalshi_md.h"

namespace deskd {

struct MakerParams {
  Qty size = 1000;                     // per side: 10 contracts (plan: the markout leg, 10 a side)
  Px edge_anchor = 50;                 // 0.5c, plus the maker fee
  Px edge_alone = 100;                 // 1c with no anchor
  Px skew_per_contract = 10;           // 0.1c
  Px anchor_gap = 300;                 // 3c: anchor and microprice further apart pulls
  int mid_over_ticks = 3;              // a spread wider than this uses the mid
  int ttl_s = 60;                      // every order lives at most 60 s
  std::int64_t two_sided_ms = 600000;  // minutes 0-10 of the window
  std::int64_t flatten_ms = 60000;     // from T-60 s
  Qty flatten_over = 1000;             // |q| over 10 contracts
  // owner limits (any engine): the worst case one event may hold (working + positions at $1 a
  // contract), and the lane's day below which it stops quoting; 0 = none
  std::int64_t event_cap = 0;          // micro-dollars
  std::int64_t lane_pause = 0;         // micro-dollars, a loss
  // the plan's markout rule: once mo_after of the lane's fills are marked at 60 s, a mean below
  // -mo_floor per contract stops it (BAND: 100 fills, 1c; LIP-15: 200 fills, 1.5c); 0 = none
  std::int64_t mo_after = 0;
  Money mo_floor = 0;
  // Fill pace (operator, 2026-10-13: a fill every 5-10 minutes). Quoting with no fill for
  // fill_target_ms lowers the edge floor by edge_step, again each further fill_target_ms, never
  // below edge_min (the plan's 0.5c, plus the maker fee with an anchor); a fill puts it back. It
  // moves the quote toward fair only where the floor was what held it back; 0 = off.
  std::int64_t fill_target_ms = 600000;
  Px edge_step = 25;                   // 0.25c
  Px edge_min = 50;                    // 0.5c
};

struct MakerInput {
  const MdBook* book = nullptr;
  const std::vector<kalshi::PriceRange>* grid = nullptr;
  std::optional<Px> anchor;            // the model fair, only once its gate has passed
  Px maker_fee = 0;                    // per contract at this price, the venue's maker fee
  Qty position = 0;                    // the lane's signed position here
  std::int64_t now_ms = 0, open_ms = 0, close_ms = 0;
  bool private_up = false, reference_fresh = true;
  Px edge_relax = 0;                   // how far the fill pace has lowered the edge floor (MakerParams)
};

struct MakerQuote {
  Side side = Side::BUY;
  Px price = 0;
  Qty size = 0;
  bool ioc = false;                    // the T-60 s flatten
};
struct MakerDecision {
  std::vector<MakerQuote> quotes;      // what should be working
  std::string pull;                    // why nothing should be ("" when quoting)
  std::optional<Px> fair;
};
MakerDecision maker_quote(const MakerInput& in, const MakerParams& p = {});

// M2 BAND 96-99 (plan section 5): post-only on a near-certain favourite, at the back of its
// queue: a YES bid joining a best bid of 0.96-0.99, or (NO the favourite) a YES ask joining a
// best ask of 0.01-0.04; p.size a market, and no more once that much is held on that side. The
// position is held to settlement. Pulled on a stale book or the private feed down.
MakerDecision band_quote(const MakerInput& in, const MakerParams& p);

// the grid's own prices: the step at p, and the nearest grid price at or below / at or above
Px grid_step(Px p, const std::vector<kalshi::PriceRange>& grid);
std::optional<Px> grid_floor(Px p, const std::vector<kalshi::PriceRange>& grid);
std::optional<Px> grid_ceil(Px p, const std::vector<kalshi::PriceRange>& grid);
// The venue's maker fee per contract at price p (quadratic_with_maker_fees: 0.0175 x mult x p(1-p),
// rounded up to the 1/10,000 dollar); 0 on a series without maker fees.
Px maker_fee(const kalshi::MarketRow& m, Px p);

// One quoting lane's owner: keeps each market's working quotes equal to maker_quote()'s, by
// cancel and re-post (no amends), through gw. Strat thread only.
class MakerEngine {
 public:
  struct Env {
    std::function<std::shared_ptr<const Gateway::SysView>()> view;
    std::function<std::shared_ptr<const rest::Snapshot>()> snapshot;
    std::function<std::optional<MdBook>(const std::string&)> book;
    // the market's model anchor: gated once its strike replication gate has passed (plan
    // section 5: 50 windows in a row); a gated anchor with no fresh fair pulls the quotes.
    // fresh: the underlying's Pyth value is fresh (by the gap rule), whatever the gate; spot and
    // sd_log (sigma x sqrt(time left), in log terms) when the model has them, for the 0.5 sigma pull
    struct Anchor {
      bool gated = false;
      std::optional<Px> fair;
      bool fresh = true;
      double spot = 0, sd_log = 0;
    };
    std::function<Anchor(const std::string&)> anchor;
    std::function<bool()> private_up, reference_fresh;
    std::function<bool(GwMsg)> post;                              // gw's inbox
    std::function<void(std::set<std::string>)> want;              // markets whose books it needs
    // the lane's 60 s markouts so far: fills marked and the mean per contract
    std::function<std::pair<std::int64_t, Money>()> markout;
    // the fill model (strat/fill_model.h) for a resting quote: P(complete fill within ttl_s)
    std::function<double(const std::string& tk, bool buy, Qty ahead, Qty size, int ttl_s)> fill_p;
    std::function<void(const std::string& lvl, const std::string& msg)> alert;
  };
  using Decide = std::function<MakerDecision(const MakerInput&, const MakerParams&)>;
  MakerEngine(std::string lane, std::vector<std::string> series, Env env, MakerParams params = {},
              Decide decide = {});
  void step(std::int64_t now_ms);
  nlohmann::json status() const;   // any thread (deskd's /ops/status reads it from the web's)
 private:
  struct Side_ {
    std::string cid;                   // the working (or pending) quote, "" = none
    std::int64_t acted_ms = 0, backoff_until = 0;
    OrderPromise reply;
    std::future<OrderReply> answer;
  };
  std::string lane_;
  std::vector<std::string> series_;
  Env env_;
  MakerParams p_;
  Decide decide_;
  std::map<std::string, std::map<int, Side_>> sides_;   // ticker -> 0 bid, 1 ask, 2 flatten
  std::map<std::string, Qty> last_pos_;
  std::map<std::string, std::int64_t> cool_until_;      // after a fill of ours, or a 0.5 sigma move
  std::map<std::string, double> quote_spot_;            // the underlying when the quotes went out
  std::map<std::string, std::string> cool_why_;
  // what status() shows, written by step() and read from any thread: the per-market reason
  // under status_mu_ (an unguarded map was iterated by /ops/status while step() inserted into
  // it: a TSan race and a possible crash), the counters atomic
  mutable std::mutex status_mu_;
  std::map<std::string, std::string> last_pull_;
  std::uint64_t seq_ = 0;
  std::atomic<std::int64_t> places_{0}, cancels_{0}, refusals_{0};
  // the fill pace: when the lane last filled or began quoting, its fills in the last 30 minutes,
  // how many fill_target_ms steps the edge floor is down, and the expected wait for the next fill
  std::int64_t quoting_since_ms_ = 0, pace_checked_ms_ = 0;
  std::deque<std::int64_t> fill_times_;
  std::atomic<std::int64_t> last_fill_ms_{0};      // atomics: status() reads them from another thread
  std::atomic<int> relax_steps_{0}, fills_30m_{0};
  double est_every_s_ = -1;   // under status_mu_ with last_pull_
  void pace(std::int64_t now, bool running, bool quoting, const Gateway::SysView& v, const rest::Snapshot& s);
  void set_last(const std::string& tk, const std::string& why);
  std::string new_cid();
  static void without_own(MdBook& b, const Gateway::SysView& v, const std::string& tk);
  std::int64_t event_worst(const Gateway::SysView& v, const rest::Snapshot& s, const std::string& event) const;
};

}  // namespace deskd
