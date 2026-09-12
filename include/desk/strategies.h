#pragma once
// Concrete strategies. Each one is a hypothesis with a data source; see docs/hypotheses.md.
#include <deque>
#include <map>
#include <set>
#include <string>

#include "desk/http.h"
#include "desk/models/terminal_price.h"
#include "desk/reference_feed.h"
#include "desk/strategy.h"
#include "desk/wxdist.h"

namespace desk {

// 1. Your own number. Enter p/sigma on the dashboard; the engine prices, sizes, executes.
class ManualFair : public Strategy {
 public:
  std::string name() const override { return "manual_fair"; }
  std::string category() const override { return "*"; }
  bool covers(const Market& m, const Series&) const override { return fairs_.count(m.ticker) > 0; }
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override { return "fair values you enter on the dashboard"; }
  double default_market_weight() const override { return 0.0; }   // it is your number, not a guess

 private:
  std::map<std::string, FairValue> fairs_;
};

// 1b. The other venue's price. A Kalshi game market that also trades on Polymarket US takes its
//     fair from the Polymarket book (deep, 0.005 tick): a market, not a model, so it is never
//     research-only and is not blended toward the Kalshi mid. Pairs come from PolyDesk's matcher
//     (poly_pairs / poly_markets in the store).
class XVenue : public Strategy {
 public:
  std::string name() const override { return "xvenue"; }
  std::string category() const override { return "Sports"; }
  bool covers(const Market& m, const Series&) const override { return pairs_.count(m.ticker) > 0; }
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  std::string describe() const override { return "Polymarket US book as the fair for matched game markets (" + std::to_string(pairs_.size()) + " matched)"; }
  double default_market_weight() const override { return 0.0; }   // a market, not a model

 private:
  struct Ref { std::string slug; bool flipped = false; double bid = 0, ask = 0; long long quote_ts = 0; };
  std::map<std::string, Ref> pairs_;
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
  // Successful data clocks, separate from request/backoff clocks. Diagnostics for the
  // hourly/daily research workspace; the 15-minute pricing path is unchanged.
  std::map<std::string, long long> spot_data_ts_, rv_data_ts_, rv_span_;
  std::map<std::string, int> rv_samples_;
  std::map<std::string, int> spot_fails_;        // consecutive misses per coin
  std::map<std::string, bool> backfilled_;       // realized-vol history seeded from candles once per run
  std::string candles_url_;                      // template with {sym}; empty disables the backfill
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
    // The NWS daily climate product Kalshi's rules name for this city (CLINYC, CLIMDW, ...).
    // Empty means nobody has read the rules for this series and confirmed it: such a series is
    // research-only, because we would be modelling a target we have not identified.
    std::string cli_product;
    // Settled value minus the running maximum of the hourly series, in whole F. Not a constant of
    // nature: it is the chance the hourly series missed the day's peak, so it belongs to the
    // station's reporting frequency. See docs/settlement.md.
    std::vector<std::pair<int, double>> settle_shift;
  };
  struct DayForecast {
    std::string date;
    double fc_max = -999, fc_min = 999;   // same day: max/min over the REMAINING hourly periods
    double fc_day_max = -999, fc_day_min = 999;  // whole-day hourly forecast extremes
    double obs_max = -999, obs_min = 999;  // running extremes; never move backwards within a day
    bool have_obs = false;
    bool day_complete = false;  // fetched after the model's civil day; not proof of settlement finality
    double t_now = -999;       // latest observation
    long long t_now_ts = 0;
    double fc_at_now = -999;   // hourly forecast for the hour of the latest observation
    double resid = 0;          // t_now - fc_at_now: how far the day is running from forecast
    bool have_resid = false;
    // The hourly forecast path for this date, (period start, temperature F), in time order.
    // The model used to keep only max(hourly) and price a normal around it. The maximum of the
    // expected hourly values is not the expected maximum - whenever several hours sit near the
    // top, whichever one runs warm becomes the day's high - and that gap cannot be recovered
    // from a single number afterwards. wxdist::simulate_extreme needs the path itself.
    std::vector<std::pair<long long, double>> hourly;
    // The hours that have not finished yet: the only ones that can still set the day's peak.
    std::vector<double> remaining(long long now) const;
    int offset_seconds = 0;
    long long fetched = 0;     // refresh scheduling; can be a retry timestamp after failure
    long long forecast_fetched_ts = 0;  // successful forecast response, separate from retry scheduling
    long long forecast_issued_ts = 0;
    long long observation_fetched_ts = 0, observation_attempt_ts = 0;
    bool observation_fetch_ok = false;
    std::string observation_error;
    // Open-Meteo ensemble (ECMWF 51 members): daily max/min across members.
    double ens_mean_max = -999, ens_sd_max = 0, ens_mean_min = 999, ens_sd_min = 0;
    int ens_members = 0;
    long long ens_fetched = 0;
  };
  // Exposed for tests: probability that the settled integer temperature falls in the market's bracket.
  static double bracket_prob(const Market& m, double mu, double sd, double observed_max, bool have_obs, bool low);
  // WX1: the station-day priced from a simulated temperature trajectory rather than a normal
  // placed around the highest hourly point forecast. Every bracket of a ladder reads off ONE
  // distribution, so the ladder sums to one because of how it was built rather than by accident.
  // Recorded beside WX0 under the strategy name "weather_wx1"; it does not produce the Fair the
  // desk trades on until the horse race in reports/wx_versions.md says it should.
  struct Wx1Dist {
    long long fc_ts = 0, obs_ts = 0, hour = 0;   // what it was built from; rebuilt when these move
    MaxDistribution mid, lo, hi;                 // centre, and the same day at 0.7x / 1.3x day sigma
    bool ok = false;
  };
  static double read_bracket(const MaxDistribution& dist, const Market& m);
  void set_day_forecast(const std::string& series, const DayForecast& d) { fc_[series + "|" + d.date] = d; }
  void set_clock(long long now) { now_override_ = now; }

 private:
  void refresh_station(Station& st, const std::string& date, long long now);
  void refresh_ensemble(Station& st, long long now);
  Http http_;
  std::map<std::string, Station> stations_;                    // series -> station
  std::map<std::string, DayForecast> fc_;                       // series|date -> forecast
  std::map<std::string, Wx1Dist> wx1_;                          // series|date -> simulated ladder
  const Wx1Dist* wx1_for(const Station& st, const DayForecast& d, const std::string& key, long long now);
  double sigma_hour_ = 1.0;    // hour-to-hour wiggle around the day's shared bias
  double rho_ = 0.7;           // its persistence between adjacent hours
  int wx1_draws_ = 20000;
  std::map<std::string, long long> ens_ts_;                     // series -> last ensemble fetch
  int refresh_seconds_ = 600;
  int ens_refresh_seconds_ = 1800;
  int forecast_max_age_seconds_ = 1800, forecast_issue_max_age_seconds_ = 21600;
  int observation_max_age_seconds_ = 5400, ensemble_max_age_seconds_ = 21600;
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
// Where a match is in its lifecycle. Never guessed: a missing or unrecognised provider status
// is Unknown, and Unknown is not Prematch. Rate fitting keys off this, and the difference
// between "before kick-off" and "we do not know" is the difference between a clean calibration
// and one that hands the goals already in the book to the scoring rates.
enum class MatchPhase { Unknown = 0, Prematch, FirstHalf, Halftime, SecondHalf, Stoppage, Finished, Suspended };
const char* phase_name(MatchPhase p);
MatchPhase phase_parse(const std::string& n);   // round-trips phase_name
bool phase_started(MatchPhase p);               // kick-off has happened
bool phase_in_play(MatchPhase p);               // the clock is running
MatchPhase phase_from_espn(const std::string& state, const std::string& status_name, int period);

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
    MatchPhase phase = MatchPhase::Unknown;
    long long phase_ts = 0;   // when this phase was first observed
    long long data_ts = 0;    // age of the provider data behind it
    bool from_disk = false;   // restored across a restart, not yet re-observed this process
  };
  struct EventInfo {
    std::string event_ticker, series, home, away, date;  // names parsed from the rules text
    std::string tie_ticker, home_ticker, away_ticker;
    double lam_home = 0, lam_away = 0;  // scoring rates; 0 = not fitted
    bool lam_default = false;           // fallback rates: no market fit at all
    long long lam_ts = 0;
    // Fit provenance. A mid-match calibration reproduces the book it was fitted to, so it holds
    // no independent edge until the match moves away from the snapshot recorded here.
    std::string lam_calibration;        // "" | "prematch" | "inplay"
    MatchPhase lam_phase = MatchPhase::Unknown;
    double lam_cal_minute = 0;
    int lam_cal_home = 0, lam_cal_away = 0;
    double lam_cal_ph = 0, lam_cal_pd = 0, lam_cal_pa = 0;  // the calibration prices
    double lam_residual = 0;
    bool lam_converged = false;
  };
  static constexpr const char* MODEL_VERSION = "inplay-poisson-2";
  // Exposed for tests and the research CLI.
  static const std::map<std::string, std::string>& default_leagues();
  static bool parse_teams(const std::string& rules, std::string& home, std::string& away);
  static std::string market_side(const Market& m, const std::string& home, const std::string& away);
  static double name_match(const std::string& a, const std::string& b);
  static double parse_clock(const std::string& clock, int period, const std::string& status_name);
  double late_factor(double minute) const { return 1.0 + late_boost_ * std::min(1.0, minute / 90.0); }

 private:
  void fetch_league(const std::string& slug, long long now);
  void load_match_state(Store& store);   // restore phases across a restart
  void save_match_state(Store& store, const std::string& ev, const MatchState& s);
  void fit_rates(const std::string& ev, EventInfo& e, const std::vector<const Market*>& ms,
                 Store& store, long long now);
  Http http_;
  bool state_loaded_ = false;
  std::map<std::string, std::string> leagues_;         // Kalshi series -> ESPN slug
  std::map<std::string, EventInfo> events_;             // event_ticker -> info
  std::map<std::string, MatchState> states_;            // event_ticker -> live state
  std::map<std::string, json> espn_cache_;              // slug -> scoreboard json
  std::map<std::string, long long> espn_ts_;            // slug -> last fetch attempt
  std::map<std::string, long long> espn_data_ts_;       // slug -> last successful fetch (data age)
  std::map<std::string, long long> last_log_;
  double lambda_home_ = 1.45, lambda_away_ = 1.15, red_self_ = 0.65, red_opp_ = 1.25, late_boost_ = 0.2,
         t_eff_ = 95.0, rho_ = -0.13;
  double fit_tolerance_ = 0.02;      // max |model - calibration price| for a fit to count as converged
  double inplay_min_progress_ = 5.0; // minutes of clock before an in-play fit carries own information
  long long stale_seconds_ = 90;     // provider data older than this cannot condition a fit
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
    std::string clock, last_play_id;
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
  static std::string state_signature(const GameState& game);
  static bool accept_win_probability(const json& summary, GameState& game, long long now);

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
// The short-duration commodity contracts (session 16). One terminal distribution for the
// underlying, every contract on it priced from that distribution, and the settlement reference read
// from the feed the contract actually settles on (docs/commodity-settlement.md). One instance per
// underlying: gold, silver, WTI, natural gas.
class CommodityTerminal : public Strategy {
 public:
  struct Underlying {
    std::string key;                          // gold | silver | wti | natgas
    std::string model_name;                   // commodity_terminal_gold
    std::vector<std::string> series;          // KXGOLD15M, and any other series on the same feed
    std::vector<std::string> symbol_patterns; // how to find the feed's own name for it
    std::vector<std::string> rules_tokens;    // what the contract's rules must say for it to be ours
    std::string external_symbol;              // a futures cross-check, diagnostic only
    double strike_tolerance = 0.02;           // how close our candle must be to the published strike
  };
  CommodityTerminal(Underlying u, ReferenceFeed* feed, const json& params, const std::string& user_agent);

  std::string name() const override { return u_.model_name; }
  std::string category() const override { return "Commodities"; }
  std::string model_version() const override { return "v1"; }
  bool covers(const Market& m, const Series& s) const override;
  void prepare(const std::vector<Market>& markets, Store& store) override;
  std::optional<Fair> fair(const Market& m, const Series& s, Store& store) override;
  // The settlement check is empirical: the rules must name this feed, and the feed must have
  // reproduced the exchange's own published strike recently.
  std::optional<bool> settlement_verified(const Market& m) const override;
  std::string describe() const override;
  // Not blended toward the mid. The question this model exists to answer is whether Kalshi's
  // price is wrong about the terminal distribution; shrinking its answer halfway to that price
  // would make the answer partly a copy of the thing being tested, and would leave the published
  // probability describing neither the distribution in its own diagnostics nor the market.
  double default_market_weight() const override { return 0.0; }

  // One window's estimate, with no opinion about whether it may be traded: the distribution, the
  // volatility behind it, the payoff, and the probability that falls out. `fair()` wraps this in
  // the tradability rules; the calibration recorder needs the same number at moments those rules
  // refuse, which is most of what the research ladder is about.
  struct Estimate {
    TerminalPayoff payoff;
    TerminalDistribution dist;
    VolEstimate vol;
    ReferenceFeed::Value value;
    long long close_ts = 0, feed_age = 0;
    double seconds_to_close = 0, strike = 0, p = 0, band = 0, sigma_move = 0;
  };
  std::optional<Estimate> estimate(const Market& m, long long now);

  // The tolerance the CONTRACT states ("rounded to the nearest N decimal places"), which is what
  // "within a tick" means on this underlying. nullopt when the rules do not say, and then the
  // per-underlying fallback above is used.
  static std::optional<double> stated_tolerance(const Market& m);
  double tolerance_for(const Market& m) const;

  // Exposed for tests.
  static std::vector<Underlying> default_underlyings();
  const std::string& symbol() const { return symbol_; }
  void set_symbol_for_tests(const std::string& s) { symbol_ = s; }
  // The futures cross-check is a diagnostic and nothing else. A test sets it here so that it can
  // check the model prices the same number whatever the futures say - and prices nothing at all
  // when the settlement reference is missing, however good the futures print is.
  void set_external_for_tests(double price, long long ts) { external_price_ = price; external_price_ts_ = ts; external_ts_ = ts + 86400; }

 private:
  bool rules_name_this_feed(const Market& m) const;
  void check_replication(const std::vector<Market>& markets, long long now);
  void refresh_external(long long now, Store& store);
  // One row per window per checkpoint, written as the window runs down. There is no way to
  // reconstruct this afterwards: the probability at ten seconds to close depends on a reference
  // value and a volatility estimate as they stood at that instant, and nothing else recorded on
  // this desk keeps either. If it is not written now, the four hundred settlements arrive with
  // nothing to score.
  void record_checkpoints(const std::vector<Market>& markets, Store& store, long long now);

  // seconds-to-close thresholds, largest first
  std::vector<double> checkpoints_{840, 600, 300, 120, 60, 30, 10};
  std::map<std::string, std::set<int>> checkpoints_done_;   // ticker -> indices already written

  Underlying u_;
  ReferenceFeed* feed_ = nullptr;
  VolSettings vol_;
  std::string symbol_;                  // the feed's own name for this underlying, once seen
  Http http_;
  std::string external_url_;
  bool external_enabled_ = true;
  int external_every_seconds_ = 60;
  long long external_ts_ = 0, external_fails_ = 0;
  double external_price_ = 0;
  long long external_price_ts_ = 0;
  std::string external_error_;
  double min_seconds_to_close_ = 20, max_seconds_to_close_ = 7200;
  long long replication_max_age_ = 7200;
  // Consecutive windows whose published strike this feed has reproduced, before the model's
  // probabilities are trusted at all. One match can be luck on a quiet minute; fifty consecutive
  // cannot. Until then settlement_verified is false and the router keeps the model in RESEARCH.
  long long windows_required_ = 50;
};

// Where a commodity model stands economically, computed from what has actually been recorded.
//
// The ladder is deliberately one-way and mostly NOT computable: a desk can prove to itself that a
// feed replicates, and that it has enough settled windows, and even that its Brier beats the
// market's. It cannot promote itself to trading. The last two states are set by a person.
//
//   SETTLEMENT_UNVERIFIED  the feed has not reproduced the exchange's strike often enough
//   DATA_ACCUMULATING      verified, but not enough settled windows to say anything
//   CALIBRATED_NO_ALPHA    enough windows, and the model does not beat the takeable price
//   MISPRICING_DETECTED    it does beat it, by more than the interval
//   FAILED_COST            it beats it, and the fees eat the difference
//   PAPER_CANDIDATE        a person decided to paper-trade it
//   LIVE_CANDIDATE         a person decided to risk money on it
//
// The analytical states come from research/commodity_terminal.py writing its verdict back; the
// first two the desk works out for itself, and nothing here ever writes the last two.
nlohmann::json commodity_model_status(Store& store, const std::string& model, long long windows_required = 50,
                                      long long settlements_required = 400);

class CommodityFutures : public Strategy {
 public:
  std::string name() const override { return "commodity_futures"; }
  std::string category() const override { return "Commodities"; }
  // Superseded by CommodityTerminal for the series it covers; kept for the daily/monthly
  // contracts nothing prices yet.
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
