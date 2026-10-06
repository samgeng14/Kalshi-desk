#pragma once
// The fake Kalshi's wire formats: fixed-point strings as the venue accepts and sends them, ids and
// RFC 3339 times.
//
// Written apart from deskd's own parsers (include/deskd/bus/units.h) on purpose: a fake that
// shared deskd's parsing would agree with deskd's mistakes, and a test against it would pass.
//
// The venue's rules (docs.kalshi.com/api-reference/orders/create-order-v2.md, fetched 2026-10-06):
//   FixedPointCount   "Requests accept 0-2 decimal places (e.g., "10", "10.0", "10.00");
//                     responses always emit 2 decimals ... the minimum granularity is 0.01."
//   FixedPointDollars "Most request fields accept 2-4 decimal places (e.g., "0.56", "0.5600");
//                     responses emit up to 6."
// Prices on the wire are always the YES price ("this endpoint quotes everything from the YES
// side"); a book level on the NO side is a NO price (orderbook-updates.md).
#include <cstdint>
#include <optional>
#include <random>
#include <string>

namespace fakex {

using Px = std::int64_t;      // a price in 1/10,000 dollar: "0.5600" -> 5600
using Qty = std::int64_t;     // contracts in 1/100: "10.00" -> 1000
using Micros = std::int64_t;  // money in 1/1,000,000 dollar: the venue rounds fees to $0.000001

constexpr Px kDollar = 10000;
constexpr Qty kContract = 100;
constexpr Micros kMicrosPerDollar = 1000000;

// A request price: digits, a point, 2 to 4 decimals ("0.56", "0.5600"). nullopt otherwise.
std::optional<Px> parse_price_req(const std::string& s);
// A request count: digits with 0 to 2 decimals ("10", "10.5", "10.00"); never negative.
std::optional<Qty> parse_count_req(const std::string& s);
// Any fixed-point dollar or count string with up to `max_dp` decimals and an optional minus, for
// the tape and scenario files, which hold what the venue sent ("2774.20", "-62.00", "0.9980").
std::optional<std::int64_t> parse_fixed(const std::string& s, int dp, bool allow_negative);

std::string px_str(Px p);          // 5600 -> "0.5600"
std::string qty_str(Qty q);        // 1000 -> "10.00", -350 -> "-3.50"
std::string usd6(Micros m);        // 10000 -> "0.010000"   (fee_cost, user_order costs and fees)
std::string usd4(Micros m);        // 4 decimals when exact, else 6: "50.0000", "0.005612"

// p * q is exactly micro-dollars: 1/10,000 times 1/100 is 1/1,000,000.
inline Micros notional(Px p, Qty q) { return static_cast<Micros>(p) * q; }

std::string rfc3339(std::int64_t ms);   // "2026-10-06T12:00:00.123000Z"

// Version-4 UUID text from a seeded generator: ids repeat run to run, which tests rely on.
std::string uuid4(std::mt19937_64& rng);

}  // namespace fakex
