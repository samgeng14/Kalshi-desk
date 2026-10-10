// The maker engine's decision (strat/maker.h): plan section 5's quote rules, worked by hand.
#include "check.h"
#include "deskd/strat/maker.h"
using namespace deskd;
namespace {
const std::vector<kalshi::PriceRange> CENT = {{0, 10000, 100}};
const std::vector<kalshi::PriceRange> TAPER = {{0, 1000, 10}, {1000, 9000, 100}, {9000, 10000, 10}};
MdBook book(Px bid, Qty bq, Px ask, Qty aq) {
  MdBook b; b.ticker = "TK"; b.stale = false;
  b.bids = {{bid, bq}}; b.asks = {{ask, aq}};
  return b;
}
const std::int64_t OPEN = 1791000000000, CLOSE = OPEN + 900000;
MakerInput input(const MdBook& b, std::int64_t now = OPEN + 60000, Qty q = 0) {
  MakerInput in; in.book = &b; in.grid = &CENT; in.position = q; in.now_ms = now; in.open_ms = OPEN; in.close_ms = CLOSE;
  in.private_up = true;
  return in;
}

void grid() {
  CHECK(grid_floor(4049, CENT) == Px{4000} && grid_ceil(4001, CENT) == Px{4100} && grid_ceil(4000, CENT) == Px{4000});
  CHECK(grid_step(4000, CENT) == 100 && grid_step(950, TAPER) == 10 && grid_step(9500, TAPER) == 10 && grid_step(5000, TAPER) == 100);
  CHECK(grid_floor(1050, TAPER) == Px{1000} && grid_ceil(995, TAPER) == Px{1000} && grid_ceil(9001, TAPER) == Px{9010});
  CHECK(!grid_floor(0, CENT) && !grid_ceil(10000, CENT));   // never 0 or $1
  kalshi::MarketRow m; m.fee_type = "quadratic_with_maker_fees"; m.fee_multiplier = 1.0;
  CHECK(maker_fee(m, 5000) == 44);   // 0.0175 x 0.25, rounded up
  m.fee_type = "quadratic";
  CHECK(maker_fee(m, 5000) == 0);
}

void quotes() {
  // a wide book: the mid (spread over 3 ticks), e = 1c, both sides one tick inside the touch
  auto b = book(4000, 500, 4600, 500);
  auto d = maker_quote(input(b));
  CHECK(d.fair == Px{4300} && d.pull.empty() && d.quotes.size() == 2);
  if (d.quotes.size() == 2) {
    CHECK(d.quotes[0].side == Side::BUY && d.quotes[0].price == 4100 && d.quotes[0].size == 1000 && !d.quotes[0].ioc);
    CHECK(d.quotes[1].side == Side::SELL && d.quotes[1].price == 4500 && d.quotes[1].size == 1000);
  }
  // a narrower book: never above fair - e on the bid, never below fair + e on the ask
  b = book(4000, 500, 4300, 500);   // 3 ticks: the microprice 0.4150
  d = maker_quote(input(b));
  CHECK(d.fair == Px{4150} && d.quotes.size() == 2);
  if (d.quotes.size() == 2) CHECK(d.quotes[0].price == 4000 && d.quotes[1].price == 4300);
  // one tick wide: fair is within e of both touches, so no quote
  b = book(4500, 1000, 4600, 3000);
  d = maker_quote(input(b));
  CHECK(d.quotes.empty() && d.pull == "fair too close to the touch");
  // long 10: the skew lowers both sides by 1c
  b = book(4000, 500, 4600, 500);
  d = maker_quote(input(b, OPEN + 60000, 1000));
  CHECK(d.quotes.size() == 2);
  if (d.quotes.size() == 2) CHECK(d.quotes[0].price == 4100 && d.quotes[1].price == 4500);   // still at the touch's inside
  d = maker_quote(input(book(4000, 500, 4400, 500), OPEN + 60000, 1000));   // fair 0.42, skew 1c: bid cap 0.40, ask floor 0.42
  CHECK(d.quotes.size() == 2);
  if (d.quotes.size() == 2) CHECK(d.quotes[0].price == 4000 && d.quotes[1].price == 4300);
  // past minute 10: the reducing side only, at most the position
  d = maker_quote(input(b, OPEN + 660000, 500));
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::SELL && d.quotes[0].size == 500);
  d = maker_quote(input(b, OPEN + 660000, -300));
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::BUY && d.quotes[0].size == 300);
  d = maker_quote(input(b, OPEN + 660000, 0));
  CHECK(d.quotes.empty() && d.pull == "past minute 10, flat");
  // T-60 s: over 10 contracts goes IOC at the touch; 10 or less waits it out
  d = maker_quote(input(b, CLOSE - 30000, 1500));
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4000 && d.quotes[0].size == 1500);
  d = maker_quote(input(b, CLOSE - 30000, -1200));
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].side == Side::BUY && d.quotes[0].price == 4600);
  d = maker_quote(input(b, CLOSE - 30000, 1000));
  CHECK(d.quotes.empty() && d.pull == "last minute");
}

void anchor_and_pulls() {
  auto b = book(4000, 500, 4600, 500);
  auto in = input(b);
  in.anchor = 4800;   // 5c from the microprice
  CHECK(maker_quote(in).pull == "anchor and microprice over 3c apart");
  in.anchor = 4400;
  in.maker_fee = 44;
  auto d = maker_quote(in);   // fair (0.43 + 0.44) / 2, e = 0.5c + the fee
  CHECK(d.fair == Px{4350} && d.quotes.size() == 2);
  if (d.quotes.size() == 2) CHECK(d.quotes[0].price == 4100 && d.quotes[1].price == 4500);
  in.reference_fresh = false;
  CHECK(maker_quote(in).pull == "reference not fresh");
  in = input(b);
  in.private_up = false;
  CHECK(maker_quote(in).pull == "private feed down");
  b.stale = true;
  CHECK(maker_quote(input(b)).pull == "book stale");
  auto one = book(4000, 500, 4600, 500);
  one.asks.clear();
  CHECK(maker_quote(input(one)).pull == "one-sided book");
  CHECK(maker_quote(input(book(4000, 500, 4600, 500), CLOSE)).pull == "closed");
}
void band() {
  MakerParams p; p.size = 500;
  // YES the favourite at 0.97: join its bid, 5 contracts
  auto b = book(9700, 2000, 9900, 300);
  auto d = band_quote(input(b), p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::BUY && d.quotes[0].price == 9700 && d.quotes[0].size == 500);
  // holding 3: two more; holding 5: none
  d = band_quote(input(b, OPEN + 60000, 300), p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].size == 200);
  CHECK(band_quote(input(b, OPEN + 60000, 500), p).pull == "holding the favourite");
  // NO the favourite (YES ask 0.02): join the ask, selling YES
  b = book(100, 500, 200, 800);
  d = band_quote(input(b), p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::SELL && d.quotes[0].price == 200 && d.quotes[0].size == 500);
  // nothing near certain: no quote; 0.95 is outside the band, as is 0.995 on a deci grid
  CHECK(band_quote(input(book(4000, 500, 4600, 500)), p).pull == "no favourite at 96-99c");
  CHECK(band_quote(input(book(9500, 500, 9700, 500)), p).pull == "no favourite at 96-99c");
  CHECK(band_quote(input(book(9950, 500, 9990, 500)), p).pull == "no favourite at 96-99c");
  auto in = input(book(9700, 500, 9900, 500));
  in.private_up = false;
  CHECK(band_quote(in, p).pull == "private feed down");
}
}  // namespace

int main() {
  grid(); quotes(); anchor_and_pulls(); band();
  return finish("deskd-maker");
}
