#pragma once
// Concrete strategies. Each one is a hypothesis with a data source; see docs/hypotheses.md.
#include <deque>
#include <map>
#include <string>

#include "desk/http.h"
#include "desk/strategy.h"

namespace desk {

// 1. Your own number. Enter p/sigma on the dashboard; the engine prices, sizes, executes.
class ManualFair : public Strategy {
 public:
  std::string name() const override { return "manual_fair"; }
  std::string category() const override { return "*"; }
  bool covers(const Market& m, const Series& s) const override { return fairs_.count(m.ticker) > 0; }
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override { return "fair values you enter on the dashboard"; }

 private:
  std::map<std::string, FairValue> fairs_;
};

// 2. Crypto hourly/daily brackets priced off live spot with a lognormal model.
//    Hypothesis: bracket quotes lag spot; a live-spot fair finds stale prices after fees.
class CryptoLognormal : public Strategy {
 public:
  explicit CryptoLognormal(const json& params, const std::string& user_agent);
  std::string name() const override { return "crypto_lognormal"; }
  std::string category() const override { return "Crypto"; }
  bool covers(const Market& m, const Series& s) const override;
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override;

  // Exposed for tests.
  static std::string symbol_for(const std::string& series_ticker);
  double vol_for(const std::string& symbol) const;

 private:
  Http http_;
  std::map<std::string, double> spot_;           // symbol -> last spot
  std::map<std::string, long long> spot_ts_;
  std::map<std::string, int> spot_fails_;        // consecutive misses per coin
  std::map<std::string, double> vol_cfg_;        // symbol -> annual vol from config
  std::map<std::string, double> vol_realized_;   // symbol -> realized annual vol (if enough data)
  std::map<std::string, double> vol_dvol_;       // symbol -> Deribit DVOL (30d implied), annualized
  std::map<std::string, long long> dvol_ts_;
  double vol_band_ = 0.25;                       // +-25% vol uncertainty -> sigma on p
  double basis_ = 0.0004;                        // spot-vs-settlement-index basis (log sd)
  long long max_horizon_seconds_ = 36 * 3600;    // do not price markets further out than this
  std::string dvol_url_;                         // template with {sym}; empty disables
  std::string spot_url_;                         // template with {sym}
  int refresh_seconds_ = 20;
};

// 3. Daily high/low temperature brackets priced off the NWS hourly forecast plus today's
//    observations. Hypothesis: on the settlement day, observed running max + remaining
//    forecast beats bracket prices, especially in the afternoon.
class WeatherNws : public Strategy {
 public:
  explicit WeatherNws(const json& params, const std::string& user_agent);
  std::string name() const override { return "weather_nws"; }
  std::string category() const override { return "Climate and Weather"; }
  bool covers(const Market& m, const Series& s) const override;
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override;

  struct Station {
    std::string series, obs_station, hourly_url, tz;  // tz: IANA zone from the NWS points endpoint
    double lat = 0, lon = 0, bias_f = 0.5, sigma_f = 2.5;
    bool low = false;  // series is a daily LOW market
  };
  struct DayForecast {
    std::string date;
    double fc_max = -999, fc_min = 999;   // same day: max/min over the REMAINING hourly periods
    double fc_day_max = -999, fc_day_min = 999;  // whole-day hourly forecast extremes
    double obs_max = -999, obs_min = 999;  // running extremes; never move backwards within a day
    bool have_obs = false;
    bool day_complete = false;  // observations were fetched after the local day ended: extremes are final
    double t_now = -999;       // latest observation
    long long t_now_ts = 0;
    double fc_at_now = -999;   // hourly forecast for the hour of the latest observation
    double resid = 0;          // t_now - fc_at_now: how far the day is running from forecast
    bool have_resid = false;
    int offset_seconds = 0;
    long long fetched = 0;
    // Open-Meteo ensemble (ECMWF 51 members): daily max/min across members.
    double ens_mean_max = -999, ens_sd_max = 0, ens_mean_min = 999, ens_sd_min = 0;
    int ens_members = 0;
    long long ens_fetched = 0;
  };
  // Exposed for tests: probability that the settled integer temperature falls in the market's bracket.
  static double bracket_prob(const Market& m, double mu, double sd, double observed_max, bool have_obs, bool low);
  void set_day_forecast(const std::string& series, const DayForecast& d) { fc_[series + "|" + d.date] = d; }
  void set_clock(long long now) { now_override_ = now; }

 private:
  void refresh_station(Station& st, const std::string& date, long long now);
  void refresh_ensemble(Station& st, long long now);
  Http http_;
  std::map<std::string, Station> stations_;                    // series -> station
  std::map<std::string, DayForecast> fc_;                       // series|date -> forecast
  std::map<std::string, long long> ens_ts_;                     // series -> last ensemble fetch
  int refresh_seconds_ = 600;
  int ens_refresh_seconds_ = 1800;
  bool same_day_trading_ = false;  // WX-1b/WX-1d: the market beat this model at every hour, 9am to midnight
  bool day_ahead_trading_ = false; // WX-1c: and at every decision point the day before
  double resid_k_ = 0.6;           // carry of today's forecast residual into the remaining hours
  double same_day_sd_floor_ = 2.0; // 1.5 was too tight on 2026-09-05 (AUS ran +4F over forecast)
  double ens_weight_ = 0.5;     // weight on ensemble mean vs NWS point forecast (day-ahead)
  double ens_sd_inflate_ = 1.6; // ensembles are under-dispersed
  std::string ens_model_ = "ecmwf_ifs025";
  long long now_override_ = 0;  // tests
};

// Placeholders for the remaining categories. Each returns nothing until its data source is
// wired in; they exist so the registry, config and dashboard already show the slots.
// 4. Soccer in-play: independent Poisson goals for the remaining minutes, rates fitted from the
//    pre-match Kalshi prices, adjusted for red cards and the late-game scoring bump.
//    Hypothesis SPORT-1: tied late, No-on-draw is underpriced. Logs every in-play state to
//    ext_sports so the sample builds whether or not anything trades.
class SportsInplay : public Strategy {
 public:
  explicit SportsInplay(const json& params, const std::string& user_agent);
  std::string name() const override { return "sports_inplay"; }
  std::string category() const override { return "Sports"; }
  bool covers(const Market& m, const Series& s) const override;
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override;

  struct MatchState {
    std::string espn_id, state, status_name, home, away, kickoff;
    double minute = 0;      // effective minute incl. stoppage, 0..95+
    int period = 0, home_score = 0, away_score = 0, red_home = 0, red_away = 0;
    double sot_home = 0, sot_away = 0, poss_home = 0;
    long long updated = 0;
  };
  struct EventInfo {
    std::string event_ticker, series, home, away, date;  // names parsed from the rules text
    std::string tie_ticker, home_ticker, away_ticker;
    double lam_home = 0, lam_away = 0;  // scoring rates; 0 = not fitted
    bool lam_default = false;           // fallback rates: no market fit at all
    bool lam_inplay_fit = false;        // fitted from a live market (desk started mid-match)
    long long lam_ts = 0;
  };
  // Exposed for tests and the research CLI.
  static const std::map<std::string, std::string>& default_leagues();
  static bool parse_teams(const std::string& rules, std::string& home, std::string& away);
  static std::string market_side(const Market& m, const std::string& home, const std::string& away);
  static double name_match(const std::string& a, const std::string& b);
  static double parse_clock(const std::string& clock, int period, const std::string& status_name);
  double late_factor(double minute) const { return 1.0 + late_boost_ * std::min(1.0, minute / 90.0); }

 private:
  void fetch_league(const std::string& slug, long long now);
  Http http_;
  std::map<std::string, std::string> leagues_;         // Kalshi series -> ESPN slug
  std::map<std::string, EventInfo> events_;             // event_ticker -> info
  std::map<std::string, MatchState> states_;            // event_ticker -> live state
  std::map<std::string, json> espn_cache_;              // slug -> scoreboard json
  std::map<std::string, long long> espn_ts_;            // slug -> last fetch attempt
  std::map<std::string, long long> espn_data_ts_;       // slug -> last successful fetch (data age)
  std::map<std::string, long long> last_log_;
  double lambda_home_ = 1.45, lambda_away_ = 1.15, red_self_ = 0.65, red_opp_ = 1.25, late_boost_ = 0.2,
         t_eff_ = 95.0, rho_ = -0.13;
  int refresh_seconds_ = 15, log_seconds_ = 30;
};

// 5. US team sports in-game: ESPN's live win probability where ESPN publishes one (NFL, NBA,
//    WNBA, D1 basketball/football, MLB, NHL), a lead/time-remaining model where it only has the
//    score (Stern 1994: P(home) = Phi((lead + mu*f) / (sigma*sqrt(f))), f = fraction of the game
//    left, mu = pre-game expected margin from the spread or the Kalshi pre-game price), and the
//    spread alone before tip-off. Logs every live state to ext_wp. Hypothesis SPORT-4: a public
//    live win probability quoted as a maker in wide-spread game markets, with reduce-only exits,
//    earns the spread without being picked off more often than it is paid.
class SportsLive : public Strategy {
 public:
  explicit SportsLive(const json& params, const std::string& user_agent);
  std::string name() const override { return "sports_live"; }
  std::string category() const override { return "Sports"; }
  bool covers(const Market& m, const Series& s) const override;
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override;

  struct League {
    std::string sport, league;   // ESPN path pieces
    double sigma_game = 12;      // final-margin sd in points (lead/time model)
    int periods = 4;
    double minutes_per_period = 12;
    std::vector<std::string> groups;  // ESPN "groups" filters (college), empty = none
    bool leadtime = true;        // lead/time model makes sense (not for baseball/hockey)
  };
  struct GameState {
    std::string espn_id, league, state, detail, home_abbr, away_abbr, home_name, away_name, start_utc;
    int period = 0, home_score = 0, away_score = 0;
    std::string clock;
    double wp_home = -1, tie = 0;   // ESPN win probability (-1 = none yet)
    double spread_home = 0;         // pre-game expected home margin (positive = home favored)
    bool have_spread = false;
    long long updated = 0;          // last scoreboard fetch that contained this game
    long long wp_ts = 0;            // age of wp_home (set only when a summary fetch succeeded)
    long long wp_attempt_ts = 0;    // pacing of summary fetches
    std::string last_sig;           // period|score|clock signature at the last successful WP fetch
  };
  struct EventInfo {
    std::string event_ticker, series, date, code_a, code_b, name_a, name_b;
    std::string ticker_a, ticker_b;  // markets: "-<code>"
    std::string espn_id;             // matched game
    bool home_is_a = false;          // which Kalshi market is the ESPN home team
    double mu_home = 0;              // pre-game expected margin used when ESPN has no spread
    bool mu_from_market = false;
  };
  // Exposed for tests.
  static double leadtime_prob(double lead_home, double mu_home, double frac_left, double sigma_game);
  static double frac_left(const League& lg, int period, const std::string& clock, const std::string& state);
  static bool parse_us_teams(const std::string& rules, std::string& a, std::string& b);
  static double spread_to_prob(double spread_home, double sigma_game);
  static const std::map<std::string, League>& default_leagues();

 private:
  void fetch_scoreboard(const std::string& series, const League& lg, const std::string& date, long long now);
  void fetch_wp(const League& lg, GameState& g, long long now);
  Http http_;
  std::map<std::string, League> leagues_;               // Kalshi series -> ESPN league
  std::map<std::string, EventInfo> events_;              // event_ticker -> info
  std::map<std::string, GameState> games_;               // espn_id -> live state
  std::map<std::string, long long> sb_ts_;               // league|date -> last scoreboard fetch
  std::map<std::string, long long> last_log_;
  int sb_refresh_seconds_ = 20, wp_refresh_seconds_ = 30, log_seconds_ = 30;
  double sigma_wp_ = 0.04, sigma_leadtime_ = 0.07, sigma_pregame_ = 0.10;
};

class EconNowcast : public Strategy {
 public:
  std::string name() const override { return "econ_nowcast"; }
  std::string category() const override { return "Economics"; }
  bool covers(const Market&, const Series& s) const override { return s.category == "Economics"; }
  std::optional<Fair> fair(const Market&, const Series&, Store&) override { return std::nullopt; }
  std::string describe() const override { return "TODO: Cleveland Fed nowcast / consensus vs bracket"; }
};
class CommodityFutures : public Strategy {
 public:
  std::string name() const override { return "commodity_futures"; }
  std::string category() const override { return "Commodities"; }
  bool covers(const Market&, const Series& s) const override { return s.category == "Commodities"; }
  std::optional<Fair> fair(const Market&, const Series&, Store&) override { return std::nullopt; }
  std::string describe() const override { return "TODO: front-month futures + implied vol lognormal"; }
};
class IndexClose : public Strategy {
 public:
  std::string name() const override { return "index_close"; }
  std::string category() const override { return "Financials"; }
  bool covers(const Market&, const Series& s) const override { return s.category == "Financials"; }
  std::optional<Fair> fair(const Market&, const Series&, Store&) override { return std::nullopt; }
  std::string describe() const override { return "TODO: live index + VIX-implied vol lognormal"; }
};
class SciTech : public Strategy {
 public:
  std::string name() const override { return "scitech"; }
  std::string category() const override { return "Science and Technology"; }
  bool covers(const Market&, const Series& s) const override { return s.category == "Science and Technology"; }
  std::optional<Fair> fair(const Market&, const Series&, Store&) override { return std::nullopt; }
  std::string describe() const override { return "TODO: release-date base rates (manual_fair for now)"; }
};

}  // namespace desk
