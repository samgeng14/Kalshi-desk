#pragma once
// The fake Kalshi's matching engine and account: books, orders, fills, positions, per-shard cash,
// order groups and fees. No sockets and no clock of its own: every call takes the time, and
// everything the venue would push on its WebSocket comes out as Events for the server to send.
// One thread at a time (the server holds one mutex around it).
//
// What it copies from the venue, and where each rule comes from (docs.kalshi.com, fetched
// 2026-10-06, unless the line says otherwise; the research behind the plan is in
// redesign_all.json's "kalshi" report and v3_research.json's "fills" report):
//   - One book per market in YES terms, held as YES bids and NO bids. A `bid` buys YES at p and
//     rests on the YES side at p; an `ask` sells YES at p, which is buying NO at 1 - p, and rests
//     on the NO side at 1 - p (create-order-v2.md BookSide; get-market-orderbook.md: "yes bids and
//     no bids only"). Trades print at the resting order's price.
//   - Price-time priority (FIFO) inside a level (v3_research fills F2; amend-order-v2.md).
//   - time_in_force fill_or_kill, good_till_canceled, immediate_or_cancel; there are no market
//     orders (redesign_all kalshi K0). expiration_time is whole Unix seconds, only with
//     good_till_canceled; IOC "cannot be combined with expiration_time" (create-order-v2.md).
//     The minimum lead time is undocumented (K1): min_expiry_lead_s, default 0.
//   - reduce_only "will be rejected unless time_in_force is immediate_or_cancel", and the count is
//     "capped by the member's current position" (create-order-v2.md).
//   - post_only that would cross: refused with 400 invalid_order / "post only cross", as the
//     venue answered the old desk (docs/handoff.md session 21, include/desk/reject.h,
//     2026-09-11/12). user-orders.md also lists PostOnlyCrossCancel, so PostOnlyCross::CANCEL
//     accepts the order (201) and cancels it with that reason instead; deskd must handle both.
//   - self_trade_prevention_type: taker_at_cross "cancels the taker order ... execution stops and
//     any partial fills already matched are executed"; maker "cancels the resting maker order and
//     continues matching" (create-order-v2.md). Applied within one subaccount: across subaccounts
//     it is undocumented (plan decision 8).
//   - A duplicate client_order_id is refused with 409. REST does not document it; FIX does
//     ("Duplicate order / ClOrdID already used", fix/error-handling.md), and 409 is create's
//     "resource already exists". Demo-check it (plan section 8).
//   - cancel_order_on_pause: such orders are cancelled when trading pauses (reason HaltCancel,
//     user-orders.md); others keep resting; new orders are refused while paused.
//   - Market close cancels resting orders (CloseCancel) and the book socket sends each market an
//     empty orderbook_snapshot, as recorded on 2026-09-24 (tests/test_commodity.cpp kClose).
//   - Order groups: a contracts_limit over a rolling 15 s window of filled contracts; going over
//     it, a manual trigger, or a limit lowered under the window's count triggers the group: its
//     resting orders are cancelled and new orders refused until a reset; delete cancels them and
//     removes the group; groups do not span shards and every call names the shard with
//     exchange_index, default 0 (getting_started/order_groups.md, order-groups/*.md,
//     exchange_sharding.md "Order groups do not function across exchange instances").
//   - Cash per (subaccount, shard). An order is collateralised on its market's shard and API
//     orders never move cash between shards (exchange_sharding.md; K12, changelog 2026-10-02
//     "Disable automatic order transfers").
//   - Fees: taker = multiplier x 0.07 x C x P x (1 - P); maker = multiplier x 0.0175 x ... only on
//     quadratic_with_maker_fees series (0.035 on combo series); the trade fee is rounded up to
//     $0.000001, then the balance change is aligned to $0.0001 (direct member) or $0.01, the
//     difference charged as a rounding fee into a per-order accumulator that is refunded in whole
//     precision units (redesign_all kalshi K6; getting_started/fee_rounding.md).
//   - Kalshi nets one position per market: buying YES while holding NO closes pairs, each worth
//     $1 at once (plan section 4, "Kalshi nets one position per market").
//
// The fake's own choices where the venue documents nothing, so deskd must not rely on them: the
// error `code` strings other than invalid_order and the 429 body; the collateral rule (an order
// reserves the cost of the part its opposite position cannot close, plus the worst taker fee
// unless post-only); a FOK or reduce-only order that cannot fill answers 201 with nothing filled;
// a fill's `action` ("buy" when it opens contracts, else "sell"); no last_update_reason on user
// cancels, cancel-all and group triggers; open interest counted as traded volume; an explicit
// exchange_index that is not the market's own shard answers 404 not_found "market not found on
// exchange N" (exchange_sharding.md says a write naming a shard goes to that shard and is silent
// on a market that lives elsewhere: a demo check, plan section 8); a cancel-all's minute of
// echo applies to the whole account, not to the subaccount it named.
//
// Street liquidity. Everyone who is not the account is "the street": its orders sit in the same
// FIFO queues as ours. A tape or scenario adds street orders at the back of a level, removes them
// (StreetCancel: from behind our orders first, the plan's "no credit for cancels ahead", or from
// the front), or sends street takers. A street order that crosses ours trades with it. While a
// tape drives the book (set_tape_debts), street quantity our orders already traded is remembered
// as a debt at its level, so the tape's own removal of it later is not applied twice.
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wire.h"

namespace fakex {

using json = nlohmann::json;

// One price grid: [start, end] in steps of `step` (a market's price_ranges).
struct PriceRange {
  Px start, end, step;
};
// linear_cent: 1c from 0.01 to 0.99, most markets.
std::vector<PriceRange> linear_cent();
// tapered_deci_cent: 0.1c below 10c and above 90c, 1c between; the 15-minute commodity and
// crypto series (redesign_all kalshi K7, from the market objects' price_ranges, 2026-10-05).
std::vector<PriceRange> tapered_deci_cent();
bool on_grid(const std::vector<PriceRange>& g, Px p);

enum class FeeType { QUADRATIC, QUADRATIC_WITH_MAKER_FEES, QUADRATIC_WITH_COMBO_MAKER_FEES };
const char* fee_type_name(FeeType t);

// The model fee for one fill, rounded up to $0.000001. mult_milli: the series' fee_multiplier in
// thousandths (1000 = 1, 500 = 0.5, 0 = fee-free).
Micros model_fee(FeeType type, int mult_milli, bool taker, Qty q, Px yes_px);

// fee_rounding.md for one fill: `revenue` is the fill's cash change before fees, `trade_fee` the
// model fee. Returns what the fill is charged (trade fee + rounding fee - rebate) and updates the
// order's accumulator. precision: 100 ($0.0001, direct) or 10,000 ($0.01).
Micros charge_fee(Micros revenue, Micros trade_fee, Micros precision, Micros& accumulator);

struct MarketSpec {
  std::string ticker;
  std::string event_ticker;     // default: the ticker up to its last '-'
  std::string market_id;        // default: a uuid
  int exchange_index = 0;
  std::vector<PriceRange> ticks = linear_cent();
  std::string price_level_structure = "linear_cent";
  FeeType fee_type = FeeType::QUADRATIC;
  int fee_mult_milli = 1000;
  std::int64_t close_ms = 0;    // 0: never closes on its own
  bool fractional = false;      // fractional_trading_enabled (listed only; the engine does not refuse)
  std::int64_t open_ms = 0;     // open_time; 0: left out
  // strike_type, floor_strike, cap_strike as listed ("" / nullopt: left out); the engine never
  // settles on them
  std::string strike_type;
  std::optional<double> floor_strike, cap_strike;
};

enum class Side { BID, ASK };  // the order's book side, YES terms
enum class Tif { FOK, GTC, IOC };
enum class Stp { TAKER_AT_CROSS, MAKER };
enum class OrdStatus { RESTING, CANCELED, EXECUTED };
enum class StreetCancel { BACK_FIRST, FRONT_FIRST };
enum class PostOnlyCross { REFUSE_400, CANCEL };

const char* tif_name(Tif t);
const char* stp_name(Stp s);
const char* status_name(OrdStatus s);

struct Order {
  std::uint64_t seq = 0;  // arrival order: time priority, and the paging key
  bool street = false;
  std::string order_id, client_order_id, ticker, order_group_id;
  int subaccount = 0;
  int exchange_index = 0;
  Side side = Side::BID;
  Px price = 0;           // YES price
  Qty initial = 0, filled = 0, remaining = 0;
  Tif tif = Tif::GTC;
  Stp stp = Stp::TAKER_AT_CROSS;
  bool post_only = false, reduce_only = false, cancel_on_pause = false;
  std::int64_t expiration_s = 0;  // 0: none
  OrdStatus status = OrdStatus::RESTING;
  bool in_book = false;
  std::int64_t created_ms = 0, updated_ms = 0;
  std::string reason;     // last_update_reason; "" where the venue documents none
  Micros taker_cost = 0, maker_cost = 0, taker_fees = 0, maker_fees = 0, fee_acc = 0;
};

struct Fill {
  std::uint64_t seq = 0;
  std::string fill_id, trade_id, order_id, client_order_id, ticker;
  int subaccount = 0, exchange_index = 0;
  Side side = Side::BID;
  Px yes_px = 0;
  Qty count = 0;
  bool taker = false;
  Micros fee = 0;
  Qty post_position = 0;
  bool opened = false;    // action "buy" (it opened contracts) or "sell"
  std::int64_t ts_ms = 0;
};

struct Position {
  Qty pos = 0;             // + YES, - NO (position_fp)
  Micros cost = 0;         // cost of the open contracts (position_cost_dollars)
  Micros realized = 0;
  Micros fees = 0;         // every fee paid in this market
  Micros pos_fees = 0;     // fees on the open contracts (position_fee_cost_dollars)
  Qty volume = 0;
  Micros traded = 0;       // total_traded_dollars
  std::int64_t updated_ms = 0;
  int exchange_index = 0;
};

struct Group {
  std::string id;
  int subaccount = 0, exchange_index = 0;
  Qty limit = 0;
  bool triggered = false;
  std::deque<std::pair<std::int64_t, Qty>> window;  // (fill time, contracts)
  std::vector<std::string> orders;                  // every order placed in it
  std::int64_t created_ms = 0;
};

// One message the venue would push. `channel` is the subscription it belongs to, `type` the
// message's type field; `ticker` filters market subscriptions ("" for order groups); `seq` says
// whether the channel numbers its messages (orderbook_delta, trade and order_group_updates do;
// ticker, fill, user_orders and market_positions do not: websockets/*.md and the frames recorded
// on 2026-09-24). `reset_book` is a market's book replaced (close): the server sends that
// market's orderbook_snapshot to every book subscriber.
struct Event {
  std::string channel, type, ticker;
  bool seq = false;
  bool reset_book = false;
  json msg;
};

struct Refusal {
  int http = 0;  // 0: none
  std::string code, message, details;
  explicit operator bool() const { return http != 0; }
};

struct NewOrder {
  std::string ticker, client_order_id, order_group_id;
  Side side = Side::BID;
  Qty count = 0;
  Px price = 0;
  Tif tif = Tif::GTC;
  Stp stp = Stp::TAKER_AT_CROSS;
  bool post_only = false, reduce_only = false, cancel_on_pause = false;
  std::int64_t expiration_s = 0;
  int subaccount = 0;
  int exchange_index = -1;  // -1: route by ticker (the documented default when a ticker is given)
};

struct CreateResult {
  Refusal err;
  std::string order_id, client_order_id;
  Qty fill_count = 0, remaining = 0;
  Micros fill_cost = 0;  // YES-price notional of the fills in this request, for the average
  Micros fees = 0;
  std::int64_t ts_ms = 0;
};

struct CancelResult {
  Refusal err;
  std::string order_id, client_order_id;
  Qty reduced_by = 0;
  std::int64_t ts_ms = 0;
};

struct Level {
  std::deque<Order*> q;
  Qty total = 0;
};

struct Market {
  MarketSpec spec;
  enum class State { OPEN, PAUSED, CLOSED } state = State::OPEN;
  std::map<Px, Level> yes, no;  // keyed by the side's own price (a NO level by its NO price)
  Px last_px = 0;
  Qty last_size = 0, volume = 0, open_interest = 0;
  Micros dollar_volume = 0;
  json last_ticker;             // the last ticker message's fields, to send only on change
  std::map<std::pair<bool, Px>, std::pair<Qty, std::int64_t>> debt;  // (no side?, px) -> (qty, since)
};

struct EngineConfig {
  Micros balance_precision = 100;      // $0.0001: direct members (fee_rounding.md); 10,000 = $0.01
  StreetCancel street_cancel = StreetCancel::BACK_FIRST;
  PostOnlyCross post_only_cross = PostOnlyCross::REFUSE_400;
  std::int64_t min_expiry_lead_s = 0;  // undocumented on the venue (K1): demo-check
  std::int64_t expiry_delay_ms = 0;    // how late the venue's expiry runs after the second
  // "Newly placed orders may also be cancelled during the minute after the request"
  // (cancel-all-orders.md): orders that rest within this long after a cancel-all are cancelled as
  // they rest. On by default, as on the venue (until 2026-10-07 it was 0 here, so no test showed
  // that an order placed in the minute after KILL's cancel-all is cancelled); 0 turns it off.
  std::int64_t cancel_all_echo_ms = 60000;
  std::int64_t group_window_ms = 15000;
  std::int64_t tape_debt_ms = 2000;    // how long a tape debt waits for the tape's removal
  std::uint64_t seed = 20261006;
  std::string user_id = "6d2f8a1e-0000-4000-8000-00000000fa4e";
};

class Engine {
 public:
  explicit Engine(EngineConfig cfg = {});
  const EngineConfig& config() const { return cfg_; }
  EngineConfig& config() { return cfg_; }

  // ---- markets
  // A new market, or a new spec for one that exists (its book, state and orders stay).
  Market& add_market(MarketSpec spec, std::int64_t now_ms);
  Market* market(const std::string& ticker);
  const Market* market(const std::string& ticker) const;
  const std::map<std::string, Market>& markets() const { return markets_; }
  // Aggregated levels, best last (the venue lists them ascending: "the best bid is the last
  // element", src/types.cpp parse_levels).
  std::vector<std::pair<Px, Qty>> levels(const std::string& ticker, bool no_side) const;
  json ticker_msg(const Market& m, std::int64_t now_ms) const;
  void pause(const std::string& ticker, std::int64_t now_ms);  // "" pauses every market
  void resume(const std::string& ticker, std::int64_t now_ms);
  void close(const std::string& ticker, std::int64_t now_ms);

  // ---- time: expiries, closes, group windows and tape debts
  void tick(std::int64_t now_ms);

  // ---- the account
  void set_cash(int subaccount, int shard, Micros cash, std::int64_t now_ms = 0);
  Micros cash(int subaccount, int shard) const;
  Micros reserved(int subaccount, int shard) const;
  Micros available(int subaccount, int shard) const { return cash(subaccount, shard) - reserved(subaccount, shard); }
  std::set<int> shards() const;      // shards with a market or cash
  std::int64_t cash_updated_ms() const { return cash_updated_ms_; }
  Micros position_value(int subaccount, int shard) const;  // marked at the held side's best bid
  Refusal transfer(int subaccount, int from, int to, Micros amount, std::int64_t now_ms, std::string* id);

  CreateResult create(const NewOrder& o, std::int64_t now_ms);
  // exchange_index: >= 0 routes there, -1 routes by market_ticker (required then), -2 omitted
  // (routes by market_ticker when given, else shard 0): exchange_sharding.md "Order routing".
  CancelResult cancel(const std::string& order_id, const std::string& market_ticker, int exchange_index,
                      int subaccount, std::int64_t now_ms);
  int cancel_all(std::optional<int> subaccount, std::int64_t now_ms);

  const Order* order(const std::string& order_id) const;
  const std::vector<const Order*>& user_orders() const { return user_orders_; }
  const std::vector<Fill>& fills() const { return fills_; }
  const std::map<std::pair<int, std::string>, Position>& positions() const { return positions_; }

  // ---- order groups
  std::string group_create(int subaccount, int shard, Qty limit, std::int64_t now_ms);
  Refusal group_trigger(const std::string& id, int subaccount, int shard, std::int64_t now_ms);
  Refusal group_reset(const std::string& id, int subaccount, int shard, std::int64_t now_ms);
  Refusal group_delete(const std::string& id, int subaccount, int shard, std::int64_t now_ms);
  Refusal group_limit(const std::string& id, int subaccount, int shard, Qty limit, std::int64_t now_ms);
  const Group* group(const std::string& id) const;
  const std::map<std::string, Group>& groups() const { return groups_; }
  Qty group_window(const Group& g, std::int64_t now_ms) const;

  // ---- the street (tapes, scenarios, tests)
  // A resting street order of qty on one side ("yes": a YES bid at px; "no": a NO bid at px). One
  // that crosses the book trades first, like any order.
  void street_add(const std::string& ticker, bool no_side, Px side_px, Qty qty, std::int64_t now_ms);
  // Street quantity leaves a level (a cancel, or a trade the tape shows only as a book change).
  // Our orders never leave this way.
  void street_remove(const std::string& ticker, bool no_side, Px side_px, Qty qty, std::int64_t now_ms);
  // A street taker (IOC) buying (bid) or selling (ask) YES up to `yes_limit`.
  Qty street_take(const std::string& ticker, Side side, Px yes_limit, Qty qty, std::int64_t now_ms);
  // A book snapshot for the street: each level's street quantity becomes what it says, applied as
  // adds and removes so our orders keep their place.
  void street_snapshot(const std::string& ticker, const std::vector<std::pair<Px, Qty>>& yes,
                       const std::vector<std::pair<Px, Qty>>& no, std::int64_t now_ms);
  void set_tape_debts(bool on) { tape_debts_ = on; }

  // Everything the venue would have pushed since the last call.
  std::vector<Event> drain_events();

  // A cancel that loses a race with a fill (plan section 8: "a fill during a cancel"): the street
  // takes `qty` of the order first, then the cancel runs. Used by the server's fault plan.
  Qty street_hit(const std::string& order_id, Qty qty, std::int64_t now_ms);

 private:
  struct Op;  // one call's bookkeeping: touched orders, positions and markets
  Order* new_order(bool street);
  void rest(Market& m, Order& o, Op& op, const std::string& cause_cid, int cause_sub);
  void unrest(Market& m, Order& o, Qty qty, Op& op, const std::string& cause_cid, int cause_sub);
  void finish_cancel(Market& m, Order& o, const std::string& reason, Op& op, const std::string& cause_cid,
                     int cause_sub);
  void match(Market& m, Order& t, Op& op);
  Qty fillable(const Market& m, const Order& t) const;
  void execute(Market& m, Order& taker, Order& maker, Qty q, Op& op);
  void settle(Market& m, Order& o, Qty q, Px yes_px, bool taker, const std::string& trade_id, Op& op);
  void group_fill(Order& o, Qty q, Op& op);
  void trigger(Group& g, Op& op);
  void flush(Op& op);
  void emit_delta(const Market& m, bool no_side, Px side_px, Qty delta, const std::string& cause_cid, int cause_sub,
                  Op& op);
  json user_order_msg(const Order& o) const;
  json position_msg(int subaccount, const std::string& ticker, const Position& p) const;
  Micros reserve_for(int subaccount, const Market& m, Side side, const Order* extra) const;
  Qty reducible(int subaccount, const std::string& ticker, Side side) const;
  Group* find_group(const std::string& id, int subaccount, int shard);
  void erase_street(Order* o);

  EngineConfig cfg_;
  std::mt19937_64 rng_;
  std::uint64_t next_seq_ = 1;
  std::map<std::string, Market> markets_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Order>> pool_;
  std::unordered_map<std::string, Order*> by_id_;
  std::vector<const Order*> user_orders_;
  std::set<std::uint64_t> resting_user_;                          // our orders in a book, by seq
  // The same orders by market. Collateral is worked out per market, and walking every resting
  // order for every market on a shard made 5,000 resting orders cost billions of steps.
  std::unordered_map<std::string, std::map<std::uint64_t, Order*>> resting_by_market_;
  void index_rest(Order& o);
  void index_unrest(const Order& o);
  std::set<std::pair<std::int64_t, std::uint64_t>> expiries_;     // (expiry ms, seq)
  std::set<std::string> cids_seen_;
  std::vector<Fill> fills_;
  std::map<std::pair<int, std::string>, Position> positions_;
  std::map<std::pair<int, int>, Micros> cash_;
  std::int64_t cash_updated_ms_ = 0;
  std::map<std::string, Group> groups_;
  std::vector<Event> events_;
  std::int64_t last_cancel_all_ms_ = std::numeric_limits<std::int64_t>::min() / 2;
  bool tape_debts_ = false;
  std::uint64_t transfers_ = 0;
};

}  // namespace fakex
