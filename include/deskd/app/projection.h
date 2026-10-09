#pragma once
#include <nlohmann/json.hpp>
#include "deskd/gw/kalshi_wire.h"

namespace deskd {
// Stable IDs for foreign orders too; a foreign CID is never given a Manual prefix.
std::string screen_cid(const kalshi::OrderRow& row);
nlohmann::json rest_order_json(const kalshi::OrderRow& row, std::int64_t now_ms);
nlohmann::json rest_position_json(const kalshi::PositionRow& row);
}
