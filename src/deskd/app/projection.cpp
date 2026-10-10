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
json rest_position_json(const kalshi::PositionRow& row, std::optional<Px> mark, Money real, Money fees) {
  const Qty n = std::abs(row.position);
  const Money cost = std::max<Money>(0, (row.exposure + 99) / 100);
  const Money payout = n * 100;
  json avg = nullptr;
  if (n) {
    const Px p = std::clamp<Money>(row.exposure / n, 0, kOneDollar);
    avg = price_str(row.position > 0 ? p : kOneDollar - p);
  }
  // REST's realized/fees are lifetime aggregates, never labelled today's: real and fees come
  // from today's fills (day_pnl). unrl is the open position marked at the live mid.
  Money unrl = 0;
  if (n && mark && *mark >= 0 && *mark <= kOneDollar) {
    const kalshi::Micros value = static_cast<kalshi::Micros>(row.position > 0 ? *mark : kOneDollar - *mark) * n;
    const kalshi::Micros diff = value - row.exposure;
    unrl = diff >= 0 ? (diff + 50) / 100 : -((-diff + 50) / 100);
  }
  return {{"v","K"},{"tk",row.ticker},{"pos",qty_to_number(row.position)},
      {"avg",avg},{"own",n ? json("MAN") : json(nullptr)},{"mark",n && mark ? json(price_str(*mark)) : json(nullptr)},
      {"unrl",money_str(unrl)},{"real",money_str(real)},{"fees",money_str(fees)},{"maxl",money_str(cost)},
      {"if_yes",money_str((row.position > 0 ? payout : 0) - cost)},
      {"if_no",money_str((row.position < 0 ? payout : 0) - cost)}};
}
namespace {
Money micros_to_money(kalshi::Micros m) { return m >= 0 ? (m + 50) / 100 : -((-m + 50) / 100); }
int sign(Qty q) { return (q > 0) - (q < 0); }
}  // namespace

DayPnl day_pnl(const rest::Snapshot& state, std::int64_t day_start_ms, const MidFn& mid) {
  DayPnl out;
  std::map<std::string, std::string> cid_of;   // order id -> client order id, where the venue gave one
  for (const auto& o : state.orders) if (!o.client_order_id.empty()) cid_of[o.order_id] = o.client_order_id;
  using Key = std::pair<std::string, int>;
  std::map<Key, std::vector<const kalshi::FillRow*>> by_market;
  for (const auto& f : state.fills)
    if (f.ts_ms >= day_start_ms && f.count > 0) by_market[{f.ticker, f.exchange_index}].push_back(&f);
  std::map<Key, const kalshi::PositionRow*> positions;
  for (const auto& p : state.positions) positions[{p.ticker, p.exchange_index}] = &p;
  for (auto& [key, fills] : by_market) {
    std::sort(fills.begin(), fills.end(), [](const auto* a, const auto* b) {
      return a->ts_ms != b->ts_ms ? a->ts_ms < b->ts_ms : a->trade_id < b->trade_id;
    });
    // Kalshi's position now, as signed YES contracts and signed YES-terms cost (micro-dollars).
    Qty p_now = 0;
    kalshi::Micros s_now = 0;
    if (const auto it = positions.find(key); it != positions.end()) {
      p_now = it->second->position;
      const Qty n = std::abs(p_now);
      s_now = p_now > 0 ? it->second->exposure : -static_cast<kalshi::Micros>(n) * kOneDollar + it->second->exposure;
    }
    auto signed_qty = [](const kalshi::FillRow* f) { return f->side == kalshi::BookSide::BID ? f->count : -f->count; };
    // Backwards: the day's starting cost, if today's fills leave it recoverable.
    Qty p = p_now;
    kalshi::Micros s = s_now;
    bool known = true;
    for (auto it = fills.rbegin(); it != fills.rend(); ++it) {
      const Qty q = signed_qty(*it), before = p - q;
      if (before == 0) { s = 0; known = true; }
      else if (sign(q) == sign(before)) s -= static_cast<kalshi::Micros>((*it)->yes_price) * q;
      else if (std::abs(q) <= std::abs(before) && p != 0)
        s = static_cast<kalshi::Micros>(std::llround(static_cast<long double>(s) * before / p));
      else known = false;   // went flat or flipped: the earlier cost is not in Kalshi's numbers
      p = before;
    }
    // Forwards: each fill's realized part, net of its fee.
    Money real_sum = 0, fee_sum = 0;
    for (const auto* f : fills) {
      const Qty q = signed_qty(f);
      const kalshi::Micros px = f->yes_price;
      kalshi::Micros realized = 0;
      bool unknown = false;
      if (p == 0 || sign(q) == sign(p)) {
        s += px * q;
        p += q;
      } else {
        const Qty r = sign(q) * std::min(std::abs(q), std::abs(p));   // the reducing part
        if (known) {
          const long double removed = static_cast<long double>(s) * r / p;   // cost leaving with r
          realized = static_cast<kalshi::Micros>(std::llround(-static_cast<long double>(r) * px + removed));
          s = static_cast<kalshi::Micros>(std::llround(static_cast<long double>(s) - removed));
        } else {
          unknown = true;
        }
        p += r;
        if (p == 0) { s = 0; known = true; }
        const Qty rest = q - r;   // a flip opens the remainder at this price
        if (rest) { s = px * rest; p = rest; known = true; }
      }
      if (unknown) ++out.unknown_basis;
      const Money fee = micros_to_money(f->fee);
      const Money real = micros_to_money(realized) - fee;
      real_sum += real;
      fee_sum += fee;
      kalshi::OrderRow tmp;
      tmp.order_id = f->order_id;
      if (const auto c = cid_of.find(f->order_id); c != cid_of.end()) tmp.client_order_id = c->second;
      const std::string cid = screen_cid(tmp);
      const std::string own = !tmp.client_order_id.empty() && cid == tmp.client_order_id && cid[0] == 'M' ? "MAN" :
                              !tmp.client_order_id.empty() && cid == tmp.client_order_id && cid[0] == 'S' ? "SYS" : "EXT";
      const auto m = mid ? mid(f->ticker) : std::nullopt;
      out.fills.push_back({{"fid", f->trade_id.empty() ? f->fill_id : f->trade_id}, {"cid", cid},
          {"oid", f->order_id.empty() ? json(nullptr) : json(f->order_id)}, {"v", "K"}, {"tk", f->ticker},
          {"side", f->side == kalshi::BookSide::BID ? "BUY" : "SELL"}, {"px", price_str(f->yes_price)},
          {"qty", qty_to_number(f->count)}, {"liq", f->taker ? "T" : "M"}, {"fee", money_str(fee)},
          {"ts_ms", f->ts_ms}, {"own", own}, {"lane", nullptr}, {"mid", m ? json(price_str(*m)) : json(nullptr)},
          {"fair", nullptr}, {"real", money_str(real)}});
    }
    out.real[key] = real_sum;
    out.fees[key] = fee_sum;
    out.real_total += real_sum;
  }
  std::sort(out.fills.begin(), out.fills.end(), [](const json& a, const json& b) { return a["ts_ms"] < b["ts_ms"]; });
  return out;
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
