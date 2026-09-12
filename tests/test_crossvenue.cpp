#include <cmath>
#include <iostream>
#include "desk/crossvenue.h"
#include "desk/polymarket.h"
#include "desk/timeutil.h"
using namespace desk;
int failures=0, checks=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": "<<#x<<"\n";}}while(0)
template<class F>bool throws(F&& f){try{f();return false;}catch(...){return true;}}
bool contains(const json& j,const std::string& s){return j.dump().find(s)!=std::string::npos;}
int main(){
 const long long now=parse_iso("2026-09-08T20:00:00Z");
 json s=crossvenue_defaults();s["kalshi_budget"]=50;s["polymarket_budget"]=50;
 CHECK(throws([&]{crossvenue_settings({{"kalshi_budget",101}},s);}));
 CHECK(throws([&]{crossvenue_settings({{"max_horizon_hours",49}},s);}));
 CHECK(throws([&]{crossvenue_settings({{"surprise",1}},s);}));
 CHECK(throws([&]{crossvenue_settings({{"enabled",1}},s);}));
 CHECK(throws([&]{crossvenue_settings({{"total_budget",50}},s);}));
 json k={{"ticker","KXTEST-26SEP08-A"},{"series_ticker","KXTEST"},{"title","Washington at San Diego winner"},
   {"rules","Same fixture outcome; full game including extra innings. Cancellation identical."},
   {"yes_label","Washington wins"},{"no_label","San Diego wins"},{"status","active"},{"quote_ts",now},
   {"close_ts",now+3600},{"settlement_ts",now+7200},{"yes_bid",.30},{"yes_ask",.35},{"no_bid",.65},{"no_ask",.70},
   {"yes_ask_size",10.0},{"no_ask_size",20.0},{"fee_type","quadratic"},{"fee_multiplier",.5}};
 json p={{"slug","baseball-test"},{"title","Washington at San Diego winner"},{"rules",k["rules"]},
   {"yes_label","Washington wins"},{"no_label","San Diego wins"},{"status","open"},{"quote_ts",now},{"source_ts",now},
   {"close_ts",now+3600},{"settlement_ts",now+7200},{"yes_bid",.45},{"yes_ask",.50},{"no_bid",.50},{"no_ask",.55},
   {"yes_ask_size",9.0},{"no_ask_size",7.0},{"fees",{{"known",true},{"theta",.06}}}};
 json pair={{"id","test"},{"kalshi_ticker",k["ticker"]},{"polymarket_slug",p["slug"]},{"relation","same"},{"verified",true},
   {"kalshi_hash",crossvenue_contract_hash(k)},{"polymarket_hash",crossvenue_contract_hash(p)}};
 auto result=crossvenue_evaluate(pair,k,p,s,now);auto o=result["opportunities"][0];
 CHECK(o["eligible"]==true);CHECK(o["kalshi_side"]=="yes");CHECK(o["polymarket_side"]=="no");
 CHECK(o["quantity"]==7);CHECK(o["net_edge"].get<double>()>.04);CHECK(o["total_cost"].get<double>()>.90);
 auto reverse=pair;reverse["relation"]="opposite";
 CHECK(crossvenue_evaluate(reverse,k,p,s,now)["opportunities"][0]["polymarket_side"]=="yes");
 auto unverified=pair;unverified["verified"]=false;
 CHECK(!crossvenue_evaluate(unverified,k,p,s,now)["opportunities"][0]["eligible"].get<bool>());
 auto stale=p;stale["source_ts"]=now-16;
 CHECK(contains(crossvenue_evaluate(pair,k,stale,s,now),"timestamp is missing or stale"));
 stale=p;stale["quote_ts"]=now-6;
 CHECK(contains(crossvenue_evaluate(pair,k,stale,s,now),"too far apart"));
 auto empty=p;empty["fees"]["known"]=false;
 CHECK(crossvenue_evaluate(pair,k,empty,s,now)["opportunities"][0]["net_edge"].is_null());
 empty=p;empty["no_ask"]=nullptr;
 CHECK(!crossvenue_evaluate(pair,k,empty,s,now)["opportunities"][0]["eligible"].get<bool>());
 empty=p;empty["no_ask_size"]=.5;
 CHECK(crossvenue_evaluate(pair,k,empty,s,now)["opportunities"][0]["quantity"]==0);
 empty=p;empty["rules"]="different overtime terms";
 CHECK(contains(crossvenue_evaluate(pair,k,empty,s,now),"terms changed"));
 empty=p;empty["settlement_ts"]=0;
 CHECK(contains(crossvenue_evaluate(pair,k,empty,s,now),"endDate is not settlement"));
 auto wk=k,wp=p;wk["rules"]="The Weather Company";wp["rules"]="National Weather Service";
 auto weather=pair;weather["kalshi_hash"]=crossvenue_contract_hash(wk);weather["polymarket_hash"]=crossvenue_contract_hash(wp);
 CHECK(contains(crossvenue_evaluate(weather,wk,wp,s,now),"settlement sources differ"));
 CHECK(crossvenue_evaluate(pair,k,p,crossvenue_defaults(),now)["opportunities"][0]["quantity"]==0);
 CHECK(crossvenue_candidates(json::array({k}),json::array({p})).size()==1);
 auto fee=polymarket_fee_metadata({{"feeCoefficient",.06}});CHECK(fee["known"]==true);
 CHECK(!polymarket_fee_metadata(json::object())["known"].get<bool>());
 CHECK(std::fabs(polymarket_taker_fee(.06,1,.5)-.02)<1e-12);
 CHECK(std::fabs(polymarket_taker_fee(.1,1,.5)-.02)<1e-12);
 CHECK(std::fabs(polymarket_taker_fee(.14,1,.5)-.04)<1e-12);
 CHECK(throws([]{polymarket_taker_fee(-.1,1,.5);}));
 json book={{"marketData",{{"marketSlug","test"},{"state","MARKET_STATE_OPEN"},{"transactTime","2026-09-08T20:00:00Z"},
   {"bids",json::array({{{"px",{{"value","0.3"},{"currency","USD"}}},{"qty","1.5"}},{{"px",{{"value","0.4"},{"currency","USD"}}},{"qty","3.75"}}})},
   {"offers",json::array({{{"px",{{"value","0.6"},{"currency","USD"}}},{"qty","2.25"}}})}}}};
 auto b=normalize_polymarket_book(book,"test",now);CHECK(b["valid"]==true);CHECK(b["quotes"]["yes_bid"]==.4);
 CHECK(b["quotes"]["no_ask"]==.6);CHECK(b["quotes"]["no_ask_size"]==3.75);CHECK(b["quotes"]["yes_ask_size"]==2.25);
 CHECK(!normalize_polymarket_book(book,"wrong",now)["valid"].get<bool>());
 book["marketData"]["bids"][0]["px"]["currency"]="EUR";
 CHECK(!normalize_polymarket_book(book,"test",now)["valid"].get<bool>());
 CHECK(throws([]{polymarket_us_signature("invalid","GET","/v1/account/balances",1);}));
 // Public all-zero test seed, deliberately unrelated to any account credential.
 const std::string seed="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
 auto sig=polymarket_us_signature(seed,"GET","/v1/account/balances",1234567890000LL);
 CHECK(sig.size()==88);CHECK(sig==polymarket_us_signature(seed,"GET","/v1/account/balances?cursor=abc",1234567890000LL));
 CHECK(sig!=polymarket_us_signature(seed,"POST","/v1/account/balances",1234567890000LL));
 PolymarketClient anonymous;CHECK(!anonymous.credential_status());CHECK(!anonymous.account_summary()["connected"].get<bool>());
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
