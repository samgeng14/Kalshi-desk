#include "desk/pair_models.h"
#include "desk/sports_models.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error("check failed: " #x);}while(0)
using namespace desk;
json book(long long ts,double kb=.40,double ka=.42,double pb=.56,double pa=.58){return {{"kalshi_ticker","GAME-A-HOME"},{"kalshi_event","GAME-A"},{"slug","fixture"},{"regime","pregame"},
  {"kalshi_bid",kb},{"kalshi_ask",ka},{"pm_bid",pb},{"pm_ask",pa},{"kalshi_quote_ts",ts},{"pm_depth_ts",ts},
  {"kalshi_bid_size",50.},{"kalshi_ask_size",50.},{"pm_bid_size",50.},{"pm_ask_size",50.},{"fee_type","quadratic"},{"fee_multiplier",1.}};}
int main(){
  PairSample cur{2000,.4,.42,.56,.58};auto s=pair_model_signals({},cur,.07,.09);
  CHECK(s.size()==3&&s[0]["eligible"]==true&&s[1]["eligible"]==false&&s[2]["eligible"]==false);
  std::deque<PairSample> history;for(int i=0;i<100;++i)history.push_back({1000+i*10,.49,.51,.49,.51});
  CHECK(pair_model_signals(history,cur,.07,-.02)[1]["eligible"]==false);
  auto before=pair_model_signals(history,cur,.07,-.02);history.push_back({2001,.01,.02,.97,.98});
  CHECK(pair_model_signals(history,cur,.07,-.02)==before); // future observations cannot affect inference
  cur.ka=.39;CHECK(pair_model_signals(history,cur,.07,.09)[0]["eligible"]==false);
  Store store(":memory:");PairModelLab lab(store);lab.configure({{"running",true}});
  lab.step(json::array({book(1000)}),1000,40);CHECK(lab.view()["positions"].empty());CHECK(!lab.view()["pending"].empty());
  lab.step(json::array({book(1010)}),1010,40);auto v=lab.view();CHECK(v["positions"].size()==1);CHECK(v["reserved"].get<double>()<=6);
  auto thin=book(1320,.47,.49,.51,.53);thin["pm_ask_size"]=0.;lab.step(json::array({thin}),1320,40);
  CHECK(lab.view()["history"].empty());CHECK(lab.view()["positions"][0]["state"]=="exit_waiting_for_depth");
  lab.configure({{"running",false}});lab.step(json::array({book(1330,.47,.49,.51,.53)}),1330,40);
  CHECK(lab.view()["positions"].empty()&&lab.view()["history"].size()==1);CHECK(lab.view()["pnl"].get<double>()>0);
  bool refused=false;try{lab.configure({{"running",true},{"per_pair_dollars",8}});}catch(...){refused=true;}
  CHECK(refused&&!lab.running());PairModelLab restore(store);CHECK(!restore.running()&&restore.view()["history"].size()==1);
  Store staleStore(":memory:");PairModelLab stale(staleStore);stale.configure({{"running",true}});auto r=book(1000);r["pm_depth_ts"]=980;
  stale.step(json::array({r}),1000,40);CHECK(stale.view()["pending"].empty());
  r=book(1010);r["pm_bid_size"]=0;stale.step(json::array({r}),1010,40);r["kalshi_quote_ts"]=1020;r["pm_depth_ts"]=1020;stale.step(json::array({r}),1020,40);CHECK(stale.view()["positions"].empty());
  CHECK(std::isnan(soccer_total_over(2.,0,0,2.5,2.)));
  CHECK(soccer_total_over(2.5,3,.5,2.5,2.)==1.);
  CHECK(soccer_total_over(2.5,2,1.,2.5,2.)==0.);
  double p=soccer_total_over(2.5,0,0,2.,std::sqrt(2.));CHECK(std::fabs(p-(1-5*std::exp(-2.)))<1e-9);
  CHECK(soccer_total_over(2.5,0,.8,2.5,2.)<soccer_total_over(2.5,0,.2,2.5,2.));
  CHECK(basketball_total_over(220.5,221,.95,220.,15.)==1.);
  double reg=basketball_total_over(220.5,200,.9,220.,15.);double ot=basketball_total_over(220.5,200,.9,220.,15.,.1,20.,6.);CHECK(ot>reg&&ot<1);
  CHECK(sports_comeback_candidate(.09,.15,.20,.01,.5,true)["eligible"]==true);
  CHECK(sports_comeback_candidate(.09,.15,.20,.01,.1,true)["eligible"]==false);
  CHECK(sports_comeback_candidate(.65,.7,.60,.015,.5,true)["eligible"]==false);
  CHECK(sports_comeback_candidate(.09,.15,.20,.01,.5,false)["eligible"]==false);
  SportsTotals totals(json::object());CHECK(!totals.covers(Market{},Series{}));

  // Conditional overtime. NBA fit: margin sd 10.06 over a game, tie width 1.640, from 4,187 games.
  const double NBA_SD=10.06,NBA_TIE=1.640;
  CHECK(std::isnan(overtime_probability(-1,.25,NBA_SD,NBA_TIE)));      // margin cannot be negative
  CHECK(std::isnan(overtime_probability(0,.25,0,NBA_TIE)));            // no scale, no forecast
  CHECK(std::isnan(overtime_probability(0,1.5,NBA_SD,NBA_TIE)));       // more than a game left
  CHECK(overtime_probability(0,0,NBA_SD,NBA_TIE)==1.);                 // level at the horn
  CHECK(overtime_probability(3,0,NBA_SD,NBA_TIE)==0.);                 // decided at the horn
  // The measured number this replaces a constant with: level entering the last regulation
  // period, the NBA went to overtime 12.6% of the time against a 4.8% season rate.
  double late_level=overtime_probability(0,.25,NBA_SD,NBA_TIE);
  CHECK(late_level>.11&&late_level<.15);
  CHECK(late_level>2*.0481);                                           // the whole point: >2x base
  // Monotone in the margin, and a lead is worth more the less time is left to erase it.
  CHECK(overtime_probability(0,.25,NBA_SD,NBA_TIE)>overtime_probability(6,.25,NBA_SD,NBA_TIE));
  CHECK(overtime_probability(6,.25,NBA_SD,NBA_TIE)>overtime_probability(12,.25,NBA_SD,NBA_TIE));
  CHECK(overtime_probability(8,.25,NBA_SD,NBA_TIE)<overtime_probability(8,.75,NBA_SD,NBA_TIE));
  // Early on, a level game says little: the margin still to come swamps the one on the board.
  CHECK(overtime_probability(0,1,NBA_SD,NBA_TIE)<late_level);

  // And it moves the number that matters. A level game late, two points under the line: pricing
  // overtime at the season base rate understates the over against pricing it off the margin.
  // The line has to sit above the scoring still expected in regulation, or the over is already
  // decided and overtime cannot move it. 214 with 6% of regulation left projects about 228.
  const double line=232.5,scored=214;
  double base_rate=basketball_total_over(line,scored,.94,226.83,19.71,.0481,24.07,8.59);
  double conditional=basketball_total_over(line,scored,.94,226.83,19.71,
                                           overtime_probability(0,.06,NBA_SD,NBA_TIE),24.07,8.59);
  CHECK(base_rate>.1&&base_rate<.3);          // the constant leaves it a long shot
  CHECK(conditional>base_rate+.10);           // conditioning on a level game roughly doubles it
  CHECK(conditional<1.);
  std::cout<<"PASS models: causal history, degenerate models, next-snapshot entries, fee-inclusive cap, depth-limited exits, stale/skewed quotes, restart, totals boundaries, comeback gates, conditional overtime\n";
}
