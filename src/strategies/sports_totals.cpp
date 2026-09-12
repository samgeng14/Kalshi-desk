#include "desk/sports_models.h"
#include "desk/strategies.h"
#include "desk/log.h"        // now_s(); timeutil.h has parse_iso/iso_of but not the clock
#include "desk/timeutil.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace desk {
namespace {
double invalid(){return std::numeric_limits<double>::quiet_NaN();}
bool inputs(double line,int points,double elapsed,double mean,double sd){return std::isfinite(line)&&line>=0&&std::fabs(line-std::floor(line)-.5)<1e-8&&points>=0&&std::isfinite(elapsed)&&elapsed>=0&&elapsed<=1&&std::isfinite(mean)&&mean>0&&std::isfinite(sd)&&sd>0;}
double normal_tail(double threshold,double mean,double sd){if(sd<=1e-10)return mean>threshold?1:0;return .5*std::erfc((threshold-mean)/(sd*std::sqrt(2.)));}
}
double soccer_total_over(double line,int goals,double elapsed,double prior_mean,double prior_sd){
  if(!inputs(line,goals,elapsed,prior_mean,prior_sd)||prior_mean>20||line>100)return invalid();
  const int needed=static_cast<int>(std::floor(line))-goals;
  if(needed<0)return 1;if(elapsed>=1)return 0;
  const double remaining=1-elapsed,var=prior_sd*prior_sd;
  double cdf=0,prob=0;
  if(var<=prior_mean+1e-8){double mu=prior_mean*remaining;prob=std::exp(-mu);cdf=prob;for(int n=1;n<=needed;++n){prob*=mu/n;cdf+=prob;}}
  else{
    double alpha=prior_mean*prior_mean/(var-prior_mean),rate=alpha/prior_mean+elapsed,shape=alpha+goals;
    double p=rate/(rate+remaining);prob=std::pow(p,shape);cdf=prob;
    for(int n=1;n<=needed;++n){prob*=(n-1+shape)/n*(1-p);cdf+=prob;}
  }
  return std::clamp(1-cdf,0.,1.);
}
double basketball_total_over(double line,int points,double elapsed,double prior_mean,double prior_sd,double ot,double otmean,double otsd){
  if(!inputs(line,points,elapsed,prior_mean,prior_sd)||!std::isfinite(ot)||ot<0||ot>1||!std::isfinite(otmean)||!std::isfinite(otsd)||otmean<0||otsd<0||(ot>0&&(otmean<=0||otsd<=0)))return invalid();
  if(points>line)return 1;
  const double f=1-elapsed,w=elapsed/(elapsed+.5);
  const double pace=elapsed>0?points/elapsed:prior_mean;
  const double mu=f*((1-w)*prior_mean+w*pace),sd=prior_sd*std::sqrt(f);
  const double reg=normal_tail(line-points,mu,sd),extra=normal_tail(line-points,mu+otmean,std::hypot(sd,otsd));
  return std::clamp((1-ot)*reg+ot*extra,0.,1.);
}
double overtime_probability(double margin,double fraction_left,double margin_sd,double tie_width){
  if(!std::isfinite(margin)||margin<0||!std::isfinite(fraction_left)||fraction_left<0||fraction_left>1||
     !std::isfinite(margin_sd)||margin_sd<=0||!std::isfinite(tie_width)||tie_width<=0)return invalid();
  // No regulation left: the margin on the board is the one that decides it.
  if(fraction_left<=0)return margin<1e-9?1.:0.;
  const double sd=margin_sd*std::sqrt(fraction_left);
  const double density=std::exp(-.5*(margin/sd)*(margin/sd))/(sd*std::sqrt(2*M_PI));
  return std::clamp(tie_width*density,0.,1.);
}
json sports_comeback_candidate(double price,double earlier,double wp,double fee,double left,bool fresh){
  json out={{"eligible",false},{"model","sports_comeback"},{"reason","Needs fresh game state, a dip, and independent positive value"},{"live_enabled",false}};
  if(!fresh||!std::isfinite(price)||!std::isfinite(earlier)||!std::isfinite(wp)||!std::isfinite(fee)||!std::isfinite(left)||fee<0||wp<0||wp>1||left<0||left>1)return out;
  double edge=wp-price-fee;
  out.update({{"net_edge",edge},{"p_win",wp},{"target_price",std::min(.95,2*price)},
    {"eligible",price>=.07&&price<=.30&&earlier-price>=.04&&left>=.25&&edge>=.05},
    {"reason","Rebound candidate only: 4c dip, 25%+ regulation remaining, independent value exceeds entry fees by 5c. Double-price target is not a win-probability forecast."}});
  return out;
}
bool SportsTotals::covers(const Market& m,const Series&) const {
  return params_.is_object()&&params_.contains("markets")&&params_["markets"].is_object()&&params_["markets"].contains(m.ticker);
}
std::optional<Fair> SportsTotals::fair(const Market& m,const Series&,Store& store){
  if(!covers(m,Series{}))return std::nullopt;
  // Priors live once per league and the market entry names its league, so a line, a rules string
  // and a league is all a new contract needs. Anything the market restates wins, which is what
  // lets one odd contract differ without editing the league everyone else uses.
  json c = params_["markets"][m.ticker];
  if(c.is_object()&&c.contains("league")&&params_.contains("leagues")&&params_["leagues"].is_object()){
    const auto& lp=params_["leagues"];const auto key=c.value("league","");
    if(lp.contains(key)&&lp[key].is_object()){json merged=lp[key];merged.update(c);c=merged;}
    else return std::nullopt;   // a named league with no priors is a typo, not a default
  }
  if(!c.is_object()||!c.value("rules_verified",false)||m.rules_primary.empty()||c.value("rules_primary","")!=m.rules_primary)return std::nullopt;
  auto kind=c.value("kind","");auto event=c.value("event_ticker","");const auto side=c.value("yes_means","");
  auto now=now_s(),start=c.value("start_ts",0LL),prior_ts=c.value("prior_ts",0LL);
  if(event.empty()||(side!="over"&&side!="under")||start<=0||prior_ts<=0||prior_ts>=start||prior_ts>now)return std::nullopt;
  double elapsed=0;int points=0;double margin=-1;long long source_ts=prior_ts;
  if(now>=start){
    if(kind=="soccer"){
      const auto rows=store.query("SELECT * FROM match_state WHERE event_ticker=?",{event});if(rows.empty())return std::nullopt;const auto& r=rows[0];
      source_ts=r.value("data_ts",0LL);const auto phase=r.value("phase","");
      if(r.value("state","")=="pre"||phase=="post"||phase=="final"||r.value("period",0)>2||r.value("red_home",0)>0||r.value("red_away",0)>0)return std::nullopt;
      points=r.value("home_score",0)+r.value("away_score",0);margin=std::fabs(r.value("home_score",0)-r.value("away_score",0));
      elapsed=std::clamp(r.value("minute",0.)/c.value("regulation_minutes",90.),0.,1.);
    }else if(kind=="basketball"||kind=="football"){
      const auto rows=store.query("SELECT * FROM ext_wp WHERE event_ticker=? ORDER BY ts DESC LIMIT 1",{event});if(rows.empty())return std::nullopt;const auto& r=rows[0];
      source_ts=r.value("ts",0LL);auto lg=SportsLive::default_leagues().find(c.value("game_series",""));
      // The configured kind must equal the league map's sport. That equality is the whole guard
      // against a football total being priced on basketball periods or the wrong overtime rule.
      if(lg==SportsLive::default_leagues().end()||lg->second.sport!=kind||r.value("period",0)>lg->second.periods||r.value("state","")=="pre"||r.value("state","")=="post")return std::nullopt;
      points=r.value("home_score",0)+r.value("away_score",0);margin=std::fabs(r.value("home_score",0)-r.value("away_score",0));
      elapsed=1-SportsLive::frac_left(lg->second,r.value("period",0),r.value("clock",""),r.value("state",""));
    }else return std::nullopt;
    if(source_ts<=0||source_ts>now||now-source_ts>90)return std::nullopt;
  }
  const double line=c.value("line",-1.),mean=c.value("prior_total",0.),sd=c.value("prior_sd",0.);
  bool ot=c.value("includes_overtime",false);double p=invalid();
  if(kind=="soccer"&&!ot)p=soccer_total_over(line,points,elapsed,mean,sd);
  double p_ot=0;
  // Football and basketball share the machinery: a normal for the scoring still to come, plus an
  // overtime mixture. They do not share numbers - every prior, the tie width and the overtime
  // distribution come from the league block, and overtime adds 24.1 points in the NBA against
  // 5.2 in the NFL.
  if(kind=="basketball"||kind=="football"){
    if(ot&&(!c.contains("overtime_mean")||!c.contains("overtime_sd")))return std::nullopt;
    if(ot){
      // Live margin plus the fitted tie width beats a constant whenever the game is on; before
      // tip-off there is no margin to condition on, so the season base rate is the honest number.
      const bool conditional = margin>=0 && c.contains("margin_sd") && c.contains("ot_tie_width");
      p_ot = conditional ? overtime_probability(margin,1-elapsed,c.value("margin_sd",0.),c.value("ot_tie_width",0.))
                         : c.value("overtime_probability",invalid());
      if(!std::isfinite(p_ot))return std::nullopt;   // neither a fit nor a base rate was configured
    }
    p=basketball_total_over(line,points,elapsed,mean,sd,p_ot,ot?c.value("overtime_mean",0.):0.,ot?c.value("overtime_sd",0.):0.);
  }
  if(!std::isfinite(p))return std::nullopt;
  Fair f;f.p=side=="over"?p:1-p;f.p_raw=f.p;f.sigma=.12;f.cluster=event;
  f.research_only=true;f.research_reason="Totals baseline needs league-specific out-of-sample calibration before systematic live use";
  f.note=kind+" conditional total baseline: "+std::to_string(points)+" scored, elapsed "+std::to_string(elapsed)+", line "+std::to_string(line)+"; explicit contract mapping";
  f.inputs={{"valid",true},{"auto_eligible",false},{"source_ts",source_ts},{"event_ticker",event},{"elapsed",elapsed},{"current_total",points},{"line",line},{"prior_ts",prior_ts},{"includes_overtime",ot},{"margin",margin},{"p_overtime",p_ot}};
  return f;
}
} // namespace desk
