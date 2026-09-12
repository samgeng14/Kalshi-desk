#include "desk/pair_models.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include "desk/fees.h"
#include "desk/timeutil.h"
namespace desk {
namespace {
double mid(double b,double a){return .5*(a+b);}
double logit(double p){p=std::clamp(p,.001,.999);return std::log(p/(1-p));}
double sigmoid(double x){return 1/(1+std::exp(-std::clamp(x,-30.,30.)));}
double residual(const PairSample& s){return logit(mid(s.kb,s.ka))-logit(mid(s.pb,s.pa));}
double gap(const PairSample& s){return mid(s.kb,s.ka)-mid(s.pb,s.pa);}
bool valid(const PairSample& s){return std::isfinite(s.kb)&&std::isfinite(s.ka)&&std::isfinite(s.pb)&&std::isfinite(s.pa)&&s.kb>0&&s.kb<s.ka&&s.ka<1&&s.pb>0&&s.pb<s.pa&&s.pa<1;}
struct Fit {double a=0,b=0;bool valid=false;};
Fit fit(const std::vector<double>& x,const std::vector<double>& y,size_t n){
  if(n<10)return {};double mx=0,my=0,xx=0,xy=0;
  for(size_t i=0;i<n;++i){mx+=x[i];my+=y[i];}mx/=n;my/=n;
  for(size_t i=0;i<n;++i){xx+=(x[i]-mx)*(x[i]-mx);xy+=(x[i]-mx)*(y[i]-my);}
  if(xx<1e-10)return {};double b=xy/xx;return {my-b*mx,b,true};
}
// Conservative fee reserves include per-order cent rounding. No maker rebate assumed.
double fee(double price,double qty,bool kalshi,const json& row){
  if(kalshi){Series s;s.fee_type=row.value("fee_type","quadratic");s.fee_multiplier=row.value("fee_multiplier",1.);return FeeModel::from_series(s).fee(price,qty,true);}
  return std::ceil((.06*qty*price*(1-price)-1e-10)*100)/100;
}
double cost(const json& r,int direction,double qty){
  const double k=direction>0?r.value("kalshi_ask",0.):1-r.value("kalshi_bid",0.);
  const double p=direction>0?1-r.value("pm_bid",0.):r.value("pm_ask",0.);
  return qty*(k+p)+fee(k,qty,true,r)+fee(p,qty,false,r);
}
double proceeds(const json& r,int direction,double qty){
  const double k=direction>0?r.value("kalshi_bid",0.):1-r.value("kalshi_ask",0.);
  const double p=direction>0?1-r.value("pm_ask",0.):r.value("pm_bid",0.);
  return qty*(k+p)-fee(k,qty,true,r)-fee(p,qty,false,r);
}
double depth(const json& r,int direction,bool exit){
  const bool ky=(direction>0)!=exit;
  return std::max(0.,std::min(r.value(ky?"kalshi_ask_size":"kalshi_bid_size",0.),r.value(ky?"pm_bid_size":"pm_ask_size",0.)));
}
}
json pair_model_signals(const std::deque<PairSample>& prior,const PairSample& cur,double costs,double settlement_edge){
  json rows=json::array();
  for(const auto* name:{"complementary_spread","residual_reversion","venue_lead_lag"})rows.push_back({{"model",name},{"eligible",false},{"reason","Collecting synchronized quotes"},{"net_edge",0.},{"direction",0},{"samples",0},{"live_enabled",false}});
  if(!valid(cur)||!std::isfinite(costs)||costs<0||!std::isfinite(settlement_edge))return rows;
  rows[0]["net_edge"]=settlement_edge;rows[0]["direction"]=cur.ka+1-cur.pb < 1-cur.kb+cur.pa?1:-1;
  rows[0]["eligible"]=settlement_edge>=.01;
  rows[0]["reason"]=settlement_edge>=.01?"Conditional settlement spread; both legs and identical resolution required":"No spread left after both entry fees and buffer";
  std::vector<PairSample> p;
  for(const auto& s:prior)if(s.ts<cur.ts&&valid(s))p.push_back(s);
  for(int j=1;j<3;++j)rows[j]["samples"]=p.size();
  if(p.size()<90||cur.ts-p.front().ts<900)return rows;
  for(size_t i=1;i<p.size();++i)if(p[i].ts<=p[i-1].ts||p[i].ts-p[i-1].ts>30){for(int j=1;j<3;++j)rows[j]["reason"]="History has a quote gap";return rows;}
  if(cur.ts-p.back().ts>30)return rows;
  std::vector<double> x,y;
  for(size_t i=1;i<p.size();++i){x.push_back(residual(p[i-1]));y.push_back(residual(p[i]));}
  size_t train=x.size()*2/3;auto f=fit(x,y,train);
  if(f.valid&&f.b>0&&f.b<.995){
    double mse=0,base=0;for(size_t i=train;i<x.size();++i){mse+=std::pow(y[i]-f.a-f.b*x[i],2);base+=std::pow(y[i]-x[i],2);}
    auto all=fit(x,y,x.size());
    if(!all.valid||all.b<=0||all.b>=.995)return rows;
    double half=-std::log(2.)/std::log(all.b); // observations, not seconds
    double equilibrium=all.a/(1-all.b),forecast=equilibrium+(residual(cur)-equilibrium)*std::pow(all.b,6.);
    double change=sigmoid(logit(mid(cur.pb,cur.pa))+forecast)-mid(cur.kb,cur.ka);
    double error=std::sqrt(mse/(x.size()-train))*.25*std::sqrt(6.);
    double net=std::fabs(change)-costs-2*error;
    const bool stable=all.b>0&&all.b<.995&&half>=1&&half<=30;
    rows[1].update({{"net_edge",net},{"direction",change>0?1:-1},{"half_life_samples",half},{"holdout_mse",mse/(x.size()-train)},{"baseline_mse",base/(x.size()-train)},
      {"eligible",stable&&base>0&&mse<base*.95&&net>=.01},{"reason",stable&&base>0&&mse<base*.95?"Six-step residual convergence minus round-trip costs and error buffer":"Reversion failed the chronological persistence baseline"}});
  }else rows[1]["reason"]="Residual is not stably mean reverting";
  // Lagged PM-minus-K move predicts the NEXT relative move; the response is never contemporaneous.
  x.clear();y.clear();for(size_t i=1;i+1<p.size();++i){x.push_back(-(gap(p[i])-gap(p[i-1])));y.push_back(gap(p[i+1])-gap(p[i]));}
  train=x.size()*2/3;f=fit(x,y,train);
  if(f.valid){double mse=0,base=0;for(size_t i=train;i<x.size();++i){mse+=std::pow(y[i]-f.a-f.b*x[i],2);base+=y[i]*y[i];}
    auto all=fit(x,y,x.size());double forecast=all.a+all.b*(-(gap(cur)-gap(p.back())));
    double net=std::fabs(forecast)-costs-2*std::sqrt(mse/(x.size()-train));
    rows[2].update({{"net_edge",net},{"direction",forecast>0?1:-1},{"holdout_mse",mse/(x.size()-train)},{"baseline_mse",base/(x.size()-train)},
      {"eligible",base>0&&mse<base*.95&&net>=.01},{"reason",base>0&&mse<base*.95?"One-step relative catch-up after fees and prediction error":"Lead-lag did not beat no relative price change"}});
  }else rows[2]["reason"]="Not enough independent relative price moves";
  return rows;
}
PairModelLab::PairModelLab(Store& store):store_(store){
  store_.exec("CREATE TABLE IF NOT EXISTS pair_model_samples(pair_key TEXT,ts INTEGER,regime TEXT,book TEXT,PRIMARY KEY(pair_key,ts))");
  state_={{"running",false},{"per_pair_dollars",6.},{"min_edge",.01},{"hold_seconds",300},{"positions",json::array()},{"history",json::array()},{"pending",json::array()},{"pnl",0.},{"last_step_ts",0}};
  if(auto raw=store_.kv_get("pair_model_lab:v1")){try{state_.update(json::parse(*raw));}catch(...) {}}
  state_["running"]=false; // Research never silently starts after a restart.
}
void PairModelLab::persist(){store_.kv_set("pair_model_lab:v1",state_.dump());}
bool PairModelLab::running(){std::lock_guard<std::mutex> lk(mu_);return state_.value("running",false);}
json PairModelLab::configure(const json& body){
  std::lock_guard<std::mutex> lk(mu_);auto next=state_;
  if(!body.is_object())throw std::invalid_argument("paper model controls must be an object");
  for(const auto& v:body.items()){
    if(v.key()=="running"){if(!v.value().is_boolean())throw std::invalid_argument("running must be boolean");next[v.key()]=v.value();}
    else if(v.key()=="per_pair_dollars"||v.key()=="min_edge"||v.key()=="hold_seconds"){
      if(!v.value().is_number())throw std::invalid_argument("numeric model setting required");double n=v.value();
      double lo=v.key()=="per_pair_dollars"?1:(v.key()=="min_edge"?.01:30),hi=v.key()=="per_pair_dollars"?7:(v.key()=="min_edge"?.2:1800);
      if(!std::isfinite(n)||n<lo||n>hi)throw std::invalid_argument("model setting outside range");next[v.key()]=n;
    }else throw std::invalid_argument("unknown paper model setting: "+v.key());
  }
  if(!next.value("running",false))next["pending"]=json::array();state_=next;persist();return state_;
}
json PairModelLab::view(){std::lock_guard<std::mutex> lk(mu_);json out=state_;out["candidates"]=rows_;out["live_enabled"]=false;out["mode"]="paper";
  out["notice"]="Experimental models, not proven profits. Paper entries require next-snapshot confirmation and both quoted depths; assume atomic fills and omit real leg latency. Pausing stops new entries; open paper exits keep running. Existing live positions are never touched.";return out;}
void PairModelLab::step(const json& input,long long now,double existing_cap){
  std::lock_guard<std::mutex> lk(mu_);rows_=json::array();std::map<std::string,json> books;std::map<std::string,json> signals;
  state_["capital_cap"]=std::max(0.,existing_cap);state_["last_step_ts"]=now;
  for(const auto& r:input){
    const auto key=r.value("kalshi_ticker","")+"|"+r.value("slug","");const auto regime=r.value("regime","");
    PairSample cur{now,r.value("kalshi_bid",0.),r.value("kalshi_ask",0.),r.value("pm_bid",0.),r.value("pm_ask",0.)};
    const auto kt=r.value("kalshi_quote_ts",0LL),pt=r.value("pm_depth_ts",0LL);
    const bool fresh=valid(cur)&&kt>0&&pt>0&&kt<=now&&pt<=now&&now-kt<=20&&now-pt<=20&&std::abs(kt-pt)<=10;
    auto& h=history_[key];
    if(regimes_[key]!=regime||(!h.empty()&&now-h.back().ts>30)){h.clear();regimes_[key]=regime;}
    if(!fresh||regime.empty()){
      rows_.push_back({{"ticker",r.value("kalshi_ticker","")},{"model","all"},{"eligible",false},{"reason",!fresh?"Need synchronized depth on both venues":"Need current game state / verified pregame start"}});continue;
    }
    books[key]=r;
    double rt=std::max(cost(r,1,1)-proceeds(r,1,1),cost(r,-1,1)-proceeds(r,-1,1))+.02;
    double settlement=1-std::min(cost(r,1,1),cost(r,-1,1))-.02;
    auto modelRows=pair_model_signals(h,cur,rt,settlement);
    modelRows[0]["direction"]=cost(r,1,1)<cost(r,-1,1)?1:-1;
    for(auto m:modelRows){m["ticker"]=r.value("kalshi_ticker","");m["key"]=key;m["event"]=r.value("kalshi_event","");m["regime"]=regime;
      m["eligible"]=m.value("eligible",false)&&m.value("net_edge",0.)>=state_.value("min_edge",.01);rows_.push_back(m);signals[key+"|"+m["model"].get<std::string>()]=m;}
    if(h.empty()||now-h.back().ts>=10){store_.exec("INSERT OR IGNORE INTO pair_model_samples VALUES(?,?,?,?)",{key,now,regime,r.dump()});h.push_back(cur);while(h.size()>300)h.pop_front();}
  }
  double used=0;std::set<std::string> busy;
  json held=json::array();
  for(auto t:state_["positions"]){const auto key=t["key"].get<std::string>();const int dir=t["direction"];const double qty=t["quantity"],entry=t["cost"];
    auto b=books.find(key);bool closed=false;
    if(b!=books.end()){
      double net=proceeds(b->second,dir,qty)-entry;t["mark_pnl"]=net;t["mark_ts"]=now;
      const bool exit=net>=.02*qty||net<=-.7*entry||now-t.value("opened_ts",now)>=state_.value("hold_seconds",300.)||b->second.value("regime","")!=t.value("regime","");
      if(exit&&depth(b->second,dir,true)>=qty){t["closed_ts"]=now;t["pnl"]=net;t["state"]="closed";state_["pnl"]=state_.value("pnl",0.)+net;state_["history"].push_back(t);closed=true;}
      else t["state"]=exit?"exit_waiting_for_depth":"open";
    }else t["state"]="waiting_for_fresh_books";
    if(!closed){held.push_back(t);used+=entry;busy.insert(t.value("event",""));}
  }state_["positions"]=held;
  json pending=json::array();
  if(state_.value("running",false)){
    for(auto p:state_["pending"]){auto key=p["key"].get<std::string>();auto b=books.find(key);auto s=signals.find(key+"|"+p["model"].get<std::string>());
      if(now-p.value("created_ts",now)>30||busy.count(p.value("event","")))continue;
      if(now<=p.value("created_ts",now)||b==books.end()){pending.push_back(p);continue;}
      if(s==signals.end()||!s->second.value("eligible",false)||s->second["direction"]!=p["direction"]||s->second["regime"]!=p["regime"])continue;
      int dir=p["direction"];double room=std::min(state_.value("per_pair_dollars",6.),std::max(0.,existing_cap+std::min(0.,state_.value("pnl",0.))-used));
      double qty=std::floor(std::min(room/cost(b->second,dir,1),depth(b->second,dir,false)));
      while(qty>=1&&cost(b->second,dir,qty)>room+1e-9)--qty;
      if(qty<1||cost(b->second,dir,1)>p.value("unit_limit",0.)+1e-9)continue;
      p["quantity"]=qty;p["cost"]=cost(b->second,dir,qty);p["opened_ts"]=now;p["state"]="open";
      state_["positions"].push_back(p);used+=p["cost"].get<double>();busy.insert(p.value("event",""));
    }
    for(const auto& p:pending)busy.insert(p.value("event",""));
    for(const auto& s:rows_){if(!s.value("eligible",false)||busy.count(s.value("event","")))continue;
      if(used>=existing_cap+std::min(0.,state_.value("pnl",0.)))break;
      auto b=books.find(s["key"].get<std::string>());if(b==books.end())continue;
      json p=s;p["created_ts"]=now;p["unit_limit"]=cost(b->second,s["direction"].get<int>(),1);pending.push_back(p);busy.insert(s.value("event",""));
    }
  }
  state_["pending"]=pending;state_["reserved"]=used;
  while(state_["history"].size()>500)state_["history"].erase(state_["history"].begin());
  persist();
}
} // namespace desk
