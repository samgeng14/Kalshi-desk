#include "deskd/app/projection.h"
#include <algorithm>
#include <cstdlib>
#include <openssl/sha.h>
#include "deskd/bus/units.h"

namespace deskd {
using json = nlohmann::json;
std::string screen_cid(const kalshi::OrderRow& row) {
  const auto& s = row.client_order_id;
  if (s.size() >= 6 && s.size() <= 40 &&
      s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos)
    return s;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(row.order_id.data()), row.order_id.size(), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string id = "X-";
  for (int i = 0; i < 16; ++i) { id += hex[digest[i] >> 4]; id += hex[digest[i] & 15]; }
  return id;
}
json rest_order_json(const kalshi::OrderRow& row, std::int64_t now) {
  const bool resting = row.status == kalshi::OrderStatus::RESTING;
  const bool expired = !resting && row.expiration_ms && *row.expiration_ms <= now &&
                       row.status == kalshi::OrderStatus::CANCELED;
  const std::string state = resting ? (row.filled ? "PART" : "WORK") :
      row.status == kalshi::OrderStatus::EXECUTED ? "FILL" : expired ? "EXP" : "CXL";
  const std::string cid = screen_cid(row);
  const std::string owner = !row.client_order_id.empty() && row.client_order_id[0] == 'M' ? "MAN" :
                           !row.client_order_id.empty() && row.client_order_id[0] == 'S' ? "SYS" : "EXT";
  const auto ts = row.created_ms.value_or(now);
  json avg = nullptr;
  if (row.filled) {
    const auto cost = row.taker_cost + row.maker_cost;
    const Px p = cost / row.filled; // micro-dollars / centi-contracts = price units
    if (p >= 0 && p <= kOneDollar) avg = price_str(row.side == kalshi::BookSide::BID ? p : kOneDollar - p);
  }
  return {{"cid",cid},{"oid",row.order_id},{"v","K"},{"tk",row.ticker},
      {"side",row.side == kalshi::BookSide::BID ? "BUY" : "SELL"},{"px",price_str(row.yes_price)},
      {"qty",qty_to_number(row.initial)},{"filled",qty_to_number(row.filled)},
      {"left",qty_to_number(resting ? row.remaining : 0)},{"avg",avg},
      {"tif",resting || row.expiration_ms ? "POST" : "IOC"},
      {"exp_ms",row.expiration_ms ? json(*row.expiration_ms) : json(nullptr)},
      {"st",state},{"why",expired ? "EXPIRY" : ""},{"own",owner},{"lane",nullptr},
      {"piq",nullptr},{"fee",money_str((row.taker_fees + row.maker_fees) / 100)},
      {"touch",nullptr},{"edge",nullptr},
      {"t",{{"click",nullptr},{"recv",ts},{"sent",nullptr},{"ack",row.updated_ms.value_or(ts)},
             {"done",resting ? json(nullptr) : json(row.updated_ms.value_or(now))}}},{"rev",1}};
}
json rest_position_json(const kalshi::PositionRow& row) {
  const Qty n = std::abs(row.position);
  const Money cost = std::max<Money>(0, (row.exposure + 99) / 100);
  const Money payout = n * 100;
  json avg = nullptr;
  if (n) {
    const Px p = std::clamp<Money>(row.exposure / n, 0, kOneDollar);
    avg = price_str(row.position > 0 ? p : kOneDollar - p);
  }
  // REST's realized/fees are lifetime aggregates. They cannot be labelled today's P&L.
  // The execution ledger supplies daily P&L in the gateway; this startup view is cost only.
  return {{"v","K"},{"tk",row.ticker},{"pos",qty_to_number(row.position)},
      {"avg",avg},{"own",n ? json("MAN") : json(nullptr)},{"mark",nullptr},
      {"unrl","0.0000"},{"real","0.0000"},{"fees","0.0000"},{"maxl",money_str(cost)},
      {"if_yes",money_str((row.position > 0 ? payout : 0) - cost)},
      {"if_no",money_str((row.position < 0 ? payout : 0) - cost)}};
}
}
