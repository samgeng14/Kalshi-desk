#pragma once
// Historical studies against the public Kalshi tape, ESPN and IEM ASOS.
//   kalshi-desk research backfill KXEPLGAME,KXLALIGAGAME [pages]   settled markets + trades -> hist_*
//   kalshi-desk research longshot KXEPLGAME [minutes_before]        SPORT-3 / FIN-2: win rate by price
//   kalshi-desk research tied80  KXEPLGAME,KXLALIGAGAME             SPORT-1 backtest via goal minutes
//   kalshi-desk research wx-settle KXHIGHNY,KXHIGHCHI               WX-1: NWS station max vs settled bracket
#include <string>
#include <vector>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/store.h"

namespace desk {

int run_research(const std::vector<std::string>& args, const Config& cfg, Store& store, KalshiClient& client);

}  // namespace desk
