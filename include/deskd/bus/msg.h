#pragma once
// The messages deskd's workers pass (plan section 2, bus/msg.h). Prices are integers in
// 1/10,000 of a dollar and counts in 1/100 of a contract (bus/units.h). Every side is in YES
// terms, as on the page: SELL with no long YES is a BUY NO at 1 - price.
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "deskd/bus/queue.h"
#include "deskd/bus/units.h"

namespace deskd {

enum class Venue : std::uint8_t { K, P };           // Kalshi, Polymarket US
enum class Side : std::uint8_t { BUY, SELL };        // YES terms
enum class Owner : std::uint8_t { MAN, SYS, EXT };   // EXT: on the account, not placed by deskd
// TAKE is IOC (fill now, cancel the rest; the price is the slippage cap) or FOK (all or none);
// POST is post-only with an exchange-side expiry. There is no GTC.
enum class Tif : std::uint8_t { IOC, FOK, POST };
enum class OrdState : std::uint8_t { PEND, WORK, PART, FILL, CXL, REJ, EXP };
enum class RunState : std::uint8_t { RUN, STOP, KILLED, HALTED };

const char* venue_code(Venue v);   // "K", "P"
const char* run_code(RunState s);  // "RUN", "STOP", "KILLED", "HALTED"

// gw's answer to one new order, for the web thread that posted it. ord is the order's contract
// row as JSON text ("" when it never reached gw's book: a check refused it, or nothing was sent).
struct OrderReply {
  std::string code;   // "" = accepted by the venue (or ended CXL there: POST_CROSS, STP, USER)
  std::string msg;
  std::string ord;
};
using OrderPromise = std::shared_ptr<std::promise<OrderReply>>;

// One new order. The cid carries its owner (M... for Manual, S<lane>-... for Systematic) so
// ownership survives a crash; an unprefixed open order on the account is foreign.
struct PlaceIntent {
  std::string cid;
  Owner owner = Owner::MAN;
  std::string lane;                 // SYS only
  Venue venue = Venue::K;
  std::string ticker;
  Side side = Side::BUY;
  Px price = 0;                     // POST: the limit; TAKE: the worst acceptable price
  Qty count = 0;
  Tif tif = Tif::IOC;
  int ttl_s = 0;                    // POST: the expiry from now, or 0 with exp_ms
  std::int64_t exp_ms = 0;          // POST: an absolute expiry (a game's start), whole seconds
  int slip_ticks = 1;               // TAKE: how far past the touch it may fill ("fills up to X")
  bool override_guard = false;      // skips the 2-tick fat-finger guard (Shift+click, OVERRIDE)
  bool reduce_only = false;         // IOC only, as Kalshi allows
  // The market's and lane's cancel count when the intent was made. gw drops an intent older
  // than a cancel it already drained, so a click followed at once by Esc never rests.
  std::uint64_t cancel_epoch = 0;
  std::int64_t t_click_ms = 0;      // browser click time (performance.timeOrigin + timeStamp)
  std::int64_t t_recv_ms = 0;
  OrderPromise reply;               // MAN from the web: answered once the venue has (or never will)
};

// Expiry-only amend (plan section 3: no price amends in the first live weeks). gw builds the
// venue body from its own latest order state, never from the sender's copy, because Kalshi
// wants "the current price and total quantity" resent with it.
struct AmendIntent {
  std::string cid;
  std::int64_t expiry_ms = 0;
};

struct CancelReply {
  std::string code, msg;
  std::vector<std::string> cids;   // orders asked to cancel (contract: n = cids.size())
};
using CancelPromise = std::shared_ptr<std::promise<CancelReply>>;

struct CancelIntent {
  enum class Scope : std::uint8_t { CID, MARKET, LANE, ALL };
  enum class Own : std::uint8_t { MAN, SYS, ANY };
  Scope scope = Scope::CID;
  std::string cid;          // CID
  Venue venue = Venue::K;   // MARKET
  std::string ticker;       // MARKET
  std::string lane;         // LANE
  Own own = Own::ANY;       // MARKET and ALL
  bool has_side = false;    // MARKET: one ladder level (right-click on WRK) when side and/or px are set
  Side side = Side::BUY;
  Px px = 0;                // 0 = any price
  CancelPromise reply;
};

struct RunView {
  RunState state = RunState::STOP;
  std::int64_t since_ms = 0;   // 0 = null
  std::int64_t rearm_ms = 0;   // KILLED only; 0 = null
  std::string why;             // '' or a short code: 'operator', 'daily stop'
};

struct PreflightRow {
  std::string id;   // KEY CASH SHARD CLK DISK FEED PRIV GROUPS
  bool ok = false;
  std::string why;
};

// gw's answer to a control message the web worker waits on (Start, Stop).
struct ControlReply {
  std::string code;   // "" = ok, else the contract's error code (PREFLIGHT, KILLED, HALTED ...)
  std::string msg;
  RunView run;
  int n = 0;          // orders asked to cancel
  std::vector<PreflightRow> preflight;
};
using ControlPromise = std::shared_ptr<std::promise<ControlReply>>;

// KILL and its re-arm are no messages: the route trips the kill switch (gw/kill.h), and gw and
// send-cx each watch its generation, woken by the trip. A KILL therefore needs no queue slot and
// cannot be dropped, and gw applies a re-arm on its own thread before it handles a Start, so a
// Start answered RUN is never flipped back to STOP by a re-arm message arriving after it (review,
// 2026-10-06).
struct Start {
  ControlPromise reply;
};
struct Stop {
  std::string why;
  ControlPromise reply;
};

// A sender's answer for one write. 5xx, a timeout or no response is UNKNOWN, never "failed":
// the order may rest at the venue (plan section 2, kept from executor.cpp:225-251).
struct SendResult {
  enum class Outcome : std::uint8_t { ACK, REJECTED, UNKNOWN };
  std::string cid;
  Outcome outcome = Outcome::UNKNOWN;
  int http_status = 0;
  std::string order_id;
  Qty filled = 0;        // the 201's fill_count: raises cumulative filled, never the position
  std::string reason;
  std::int64_t t_sent_ms = 0;
  std::int64_t t_ack_ms = 0;
};

// An order update from the private feed (or a REST reconcile).
struct OrderEvent {
  std::string cid;
  std::string order_id;
  OrdState state = OrdState::PEND;
  Qty filled = 0;
  Qty left = 0;
  Px avg = 0;
  std::string reason;
};

// Runs fn on the gw thread: tests freeze gw with it, and a status read of gw-owned state goes
// through it rather than through a lock.
struct Probe {
  std::function<void()> fn;
};

// The Kalshi Manual or cancel sender's answer for one write (net/kalshi_sender.h), back to gw.
struct KalshiWriteReply;
struct ManualWrite {
  std::shared_ptr<const KalshiWriteReply> reply;
};

using GwMsg = std::variant<PlaceIntent, AmendIntent, CancelIntent, Start, Stop, SendResult, OrderEvent, Probe, ManualWrite>;

// Stop goes in the control lane, which nothing else fills; cancels and private events go before
// new orders (plan section 2). Start and amends wait in the normal lane with the orders, so a
// Stop sent after a Start overtakes it and deletes it instead of being undone by it.
Lane lane_of(const GwMsg& m);

}  // namespace deskd
