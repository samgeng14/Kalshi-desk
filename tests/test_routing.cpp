#include <cmath>
#include <iostream>
#include <map>
#include "desk/decision_router.h"
#include "desk/systematic.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"
using namespace desk;
static int checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": "<<#x<<"\n";}}while(0)
int main(){
  const long long now=parse_iso("2026-09-09T05:30:00Z"); // 00:30 Chicago
  json settings=systematic_defaults();
  CHECK(settings["capital_budget_dollars"]==40);
  CHECK(settings["stop_loss_mode"]=="price"); // no silent widening of existing positions
  for(const auto& [label,cat]:std::vector<std::pair<std::string,std::string>>{{"Commodities","commodities"},{"Economics","economics"},{"Financials","finance"},{"Science and Technology","tech_science"}})
    CHECK(systematic_category({{"category",label}})==cat);
  json m={{"ticker","ECON-A-YES"},{"event_ticker","ECON-A"},{"category","Economics"},{"status","active"},
    {"quote_ts",now},{"fair_ts",now},{"quote_stale",false},{"fair_stale",false},{"close_ts",now+3600},{"settlement_ts",now+3600},
    {"p_model",.7},{"p_raw",.7},{"sigma",.01},{"yes_bid",.60},{"yes_ask",.62},{"no_bid",.38},{"no_ask",.4},
    {"yes_bid_size",20},{"yes_ask_size",20},{"no_bid_size",20},{"no_ask_size",20},
    {"fee_type","quadratic"},{"fee_multiplier",1},{"model_inputs",{{"valid",true}}},
    {"entry_context",{{"allowed",true}}},{"fair_strategy","test_model"},{"settlement_verified",true}};
  // Session 15: the model's own route decides inside the category's permission, and only a
  // graduated model is ever AUTO. This test is about the category rules, so the model is set up
  // to be eligible.
  settings["model_routes"]["test_model_v1"]="auto";
  settings["graduated_models"]=json::array({"test_model_v1"});
  auto screen=[&]{return systematic_screen({{"markets",json::array({m})}},settings,now);};
  CHECK(screen()["rows"][0]["verdict"]=="manual"); // category not enabled automatically
  settings["economics"]=true;
  CHECK(screen()["rows"][0]["verdict"]=="algo");
  m["settlement_ts"]=parse_iso("2026-09-11T05:01:00Z"); // under48h but day after tomorrow
  CHECK(screen()["rows"][0]["verdict"]=="blocked");
  m["settlement_ts"]=parse_iso("2026-09-11T04:59:00Z"); // tomorrow 23:59 Chicago
  CHECK(screen()["rows"][0]["verdict"]=="algo");
  m["model_inputs"]["auto_eligible"]=false;
  CHECK(screen()["rows"][0]["verdict"]=="manual");
  Store store(":memory:");Config cfg;SystematicTrader trader(store,nullptr,cfg);
  CHECK(systematic_capital(store,settings,"paper")["available"]==40);
  store.exec("INSERT INTO systematic_trades(mode,state,entry_cost,realized_pnl) VALUES('paper','closed',0,-5),('paper','open',10,0),('paper','released',3,0)");
  CHECK(systematic_capital(store,settings,"paper")["available"]==22);
  CHECK(systematic_capital(store,settings,"live")["available"]==40); // separate paper/live books
  // Percentage stop works on all-in cost, does not dump on a5% fluctuation, and never sends.
  store.exec("DELETE FROM systematic_trades");
  trader.configure({{"mode","paper"},{"settings",{{"stop_loss_mode","fraction"},{"stop_loss_fraction",.7},
      {"model_value_exit",false},{"take_profit",0},{"max_hold_minutes",0},{"stop_grace_seconds",0},{"stop_persist_seconds",0},{"paper_slippage",0}}}});
  store.exec("DELETE FROM systematic_trades");
  Market market;market.ticker="WX-A-YES";market.event_ticker="WX-A";market.series_ticker="WX";market.status="active";market.close_time=iso_of(now+3600);market.expected_expiration_time=market.close_time;store.upsert_market(market);
  store.exec("INSERT INTO systematic_trades(ticker,event_ticker,series_ticker,mode,state,side,opened_ts,filled_ts,entry_cost,entry_price,count,filled,realized_pnl,exit_proceeds,exit_fee) VALUES('WX-A-YES','WX-A','WX','paper','open','yes',?,?,.52,.50,1,1,0,0,0)",{now-100,now-100});
  store.exec("UPDATE systematic_trades SET day='',closed_ts=0,entry_fee=0,unrealized=0");
  json quote=m;quote["ticker"]=market.ticker;quote["event_ticker"]=market.event_ticker;quote["p_model"]=.6;quote["yes_bid"]=.47;quote["yes_ask"]=.49;
  trader.step({{"markets",json::array({quote})}},now);
  CHECK(trader.view()["positions"].size()==1);
  quote["yes_bid"]=.13;quote["yes_ask"]=.15;quote["quote_ts"]=now+1;
  trader.step({{"markets",json::array({quote})}},now+1);
  CHECK(trader.view()["positions"].empty());
  CHECK(trader.view()["history"][0]["exit_reason"].get<std::string>().find("capital loss stop")!=std::string::npos);
  SportsLive::GameState game;game.espn_id="game1";game.state="in";game.period=5;game.home_score=2;game.away_score=3;game.clock="0:00";game.last_play_id="play2";game.updated=now;
  json comp={{"status",{{"period",5},{"displayClock","0:00"},{"type",{{"state","in"}}}}},
             {"competitors",json::array({{{"homeAway","home"},{"score","2"}},{{"homeAway","away"},{"score","3"}}})}};
  json summary={{"header",{{"id","game1"},{"competitions",json::array({comp})}}},
    {"plays",json::array({{{"id","play2"}}})},{"winprobability",json::array({{{"playId","play2"},{"homeWinPercentage",.3}}})}};
  CHECK(SportsLive::accept_win_probability(summary,game,now));
  const auto accepted=game.wp_ts;
  summary["winprobability"][0]["playId"]="play1";
  CHECK(!SportsLive::accept_win_probability(summary,game,now+1));CHECK(game.wp_ts==accepted);
  summary["winprobability"][0]["playId"]="play2";game.home_score=3;
  CHECK(!SportsLive::accept_win_probability(summary,game,now+1));
  game.home_score=2;game.last_play_id="play3";
  CHECK(game.last_sig!=SportsLive::state_signature(game));
  CHECK(!SportsLive::accept_win_probability(summary,game,now+1));
  game.last_play_id="play2";summary.erase("plays");game.last_play_id="";
  CHECK(!SportsLive::accept_win_probability(summary,game,now+1));
  // ---- session 16: a 15-minute commodity row and an algorithm row in the same cycle ----------
  // The two lanes run off one screen. A commodity terminal-price estimate is MANUAL because its
  // model is routed there - not because the category is, and not because the cycle is in some
  // other mode - while an unrelated graduated model is still AUTO in that same cycle.
  {
    json s16=systematic_defaults();
    s16["economics"]="auto";s16["commodities"]="auto";     // the category permits it; the model does not
    s16["model_routes"]["test_model_v1"]="auto";s16["graduated_models"]=json::array({"test_model_v1"});
    CHECK(s16["model_routes"]["commodity_terminal_gold_v1"]=="manual");
    CHECK(s16["model_routes"]["commodity_terminal_wti_v1"]=="manual");
    CHECK(s16["graduated_models"].size()==1);             // nothing commodity graduated this session
    json econ=m;econ["minutes_to_close"]=120;econ["close_ts"]=now+7200;econ["open_ts"]=now-86400;
    econ["settlement_ts"]=parse_iso("2026-09-11T04:59:00Z");econ["model_inputs"]={{"valid",true}};
    const std::string GOLD="KXGOLD15M-26SEP09T0545-T4350";
    json gold={{"ticker",GOLD},{"event_ticker","KXGOLD15M-26SEP09T0545"},
      {"series_ticker","KXGOLD15M"},{"category","Commodities"},{"status","active"},
      {"quote_ts",now},{"fair_ts",now},{"quote_stale",false},{"fair_stale",false},
      // A fifteen-minute window with four minutes left: open, close and settlement are minutes
      // apart, not days.
      {"open_ts",now-660},{"close_ts",now+240},{"settlement_ts",now+241},{"minutes_to_close",4},
      {"p_model",.72},{"p_raw",.72},{"sigma",.02},{"yes_bid",.58},{"yes_ask",.60},{"no_bid",.40},{"no_ask",.42},
      {"yes_bid_size",50},{"yes_ask_size",50},{"no_bid_size",50},{"no_ask_size",50},
      {"fee_type","quadratic"},{"fee_multiplier",1},
      {"model_inputs",{{"valid",true},{"auto_eligible",false}}},
      {"entry_context",{{"allowed",true}}},{"fair_strategy","commodity_terminal_gold"},{"settlement_verified",true}};
    const json screen16=systematic_screen({{"markets",json::array({econ,gold})}},s16,now);
    // The screen prices both sides of every market; the buy side is the one either lane acts on.
    auto row_for=[&](const std::string& ticker)->json{
      for(const auto& r:screen16["rows"])if(r["ticker"]==ticker&&r["side"]=="yes")return r;
      return json::object();};
    const json econ_row=row_for("ECON-A-YES"),gold_row=row_for(GOLD);
    CHECK(econ_row.value("verdict","")=="algo");
    CHECK(gold_row.value("verdict","")=="manual");
    CHECK(econ_row.value("route","")=="auto"&&gold_row.value("route","")=="manual");
    // ... and it is the model's route that put it there, named in the row's reasons.
    bool named=false,floor_blamed=false;
    for(const auto& why:gold_row["reasons"]){
      named=named||why.get<std::string>().find("commodity_terminal_gold_v1")!=std::string::npos;
      floor_blamed=floor_blamed||why.get<std::string>().find("min minimum")!=std::string::npos;}
    CHECK(named);
    CHECK(!floor_blamed);   // four minutes is most of a fifteen-minute window, not "about to close"
    // The desk's flat "too near close" floor is ten minutes. On a fifteen-minute contract that
    // would be an off switch, so the floor is a fifth of the contract's own life instead; on a
    // long-dated contract the flat floor still binds.
    CHECK(min_minutes_to_close_for(gold,s16)==3);
    CHECK(min_minutes_to_close_for(econ,s16)==10);
    CHECK(min_minutes_to_close_for(json{{"close_ts",now+240}},s16)==10);   // no open time: the flat floor
    // A minute is the hard floor under everything: an order placed in the last seconds of a
    // market cannot be repriced or pulled, and no fraction of any contract's life may argue
    // otherwise. A two-minute contract at 0.2 would otherwise be tradable with 24 seconds left.
    json tiny={{"open_ts",now-60},{"close_ts",now+60}};
    CHECK(min_minutes_to_close_for(tiny,s16)==1);
    // And the fraction cannot be set to zero, which would remove the ten-minute guard from every
    // market on the desk rather than adapting it to short ones.
    bool refused=false;
    try { validate_systematic_settings(json{{"min_life_fraction_to_close",0}},json::object()); }
    catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
    // A floor the operator has deliberately lowered stays lowered: the minute is a floor under
    // the fraction, not a floor over the operator.
    json relaxed=s16;relaxed["min_minutes_to_close"]=0.5;
    CHECK(min_minutes_to_close_for(tiny,relaxed)==0.5);
    json soon=econ;soon["minutes_to_close"]=4;soon["close_ts"]=now+240;soon["settlement_ts"]=now+241;
    const json soon_screen=systematic_screen({{"markets",json::array({soon})}},s16,now);
    CHECK(soon_screen["rows"][0]["verdict"]=="blocked");
    bool soon_blamed=false;
    for(const auto& why:soon_screen["rows"][0]["reasons"])
      soon_blamed=soon_blamed||why.get<std::string>().find("under the 10.00 min minimum")!=std::string::npos;
    CHECK(soon_blamed);
  }
  std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
