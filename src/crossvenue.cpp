#include "desk/crossvenue.h"

#include <openssl/sha.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>

#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/risk.h"
#include "desk/polymarket.h"
#include "desk/timeutil.h"
#include "desk/universe.h"

namespace desk {
namespace {
constexpr const char* kSettings = "crossvenue:settings:v1";
constexpr const char* kPairs = "crossvenue:pairs:v1";
double num(const json& j, const std::string& k, double fallback = 0) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() && std::isfinite(it->get<double>()) ? it->get<double>() : fallback;
}
bool finite(const json& j, const std::string& k) { return j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>()); }
bool flag(const json& j, const char* k) { return j.contains(k) && j[k].is_boolean() && j[k].get<bool>(); }
std::string str(const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : ""; }
std::set<std::string> tokens(const std::string& text) {
  const std::set<std::string> skip = {"the","will","in","on","at","of","to","a","an","vs","and","be","win","game","highest","temperature"};
  std::set<std::string> out; std::string word;
  auto flush = [&] { if (word.size() >= 3 && !skip.count(word)) out.insert(word); word.clear(); };
  for (unsigned char ch : text) { if (std::isalnum(ch)) word += static_cast<char>(std::tolower(ch)); else flush(); }
  flush(); return out;
}
bool identifier(const std::string& s) {
  return !s.empty() && s.size() <= 220 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.';
  });
}
bool quote_ok(const json& m, long long now, double max_age) {
  double ts = num(m, "quote_ts");
  return ts > 0 && ts <= now && now - ts <= max_age;
}
std::string source_kind(const std::string& rules) {
  if (rules.find("Weather Company") != std::string::npos) return "twc";
  if (rules.find("National Weather Service") != std::string::npos || rules.find("NWS") != std::string::npos) return "nws";
  return "";
}
}  // namespace

json crossvenue_defaults() {
  return {{"enabled", true}, {"total_budget", 100.0}, {"kalshi_budget", 0.0}, {"polymarket_budget", 0.0},
          {"min_net_edge", .02}, {"slippage_per_leg", .01}, {"max_quote_age_seconds", 15.0},
          {"max_quote_skew_seconds", 5.0}, {"max_horizon_hours", 48.0}};
}
json crossvenue_settings(const json& patch, const json& previous) {
  if (!patch.is_object() || !previous.is_object()) throw std::runtime_error("settings must be an object");
  json out = crossvenue_defaults(); out.update(previous);
  for (const auto& [key, value] : patch.items()) {
    if (!out.contains(key)) throw std::runtime_error("unknown cross-platform setting: " + key);
    out[key] = value;
  }
  for (const auto& [key, value] : out.items()) {
    if (!crossvenue_defaults().contains(key)) throw std::runtime_error("unknown cross-platform setting: " + key);
    if (key == "enabled") { if (!value.is_boolean()) throw std::runtime_error("enabled must be boolean"); }
    else if (!finite(out, key)) throw std::runtime_error(key + " must be finite");
  }
  auto range = [&](const char* k, double lo, double hi) {
    if (num(out,k) < lo || num(out,k) > hi) throw std::runtime_error(std::string(k) + " outside supported range");
  };
  range("total_budget", 0, 100); range("kalshi_budget", 0, 100); range("polymarket_budget", 0, 100);
  range("min_net_edge", .001, .5); range("slippage_per_leg", 0, .1);
  range("max_quote_age_seconds", 1, 60); range("max_quote_skew_seconds", 0, 15); range("max_horizon_hours", .1, 48);
  if (num(out,"kalshi_budget") + num(out,"polymarket_budget") > num(out,"total_budget") + 1e-9)
    throw std::runtime_error("venue allocations exceed the total analysis budget");
  return out;
}

std::string crossvenue_contract_hash(const json& m) {
  // Quotes and status change normally; outcome wording, time horizon and rules invalidate review.
  json identity;
  for (const char* k : {"title","rules","yes_label","no_label","close_ts","settlement_ts"})
    identity[k] = m.contains(k) ? m[k] : json(nullptr);
  const std::string payload = identity.dump();
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), hash);
  std::ostringstream hex; for (unsigned char b : hash) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  return hex.str();
}

json crossvenue_evaluate(const json& pair, const json& k, const json& p, const json& input, long long now) {
  const json settings = crossvenue_settings(json::object(), input);
  json out = pair, reasons = json::array();
  out["kalshi_title"] = str(k,"title"); out["polymarket_title"] = str(p,"title");
  out["kalshi_rules"] = str(k,"rules"); out["polymarket_rules"] = str(p,"rules");
  out["kalshi_url"] = str(k,"url"); out["polymarket_url"] = str(p,"url");
  out["kalshi_yes_label"] = str(k,"yes_label"); out["polymarket_yes_label"] = str(p,"yes_label");
  auto block = [&](const std::string& why) { reasons.push_back(why); };
  if (!flag(pair,"verified")) block("settlement and outcome mapping have not been reviewed");
  if (str(pair,"relation") != "same" && str(pair,"relation") != "opposite") block("outcome relation is unknown");
  if (str(k,"rules").empty() || str(p,"rules").empty()) block("contract rules are missing");
  if (flag(pair,"verified") && (str(pair,"kalshi_hash") != crossvenue_contract_hash(k) || str(pair,"polymarket_hash") != crossvenue_contract_hash(p)))
    block("contract terms changed or review fingerprint is missing; review again");
  const std::string ks = source_kind(str(k,"rules")), ps = source_kind(str(p,"rules"));
  if (!ks.empty() && !ps.empty() && ks != ps) block("weather settlement sources differ; this is basis risk, not a matched payout");
  if (str(k,"environment") == "demo") block("Kalshi demo quotes cannot establish an executable production pair");
  for (const auto& [m,label] : std::vector<std::pair<json,std::string>>{{k,"Kalshi"},{p,"Polymarket US"}}) {
    if (str(m,"status") != "active" && str(m,"status") != "open") block(label + " market is not open");
    if (num(m,"close_ts") <= now) block(label + " close time is missing or passed");
    if (!quote_ok(m,now,num(settings,"max_quote_age_seconds"))) block(label + " quote is missing, stale or future-dated");
  }
  if (num(p,"source_ts") <= 0 || num(p,"source_ts") > now || now-num(p,"source_ts") > num(settings,"max_quote_age_seconds"))
    block("Polymarket exchange book timestamp is missing or stale");
  double p_settle = num(p,"settlement_ts");
  if (p_settle <= 0) p_settle = num(pair,"polymarket_settlement_ts");
  const double max_ts = now + 3600*num(settings,"max_horizon_hours");
  if (num(k,"settlement_ts") <= now || num(k,"settlement_ts") > max_ts) block("Kalshi expected settlement is unknown or outside the horizon");
  if (p_settle <= now || p_settle > max_ts) block("Polymarket expected settlement is unknown or outside the horizon; endDate is not settlement");
  const double skew = std::fabs(num(k,"quote_ts") - num(p,"quote_ts"));
  if (skew > num(settings,"max_quote_skew_seconds")) block("venue quote snapshots are too far apart");
  const auto fee = p.value("fees", json::object());
  const bool p_fee_known = flag(fee,"known") && finite(fee,"theta") && num(fee,"theta") >= 0;
  const bool k_fee_known = finite(k,"fee_multiplier") && num(k,"fee_multiplier") >= 0 &&
      (str(k,"fee_type") == "quadratic" || str(k,"fee_type") == "quadratic_with_maker_fees" || str(k,"fee_type") == "quadratic_with_combo_maker_fees");
  if (!p_fee_known) block("Polymarket taker fee is unknown");
  if (!k_fee_known) block("Kalshi taker fee is unknown");
  out["reasons"] = reasons;
  out["opportunities"] = json::array();
  for (const std::string side : {"yes","no"}) {
    const std::string other = str(pair,"relation") == "opposite" ? side : (side == "yes" ? "no" : "yes");
    const double ka = num(k,side+"_ask"), pa = num(p,other+"_ask");
    const double kb = num(k,side+"_bid"), pb = num(p,other+"_bid");
    const double kd = num(k,side+"_ask_size"), pd = num(p,other+"_ask_size");
    json why = reasons;
    bool prices = ka > 0 && ka < 1 && pa > 0 && pa < 1 && kb > 0 && kb <= ka && pb > 0 && pb <= pa;
    if (!prices) why.push_back("missing or crossed executable two-sided books");
    if (kd < 1 || pd < 1) why.push_back("not enough displayed depth for both legs");
    const double slip = num(settings,"slippage_per_leg");
    // Fee reserves are rounded upward per one-contract fill, conservatively covering fragmented
    // fills. This is an upper estimate, not a claim about the actual exchange fee charged.
    const double kpx = std::min(.9999,ka+slip), ppx = std::min(.9999,pa+slip);
    const double kf = k_fee_known ? FeeModel{str(k,"fee_type"),num(k,"fee_multiplier")}.fee(kpx,1,true) : 0;
    const double pf = p_fee_known ? std::ceil(num(fee,"theta")*ppx*(1-ppx)*100-1e-9)/100 : 0;
    const bool costs_known = prices && k_fee_known && p_fee_known && ka+slip < 1 && pa+slip < 1;
    const double kc = ka+slip+kf, pc = pa+slip+pf, cost = kc+pc, edge = 1-cost;
    if (costs_known && edge + 1e-9 < num(settings,"min_net_edge")) why.push_back("combined entry cost leaves too little net payout cushion");
    if (!costs_known) why.push_back("pair cost cannot be established");
    double quantity = costs_known ? std::floor(std::min({kd,pd,num(settings,"kalshi_budget")/kc,num(settings,"polymarket_budget")/pc,num(settings,"total_budget")/cost})) : 0;
    if (quantity < 1) why.push_back("analysis allocations or depth cannot fund one matched pair");
    const bool eligible = why.empty();
    out["opportunities"].push_back({{"kalshi_side",side},{"polymarket_side",other},{"kalshi_ask",ka>0?json(ka):json(nullptr)},
      {"polymarket_ask",pa>0?json(pa):json(nullptr)},{"kalshi_fee",k_fee_known?json(kf):json(nullptr)},
      {"polymarket_fee",p_fee_known?json(pf):json(nullptr)},{"total_cost",costs_known?json(cost):json(nullptr)},
      {"payout",1.0},{"net_edge",costs_known?json(edge):json(nullptr)},{"quantity",eligible?quantity:0},
      {"analysis_quantity",quantity},{"kalshi_cost",costs_known?json(kc):json(nullptr)},{"polymarket_cost",costs_known?json(pc):json(nullptr)},
      {"kalshi_depth",kd},{"polymarket_depth",pd},{"kalshi_quote_ts",num(k,"quote_ts")},{"polymarket_quote_ts",num(p,"quote_ts")},
      {"quote_skew_seconds",skew},{"eligible",eligible},{"reasons",why},{"reason",why.empty()?"reviewed price discrepancy; no orders sent":why.dump()},
      {"warnings",json::array({"$1 payout assumes the reviewed complementary outcomes resolve normally; void or cancellation rules can change it",
        "Two venues do not offer atomic fills; one leg can fill while the other fails",
        "Fee reserves include upward rounding; allocations are a plan, not exchange balances or reserved capital"})}});
  }
  return out;
}

json crossvenue_candidates(const json& kalshi, const json& poly, int limit) {
  struct Candidate { double score; json row; };
  std::vector<Candidate> found;
  for (const auto& p : poly) {
    const auto pt = tokens(str(p,"title"));
    if (pt.size() < 2) continue;
    for (const auto& k : kalshi) {
      if (str(k,"series_ticker").find("15M") != std::string::npos) continue;
      const auto kt = tokens(str(k,"title"));
      int common = 0; for (const auto& token : pt) if (kt.count(token)) ++common;
      if (common < 2) continue;
      const double score = static_cast<double>(common)/(pt.size()+kt.size()-common);
      if (score < .22) continue;
      found.push_back({score, {{"kalshi_ticker",str(k,"ticker")},{"polymarket_slug",str(p,"slug")},
        {"kalshi_title",str(k,"title")},{"polymarket_title",str(p,"title")},{"score",score},
        {"reason","title similarity only; dates, outcome orientation, source, overtime and void rules still need review"}}});
    }
  }
  std::sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.score>b.score;});
  json out=json::array(); std::set<std::string> used;
  for (const auto& c:found) if (used.insert(c.row["polymarket_slug"].get<std::string>()).second) {out.push_back(c.row);if(static_cast<int>(out.size())>=limit)break;}
  return out;
}

CrossVenueDesk::CrossVenueDesk(const Config& cfg, Store& store)
    : cfg_(cfg), store_(store), kalshi_(cfg.rest_base(),nullptr,cfg.user_agent), settings_(crossvenue_defaults()) {
  if (auto s=store_.kv_get(kSettings)) settings_=crossvenue_settings(json::parse(*s),settings_);
  if (auto p=store_.kv_get(kPairs)) {mappings_=json::parse(*p);if(!mappings_.is_array())throw std::runtime_error("invalid saved cross-platform mappings");}
  store_.exec("CREATE TABLE IF NOT EXISTS crossvenue_observations(pair_id TEXT,ts INTEGER,k_quote_ts INTEGER,p_quote_ts INTEGER,basis REAL,payload TEXT,PRIMARY KEY(pair_id,ts))");
  status_={{"read_only",true},{"last_success_ts",0},{"last_error",""},{"credential_error",""}};
  try {
    const char* path=std::getenv("POLYMARKET_US_CREDENTIAL_FILE");
    poly_=std::make_unique<PolymarketClient>(PolymarketClient::from_environment(cfg.user_agent,path?path:""));
  } catch (const std::exception&) {
    status_["credential_error"]="Polymarket credentials could not be loaded; check key format and file permissions. Public data remains available.";
    poly_=std::make_unique<PolymarketClient>(cfg.user_agent);
  }
}
CrossVenueDesk::~CrossVenueDesk(){stop();}
void CrossVenueDesk::start(){if(worker_.joinable())return;stop_=false;worker_=std::thread([this]{while(!stop_){try{refresh();}catch(const std::exception& e){std::lock_guard<std::mutex>lk(mu_);status_["last_error"]=e.what();}std::unique_lock<std::mutex>lk(mu_);wake_.wait_for(lk,std::chrono::seconds(5),[this]{return stop_.load();});}});}
void CrossVenueDesk::stop(){stop_=true;wake_.notify_all();if(worker_.joinable())worker_.join();}
void CrossVenueDesk::persist(){store_.kv_set(kSettings,settings_.dump());store_.kv_set(kPairs,mappings_.dump());}

json CrossVenueDesk::kalshi_market(const Market& m) {
  json out={{"ticker",m.ticker},{"series_ticker",m.series_ticker},{"title",m.title},
    {"rules",m.rules_primary},{"yes_label",m.yes_sub_title.empty()?m.subtitle:m.yes_sub_title},{"no_label",m.no_sub_title},
    {"status",m.status},{"close_ts",parse_iso(m.close_time)},{"settlement_ts",expected_settlement_ts(m)},
    {"url","https://kalshi.com/markets/"+m.series_ticker},{"environment",cfg_.env}};
  if(auto s=store_.get_series(m.series_ticker)){out["fee_type"]=s->fee_type;out["fee_multiplier"]=s->fee_multiplier;}
  return out;
}

void CrossVenueDesk::refresh(){
  json mappings;
  {std::lock_guard<std::mutex>lk(mu_);if(!settings_.value("enabled",true))return;mappings=mappings_;}
  long long now=now_s();
  if(now-discovery_ts_>=60){
    json page=poly_->markets(100,discovery_offset_);
    {std::lock_guard<std::mutex>lk(mu_);
      for(const auto& row:page.value("markets",json::array())){
        const std::string slug=str(row,"slug");if(slug.empty())continue;poly_cache_[slug]=row;
      }
      poly_markets_=json::array();for(const auto& [slug,row]:poly_cache_)if(str(row,"status")=="active"||str(row,"status")=="open")poly_markets_.push_back(row);
      status_["last_success_ts"]=now_s();status_["last_error"]="";
    }
    discovery_offset_=page.value("markets",json::array()).size()<100?0:(discovery_offset_+100)%1000;
    discovery_ts_=now;
  }
  // Poll bounded batches independently of the live strategy's mutex and HTTP client.
  const size_t count=std::min<size_t>(mappings.size(),4);
  for(size_t i=0;i<count&&!stop_;++i){
    const auto pair=mappings[(pair_cursor_+i)%mappings.size()];
    const std::string tk=str(pair,"kalshi_ticker"),slug=str(pair,"polymarket_slug");
    try{
      auto market=kalshi_.get_market(tk);json k=kalshi_market(market);
      auto b=kalshi_.get_orderbook(tk,10);
      k.update({{"quote_ts",now_s()},{"yes_bid",b.best_yes_bid()},{"yes_ask",b.best_yes_ask()},
        {"no_bid",b.best_no_bid()},{"no_ask",b.best_no_ask()},
        {"yes_bid_size",b.yes_bids.empty()?0:b.yes_bids.back().qty},{"yes_ask_size",b.no_bids.empty()?0:b.no_bids.back().qty},
        {"no_bid_size",b.no_bids.empty()?0:b.no_bids.back().qty},{"no_ask_size",b.yes_bids.empty()?0:b.yes_bids.back().qty}});
      json p=poly_->market(slug),book=poly_->book(slug);
      p.update(book.value("quotes",json::object()));p["quote_ts"]=book.value("quote_ts",0LL);p["source_ts"]=book.value("source_ts",0LL);
      p["book"]=book.value("book",json::object());k["book"]=orderbook_to_json(b);
      if(!book.value("valid",false))p["quote_ts"]=0;
      {std::lock_guard<std::mutex>lk(mu_);kalshi_cache_[tk]=k;poly_cache_[slug]=p;status_["last_success_ts"]=now_s();}
      const long long received=now_s();
      if(quote_ok(k,received,15)&&quote_ok(p,received,15)&&num(p,"source_ts")>0&&received-num(p,"source_ts")<=15&&num(p,"source_ts")<=received&&
         std::fabs(num(k,"quote_ts")-num(p,"quote_ts"))<=5 && received-last_record_[str(pair,"id")]>=60){
        const double km=(num(k,"yes_bid")+num(k,"yes_ask"))/2,pm=(num(p,"yes_bid")+num(p,"yes_ask"))/2;
        if(km>0&&km<1&&pm>0&&pm<1){
          const double basis=km-(str(pair,"relation")=="opposite"?1-pm:pm);
          json settings;{std::lock_guard<std::mutex>lk(mu_);settings=settings_;}
          store_.exec("INSERT OR IGNORE INTO crossvenue_observations(pair_id,ts,k_quote_ts,p_quote_ts,basis,payload) VALUES(?,?,?,?,?,?)",
            {str(pair,"id"),received,static_cast<long long>(num(k,"quote_ts")),static_cast<long long>(num(p,"quote_ts")),basis,
             json{{"kalshi",k},{"polymarket",p},{"pair",pair},{"evaluation",crossvenue_evaluate(pair,k,p,settings,received)}}.dump()});
          last_record_[str(pair,"id")]=received;
        }
      }
    }catch(const std::exception& e){std::lock_guard<std::mutex>lk(mu_);status_["last_error"]=std::string("pair ")+tk+": "+e.what();}
  }
  if(!mappings.empty())pair_cursor_=(pair_cursor_+count)%mappings.size();
  if(poly_->credential_status() && now-account_ts_>=60){
    try{json a=poly_->account_summary();std::lock_guard<std::mutex>lk(mu_);account_=a;status_["account_error"]="";}
    catch(const std::exception&){std::lock_guard<std::mutex>lk(mu_);status_["account_error"]="Polymarket account read failed; public prices may still be available";}
    account_ts_=now;
  }
}

json CrossVenueDesk::spread_stats(const std::string& id,double basis,long long now){
  auto rows=store_.query("SELECT COUNT(*) AS n,MIN(ts) AS first,MAX(ts) AS last,AVG(basis) AS mean,AVG(basis*basis) AS second FROM crossvenue_observations WHERE pair_id=? AND ts<? AND ts>=?",{id,now,now-7*86400});
  const json r=rows.empty()?json::object():rows[0];const double n=num(r,"n"),span=num(r,"last")-num(r,"first"),mean=num(r,"mean"),sd=std::sqrt(std::max(0.0,num(r,"second")-mean*mean));
  return {{"samples",n},{"span_seconds",span},{"mean",n>0?json(mean):json(nullptr)},{"sd",n>1?json(sd):json(nullptr)},
    {"z_score",n>=30&&span>=21600&&sd>1e-5?json((basis-mean)/sd):json(nullptr)},
    {"notice","Descriptive spread history only; a z-score does not establish mean reversion, independent samples or profitable fills"}};
}

json CrossVenueDesk::view(){
  const long long now=now_s();json settings,mappings,pm,status,account;std::map<std::string,json>kc,pc;
  {std::lock_guard<std::mutex>lk(mu_);settings=settings_;mappings=mappings_;pm=poly_markets_;status=status_;account=account_;kc=kalshi_cache_;pc=poly_cache_;}
  json kalshi=json::array();for(const auto& m:store_.open_markets())kalshi.push_back(kalshi_market(m));
  json pairs=json::array();
  for(const auto& pair:mappings){
    const std::string tk=str(pair,"kalshi_ticker"),slug=str(pair,"polymarket_slug");
    json k=kc.count(tk)?kc.at(tk):json::object(),p=pc.count(slug)?pc.at(slug):json::object();
    if(k.empty())if(auto m=store_.get_market(tk))k=kalshi_market(*m);
    json row=crossvenue_evaluate(pair,k,p,settings,now);
    const double km=(num(k,"yes_bid")+num(k,"yes_ask"))/2,pmid=(num(p,"yes_bid")+num(p,"yes_ask"))/2;
    row["spread_stats"]=spread_stats(str(pair,"id"),km-(str(pair,"relation")=="opposite"?1-pmid:pmid),now);
    pairs.push_back(std::move(row));
  }
  json poly={{"connected",num(status,"last_success_ts")>0&&now-num(status,"last_success_ts")<180},
    {"credentials_configured",poly_->credential_status()},{"read_only",true},{"last_success_ts",status.value("last_success_ts",0LL)},
    {"last_error",status.value("last_error","")},{"markets",pm},{"account",account}};
  json observations=store_.query("SELECT COUNT(*) AS samples,MIN(ts) AS first_ts,MAX(ts) AS last_ts FROM crossvenue_observations");
  return {{"status",status},{"settings",settings},{"polymarket",poly},{"pairs",pairs},
    {"candidates",crossvenue_candidates(kalshi,pm)},{"observations",observations.empty()?json::object():observations[0]},
    {"now",now},{"notice","Read-only research; the existing systematic budget is unchanged. No cross-venue orders or transfers are implemented."}};
}

json CrossVenueDesk::configure(const json& body){
  if(!body.is_object())throw std::runtime_error("settings body must be an object");
  {std::lock_guard<std::mutex>lk(mu_);settings_=crossvenue_settings(body.contains("settings")?body.at("settings"):body,settings_);persist();}
  wake_.notify_all();return view();
}
json CrossVenueDesk::save_pair(const json& body){
  if(!body.is_object())throw std::runtime_error("pair must be an object");
  for(const auto& [key,value]:body.items())if(!std::set<std::string>{"kalshi_ticker","polymarket_slug","relation","verified","review_note","polymarket_settlement_time"}.count(key))throw std::runtime_error("unknown pair field: "+key);
  const std::string tk=str(body,"kalshi_ticker"),slug=str(body,"polymarket_slug"),relation=str(body,"relation");
  if(!identifier(tk)||!identifier(slug))throw std::runtime_error("invalid market identifier");
  if(tk.find("15M")!=std::string::npos)throw std::runtime_error("15-minute crypto is outside this integration");
  if(relation!="same"&&relation!="opposite")throw std::runtime_error("relation must be same or opposite");
  if(!body.contains("verified")||!body["verified"].is_boolean())throw std::runtime_error("verified must be boolean");
  if(str(body,"review_note").size()>2000)throw std::runtime_error("review note too long");
  json pair={{"id",tk+"|"+slug},{"kalshi_ticker",tk},{"polymarket_slug",slug},{"relation",relation},
    {"verified",flag(body,"verified")},{"review_note",str(body,"review_note")},{"reviewed_ts",now_s()},
    {"polymarket_settlement_ts",parse_iso(str(body,"polymarket_settlement_time"))},
    {"polymarket_settlement_time",str(body,"polymarket_settlement_time")}};
  if(!str(body,"polymarket_settlement_time").empty()&&num(pair,"polymarket_settlement_ts")<=0)throw std::runtime_error("expected settlement must be a valid ISO8601 timestamp");
  {std::lock_guard<std::mutex>lk(mu_);
    if(flag(pair,"verified")){
      if(str(pair,"review_note").size()<20)throw std::runtime_error("record what you checked about sources, outcomes, dates, overtime and void handling");
      if(!kalshi_cache_.count(tk)||!poly_cache_.count(slug))throw std::runtime_error("save as unverified first; wait for both contracts and read their rules");
      const auto& k=kalshi_cache_.at(tk);const auto& p=poly_cache_.at(slug);
      if(str(k,"rules").empty()||str(p,"rules").empty())throw std::runtime_error("cannot verify a pair without both rule texts");
      pair["kalshi_hash"]=crossvenue_contract_hash(k);pair["polymarket_hash"]=crossvenue_contract_hash(p);
    }
    bool replaced=false;for(auto& row:mappings_)if(row["id"]==pair["id"]){row=pair;replaced=true;break;}
    if(!replaced){if(mappings_.size()>=20)throw std::runtime_error("20 mappings maximum for this polling service");mappings_.push_back(pair);}
    persist();
  }
  wake_.notify_all();return view();
}
}  // namespace desk
