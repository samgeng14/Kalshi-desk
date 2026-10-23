#include "deskd/gw/fees.h"

#include <algorithm>
#include <cmath>

namespace deskd {

namespace {
constexpr Micros kCent = 10000;   // micro-dollars

// A fee rate in 1/scale steps, rounded up (a Kalshi multiplier) or to the nearest step (a
// Polymarket rate, which the venue lists in decimals). Not finite or not positive: 0. The cap only
// keeps the cast defined; fee_ready() refuses a multiplier over 1000 long before it.
std::int64_t rate_steps(double rate, double scale, bool up) {
  if (!(rate > 0)) return 0;
  const double x = std::min(rate, 1e6) * scale;
  return static_cast<std::int64_t>(up ? std::ceil(x - 1e-6) : std::llround(x));
}

Micros floor_mod(Micros x, Micros unit) {
  const Micros r = x % unit;
  return r < 0 ? r + unit : r;
}

Micros ceil_to(Micros x, Micros unit) { return (x + unit - 1) / unit * unit; }   // x >= 0

// num / den rounded to the nearest whole number, half to even (num >= 0)
__int128 half_even(__int128 num, __int128 den) {
  __int128 q = num / den;
  const __int128 r = num % den;
  if (2 * r > den || (2 * r == den && q % 2 != 0)) ++q;
  return q;
}

// rate x C x p x (1 - p) in cents, unrounded: rate_u q p (1e4 - p) / 1e14 with the rate in
// millionths, C = q / 100 and p in 1/10,000
constexpr __int128 kPolyDen = static_cast<__int128>(100000000) * 1000000;
__int128 poly_num(Px px, Qty qty, double rate) {
  const std::int64_t rate_u = rate_steps(rate, 1e6, false);
  if (rate_u <= 0 || qty <= 0 || px <= 0 || px >= kOneDollar) return 0;
  return static_cast<__int128>(rate_u) * qty * px * (kOneDollar - px);
}
}  // namespace

Micros kalshi_model_fee(double mult, Px px, Qty qty) {
  // 0.07 x (q / 100) x (p / 1e4) x ((1e4 - p) / 1e4) x (milli / 1e3) dollars
  //   = 7 q p (1e4 - p) milli / 1e9 micro-dollars, in 128 bits as the fake does
  const std::int64_t milli = rate_steps(mult, 1000, true);
  if (milli <= 0 || qty <= 0 || px <= 0 || px >= kOneDollar) return 0;
  const __int128 num = static_cast<__int128>(7) * qty * px * (kOneDollar - px) * milli;
  const __int128 den = 1000000000;
  return static_cast<Micros>((num + den - 1) / den);
}

Micros kalshi_taker_fee(double mult, Px px, Qty qty, Micros precision) {
  if (qty <= 0 || px <= 0 || px >= kOneDollar) return 0;
  // fee_rounding.md: the change -(cost + fee) is floored to the precision. Only the part of the
  // cost (q x p micro-dollars) under one unit moves the rounding.
  const Micros unit = std::max<Micros>(1, precision);
  const Micros part = floor_mod(qty * px, unit);
  return ceil_to(part + kalshi_model_fee(mult, px, qty), unit) - part;
}

Micros order_fee_bound(double mult, const std::vector<MdLevel>& fills, Micros precision) {
  // The order is charged its model fees plus what its accumulator keeps. Each fill's rounding fee
  // is under one unit. While every cost is a whole number of units, a fill's fee and rounding fee
  // add up to whole units, so its rebate can always take the accumulator back under one unit; a
  // fill whose cost is not can leave up to one more unit in it for good (charge_fee caps a rebate
  // at the fill's own fees, and that fill's fees may be under a unit).
  const Micros unit = std::max<Micros>(1, precision);
  Micros bound = unit;
  for (const auto& f : fills) {
    if (f.qty <= 0) continue;
    bound += kalshi_model_fee(mult, f.px, f.qty);
    if (floor_mod(f.qty * f.px, unit) != 0) bound += unit;
  }
  return bound;
}

Money kalshi_fee_order_cent(Px px, Qty qty, double mult) {
  return ceil_to(kalshi_model_fee(mult, px, qty), kCent) / 100;   // micro-dollars -> Money
}

Micros poly_taker_fee_fill(Px px, Qty qty, double coeff) {
  return static_cast<Micros>(half_even(poly_num(px, qty, coeff), kPolyDen)) * kCent;
}

Micros poly_maker_rebate_fill(Px px, Qty qty, double rate) {
  return -static_cast<Micros>(half_even(poly_num(px, qty, rate), kPolyDen)) * kCent;
}

Qty min_rebate_size(Px px, double rate) {
  // A rebate rounds to 1c or more once it is over half a cent (exactly half rounds to 0, the even
  // one): n contracts with n x per > den / 2, so n = floor(den / (2 per)) + 1.
  const __int128 per = poly_num(px, kOneContract, rate);
  if (per <= 0) return 0;
  return static_cast<Qty>(kPolyDen / (2 * per) + 1) * kOneContract;
}

}  // namespace deskd
