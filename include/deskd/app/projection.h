#pragma once
#include <functional>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>
#include "deskd/gw/kalshi_wire.h"

namespace deskd {
namespace rest { struct Snapshot; }
// Stable IDs for foreign orders too; a foreign CID is never given a Manual prefix.
std::string screen_cid(const kalshi::OrderRow& row);
nlohmann::json rest_order_json(const kalshi::OrderRow& row, std::int64_t now_ms);
// mark: the live book's mid (unrl is then the position marked there); real and fees: today's.
nlohmann::json rest_position_json(const kalshi::PositionRow& row, std::optional<Px> mark = std::nullopt,
                                  Money real = 0, Money fees = 0);

// Today's executions as the contract's fill rows, and today's realized P&L per market, by
// average cost in YES terms. The day starts flat or at the position Kalshi reports less today's
// fills; that starting cost is recovered by replaying today's fills backwards from Kalshi's
// current position and cost. It cannot be recovered when the position went flat or flipped
// after the day's start and before the end: a fill reducing inventory of unknown cost then
// counts only its fee (unknown_basis counts them).
struct DayPnl {
  std::vector<nlohmann::json> fills;                          // oldest first
  std::map<std::pair<std::string, int>, Money> real, fees;    // (ticker, shard) -> today
  Money real_total = 0;
  std::size_t unknown_basis = 0;
};
using MidFn = std::function<std::optional<Px>(const std::string&)>;
DayPnl day_pnl(const rest::Snapshot& state, std::int64_t day_start_ms, const MidFn& mid);
// REST balance is AVAILABLE cash, already excluding venue reservations. Reconstruct the
// contract's cash = free + modeled resting collateral, never subtract the collateral twice.
// Reducing orders share a position's room, best price first, as in contract.js.
nlohmann::json rest_cash_json(const rest::Snapshot& state, bool read_only);
}
