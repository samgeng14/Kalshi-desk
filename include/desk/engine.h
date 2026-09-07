#pragma once
// The cycle: refresh reference data -> quotes -> strategy fairs -> fee-aware proposals
// -> risk checks -> (bot) submit. Manual proposals wait for the dashboard.
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "desk/client.h"
#include "desk/config.h"
#include "desk/executor.h"
#include "desk/store.h"
#include "desk/strategy.h"

namespace desk {

struct ProposalCandidate {
  std::string outcome_side, exec_style;
  double price = 0, fee_per = 0, net_edge = 0, p_win = 0;
};

class Engine {
 public:
  Engine(Config cfg, Store& store, KalshiClient& client, Executor& exec);

  void cycle();
  json status();

  // ---- operations from the dashboard ------------------------------------------
  void set_kill(bool on, const std::string& reason);
  // Lock-free kill request from the dashboard: takes effect immediately for every send path,
  // the resting-order sweep runs in the next cycle (or right away if the cycle is idle).
  void request_kill(const std::string& reason);
  bool killed() const { return kill_.load(); }
  // Master switch for unattended sends (auto/bot and their exits). Off = everything queues.
  bool auto_armed() const;
  void set_auto_armed(bool on);
  // Net edge at which a proposal stops waiting for you (0 = never). Persisted across restarts.
  double instant_edge() const;
  void set_instant_edge(double edge);
  void set_mode(const std::string& strategy, const std::string& mode);
  std::string mode_for(const std::string& strategy) const;
  // Weight on the market mid when blending a model fair (0 = trust the model fully).
  double market_weight(const std::string& strategy) const;
  void add_watch(const std::string& series_ticker);
  void remove_watch(const std::string& series_ticker);
  std::vector<std::string> watch() const;
  // Manual approve -> submit. expected_price/count (when >= 0) must match the proposal as it
  // stands now; otherwise the approval is refused because the row was re-priced meanwhile.
  json approve(long long proposal_id, double expected_price = -1, double expected_count = -1);
  json risk_view();
  void request_reconcile() { force_reconcile_ = true; }
  // Latest fair for a ticker (any strategy), if computed this session.
  std::optional<std::pair<Fair, std::string>> fair_for(const std::string& ticker) const;
  // Auto-trading rule for a strategy in "auto" mode; returns true when the proposal may go
  // without a human. `why` gets the failing condition.
  bool auto_rule_ok(const Proposal& p, std::string* why) const;
  json auto_rule_json(const std::string& strategy) const;
  json reject(long long proposal_id, const std::string& why);
  json markets_view(const std::string& series_filter);
  // What the board draws: one compact row per contract, ranked by the edge a taker would keep,
  // capped so the dashboard never parses the whole exchange every few seconds.
  json board_view(int limit);
  // Hedging: the payoff table of an event and the protective orders that cut its worst case.
  // ticker/side/count/price describe a trade not yet made ("" = hedge what is already held).
  json hedge_view(const std::string& event_ticker, const std::string& ticker, const std::string& side,
                  double count, double price);
  void refresh_event_meta();
  json auto_budget_view();          // today's unattended spending against each bucket
  // The hedging book: per event, what the desk covers by itself and what it leaves to you.
  json hedge_book_view();
  // How many contracts the caps (and Kelly, when a model covers the ticker) allow for a
  // hand trade at this price. What the board fills into the size box.
  json size_suggestion(const std::string& ticker, const std::string& side, double price, const std::string& style);
  json strategies_view() const;
  const Config& config() const { return cfg_; }
  Config& config() { return cfg_; }
  std::mutex& mutex() { return mu_; }

  // Exposed for tests.
  void add_strategy(std::unique_ptr<Strategy> s) { strategies_.insert(strategies_.begin(), std::move(s)); }
  std::vector<ProposalCandidate> candidates(const Market& m, const Series& s, const Fair& f) const;
  double size_for(const ProposalCandidate& c, const Market& m, double bankroll, double event_used,
                  double cluster_used, double open_used) const;

 private:
  void refresh_series();
  void refresh_markets();
  void run_strategies();
  void run_ladder_arb(const std::vector<Market>& markets);  // locked baskets in exclusive events
  void run_auto_hedge();   // covers the configured share of any event's excess worst case
  void run_exits();   // reduce-only exit proposals for positions whose fair we know
  void run_brackets();  // take-profit / stop-loss / trailing stop per position (dashboard-set)
  void refresh_results();  // settle sweep: results for closed markets we logged a fair on (Brier sample)
  void apply_kill_sweep();  // cancel every resting order, expire pending proposals
  double auto_spent_today();
  double auto_spent_bucket(const std::string& key);
  void add_auto_spent_bucket(const std::string& key, double amount);
  std::pair<std::string, int> bucket_of(const Proposal& p);   // category, exchange shard
  void auto_submit();
  void update_daily_stop();
  std::map<std::string, double> exposure_by(const std::string& key_kind, bool include_proposals = true);  // "event" | "cluster"
  double open_exposure();

  Config cfg_;
  Store& store_;
  KalshiClient& client_;
  Executor& exec_;
  std::vector<std::unique_ptr<Strategy>> strategies_;
  std::mutex mu_;
  std::atomic<bool> kill_{false};
  std::atomic<bool> kill_pending_{false};  // request_kill() asked for the sweep
  std::atomic<bool> force_reconcile_{false};  // something was sent by hand: sync the portfolio now
  long long results_settled_ = 0;
  std::string kill_reason_;
  long long last_series_refresh_ = 0;
  long long cycle_count_ = 0;
  long long last_cycle_ts_ = 0;
  std::string last_error_;
  std::map<std::string, long long> last_quote_log_;
  std::map<std::string, std::pair<double, double>> last_quote_;
  std::map<std::string, long long> last_fair_log_;
  std::map<std::string, Fair> latest_fair_;
  std::map<std::string, long long> latest_fair_ts_;
  std::map<std::string, long long> snap_fair_ts_;   // published copy for the dashboard
  std::map<std::string, long long> event_meta_ts_;  // event -> last /events fetch attempt
  json market_feeds_ = json::object();             // series -> {last_success_ts, open_count, last_error}
  std::set<std::string> fair_set_this_cycle_;  // first covering strategy governs a ticker   // ticker -> most recent fair (any strategy)
  std::map<std::string, std::string> latest_fair_strategy_;
  std::map<std::string, Series> series_cache_;
  std::set<std::string> watch_;
  int markets_seen_ = 0;
  int prune_counter_ = 0;

  // Read-side snapshot: the dashboard reads these under snap_mu_ while a cycle (which can
  // spend seconds in network calls under mu_) is running.
  void publish_snapshot();
  mutable std::mutex snap_mu_;
  std::map<std::string, Fair> snap_fair_;
  std::map<std::string, std::string> snap_fair_strategy_;
  std::map<std::string, Series> snap_series_;
  std::set<std::string> snap_watch_;
  json snap_strategies_ = json::array();
  json snap_status_;
};

}  // namespace desk
