#pragma once
// Kalshi WebSocket subscribe commands, with the empty-subscription guard (plan section 8,
// invariants: "an empty subscription list is refused").
//
// Kalshi reads a market-data subscribe without market_tickers as "every market on the
// exchange". The old desk's recorder did exactly that by accident: 77.6% of September's rows
// were for markets nobody asked for, and the book stream dominated the disk (3 GB a day after
// trimming). Account channels (fills, our orders, order groups, positions) are scoped to the
// key, not the exchange, and need no list.
#include <string>
#include <vector>

namespace deskd {

// true for a channel scoped to the account (fill, user_orders, order_group_updates,
// market_positions); false for market data (orderbook_delta, ticker, trade,
// market_lifecycle_v2) and for anything unknown
bool kalshi_account_channel(const std::string& channel);

// The subscribe command as JSON text, or "" with *err set when it is refused: no channels, an
// unknown channel, a market-data channel with an empty ticker list, a malformed or repeated
// ticker, or market data and account channels mixed in one command (each needs its own
// handling of the list).
std::string kalshi_subscribe(int id, const std::vector<std::string>& channels,
                             const std::vector<std::string>& tickers, std::string* err);

}  // namespace deskd
