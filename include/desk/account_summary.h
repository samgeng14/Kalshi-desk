#pragma once

#include "desk/config.h"
#include "desk/store.h"

namespace desk {

// Read-only account totals. Perpetual settled funds are cash; available margin is
// buying power. Equity already includes position value and is never added twice.
json account_summary(Store& store, const Config& cfg, const json& perps_view);

// Pure accounting core, also used by fixture tests. Each account input has cash,
// equity, ts, an optional baseline {equity,ts}, and optional pnl_total / pnl_recorded.
// Cashflows use the events ledger sign; only to_margined/from_margined have an
// equal and opposite perps leg. Unknown values remain JSON null.
json calculate_account_summary(const json& inputs, long long now, long long day_start);

}  // namespace desk
