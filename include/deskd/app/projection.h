#pragma once
#include <nlohmann/json.hpp>
#include "deskd/gw/kalshi_wire.h"

namespace deskd {
namespace rest { struct Snapshot; }
// Stable IDs for foreign orders too; a foreign CID is never given a Manual prefix.
std::string screen_cid(const kalshi::OrderRow& row);
nlohmann::json rest_order_json(const kalshi::OrderRow& row, std::int64_t now_ms);
nlohmann::json rest_position_json(const kalshi::PositionRow& row);
// REST balance is AVAILABLE cash, already excluding venue reservations. Reconstruct the
// contract's cash = free + modeled resting collateral, never subtract the collateral twice.
// Reducing orders share a position's room, best price first, as in contract.js.
nlohmann::json rest_cash_json(const rest::Snapshot& state, bool read_only);
}
