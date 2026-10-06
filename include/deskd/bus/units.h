#pragma once
// Prices, money and sizes as integers (plan section 2). A price or a dollar amount is in
// u = $0.0001, the unit the page's Desk.num converts to at the door, so the two never disagree
// by a rounding step. A size is in 1/100 of a contract: Kalshi trades 0.01-contract steps, and
// an integer count never compares 0.1 + 0.2 against 0.3.
#include <cstdint>
#include <optional>
#include <string>

namespace deskd {

using Px = std::int64_t;     // 1/10,000 dollar: "0.6400" -> 6400
using Money = std::int64_t;  // the same unit, signed: "-3.2100" -> -32100
using Qty = std::int64_t;    // 1/100 contract: 10 contracts -> 1000, 85.5 -> 8550

constexpr Px kOneDollar = 10000;
constexpr Qty kOneContract = 100;

// The contract's PRICE: one digit, a point, exactly 4 decimals, "0.0000".."1.0000".
std::optional<Px> parse_price(const std::string& s);
std::string price_str(Px p);  // 6400 -> "0.6400"

// The contract's MONEY: an optional minus, 1-9 digits, a point, exactly 4 decimals.
std::optional<Money> parse_money(const std::string& s);
std::string money_str(Money m);  // -32100 -> "-3.2100"; zero is never "-0.0000"

// A JSON size (10, 85.5, 0.01) -> 1000, 8550, 1. nullopt when it is not finite, finer than
// 0.01, or negative without allow_negative (a position is signed, an order size is not).
std::optional<Qty> qty_from_number(double v, bool allow_negative = false);
double qty_to_number(Qty q);  // 1050 -> 10.5, for JSON

}  // namespace deskd
