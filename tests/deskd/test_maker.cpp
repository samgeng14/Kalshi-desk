// The maker engine's decision (strat/maker.h): plan section 5's quote rules, worked by hand.
#include <cmath>
#include "check.h"
#include "deskd/strat/maker.h"
#include "deskd/rest/state.h"
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
// The owner's limits, with gw, the REST read and the books stubbed: what it posts, or why not.
void engine_limits() {
  const std::string tk = "KXHIGHNY-26OCT10-B70";
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"M2"};
  auto snap = std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m; m.ticker = tk; m.event_ticker = "KXHIGHNY-26OCT10"; m.status = "active";
  m.open_ms = OPEN; m.close_ms = OPEN + 86400000; m.price_ranges = CENT;
  snap->markets[tk] = m;
  snap->clock.post_trusted = true;
  std::vector<GwMsg> posted;
  std::pair<std::int64_t, Money> mo{0, 0};
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(book(9700, 2000, 9900, 300)); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  env.markout = [&] { return mo; };
  MakerParams p; p.size = 500; p.event_cap = 15000000; p.lane_pause = 20000000; p.mo_after = 100; p.mo_floor = 100;
  MakerEngine e("M2", {"KXHIGHNY"}, env, p, band_quote);
  e.step(OPEN + 1000);
  CHECK(posted.size() == 1);
  if (posted.size() == 1) {
    const auto* pi = std::get_if<PlaceIntent>(&posted[0]);
    CHECK(pi && pi->owner == Owner::SYS && pi->lane == "M2" && pi->tif == Tif::POST && pi->ttl_s == 60 &&
          pi->price == 9700 && pi->count == 500 && pi->cid.rfind("SM2-", 0) == 0 && pi->touch == Px{9700});
  }
  // the markout floor: 100 fills at -1.5c stops it; a fresh engine sees why
  mo = {100, -150};
  MakerEngine paused("M2", {"KXHIGHNY"}, env, p, band_quote);
  posted.clear();
  paused.step(OPEN + 1000);
  CHECK(posted.empty() && paused.status()["last"][tk] == "lane paused: its 60 s markout is below the floor");
  mo = {99, -150};   // not enough fills yet to judge
  MakerEngine young("M2", {"KXHIGHNY"}, env, p, band_quote);
  young.step(OPEN + 1000);
  CHECK(posted.size() == 1);
  // the lane's day past -$20
  mo = {0, 0};
  view->lane_day["M2"] = -20000000;
  MakerEngine down("M2", {"KXHIGHNY"}, env, p, band_quote);
  posted.clear();
  down.step(OPEN + 1000);
  CHECK(posted.empty() && down.status()["last"][tk] == "lane paused: its day is past the loss limit");
  // the event cap: 15 contracts held in the event at $1 a contract is $15
  view->lane_day.clear();
  view->positions["M2|" + tk] = 1500;
  MakerEngine capped("M2", {"KXHIGHNY"}, env, p, band_quote);
  capped.step(OPEN + 1000);
  CHECK(posted.empty() && capped.status()["last"][tk] == "event cap");
  // not running: nothing
  view->positions.clear();
  view->run = RunState::STOP;
  MakerEngine stopped("M2", {"KXHIGHNY"}, env, p, band_quote);
  stopped.step(OPEN + 1000);
  CHECK(posted.empty() && stopped.status()["last"][tk] == "not running");
  // an untrusted clock: no resting quote is sent (gw would refuse its expiry), and it says why
  view->run = RunState::RUN;
  snap->clock.post_trusted = false;
  MakerEngine no_clock("M2", {"KXHIGHNY"}, env, p, band_quote);
  no_clock.step(OPEN + 1000);
  CHECK(posted.empty() && no_clock.status()["last"][tk] == "the clock is not trusted for an expiry");
  snap->clock.post_trusted = true;
  // an active market without open_time has opened (the sandbox's weather rows had none, and M2
  // sat idle); one listed ahead, "initialized", is still not quoted
  view->run = RunState::RUN;
  snap->markets[tk].open_ms.reset();
  MakerEngine no_open("M2", {"KXHIGHNY"}, env, p, band_quote);
  no_open.step(OPEN + 1000);
  CHECK(posted.size() == 1);
  snap->markets[tk].status = "initialized";
  posted.clear();
  MakerEngine ahead("M2", {"KXHIGHNY"}, env, p, band_quote);
  ahead.step(OPEN + 1000);
  CHECK(posted.empty());
}

// The fill pace: quoting a minute with no fill (fill_target_ms 60 s here) lowers the edge floor a
// step; at the plan's 0.5c floor the bid reaches the price the 1c floor kept it from; a fill puts
// it back. Book 0.40 x10 / 0.43 x10: fair 0.415. A 1c floor caps the bid at 0.405 (the touch,
// 0.40); 0.75c at 0.4075 (still 0.40); 0.5c at 0.41 (one tick better than the touch).
void fill_pace() {
  const std::string tk = "KXNATGAS15M-26OCT10-A";
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"M1"};
  auto snap = std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m; m.ticker = tk; m.event_ticker = "KXNATGAS15M-26OCT10"; m.status = "active";
  m.open_ms = OPEN; m.close_ms = OPEN + 3 * 3600000; m.price_ranges = CENT;
  snap->markets[tk] = m;
  snap->clock.post_trusted = true;
  std::vector<GwMsg> posted;
  std::vector<std::string> alerts;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { auto b = book(4000, 1000, 4300, 1000); b.ticker = tk; return std::optional<MdBook>(b); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  env.fill_p = [](const std::string&, bool, Qty, Qty, int) { return 0.05; };   // 5% per 15 s quote
  env.alert = [&](const std::string&, const std::string& msg) { alerts.push_back(msg); };
  MakerParams p; p.size = 100; p.ttl_s = 15; p.two_sided_ms = 3 * 3600000; p.fill_target_ms = 60000;
  MakerEngine e("M1", {"KXNATGAS15M"}, env, p);
  // what gw would show: every placed quote working, every cancelled one gone
  auto settle = [&] {
    for (const auto& g : posted) {
      if (const auto* pi = std::get_if<PlaceIntent>(&g)) {
        Gateway::SysOrder o; o.cid = pi->cid; o.lane = "M1"; o.ticker = tk; o.side = pi->side; o.price = pi->price;
        o.count = o.left = pi->count; o.state = OrdState::WORK;
        view->orders[o.cid] = o;
      } else if (const auto* ci = std::get_if<CancelIntent>(&g)) {
        auto it = view->orders.find(ci->cid);
        if (it != view->orders.end()) { it->second.terminal = true; it->second.left = 0; }
      }
    }
    posted.clear();
  };
  auto bid = [&]() -> Px { for (const auto& [cid, o] : view->orders) if (o.side == Side::BUY && !o.terminal) return o.price; return 0; };
  const std::int64_t t0 = OPEN + 1000;
  e.step(t0);
  settle();
  CHECK(bid() == 4000 && e.status()["edge_relax_c"] == 0.0);
  e.step(t0 + 61000);
  settle();
  // two quotes working at 5% a 15 s life each: one fill expected every 150 s
  CHECK(e.status()["est_fill_every_s"] == 150);
  CHECK(alerts.size() == 1 && alerts.back().find("no fill in 1 min") != std::string::npos);
  CHECK(e.status()["edge_relax_c"] == 0.25 && bid() == 4000);
  e.step(t0 + 121000);   // the floor reaches 0.5c...
  settle();
  CHECK(alerts.size() == 2 && alerts.back().find("0.5c floor") != std::string::npos);
  e.step(t0 + 122000);   // ...the 0.40 bid is cancelled...
  settle();
  e.step(t0 + 123000);   // ...and 0.41 posted
  settle();
  CHECK(bid() == 4100 && e.status()["edge_relax_c"] == 0.5);
  e.step(t0 + 300000);   // never below the floor
  settle();
  CHECK(alerts.size() == 2 && e.status()["edge_relax_c"] == 0.5);
  // a fill: back to the 1c floor, and the bid back to the touch
  view->positions["M1|" + tk] = 100;
  e.step(t0 + 301000);
  settle();
  CHECK(alerts.size() == 3 && alerts.back().find("back to normal") != std::string::npos);
  CHECK(e.status()["edge_relax_c"] == 0.0 && e.status()["fills_30m"] == 1);
  e.step(t0 + 303000);   // after the 1 s pull on our fill
  settle();
  e.step(t0 + 304000);
  settle();
  CHECK(bid() == 4000);
}

// The maker-math review (2026-10-10), each with its worked example.
void review_fixes() {
  // the grid at $0 and $1: the range's own end is not a price, and no lower range wins instead
  CHECK(grid_floor(10000, TAPER) == Px{9990} && grid_ceil(0, TAPER) == Px{10});
  CHECK(grid_floor(10000, CENT) == Px{9900} && grid_ceil(0, CENT) == Px{100} && !grid_floor(-50, CENT));
  // a cap through the touch joins it instead of dropping the quote: 0.45 x10 / 0.46 x30, micro
  // 0.4525; short 18, the bid cap 0.4605 is past the ask: bid the best bid
  auto b = book(4500, 1000, 4600, 3000);
  auto d = maker_quote(input(b, OPEN + 60000, -1800));
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::BUY && d.quotes[0].price == 4500);
  // long 13: the ask floor 0.4495 is under the bid: offer at the best ask
  d = maker_quote(input(b, OPEN + 60000, 1300));
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4600);
  // the T-60 s flatten goes ahead of the pulls: an anchor 4c off the microprice, a quiet reference
  b = book(4000, 500, 4600, 500);
  auto in = input(b, CLOSE - 45000, 1500);
  in.anchor = 4700;
  d = maker_quote(in);
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4000);
  in.anchor.reset();
  in.reference_fresh = false;
  d = maker_quote(in);
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc);
  // a quiet reference pulls the quotes with no anchor gated
  in = input(b);
  in.reference_fresh = false;
  CHECK(maker_quote(in).pull == "reference not fresh");
}

// The engine against its own quotes: they come off the book before it decides, the flatten goes
// through a lane pause, and a 0.5 sigma move of the underlying pulls what rests.
void engine_own_quotes() {
  const std::string tk = "KXNATGAS15M-26OCT101500-00";
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"M1"};
  auto snap = std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m; m.ticker = tk; m.event_ticker = "KXNATGAS15M-26OCT101500"; m.status = "active";
  m.open_ms = OPEN; m.close_ms = CLOSE; m.price_ranges = CENT;
  snap->markets[tk] = m;
  snap->clock.post_trusted = true;
  MdBook shown = book(4000, 500, 4600, 500);
  MakerEngine::Env::Anchor anchor;
  std::vector<GwMsg> posted;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(shown); };
  env.anchor = [&](const std::string&) { return anchor; };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  MakerEngine e("M1", {"KXNATGAS15M"}, env, MakerParams{});
  anchor.spot = 3.1;
  anchor.sd_log = 0.002;
  e.step(OPEN + 60000);
  CHECK(posted.size() == 2);
  // they rest: the venue's book now shows them as its touch
  auto rest = [&] {
    view->orders.clear();
    for (const auto& g : posted)
      if (const auto* pi = std::get_if<PlaceIntent>(&g)) {
        Gateway::SysOrder o;
        o.cid = pi->cid; o.lane = "M1"; o.ticker = tk; o.side = pi->side; o.price = pi->price; o.count = pi->count;
        o.left = pi->count; o.state = OrdState::WORK;
        view->orders[o.cid] = o;
      }
  };
  rest();
  CHECK(view->orders.size() == 2);
  shown.bids = {{4100, 1000}, {4000, 500}};
  shown.asks = {{4500, 1000}, {4600, 500}};
  posted.clear();
  e.step(OPEN + 61000);
  CHECK(posted.empty());   // everyone else's touch is still 0.40 / 0.46: the quotes stay
  // the others leave 0.40: our bid is alone above them, and goes to one tick over 0.39
  shown.bids = {{4100, 1000}, {3900, 500}};
  e.step(OPEN + 62000);
  CHECK(posted.size() == 1 && std::holds_alternative<CancelIntent>(posted[0]));
  // the underlying moves 0.6 sigma while the quotes rest: both are pulled
  shown.bids = {{4100, 1000}, {4000, 500}};
  posted.clear();
  MakerEngine moved("M1", {"KXNATGAS15M"}, env, MakerParams{});
  view->orders.clear();
  moved.step(OPEN + 60000);
  rest();
  anchor.spot = 3.1 * std::exp(0.0012);
  posted.clear();
  moved.step(OPEN + 61000);
  int cancels = 0;
  for (const auto& g : posted) cancels += std::holds_alternative<CancelIntent>(g);
  CHECK(cancels == 2 && moved.status()["last"][tk] == "the underlying moved 0.5 sigma");
  // a paused lane still flattens at T-60 s
  view->orders.clear();
  view->positions["M1|" + tk] = 1500;
  view->lane_day["M1"] = -100000000;
  MakerParams paused; paused.lane_pause = 1;
  MakerEngine late("M1", {"KXNATGAS15M"}, env, paused);
  shown = book(4000, 500, 4600, 500);
  posted.clear();
  late.step(CLOSE - 30000);
  CHECK(posted.size() == 1);
  if (posted.size() == 1) {
    const auto* pi = std::get_if<PlaceIntent>(&posted[0]);
    CHECK(pi && pi->tif == Tif::IOC && pi->side == Side::SELL && pi->count == 1500 && pi->reduce_only);
  }
}
}  // namespace

int main() {
  grid(); quotes(); anchor_and_pulls(); band(); engine_limits(); review_fixes(); engine_own_quotes(); fill_pace();
  return finish("deskd-maker");
}
