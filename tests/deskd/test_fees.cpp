// Fees (gw/fees.h). Kalshi's taker fee against the fake exchange's own model (fakex::model_fee
// and charge_fee, and its engine filling IOC orders) at the fake's balance precision, which is
// not deskd's $0.01 default; the order bound against charge_fee's accumulator over random fills;
// the take engines' cent rule; Polymarket US's per-fill rounding and its worked rows, and its fee
// and rebate against the fake's FeeType::POLY.
#include <cstdio>
#include <random>

#include "check.h"
#include "deskd/gw/fees.h"
#include "engine.h"

using namespace deskd;

namespace {
const Px kPx[] = {1, 100, 500, 5000, 9500, 9900, 9999};
const Qty kQty[] = {1, 100, 1000, 2000};
const double kMult[] = {0, 0.5, 1};
// the fake's own precision: what its engine charges unless a test sets another
const Micros kFake = fakex::EngineConfig{}.balance_precision;

int milli(double mult) { return static_cast<int>(mult * 1000); }

// what charge_fee makes one fill pay as its order's first
Micros first_fill(Micros revenue, Micros model, Micros precision) {
  Micros acc = 0;
  return fakex::charge_fee(revenue, model, precision, acc);
}

void same(Micros got, Micros want, const char* what, Px p, Qty q, double mult, Micros precision) {
  CHECK(got == want);
  if (got != want)
    std::fprintf(stderr, "  %s p=%lld q=%lld mult=%g precision=%lld: %lld, the fake %lld\n", what, (long long)p,
                 (long long)q, mult, (long long)precision, (long long)got, (long long)want);
}

void kalshi_parity() {
  CHECK(kFake == 100);   // $0.0001: a direct member (fee_rounding.md)
  for (const Px p : kPx)
    for (const Qty q : kQty)
      for (const double mult : kMult) {
        const Micros model = fakex::model_fee(fakex::FeeType::QUADRATIC, milli(mult), true, q, p);
        same(kalshi_model_fee(mult, p, q), model, "model", p, q, mult, 0);
        same(kalshi_model_fee(mult, kOneDollar - p, q), model, "model, the other side", p, q, mult, 0);
        // a buy of YES at p; a buy of NO at 1 - p (a SELL of YES at p); a SELL of YES at p that
        // closes a long, which nets each pair back to $1 at once
        same(kalshi_taker_fee(mult, p, q, kFake), first_fill(-q * p, model, kFake), "buy", p, q, mult, kFake);
        same(kalshi_taker_fee(mult, kOneDollar - p, q, kFake), first_fill(-q * (kOneDollar - p), model, kFake),
             "buy NO", p, q, mult, kFake);
        same(kalshi_taker_fee(mult, kOneDollar - p, q, kFake), first_fill(q * p, model, kFake), "closing sale", p, q,
             mult, kFake);
        // deskd's default rounds to a coarser unit, so it never assumes less than the fake charges
        CHECK(kalshi_taker_fee(mult, p, q) >= kalshi_taker_fee(mult, p, q, kFake));
        CHECK(kalshi_taker_fee(mult, p, q) == kalshi_taker_fee(mult, p, q, kFeePrecision));
      }
  // a multiplier finer than the fake's thousandths rounds up; one fee_ready() refuses counts 0
  CHECK(kalshi_model_fee(0.0004, 5000, 100) == fakex::model_fee(fakex::FeeType::QUADRATIC, 1, true, 100, 5000));
  // ... but not a binary fraction's last bit: 0.1 x 3 is 0.30000000000000004, still 300
  CHECK(kalshi_model_fee(0.1 * 3, 5000, 100) == fakex::model_fee(fakex::FeeType::QUADRATIC, 300, true, 100, 5000));
  CHECK(kalshi_model_fee(std::nan(""), 5000, 100) == 0 && kalshi_model_fee(-1, 5000, 100) == 0);
  CHECK(kalshi_taker_fee(1, 0, 100) == 0 && kalshi_taker_fee(1, kOneDollar, 100) == 0 && kalshi_taker_fee(1, 5000, 0) == 0);
}

void kalshi_rounding() {
  CHECK(kFeePrecision == 10000);   // decisions 7: $0.01 until demo fills prove $0.0001
  // fee_rounding.md's worked FCM example: one contract at 5.5c, a $0.003639 trade fee, $0.005 in
  // all at $0.01 (the change -$0.058639 floors to -$0.06); $0.0037 at $0.0001
  CHECK(kalshi_model_fee(1, 550, 100) == 3639);
  CHECK(kalshi_taker_fee(1, 550, 100) == 5000 && kalshi_taker_fee(1, 550, 100, 100) == 3700);
  // 3 at 42c: $0.051156, $0.0512 at $0.0001 (the fake's IOC example), $0.06 at $0.01
  CHECK(kalshi_taker_fee(1, 4200, 300, 100) == 51200 && kalshi_taker_fee(1, 4200, 300) == 60000);
  // fee-free, but 0.01 contract at 0.01c is not a whole $0.0001: the rounding fee is still charged
  CHECK(kalshi_taker_fee(0, 1, 1, 100) == 99);
  // the side bought decides the rounding once the cost is not whole units: a contract of YES at
  // 0.1c costs $0.001 and pays $0.009, one of NO at 99.9c costs $0.999 and pays $0.001
  CHECK(kalshi_taker_fee(1, 10, 100) == 9000 && kalshi_taker_fee(1, 9990, 100) == 1000);
}

void order_bound() {
  // whole contracts at cent prices: the model fees plus one unit
  const std::vector<MdLevel> walk = {{4000, 300}, {4100, 500}, {4200, 200}};
  Micros sum = 0;
  for (const auto& l : walk) sum += kalshi_model_fee(1, l.px, l.qty);
  CHECK(order_fee_bound(1, walk, 100) == sum + 100 && order_fee_bound(1, walk) == sum + kFeePrecision);
  // two contracts at 0.1c, one fill each: charged $0.018 at $0.01, past the fees plus one unit
  Micros acc = 0, charged = 0;
  for (int i = 0; i < 2; ++i) charged += fakex::charge_fee(-1000, kalshi_model_fee(1, 10, 100), 10000, acc);
  CHECK(charged == 18000 && charged > 2 * kalshi_model_fee(1, 10, 100) + 10000);
  CHECK(order_fee_bound(1, {{10, 100}, {10, 100}}) == 2 * 70 + 3 * 10000);
  CHECK(order_fee_bound(1, {{10, 100}, {10, 100}}, 100) == 2 * 70 + 100);   // whole units at $0.0001

  // random orders of up to 6 fills: charge_fee's total never passes the bound, at either precision
  std::mt19937_64 rng(11);
  std::uniform_int_distribution<Px> px(1, kOneDollar - 1);
  std::uniform_int_distribution<int> n_fills(1, 6), pick(0, 3);
  int cases = 0, over_one_unit = 0;
  for (int i = 0; i < 20000; ++i) {
    const Micros precision = i % 2 ? 100 : 10000;
    const double mult = kMult[i % 3];
    std::vector<MdLevel> fills;
    Micros acc = 0, total = 0, sum_model = 0;
    const int n = n_fills(rng);
    for (int k = 0; k < n; ++k) {
      // cent and deci-cent prices, whole and fractional sizes, and anything at all
      const int kind = pick(rng);
      Px p = px(rng);
      if (kind == 0) p = std::max<Px>(100, p / 100 * 100);
      if (kind == 1) p = std::max<Px>(10, p / 10 * 10);
      const Qty q = kind == 3 ? 1 + static_cast<Qty>(rng() % 2500) : 100 * (1 + static_cast<Qty>(rng() % 20));
      const bool no = rng() % 2;   // the YES price either way; the side bought pays its own price
      const Micros model = fakex::model_fee(fakex::FeeType::QUADRATIC, milli(mult), true, q, p);
      const Micros charge = fakex::charge_fee(no ? -q * (kOneDollar - p) : -q * p, model, precision, acc);
      if (k == 0) CHECK(charge == kalshi_taker_fee(mult, no ? kOneDollar - p : p, q, precision));
      total += charge;
      sum_model += model;
      fills.push_back({p, q});
    }
    ++cases;
    if (total > sum_model + precision) ++over_one_unit;
    const Micros bound = order_fee_bound(mult, fills, precision);
    CHECK(total <= bound);
    if (total > bound)
      std::fprintf(stderr, "  order %d: charged %lld, bound %lld\n", i, (long long)total, (long long)bound);
  }
  CHECK(cases == 20000 && over_one_unit > 0);   // the extra units are needed, not only safe
}

void fake_engine() {
  // The fake's engine filling IOC orders at its own precision, a whole level at a time
  const std::string T = "KXFEES-26OCT21-A";
  for (const double mult : kMult)
    for (const Px p : {Px{100}, Px{500}, Px{5000}, Px{9500}, Px{9900}})
      for (const Qty q : kQty) {
        fakex::Engine e;
        fakex::MarketSpec s;
        s.ticker = T;
        s.fee_mult_milli = milli(mult);
        e.add_market(s, 0);
        e.set_cash(0, 0, 1000000 * fakex::kMicrosPerDollar, 0);
        fakex::NewOrder n;
        n.ticker = T;
        n.tif = fakex::Tif::IOC;
        n.count = q;
        n.price = p;
        e.street_add(T, true, kOneDollar - p, q, 0);   // a YES ask at p
        n.side = fakex::Side::BID;
        const auto buy = e.create(n, 1);
        CHECK(buy.fill_count == q);
        same(buy.fees, kalshi_taker_fee(mult, p, q, e.config().balance_precision), "engine buy", p, q, mult, kFake);
        e.street_add(T, false, p, q, 2);   // a YES bid at p: the sale closes the long
        n.side = fakex::Side::ASK;
        const auto sale = e.create(n, 3);
        CHECK(sale.fill_count == q);
        same(sale.fees, kalshi_taker_fee(mult, kOneDollar - p, q, e.config().balance_precision), "engine sale", p,
             q, mult, kFake);
      }
  // an IOC that walks three levels of the 0.1c grid, at $0.01: inside the bound
  const std::string G = "KXFEES15M-26OCT21-B";
  fakex::Engine e;
  e.config().balance_precision = kFeePrecision;
  fakex::MarketSpec s;
  s.ticker = G;
  s.ticks = fakex::tapered_deci_cent();
  s.price_level_structure = "tapered_deci_cent";
  e.add_market(s, 0);
  e.set_cash(0, 0, 1000 * fakex::kMicrosPerDollar, 0);
  const std::vector<MdLevel> levels = {{10, 100}, {20, 300}, {30, 200}};
  for (const auto& l : levels) e.street_add(G, true, kOneDollar - l.px, l.qty, 0);
  fakex::NewOrder n;
  n.ticker = G;
  n.side = fakex::Side::BID;
  n.tif = fakex::Tif::IOC;
  n.count = 600;
  n.price = 30;
  const auto walk = e.create(n, 1);
  Micros sum = 0;
  for (const auto& l : levels) sum += kalshi_model_fee(1, l.px, l.qty);
  // $0.009, $0.004 and $0.004: more than the fees plus one unit, as each cost rounds to a cent
  CHECK(walk.fill_count == 600 && walk.fees == 17000 && walk.fees > sum + kFeePrecision);
  CHECK(walk.fees <= order_fee_bound(1, levels));
}

void cent_rule() {
  // C1's rows: 5 at 40c $0.084 -> $0.09; 1 at 50c $0.0175 -> $0.02; multiplier 0.5 halves the
  // fee before it is rounded
  CHECK(kalshi_fee_order_cent(4000, 500, 1) == 900);
  CHECK(kalshi_fee_order_cent(5000, 100, 1) == 200);
  CHECK(kalshi_fee_order_cent(4000, 500, 0.5) == 500);
  CHECK(kalshi_fee_order_cent(5000, 400, 1) == 700);   // exactly 7c stays 7c
  CHECK(kalshi_fee_order_cent(5000, 100, 0) == 0);
  // whole contracts at cent prices: the taker fee at $0.01, in Money
  for (const Px p : {Px{100}, Px{500}, Px{5000}, Px{9500}, Px{9900}})
    for (const Qty q : {Qty{100}, Qty{1000}, Qty{2000}})
      for (const double mult : kMult) CHECK(kalshi_fee_order_cent(p, q, mult) * 100 == kalshi_taker_fee(mult, p, q));
}

void polymarket() {
  // the worked rows: a rebate rounds to the cent per fill
  CHECK(poly_maker_rebate_fill(5000, 100) == 0);        // $0.003125
  CHECK(poly_maker_rebate_fill(5000, 200) == -10000);   // $0.00625
  CHECK(poly_maker_rebate_fill(9000, 400) == 0);        // $0.0045
  CHECK(poly_maker_rebate_fill(9000, 500) == -10000);   // $0.005625
  CHECK(min_rebate_size(5000) == 200 && min_rebate_size(9000) == 500);
  // exactly half a cent rounds to 0, the even one: at a 0.02 rate one contract at 50c earns nothing
  CHECK(poly_maker_rebate_fill(5000, 100, 0.02) == 0 && min_rebate_size(5000, 0.02) == 200);
  // half to even: 8 at 50c is 2.5c -> 2c; 0.06 x 1 at 50c is 1.5c -> 2c, x 3 is 4.5c -> 4c
  CHECK(poly_maker_rebate_fill(5000, 800) == -20000);
  CHECK(poly_taker_fee_fill(5000, 100, 0.06) == 20000 && poly_taker_fee_fill(5000, 300, 0.06) == 40000);
  // 0.0695 at 50c: 1.7375c -> 2c; two 3.475c -> 3c
  CHECK(poly_taker_fee_fill(5000, 100, 0.0695) == 20000 && poly_taker_fee_fill(5000, 200, 0.0695) == 30000);
  CHECK(poly_taker_fee_fill(0, 100, 0.06) == 0 && poly_taker_fee_fill(kOneDollar, 100, 0.06) == 0);
  CHECK(poly_maker_rebate_fill(5000, 0) == 0 && poly_taker_fee_fill(5000, 100, 0) == 0);
  CHECK(min_rebate_size(0) == 0 && min_rebate_size(kOneDollar) == 0 && min_rebate_size(5000, 0) == 0);
  // min_rebate_size is the first whole size that earns a cent, at every price
  int bad = 0;
  for (Px p = 1; p < kOneDollar; ++p) {
    const Qty n = min_rebate_size(p);
    if (n <= 0 || n % kOneContract != 0 || poly_maker_rebate_fill(p, n) > -10000 ||
        (n > kOneContract && poly_maker_rebate_fill(p, n - kOneContract) != 0))
      ++bad;
  }
  CHECK(bad == 0);
  CHECK(min_rebate_size(1) == 400100);   // 0.01c: 4,001 contracts
}

void poly_parity() {
  // the fake's FeeType::POLY (fakex::poly_fee) one fill at a time: every price, whole and
  // fractional sizes, the old desk's and core.js's coefficients
  int bad = 0;
  for (Px p = 1; p < kOneDollar; ++p)
    for (const Qty q : {Qty{1}, Qty{100}, Qty{160}, Qty{500}, Qty{2000}})
      for (const int coeff : {60000, 69500}) {
        if (fakex::poly_fee(coeff, 12500, true, q, p) != poly_taker_fee_fill(p, q, coeff / 1e6)) ++bad;
        if (fakex::poly_fee(coeff, 12500, false, q, p) != poly_maker_rebate_fill(p, q)) ++bad;
      }
  CHECK(bad == 0);
  // ... and its engine: a take and a resting bid's fill on a POLY market, at the 0.5c grid
  const std::string P = "aec-nba-bos-nyk-2026-10-22";
  for (const Px p : {Px{50}, Px{1000}, Px{5000}, Px{5050}, Px{9000}, Px{9950}})
    for (const Qty q : kQty) {
      fakex::Engine e;
      fakex::MarketSpec s;
      s.ticker = P;
      s.fee_type = fakex::FeeType::POLY;
      s.ticks = {{50, 9950, 50}};
      s.poly_coeff_micro = 69500;
      e.add_market(s, 0);
      e.set_cash(0, 0, 1000000 * fakex::kMicrosPerDollar, 0);
      e.street_add(P, true, kOneDollar - p, q, 0);   // a YES ask at p
      fakex::NewOrder n;
      n.ticker = P;
      n.side = fakex::Side::BID;
      n.tif = fakex::Tif::IOC;
      n.count = q;
      n.price = p;
      const auto take = e.create(n, 1);
      CHECK(take.fill_count == q && take.fees == poly_taker_fee_fill(p, q, 0.0695));
      n.tif = fakex::Tif::GTC;
      n.post_only = true;
      const auto made = e.create(n, 2);
      CHECK(e.street_take(P, fakex::Side::ASK, p, q, 3) == q);
      CHECK(e.order(made.order_id)->maker_fees == poly_maker_rebate_fill(p, q));
    }
}
}  // namespace

int main() {
  kalshi_parity(); kalshi_rounding(); order_bound(); fake_engine(); cent_rule(); polymarket(); poly_parity();
  return finish("deskd-fees");
}
