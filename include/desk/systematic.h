#pragma once
// The systematic trader: the part of the desk that trades on its own once you press Start.
//
// It runs inside the engine cycle, after the strategies have refreshed their fairs and the
// portfolio has been reconciled, and it does three things every cycle:
//
//   1. screens every watched contract, both sides, and ranks what passes its rules by Kelly
//      fraction (edge per dollar of risk, which is where "edge" and "probability of winning"
//      meet: a 5c edge on a 90c contract is worth far more per dollar than a 5c edge at 30c);
//   2. sends new entries, within its own budgets, through the same Executor every other order
//      on this desk goes through (order cap, turnover cap, exposure caps, kill switch, daily
//      equity stop: none of them are bypassed);
//   3. manages the exits of what it holds: take-profit, stop, time stop, hold-value-vs-sale,
//      and settlement.
//
// The line between the algorithm and you is drawn by the settings and is shown on its screen:
// the algorithm takes candidates whose net edge sits inside [min_edge, max_edge] with a win
// probability at or above the floor, fresh two-sided books, validated model inputs and a
// settlement inside the horizon. Everything else stays in Signals and on the board for a hand
// trade: edges above the cap (an estimate that far from a liquid market is usually the
// estimate that is wrong), large model/market gaps, wide books, and additions to events
// already held.
//
// Paper mode runs the same screen and the same exit rules against simulated fills and sends
// nothing. Live mode sends real orders.
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "desk/config.h"
#include "desk/executor.h"
#include "desk/reject.h"
#include "desk/store.h"

namespace desk {

json systematic_defaults();
// Capital reserved for this strategy only. Losses and released positions reduce its allowance;
// profits do not increase the configured ceiling. This never moves funds between accounts.
json systematic_capital(Store& store, const json& settings, const std::string& mode);
// Strict, atomic merge; invalid fields throw std::invalid_argument.
json validate_systematic_settings(const json& patch, const json& previous);
// Which of the algorithm's asset buckets a market falls in: weather | crypto | sports | other.
std::string systematic_category(const json& market_row);

// The screen. `board` is Engine::research_board(): the complete research universe with the
// captured books merged in. Returns settings, generated_ts, counts and rows, both sides of every
// market in an enabled category. Every row carries `verdict`:
//   algo    - the algorithm would send this now (price, style, count, kelly are filled in)
//   manual  - tradable by hand, held back from the algorithm by one of its policies
//   weak    - passes the data checks but not the thresholds (edge or probability too low)
//   blocked - data or risk gate: not for anybody right now
// with `reasons[]` saying why, in words that go on the dashboard.
// `held_events` are the events the algorithm may not open anything new in: its own open trades,
// plus every event the account holds a position in through ANY route - a dashboard ticket, an
// order placed in the Kalshi app. Build it with SystematicTrader::blocked_events so the screen,
// the paper ledger and the live sender cannot disagree about where that line is.
// `hard_gap` is the desk's own model/market disagreement limit (risk.max_disagreement): a raw
// estimate that far from a two-sided market is blocked for everybody, whatever else the row says.
//
// Since session 15 the screen is the decision router (decision_router.h) rendered as rows: every
// row is one TradeDecision and also carries `route` (auto | manual | research | blocked, where
// "weak" rows are blocked), `decision_id`, the model, and the edge breakdown. This overload is the
// policy view (master switch armed); the engine passes its real state through the other.
json systematic_screen(const json& board, const json& settings, long long now,
                       const std::set<std::string>& held_events = {},
                       const std::map<std::string, long long>& cooldowns = {}, double hard_gap = 0.30);
struct RouterContext;
json systematic_screen(const json& board, const json& settings, const RouterContext& ctx);

// Revalidate a resting entry at its original limit each cycle, rather than treating a fresh model
// timestamp as permission to leave an obsolete price on the exchange. Empty = leave it resting.
// A resting entry is managed, not opened, so this reads the board's management freshness flags
// (`quote_stale_manage`, `fair_stale_manage`) when present; `max_age` is the absolute backstop.
std::string systematic_entry_cancel_reason(const json& trade, const json& market,
                                           const json& settings, long long now, double hard_gap = 0.30,
                                           long long max_age = 120);

class SystematicTrader {
 public:
  struct Hooks {
    std::function<bool()> killed;
    std::function<void(const std::string&)> request_kill;
    std::function<void()> request_reconcile;   // something was sent: sync the portfolio next cycle
    // A fresh read of one ticker's book, as board depth fields (yes_bid, yes_ask, sizes,
    // quote_ts), or null when it cannot be read. Used once when a send is refused because the
    // book moved (post-only would cross): re-read, re-price, and send again only if the edge holds.
    std::function<json(const std::string&)> fresh_quote;
    Hooks() : killed([] { return false; }), request_kill([](const std::string&) {}), request_reconcile([] {}),
              fresh_quote([](const std::string&) { return json(); }) {}
    Hooks(std::function<bool()> k, std::function<void(const std::string&)> rk, std::function<void()> rr,
          std::function<json(const std::string&)> fq = [](const std::string&) { return json(); })
        : killed(std::move(k)), request_kill(std::move(rk)), request_reconcile(std::move(rr)), fresh_quote(std::move(fq)) {}
  };
  // `exec` may be null: then live mode cannot be switched on and paper is all there is.
  SystematicTrader(Store& store, Executor* exec, const Config& cfg, Hooks hooks = {});

  // {running, mode, settings, settings_version, halted, halt_reason, day, spent_today,
  //  performance{...}, positions[], history[], log[], last_step_ts, notice}
  json view();
  // Accepts {settings:{...}}, {running:bool}, {mode:"live"|"paper"}, {clear_halt:true},
  // {release:"TICKER"} (hand a position back to manual management), any combination.
  json configure(const json& body);
  // One cycle. `board` is Engine::research_board(). Must be called under the engine lock.
  // `screen` is the cycle's decisions (systematic_screen with the engine's context); entries are
  // taken from its AUTO rows. Without one the trader asks the router itself, with its own context.
  void step(const json& board, long long now);
  void step(const json& board, const json& screen, long long now);
  // What the router needs from the trader: the events it must leave alone, the cooldowns, and
  // whether the AUTO master switch is armed (running, not halted, not killed, able to send).
  void router_context(RouterContext& ctx);
  // Sell a managed position into the bid now (reduce-only, taker). Under the engine lock.
  json close_now(const std::string& ticker);
  // Tickers the algorithm is managing: the engine's own exit logic leaves these alone.
  std::set<std::string> owned_tickers();
  bool running();

  // Exposed for tests.
  json screen(const json& board, long long now);
  json state() { std::lock_guard<std::mutex> lk(mu_); return state_; }
  json settings_copy() { std::lock_guard<std::mutex> lk(mu_); return state_["settings"]; }

 private:
  json view_unlocked();
  void persist();
  void log(const std::string& level, const std::string& msg);
  void roll_day(long long now);
  // Lanes (M2, include/desk/lanes.h). All under mu_.
  void evaluate_lanes(long long now);          // the live-sizing rule, at the day roll
  json lane_today(const std::string& mode, long long now);   // per model: volume, realized, mark
  void check_lane_loss_stops(long long now);   // per-lane daily loss stop
  json lanes_view(long long now);              // attribution per lane and model
  json performance(long long now);
  // Live path.
  void live_manage(const json& markets, long long now);
  void live_enter(const json& board, const json& screen, long long now);
  void live_arbitrage(long long now);
  // `refused` gets the class of a refusal (RejectClass) when the send did not go out.
  bool live_send_entry(const json& row, double count, long long now, std::string* why,
                       RejectClass* refused = nullptr);
  json execution_stats();
  bool live_send_exit(json& trade, const std::string& reason, double bid, double bid_size, long long now,
                      const std::string& style = "taker", double maker_price = 0);
  // A resting maker exit: filled, still resting, or pulled after its TTL. Returns true when the
  // row was handled (the caller moves on).
  bool live_manage_resting_exit(json& trade, const json& market, double held, long long now, bool killed);
  void finalize(json& trade, const std::string& reason, double realized, double proceeds, double exit_fee,
                double exit_price, long long now);
  // Paper path.
  void paper_manage(const json& markets, long long now);
  void paper_enter(const json& screen, long long now);
  json screen_unlocked(const json& board, long long now);
  // Shared.
  // The exit plan for a held position: why, how (taker into the bid, or a resting sell one tick
  // inside the offer) and at what price. Empty reason = hold. Mutates the trade's stop_seen_ts.
  struct ExitPlan { std::string reason, style; double price = 0; };
  ExitPlan plan_exit(json& trade, const json& market, double bid, double bid_size, double ask, long long now, bool* stale) const;
  std::string decide_exit(const json& trade, const json& market, double bid, double bid_size,
                          long long now, bool* stale) const;
  double pnl_today(long long now);
  double spent_today();
  void add_spent(double cost);
  // Every event the algorithm must leave alone for a NEW entry, in one place so the screen it
  // shows, the paper ledger and the live sender cannot disagree about where the line is.
  // Two sources: the algorithm's own live trades in `mode`, and - the point of this - every
  // event the account already holds through ANY route, including a ticket you sent from the
  // dashboard and an order you placed in the Kalshi app. Those positions are yours; the
  // algorithm neither adds to them nor takes the other side of them.
  std::set<std::string> blocked_events(const std::string& mode);
  json trades(const std::string& state_filter = "");   // rows from systematic_trades
  void upsert_trade(json& trade);

  Store& store_;
  Executor* exec_;
  const Config& cfg_;
  Hooks hooks_;
  std::mutex mu_;
  json state_;                       // running, mode, settings, settings_version, halted, halt_reason, day, cooldowns, backoff, log
  std::map<std::string, long long> refused_until_;   // ticker -> do not retry an entry before this
  std::map<std::string, int> unfilled_streak_;       // ticker -> maker posts pulled unfilled in a row
  std::map<std::string, int> improve_ticks_;         // ticker -> ticks to improve the next maker post by (the ladder)
};

}  // namespace desk
