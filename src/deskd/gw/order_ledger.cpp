#include "deskd/gw/order_ledger.h"
#include <algorithm>
#include <limits>

namespace deskd {
namespace {
constexpr Qty kMaxQty = 1000000000; // arithmetic bound, not an admission/risk limit
constexpr kalshi::Micros kMaxReserve = 1000000000000000LL;
const char* state_name(OrdState s) {
  switch (s) {
    case OrdState::PEND: return "PEND"; case OrdState::WORK: return "WORK";
    case OrdState::PART: return "PART"; case OrdState::FILL: return "FILL";
    case OrdState::CXL: return "CXL"; case OrdState::REJ: return "REJ";
    case OrdState::EXP: return "EXP";
  }
  return "PEND";
}
bool same_fill(const kalshi::FillRow& a, const kalshi::FillRow& b) {
  // REST and WS use different fill_id and timestamp precision for the same trade_id.
  return a.trade_id == b.trade_id && a.order_id == b.order_id && a.ticker == b.ticker &&
      a.side == b.side && a.count == b.count && a.yes_price == b.yes_price &&
      a.fee == b.fee && a.taker == b.taker && a.exchange_index == b.exchange_index &&
      a.subaccount == b.subaccount;
}
// What makes a venue order row contradict the order gw placed; "" when it is consistent.
std::string order_mismatch(const OrderLedger::Order& o, const kalshi::OrderRow& row) {
  auto n = [](Qty q) { return std::to_string(q); };
  if (!row.client_order_id.empty() && row.client_order_id != o.intent.cid) return "names client order id " + row.client_order_id;
  if (row.ticker != o.intent.ticker) return "is on " + row.ticker;
  if (row.side != (o.intent.side == Side::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK)) return "is on the other side";
  if (row.exchange_index && *row.exchange_index != o.shard) return "is on shard " + std::to_string(*row.exchange_index);
  if (row.initial != o.intent.count) return "has size " + n(row.initial) + ", sent " + n(o.intent.count) + " (1/100 contract)";
  if (row.yes_price != o.intent.price) return "has price " + price_str(row.yes_price) + ", sent " + price_str(o.intent.price);
  if (row.filled < 0 || row.remaining < 0 || row.filled > row.initial || row.remaining > row.initial - row.filled)
    return "has filled " + n(row.filled) + " remaining " + n(row.remaining) + " of " + n(row.initial);
  if (row.status == kalshi::OrderStatus::EXECUTED && row.filled != row.initial)
    return "is executed with " + n(row.filled) + " of " + n(row.initial) + " filled";
  return "";
}
}
bool OrderLedger::Order::terminal() const {
  return state == OrdState::FILL || state == OrdState::CXL || state == OrdState::EXP || state == OrdState::REJ;
}
kalshi::Micros OrderLedger::Order::reservation() const {
  const Qty held = (terminal() ? 0 : left) + std::max<Qty>(0, filled - executions);
  // ceil(reserve * held / original qty), without overflowing the multiplication.
  const auto n = intent.count;
  return original_reserve / n * held + ((original_reserve % n) * held + n - 1) / n;
}
nlohmann::json OrderLedger::Order::json() const {
  using J = nlohmann::json;
  J avg = nullptr;
  if (executions) avg = price_str(execution_cost / executions); // micro-$ / centi-contract = Px
  return {{"cid",intent.cid},{"oid",oid.empty() ? J(nullptr) : J(oid)},
    {"v",venue_code(intent.venue)},{"tk",intent.ticker},
    {"side",intent.side == Side::BUY ? "BUY" : "SELL"},{"px",price_str(intent.price)},
    {"qty",qty_to_number(intent.count)},{"filled",qty_to_number(filled)},
    {"left",qty_to_number(left)},{"avg",avg},
    {"tif",intent.tif == Tif::POST ? "POST" : intent.tif == Tif::FOK ? "FOK" : "IOC"},
    {"exp_ms",intent.exp_ms ? J(intent.exp_ms) : J(nullptr)},{"st",state_name(state)},
    {"why",why},{"own",intent.owner == Owner::MAN ? "MAN" : intent.owner == Owner::SYS ? "SYS" : "EXT"},
    {"lane",intent.lane.empty() ? J(nullptr) : J(intent.lane)},{"piq",nullptr},
    {"fee",money_str(fees / 100)},{"touch",nullptr},{"edge",nullptr},
    {"t",{{"click",intent.t_click_ms ? J(intent.t_click_ms) : J(nullptr)},
          {"recv",intent.t_recv_ms},{"sent",sent_ms ? J(sent_ms) : J(nullptr)},
          {"ack",ack_ms ? J(ack_ms) : J(nullptr)},{"done",done_ms ? J(done_ms) : J(nullptr)}}},{"rev",rev}};
}
bool OrderLedger::invalid(const Order& o, const std::string& why) {
  if (healthy_) why_unhealthy_ = o.intent.cid + ": " + why;
  healthy_ = false; return false;
}
const OrderLedger::Order* OrderLedger::find(const std::string& cid) const {
  const auto it = orders_.find(cid); return it == orders_.end() ? nullptr : &it->second;
}
bool OrderLedger::insert(PlaceIntent intent, int shard, kalshi::Micros reserve) {
  if (!healthy_ || orders_.count(intent.cid)) return false;
  if (intent.cid.empty() || intent.ticker.empty() || intent.venue != Venue::K ||
      intent.count <= 0 || intent.count > kMaxQty || intent.price <= 0 || intent.price >= kOneDollar ||
      shard < 0 || reserve < 0 || reserve > kMaxReserve || intent.t_recv_ms <= 0 ||
      (intent.tif == Tif::POST && intent.exp_ms <= intent.t_recv_ms)) return false;
  Order o; o.left = intent.count; o.intent = std::move(intent); o.shard = shard; o.original_reserve = reserve;
  const auto cid = o.intent.cid; orders_.emplace(cid,std::move(o)); return true;
}
bool OrderLedger::bind(Order& o, const std::string& oid) {
  if (oid.empty()) return invalid(o,"the venue named no order id");
  if (!o.oid.empty() && o.oid != oid) return invalid(o,"order id changed from "+o.oid+" to "+oid);
  const auto it = by_oid_.find(oid);
  if (it != by_oid_.end() && it->second != o.intent.cid) return invalid(o,"order id "+oid+" already belongs to "+it->second);
  o.oid = oid; by_oid_[oid] = o.intent.cid; return true;
}
void OrderLedger::missing(Order& o, std::int64_t at) {
  if (o.filled > o.executions) missing_since_.emplace(o.intent.cid,at);
  else missing_since_.erase(o.intent.cid);
}
bool OrderLedger::sent(const std::string& cid, std::int64_t at) {
  auto it = orders_.find(cid); if (it == orders_.end() || at <= 0 || it->second.sent_ms) return false;
  it->second.sent_ms = at; ++it->second.rev; return true;
}
bool OrderLedger::unknown(const std::string& cid) {
  auto it = orders_.find(cid); if (it == orders_.end()) return false;
  auto& o = it->second;
  // A late HTTP timeout must not undo the private feed's positive evidence.
  if (!o.oid.empty() || o.terminal()) return true;
  if (!o.uncertain) { o.uncertain = true; o.state = OrdState::PEND; o.why.clear(); ++o.rev; }
  return true;
}
bool OrderLedger::rejected(const std::string& cid, const std::string& code, std::int64_t at) {
  auto it = orders_.find(cid); if (it == orders_.end() || code.empty()) return false;
  auto& o = it->second;
  // Contradictory venue evidence requires reconciliation, never freeing this reservation.
  if (!o.oid.empty() || o.filled || o.executions || o.uncertain)
    return invalid(o,"refused "+code+" after the venue showed it (order id, fills or an unknown send)");
  const bool cancel = code == "POST_CROSS" || code == "USER" || code == "STP";
  if (o.terminal()) return o.refused && o.why == code;
  o.refused = true; o.state = cancel ? OrdState::CXL : OrdState::REJ; o.why = code; o.left = 0; o.done_ms = at; ++o.rev; return true;
}
bool OrderLedger::acknowledged(const std::string& cid, const kalshi::CreateAck& ack, std::int64_t at) {
  auto it = orders_.find(cid); if (it == orders_.end()) return false;
  auto& o = it->second;
  if (o.refused) return invalid(o,"acknowledged after it was refused "+o.why);
  if (!ack.client_order_id.empty() && ack.client_order_id != cid)
    return invalid(o,"acknowledged under client order id "+ack.client_order_id);
  if (ack.fill_count < 0 || ack.remaining < 0 || ack.fill_count > o.intent.count ||
      ack.remaining > o.intent.count - ack.fill_count)
    return invalid(o,"acknowledged filled "+std::to_string(ack.fill_count)+" remaining "+
                   std::to_string(ack.remaining)+" of "+std::to_string(o.intent.count)+" (1/100 contract)");
  if (!bind(o,ack.order_id)) return false;
  o.uncertain = o.cancel_unverified; if (!o.ack_ms) o.ack_ms = at;
  o.filled = std::max(o.filled,ack.fill_count);
  if (!o.terminal() && !o.cancel_unverified) {
    o.left = std::min({o.left,ack.remaining,o.intent.count - o.filled});
    if (o.intent.tif != Tif::POST) o.left = 0;
    if (!o.left) {
      o.state = o.filled == o.intent.count ? OrdState::FILL : OrdState::CXL;
      o.why = o.state == OrdState::FILL ? "" : o.intent.tif == Tif::FOK ? "FOK" :
          o.intent.tif == Tif::IOC ? "IOC_REST" : "STP";
      o.done_ms = at;
    } else { o.state = o.filled ? OrdState::PART : OrdState::WORK; o.why.clear(); }
  }
  if (o.filled == o.intent.count) {
    o.state=OrdState::FILL; o.uncertain=false; o.cancel_unverified=false;
    o.why.clear(); o.left=0; if(!o.done_ms) o.done_ms=at;
  }
  o.left=std::min(o.left,o.intent.count-o.filled);
  missing(o,at); ++o.rev; return true;
}
bool OrderLedger::observed(const kalshi::OrderRow& row, std::int64_t at) {
  auto it = orders_.find(row.client_order_id);
  if (it == orders_.end()) {
    const auto id = by_oid_.find(row.order_id);
    if (id == by_oid_.end()) return false;
    it = orders_.find(id->second);
  }
  auto& o = it->second;
  if (o.refused) return invalid(o,"the venue shows order "+row.order_id+" after it was refused "+o.why);
  if (const auto bad = order_mismatch(o,row); !bad.empty())
    return invalid(o,"order "+row.order_id+" "+bad);
  if (!bind(o,row.order_id)) return false;
  if(row.status != kalshi::OrderStatus::RESTING) o.cancel_unverified=false;
  o.uncertain = o.cancel_unverified; if (!o.ack_ms) o.ack_ms = at;
  o.filled = std::max(o.filled,row.filled);
  const auto updated = row.updated_ms.value_or(at);
  // Private and REST delivery can be reordered. Cumulative fills may rise, but a terminal
  // order can never be resurrected, or its live remainder increased by an older snapshot.
  if (!o.terminal() && updated >= o.update_ms) {
    o.update_ms = updated;
    o.left = row.status == kalshi::OrderStatus::RESTING ? std::min({o.left,row.remaining,o.intent.count-o.filled}) : 0;
    if (o.filled == o.intent.count) o.state = OrdState::FILL;
    else if (row.status == kalshi::OrderStatus::RESTING) o.state = o.cancel_unverified ? OrdState::PEND : o.filled ? OrdState::PART : OrdState::WORK;
    else o.state = o.intent.exp_ms && updated >= o.intent.exp_ms ? OrdState::EXP : OrdState::CXL;
    o.why = o.state == OrdState::EXP ? "EXPIRY" : o.state == OrdState::CXL ?
        (o.cancel_requested ? "USER" : o.intent.tif == Tif::IOC ? "IOC_REST" : o.intent.tif == Tif::FOK ? "FOK" : "") : "";
    if (o.terminal()) o.done_ms = at;
  }
  if (o.filled == o.intent.count) {
    o.state=OrdState::FILL; o.uncertain=false; o.cancel_unverified=false;
    o.why.clear(); o.left=0; if(!o.done_ms) o.done_ms=at;
  }
  o.left = std::min(o.left,o.intent.count-o.filled);
  missing(o,at); ++o.rev; return true;
}
OrderLedger::FillResult OrderLedger::fill(const kalshi::FillRow& row, const std::string& cid, std::int64_t at) {
  auto bad = [&](const Order* o, const std::string& why) {
    if (healthy_) why_unhealthy_ = (o ? o->intent.cid + ": " : std::string()) + "fill " + row.trade_id + " " + why;
    healthy_ = false; return FillResult::INVALID;
  };
  if (row.trade_id.empty() || row.order_id.empty() || row.count <= 0 || row.count > kMaxQty ||
      row.yes_price < 0 || row.yes_price > kOneDollar || row.fee < -kMaxReserve || row.fee > kMaxReserve)
    return bad(nullptr,"is malformed");
  const auto dup = fills_.find(row.trade_id);
  if (dup != fills_.end())
    return same_fill(dup->second,row) ? FillResult::DUPLICATE : bad(nullptr,"arrived twice with different fields");
  auto it = orders_.find(cid);
  if (it == orders_.end()) {
    const auto id = by_oid_.find(row.order_id);
    if (id == by_oid_.end()) return FillResult::UNMAPPED;
    it = orders_.find(id->second);
  }
  auto& o = it->second;
  if (!cid.empty() && cid != o.intent.cid) return bad(&o,"names client order id "+cid);
  if (row.ticker != o.intent.ticker) return bad(&o,"is on "+row.ticker);
  if (row.exchange_index != o.shard) return bad(&o,"is on shard "+std::to_string(row.exchange_index));
  if (row.side != (o.intent.side == Side::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK))
    return bad(&o,"is on the other side");
  if (row.count > o.intent.count - o.executions)
    return bad(&o,"overfills: "+std::to_string(row.count)+" after "+std::to_string(o.executions)+" of "+
               std::to_string(o.intent.count)+" (1/100 contract)");
  if (o.refused) return bad(&o,"for an order refused "+o.why);
  if ((row.fee > 0 && o.fees > std::numeric_limits<kalshi::Micros>::max()-row.fee) ||
      (row.fee < 0 && o.fees < std::numeric_limits<kalshi::Micros>::min()-row.fee)) return bad(&o,"overflows the fee total");
  if (!bind(o,row.order_id)) return FillResult::INVALID;
  fills_.emplace(row.trade_id,row); o.executions += row.count; o.fees += row.fee;
  o.execution_cost += row.yes_price * row.count;
  o.filled = std::max(o.filled,o.executions); o.uncertain=o.cancel_unverified; if(!o.ack_ms) o.ack_ms=at;
  o.left = o.terminal() ? 0 : std::min(o.left,o.intent.count-o.filled);
  if (o.filled == o.intent.count) { o.state=OrdState::FILL; o.uncertain=false; o.cancel_unverified=false; o.why.clear(); o.left=0; if(!o.done_ms) o.done_ms=at; }
  else if (!o.terminal()) { o.state=o.cancel_unverified ? OrdState::PEND : OrdState::PART; o.why.clear(); }
  missing(o,at); ++o.rev; return FillResult::APPLIED;
}
bool OrderLedger::request_cancel(const std::string& cid) {
  auto it=orders_.find(cid); if(it==orders_.end() || it->second.terminal()) return false;
  if(!it->second.cancel_requested) { it->second.cancel_requested=true; ++it->second.rev; }
  return true;
}
bool OrderLedger::cancelled(const std::string& cid, const kalshi::CancelAck& ack,
                             const std::string& reason, std::int64_t at) {
  auto it=orders_.find(cid); if(it==orders_.end()) return false;
  auto& o=it->second;
  if(o.refused) return invalid(o,"cancel acknowledged after it was refused "+o.why);
  if (!ack.client_order_id.empty() && ack.client_order_id!=cid)
    return invalid(o,"cancel acknowledged under client order id "+ack.client_order_id);
  if (ack.reduced_by<0 || ack.reduced_by>o.intent.count)
    return invalid(o,"cancel reduced it by "+std::to_string(ack.reduced_by)+" of "+std::to_string(o.intent.count)+" (1/100 contract)");
  if(!bind(o,ack.order_id)) return false;
  // reduced_by=0 alone cannot distinguish an already-cancelled order from a fully filled
  // order whose executions are still in flight. Keep every dollar until an order read/event.
  if(!o.terminal() && ack.reduced_by==0) {
    o.cancel_requested=true; o.cancel_unverified=true; o.uncertain=true;
    o.state=OrdState::PEND; o.why.clear(); ++o.rev;
    return true;
  }
  if(!o.terminal() && ack.reduced_by>0) o.filled=std::max(o.filled,o.intent.count-ack.reduced_by);
  o.cancel_unverified=false; o.uncertain=false; o.left=0; if(!o.ack_ms) o.ack_ms=at;
  if(o.filled==o.intent.count) { o.state=OrdState::FILL; o.why.clear(); }
  else if(!o.terminal()) {
    o.state=o.intent.exp_ms && at>=o.intent.exp_ms ? OrdState::EXP : OrdState::CXL;
    o.why=o.state==OrdState::EXP ? "EXPIRY" : reason;
  }
  if(!o.done_ms) o.done_ms=at;
  missing(o,at); ++o.rev; return true;
}
bool OrderLedger::resolve_absent(const std::string& cid, const History& h) {
  auto it=orders_.find(cid); if(it==orders_.end()) return false;
  auto& o=it->second;
  if(!o.uncertain || o.cancel_unverified || !o.sent_ms || o.filled!=o.executions || !h.orders_complete || !h.fills_complete ||
     h.shard!=o.shard || h.ticker!=o.intent.ticker || h.since_ms>o.sent_ms-5000 ||
     h.began_ms<(o.intent.exp_ms ? o.intent.exp_ms+5000 : o.sent_ms+120000) || h.completed_ms<h.began_ms) return false;
  for(const auto& row:h.orders) if(row.client_order_id==cid || (!o.oid.empty() && row.order_id==o.oid)) return false;
  for(const auto& row:h.fills) if(!o.oid.empty() && row.order_id==o.oid) return false;
  o.uncertain=false; o.left=0; o.done_ms=h.completed_ms;
  o.state=o.intent.exp_ms ? OrdState::EXP : OrdState::CXL;
  o.why=o.intent.exp_ms ? "EXPIRY" : o.intent.tif==Tif::FOK ? "FOK" : "IOC_REST";
  ++o.rev; return true;
}
bool OrderLedger::blocks(const std::string& ticker,int shard) const {
  for(const auto& pair:orders_) if(pair.second.intent.ticker==ticker && pair.second.shard==shard && pair.second.uncertain) return true;
  return false;
}
kalshi::Micros OrderLedger::reserved(int shard) const {
  kalshi::Micros result=0;
  for(const auto& pair:orders_) if(pair.second.shard==shard) {
    const auto r=pair.second.reservation();
    if(result>std::numeric_limits<kalshi::Micros>::max()-r) return std::numeric_limits<kalshi::Micros>::max();
    result+=r;
  }
  return result;
}
std::vector<std::string> OrderLedger::fills_due(std::int64_t now) const {
  std::vector<std::string> out;
  for(const auto& pair:missing_since_) if(now-pair.second>=5000) out.push_back(pair.first);
  return out;
}
}
