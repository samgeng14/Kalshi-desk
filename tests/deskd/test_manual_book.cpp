// gw/manual_book: the context admission sees (shard cash without double counting, reducing room,
// foreign and duplicate orders, staleness) and the sender replies that end or keep a reservation.
#include "check.h"
#include "deskd/gw/manual_book.h"
#include "deskd/rest/state.h"
using namespace deskd;
constexpr std::int64_t now = 1791500000123LL;
constexpr kalshi::Micros kDollar = 1000000;

struct F {
  rest::Snapshot s;
  MdBook b;
  ManualHealth h;
  ManualBook mb;
  F() {
    kalshi::MarketRow m;
    m.ticker = "TEST-A"; m.status = "active"; m.exchange_index = 2;
    m.open_ms = now - 60000; m.close_ms = (now / 1000 + 3600) * 1000;
    m.price_ranges = {{0, 10000, 100}}; m.fee_type = "quadratic"; m.fee_multiplier = 1; m.fee_ms = now;
    s.markets[m.ticker] = m;
    s.balances[0] = 0;
    s.balances[2] = 100 * 10000;   // $100 available on shard 2 (Money: 1/10,000 dollar)
    s.exchange.exchange_active = s.exchange.trading_active = true;
    s.exchange.shards = {{0, true, true, std::nullopt}, {2, true, true, std::nullopt}};
    s.clock.post_trusted = true;
    s.ready = s.account_ready = s.listing_ready = true;
    s.account_began_ms = now - 2000; s.account_ms = now - 1000;
    b.ticker = "TEST-A"; b.stale = false; b.bids = {{4000, 1000}}; b.asks = {{4200, 1000}};
    h.read_only = false; h.public_feed = h.private_feed = h.publisher = h.manual_sender = h.cancel_sender = true;
    h.kill_generation = 7;
    publish();
    mb.set_book(b);
  }
  void publish() { mb.set_snapshot(std::make_shared<const rest::Snapshot>(s)); }
  PlaceIntent intent(const std::string& cid, Side side = Side::BUY, Px px = 4200, Qty n = 1000) {
    PlaceIntent p; p.cid = cid; p.ticker = "TEST-A"; p.side = side; p.price = px; p.count = n; return p;
  }
  ManualBook::Placed place(const PlaceIntent& p) { return mb.place(p, h, now, 5000); }
};
KalshiWriteReply ack(const std::string& cid, const std::string& oid, Qty filled, Qty left, std::int64_t at) {
  KalshiWriteReply r; r.cid = cid; r.op = WireOp::CREATE; r.outcome.kind = kalshi::WriteOutcome::Kind::ACK;
  kalshi::CreateAck a; a.order_id = oid; a.client_order_id = cid; a.fill_count = filled; a.remaining = left; a.ts_ms = at;
  r.create = a; r.entered_ms = at - 30; r.sent_ms = at - 25; r.ack_ms = at; return r;
}

static void places_and_reserves() {
  F f;
  CHECK(f.mb.shard_free(2) == 100 * kDollar); CHECK(f.mb.venue_free() == 100 * kDollar);
  auto p = f.place(f.intent("Msess-1"));
  CHECK(p.admission.ok()); CHECK(p.job.has_value());
  CHECK(p.job->cid == "Msess-1" && p.job->shard == 2 && p.job->create.exchange_index == 2);
  CHECK(p.job->kill_generation == 7 && p.job->valid_until_mono_ms == 5000 + ManualBook::kSendWindowMs);
  CHECK(p.job->permit && p.job->permit->load());
  const auto worst = p.admission.required();
  CHECK(f.mb.ledger().find("Msess-1") && f.mb.ledger().reserved(2) == p.admission.collateral);
  CHECK(f.mb.shard_free(2) == 100 * kDollar - worst);   // collateral AND the taker-fee allowance
  CHECK(f.mb.venue_free() == 100 * kDollar - worst);
  CHECK(f.place(f.intent("Msess-1")).admission.code == "DUP_CID");
  CHECK(f.mb.revoke("Msess-1") && !p.job->permit->load());
  CHECK(!f.mb.revoke("Mnope-1"));
  // a shard that cannot cover the second order
  F g; g.s.balances[2] = 50000; g.s.balances[0] = 100 * 10000; g.publish();   // $5.00 here, $100 elsewhere
  CHECK(g.place(g.intent("Msess-1")).admission.ok());
  CHECK(g.place(g.intent("Msess-2")).admission.code == "SHARD_CASH");
  F z; z.s.balances.erase(2); z.s.balances[0] = 100 * 10000; z.publish();
  CHECK(z.place(z.intent("Msess-1")).admission.code == "SHARD_CASH");
  F v; v.s.balances[2] = 50000; v.publish();   // $5 in all: the venue check answers first
  CHECK(v.place(v.intent("Msess-1")).admission.ok());
  CHECK(v.place(v.intent("Msess-2")).admission.code == "NO_CASH");
}

static void reflection() {
  // An IOC that filled spends cash the snapshot cannot show until a read begins after it ended.
  F f; auto p = f.place(f.intent("Msess-1")); const auto worst = p.admission.required();
  CHECK(f.mb.apply(ack("Msess-1", "oid-1", 1000, 0, now + 40)));
  CHECK(f.mb.ledger().find("Msess-1")->state == OrdState::FILL);
  CHECK(f.mb.shard_free(2) == 100 * kDollar - worst);
  f.s.balances[2] = 100 * 10000 - 42000 - 1800; f.s.account_began_ms = now + 40; f.s.account_ms = now + 100; f.publish();
  CHECK(f.mb.shard_free(2) == (100 * 10000 - 42000 - 1800) * 100LL - worst);   // began at the ack, not after it
  f.s.account_began_ms = now + 41; f.publish();
  CHECK(f.mb.shard_free(2) == (100 * 10000 - 42000 - 1800) * 100LL);   // the venue's number alone
  // An IOC that filled nothing moved no cash: free at once.
  F g; g.place(g.intent("Msess-1"));
  CHECK(g.mb.apply(ack("Msess-1", "oid-1", 0, 0, now + 40)));
  CHECK(g.mb.ledger().find("Msess-1")->state == OrdState::CXL);
  CHECK(g.mb.shard_free(2) == 100 * kDollar);
}

static void resting_not_twice() {
  // A POST the venue acknowledged before the read began: the venue already took its collateral
  // out of available, so only its fee allowance stays subtracted.
  F f; auto in = f.intent("Msess-1", Side::BUY, 4100); in.tif = Tif::POST; in.ttl_s = 60;
  auto p = f.place(in); CHECK(p.admission.ok());
  CHECK(f.mb.apply(ack("Msess-1", "oid-1", 0, 1000, now + 40)));
  CHECK(f.mb.ledger().find("Msess-1")->state == OrdState::WORK);
  kalshi::OrderRow row; row.order_id = "oid-1"; row.client_order_id = "Msess-1"; row.ticker = "TEST-A";
  row.side = kalshi::BookSide::BID; row.yes_price = 4100; row.initial = row.remaining = 1000; row.exchange_index = 2;
  f.s.orders = {row}; f.s.balances[2] = 100 * 10000 - 41000;
  f.s.account_began_ms = now + 50; f.s.account_ms = now + 90; f.publish();
  CHECK(f.mb.shard_free(2) == (100 * 10000 - 41000) * 100LL - p.admission.fee_check);
  // the same row in a snapshot that began BEFORE the ack is not the venue's hold of this order
  f.s.account_began_ms = now + 40; f.publish();
  CHECK(f.mb.shard_free(2) == (100 * 10000 - 41000) * 100LL - p.admission.required());
  // and it is ours, not foreign
  f.s.account_began_ms = now + 50; f.publish();
  CHECK(f.mb.place(f.intent("Msess-2", Side::BUY, 4000, 100), f.h, now + 100, 5000).admission.ok());
}

static void replies() {
  using K = kalshi::WriteOutcome::Kind;
  F f; f.place(f.intent("Msess-1"));
  KalshiWriteReply r; r.cid = "Msess-1"; r.op = WireOp::CREATE; r.entered_ms = now + 1;
  r.outcome.kind = K::NOT_SENT; r.local_code = "USER";
  CHECK(f.mb.apply(r)); CHECK(f.mb.ledger().find("Msess-1")->state == OrdState::CXL);
  CHECK(f.mb.ledger().find("Msess-1")->why == "USER"); CHECK(f.mb.shard_free(2) == 100 * kDollar);
  f.place(f.intent("Msess-2"));
  r.cid = "Msess-2"; r.outcome.kind = K::REFUSED; r.outcome.cls = RejectClass::SOFT_COLLATERAL; r.local_code.clear();
  r.sent_ms = now + 2; r.ack_ms = now + 30;
  CHECK(f.mb.apply(r)); CHECK(f.mb.ledger().find("Msess-2")->state == OrdState::REJ);
  CHECK(f.mb.ledger().find("Msess-2")->why == "SHARD_CASH"); CHECK(f.mb.shard_free(2) == 100 * kDollar);
  // UNKNOWN keeps every dollar and blocks the market for the next click
  auto p3 = f.place(f.intent("Msess-3"));
  r.cid = "Msess-3"; r.outcome.kind = K::UNKNOWN;
  CHECK(f.mb.apply(r)); CHECK(f.mb.ledger().find("Msess-3")->state == OrdState::PEND);
  CHECK(f.mb.shard_free(2) == 100 * kDollar - p3.admission.required());
  CHECK(f.place(f.intent("Msess-4")).admission.code == "STALE");
  // a 2xx whose body did not parse is UNKNOWN too
  F g; g.place(g.intent("Msess-1")); auto bad = ack("Msess-1", "oid-1", 0, 0, now + 40); bad.create.reset();
  CHECK(g.mb.apply(bad)); CHECK(g.mb.ledger().find("Msess-1")->uncertain);
  // contradictions make the ledger unhealthy, which stops admission
  F c; c.place(c.intent("Msess-1"));
  r.cid = "Msess-1"; r.outcome.kind = K::REFUSED; r.outcome.cls = RejectClass::SOFT_MARKET; r.outcome.why = "post only cross";
  CHECK(c.mb.apply(r)); CHECK(c.mb.ledger().find("Msess-1")->state == OrdState::CXL);
  CHECK(c.mb.ledger().find("Msess-1")->why == "POST_CROSS");
  CHECK(!c.mb.apply(ack("Msess-1", "oid-9", 1000, 0, now + 50))); CHECK(!c.mb.ledger().healthy());
  CHECK(c.place(c.intent("Msess-2")).admission.code == "RO");
  CHECK(!c.mb.apply(ack("Mnope-1", "oid-1", 0, 0, now)));   // not ours
  // the codes
  KalshiWriteReply q; q.outcome.kind = K::REFUSED;
  q.outcome.cls = RejectClass::RETRYABLE; CHECK(ManualBook::refusal_code(q) == "RATE");
  q.outcome.cls = RejectClass::SOFT_SELF_TRADE; CHECK(ManualBook::refusal_code(q) == "STP");
  q.outcome.cls = RejectClass::SOFT_MARKET; q.outcome.why = "market closed"; CHECK(ManualBook::refusal_code(q) == "MARKET_CLOSED");
  q.outcome.cls = RejectClass::UNKNOWN; CHECK(ManualBook::refusal_code(q) == "EXCHANGE");
  q.outcome.kind = K::NOT_SENT; q.local_code = "EXPIRY"; CHECK(ManualBook::refusal_code(q) == "STALE");
  q.local_code = "STOP"; CHECK(ManualBook::refusal_code(q) == "RO");
  q.local_code = "KILLED"; CHECK(ManualBook::refusal_code(q) == "KILLED");
}

static void reducing_room() {
  F f; f.s.positions = {{"TEST-A", 2, -500, 0, 0, 0, 0, std::nullopt}}; f.publish();   // long 5 NO: a YES buy reduces
  auto a = f.place(f.intent("Msess-1"));
  CHECK(a.admission.ok() && a.admission.collateral == 4200LL * 500);   // 5 reduce, 5 open
  auto b = f.place(f.intent("Msess-2"));
  CHECK(b.admission.ok() && b.admission.collateral == 4200LL * 1000);  // the first order claimed the room
  auto r = f.intent("Msess-3"); r.reduce_only = true;
  CHECK(f.place(r).admission.code == "REDUCE");
  // the position on another shard is not this market's room
  F g; g.s.positions = {{"TEST-A", 0, -500, 0, 0, 0, 0, std::nullopt}}; g.publish();
  CHECK(g.place(g.intent("Msess-1")).admission.collateral == 4200LL * 1000);
  // a foreign resting order on the market: shown, and the market is blocked until cancelled
  F x; kalshi::OrderRow row; row.order_id = "ext-1"; row.client_order_id = ""; row.ticker = "TEST-A";
  row.side = kalshi::BookSide::ASK; row.yes_price = 4500; row.initial = row.remaining = 100; row.exchange_index = 2;
  x.s.orders = {row}; x.publish();
  CHECK(x.place(x.intent("Msess-1")).admission.code == "RO");
  row.client_order_id = "Msess-1"; row.status = kalshi::OrderStatus::CANCELED; row.remaining = 0;
  x.s.orders = {row}; x.publish();
  CHECK(x.place(x.intent("Msess-1")).admission.code == "DUP_CID");   // a cid the venue has seen
  CHECK(x.place(x.intent("Msess-2")).admission.ok());                // not resting: not foreign
}

static void staleness() {
  F f; ManualBook empty;
  CHECK(empty.place(f.intent("Msess-1"), f.h, now, 0).admission.code == "STALE");
  f.s.account_ms = now - ManualBook::kSnapshotMaxAgeMs - 1; f.publish();
  CHECK(f.place(f.intent("Msess-1")).admission.code == "STALE");
  F g; g.s.ready = false; g.publish(); CHECK(g.place(g.intent("Msess-1")).admission.code == "STALE");
  F h; h.h.private_feed = false; CHECK(h.place(h.intent("Msess-1")).admission.code == "STALE");
  F i; i.s.exchange.shards[1].trading_active = false; i.publish();
  CHECK(i.place(i.intent("Msess-1")).admission.code == "MARKET_CLOSED");
  F j; j.s.exchange.shards.clear(); j.publish(); CHECK(j.place(j.intent("Msess-1")).admission.ok());
  j.s.exchange.trading_active = false; j.publish(); CHECK(j.place(j.intent("Msess-2")).admission.code == "MARKET_CLOSED");
  F k; k.s.markets["TEST-A"].fee_ms = now - rest::kFeeFreshMs; k.publish();
  CHECK(k.place(k.intent("Msess-1")).admission.code == "RO");
  F l; MdBook stale = l.b; stale.stale = true; l.mb.set_book(stale);
  CHECK(l.place(l.intent("Msess-1")).admission.code == "STALE");
  F m; CHECK(m.place(m.intent("Msess-1")).admission.ok());
  m.h.killed = true; CHECK(m.place(m.intent("Msess-2")).admission.code == "KILLED");
  F n; n.s.clock.post_trusted = false; n.publish();
  auto post = n.intent("Msess-1", Side::BUY, 4100); post.tif = Tif::POST; post.ttl_s = 60;
  CHECK(n.place(post).admission.code == "STALE");
  CHECK(n.place(n.intent("Msess-2")).admission.ok());   // a bad clock blocks POST only
}

int main() {
  places_and_reserves(); reflection(); resting_not_twice(); replies(); reducing_room(); staleness();
  return finish("deskd-manual-book");
}
