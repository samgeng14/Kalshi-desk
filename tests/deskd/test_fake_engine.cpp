// The fake Kalshi without sockets: its wire formats, grids and fees, the matching engine and
// account, order groups, the street and its tape debts, and the tape and scenario readers.
//
// Expected numbers are worked by hand from the venue's documents (docs.kalshi.com, fetched
// 2026-10-06): fees from the formula in redesign_all kalshi K6 and the worked example in
// getting_started/fee_rounding.md; message fields from websockets/*.md; order rules from
// api-reference/orders/create-order-v2.md. Every file the test makes lives under $TMPDIR.
#include <csignal>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "check.h"
#include "deskd/build_paths.h"
#include "engine.h"
#include "fake_kalshi.h"
#include "tape.h"

using namespace fakex;

namespace {

constexpr std::int64_t T0 = 1791300600000;  // 2026-10-06T15:30:00Z, a whole second
constexpr Micros D = kMicrosPerDollar;
const std::string T = "KXTEST-26OCT06-A";            // shard 0, 1c grid
const std::string G = "KXNATGAS15M-26OCT061145-45";  // shard 2, tapered grid
using Levels = std::vector<std::pair<Px, Qty>>;

MarketSpec spec(const std::string& ticker, int shard = 0, FeeType fee = FeeType::QUADRATIC) {
  MarketSpec s;
  s.ticker = ticker;
  s.exchange_index = shard;
  s.fee_type = fee;
  if (shard == 2) {
    s.ticks = tapered_deci_cent();
    s.price_level_structure = "tapered_deci_cent";
  }
  return s;
}

NewOrder ord(const std::string& ticker, Side side, Px px, Qty count, Tif tif = Tif::GTC, const std::string& cid = "") {
  NewOrder n;
  n.ticker = ticker;
  n.side = side;
  n.price = px;
  n.count = count;
  n.tif = tif;
  n.client_order_id = cid;
  return n;
}

std::vector<Event> of_type(const std::vector<Event>& ev, const std::string& type) {
  std::vector<Event> out;
  for (const auto& e : ev)
    if (e.type == type) out.push_back(e);
  return out;
}

std::set<std::string> keys(const json& j) {
  std::set<std::string> k;
  for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
  return k;
}

bool subset(const std::set<std::string>& a, const std::set<std::string>& b) {
  for (const auto& x : a)
    if (!b.count(x)) {
      std::fprintf(stderr, "  unexpected field %s\n", x.c_str());
      return false;
    }
  return true;
}

json read_json(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return json::parse(ss.str(), nullptr, false);
}

Levels levels_of(const json& arr) {
  Levels out;
  for (const auto& lv : arr)
    out.emplace_back(*parse_fixed(lv[0].get<std::string>(), 4, false), *parse_fixed(lv[1].get<std::string>(), 2, false));
  return out;
}

// ---- wire formats ---------------------------------------------------------------------------------

void test_wire() {
  // FixedPointDollars requests: 2 to 4 decimals (create-order-v2.md)
  CHECK(parse_price_req("0.56") == 5600);
  CHECK(parse_price_req("0.5600") == 5600);
  CHECK(parse_price_req("0.001") == 10);
  CHECK(parse_price_req("0.0010") == 10);
  CHECK(!parse_price_req("0.5"));
  CHECK(!parse_price_req("0.56789"));
  CHECK(!parse_price_req(".56"));
  CHECK(!parse_price_req("0.56 "));
  CHECK(!parse_price_req("-0.56"));
  CHECK(!parse_price_req(""));
  CHECK(!parse_price_req("1e-2"));
  // FixedPointCount requests: 0 to 2 decimals, 0.01 granularity
  CHECK(parse_count_req("10") == 1000);
  CHECK(parse_count_req("10.5") == 1050);
  CHECK(parse_count_req("10.00") == 1000);
  CHECK(parse_count_req("0.01") == 1);
  CHECK(!parse_count_req("10.001"));
  CHECK(!parse_count_req("-1"));
  CHECK(!parse_count_req("1e3"));
  CHECK(!parse_count_req("10."));
  CHECK(!parse_count_req("1234567890123"));  // 13 integer digits
  CHECK(parse_fixed("-62.00", 2, true) == -6200);
  CHECK(!parse_fixed("-62.00", 2, false));
  CHECK(parse_fixed("2774.2", 2, false) == 277420);
  CHECK(parse_fixed("0.9980", 4, false) == 9980);

  CHECK(px_str(5600) == "0.5600");
  CHECK(px_str(10) == "0.0010");
  CHECK(qty_str(1000) == "10.00");
  CHECK(qty_str(-350) == "-3.50");
  CHECK(qty_str(7) == "0.07");
  CHECK(usd6(10000) == "0.010000");  // fee_cost in the fill example: "0.010000"
  CHECK(usd4(50 * D) == "50.0000");  // position_cost_dollars: "50.0000"
  CHECK(usd4(5612) == "0.005612");   // not a whole $0.0001: six places
  CHECK(usd4(-1500) == "-0.0015");
  CHECK(rfc3339(1791300600123) == "2026-10-06T15:30:00.123000Z");

  std::mt19937_64 a(7), b(7);
  const std::string u = uuid4(a);
  CHECK(u == uuid4(b));  // seeded: a run repeats
  CHECK(u.size() == 36 && u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-');
  CHECK(u[14] == '4');                                          // version 4
  CHECK(std::string("89ab").find(u[19]) != std::string::npos);  // RFC 4122 variant
  CHECK(uuid4(a) != u);
}

// ---- grids and fees -------------------------------------------------------------------------------

void test_grids_and_fees() {
  const auto lin = linear_cent(), tap = tapered_deci_cent();
  CHECK(on_grid(lin, 100) && on_grid(lin, 9900) && on_grid(lin, 4500));
  CHECK(!on_grid(lin, 150) && !on_grid(lin, 0) && !on_grid(lin, 10000) && !on_grid(lin, 50));
  // 0.1c below 10c and above 90c, 1c between (redesign_all kalshi K7)
  CHECK(on_grid(tap, 10) && on_grid(tap, 550) && on_grid(tap, 990) && on_grid(tap, 1000));
  CHECK(!on_grid(tap, 1010) && on_grid(tap, 1100) && on_grid(tap, 9000) && on_grid(tap, 9010) && on_grid(tap, 9990));
  CHECK(!on_grid(tap, 5) && !on_grid(tap, 9995) && !on_grid(tap, 4550));

  // taker 0.07 x C x P x (1 - P): one contract at 50c is $0.0175
  CHECK(model_fee(FeeType::QUADRATIC, 1000, true, 100, 5000) == 17500);
  CHECK(model_fee(FeeType::QUADRATIC, 1000, false, 100, 5000) == 0);  // no maker fee on quadratic
  CHECK(model_fee(FeeType::QUADRATIC_WITH_MAKER_FEES, 1000, false, 100, 5000) == 4375);        // 0.0175
  CHECK(model_fee(FeeType::QUADRATIC_WITH_COMBO_MAKER_FEES, 1000, false, 100, 5000) == 8750);  // 0.035
  CHECK(model_fee(FeeType::QUADRATIC, 500, true, 100, 5000) == 8750);  // multiplier 0.5
  CHECK(model_fee(FeeType::QUADRATIC, 0, true, 100, 5000) == 0);      // fee-free series
  CHECK(model_fee(FeeType::QUADRATIC, 1000, true, 1000, 1000) == 63000);  // 10 at 10c: $0.063
  // 0.07 contracts at 42c: $0.00119364, rounded up to the micro-dollar
  CHECK(model_fee(FeeType::QUADRATIC, 1000, true, 7, 4200) == 1194);

  // fee_rounding.md's worked FCM example: one contract bought at 5.5c, a $0.00363825 model fee,
  // a $0.01 precision: trade fee $0.003639, aligned change -$0.06, rounding fee $0.001361,
  // combined fees $0.005
  CHECK(model_fee(FeeType::QUADRATIC, 1000, true, 100, 550) == 3639);
  Micros acc = 0;
  CHECK(charge_fee(-55000, 3639, 10000, acc) == 5000);
  CHECK(acc == 1361);
  // ... and its accumulator: three fills over-rounded $0.004 each; the third gets $0.01 back and
  // $0.002 carries forward
  acc = 0;
  CHECK(charge_fee(-50000, 6000, 10000, acc) == 10000 && acc == 4000);
  CHECK(charge_fee(-50000, 6000, 10000, acc) == 10000 && acc == 8000);
  CHECK(charge_fee(-50000, 6000, 10000, acc) == 0 && acc == 2000);
  // "capped so the fill's net fee cannot be negative": a fill with no fee of its own gets no
  // rebate however full the accumulator
  acc = 50000;
  CHECK(charge_fee(-50000, 0, 10000, acc) == 0 && acc == 50000);
  // a direct member aligns to $0.0001
  acc = 0;
  CHECK(charge_fee(-55000, 3639, 100, acc) == 3700 && acc == 61);
  // a sale's positive change is floored too
  acc = 0;
  CHECK(charge_fee(1800000, 69250, 100, acc) == 69300);
}

// ---- matching ------------------------------------------------------------------------------------

void test_matching() {
  Engine e;
  e.add_market(spec(T), T0);
  e.set_cash(0, 0, 1000 * D, T0);
  e.street_add(T, false, 4000, 500, T0);  // YES bid 0.40 x5
  e.street_add(T, true, 5800, 300, T0);   // NO bid 0.58 x3, a YES ask at 0.42
  CHECK(e.levels(T, false) == Levels({{4000, 500}}));
  CHECK(e.levels(T, true) == Levels({{5800, 300}}));
  e.drain_events();

  // price-time: our bid joins the 0.40 queue behind the street's 5
  const auto a = e.create(ord(T, Side::BID, 4000, 200, Tif::GTC, "A"), T0 + 1);
  CHECK(!a.err && a.fill_count == 0 && a.remaining == 200 && a.client_order_id == "A" && a.ts_ms == T0 + 1);
  CHECK(e.levels(T, false) == Levels({{4000, 700}}));
  CHECK(e.street_take(T, Side::ASK, 4000, 600, T0 + 2) == 600);
  const Order* A = e.order(a.order_id);
  CHECK(A->filled == 100 && A->remaining == 100 && A->status == OrdStatus::RESTING && A->reason == "Trade");
  CHECK(e.fills().size() == 1 && !e.fills()[0].taker && e.fills()[0].yes_px == 4000 && e.fills()[0].count == 100);
  CHECK(e.fills()[0].fee == 0);             // no maker fee on a quadratic series
  CHECK(e.cash(0, 0) == 1000 * D - 400000);  // 1 contract at 40c

  // IOC: 3 fill at 0.42, the rest is cancelled
  const auto b = e.create(ord(T, Side::BID, 4200, 500, Tif::IOC, "B"), T0 + 3);
  const Order* B = e.order(b.order_id);
  CHECK(b.fill_count == 300 && b.remaining == 0 && B->status == OrdStatus::CANCELED && B->reason == "Trade");
  CHECK(b.fill_cost == notional(4200, 300));
  // taker fee 0.07 x 3 x 0.42 x 0.58 = $0.051156; the balance change -$1.311156 aligns to
  // -$1.3112, so $0.0512 is charged
  CHECK(b.fees == 51200);
  CHECK(e.levels(T, true).empty());
  // an IOC that finds nothing: cancelled with no reason
  const auto b2 = e.create(ord(T, Side::BID, 4200, 100, Tif::IOC), T0 + 3);
  CHECK(b2.fill_count == 0 && e.order(b2.order_id)->status == OrdStatus::CANCELED && e.order(b2.order_id)->reason.empty());

  // FOK: all or nothing
  e.street_add(T, true, 5700, 200, T0 + 4);  // YES ask 0.43 x2
  const auto c = e.create(ord(T, Side::BID, 4300, 300, Tif::FOK), T0 + 5);
  CHECK(!c.err && c.fill_count == 0 && e.order(c.order_id)->status == OrdStatus::CANCELED);
  CHECK(e.levels(T, true) == Levels({{5700, 200}}));
  const auto d = e.create(ord(T, Side::BID, 4300, 200, Tif::FOK), T0 + 6);
  CHECK(d.fill_count == 200 && e.order(d.order_id)->status == OrdStatus::EXECUTED);

  // post only: a crossing one is refused 400 "post only cross" (what the venue told the old desk)
  e.street_add(T, true, 5500, 100, T0 + 7);  // YES ask 0.45
  NewOrder p = ord(T, Side::BID, 4500, 100);
  p.post_only = true;
  const auto p1 = e.create(p, T0 + 8);
  CHECK(p1.err.http == 400 && p1.err.code == "invalid_order" && p1.err.details == "post only cross");
  CHECK(p1.order_id.empty());
  p.price = 4400;
  const auto p2 = e.create(p, T0 + 8);
  CHECK(!p2.err && e.order(p2.order_id)->status == OrdStatus::RESTING);
  // ... or, as user-orders.md's PostOnlyCrossCancel has it, accepted and cancelled at once
  e.config().post_only_cross = PostOnlyCross::CANCEL;
  p.price = 4600;
  const auto p3 = e.create(p, T0 + 8);
  CHECK(!p3.err && p3.fill_count == 0 && p3.remaining == 0);
  CHECK(e.order(p3.order_id)->status == OrdStatus::CANCELED && e.order(p3.order_id)->reason == "PostOnlyCrossCancel");
  CHECK(e.levels(T, true) == Levels({{5500, 100}}));
  e.config().post_only_cross = PostOnlyCross::REFUSE_400;
  p.tif = Tif::IOC;
  CHECK(e.create(p, T0 + 8).err.http == 400);
  CHECK(!e.cancel(p2.order_id, T, -2, 0, T0 + 9).err);

  // expiry: whole Unix seconds, GTC only, cancelled at the second
  const std::int64_t now = T0 + 10000;
  NewOrder x = ord(T, Side::BID, 3000, 100);
  x.expiration_s = now / 1000 + 2;
  const auto xr = e.create(x, now);
  CHECK(!xr.err);
  e.tick(now + 1999);
  CHECK(e.order(xr.order_id)->status == OrdStatus::RESTING);
  e.tick(now + 2000);
  CHECK(e.order(xr.order_id)->status == OrdStatus::CANCELED && e.order(xr.order_id)->reason == "ExpiryCancel");
  x.tif = Tif::IOC;
  CHECK(e.create(x, now).err.details == "expiration_time requires good_till_canceled");
  x.tif = Tif::GTC;
  x.expiration_s = now / 1000;
  CHECK(e.create(x, now).err.details == "expiration_time is not in the future");
  e.config().min_expiry_lead_s = 2;  // the venue's minimum is undocumented (K1): settable
  x.expiration_s = now / 1000 + 2;
  CHECK(e.create(x, now).err.http == 400);
  x.expiration_s = now / 1000 + 3;
  CHECK(!e.create(x, now).err);
  e.config().min_expiry_lead_s = 0;
  e.config().expiry_delay_ms = 400;  // a venue whose expiry runs late
  x.expiration_s = now / 1000 + 1;
  const auto xl = e.create(x, now);
  e.tick(now + 1399);
  CHECK(e.order(xl.order_id)->status == OrdStatus::RESTING);
  e.tick(now + 1400);
  CHECK(e.order(xl.order_id)->reason == "ExpiryCancel");
  e.config().expiry_delay_ms = 0;

  // reduce_only: IOC only, capped by the position (6 YES now: 1 + 3 + 2)
  CHECK(e.positions().at({0, T}).pos == 600);
  NewOrder r = ord(T, Side::ASK, 4100, 1000, Tif::GTC);
  r.reduce_only = true;
  CHECK(e.create(r, now).err.details == "reduce_only requires immediate_or_cancel");
  r.tif = Tif::IOC;
  NewOrder r_up = r;
  r_up.side = Side::BID;  // buying more YES reduces nothing
  const auto ru = e.create(r_up, now);
  CHECK(!ru.err && ru.fill_count == 0 && e.order(ru.order_id)->reason == "ReduceOnlyCancel");
  e.street_add(T, false, 4100, 1000, now);  // a street YES bid at 0.41 x10
  const auto rr = e.create(r, now);
  CHECK(rr.fill_count == 600 && e.positions().at({0, T}).pos == 0);

  // duplicate client_order_id: 409, even for an order long gone
  const auto dup = e.create(ord(T, Side::BID, 3000, 100, Tif::GTC, "A"), now);
  CHECK(dup.err.http == 409 && dup.err.code == "order_already_exists");
  const auto dup2 = e.create(ord(T, Side::BID, 3000, 100, Tif::IOC, "B"), now);
  CHECK(dup2.err.http == 409);
  // no client id: never a duplicate
  CHECK(!e.create(ord(T, Side::BID, 3000, 100, Tif::IOC), now).err);
  CHECK(!e.create(ord(T, Side::BID, 3000, 100, Tif::IOC), now).err);

  // validation
  CHECK(e.create(ord(T, Side::BID, 4050, 100), now).err.details == "price is not on the market's price grid");
  CHECK(e.create(ord(T, Side::BID, 4000, 0), now).err.http == 400);
  CHECK(e.create(ord("NOPE", Side::BID, 4000, 100), now).err.http == 404);
  NewOrder wrong = ord(T, Side::BID, 4000, 100);
  wrong.exchange_index = 2;  // routed to a shard that does not hold T
  CHECK(e.create(wrong, now).err.http == 404);
  wrong.exchange_index = 0;
  CHECK(!e.create(wrong, now).err);
}

void test_self_trade() {
  Engine e;
  e.add_market(spec(T), T0);
  e.set_cash(0, 0, 100 * D, T0);
  e.street_add(T, true, 5600, 100, T0);                         // street YES ask 0.44 x1
  const auto own = e.create(ord(T, Side::ASK, 4500, 200), T0);  // our ask 0.45 x2 (a NO bid at 0.55)
  CHECK(!own.err && e.order(own.order_id)->status == OrdStatus::RESTING);
  e.street_add(T, true, 5400, 200, T0);  // street YES ask 0.46 x2

  // taker_at_cross: "cancels the taker order ... any partial fills already matched are executed"
  NewOrder t = ord(T, Side::BID, 4600, 500, Tif::IOC);
  t.stp = Stp::TAKER_AT_CROSS;
  const auto t1 = e.create(t, T0 + 1);
  CHECK(t1.fill_count == 100 && t1.remaining == 0);
  CHECK(e.order(t1.order_id)->status == OrdStatus::CANCELED && e.order(t1.order_id)->reason == "SelfTradeCancel");
  CHECK(e.order(own.order_id)->status == OrdStatus::RESTING && e.order(own.order_id)->remaining == 200);

  // maker: "cancels the resting maker order and continues matching"
  t.stp = Stp::MAKER;
  t.count = 300;
  const auto t2 = e.create(t, T0 + 2);
  CHECK(e.order(own.order_id)->status == OrdStatus::CANCELED && e.order(own.order_id)->reason == "SelfTradeCancel");
  CHECK(t2.fill_count == 200 && e.order(t2.order_id)->status == OrdStatus::CANCELED);
  CHECK(e.levels(T, true).empty());

  // another subaccount is not "self" here (undocumented on the venue: plan decision 8)
  e.set_cash(1, 0, 100 * D, T0);
  NewOrder s1 = ord(T, Side::ASK, 4500, 100);
  s1.subaccount = 1;
  CHECK(!e.create(s1, T0 + 3).err);
  const auto t3 = e.create(ord(T, Side::BID, 4500, 100, Tif::IOC), T0 + 3);
  CHECK(t3.fill_count == 100);
  CHECK(e.positions().at({1, T}).pos == -100 && e.cash(1, 0) == 100 * D - 550000);

  // fill_or_kill counts only what it could take before its own order stops it
  e.create(ord(T, Side::ASK, 4700, 100), T0 + 4);  // ours at 0.47
  e.street_add(T, true, 5200, 500, T0 + 4);         // the street's 5 at 0.48 behind it
  NewOrder fok = ord(T, Side::BID, 4800, 300, Tif::FOK);
  CHECK(e.create(fok, T0 + 5).fill_count == 0);
  fok.stp = Stp::MAKER;  // ours goes, and the street's 5 suffice
  CHECK(e.create(fok, T0 + 5).fill_count == 300);
}

void test_pause_and_close() {
  Engine e;
  MarketSpec s = spec(T);
  s.close_ms = T0 + 60000;
  e.add_market(s, T0);
  e.set_cash(0, 0, 100 * D, T0);
  NewOrder a = ord(T, Side::BID, 3000, 100);
  a.cancel_on_pause = true;
  const auto o1 = e.create(a, T0);
  const auto o2 = e.create(ord(T, Side::BID, 3100, 100), T0);
  e.drain_events();
  e.pause(T, T0 + 1);
  CHECK(e.order(o1.order_id)->status == OrdStatus::CANCELED && e.order(o1.order_id)->reason == "HaltCancel");
  CHECK(e.order(o2.order_id)->status == OrdStatus::RESTING);
  CHECK(e.create(ord(T, Side::BID, 3000, 100), T0 + 2).err.code == "trading_paused");
  e.street_add(T, false, 2000, 100, T0 + 2);  // the book does not move while paused
  CHECK(e.levels(T, false) == Levels({{3100, 100}}));
  e.resume(T, T0 + 3);
  CHECK(!e.create(ord(T, Side::BID, 3000, 100), T0 + 4).err);
  // a market described again keeps its book and our orders
  e.add_market(s, T0 + 4);
  CHECK(e.levels(T, false) == Levels({{3000, 100}, {3100, 100}}));
  e.drain_events();

  e.tick(T0 + 59999);
  CHECK(e.market(T)->state == Market::State::OPEN);
  e.tick(T0 + 60000);
  CHECK(e.market(T)->state == Market::State::CLOSED);
  CHECK(e.order(o2.order_id)->status == OrdStatus::CANCELED && e.order(o2.order_id)->reason == "CloseCancel");
  CHECK(e.levels(T, false).empty());
  const auto ev = e.drain_events();
  // the venue sends a closing market one empty snapshot, not a delta per level
  const auto snaps = of_type(ev, "orderbook_snapshot");
  CHECK(snaps.size() == 1 && snaps[0].reset_book && snaps[0].seq && snaps[0].ticker == T);
  CHECK(of_type(ev, "orderbook_delta").empty());
  CHECK(of_type(ev, "user_order").size() == 2);
  CHECK(e.create(ord(T, Side::BID, 3000, 100), T0 + 60001).err.code == "market_closed");
}

// ---- the account ----------------------------------------------------------------------------------

void test_account() {
  Engine e;
  e.add_market(spec(G, 2), T0);
  e.set_cash(0, 0, 100 * D, T0);
  e.street_add(G, true, 6000, 2000, T0);   // YES ask 0.40 x20
  e.street_add(G, false, 3800, 2000, T0);  // YES bid 0.38 x20
  // collateral lives on the market's shard; API orders never move it (exchange_sharding.md)
  CHECK(e.create(ord(G, Side::BID, 4000, 1000, Tif::IOC), T0).err.code == "insufficient_balance");
  std::string id;
  CHECK(!e.transfer(0, 0, 2, 50 * D, T0, &id) && !id.empty());
  CHECK(e.cash(0, 0) == 50 * D && e.cash(0, 2) == 50 * D);
  CHECK(e.transfer(0, 0, 2, 51 * D, T0, &id).code == "insufficient_balance");
  CHECK(e.transfer(0, 2, 2, D, T0, &id).http == 400);

  // buy 10 YES at 0.40 as taker: $4.00 plus 0.07 x 10 x 0.4 x 0.6 = $0.168
  const auto b = e.create(ord(G, Side::BID, 4000, 1000, Tif::IOC), T0 + 1);
  CHECK(b.fill_count == 1000 && b.fees == 168000);
  CHECK(e.cash(0, 2) == 50 * D - 4168000 && e.cash(0, 0) == 50 * D);
  const Position& p = e.positions().at({0, G});
  CHECK(p.pos == 1000 && p.cost == 4 * D && p.fees == 168000 && p.pos_fees == 168000 && p.exchange_index == 2);
  CHECK(e.fills().back().opened && e.fills().back().post_position == 1000);

  // sell 4 YES at 0.45 into a street bid: Kalshi nets, and each closed pair is worth $1
  e.street_remove(G, true, 6000, 1000, T0 + 2);
  e.street_remove(G, false, 3800, 2000, T0 + 2);
  e.street_add(G, false, 4500, 1000, T0 + 2);  // YES bid 0.45 x10
  const auto s = e.create(ord(G, Side::ASK, 4500, 400, Tif::IOC), T0 + 3);
  CHECK(s.fill_count == 400 && s.fees == 69300);  // 0.07 x 4 x 0.45 x 0.55
  // +$1.80 for the pairs (4 x $1, less 4 NO at 55c), less the fee
  CHECK(e.cash(0, 2) == 50 * D - 4168000 + 1800000 - 69300);
  const Position& q = e.positions().at({0, G});
  CHECK(q.pos == 600 && q.cost == 2400000 && q.realized == 200000);  // 4 x (45c - 40c)
  CHECK(q.pos_fees == 100800 && q.fees == 237300 && q.volume == 1400);
  CHECK(!e.fills().back().opened && e.fills().back().post_position == 600);

  // reservations: a resting bid holds its cost; a resting ask our YES covers holds nothing
  const Micros cash2 = e.cash(0, 2);
  NewOrder bid = ord(G, Side::BID, 3000, 1000);
  bid.post_only = true;
  CHECK(!e.create(bid, T0 + 4).err);
  CHECK(e.reserved(0, 2) == 3 * D && e.available(0, 2) == cash2 - 3 * D);
  NewOrder ask = ord(G, Side::ASK, 5000, 500);
  ask.post_only = true;
  CHECK(!e.create(ask, T0 + 5).err);
  CHECK(e.reserved(0, 2) == 3 * D);
  ask.count = 300;  // 1 more is covered, 2 are not: 2 NO at 50c
  CHECK(!e.create(ask, T0 + 6).err);
  CHECK(e.reserved(0, 2) == 4 * D);
  CHECK(e.reserved(0, 0) == 0);
  CHECK(e.create(ord(G, Side::BID, 9000, 100000), T0 + 7).err.code == "insufficient_balance");
  // marked at the held side's best bid: 6 YES at 0.45
  CHECK(e.position_value(0, 2) == 6 * 450000);
  CHECK(e.shards() == std::set<int>({0, 2}));
}

// ---- order groups ---------------------------------------------------------------------------------

void test_groups() {
  Engine e;
  e.add_market(spec(T), T0);
  e.add_market(spec(G, 2), T0);
  e.set_cash(0, 0, 1000 * D, T0);
  e.set_cash(0, 2, 1000 * D, T0);
  const std::string g = e.group_create(0, 0, 500, T0);  // 5 contracts
  auto ev = e.drain_events();
  CHECK(ev.size() == 1 && ev[0].type == "order_group_updates" && ev[0].seq && ev[0].ticker.empty());
  CHECK(ev[0].msg["event_type"] == "created" && ev[0].msg["order_group_id"] == g && ev[0].msg["contracts_limit_fp"] == "5.00");

  auto in_group = [&](const std::string& t, Side side, Px px, Qty q) {
    NewOrder n = ord(t, side, px, q);
    n.order_group_id = g;
    return n;
  };
  // groups do not span shards (exchange_sharding.md)
  CHECK(e.create(in_group(G, Side::BID, 4000, 100), T0).err.details == "order group not found");
  const auto o1 = e.create(in_group(T, Side::BID, 4000, 300), T0);
  const auto o2 = e.create(in_group(T, Side::ASK, 6000, 300), T0);
  const auto o3 = e.create(in_group(T, Side::BID, 3500, 200), T0);
  CHECK(!o1.err && !o2.err && !o3.err);
  e.street_take(T, Side::ASK, 4000, 300, T0 + 1);
  CHECK(!e.group(g)->triggered && e.group_window(*e.group(g), T0 + 1) == 300);
  e.drain_events();
  // 6 filled in 15 s is over 5: "If the rolling total exceeds the limit, the group is triggered"
  e.street_take(T, Side::BID, 6000, 300, T0 + 2);
  CHECK(e.group(g)->triggered);
  CHECK(e.order(o3.order_id)->status == OrdStatus::CANCELED);
  ev = e.drain_events();
  bool saw = false;
  for (const auto& x : of_type(ev, "order_group_updates")) saw = saw || x.msg["event_type"] == "triggered";
  CHECK(saw);
  CHECK(e.create(in_group(T, Side::BID, 3000, 100), T0 + 3).err.details == "order group triggered");
  CHECK(!e.create(ord(T, Side::BID, 3000, 100), T0 + 3).err);  // outside the group: fine
  e.drain_events();

  CHECK(!e.group_reset(g, 0, 0, T0 + 4));
  CHECK(!e.group(g)->triggered && e.group_window(*e.group(g), T0 + 4) == 0);
  ev = e.drain_events();
  CHECK(ev.size() == 1 && ev[0].msg["event_type"] == "reset");

  // the window rolls: 4 now and 4 more 15 s later never exceed 5
  e.create(in_group(T, Side::BID, 4000, 800), T0 + 10);
  e.street_take(T, Side::ASK, 4000, 400, T0 + 10);
  e.street_take(T, Side::ASK, 4000, 400, T0 + 10 + 15000);
  CHECK(!e.group(g)->triggered && e.group_window(*e.group(g), T0 + 10 + 15000) == 400);

  // lowering the limit under the window triggers it (update-order-group-limit.md)
  CHECK(e.group_limit(g, 0, 0, 50, T0 + 15011).code == "invalid_parameters");
  e.drain_events();
  CHECK(!e.group_limit(g, 0, 0, 300, T0 + 15011));
  CHECK(e.group(g)->triggered && e.group(g)->limit == 300);
  ev = e.drain_events();
  CHECK(ev.size() == 2 && ev[0].msg["event_type"] == "limit_updated" && ev[0].msg["contracts_limit_fp"] == "3.00" &&
        ev[1].msg["event_type"] == "triggered");

  // a manual trigger cancels what rests; delete cancels and removes
  e.group_reset(g, 0, 0, T0 + 20000);
  const auto o4 = e.create(in_group(T, Side::BID, 3000, 100), T0 + 20000);
  CHECK(!e.group_trigger(g, 0, 0, T0 + 20001));
  CHECK(e.order(o4.order_id)->status == OrdStatus::CANCELED);
  e.group_reset(g, 0, 0, T0 + 20002);
  const auto o5 = e.create(in_group(T, Side::BID, 3000, 100), T0 + 20002);
  CHECK(e.group_trigger(g, 1, 0, T0 + 20003).http == 404);  // another subaccount's view
  CHECK(e.group_trigger(g, 0, 2, T0 + 20003).http == 404);  // another shard
  e.drain_events();
  CHECK(!e.group_delete(g, 0, 0, T0 + 20004));
  CHECK(e.order(o5.order_id)->status == OrdStatus::CANCELED && !e.group(g));
  ev = e.drain_events();
  CHECK(of_type(ev, "order_group_updates").size() == 1 &&
        of_type(ev, "order_group_updates")[0].msg["event_type"] == "deleted");
  CHECK(e.create(in_group(T, Side::BID, 3000, 100), T0 + 20005).err.details == "order group not found");
  CHECK(e.group_reset(g, 0, 0, T0 + 20005).http == 404);
}

// ---- what the socket would say ---------------------------------------------------------------------

void test_events() {
  Engine e;
  e.add_market(spec(T), T0);
  e.set_cash(0, 0, 100 * D, T0);
  e.set_cash(3, 0, 100 * D, T0);
  e.street_add(T, true, 5800, 300, T0);
  e.street_add(T, false, 4000, 300, T0);
  e.drain_events();
  e.create(ord(T, Side::BID, 4200, 100, Tif::IOC, "E1"), T0 + 1);
  const auto ev = e.drain_events();
  // documented fields (websockets/user-fills.md, public-trades.md, market-positions.md,
  // market-ticker.md, orderbook-updates.md, user-orders.md)
  const auto fills = of_type(ev, "fill");
  CHECK(fills.size() == 1 && fills[0].channel == "fill" && !fills[0].seq);
  CHECK(keys(fills[0].msg) == std::set<std::string>({"trade_id", "order_id", "client_order_id", "market_ticker",
                                                     "exchange_index", "is_taker", "side", "yes_price_dollars",
                                                     "count_fp", "fee_cost", "action", "ts", "ts_ms",
                                                     "post_position_fp", "purchased_side", "outcome_side",
                                                     "book_side"}));
  const json& f = fills[0].msg;
  CHECK(f["is_taker"] == true && f["side"] == "yes" && f["book_side"] == "bid" && f["yes_price_dollars"] == "0.4200");
  CHECK(f["count_fp"] == "1.00" && f["post_position_fp"] == "1.00" && f["action"] == "buy" && f["ts_ms"] == T0 + 1);
  CHECK(f["fee_cost"] == "0.017100");  // 0.07 x 0.42 x 0.58 = $0.017052, aligned to $0.0001
  const auto trades = of_type(ev, "trade");
  CHECK(trades.size() == 1 && trades[0].seq && trades[0].msg["trade_id"] == f["trade_id"]);
  CHECK(keys(trades[0].msg) == std::set<std::string>({"trade_id", "market_ticker", "yes_price_dollars",
                                                      "no_price_dollars", "count_fp", "taker_side",
                                                      "taker_outcome_side", "taker_book_side", "is_block_trade",
                                                      "ts", "ts_ms"}));
  CHECK(trades[0].msg["taker_book_side"] == "bid" && trades[0].msg["no_price_dollars"] == "0.5800");
  const auto uo = of_type(ev, "user_order");
  CHECK(uo.size() == 1 && !uo[0].seq && uo[0].channel == "user_orders");
  CHECK(subset(keys(uo[0].msg),
               {"order_id", "user_id", "ticker", "exchange_index", "status", "side", "is_yes", "outcome_side",
                "book_side", "yes_price_dollars", "fill_count_fp", "remaining_count_fp", "initial_count_fp",
                "taker_fill_cost_dollars", "maker_fill_cost_dollars", "taker_fees_dollars", "maker_fees_dollars",
                "client_order_id", "order_group_id", "self_trade_prevention_type", "created_time",
                "last_update_time", "expiration_time", "created_ts_ms", "last_updated_ts_ms", "expiration_ts_ms",
                "last_update_reason", "subaccount_number"}));
  CHECK(uo[0].msg["status"] == "executed" && uo[0].msg["fill_count_fp"] == "1.00" && uo[0].msg["remaining_count_fp"] == "0.00");
  CHECK(uo[0].msg["taker_fill_cost_dollars"] == "0.420000" && uo[0].msg["last_update_reason"] == "Trade");
  const auto mp = of_type(ev, "market_position");
  CHECK(mp.size() == 1 && !mp[0].seq);
  CHECK(keys(mp[0].msg) == std::set<std::string>({"user_id", "market_ticker", "position_fp", "position_cost_dollars",
                                                  "realized_pnl_dollars", "fees_paid_dollars",
                                                  "position_fee_cost_dollars", "volume_fp"}));
  CHECK(mp[0].msg["position_fp"] == "1.00" && mp[0].msg["position_cost_dollars"] == "0.4200");
  const auto deltas = of_type(ev, "orderbook_delta");
  CHECK(deltas.size() == 1 && deltas[0].seq);
  // "Present only when you caused this orderbook change"
  CHECK(deltas[0].msg["client_order_id"] == "E1" && deltas[0].msg["side"] == "no" &&
        deltas[0].msg["price_dollars"] == "0.5800" && deltas[0].msg["delta_fp"] == "-1.00");
  const auto tick = of_type(ev, "ticker");
  CHECK(tick.size() == 1 && !tick[0].seq);
  CHECK(keys(tick[0].msg) == std::set<std::string>({"market_id", "market_ticker", "price_dollars", "yes_bid_dollars",
                                                    "yes_ask_dollars", "volume_fp", "open_interest_fp",
                                                    "dollar_volume", "dollar_open_interest", "yes_bid_size_fp",
                                                    "yes_ask_size_fp", "last_trade_size_fp", "ts", "ts_ms",
                                                    "time"}));
  CHECK(tick[0].msg["yes_ask_size_fp"] == "2.00" && tick[0].msg["price_dollars"] == "0.4200");

  // the street's change carries no client id; the ticker speaks only when a field changes
  e.street_add(T, false, 3000, 100, T0 + 2);  // below the best bid: nothing in the ticker moves
  auto ev2 = e.drain_events();
  CHECK(ev2.size() == 1 && ev2[0].type == "orderbook_delta" && !ev2[0].msg.contains("client_order_id"));
  e.street_add(T, false, 4000, 100, T0 + 3);  // the best bid's size moves
  ev2 = e.drain_events();
  CHECK(of_type(ev2, "ticker").size() == 1 && of_type(ev2, "ticker")[0].msg["yes_bid_size_fp"] == "4.00");

  // a subaccount shows on every private message
  NewOrder s = ord(T, Side::BID, 4200, 100, Tif::IOC, "S3-1");
  s.subaccount = 3;
  e.create(s, T0 + 4);
  ev2 = e.drain_events();
  CHECK(of_type(ev2, "fill")[0].msg["subaccount"] == 3);
  CHECK(of_type(ev2, "user_order")[0].msg["subaccount_number"] == 3);
  CHECK(of_type(ev2, "market_position")[0].msg["subaccount"] == 3);
  CHECK(of_type(ev2, "orderbook_delta")[0].msg["subaccount"] == 3);
  // a resting order with an expiry and a group says so
  const std::string g = e.group_create(0, 0, 1000, T0);
  NewOrder r = ord(T, Side::BID, 3500, 100, Tif::GTC, "R1");
  r.expiration_s = T0 / 1000 + 30;
  r.order_group_id = g;
  e.drain_events();
  e.create(r, T0 + 5);
  ev2 = e.drain_events();
  const json u = of_type(ev2, "user_order")[0].msg;
  CHECK(u["status"] == "resting" && u["remaining_count_fp"] == "1.00" && u["order_group_id"] == g);
  CHECK(u["expiration_ts_ms"] == T0 + 30000 && !u.contains("last_update_reason"));
}

void test_cancel() {
  Engine e;
  e.add_market(spec(G, 2), T0);
  e.set_cash(0, 2, 100 * D, T0);
  e.set_cash(1, 2, 100 * D, T0);
  auto rest = [&](int sub) {
    NewOrder n = ord(G, Side::BID, 3000, 500);
    n.subaccount = sub;
    return e.create(n, T0).order_id;
  };
  // routing (cancel-order-v2.md): exchange_index defaults to 0 "or auto-routes if market_ticker
  // is provided"; -1 requires the ticker
  const std::string o = rest(0);
  CHECK(e.cancel(o, "", -2, 0, T0).err.http == 404);
  CHECK(e.cancel(o, "", 0, 0, T0).err.http == 404);
  CHECK(e.cancel(o, "NOPE", -2, 0, T0).err.http == 404);
  CHECK(e.cancel(o, "", -1, 0, T0).err.http == 400);
  CHECK(e.cancel(o, G, -1, 1, T0).err.http == 404);  // another subaccount's order is not found
  const auto c = e.cancel(o, G, -2, 0, T0 + 1);
  CHECK(!c.err && c.reduced_by == 500 && c.ts_ms == T0 + 1 && c.order_id == o);
  CHECK(e.cancel(o, G, -2, 0, T0 + 2).err.http == 404);  // gone
  CHECK(!e.cancel(rest(0), "", 2, 0, T0).err);
  // a partly filled order cancels its remainder
  const std::string pf = rest(0);
  e.street_take(G, Side::ASK, 3000, 200, T0);
  CHECK(e.cancel(pf, G, -1, 0, T0).reduced_by == 300);
  CHECK(e.order(pf)->filled == 200 && e.order(pf)->status == OrdStatus::CANCELED);

  // cancel-all: one subaccount, or "Omitting defaults to all subaccounts"
  const std::string a0 = rest(0), a1 = rest(1);
  CHECK(e.cancel_all(1, T0 + 3) == 1);
  CHECK(e.order(a0)->status == OrdStatus::RESTING && e.order(a1)->status == OrdStatus::CANCELED);
  CHECK(e.cancel_all(std::nullopt, T0 + 3) == 1 && e.order(a0)->status == OrdStatus::CANCELED);
  CHECK(e.reserved(0, 2) == 0);
  // "Newly placed orders may also be cancelled during the minute after the request": the 201
  // reports the order placed, and user_orders then reports it cancelled
  e.config().cancel_all_echo_ms = 60000;
  e.cancel_all(std::nullopt, T0 + 10000);
  const auto late = e.create(ord(G, Side::BID, 3000, 100), T0 + 40000);
  CHECK(!late.err && late.remaining == 100 && e.order(late.order_id)->status == OrdStatus::CANCELED);
  const auto later = e.create(ord(G, Side::BID, 3000, 100), T0 + 70001);
  CHECK(e.order(later.order_id)->status == OrdStatus::RESTING);
}

// ---- the street -----------------------------------------------------------------------------------

void test_street() {
  for (const StreetCancel how : {StreetCancel::BACK_FIRST, StreetCancel::FRONT_FIRST}) {
    EngineConfig cfg;
    cfg.street_cancel = how;
    Engine e(cfg);
    e.add_market(spec(T), T0);
    e.set_cash(0, 0, 100 * D, T0);
    e.street_add(T, false, 4000, 500, T0);                           // the street's 5 ahead
    const auto o = e.create(ord(T, Side::BID, 4000, 200), T0 + 1);  // ours, 2
    e.street_add(T, false, 4000, 300, T0 + 2);                       // the street's 3 behind
    e.street_remove(T, false, 4000, 400, T0 + 3);
    CHECK(e.levels(T, false) == Levels({{4000, 600}}));
    // BACK_FIRST: the 3 behind us go, then 1 of the 5 ahead, so 4 are still ahead. FRONT_FIRST:
    // 4 of the 5 ahead go, so 1 is. A removal never takes ours.
    const Qty ahead = how == StreetCancel::BACK_FIRST ? 400 : 100;
    e.street_take(T, Side::ASK, 4000, ahead, T0 + 4);
    CHECK(e.order(o.order_id)->filled == 0);
    e.street_take(T, Side::ASK, 4000, 100, T0 + 5);
    CHECK(e.order(o.order_id)->filled == 100);
    // a snapshot sets the street's sizes and leaves our place
    e.street_snapshot(T, {{4000, 900}, {3900, 100}}, {{5500, 200}}, T0 + 6);
    CHECK(e.levels(T, false) == Levels({{3900, 100}, {4000, 1000}}));
    CHECK(e.levels(T, true) == Levels({{5500, 200}}));
    e.street_take(T, Side::ASK, 4000, 100, T0 + 7);
    CHECK(e.order(o.order_id)->filled == 200);  // ours was first in the queue
  }

  // tape debts: street quantity our order took is not taken again by the tape's own removal
  Engine e;
  e.add_market(spec(T), T0);
  e.set_cash(0, 0, 100 * D, T0);
  e.set_tape_debts(true);
  e.street_add(T, true, 5800, 300, T0);
  CHECK(e.create(ord(T, Side::BID, 4200, 200, Tif::IOC), T0 + 1).fill_count == 200);
  CHECK(e.levels(T, true) == Levels({{5800, 100}}));
  e.street_remove(T, true, 5800, 200, T0 + 2);  // the tape sees the venue's book lose our 2
  CHECK(e.levels(T, true) == Levels({{5800, 100}}));
  e.street_remove(T, true, 5800, 100, T0 + 3);
  CHECK(e.levels(T, true).empty());
  // ... for tape_debt_ms; after that a removal is applied as written
  e.street_add(T, true, 5800, 300, T0 + 4);
  CHECK(e.create(ord(T, Side::BID, 4200, 100, Tif::IOC), T0 + 5).fill_count == 100);
  e.tick(T0 + 5 + 2001);
  e.street_remove(T, true, 5800, 100, T0 + 5 + 2001);
  CHECK(e.levels(T, true) == Levels({{5800, 100}}));
  // a street order that crosses ours trades with it, and the tape owes that level
  const auto rb = e.create(ord(T, Side::BID, 4100, 200), T0 + 9000);
  e.street_add(T, true, 5900, 300, T0 + 9001);  // a YES ask at 0.41 crosses our 0.41 bid
  CHECK(e.order(rb.order_id)->filled == 200 && e.levels(T, true) == Levels({{5800, 100}, {5900, 100}}));
  e.street_remove(T, true, 5900, 200, T0 + 9002);  // the tape's own trade with what it thought rested
  CHECK(e.levels(T, true) == Levels({{5800, 100}, {5900, 100}}));
  // a snapshot is the whole book: it clears the debts
  CHECK(e.create(ord(T, Side::BID, 4200, 100, Tif::IOC), T0 + 9004).fill_count == 100);  // owes 1 at 0.41
  e.street_snapshot(T, {}, {{5800, 100}, {5900, 300}}, T0 + 9005);
  e.street_remove(T, true, 5900, 100, T0 + 9006);
  CHECK(e.levels(T, true) == Levels({{5800, 100}, {5900, 200}}));
}

// ---- tapes and scenarios --------------------------------------------------------------------------

void test_tickers() {
  // KXNATGAS15M-26SEP241145-45 closes 11:45 New York time, 15:45Z (the recorded 11:30 windows
  // closed at 15:30:00Z)
  CHECK(rfc3339(*close_ms_from_ticker("KXNATGAS15M-26SEP241145-45")) == "2026-09-24T15:45:00.000000Z");
  CHECK(rfc3339(*close_ms_from_ticker("KXGOLD15M-26DEC011200-00")) == "2026-12-01T17:00:00.000000Z");  // EST
  CHECK(rfc3339(*close_ms_from_ticker("KXINXU-26OCT0617-T5000")) == "2026-10-06T21:00:00.000000Z");    // hourly
  // daylight time from 02:00 on 2026-03-08 to 02:00 on 2026-11-01
  CHECK(rfc3339(*close_ms_from_ticker("X-26MAR080100-00")) == "2026-03-08T06:00:00.000000Z");
  CHECK(rfc3339(*close_ms_from_ticker("X-26MAR080300-00")) == "2026-03-08T07:00:00.000000Z");
  CHECK(rfc3339(*close_ms_from_ticker("X-26NOV010100-00")) == "2026-11-01T05:00:00.000000Z");
  CHECK(rfc3339(*close_ms_from_ticker("X-26NOV010300-00")) == "2026-11-01T08:00:00.000000Z");
  CHECK(!close_ms_from_ticker("X-26FOO011200-00"));
  CHECK(!close_ms_from_ticker("KXHIGHNY-26OCT06-B70"));
  CHECK(!close_ms_from_ticker("KXBTC"));
  CHECK(!close_ms_from_ticker("X-26OCT062500-00"));

  const MarketSpec ng = default_spec("KXNATGAS15M-26OCT061145-45");
  CHECK(ng.exchange_index == 2 && ng.price_level_structure == "tapered_deci_cent" && on_grid(ng.ticks, 50));
  CHECK(ng.close_ms == *close_ms_from_ticker("KXNATGAS15M-26OCT061145-45"));
  const MarketSpec fx = default_spec("KXEURUSD15M-26OCT061145-45");
  CHECK(fx.exchange_index == 0 && !on_grid(fx.ticks, 50));
  const MarketSpec wx = default_spec("KXHIGHNY-26OCT06-B70");
  CHECK(wx.exchange_index == 0 && wx.close_ms == 0);
  Engine e;
  CHECK(e.add_market(ng, T0).spec.event_ticker == "KXNATGAS15M-26OCT061145");
}

// Frames copied byte for byte from tests/test_commodity.cpp, which took them from the recorder's
// raw_json (commodity_book_events, 2026-09-24), written here as an archive day file holds them.
const char* kDeltaWti =
    R"({"type":"orderbook_delta","sid":2,"seq":2455956,"msg":{"market_ticker":"KXWTI15M-26SEP241145-45",)"
    R"("market_id":"1b8e56c9-7dbd-4dd5-b5de-864346d03de8","price_dollars":"0.5100","delta_fp":"30.00","side":"no",)"
    R"("ts":"2026-09-24T15:33:21.996783Z","ts_ms":1790264001996}}
)";
const char* kCloseGold =
    R"({"type":"orderbook_snapshot","sid":2,"seq":2287181,"msg":{"market_ticker":"KXGOLD15M-26SEP241130-30",)"
    R"("market_id":"9dbc5400-3f68-440b-9ed2-881672804d31"}}
)";
const char* kNatgasSnapshot =
    R"({"type":"orderbook_snapshot","sid":2,"seq":2287188,"msg":{"market_ticker":"KXNATGAS15M-26SEP241145-45",)"
    R"("market_id":"4a7acdee-83c0-4030-b857-19e30d47ba45","yes_dollars_fp":[["0.0100","2774.20"],["0.0200",)"
    R"("1728.00"],["0.0300","1098.00"],["0.0400","904.00"],["0.0500","1540.00"],["0.0600","900.00"],["0.0700",)"
    R"("1578.00"],["0.0800","750.00"],["0.0900","806.00"],["0.1000","450.00"],["0.1100","450.00"],["0.1200",)"
    R"("300.00"],["0.1300","1332.20"],["0.1500","390.00"],["0.1700","90.00"],["0.2500","20.00"],["0.2600",)"
    R"("19.00"],["0.3400","14.00"],["0.3500","1.00"],["0.3600","11.00"],["0.4200","20.20"],["0.4300","3.00"],)"
    R"(["0.5000","4.60"],["0.5500","2.00"],["0.5700","15.00"]],"no_dollars_fp":[["0.0100","2779.20"],["0.0200",)"
    R"("1718.00"],["0.0300","1102.67"],["0.0400","904.00"],["0.0500","1540.00"],["0.0600","900.00"],["0.0700",)"
    R"("1428.00"],["0.0800","750.00"],["0.0900","806.00"],["0.1000","1782.20"],["0.1100","450.00"],["0.1200",)"
    R"("1632.20"],["0.1500","300.00"],["0.2600","19.00"],["0.2800","17.00"],["0.2900","17.00"],["0.3100",)"
    R"("16.00"],["0.3400","20.20"],["0.3500","1.00"],["0.3800","19.20"],["0.4000","5.00"],["0.4100","73.20"]]}}
)";

std::string csv_quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

void test_archive_reader() {
  TempDir dir;
  const std::string path = dir.file("commodity_book_events-2026-09-24.csv");
  {
    std::ofstream out(path, std::ios::binary);
    out << "id,ts_received_ms,ts_exchange_ms,ticker,kind,sequence,side,price,delta,raw_json\r\n";
    out << "1,1790263800000,,,connect,,,,," << csv_quote(R"({"subscribed": ["KXGOLD15M-26SEP241130-30"]})") << "\r\n";
    // a whole delta: the columns and the frame agree
    out << "2,1790264001987,1790264001996,KXWTI15M-26SEP241145-45,orderbook_delta,2455956,no,0.51,30.0,"
        << csv_quote(kDeltaWti) << "\r\n";
    // before 2026-09-24 the columns held 0 and 0: the frame wins
    out << "3,1790264001988,1790264001996,KXWTI15M-26SEP241145-45,orderbook_delta,2455957,no,0.0,0.0,"
        << csv_quote(kDeltaWti) << "\r\n";
    // trimmed: no frame, the columns hold it (scripts/backfill_book_columns.py --trim)
    out << "4,1790264001989,1790264001990,KXWTI15M-26SEP241145-45,orderbook_delta,2455958,yes,0.998,-62.0,\r\n";
    out << "5,1790263800087,,KXGOLD15M-26SEP241130-30,orderbook_snapshot,2455959,,,," << csv_quote(kCloseGold) << "\r\n";
    out << "6,1790263809564,,KXNATGAS15M-26SEP241145-45,orderbook_snapshot,2455960,,,," << csv_quote(kNatgasSnapshot)
        << "\r\n";
    out << "7,1790263809565,,KXNATGAS15M-26SEP241145-45,orderbook_snapshot,2455961,,,,\r\n";  // no frame
    out << "8,1790263809566,,,ok,2455963,,,," << csv_quote(R"({"id":3,"sid":2,"seq":2455963,"type":"ok"})") << "\r\n";
    out << "9,1790263809567,,KXWTI15M-26SEP241145-45,orderbook_delta,2455964,maybe,0.5,1.0,\r\n";  // unreadable
  }
  Tape t;
  std::string err;
  CHECK(load_archive_csv(path, t, &err));
  CHECK(t.rows == 9 && t.kinds["orderbook_delta"] == 4 && t.kinds["orderbook_snapshot"] == 3 && t.kinds["ok"] == 1);
  CHECK(t.kinds["snapshot_without_frame"] == 1 && t.kinds["unreadable"] == 1);
  CHECK(t.seq_gaps == 1);  // 2455962 never came
  CHECK(t.events.size() == 5);
  const auto& d0 = t.events[0];
  CHECK(d0.kind == TapeEvent::Kind::DELTA && d0.no_side && d0.px == 5100 && d0.qty == 3000 && d0.t_ms == 1790264001987);
  CHECK(t.events[1].px == 5100 && t.events[1].qty == 3000);  // from the frame, not the zeros
  CHECK(!t.events[2].no_side && t.events[2].px == 9980 && t.events[2].qty == -6200);
  CHECK(t.events[3].kind == TapeEvent::Kind::SNAPSHOT && t.events[3].yes.empty() && t.events[3].no.empty());
  const auto& snap = t.events[4];
  CHECK(snap.yes.size() == 25 && snap.no.size() == 22);
  CHECK(snap.yes.front() == std::make_pair(Px{100}, Qty{277420}) && snap.no.back() == std::make_pair(Px{4100}, Qty{7320}));
  CHECK(snap.no[2].second == 110267);  // fractional contracts
  // filters
  Tape only;
  TapeFilter f;
  f.series = "KXWTI15M";
  CHECK(load_archive_csv(path, only, &err, f) && only.events.size() == 3);
  Tape capped;
  f = TapeFilter{};
  f.max_events = 2;
  CHECK(load_archive_csv(path, capped, &err, f) && capped.events.size() == 2);
  Tape none;
  CHECK(!load_archive_csv(dir.file("missing.csv.gz"), none, &err) && !err.empty());
  {
    std::ofstream out(dir.file("quotes.csv"));
    out << "id,ts,ticker,yes_bid\n1,2,X,3\n";
  }
  CHECK(!load_archive_csv(dir.file("quotes.csv"), none, &err) && err.find("not a commodity_book_events") != std::string::npos);

  // the synthetic fixture (tests/fake_exchange/make_tape.py), gzip, as the archive writes it
  const std::string fixture = std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.csv.gz";
  const json expect = read_json(std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.expect.json");
  CHECK(expect.is_object());
  Tape fx;
  CHECK(load_archive_csv(fixture, fx, &err));
  CHECK(fx.rows == expect["rows"].get<std::size_t>());
  for (const auto& [kind, n] : expect["kinds"].items()) CHECK(fx.kinds[kind] == n.get<std::size_t>());
  CHECK(fx.kinds["unreadable"] == 0 && fx.kinds["snapshot_without_frame"] == 0);
  CHECK(fx.seq_gaps == expect["lost_seq"].size());
  CHECK(fx.events.size() == fx.kinds["orderbook_delta"] + fx.kinds["orderbook_snapshot"]);
}

void test_scenarios() {
  Tape t;
  std::string err;
  const std::string text = R"(# a comment, then a blank line

{"t":0,"market":{"ticker":"KXTEST-26OCT06-A","exchange_index":0,"ticks":"linear_cent","fee_type":"quadratic_with_maker_fees","fee_multiplier":0.5,"close_in_ms":60000}}
{"t":0,"snapshot":{"ticker":"KXTEST-26OCT06-A","yes":[["0.4000","10.00"]],"no":[["0.5800","5.00"]]}}
{"t":100,"delta":{"ticker":"KXTEST-26OCT06-A","side":"yes","price":"0.4100","delta":"2.00"}}
{"t":200,"trade":{"ticker":"KXTEST-26OCT06-A","taker_book_side":"ask","price":"0.4000","count":"3.00"}}
{"t":300,"pause":{"ticker":"KXTEST-26OCT06-A"}}
{"t":400,"resume":{"ticker":"KXTEST-26OCT06-A"}}
{"t":500,"close":{"ticker":"KXTEST-26OCT06-A"}}
)";
  CHECK(parse_scenario(text, t, &err));
  CHECK(t.events.size() == 7 && t.rows == 7);
  CHECK(t.events[0].kind == TapeEvent::Kind::MARKET && t.events[0].spec->fee_type == FeeType::QUADRATIC_WITH_MAKER_FEES);
  CHECK(t.events[0].spec->fee_mult_milli == 500 && t.events[0].spec->close_ms == 60000);
  CHECK(t.events[3].kind == TapeEvent::Kind::TRADE && t.events[3].side == Side::ASK && t.events[3].qty == 300);
  CHECK(t.kinds["pause"] == 1 && t.kinds["close"] == 1);
  Tape bad;
  CHECK(!parse_scenario("{\"t\":5,\"close\":{}}\n{\"t\":4,\"close\":{}}\n", bad, &err) && err.find("backwards") != std::string::npos);
  bad = Tape{};
  CHECK(!parse_scenario("{\"t\":0,\"warp\":{}}\n", bad, &err) && err.find("line 1") != std::string::npos);
  bad = Tape{};
  CHECK(!parse_scenario("{\"t\":0,\"market\":{\"ticker\":\"X\",\"ticks\":\"hex\"}}\n", bad, &err));
  bad = Tape{};
  CHECK(!parse_scenario("{\"t\":0,\"delta\":{\"ticker\":\"X\",\"side\":\"bid\",\"price\":\"0.40\",\"delta\":\"1\"}}\n", bad, &err));
  bad = Tape{};
  CHECK(!parse_scenario("not json\n", bad, &err));

  // played: the scenario's trade, its pause and its close
  FakeConfig cfg;
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  FakeKalshi fake(cfg);
  fake.play(t, 1.0);
  const auto o = fake.with_engine([](Engine& e, std::int64_t now) {
    e.set_cash(0, 0, 100 * D, now);
    NewOrder n = ord(T, Side::BID, 4000, 200);
    n.cancel_on_pause = true;
    return e.create(n, now);
  });
  CHECK(!o.err);
  fake.advance_ms(199);
  CHECK(fake.with_engine([](Engine& e, std::int64_t) { return e.levels(T, false); }) == Levels({{4000, 1200}, {4100, 200}}));
  fake.advance_ms(1);
  // the 3-lot sells to the best bid first: the 2 at 0.41, then 1 of the street's 10 ahead of ours
  CHECK(fake.with_engine([](Engine& e, std::int64_t) { return e.levels(T, false); }) == Levels({{4000, 1100}}));
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.order(o.order_id)->filled; }) == 0);
  fake.advance_ms(100);
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.order(o.order_id)->reason; }) == "HaltCancel");
  fake.advance_ms(200);
  CHECK(fake.tape_done() && fake.tape_played() == 7);
  CHECK(fake.with_engine([](Engine& e, std::int64_t) { return e.market(T)->state; }) == Market::State::CLOSED);
  CHECK(fake.with_engine([](Engine& e, std::int64_t) { return e.market(T)->spec.fee_mult_milli; }) == 500);
}

// The fixture played into the engine at the venue's own pace (a manual clock, no sockets): window 1
// closes on time and takes our order with it, the delta lost before the reconnect is put right by
// the reconnect's snapshot, and window 2 ends as the generator's own book says.
void test_tape_play() {
  const std::string fixture = std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.csv.gz";
  const json expect = read_json(std::string(DESKD_SOURCE_DIR) + "/tests/fake_exchange/fixtures/natgas_tape.expect.json");
  const std::string w1 = "KXNATGAS15M-26OCT061145-45", w2 = "KXNATGAS15M-26OCT061200-00";
  Tape tape;
  std::string err;
  CHECK(load_archive_csv(fixture, tape, &err));
  const std::int64_t t0 = tape.t0(), span = tape.t1() - tape.t0();
  const std::int64_t close1 = expect["close_ms"][w1].get<std::int64_t>() - t0;  // ms after the tape's start
  FakeConfig cfg;
  cfg.manual_clock = true;
  cfg.start_ms = T0;
  FakeKalshi fake(cfg);
  fake.with_engine([](Engine& e, std::int64_t now) { e.set_cash(0, 2, 100 * D, now); });
  fake.play(std::move(tape), 1.0);
  const auto o = fake.with_engine([&](Engine& e, std::int64_t now) {
    return e.create(ord(w1, Side::BID, 200, 100), now);  // deep in window 1's book
  });
  CHECK(!o.err);
  fake.advance_ms(close1 - 1);
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.market(w1)->state; }) == Market::State::OPEN);
  fake.advance_ms(1);
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.market(w1)->state; }) == Market::State::CLOSED);
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.order(o.order_id)->reason; }) == "CloseCancel");
  for (std::int64_t t = close1; t <= span; t += 1000) fake.advance_ms(1000);
  CHECK(fake.tape_done());
  const auto yes = fake.with_engine([&](Engine& e, std::int64_t) { return e.levels(w2, false); });
  const auto no = fake.with_engine([&](Engine& e, std::int64_t) { return e.levels(w2, true); });
  CHECK(yes == levels_of(expect["final"][w2]["yes"]));
  CHECK(no == levels_of(expect["final"][w2]["no"]));
  CHECK(fake.with_engine([&](Engine& e, std::int64_t) { return e.market(w2)->state; }) == Market::State::OPEN);
  // the fixture's fractional sizes survive: 29.47 contracts at 0.41 on the NO side
  bool frac = false;
  for (const auto& [px, q] : no) frac = frac || (px == 4100 && q == 2947);
  CHECK(frac);
}

// The plan's race tests push 5,000 orders through the UNKNOWN table (section 8): the fake must
// take them without slowing to a crawl.
void test_many_orders() {
  Engine e;
  for (int m = 0; m < 50; ++m) e.add_market(spec("KXMANY-26OCT06-" + std::to_string(m)), T0);
  e.set_cash(0, 0, 100000 * D, T0);
  const auto t_start = std::chrono::steady_clock::now();
  int ok = 0;
  for (int i = 0; i < 5000; ++i) {
    NewOrder n = ord("KXMANY-26OCT06-" + std::to_string(i % 50), Side::BID, 100 + 100 * (i / 50 % 40), 100, Tif::GTC,
                     "S1-" + std::to_string(i));
    n.post_only = true;
    ok += e.create(n, T0 + i).err ? 0 : 1;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
  std::printf("  5,000 resting orders placed in %.0f ms\n", ms);
  CHECK(ok == 5000);
  CHECK(ms < 3000);
  CHECK(e.reserved(0, 0) > 0);
  CHECK(e.cancel_all(std::nullopt, T0 + 6000) == 5000);
  CHECK(e.reserved(0, 0) == 0 && e.user_orders().size() == 5000);
  // seeded: two engines give the same ids
  Engine x, y;
  x.add_market(spec(T), T0);
  y.add_market(spec(T), T0);
  x.set_cash(0, 0, D, T0);
  y.set_cash(0, 0, D, T0);
  CHECK(x.create(ord(T, Side::BID, 100, 100), T0).order_id == y.create(ord(T, Side::BID, 100, 100), T0).order_id);
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);   // every deskd test main (this one opens no socket, but stays uniform)
  test_wire();
  test_grids_and_fees();
  test_matching();
  test_self_trade();
  test_pause_and_close();
  test_account();
  test_groups();
  test_events();
  test_cancel();
  test_street();
  test_tickers();
  test_archive_reader();
  test_scenarios();
  test_tape_play();
  test_many_orders();
  return finish("deskd-fake-engine");
}
