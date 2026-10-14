#include "deskd/gw/manual_admission.h"
#include "deskd/rest/state.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>

namespace deskd {
namespace {
constexpr Qty kMaxQty = 1000000000; // same arithmetic bound as OrderLedger, not a budget
// Manual cids start with M, Systematic's with S (contract: ownership survives a crash).
bool cid_ok(const std::string& cid, Owner owner) {
  const char own=owner==Owner::MAN ? 'M' : owner==Owner::SYS ? 'S' : 0;
  if(!own || cid.size()<6 || cid.size()>40 || cid.front()!=own) return false;
  return std::all_of(cid.begin(),cid.end(),[](unsigned char ch) {
    return std::isalnum(ch) || ch=='-' || ch=='_';
  });
}
bool grid_valid(const std::vector<kalshi::PriceRange>& grid) {
  if(grid.empty() || grid.size()>100) return false;
  Px previous=-1;
  for(const auto& r:grid) {
    if(r.start<0 || r.end>kOneDollar || r.start>=r.end || r.step<=0 ||
       r.step>kOneDollar || r.start<previous) return false;
    previous=r.end;
  }
  return true;
}
bool on_grid(Px p,const std::vector<kalshi::PriceRange>& grid) {
  for(const auto& r:grid)
    if(p>=r.start && p<=r.end && (p-r.start)%r.step==0) return true;
  return false;
}
// Count valid prices through the touch, stopping after three. This handles range boundaries
// without assuming that two ticks = twice the tick at the touch. The price domain is only 1e4.
bool through_two(Px p,Px touch,Side side,const std::vector<kalshi::PriceRange>& grid) {
  int ticks=0;
  if(side==Side::BUY) {
    for(Px v=touch+1;v<=p;++v) if(on_grid(v,grid) && ++ticks>2) return true;
  } else {
    for(Px v=touch-1;v>=p;--v) if(on_grid(v,grid) && ++ticks>2) return true;
  }
  return false;
}
}
ManualAdmission admit_manual(const PlaceIntent& input,const ManualContext& c) {
  ManualAdmission a; a.intent=input;
  auto refuse=[&](const char* code,const std::string& text) { a.code=code; a.message=text; return a; };
  if(c.now_ms<=0 || c.now_ms>std::numeric_limits<std::int64_t>::max()-1800000 ||
     !cid_ok(input.cid,input.owner) || (input.owner==Owner::MAN)!=input.lane.empty() ||
     (input.side!=Side::BUY && input.side!=Side::SELL) ||
     (input.tif!=Tif::IOC && input.tif!=Tif::FOK && input.tif!=Tif::POST))
    return refuse("BAD_REQUEST","invalid intent");
  if(c.duplicate_cid) return refuse("DUP_CID","client order id already registered");
  if(input.venue!=Venue::K || !c.market || c.market->ticker!=input.ticker)
    return refuse("UNKNOWN_MARKET","no Kalshi market for this intent");
  const auto& m=*c.market;
  if(input.count<=0 || input.count>kMaxQty) return refuse("BAD_QTY","quantity outside supported range");
  // Qty is in 1/100 contract. A market without fractional_trading_enabled takes whole contracts.
  const bool whole_only=!m.fractional.value_or(false);
  if(whole_only && input.count%100) return refuse("BAD_QTY","this market trades whole contracts only");
  if(input.price<=0 || input.price>=kOneDollar) return refuse("OFF_GRID","price must be inside (0,1)");
  if((input.tif!=Tif::POST && (input.ttl_s || input.exp_ms)) ||
     (input.reduce_only && input.tif!=Tif::IOC))
    return refuse("BAD_REQUEST","expiry requires POST; reduce_only requires IOC");
  if(c.killed) return refuse("KILLED","KILL lockout active");
  if(c.read_only || !c.publisher_healthy || !c.ledger_healthy ||
     !c.manual_sender_healthy || !c.cancel_sender_healthy)
    return refuse("RO",c.owner_why.empty() ? "an execution owner is not ready" : c.owner_why);
  // STALE names which input is not current (one message for all six hid it: a soak under TSan
  // refused a quote a minute and could not say why)
  if(!c.account_ready) return refuse("STALE","the account read (REST) is not current yet");
  if(!c.public_ready) return refuse("STALE","market data is not ready");
  if(!c.private_ready) return refuse("STALE","the private order/fill feed is not up");
  if(c.unknown_order)
    return refuse("STALE","order "+(c.unknown_cid.empty() ? std::string("?") : c.unknown_cid)+
                  " on this market has an unknown outcome; an account read settles it (an IOC within about 2 minutes)");
  if(!c.book || c.book->ticker!=input.ticker) return refuse("STALE","no book for this market yet");
  if(c.book->stale) return refuse("STALE","this market's book is stale");
  if(c.foreign_order) return refuse("RO","cancel the foreign order on this market first");
  if(!m.exchange_index || *m.exchange_index<0)
    return refuse("RO","market has no authoritative exchange index");
  if((m.status!="active" && m.status!="open") || !m.close_ms || *m.close_ms<=c.now_ms ||
     (m.open_ms && *m.open_ms>c.now_ms) || !c.trading_active || !c.shard_active)
    return refuse("MARKET_CLOSED","market or its exchange shard is not trading");
  if(!rest::fee_ready(m,c.now_ms)) return refuse("RO","fee metadata is missing, stale or unsupported");
  if(!grid_valid(m.price_ranges)) return refuse("RO","market price grid is missing or invalid");
  if(!on_grid(input.price,m.price_ranges)) return refuse("OFF_GRID","price is outside the market grid");
  const auto& bids=c.book->bids; const auto& asks=c.book->asks;
  auto valid_touch=[&](const std::vector<MdLevel>& levels) {
    return levels.empty() || (levels.front().px>0 && levels.front().px<kOneDollar &&
        levels.front().qty>0 && on_grid(levels.front().px,m.price_ranges));
  };
  if(!valid_touch(bids) || !valid_touch(asks) ||
     (!bids.empty() && !asks.empty() && bids.front().px>=asks.front().px))
    return refuse("STALE","book touch is invalid or crossed");
  const auto& contra=input.side==Side::BUY ? asks : bids;
  if(!input.override_guard && !contra.empty() &&
     through_two(input.price,contra.front().px,input.side,m.price_ranges))
    return refuse("PRICE_BAND","limit is more than two market ticks through the touch");
  // POST crossing is deliberately sent post_only: the venue's soft refusal is the authority,
  // since the book can change between validation and arrival. Never silently convert to IOC.
  if(input.tif==Tif::POST) {
    if(!c.clock_trusted) return refuse("STALE","clock is not trusted for an exchange-side expiry");
    const bool relative=input.ttl_s!=0, absolute=input.exp_ms!=0;
    if(relative==absolute) return refuse("BAD_REQUEST","POST needs exactly one expiry");
    if(relative) {
      // Systematic orders live at most 60 s (plan section 3), Manual's on the ticket's choices
      if(input.owner==Owner::SYS ? input.ttl_s<5 || input.ttl_s>60 :
         input.ttl_s!=5 && input.ttl_s!=15 && input.ttl_s!=60 && input.ttl_s!=300 && input.ttl_s!=1800)
        return refuse("BAD_REQUEST","unsupported TTL");
      a.intent.exp_ms=(c.now_ms/1000+input.ttl_s)*1000;
    } else if(input.exp_ms%1000 || input.exp_ms<c.now_ms+5000) {
      return refuse("BAD_REQUEST","absolute expiry must be whole seconds and at least five seconds ahead");
    }
    if(a.intent.exp_ms>*m.close_ms) return refuse("BAD_REQUEST","expiry is later than the market close");
    if(input.owner==Owner::SYS && a.intent.exp_ms>c.now_ms+61000)
      return refuse("BAD_REQUEST","a Systematic order lives at most 60 s");
    a.intent.ttl_s=0;
  }
  if(c.position < -kMaxQty || c.position>kMaxQty || c.reducing_committed<0 ||
     c.reducing_committed>kMaxQty)
    return refuse("RO","invalid position or outstanding reducing quantity");
  const Qty reducing=input.side==Side::BUY ? std::max<Qty>(0,-c.position) : std::max<Qty>(0,c.position);
  const Qty room=std::max<Qty>(0,reducing-c.reducing_committed);
  if(input.reduce_only) {
    // A fractional position on a whole-contract market can only be reduced in whole contracts.
    const Qty clip=whole_only ? room/100*100 : room;
    if(!clip) return refuse("REDUCE","no uncommitted position on the reducing side");
    a.intent.count=std::min(input.count,clip);
  }
  // Conservative incremental collateral: pending orders already own their reducing room.
  // Do not release their reservations speculatively because a new order has a better price.
  a.collateral=(input.side==Side::BUY ? input.price : kOneDollar-input.price)*
      std::max<Qty>(0,a.intent.count-room); // Px * centi-contract = micro-dollars
  // Taker fee check even for POST (contract 1.5). A better execution towards 0.50 can cost
  // more fees than the limit. Use the maximum quadratic fee over every executable price.
  // Round each smallest possible fill upward to micro-dollars, then allow a cent for the
  // order rounding accumulator. This is an admission allowance, not a promised charged fee.
  // Official formula/rounding: docs.kalshi.com/getting_started/fee_rounding and Kalshi fee schedule.
  const Px fee_px=input.side==Side::BUY ? std::min<Px>(input.price,5000) : std::max<Px>(input.price,5000);
  const long double per_centi=7.L*static_cast<long double>(*m.fee_multiplier)*
      fee_px*(kOneDollar-fee_px)/1000000.L;
  a.fee_check=static_cast<kalshi::Micros>(std::ceil(per_centi))*a.intent.count+10000;
  if(c.free<a.required()) return refuse("NO_CASH","not enough available venue cash including taker fee allowance");
  if(c.shard_free<a.required()) return refuse("SHARD_CASH","not enough available cash on this market's exchange shard");
  a.intent.t_recv_ms=input.t_recv_ms ? input.t_recv_ms : c.now_ms;
  a.wire=kalshi::from_intent(a.intent,*m.exchange_index,c.now_ms);
  if(!kalshi::check_create(a.wire).empty()) return refuse("BAD_REQUEST","intent cannot be represented by the venue");
  return a;
}
} // namespace deskd
