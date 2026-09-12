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
#include "desk/decision_router.h"
#include "desk/executor.h"
#include "desk/manual_orders.h"
#include "desk/perps.h"
#include "desk/poly.h"
#include "desk/pair_models.h"
#include "desk/reference_feed.h"
#include "desk/risk.h"
#include "desk/store.h"
#include "desk/strategy.h"
#include "desk/systematic.h"

namespace desk {

class Engine {
 public:
  Engine(Config cfg, Store& store, KalshiClient& client, Executor& exec);

  void cycle();
  json status();
  // ---- the book: two lanes, one switch ------------------------------------------------------
  // The desk grew four independent algorithms with four run switches on three tabs, and no one
  // place that said where the line between "it trades this" and "you trade this" actually falls.
  // `book()` is that place, and `book_configure({running:bool})` is the one switch: it starts or
  // stops every lane that can run and says plainly why any lane could not.
  //   lane 1, systematic: the algorithms send inside their own rules and budgets.
  //   lane 2, yours:      everything that clears the bar but one of those rules held back, with
  //                       the reason, waiting for a click. Nothing here is ever sent for you.
  json book();
  json book_configure(const json& body);
  ManualOrders& manual_orders() { return manual_orders_; }
  // The settlement reference feed, or null when this desk watches none of the series that need
  // one. Exposed for the dashboard's status and for tests.
  ReferenceFeed* reference_feed() { return reference_feed_.get(); }
  MarketQuoteFeed* quote_feed() { return quote_feed_.get(); }
  void process_manual_orders(); // called under the engine lock, before autonomous entries

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
  json research_screen(const json& settings);
  json systematic_view(int screen_limit = 0);
  // ---- the decision pipeline (session 15) -------------------------------------------------------
  // The router's context as the desk stands now: who owns which events (positions, working orders,
  // the algorithm's trades, queued manual requests), cooldowns, the kill switch and whether the
  // AUTO master switch is armed.
  RouterContext router_context(long long now, bool include_manual_queue = true);
  // This cycle's decisions, as the Trades board reads them: MANUAL and AUTO rows (RESEARCH too
  // when asked), each with its decision_id, route, model and edge breakdown. Not recomputed per
  // request: what the board shows is what the automatic lane saw.
  json decisions_view(bool include_research = false, int limit = 200);
  // The manual lane's last check before an order goes: the decision you reviewed still exists,
  // its probability and book are fresh, the router still routes it MANUAL, the edge at the price
  // you chose still clears the manual floor, and ownership is clear. Throws with what changed.
  // Returns the fresh decision row the order is sent on. Under the engine lock.
  json revalidate_manual_decision(const json& request, const std::string& request_id);
  // Cheap check at enqueue time (no network, no engine lock): is the decision still on the board,
  // still MANUAL, and does the edge at your price and style clear the manual floor?
  void precheck_manual_decision(const json& request);
  // The (validated) settings the last cycle decided with; the algorithm's own before the first one.
  json decision_settings();
  // order id -> proposal -> decision snapshots.
  json decision_trace(const std::string& order_id);
  json configure_systematic(const json& body);
  json close_systematic(const std::string& ticker) { return systematic_.close_now(ticker); }
  // The margined instance (perpetual futures): its own cash, rails and rule. See docs/perps.md.
  PerpsDesk& perps() { return perps_; }
  // Polymarket US: the second venue, the cross-venue fair and the pair rule. See docs/polymarket.md.
  PolyDesk& poly() { return poly_; }
  PairModelLab& pair_models() { return pair_models_; }
  void step_pair_models();
  json sports_models_view();
  // Hedging: the payoff table of an event and the protective orders that cut its worst case.
  // ticker/side/count/price describe a trade not yet made ("" = hedge what is already held).
  json hedge_view(const std::string& event_ticker, const std::string& ticker, const std::string& side,
                  double count, double price);
  void refresh_event_meta();
  // Rebuilds the contract universe: what each market settles on and where it sits in its own
  // day. Runs on a timer and immediately when any tracked zone's local date turns over.
  void refresh_universe();
  // Snapshots the order book under every live decision. Bounded on purpose: the book that
  // matters for a later execution study is the one a proposal was standing on, not all 2,192.
  void record_books();
  // Which books record_books() reads this cycle, in priority order. Pure selection (no I/O):
  // positions and working entries first, then research candidates in every category whose route
  // is not "off". Exposed for tests.
  std::vector<std::string> book_capture_plan();
  void clear_soft_backoff_for_tests() { soft_backoff_.clear(); }
  std::map<std::string, std::string> last_book_;   // subject -> last payload recorded
  std::map<std::string, long long> last_book_log_ts_;  // periodic heartbeat proves the feed stayed live
  json auto_budget_view();          // today's unattended spending against each bucket
  // The hedging book: per event, what the desk covers by itself and what it leaves to you.
  json hedge_book_view();
  // How many contracts the caps (and Kelly, when a model covers the ticker) allow for a
  // hand trade at this price. What the board fills into the size box. With a decision_id (the
  // Trades board's order dialog) it also prices that decision at this price and style: `edge` is
  // the EdgeCandidate the dialog displays and `decision` says whether it clears the manual floor.
  json size_suggestion(const std::string& ticker, const std::string& side, double price, const std::string& style,
                       const std::string& decision_id = "");
  json strategies_view() const;
  const Config& config() const { return cfg_; }
  Config& config() { return cfg_; }
  std::mutex& mutex() { return mu_; }

  // Exposed for tests.
  void add_strategy(std::unique_ptr<Strategy> s) { strategies_.insert(strategies_.begin(), std::move(s)); }
  // Calibration per strategy, refreshed occasionally: the query walks every settled market the
  // strategy ever logged a fair on, which is not a per-proposal cost.
  // A copy, not a reference: the order dialog's size endpoint calls this from HTTP threads.
  Calibration calibration_for(const std::string& strategy) const;

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
  ManualOrders manual_orders_;
  SystematicTrader systematic_;
  PerpsDesk perps_;
  PolyDesk poly_;
  PairModelLab pair_models_;
  // The settlement reference socket for the short-duration commodity contracts. Owned here
  // because the cycle is its only writer to the store (flush) and because the models hold a bare
  // pointer to it: it must outlive strategies_, so it is declared before it.
  std::unique_ptr<ReferenceFeed> reference_feed_;
  // The exchange's own quotes for the windows in play, at the venue's millisecond clock. The
  // ordinary five-second REST poll cannot answer how fast Kalshi reprices after the reference
  // moves, which is the one question these contracts are interesting for.
  std::unique_ptr<MarketQuoteFeed> quote_feed_;
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
  mutable std::mutex calib_mu_;                                                // guards calib_
  mutable std::map<std::string, std::pair<Calibration, long long>> calib_;  // strategy -> (result, ts)
  std::map<std::string, std::string> universe_day_;  // settlement zone -> its local date last cycle
  long long last_universe_refresh_ = 0;
  json market_feeds_ = json::object();             // series -> {last_success_ts, open_count, last_error}
  std::set<std::string> fair_set_this_cycle_;  // first covering strategy governs a ticker   // ticker -> most recent fair (any strategy)
  std::map<std::string, std::string> latest_fair_strategy_;
  std::map<std::string, Series> series_cache_;
  std::set<std::string> watch_;
  int markets_seen_ = 0;
  int prune_counter_ = 0;

  // The canonical model outputs: one ProbabilityQuote per ticker, from its governing model.
  std::map<std::string, ProbabilityQuote> latest_quote_;
  // One board row for a market (board_view's row). Caller holds snap_mu_.
  // `held` (ticker -> signed position) saves a positions read per market when building a board.
  json board_row_locked(const Market& m, long long now, long long stale_after, long long manage_after,
                        double* rank_edge = nullptr, const std::map<std::string, double>* held = nullptr);
  // Keep this cycle's decisions for the board and for approvals; snapshot what was offered.
  void publish_decisions(const json& screen, long long now);
  std::mutex decisions_mu_;
  json cycle_screen_ = json::object();                       // the last cycle's full screen
  std::map<std::string, std::pair<json, long long>> recent_decisions_;   // decision_id -> (row, seen)
  struct Offered { std::string route, decision_id; double price = 0, edge = 0; long long ts = 0; };
  std::map<std::string, Offered> offered_;                   // ticker|side|model -> last snapshot recorded
  long long last_decision_prune_ = 0;

  // Read-side snapshot: the dashboard reads these under snap_mu_ while a cycle (which can
  // spend seconds in network calls under mu_) is running.
  void publish_snapshot();
  std::map<std::string, ProbabilityQuote> snap_quotes_;
  mutable std::mutex snap_mu_;
  std::map<std::string, Fair> snap_fair_;
  std::map<std::string, std::string> snap_fair_strategy_;
  std::map<std::string, Series> snap_series_;
  std::set<std::string> snap_watch_;
  json snap_strategies_ = json::array();
  json snap_status_;
  json research_board();
  // Reads one orderbook now: research depth for the ticker plus an observation. Under mu_.
  json capture_book(const std::string& ticker);
  std::map<std::string, json> research_books_, snap_research_books_;
  // The strategy modes as the router reads them. cfg_.strategy_modes is written by set_mode under
  // mu_; the router's context is also built on HTTP threads (the Systematic tab), which read this
  // copy instead.
  mutable std::mutex modes_mu_;
  std::map<std::string, std::string> modes_copy_;
  std::map<std::string, std::string> strategy_modes() const {
    std::lock_guard<std::mutex> lk(modes_mu_);
    return modes_copy_;
  }
  // Lane 2 of the Book panel, counted from the last cycle's decisions (publish_decisions). book()
  // once read systematic_.view(), which carries no screen, so it always counted zero.
  std::mutex screen_summary_mu_;
  json screen_summary_ = json::object();
  // auto_submit's breather after a soft refusal (the market moved): ticker -> not before this.
  // Under mu_ (auto_submit runs inside the cycle).
  std::map<std::string, long long> soft_backoff_;
};

}  // namespace desk
