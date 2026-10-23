#pragma once
// How one event's markets settle together, and what a book of stakes in them can lose (plan
// section 2's "What stays": the old risk.cpp event_payoffs, stack_check and loss_in_state, and
// ladder.cpp's spans, as pure functions on deskd's units). Pure; any thread: gw's risk book and
// strat's basket finder share it.
//
// An event's shape is its settlement states, each the markets that pay YES in it. Two things
// prove one, and nothing else does:
//   - the venue's mutually_exclusive flag (kalshi::EventRow): at most one market pays YES;
//   - the markets' strikes, when every market of the event has one (span_of): the settle value
//     falls in one region between the strikes, and that region pays the markets whose spans hold
//     it. Strikes show a cumulative ladder ("greater 79", "greater 83") paying both at 84, and
//     prove a ladder exhaustive (a market pays YES whatever the value) when the spans tile the
//     real line, or the series' settle lattice: KXHIGH settles on whole degrees, so "between
//     79-80" and "between 81-82" leave nothing between them (docs/edge_results.json).
// With neither, the markets may settle independently and the event has no shape: gw counts each
// market alone, as it always has. A flag the strikes contradict (two spans overlap) is no shape
// either. The state where no market pays YES ("none of them") stays unless exhaustiveness is
// proven.
//
// A stake is one thing held or working in one market: YES or NO, its size, and what it cost (the
// most it can lose). It wins $1 a contract in the states its side settles in. A working order is
// contingent: it may fill in part or not at all, so in each state it counts the worse of filling
// and not filling, and never pays for the rest of the book (a resting hedge releases nothing).
//
// Prices in Px and sizes in Qty (bus/units.h); money in micro-dollars, Px x Qty.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/bus/units.h"
#include "deskd/gw/kalshi_wire.h"

namespace deskd {

using Micros = std::int64_t;   // 1/1,000,000 dollar (as kalshi::Micros): Px x Qty

// The settle values a market pays YES on, from its strike type (the venue's words):
//   greater(f) = (f, inf)   greater_or_equal(f) = [f, inf)   less(c) = (-inf, c)
//   less_or_equal(c) = (-inf, c]   between(f, c) = [f, c]
// ok false for any other type (structured, functional, custom), a strike it needs missing or not
// a number, or a between whose cap is under its floor. The old ladder.cpp read "less 79" as 78 or
// below, which holds only on whole numbers: that is the series' lattice, not the market's.
struct Span {
  double lo = 0, hi = 0;
  bool lo_open = true, hi_open = true, lo_inf = false, hi_inf = false, ok = false;
  bool holds(double x) const;   // false when not ok
};
Span span_of(const kalshi::MarketRow& m);

struct EventShape {
  std::string event, series;
  bool exclusive = false;    // the venue's mutually_exclusive is true (and no two spans overlap)
  bool exhaustive = false;   // proven: some market pays YES in every state
  bool spans = false;        // every market has a span, and the states come from them
  std::vector<std::string> tickers;          // every market of the event, low strike first
  std::vector<std::vector<int>> states;      // each state: the indices that pay YES; {} = none of them
  int index(const std::string& ticker) const;   // -1: not in the event
};
// e's shape, from the flag and its markets' rows in `markets` (the listing). lattice_step: the
// series settles on its multiples (1: KXHIGH's whole degrees); 0, anywhere on the real line.
// nullopt with *why when e lists no markets, one of them is not in `markets`, the flag is not true
// and a market has no span, or the flag is true and two spans overlap (a contradiction the caller
// logs).
std::optional<EventShape> event_shape(const kalshi::EventRow& e, const std::map<std::string, kalshi::MarketRow>& markets,
                                      double lattice_step, std::string* why);

struct Stake {
  std::string ticker;
  bool yes = true;           // the side held: YES, or NO (YES sold)
  Qty qty = 0;
  Micros cost = 0;           // what it cost: the most it can lose
  bool contingent = false;   // a working order
};
// A lane's position in a market (SysBook::Inv: pos signed, cost what a long cost or a short
// brought in) as a stake: a long holds YES at its cost, a short NO at $1 a contract less that.
Stake position_stake(const std::string& ticker, Qty pos, Micros cost);
// A working order as a stake: contingent, on its side, at its reservation (what gw reserved for
// the contracts it may still add, OrderLedger::Order::reservation; 0 for a reducing order).
Stake order_stake(const std::string& ticker, Side side, Qty qty, Micros reservation);

// What the stakes lose in each of s's states (cost less payout; < 0 is a gain), as s.states. A
// stake in a market s does not list never pays.
std::vector<Micros> state_losses(const EventShape& s, const std::vector<Stake>& st);
// The max and the min of state_losses; worst < 0 is a locked gain. A shape with no states proves
// nothing: both are then each stake's own worst case, summed.
Micros worst_loss(const EventShape& s, const std::vector<Stake>& st);
Micros best_loss(const EventShape& s, const std::vector<Stake>& st);

// What one more leg does to the event's worst case, the only question that tells a hedge from the
// same bet twice (2026-09-05: YES KXHIGHLAX 79-80, then NO 81+, which lost together at 81+). Worst
// cases here are at least 0. stacks: it rises by the leg's whole cost, so the leg pays back in no
// state the book is at its worst in. A first leg, with nothing at risk before it, stacks nothing.
struct StackCheck {
  Micros before = 0, after = 0;
  Micros added = 0;        // the leg's cost
  Micros protection = 0;   // added less the worst case's rise: what it buys back
  bool stacks = false;
};
StackCheck stack_check(const EventShape& s, const std::vector<Stake>& existing, const Stake& add);

// The risk book's worst case over stakes in many events: each shaped event's stakes count
// max(0, worst_loss), working orders contingent at their reservations; every other stake (no
// event, an event shape_of has no shape for, a market its shape does not list) counts alone, as gw
// counts it today: a long its cost, a short $1 x |pos| less its proceeds, a working order its
// reservation.
struct RiskStake : Stake {
  std::string event;
};
Micros risk_used(const std::vector<RiskStake>& s, const std::function<const EventShape*(const std::string& event)>& shape_of);

}  // namespace deskd
