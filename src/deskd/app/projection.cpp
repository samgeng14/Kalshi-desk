#include "deskd/app/projection.h"
#include <algorithm>
#include <cstdlib>
#include <openssl/sha.h>
#include "deskd/bus/units.h"
#include "deskd/rest/state.h"

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
json rest_cash_json(const rest::Snapshot& state, bool read_only) {
  using Key=std::pair<int,std::string>;
  std::map<Key,Qty> yes_room,no_room;
  for(const auto& p:state.positions) {
    if(p.position>0) yes_room[{p.exchange_index,p.ticker}]=p.position;
    if(p.position<0) no_room[{p.exchange_index,p.ticker}]=-p.position;
  }
  std::vector<const kalshi::OrderRow*> working;
  for(const auto& o:state.orders) if(o.status==kalshi::OrderStatus::RESTING && o.remaining>0) working.push_back(&o);
  std::sort(working.begin(),working.end(),[](const auto* a,const auto* b) {
    if(a->ticker!=b->ticker) return a->ticker<b->ticker;
    if(a->side!=b->side) return a->side<b->side;
    if(a->yes_price!=b->yes_price)
      return a->side==kalshi::BookSide::BID ? a->yes_price>b->yes_price : a->yes_price<b->yes_price;
    return a->order_id<b->order_id;
  });
  constexpr Money limit=9999999999999LL; // contract MONEY's nine whole dollar digits
  auto add=[&](Money a,Money b) {
    if(a<0 || b<0 || a>limit || b>limit-a) { read_only=true; return limit; }
    return a+b;
  };
  std::map<int,Money> held;
  Money unassigned=0;
  for(const auto* o:working) {
    auto shard=o->exchange_index;
    const auto m=state.markets.find(o->ticker);
    if(!shard && m!=state.markets.end()) shard=m->second.exchange_index;
    if(shard && m!=state.markets.end() && m->second.exchange_index && shard!=m->second.exchange_index) read_only=true;
    Qty opening=o->remaining;
    if(shard) {
      auto& room=(o->side==kalshi::BookSide::BID ? no_room : yes_room)[{*shard,o->ticker}];
      const Qty reducing=std::min(room,opening); room-=reducing; opening-=reducing;
    } else read_only=true;
    const auto px=o->side==kalshi::BookSide::BID ? o->yes_price : kOneDollar-o->yes_price;
    Money cost=0;
    if(px<0 || px>kOneDollar) { read_only=true; cost=limit; }
    else if(px && opening>limit*100/px) { read_only=true; cost=limit; }
    else cost=(px*opening+99)/100;
    if(shard) held[*shard]=add(held[*shard],cost); else unassigned=add(unassigned,cost);
  }
  json shards=json::array(); Money free=0,total=unassigned;
  for(const auto& item:state.balances) {
    const auto available=std::clamp<Money>(item.second,0,limit);
    if(available!=item.second) read_only=true;
    const auto cash=add(available,held[item.first]);
    free=add(free,available); total=add(total,cash);
    shards.push_back({{"id","S"+std::to_string(item.first)},{"cash",money_str(cash)},
                      {"free",money_str(available)}});
  }
  for(const auto& item:held) if(!state.balances.count(item.first)) read_only=true;
  if(state.balances.empty()) { read_only=true; total=std::clamp<Money>(state.balance.available,0,limit); }
  return {{"total",money_str(total)},{"free",money_str(free)},{"ro",read_only},{"shards",shards}};
}
}
