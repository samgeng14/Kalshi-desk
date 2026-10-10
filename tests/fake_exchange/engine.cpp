#include "engine.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace fakex {

// ---- grids and fees ---------------------------------------------------------------------------

std::vector<PriceRange> linear_cent() { return {{100, 9900, 100}}; }
std::vector<PriceRange> tapered_deci_cent() { return {{10, 1000, 10}, {1000, 9000, 100}, {9000, 9990, 10}}; }

bool on_grid(const std::vector<PriceRange>& g, Px p) {
  if (p <= 0 || p >= kDollar) return false;
  for (const auto& r : g)
    if (p >= r.start && p <= r.end && (p - r.start) % r.step == 0) return true;
  return false;
}

const char* fee_type_name(FeeType t) {
  switch (t) {
    case FeeType::QUADRATIC: return "quadratic";
    case FeeType::QUADRATIC_WITH_MAKER_FEES: return "quadratic_with_maker_fees";
    case FeeType::QUADRATIC_WITH_COMBO_MAKER_FEES: return "quadratic_with_combo_maker_fees";
  }
  return "quadratic";
}

Micros model_fee(FeeType type, int mult_milli, bool taker, Qty q, Px yes_px) {
  // rate x C x P x (1 - P) in micro-dollars, with C = q / 100 and P = yes_px / 10,000:
  //   0.07 x (q / 100) x (p / 1e4) x ((1e4 - p) / 1e4) x 1e6 = 7 q p (1e4 - p) / 1e6
  // The maker rates are a quarter of that (0.0175) or a half (0.035, combos). The multiplier is
  // in thousandths, so the divisor gains 1e3. 128 bits: 1e8 contracts' worth of q at the
  // worst p overflows 64.
  __int128 den;
  if (taker) {
    den = static_cast<__int128>(1000000) * 1000;
  } else if (type == FeeType::QUADRATIC_WITH_MAKER_FEES) {
    den = static_cast<__int128>(4000000) * 1000;
  } else if (type == FeeType::QUADRATIC_WITH_COMBO_MAKER_FEES) {
    den = static_cast<__int128>(2000000) * 1000;
  } else {
    return 0;
  }
  const __int128 num = static_cast<__int128>(7) * q * yes_px * (kDollar - yes_px) * mult_milli;
  if (num <= 0) return 0;
  return static_cast<Micros>((num + den - 1) / den);  // "rounded up to the nearest $0.000001"
}

Micros charge_fee(Micros revenue, Micros trade_fee, Micros precision, Micros& acc) {
  // fee_rounding.md: aligned_change = floor_precision(revenue - trade_fee); the difference is the
  // rounding fee, which goes into the order's accumulator; whole precision units in the
  // accumulator come back as a rebate. Net fee = trade fee + rounding fee - rebate, "never
  // negative": a fill's rebate is at most its own fee, in whole units so the balance stays aligned.
  const Micros raw = revenue - trade_fee;
  Micros aligned = raw / precision * precision;
  if (aligned > raw) aligned -= precision;  // floor, not truncation, for a negative change
  const Micros rounding = raw - aligned;
  acc += rounding;
  const Micros rebate = std::min(acc / precision * precision, (trade_fee + rounding) / precision * precision);
  acc -= rebate;
  return trade_fee + rounding - rebate;
}

const char* tif_name(Tif t) {
  switch (t) {
    case Tif::FOK: return "fill_or_kill";
    case Tif::GTC: return "good_till_canceled";
    case Tif::IOC: return "immediate_or_cancel";
  }
  return "";
}
const char* stp_name(Stp s) { return s == Stp::MAKER ? "maker" : "taker_at_cross"; }
const char* status_name(OrdStatus s) {
  switch (s) {
    case OrdStatus::RESTING: return "resting";
    case OrdStatus::CANCELED: return "canceled";
    case OrdStatus::EXECUTED: return "executed";
  }
  return "unknown";
}

namespace {
Refusal refuse(int http, std::string code, std::string message, std::string details = "") {
  return Refusal{http, std::move(code), std::move(message), std::move(details)};
}
bool is_no(const Order& o) { return o.side == Side::ASK; }
Px side_px(const Order& o) { return o.side == Side::BID ? o.price : kDollar - o.price; }
const char* outcome(Side s) { return s == Side::BID ? "yes" : "no"; }
const char* book_side(Side s) { return s == Side::BID ? "bid" : "ask"; }
}  // namespace

// ---- one call's bookkeeping -------------------------------------------------------------------

struct Engine::Op {
  std::int64_t now = 0;
  Order* req = nullptr;          // the user's order this call places, for the 201's averages
  Micros req_cost = 0, req_fees = 0;
  bool street_add = false;       // the taker is a street_add: its level owes the tape
  std::vector<Order*> orders;
  std::set<std::uint64_t> order_set;
  std::vector<std::pair<int, std::string>> pos;
  std::set<std::pair<int, std::string>> pos_set;
  std::vector<std::string> mkts;
  std::set<std::string> mkt_set;
  void touch(Order& o) {
    if (!o.street && order_set.insert(o.seq).second) orders.push_back(&o);
  }
  void touch_pos(int sub, const std::string& t) {
    if (pos_set.insert({sub, t}).second) pos.emplace_back(sub, t);
  }
  void touch_mkt(const std::string& t) {
    if (mkt_set.insert(t).second) mkts.push_back(t);
  }
};

Engine::Engine(EngineConfig cfg) : cfg_(std::move(cfg)), rng_(cfg_.seed) {}

// ---- markets ----------------------------------------------------------------------------------

Market& Engine::add_market(MarketSpec spec, std::int64_t now_ms) {
  if (spec.event_ticker.empty()) {
    const auto dash = spec.ticker.rfind('-');
    spec.event_ticker = dash == std::string::npos ? spec.ticker : spec.ticker.substr(0, dash);
  }
  auto it = markets_.find(spec.ticker);
  if (it != markets_.end()) {
    // A market described again (a scenario's second "market" line, a tape naming it later) keeps
    // its book, its state and our orders in it: replacing it would leave them pointing nowhere.
    if (spec.market_id.empty()) spec.market_id = it->second.spec.market_id;
    it->second.spec = std::move(spec);
    return it->second;
  }
  if (spec.market_id.empty()) spec.market_id = uuid4(rng_);
  Market& m = markets_[spec.ticker];
  m.spec = std::move(spec);
  return m;
}

Market* Engine::market(const std::string& t) {
  auto it = markets_.find(t);
  return it == markets_.end() ? nullptr : &it->second;
}
const Market* Engine::market(const std::string& t) const {
  auto it = markets_.find(t);
  return it == markets_.end() ? nullptr : &it->second;
}

std::vector<std::pair<Px, Qty>> Engine::levels(const std::string& ticker, bool no_side) const {
  std::vector<std::pair<Px, Qty>> out;
  const Market* m = market(ticker);
  if (!m) return out;
  for (const auto& [px, lv] : no_side ? m->no : m->yes)
    if (lv.total > 0) out.emplace_back(px, lv.total);
  return out;
}

json Engine::ticker_msg(const Market& m, std::int64_t now_ms) const {
  Px bid = 0, ask = kDollar;
  Qty bid_sz = 0, ask_sz = 0;
  if (!m.yes.empty()) {
    bid = m.yes.rbegin()->first;
    bid_sz = m.yes.rbegin()->second.total;
  }
  if (!m.no.empty()) {
    ask = kDollar - m.no.rbegin()->first;
    ask_sz = m.no.rbegin()->second.total;
  }
  return json{{"market_id", m.spec.market_id},
              {"market_ticker", m.spec.ticker},
              {"price_dollars", px_str(m.last_px)},
              {"yes_bid_dollars", px_str(bid)},
              {"yes_ask_dollars", px_str(ask)},
              {"volume_fp", qty_str(m.volume)},
              {"open_interest_fp", qty_str(m.open_interest)},
              {"dollar_volume", m.dollar_volume / kMicrosPerDollar},
              {"dollar_open_interest", notional(m.last_px, m.open_interest) / kMicrosPerDollar},
              {"yes_bid_size_fp", qty_str(bid_sz)},
              {"yes_ask_size_fp", qty_str(ask_sz)},
              {"last_trade_size_fp", qty_str(m.last_size)},
              {"ts", now_ms / 1000},
              {"ts_ms", now_ms},
              {"time", rfc3339(now_ms)}};
}

void Engine::pause(const std::string& ticker, std::int64_t now_ms) {
  Op op;
  op.now = now_ms;
  for (auto& [t, m] : markets_) {
    if (!ticker.empty() && t != ticker) continue;
    if (m.state != Market::State::OPEN) continue;
    m.state = Market::State::PAUSED;
    // "the order will be canceled if the order is open and trading on the exchange is paused for
    // any reason" (create-order-v2.md cancel_order_on_pause); the reason the socket gives a halt
    // is HaltCancel (user-orders.md).
    auto idx = resting_by_market_.find(t);
    if (idx == resting_by_market_.end()) continue;
    std::vector<Order*> resting;  // a copy: the cancels edit the index
    for (const auto& [seq, o] : idx->second) resting.push_back(o);
    for (Order* o : resting)
      if (o->in_book && o->cancel_on_pause) finish_cancel(m, *o, "HaltCancel", op, o->client_order_id, o->subaccount);
  }
  flush(op);
}

void Engine::resume(const std::string& ticker, std::int64_t now_ms) {
  for (auto& [t, m] : markets_)
    if ((ticker.empty() || t == ticker) && m.state == Market::State::PAUSED) m.state = Market::State::OPEN;
}

void Engine::settle_market(const std::string& ticker, bool yes, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->settled) return;
  close(ticker, now_ms);
  m->settled = true;
  m->result = yes ? "yes" : "no";
  for (auto& [key, p] : positions_) {
    if (key.second != ticker || !p.pos) continue;
    const bool wins = (p.pos > 0) == yes;
    const Micros payout = wins ? notional(kDollar, std::abs(p.pos)) : 0;
    cash_[{key.first, p.exchange_index}] += payout;
    p.realized += payout - p.cost;
    p.cost = 0;
    p.pos_fees = 0;
    p.pos = 0;
    p.updated_ms = now_ms;
  }
  cash_updated_ms_ = now_ms;
}

void Engine::close(const std::string& ticker, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->state == Market::State::CLOSED) return;
  Op op;
  op.now = now_ms;
  for (auto* side : {&m->yes, &m->no}) {
    for (auto& [px, lv] : *side) {
      (void)px;
      for (Order* o : lv.q) {
        if (o->street) {
          erase_street(o);
          continue;
        }
        o->in_book = false;
        index_unrest(*o);
        o->remaining = 0;
        o->status = OrdStatus::CANCELED;
        o->reason = "CloseCancel";
        o->updated_ms = now_ms;
        op.touch(*o);
      }
    }
    side->clear();
  }
  m->state = Market::State::CLOSED;
  m->debt.clear();
  // The venue sends a closing market an empty orderbook_snapshot (recorded 2026-09-24, seq
  // 2287181-2287185), not a delta per level.
  Event e;
  e.channel = "orderbook_delta";
  e.type = "orderbook_snapshot";
  e.ticker = ticker;
  e.seq = true;
  e.reset_book = true;
  events_.push_back(std::move(e));
  op.touch_mkt(ticker);
  flush(op);
}

void Engine::tick(std::int64_t now_ms) {
  Op op;
  op.now = now_ms;
  for (auto& [t, m] : markets_)
    if (m.spec.close_ms > 0 && now_ms >= m.spec.close_ms && m.state != Market::State::CLOSED) close(t, now_ms);
  while (!expiries_.empty() && expiries_.begin()->first <= now_ms) {
    const std::uint64_t seq = expiries_.begin()->second;
    expiries_.erase(expiries_.begin());
    auto it = pool_.find(seq);
    if (it == pool_.end()) continue;
    Order& o = *it->second;
    if (o.status != OrdStatus::RESTING || !o.in_book) continue;
    Market* m = market(o.ticker);
    if (m) finish_cancel(*m, o, "ExpiryCancel", op, o.client_order_id, o.subaccount);
  }
  for (auto& [t, m] : markets_) {
    (void)t;
    for (auto it = m.debt.begin(); it != m.debt.end();)
      it = now_ms - it->second.second > cfg_.tape_debt_ms ? m.debt.erase(it) : std::next(it);
  }
  flush(op);
}

// ---- the account ------------------------------------------------------------------------------

void Engine::set_cash(int sub, int shard, Micros c, std::int64_t now_ms) {
  cash_[{sub, shard}] = c;
  cash_updated_ms_ = now_ms;
}

Micros Engine::cash(int sub, int shard) const {
  auto it = cash_.find({sub, shard});
  return it == cash_.end() ? 0 : it->second;
}

std::set<int> Engine::shards() const {
  std::set<int> s;
  for (const auto& [k, v] : cash_) {
    (void)v;
    s.insert(k.second);
  }
  for (const auto& [t, m] : markets_) {
    (void)t;
    s.insert(m.spec.exchange_index);
  }
  return s;
}

Qty Engine::reducible(int sub, const std::string& ticker, Side side) const {
  auto it = positions_.find({sub, ticker});
  const Qty pos = it == positions_.end() ? 0 : it->second.pos;
  return side == Side::BID ? std::max<Qty>(0, -pos) : std::max<Qty>(0, pos);
}

// The cash a side of one market holds back: the cost of the part of the resting orders that the
// opposite position cannot close, the position allocated to the oldest orders first (the fake's
// rule; the venue documents none, see the header).
Micros Engine::reserve_for(int sub, const Market& m, Side side, const Order* extra) const {
  Qty cover = reducible(sub, m.spec.ticker, side);
  Micros r = 0;
  auto add = [&](const Order& o) {
    const Qty covered = std::min(cover, o.remaining);
    cover -= covered;
    const Qty open = o.remaining - covered;
    r += side == Side::BID ? notional(o.price, open) : notional(kDollar - o.price, open);
  };
  auto it = resting_by_market_.find(m.spec.ticker);
  if (it != resting_by_market_.end())
    for (const auto& [seq, o] : it->second)  // by seq: the oldest orders first
      if (o->subaccount == sub && o->side == side) add(*o);
  if (extra) add(*extra);
  return r;
}

Micros Engine::reserved(int sub, int shard) const {
  Micros r = 0;
  for (const auto& [ticker, seqs] : resting_by_market_) {
    (void)seqs;
    const Market* m = market(ticker);
    if (m && m->spec.exchange_index == shard)
      r += reserve_for(sub, *m, Side::BID, nullptr) + reserve_for(sub, *m, Side::ASK, nullptr);
  }
  return r;
}

void Engine::index_rest(Order& o) {
  resting_user_.insert(o.seq);
  resting_by_market_[o.ticker].emplace(o.seq, &o);
}

void Engine::index_unrest(const Order& o) {
  resting_user_.erase(o.seq);
  auto it = resting_by_market_.find(o.ticker);
  if (it == resting_by_market_.end()) return;
  it->second.erase(o.seq);
  if (it->second.empty()) resting_by_market_.erase(it);
}

Micros Engine::position_value(int sub, int shard) const {
  Micros v = 0;
  for (const auto& [k, p] : positions_) {
    if (k.first != sub || p.exchange_index != shard || p.pos == 0) continue;
    const Market* m = market(k.second);
    if (!m) continue;
    if (p.pos > 0 && !m->yes.empty()) v += notional(m->yes.rbegin()->first, p.pos);
    if (p.pos < 0 && !m->no.empty()) v += notional(m->no.rbegin()->first, -p.pos);
  }
  return v;
}

Refusal Engine::transfer(int sub, int from, int to, Micros amount, std::int64_t now_ms, std::string* id) {
  if (amount <= 0 || from == to || from < 0 || to < 0)
    return refuse(400, "invalid_parameters", "a transfer needs a positive amount between two shards");
  if (available(sub, from) < amount) return refuse(400, "insufficient_balance", "insufficient balance");
  cash_[{sub, from}] -= amount;
  cash_[{sub, to}] += amount;
  cash_updated_ms_ = now_ms;
  ++transfers_;
  if (id) *id = uuid4(rng_);
  return {};
}

// ---- orders -----------------------------------------------------------------------------------

Order* Engine::new_order(bool street) {
  auto o = std::make_unique<Order>();
  o->seq = next_seq_++;
  o->street = street;
  Order* p = o.get();
  pool_.emplace(p->seq, std::move(o));
  return p;
}

void Engine::erase_street(Order* o) {
  if (o && o->street) pool_.erase(o->seq);
}

const Order* Engine::order(const std::string& id) const {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

void Engine::emit_delta(const Market& m, bool no_side, Px px, Qty delta, const std::string& cause_cid, int cause_sub,
                        Op& op) {
  if (delta == 0) return;
  Event e;
  e.channel = "orderbook_delta";
  e.type = "orderbook_delta";
  e.ticker = m.spec.ticker;
  e.seq = true;
  e.msg = json{{"market_ticker", m.spec.ticker},
               {"market_id", m.spec.market_id},
               {"price_dollars", px_str(px)},
               {"delta_fp", qty_str(delta)},
               {"side", no_side ? "no" : "yes"},
               {"ts", rfc3339(op.now)},
               {"ts_ms", op.now}};
  // "Present only when you caused this orderbook change" (orderbook-updates.md); subaccount
  // "... and are using subaccounts".
  if (!cause_cid.empty()) {
    e.msg["client_order_id"] = cause_cid;
    if (cause_sub != 0) e.msg["subaccount"] = cause_sub;
  }
  events_.push_back(std::move(e));
  op.touch_mkt(m.spec.ticker);
}

void Engine::rest(Market& m, Order& o, Op& op, const std::string& cause_cid, int cause_sub) {
  auto& side = is_no(o) ? m.no : m.yes;
  Level& lv = side[side_px(o)];
  lv.q.push_back(&o);
  lv.total += o.remaining;
  o.in_book = true;
  o.status = OrdStatus::RESTING;
  if (!o.street) {
    index_rest(o);
    if (o.expiration_s > 0) expiries_.insert({o.expiration_s * 1000 + cfg_.expiry_delay_ms, o.seq});
  }
  emit_delta(m, is_no(o), side_px(o), o.remaining, cause_cid, cause_sub, op);
}

// Takes qty of a resting order out of its level; the order leaves the queue when nothing is left.
void Engine::unrest(Market& m, Order& o, Qty qty, Op& op, const std::string& cause_cid, int cause_sub) {
  auto& side = is_no(o) ? m.no : m.yes;
  auto it = side.find(side_px(o));
  if (it == side.end() || qty <= 0) return;
  Level& lv = it->second;
  lv.total -= qty;
  o.remaining -= qty;
  emit_delta(m, is_no(o), side_px(o), -qty, cause_cid, cause_sub, op);
  if (o.remaining == 0) {
    auto pos = std::find(lv.q.begin(), lv.q.end(), &o);
    if (pos != lv.q.end()) lv.q.erase(pos);
    o.in_book = false;
    if (!o.street) index_unrest(o);
    if (lv.q.empty()) side.erase(it);
  }
}

void Engine::finish_cancel(Market& m, Order& o, const std::string& reason, Op& op, const std::string& cause_cid,
                           int cause_sub) {
  if (o.in_book) unrest(m, o, o.remaining, op, cause_cid, cause_sub);
  o.remaining = 0;
  o.status = OrdStatus::CANCELED;
  o.reason = reason;
  o.updated_ms = op.now;
  op.touch(o);
}

// How much an incoming order could fill now: what fill_or_kill checks before matching.
Qty Engine::fillable(const Market& m, const Order& t) const {
  const auto& opp = t.side == Side::BID ? m.no : m.yes;
  Qty n = 0;
  for (auto it = opp.rbegin(); it != opp.rend() && n < t.remaining; ++it) {
    const Px maker_yes = t.side == Side::BID ? kDollar - it->first : it->first;
    if (t.side == Side::BID ? maker_yes > t.price : maker_yes < t.price) break;
    for (const Order* mk : it->second.q) {
      if (!t.street && !mk->street && mk->subaccount == t.subaccount) {
        if (t.stp == Stp::TAKER_AT_CROSS) return std::min(n, t.remaining);
        continue;  // maker STP cancels it and goes on
      }
      n += mk->remaining;
    }
  }
  return std::min(n, t.remaining);
}

void Engine::match(Market& m, Order& t, Op& op) {
  auto& opp = t.side == Side::BID ? m.no : m.yes;
  while (t.remaining > 0 && !opp.empty()) {
    auto it = std::prev(opp.end());  // the best price on the other side
    const Px maker_yes = t.side == Side::BID ? kDollar - it->first : it->first;
    if (t.side == Side::BID ? maker_yes > t.price : maker_yes < t.price) break;
    Order* mk = it->second.q.front();
    if (!t.street && !mk->street && mk->subaccount == t.subaccount) {
      if (t.stp == Stp::TAKER_AT_CROSS) {
        // "cancels the taker order ... execution stops and any partial fills already matched
        // are executed"
        t.reason = "SelfTradeCancel";
        t.remaining = 0;
        t.status = OrdStatus::CANCELED;
        op.touch(t);
        return;
      }
      finish_cancel(m, *mk, "SelfTradeCancel", op, t.client_order_id, t.subaccount);
      continue;
    }
    execute(m, t, *mk, std::min(t.remaining, mk->remaining), op);
    if (!t.street && !t.order_group_id.empty()) {
      auto g = groups_.find(t.order_group_id);
      if (g != groups_.end() && g->second.triggered) return;  // the caller cancels the rest
    }
  }
}

void Engine::execute(Market& m, Order& t, Order& mk, Qty q, Op& op) {
  const Px yes_px = mk.price;
  const std::string trade_id = uuid4(rng_);
  const bool mk_no = is_no(mk);
  const Px mk_px = side_px(mk);
  t.filled += q;
  t.remaining -= q;
  t.updated_ms = op.now;
  mk.filled += q;
  mk.updated_ms = op.now;
  unrest(m, mk, q, op, t.street ? std::string() : t.client_order_id, t.street ? 0 : t.subaccount);
  // a resting order filled to the last contract is executed, as the venue reports it
  if (!mk.street && mk.remaining == 0 && mk.status == OrdStatus::RESTING) mk.status = OrdStatus::EXECUTED;
  if (!mk.street) settle(m, mk, q, yes_px, false, trade_id, op);
  if (!t.street) settle(m, t, q, yes_px, true, trade_id, op);

  Event e;
  e.channel = "trade";
  e.type = "trade";
  e.ticker = m.spec.ticker;
  e.seq = true;
  e.msg = json{{"trade_id", trade_id},
               {"market_ticker", m.spec.ticker},
               {"yes_price_dollars", px_str(yes_px)},
               {"no_price_dollars", px_str(kDollar - yes_px)},
               {"count_fp", qty_str(q)},
               {"taker_side", outcome(t.side)},
               {"taker_outcome_side", outcome(t.side)},
               {"taker_book_side", book_side(t.side)},
               {"is_block_trade", false},
               {"ts", op.now / 1000},
               {"ts_ms", op.now}};
  events_.push_back(std::move(e));
  m.last_px = yes_px;
  m.last_size = q;
  m.volume += q;
  m.open_interest += q;
  m.dollar_volume += notional(yes_px, q);
  op.touch_mkt(m.spec.ticker);

  if (tape_debts_) {
    // The tape still believes this street quantity rests; its own removal of it later must not
    // take it twice.
    auto owe = [&](bool no_side, Px px) {
      auto& d = m.debt[{no_side, px}];
      d.first += q;
      d.second = op.now;
    };
    if (mk.street) owe(mk_no, mk_px);
    if (t.street && op.street_add) owe(is_no(t), side_px(t));
  }
  if (!t.street) group_fill(t, q, op);
  if (!mk.street)
    group_fill(mk, q, op);
  else if (mk.remaining == 0)
    erase_street(&mk);  // last: `mk` is gone after this
}

void Engine::settle(Market& m, Order& o, Qty q, Px yes_px, bool taker, const std::string& trade_id, Op& op) {
  const int shard = m.spec.exchange_index;
  Position& p = positions_[{o.subaccount, m.spec.ticker}];
  p.exchange_index = shard;
  const bool bid = o.side == Side::BID;
  // Netting: a bid closes NO held, an ask closes YES held; each closed pair is worth $1 now.
  const Qty close = std::min(q, bid ? std::max<Qty>(0, -p.pos) : std::max<Qty>(0, p.pos));
  const Qty open = q - close;
  const Px paid = bid ? yes_px : kDollar - yes_px;  // per contract, on the side bought
  const Micros purchased = notional(paid, q);
  const Micros revenue = -purchased + close * (kMicrosPerDollar / kContract);
  if (close > 0) {
    const Qty held = std::abs(p.pos);
    const Micros basis = p.cost * close / held;
    const Micros fee_basis = p.pos_fees * close / held;
    p.cost -= basis;
    p.pos_fees -= fee_basis;
    p.realized += notional(kDollar - paid, close) - basis;
    p.pos += bid ? close : -close;
  }
  if (open > 0) {
    p.pos += bid ? open : -open;
    p.cost += notional(paid, open);
  }
  const Micros fee = charge_fee(revenue, model_fee(m.spec.fee_type, m.spec.fee_mult_milli, taker, q, yes_px),
                                cfg_.balance_precision, o.fee_acc);
  cash_[{o.subaccount, shard}] += revenue - fee;
  cash_updated_ms_ = op.now;
  p.fees += fee;
  if (open > 0) p.pos_fees += fee * open / q;
  p.volume += q;
  p.traded += purchased;
  p.updated_ms = op.now;
  if (taker) {
    o.taker_cost += purchased;
    o.taker_fees += fee;
  } else {
    o.maker_cost += purchased;
    o.maker_fees += fee;
  }
  if (&o == op.req) {
    op.req_cost += notional(yes_px, q);
    op.req_fees += fee;
  }
  o.reason = "Trade";

  Fill f;
  f.seq = next_seq_++;
  f.fill_id = uuid4(rng_);
  f.trade_id = trade_id;
  f.order_id = o.order_id;
  f.client_order_id = o.client_order_id;
  f.ticker = m.spec.ticker;
  f.subaccount = o.subaccount;
  f.exchange_index = shard;
  f.side = o.side;
  f.yes_px = yes_px;
  f.count = q;
  f.taker = taker;
  f.fee = fee;
  f.post_position = p.pos;
  f.opened = open > 0;
  f.ts_ms = op.now;
  fills_.push_back(f);

  Event e;
  e.channel = "fill";
  e.type = "fill";
  e.ticker = m.spec.ticker;
  e.msg = json{{"trade_id", trade_id},
               {"order_id", o.order_id},
               {"market_ticker", m.spec.ticker},
               {"exchange_index", shard},
               {"is_taker", taker},
               {"side", outcome(o.side)},
               {"ts", op.now / 1000},
               {"ts_ms", op.now},
               {"yes_price_dollars", px_str(yes_px)},
               {"count_fp", qty_str(q)},
               {"fee_cost", usd6(fee)},
               {"action", f.opened ? "buy" : "sell"},
               {"post_position_fp", qty_str(p.pos)},
               {"purchased_side", outcome(o.side)},
               {"outcome_side", outcome(o.side)},
               {"book_side", book_side(o.side)}};
  if (!o.client_order_id.empty()) e.msg["client_order_id"] = o.client_order_id;
  if (o.subaccount != 0) e.msg["subaccount"] = o.subaccount;
  events_.push_back(std::move(e));
  op.touch(o);
  op.touch_pos(o.subaccount, m.spec.ticker);
}

void Engine::group_fill(Order& o, Qty q, Op& op) {
  if (o.order_group_id.empty()) return;
  auto it = groups_.find(o.order_group_id);
  if (it == groups_.end()) return;
  Group& g = it->second;
  g.window.emplace_back(op.now, q);
  // "If the rolling total exceeds the limit, the group is triggered" (order_groups.md)
  if (!g.triggered && group_window(g, op.now) > g.limit) trigger(g, op);
}

Qty Engine::group_window(const Group& g, std::int64_t now_ms) const {
  Qty n = 0;
  for (const auto& [t, q] : g.window)
    if (now_ms - t < cfg_.group_window_ms) n += q;
  return n;
}

void Engine::trigger(Group& g, Op& op) {
  g.triggered = true;
  for (const std::string& id : g.orders) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) continue;
    Order& o = *it->second;
    if (o.status != OrdStatus::RESTING || !o.in_book) continue;
    Market* m = market(o.ticker);
    if (m) finish_cancel(*m, o, "", op, o.client_order_id, o.subaccount);
  }
  Event e;
  e.channel = "order_group_updates";
  e.type = "order_group_updates";
  e.seq = true;
  e.msg = json{{"event_type", "triggered"}, {"order_group_id", g.id}, {"ts_ms", op.now}};
  events_.push_back(std::move(e));
}

CreateResult Engine::create(const NewOrder& n, std::int64_t now_ms) {
  CreateResult r;
  r.client_order_id = n.client_order_id;
  r.ts_ms = now_ms;
  Market* m = market(n.ticker);
  if (!m) {
    r.err = refuse(404, "not_found", "market not found");
    return r;
  }
  // exchange_index >= 0 routes to that shard, which has no such market unless it is the
  // market's own; -1 and omitted route by ticker (exchange_sharding.md).
  if (n.exchange_index >= 0 && n.exchange_index != m->spec.exchange_index) {
    r.err = refuse(404, "not_found", "market not found on exchange " + std::to_string(n.exchange_index));
    return r;
  }
  if (m->state == Market::State::CLOSED) {
    r.err = refuse(400, "market_closed", "market is closed");
    return r;
  }
  if (m->state == Market::State::PAUSED) {
    r.err = refuse(400, "trading_paused", "trading is paused");
    return r;
  }
  if (n.count <= 0) {
    r.err = refuse(400, "invalid_parameters", "count must be positive");
    return r;
  }
  if (!on_grid(m->spec.ticks, n.price)) {
    r.err = refuse(400, "invalid_order", "invalid order", "price is not on the market's price grid");
    return r;
  }
  if (n.reduce_only && n.tif != Tif::IOC) {
    r.err = refuse(400, "invalid_order", "invalid order", "reduce_only requires immediate_or_cancel");
    return r;
  }
  if (n.expiration_s > 0 && n.tif != Tif::GTC) {
    r.err = refuse(400, "invalid_order", "invalid order", "expiration_time requires good_till_canceled");
    return r;
  }
  if (n.expiration_s > 0 && n.expiration_s * 1000 <= now_ms + cfg_.min_expiry_lead_s * 1000) {
    r.err = refuse(400, "invalid_order", "invalid order", "expiration_time is not in the future");
    return r;
  }
  if (n.post_only && n.tif != Tif::GTC) {
    // undocumented; the fake is the stricter reading
    r.err = refuse(400, "invalid_order", "invalid order", "post_only requires good_till_canceled");
    return r;
  }
  if (!n.client_order_id.empty() && cids_seen_.count(n.client_order_id)) {
    r.err = refuse(409, "order_already_exists", "an order with this client_order_id already exists");
    return r;
  }
  const int shard = m->spec.exchange_index;
  Group* g = nullptr;
  if (!n.order_group_id.empty()) {
    g = find_group(n.order_group_id, n.subaccount, shard);
    if (!g) {
      r.err = refuse(400, "invalid_order", "invalid order", "order group not found");
      return r;
    }
    if (g->triggered) {
      r.err = refuse(400, "invalid_order", "invalid order", "order group triggered");
      return r;
    }
  }
  // post only cross: the order would take
  const auto& opp = n.side == Side::BID ? m->no : m->yes;
  bool crosses = false;
  if (!opp.empty()) {
    const Px best_yes = n.side == Side::BID ? kDollar - opp.rbegin()->first : opp.rbegin()->first;
    crosses = n.side == Side::BID ? best_yes <= n.price : best_yes >= n.price;
  }
  if (n.post_only && crosses && cfg_.post_only_cross == PostOnlyCross::REFUSE_400) {
    r.err = refuse(400, "invalid_order", "invalid order", "post only cross");
    return r;
  }
  // collateral on the market's shard
  {
    Order probe;
    probe.subaccount = n.subaccount;
    probe.side = n.side;
    probe.price = n.price;
    probe.remaining = n.count;
    if (n.reduce_only) probe.remaining = std::min(n.count, reducible(n.subaccount, n.ticker, n.side));
    const Micros need = reserve_for(n.subaccount, *m, n.side, &probe) - reserve_for(n.subaccount, *m, n.side, nullptr) +
                        (n.post_only ? 0 : model_fee(m->spec.fee_type, m->spec.fee_mult_milli, true, probe.remaining, n.price));
    if (need > available(n.subaccount, shard)) {
      r.err = refuse(400, "insufficient_balance", "insufficient balance");
      return r;
    }
  }

  Op op;
  op.now = now_ms;
  Order& t = *new_order(false);
  t.order_id = uuid4(rng_);
  t.client_order_id = n.client_order_id;
  t.ticker = n.ticker;
  t.order_group_id = n.order_group_id;
  t.subaccount = n.subaccount;
  t.exchange_index = shard;
  t.side = n.side;
  t.price = n.price;
  t.initial = n.count;
  t.remaining = n.count;
  t.tif = n.tif;
  t.stp = n.stp;
  t.post_only = n.post_only;
  t.reduce_only = n.reduce_only;
  t.cancel_on_pause = n.cancel_on_pause;
  t.expiration_s = n.expiration_s;
  t.created_ms = t.updated_ms = now_ms;
  by_id_[t.order_id] = &t;
  user_orders_.push_back(&t);
  if (!t.client_order_id.empty()) cids_seen_.insert(t.client_order_id);
  if (g) g->orders.push_back(t.order_id);
  op.req = &t;
  op.touch(t);
  r.order_id = t.order_id;

  if (n.post_only && crosses) {
    finish_cancel(*m, t, "PostOnlyCrossCancel", op, "", 0);  // PostOnlyCross::CANCEL
  } else if (n.reduce_only && reducible(n.subaccount, n.ticker, n.side) == 0) {
    finish_cancel(*m, t, "ReduceOnlyCancel", op, "", 0);
  } else {
    if (n.reduce_only) t.remaining = std::min(t.remaining, reducible(n.subaccount, n.ticker, n.side));
    if (t.tif == Tif::FOK && fillable(*m, t) < t.remaining) {
      finish_cancel(*m, t, "", op, "", 0);
    } else {
      match(*m, t, op);
      const bool group_stopped = g && g->triggered;
      if (t.status == OrdStatus::CANCELED) {
        // stopped by self-trade prevention
      } else if (t.remaining == 0) {
        t.status = t.filled == t.initial ? OrdStatus::EXECUTED : OrdStatus::CANCELED;
      } else if (t.tif != Tif::GTC || group_stopped) {
        t.remaining = 0;
        t.status = OrdStatus::CANCELED;
        if (t.filled == 0) t.reason.clear();
      } else {
        rest(*m, t, op, t.client_order_id, t.subaccount);
      }
    }
  }
  r.fill_count = t.filled;
  r.remaining = t.status == OrdStatus::RESTING ? t.remaining : 0;
  r.fill_cost = op.req_cost;
  r.fees = op.req_fees;
  // "Newly placed orders may also be cancelled during the minute after the request"
  if (t.status == OrdStatus::RESTING && cfg_.cancel_all_echo_ms > 0 &&
      now_ms - last_cancel_all_ms_ < cfg_.cancel_all_echo_ms)
    finish_cancel(*m, t, "", op, t.client_order_id, t.subaccount);
  flush(op);
  return r;
}

CancelResult Engine::cancel(const std::string& order_id, const std::string& market_ticker, int exchange_index,
                            int subaccount, std::int64_t now_ms) {
  CancelResult r;
  r.order_id = order_id;
  r.ts_ms = now_ms;
  int shard = 0;
  if (exchange_index >= 0) {
    shard = exchange_index;
  } else if (exchange_index == -1 || !market_ticker.empty()) {
    if (market_ticker.empty()) {
      r.err = refuse(400, "invalid_parameters", "market_ticker is required to auto-route");
      return r;
    }
    const Market* m = market(market_ticker);
    if (!m) {
      r.err = refuse(404, "not_found", "market not found");
      return r;
    }
    shard = m->spec.exchange_index;
  }
  auto it = by_id_.find(order_id);
  if (it == by_id_.end() || it->second->subaccount != subaccount || it->second->exchange_index != shard ||
      it->second->status != OrdStatus::RESTING) {
    // Includes an order that has already filled or been cancelled: the shard holds no resting
    // order by that id. The venue's wording is not documented.
    r.err = refuse(404, "not_found", "order not found");
    return r;
  }
  Order& o = *it->second;
  r.client_order_id = o.client_order_id;
  r.reduced_by = o.remaining;
  Op op;
  op.now = now_ms;
  Market* m = market(o.ticker);
  if (m) finish_cancel(*m, o, "", op, o.client_order_id, o.subaccount);
  flush(op);
  return r;
}

int Engine::cancel_all(std::optional<int> subaccount, std::int64_t now_ms) {
  Op op;
  op.now = now_ms;
  int n = 0;
  const std::vector<std::uint64_t> resting(resting_user_.begin(), resting_user_.end());
  for (const std::uint64_t seq : resting) {
    Order& o = *pool_.at(seq);
    if (subaccount && o.subaccount != *subaccount) continue;
    Market* m = market(o.ticker);
    if (!m) continue;
    finish_cancel(*m, o, "", op, o.client_order_id, o.subaccount);
    ++n;
  }
  last_cancel_all_ms_ = now_ms;
  flush(op);
  return n;
}

Qty Engine::street_hit(const std::string& order_id, Qty qty, std::int64_t now_ms) {
  auto it = by_id_.find(order_id);
  if (it == by_id_.end() || !it->second->in_book) return 0;
  Order& o = *it->second;
  Market* m = market(o.ticker);
  if (!m || m->state != Market::State::OPEN) return 0;
  Op op;
  op.now = now_ms;
  Order& t = *new_order(true);
  t.ticker = o.ticker;
  t.side = o.side == Side::BID ? Side::ASK : Side::BID;
  t.price = o.price;
  t.tif = Tif::IOC;
  t.initial = t.remaining = std::min(qty, o.remaining);
  const Qty q = t.remaining;
  execute(*m, t, o, q, op);
  erase_street(&t);
  flush(op);
  return q;
}

// ---- order groups -----------------------------------------------------------------------------

Group* Engine::find_group(const std::string& id, int sub, int shard) {
  auto it = groups_.find(id);
  if (it == groups_.end() || it->second.subaccount != sub || it->second.exchange_index != shard) return nullptr;
  return &it->second;
}

const Group* Engine::group(const std::string& id) const {
  auto it = groups_.find(id);
  return it == groups_.end() ? nullptr : &it->second;
}

std::string Engine::group_create(int sub, int shard, Qty limit, std::int64_t now_ms) {
  Group g;
  g.id = uuid4(rng_);
  g.subaccount = sub;
  g.exchange_index = shard;
  g.limit = limit;
  g.created_ms = now_ms;
  const std::string id = g.id;
  groups_[id] = std::move(g);
  Event e;
  e.channel = "order_group_updates";
  e.type = "order_group_updates";
  e.seq = true;
  e.msg = json{{"event_type", "created"}, {"order_group_id", id}, {"contracts_limit_fp", qty_str(limit)},
               {"ts_ms", now_ms}};
  events_.push_back(std::move(e));
  return id;
}

Refusal Engine::group_trigger(const std::string& id, int sub, int shard, std::int64_t now_ms) {
  Group* g = find_group(id, sub, shard);
  if (!g) return refuse(404, "not_found", "order group not found");
  Op op;
  op.now = now_ms;
  trigger(*g, op);  // a trigger of a triggered group cancels nothing new and says so again
  flush(op);
  return {};
}

Refusal Engine::group_reset(const std::string& id, int sub, int shard, std::int64_t now_ms) {
  Group* g = find_group(id, sub, shard);
  if (!g) return refuse(404, "not_found", "order group not found");
  g->triggered = false;
  g->window.clear();
  Event e;
  e.channel = "order_group_updates";
  e.type = "order_group_updates";
  e.seq = true;
  e.msg = json{{"event_type", "reset"}, {"order_group_id", id}, {"ts_ms", now_ms}};
  events_.push_back(std::move(e));
  return {};
}

Refusal Engine::group_delete(const std::string& id, int sub, int shard, std::int64_t now_ms) {
  Group* g = find_group(id, sub, shard);
  if (!g) return refuse(404, "not_found", "order group not found");
  Op op;
  op.now = now_ms;
  for (const std::string& oid : g->orders) {
    auto it = by_id_.find(oid);
    if (it == by_id_.end() || !it->second->in_book) continue;
    Market* m = market(it->second->ticker);
    if (m) finish_cancel(*m, *it->second, "", op, it->second->client_order_id, it->second->subaccount);
  }
  groups_.erase(id);
  Event e;
  e.channel = "order_group_updates";
  e.type = "order_group_updates";
  e.seq = true;
  e.msg = json{{"event_type", "deleted"}, {"order_group_id", id}, {"ts_ms", now_ms}};
  events_.push_back(std::move(e));
  flush(op);
  return {};
}

Refusal Engine::group_limit(const std::string& id, int sub, int shard, Qty limit, std::int64_t now_ms) {
  Group* g = find_group(id, sub, shard);
  if (!g) return refuse(404, "not_found", "order group not found");
  if (limit < kContract) return refuse(400, "invalid_parameters", "contracts_limit must be at least 1");
  g->limit = limit;
  Event e;
  e.channel = "order_group_updates";
  e.type = "order_group_updates";
  e.seq = true;
  e.msg = json{{"event_type", "limit_updated"}, {"order_group_id", id}, {"contracts_limit_fp", qty_str(limit)},
               {"ts_ms", now_ms}};
  events_.push_back(std::move(e));
  // "If the updated limit would immediately trigger the group, all orders in the group are
  // canceled and the group is triggered" (update-order-group-limit.md)
  if (!g->triggered && group_window(*g, now_ms) > limit) {
    Op op;
    op.now = now_ms;
    trigger(*g, op);
    flush(op);
  }
  return {};
}

// ---- the street -------------------------------------------------------------------------------

void Engine::street_add(const std::string& ticker, bool no_side, Px px, Qty qty, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->state != Market::State::OPEN || qty <= 0 || px <= 0 || px >= kDollar) return;
  Op op;
  op.now = now_ms;
  op.street_add = true;
  Order& o = *new_order(true);
  o.ticker = ticker;
  o.side = no_side ? Side::ASK : Side::BID;
  o.price = no_side ? kDollar - px : px;
  o.initial = o.remaining = qty;
  o.created_ms = now_ms;
  match(*m, o, op);
  if (o.remaining > 0)
    rest(*m, o, op, "", 0);
  else
    erase_street(&o);
  flush(op);
}

void Engine::street_remove(const std::string& ticker, bool no_side, Px px, Qty qty, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->state != Market::State::OPEN || qty <= 0) return;
  auto d = m->debt.find({no_side, px});
  if (d != m->debt.end()) {
    const Qty paid = std::min(qty, d->second.first);
    d->second.first -= paid;
    qty -= paid;
    if (d->second.first == 0) m->debt.erase(d);
  }
  auto& side = no_side ? m->no : m->yes;
  auto it = side.find(px);
  if (it == side.end() || qty <= 0) return;
  Op op;
  op.now = now_ms;
  // Street orders only, from the back of the queue (StreetCancel::BACK_FIRST: the queue ahead of
  // ours never moves up for a cancel we cannot place) or from the front.
  std::vector<Order*> street;
  for (Order* o : it->second.q)
    if (o->street) street.push_back(o);
  if (cfg_.street_cancel == StreetCancel::BACK_FIRST) std::reverse(street.begin(), street.end());
  for (Order* o : street) {
    if (qty <= 0) break;
    const Qty take = std::min(qty, o->remaining);
    qty -= take;
    unrest(*m, *o, take, op, "", 0);  // may erase the level: `it` is not used again
    if (o->remaining == 0) erase_street(o);
  }
  flush(op);
}

Qty Engine::street_take(const std::string& ticker, Side side, Px yes_limit, Qty qty, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->state != Market::State::OPEN || qty <= 0) return 0;
  Op op;
  op.now = now_ms;
  Order& t = *new_order(true);
  t.ticker = ticker;
  t.side = side;
  t.price = yes_limit;
  t.tif = Tif::IOC;
  t.initial = t.remaining = qty;
  match(*m, t, op);
  const Qty filled = t.filled;
  erase_street(&t);
  flush(op);
  return filled;
}

void Engine::street_snapshot(const std::string& ticker, const std::vector<std::pair<Px, Qty>>& yes,
                             const std::vector<std::pair<Px, Qty>>& no, std::int64_t now_ms) {
  Market* m = market(ticker);
  if (!m || m->state != Market::State::OPEN) return;
  m->debt.clear();  // a snapshot is the venue's whole book: nothing is owed against it
  std::vector<std::tuple<bool, Px, Qty>> adds, removes;
  for (const bool no_side : {false, true}) {
    std::map<Px, Qty> want;
    for (const auto& [px, q] : no_side ? no : yes)
      if (q > 0) want[px] += q;
    std::map<Px, Qty> have;
    for (const auto& [px, lv] : no_side ? m->no : m->yes)
      for (const Order* o : lv.q)
        if (o->street) have[px] += o->remaining;
    for (const auto& [px, q] : have) {
      const Qty w = want.count(px) ? want[px] : 0;
      if (w < q) removes.emplace_back(no_side, px, q - w);
    }
    for (const auto& [px, q] : want) {
      const Qty h = have.count(px) ? have[px] : 0;
      if (q > h) adds.emplace_back(no_side, px, q - h);
    }
  }
  const bool debts = tape_debts_;
  tape_debts_ = false;  // a snapshot's own changes owe nothing
  for (const auto& [no_side, px, q] : removes) street_remove(ticker, no_side, px, q, now_ms);
  tape_debts_ = debts;
  for (const auto& [no_side, px, q] : adds) street_add(ticker, no_side, px, q, now_ms);
}

// ---- what the socket says at the end of a call ------------------------------------------------

json Engine::user_order_msg(const Order& o) const {
  json j{{"order_id", o.order_id},
         {"user_id", cfg_.user_id},
         {"ticker", o.ticker},
         {"exchange_index", o.exchange_index},
         {"status", status_name(o.status)},
         {"side", outcome(o.side)},
         {"is_yes", o.side == Side::BID},
         {"outcome_side", outcome(o.side)},
         {"book_side", book_side(o.side)},
         {"yes_price_dollars", px_str(o.price)},
         {"fill_count_fp", qty_str(o.filled)},
         {"remaining_count_fp", qty_str(o.status == OrdStatus::RESTING ? o.remaining : 0)},
         {"initial_count_fp", qty_str(o.initial)},
         {"taker_fill_cost_dollars", usd6(o.taker_cost)},
         {"maker_fill_cost_dollars", usd6(o.maker_cost)},
         {"taker_fees_dollars", usd6(o.taker_fees)},
         {"maker_fees_dollars", usd6(o.maker_fees)},
         {"client_order_id", o.client_order_id},
         {"created_time", rfc3339(o.created_ms)},
         {"created_ts_ms", o.created_ms},
         {"self_trade_prevention_type", stp_name(o.stp)},
         {"last_update_time", rfc3339(o.updated_ms)},
         {"last_updated_ts_ms", o.updated_ms},
         {"subaccount_number", o.subaccount}};
  if (!o.order_group_id.empty()) j["order_group_id"] = o.order_group_id;
  if (o.expiration_s > 0) {
    j["expiration_time"] = rfc3339(o.expiration_s * 1000);
    j["expiration_ts_ms"] = o.expiration_s * 1000;
  }
  if (!o.reason.empty()) j["last_update_reason"] = o.reason;
  return j;
}

json Engine::position_msg(int sub, const std::string& ticker, const Position& p) const {
  json j{{"user_id", cfg_.user_id},
         {"market_ticker", ticker},
         {"position_fp", qty_str(p.pos)},
         {"position_cost_dollars", usd4(p.cost)},
         {"realized_pnl_dollars", usd4(p.realized)},
         {"fees_paid_dollars", usd4(p.fees)},
         {"position_fee_cost_dollars", usd4(p.pos_fees)},
         {"volume_fp", qty_str(p.volume)}};
  if (sub != 0) j["subaccount"] = sub;
  return j;
}

void Engine::flush(Op& op) {
  for (Order* o : op.orders) {
    Event e;
    e.channel = "user_orders";
    e.type = "user_order";
    e.ticker = o->ticker;
    e.msg = user_order_msg(*o);
    events_.push_back(std::move(e));
  }
  for (const auto& [sub, ticker] : op.pos) {
    Event e;
    e.channel = "market_positions";
    e.type = "market_position";
    e.ticker = ticker;
    e.msg = position_msg(sub, ticker, positions_.at({sub, ticker}));
    events_.push_back(std::move(e));
  }
  // The ticker channel "updates whenever a field changes" (redesign_all kalshi K3).
  for (const std::string& t : op.mkts) {
    Market* m = market(t);
    if (!m) continue;
    json now = ticker_msg(*m, op.now);
    json key = now;
    for (const char* k : {"ts", "ts_ms", "time"}) key.erase(k);
    if (key == m->last_ticker) continue;
    m->last_ticker = key;
    Event e;
    e.channel = "ticker";
    e.type = "ticker";
    e.ticker = t;
    e.msg = std::move(now);
    events_.push_back(std::move(e));
  }
  op.orders.clear();
  op.order_set.clear();
  op.pos.clear();
  op.pos_set.clear();
  op.mkts.clear();
  op.mkt_set.clear();
}

std::vector<Event> Engine::drain_events() {
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

}  // namespace fakex
