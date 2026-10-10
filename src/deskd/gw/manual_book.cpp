#include "deskd/gw/manual_book.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "deskd/rest/state.h"

namespace deskd {
namespace {
constexpr Qty kQtyCap = 1000000001;   // one past admission's bound: admission answers RO
constexpr kalshi::Micros kMicrosCap = std::numeric_limits<kalshi::Micros>::max() / 4;
kalshi::Micros sat_add(kalshi::Micros a, kalshi::Micros b) {
  if (b > 0 && a > kMicrosCap - b) return kMicrosCap;
  if (b < 0 && a < -kMicrosCap - b) return -kMicrosCap;
  return a + b;
}
kalshi::BookSide book_side(Side s) { return s == Side::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK; }
bool resting(const kalshi::OrderRow& o) { return o.status == kalshi::OrderStatus::RESTING && o.remaining > 0; }
std::optional<int> row_shard(const rest::Snapshot& s, const kalshi::OrderRow& o) {
  if (o.exchange_index) return o.exchange_index;
  const auto m = s.markets.find(o.ticker);
  return m == s.markets.end() ? std::nullopt : m->second.exchange_index;
}
}  // namespace

void ManualBook::set_snapshot(std::shared_ptr<const rest::Snapshot> snapshot) {
  snapshot_ = std::move(snapshot);
  held_.clear();
  if (!snapshot_) return;
  const auto& s = *snapshot_;
  // The venue's collateral for each resting order, modeled as rest_cash_json does: orders on
  // one side share the position's reducing room, best price first, and the reducing part holds
  // nothing. Exact micro-dollars (Px x centi-contract).
  using Key = std::pair<int, std::string>;
  std::map<Key, Qty> yes_room, no_room;
  for (const auto& p : s.positions) {
    if (p.position > 0) yes_room[{p.exchange_index, p.ticker}] = p.position;
    if (p.position < 0) no_room[{p.exchange_index, p.ticker}] = -p.position;
  }
  std::vector<const kalshi::OrderRow*> working;
  for (const auto& o : s.orders) if (resting(o)) working.push_back(&o);
  std::sort(working.begin(), working.end(), [](const auto* a, const auto* b) {
    if (a->ticker != b->ticker) return a->ticker < b->ticker;
    if (a->side != b->side) return a->side < b->side;
    if (a->yes_price != b->yes_price)
      return a->side == kalshi::BookSide::BID ? a->yes_price > b->yes_price : a->yes_price < b->yes_price;
    return a->order_id < b->order_id;
  });
  for (const auto* o : working) {
    const auto shard = row_shard(s, *o);
    if (!shard || o->order_id.empty()) continue;   // no add-back: errs towards less free cash
    auto& room = (o->side == kalshi::BookSide::BID ? no_room : yes_room)[{*shard, o->ticker}];
    const Qty reducing = std::min(room, o->remaining);
    room -= reducing;
    const Px px = o->side == kalshi::BookSide::BID ? o->yes_price : kOneDollar - o->yes_price;
    if (px < 0 || px > kOneDollar) continue;
    held_[o->order_id] = static_cast<kalshi::Micros>(px) * (o->remaining - reducing);
  }
}

void ManualBook::set_book(const MdBook& book) { books_[book.ticker] = book; }

bool ManualBook::reflected(const OrderLedger::Order& o) const {
  if (o.refused) return true;   // never reached the venue, or the venue refused it
  if (!o.terminal() || o.uncertain) return false;
  if (o.filled == 0 && o.executions == 0) return true;   // ended with nothing filled: no cash moved
  return snapshot_ && o.done_ms > 0 && o.done_ms < snapshot_->account_began_ms;
}

kalshi::Micros ManualBook::held_in_snapshot(const OrderLedger::Order& o) const {
  if (!snapshot_ || o.oid.empty() || !o.ack_ms || o.ack_ms >= snapshot_->account_began_ms) return 0;
  const auto it = held_.find(o.oid);
  return it == held_.end() ? 0 : it->second;
}

kalshi::Micros ManualBook::shard_free(int shard) const {
  if (!snapshot_) return 0;
  const auto b = snapshot_->balances.find(shard);
  if (b == snapshot_->balances.end()) return 0;
  kalshi::Micros free = std::clamp<kalshi::Micros>(b->second, 0, kMicrosCap / 100) * 100;   // Money -> micro-dollars
  for (const auto& [cid, o] : ledger_.orders()) {
    if (o.shard != shard || reflected(o)) continue;
    const auto fee = fee_allowance_.count(cid) ? fee_allowance_.at(cid) : 0;
    const auto worst = sat_add(o.original_reserve, fee);
    free = sat_add(free, -(worst - std::min(worst, held_in_snapshot(o))));
  }
  return free;
}

kalshi::Micros ManualBook::venue_free() const {
  if (!snapshot_) return 0;
  kalshi::Micros total = 0;
  for (const auto& b : snapshot_->balances) total = sat_add(total, shard_free(b.first));
  return total;
}

ManualContext ManualBook::context(const PlaceIntent& in, const ManualHealth& h, std::int64_t now) const {
  ManualContext c;
  c.now_ms = now;
  c.killed = h.killed;
  c.read_only = h.read_only;
  c.publisher_healthy = h.publisher;
  c.ledger_healthy = ledger_.healthy();
  c.manual_sender_healthy = h.manual_sender;
  c.cancel_sender_healthy = h.cancel_sender;
  // The same order admission checks them in: Manual off first, then the publisher, the ledger,
  // and the two senders.
  if (h.read_only || !h.publisher || ledger_.healthy()) c.owner_why = h.why;
  else c.owner_why = "order ledger stopped: " + ledger_.why_unhealthy() + "; restart deskd to reconcile";
  c.public_ready = h.public_feed;
  c.private_ready = h.private_feed;
  c.duplicate_cid = ledger_.find(in.cid) != nullptr;
  if (!snapshot_) return c;
  const auto& s = *snapshot_;
  c.account_ready = s.ready && s.account_ready && s.account_ms > 0 && now >= s.account_began_ms &&
                    now - s.account_ms <= kSnapshotMaxAgeMs;
  c.clock_trusted = s.clock.post_trusted;
  c.trading_active = s.exchange.exchange_active && s.exchange.trading_active;
  for (const auto& o : s.orders) if (!in.cid.empty() && o.client_order_id == in.cid) c.duplicate_cid = true;
  const auto m = s.markets.find(in.ticker);
  if (m == s.markets.end()) return c;
  c.market = &m->second;
  const auto bk = books_.find(in.ticker);
  if (bk != books_.end()) c.book = &bk->second;
  if (!m->second.exchange_index) return c;
  const int shard = *m->second.exchange_index;
  if (s.exchange.shards.empty()) c.shard_active = c.trading_active;
  for (const auto& row : s.exchange.shards)
    if (row.exchange_index == shard) c.shard_active = row.exchange_active && row.trading_active;
  c.unknown_order = ledger_.blocks(in.ticker, shard);
  c.free = venue_free();
  c.shard_free = shard_free(shard);
  for (const auto& p : s.positions)
    if (p.ticker == in.ticker && p.exchange_index == shard) c.position = p.position;
  // Same-side claims on the reducing room; foreign resting orders block the market.
  std::map<std::string, bool> owned;   // order_id known to the ledger
  for (const auto& [cid, o] : ledger_.orders()) if (!o.oid.empty()) owned[o.oid] = true;
  Qty committed = 0;
  auto claim = [&](Qty q) { committed = std::min<Qty>(kQtyCap, committed + std::min<Qty>(q, kQtyCap)); };
  for (const auto& o : s.orders) {
    if (!resting(o) || o.ticker != in.ticker) continue;
    const auto rs = row_shard(s, o);
    if (rs && *rs != shard) continue;
    const bool ours = owned.count(o.order_id) || ledger_.find(o.client_order_id);
    if (!ours) {
      c.foreign_order = true;
      if (o.side == book_side(in.side)) claim(o.remaining);
    }
  }
  for (const auto& [cid, o] : ledger_.orders())
    if (o.intent.ticker == in.ticker && o.shard == shard && o.intent.side == in.side && !reflected(o))
      claim(o.intent.count);
  c.reducing_committed = committed;
  return c;
}

ManualBook::Placed ManualBook::place(const PlaceIntent& in, const ManualHealth& h, std::int64_t now, std::int64_t mono_now) {
  Placed r;
  if (!snapshot_) {   // before the first account read every market would look unknown
    r.admission.intent = in;
    r.admission.code = "STALE";
    r.admission.message = "account reconciliation is pending";
    return r;
  }
  r.admission = admit_manual(in, context(in, h, now));
  auto& a = r.admission;
  if (!a.ok()) return r;
  const int shard = a.wire.exchange_index;
  if (!ledger_.insert(a.intent, shard, a.collateral)) {
    a.code = "RO";
    a.message = "the order ledger refused the reservation";
    return r;
  }
  fee_allowance_[a.intent.cid] = a.fee_check;
  auto permit = std::make_shared<std::atomic<bool>>(true);
  permits_[a.intent.cid] = permit;
  KalshiWriteJob job;
  job.kind = KalshiWriteJob::Kind::CREATE;
  job.create = a.wire;
  job.cid = a.intent.cid;
  job.ticker = a.intent.ticker;
  job.shard = shard;
  job.permit = std::move(permit);
  job.kill_generation = h.kill_generation;
  job.valid_until_mono_ms = mono_now + kSendWindowMs;
  r.job = std::move(job);
  return r;
}

bool ManualBook::revoke(const std::string& cid) {
  const auto it = permits_.find(cid);
  if (it == permits_.end()) return false;
  it->second->store(false);
  return true;
}

std::string ManualBook::refusal_code(const KalshiWriteReply& r) {
  using K = kalshi::WriteOutcome::Kind;
  if (r.outcome.kind == K::NOT_SENT) {
    const auto& c = r.local_code;
    if (c == "USER" || c == "KILLED" || c == "STALE" || c == "RO" || c == "BAD_REQUEST" || c == "EXCHANGE" ||
        c == "INTERNAL")
      return c;
    if (c == "EXPIRY") return "STALE";   // the expiry came too close before the write left
    return "RO";                          // STOP and anything new: the sender was not able to send
  }
  if (r.outcome.kind != K::REFUSED) return "";
  switch (r.outcome.cls) {
    case RejectClass::SOFT_MARKET: return is_post_only_cross(r.outcome.why) ? "POST_CROSS" : "MARKET_CLOSED";
    case RejectClass::SOFT_COLLATERAL: return "SHARD_CASH";
    case RejectClass::SOFT_SELF_TRADE: return "STP";
    case RejectClass::RETRYABLE: return "RATE";
    case RejectClass::HARD_INVALID: return "BAD_REQUEST";
    case RejectClass::SOFT_GROUP:
    case RejectClass::AUTH: return "RO";
    default: return "EXCHANGE";
  }
}

bool ManualBook::apply(const KalshiWriteReply& r) {
  using K = kalshi::WriteOutcome::Kind;
  if (r.op != WireOp::CREATE || !ledger_.find(r.cid)) return false;
  if (r.sent_ms) ledger_.sent(r.cid, r.sent_ms);
  const std::int64_t at = r.ack_ms ? r.ack_ms : r.entered_ms;
  bool ok = true;
  if (r.outcome.kind == K::ACK && r.create) {
    ok = ledger_.acknowledged(r.cid, *r.create, at);
  } else if (r.outcome.kind == K::ACK || r.outcome.kind == K::UNKNOWN) {
    ok = ledger_.unknown(r.cid);   // the venue may hold it: keep every dollar, block the market
  } else {
    ok = ledger_.rejected(r.cid, refusal_code(r), at);
  }
  const auto* o = ledger_.find(r.cid);
  if (o && o->terminal()) permits_.erase(r.cid);
  return ok;
}

}  // namespace deskd
