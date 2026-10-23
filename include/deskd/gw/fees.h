#pragma once
// The venues' fees, one module for gw and every engine (decisions 13): Kalshi's taker fee and
// its rounding, the bound on one order's fees, the cent rule the take engines size clips with,
// and Polymarket US's per-fill taker fee and maker rebate. Pure; any thread.
//
// Kalshi (redesign_all kalshi K6 and getting_started/fee_rounding.md, as the fake exchange's
// model_fee and charge_fee model them): a taker fill's model fee is 0.07 x mult x C x P x (1 - P),
// rounded up to $0.000001. The fill's balance change (its cash less that fee) is then floored to
// the account's balance precision, $0.0001 for a direct member or $0.01, and the difference is a
// rounding fee kept in the order's accumulator: whole precision units in it come back on a later
// fill of the same order, never more than that fill's own fees. deskd assumes $0.01
// (deskd.fee_precision, decisions 7) until demo fills prove $0.0001. admit_manual and
// rest::take_edge keep their own allowances.
//
// Polymarket US (ui/desk/js/core.js Desk.fees.pmFill, until the venue confirms it): a taker pays
// coeff x C x p x (1 - p), the market's own coefficient; a maker is paid 0.0125 x C x p x (1 - p);
// each fill is rounded to the cent, half to even.
//
// Prices in Px and sizes in Qty (bus/units.h). Fees are in micro-dollars, as the venues write
// them, except kalshi_fee_order_cent's, which is Money like the edges it is taken from.
#include <cstdint>
#include <vector>

#include "deskd/bus/units.h"
#include "deskd/md/kalshi_md.h"

namespace deskd {

using Micros = std::int64_t;   // 1/1,000,000 dollar (as kalshi::Micros)

// deskd.fee_precision's default: $0.01, a non-direct member's balance precision
constexpr Micros kFeePrecision = 10000;
// Polymarket US's maker rebate rate
constexpr double kPolyMakerRebate = 0.0125;

// One Kalshi taker fill's model fee, rounded up to $0.000001 (fakex::model_fee). mult: the
// market's fee_multiplier, taken in thousandths and rounded up; one that fee_ready() refuses as
// not finite or negative counts as 0. Either side's price: P x (1 - P) is the same.
Micros kalshi_model_fee(double mult, Px px, Qty qty);

// What a taker fill of qty at px is charged when it is its order's first fill, at a balance
// precision (100 = $0.0001, 10,000 = $0.01): the fill's cost and model fee together, rounded up to
// the precision, less the cost. px is the price paid, on the side bought (a SELL of YES at p buys
// NO at 1 - p): once the cost is not a whole number of units the two sides round differently.
// fee_rounding.md's one contract bought at 5.5c is charged $0.005 at $0.01. The same as
// fakex::charge_fee(-cost, model fee, precision, accumulator 0), and for a sale that closes a
// position too.
Micros kalshi_taker_fee(double mult, Px px, Qty qty, Micros precision = kFeePrecision);

// The most one order's taker fills can be charged in all, at a balance precision: their model
// fees, one unit for what the accumulator may keep, and one more for every fill whose cost is
// not a whole number of units, since the rounding fee such a fill pays need not come back (two
// fills of one contract at 0.1c are charged $0.009 each at $0.01). Each fill's px may be either
// side's price: the bound is the same.
Micros order_fee_bound(double mult, const std::vector<MdLevel>& fills, Micros precision = kFeePrecision);

// The take engines' fee for one order (C1, X1, N1, N2 and the Python tools that backtest them):
// the model fee rounded up to the cent, in Money. Five contracts at 40c: $0.09.
Money kalshi_fee_order_cent(Px px, Qty qty, double mult);

// Polymarket US, one fill, rounded to the cent half to even. The taker's fee is >= 0; coeff is
// the market's taker coefficient. The maker's rebate is <= 0 (paid to us): one contract at 50c
// earns nothing ($0.003125), two earn 1c.
Micros poly_taker_fee_fill(Px px, Qty qty, double coeff);
Micros poly_maker_rebate_fill(Px px, Qty qty, double rate = kPolyMakerRebate);
// The smallest whole size whose rebate rounds to at least 1c: 2 contracts at 50c, 5 at 90c.
// 0 when no size earns one (px not inside (0, 1), or no rate).
Qty min_rebate_size(Px px, double rate = kPolyMakerRebate);

}  // namespace deskd
