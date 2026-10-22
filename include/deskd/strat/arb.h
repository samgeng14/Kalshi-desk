#pragma once
// S1's basket finder (plan 5(c)): baskets of IOC legs in one event that pay more than they cost,
// fees included, in every way the event can settle. It needs no forecast, only the event's shape
// (gw/event_risk.h: its settlement states, from the venue's mutually_exclusive flag and the
// markets' strikes). The old ladder.cpp's find_ladder_arb and find_dominance_arb, on deskd's units
// and books. Pure; any thread. The leg executor (ArbEngine, A6) fires what it finds.
//
// Three families, each needing what proves its lock:
//   YES_COVER  buy YES on every market of an event proven exhaustive (some market pays YES in
//              every state): at least $1 a contract back whatever settles
//   NO_SET     sell YES (buy NO) on two or more markets of an event the venue flags mutually
//              exclusive: at most one of them pays YES, so |S| - 1 of the NO legs pay at least.
//              The legs are picked as the old scanner did: the fattest bids first, each adding one
//              while the sum has not passed $1 and after that only while it raises the lock per
//              dollar the basket ties up
//   DOMINANCE  sell YES on market i and buy YES on market j where every state paying i pays j
//              ("84 or above" implies "80 or above"; strikes on both): one leg pays in every state
// Whatever a family proposes is then judged on the event's states alone (state_losses): a basket
// qualifies only if its least payout over every state, less what its legs cost walked through the
// book and the bound on their fees (gw/fees.h order_fee_bound), clears Limits::thr a contract.
//
// A leg is an IOC: a BUY takes YES at the asks, a SELL takes the YES bids (buying NO at 1 - the
// bid). It walks everyone else's book (book_without_own, strat/maker.h) from the touch through at
// most Limits::max_ticks grid ticks, so admission's PRICE_BAND (two ticks) never refuses it, and
// stops short of Systematic's own resting order on the other side: a SELL's limit stays above
// every Systematic bid in the market and a BUY's below every Systematic ask, or the venue's
// self-trade prevention (taker_at_cross) would cancel the leg against M2's quote. A leg the touch
// itself would cross is dropped. 1c legs are allowed: deskd has no min_price.
//
// A market can be a leg only if it is open and active, closes over Limits::min_close_lead_ms from
// now, settles today or tomorrow (ET, rest::outcome_date), has fresh fee metadata
// (rest::fee_ready), an exchange index and a price grid, and a book that is not stale or crossed.
// A whole event is left alone while Manual is in it anywhere (ArbMarket::manual): one basket must
// never trade against a hand's position or resting order, nor in a market Manual took over.
//
// Size: whole contracts, at most Limits::max_qty, what every leg's levels show and gw's inventory
// room on its side (Systematic's position across lanes, and its working orders, under
// Limits::inventory a market: SYS_CEILING "inventory"), the lane's budget and the venue's free
// cash. The budget holds what gw will count while the legs work: with deskd.event_risk on
// (Limits::by_state) the event's worst case with every leg contingent at its reservation
// (Plan::contingent_worst), else every leg's reservation market by market (Plan::gross), as gw
// counts S1 today. The cash holds the reservations and the fees. Of what fits, the most contracts
// that still lock thr each (find_baskets' binary search).
//
// Prices in Px and sizes in Qty (bus/units.h); money in micro-dollars, Px x Qty.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "deskd/bus/units.h"
#include "deskd/gw/event_risk.h"
#include "deskd/gw/fees.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/rest/state.h"

namespace deskd::arb {

enum class Kind : std::uint8_t { YES_COVER, NO_SET, DOMINANCE };
const char* kind_str(Kind k);   // "YES_COVER", "NO_SET", "DOMINANCE"

struct Leg {
  std::string ticker;
  int market = -1;          // its index in the event's shape
  Side side = Side::BUY;    // BUY: YES at the asks; SELL: the YES bids (NO at 1 - the bid)
  Px planned = 0;           // the touch it plans to take
  Px limit = 0;             // the deepest level the basket's size walks to, at most max_ticks through the touch
  Qty shown = 0;            // what the book shows at or better than limit (the scarcer, the earlier it goes)
  Micros cost = 0;          // the basket's size walked: the YES prices bought, or 1 - the bids sold
  Micros fee = 0;           // order_fee_bound over those fills
};

struct Plan {
  std::string event, id;    // id: the engine's, when it fires
  Kind kind = Kind::NO_SET;
  std::vector<Leg> legs;    // the scarcest first
  Qty qty = 0;              // every leg's size
  int shard = -1;           // the legs' exchange index (-1: they differ)
  // cost and fees: the legs' sum; payout_min: the least the legs pay over the event's states;
  // edge = payout_min - cost - fees, at least Limits::thr x qty
  Micros cost = 0, fees = 0, payout_min = 0, edge = 0;
  Micros contingent_worst = 0;   // the event's worst case with every leg working, at its reservation
  Micros gross = 0;              // every leg's reservation (BUY: limit x qty, SELL: (1 - limit) x qty) summed
};

struct Limits {
  Money thr = 100;                     // the lock a basket contract must clear, after fees (LaneSpec S1 thr: 1c)
  Qty max_qty = 1000;                  // a basket's size at most (deskd.arb_max_qty: 10 contracts)
  Qty inventory = 2000;                // gw's limit on Systematic's position in a market (LaneSpec S1 inventory)
  Micros budget_left = 0;              // what the lane's budget, the 70% ceiling and the event's group leave
  Micros cash_left = 0;                // the venue's free cash
  bool by_state = false;               // gw counts S1 by settlement state (deskd.event_risk): budget_left
                                       // holds contingent_worst, else gross
  int max_legs = 6;                    // deskd.arb_max_legs
  int max_ticks = 2;                   // grid ticks a leg may walk through its touch
  Micros fee_precision = kFeePrecision;   // the account's balance precision (deskd.fee_precision)
  std::int64_t min_close_lead_ms = 300000;   // every leg's market closes over 5 min from now
};

// One market of the event, as the finder sees it (arb_market builds it).
struct ArbMarket {
  const kalshi::MarketRow* row = nullptr;   // the listing's
  std::optional<MdBook> book;               // everyone else's book (book_without_own)
  Px own_bid = 0, own_ask = kOneDollar;     // Systematic's highest bid and lowest ask working here (none: 0, $1)
  // gw's inventory measure: Systematic's position here, every lane's, signed; and what is left
  // of its working orders here, by side
  Qty sys_position = 0, sys_buying = 0, sys_selling = 0;
  std::string manual;                       // why Manual keeps the whole event out ("" when it does not)
};

// tk's ArbMarket from what strat holds: book is md's (nullopt: none yet), taken down to everyone
// else's. manual: the market is Manual's (SysView::held, gw's takeover), or the account read
// shows an order resting there that is not Systematic's (Manual's, or another app's: its client
// order id does not start with S), or a position there that Systematic's lanes do not hold, or
// there is no account read yet. The account read lags Systematic's own fills by up to a poll, so
// an event Systematic has just traded in stays out until it agrees (fail closed).
ArbMarket arb_market(const kalshi::MarketRow& row, std::optional<MdBook> book, const Gateway::SysView& v,
                     const rest::Snapshot& s);

// The baskets the event's markets offer now, the best edge first, at most one of each kind (a
// DOMINANCE pair is the best of its pairs). m: the event's markets (any others are ignored).
// none: *why says what stopped each family (Manual in the event, a filter, no lock after fees, no
// size), for the scan log.
std::vector<Plan> find_baskets(const EventShape& s, const std::vector<ArbMarket>& m, std::int64_t now_ms,
                               const Limits& lim, std::string* why = nullptr);

// What holding legs, qty each at its cost and fee, locks over s's states: the least payout less
// the legs' cost and fees (state_losses). A plan's edge; a basket's, on what its legs filled.
Micros plan_edge(const EventShape& s, const std::vector<Leg>& legs, Qty qty);

}  // namespace deskd::arb
