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
  // a data release's window (strat/calendar.h): no resting quote, and it says which release
  env.release = [&](const std::string& t, std::int64_t) { return t == tk ? std::string("CPI") : std::string(); };
  MakerEngine release("M2", {"KXHIGHNY"}, env, p, band_quote);
  release.step(OPEN + 1000);
  CHECK(posted.empty() && release.status()["last"][tk] == "data release: CPI");
  env.release = {};
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

// D1 PAPER: flat it joins both touches; quiet 5 min it buys at the ask (an IOC that is new risk,
// not reduce_only); holding it offers out at the touch, and after 90 s leaves by IOC at the touch,
// no more than one IOC each 10 s.
void paper() {
  MakerParams p; p.size = 100;
  const auto b = book(4000, 500, 4300, 500);
  // a market a day from its close (input()'s closes in 15 minutes)
  auto input = [](const MdBook& bk, std::int64_t now = OPEN + 60000, Qty q = 0) {
    auto x = ::input(bk, now, q); x.close_ms = OPEN + 86400000; return x;
  };
  auto in = input(b);
  auto d = paper_quote(in, p);
  CHECK(d.pull.empty() && d.quotes.size() == 2 && d.quotes[0].side == Side::BUY && d.quotes[0].price == 4000 &&
        !d.quotes[0].ioc && d.quotes[1].side == Side::SELL && d.quotes[1].price == 4300 && d.quotes[1].size == 100);
  in.quiet_ms = 299999;
  CHECK(paper_quote(in, p).quotes.size() == 2);
  in.quiet_ms = 300000;
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 3 && d.quotes[2].ioc && !d.quotes[2].reduce && d.quotes[2].side == Side::BUY && d.quotes[2].price == 4300);
  in.since_ioc_ms = 5000;   // an IOC 5 s ago: not again yet
  CHECK(paper_quote(in, p).quotes.size() == 2);
  // long 1: offered at the ask; after 90 s sold at the bid by IOC (reduce_only)
  in = input(b, OPEN + 60000, 100);
  in.held_ms = 89999;
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4300 && !d.quotes[0].ioc && d.quotes[0].size == 100);
  in.held_ms = 90000;
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].reduce && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4000);
  in.since_ioc_ms = 9000;   // the last exit IOC missed 9 s ago: rest at the ask meanwhile
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 1 && !d.quotes[0].ioc && d.quotes[0].price == 4300);
  // short 2: bought back at the ask by IOC
  in = input(b, OPEN + 60000, -200);
  in.held_ms = 95000;
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].side == Side::BUY && d.quotes[0].price == 4300 && d.quotes[0].size == 200);
  // pulls
  auto one = book(4000, 500, 4300, 500); one.asks.clear();
  CHECK(paper_quote(input(one), p).pull == "one-sided book");
  auto stale = b; stale.stale = true;
  CHECK(paper_quote(input(stale), p).pull == "book stale");
  in = input(b); in.private_up = false;
  CHECK(paper_quote(in, p).pull == "private feed down");
  // no longer picked, or within 30 min of the close: no new risk. Flat it is pulled; holding it
  // leaves by IOC at once (not after 90 s), still one IOC each 10 s
  in = input(b); in.selected = false;
  CHECK(paper_quote(in, p).pull == "no longer picked" && paper_quote(in, p).quotes.empty());
  in = input(b); in.close_ms = in.now_ms + 1799999;
  CHECK(paper_quote(in, p).pull == "closing within 30 min");
  in = input(b, OPEN + 60000, 100); in.close_ms = in.now_ms + 1799999; in.held_ms = 1000;
  d = paper_quote(in, p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].ioc && d.quotes[0].reduce && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4000);
  in.since_ioc_ms = 3000;
  CHECK(paper_quote(in, p).quotes.empty() && !paper_quote(in, p).pull.empty());
}

void paper_pick() {
  const std::int64_t now = OPEN;
  rest::Snapshot s;
  auto add = [&](const std::string& tk, Px bid, Px ask, Qty vol, std::int64_t close_in) {
    kalshi::MarketRow m; m.ticker = tk; m.status = "active"; m.exchange_index = 0; m.open_ms = now - 1000;
    m.close_ms = now + close_in; m.price_ranges = CENT; m.yes_bid = bid; m.yes_ask = ask; m.volume_24h = vol;
    s.markets[tk] = m;
  };
  add("A", 4000, 4200, 900, 86400000);
  add("B", 6000, 6100, 500, 86400000);
  add("C", 3000, 3300, 2000, 3600000);    // closes within 2 h
  add("D", 200, 400, 5000, 86400000);     // a long shot
  add("E", 4000, 4900, 5000, 86400000);   // 9c wide
  add("F", 5000, 5200, 100, 86400000);
  add("G", 5000, 5100, 800, 86400000);
  s.markets["G"].price_ranges = TAPER;    // not a cent grid
  CHECK((paper_markets(s, now, {}) == std::set<std::string>{"A", "B"}));
  // kept while it still trades for 30 min, then the best of the rest
  CHECK((paper_markets(s, now, {"C"}) == std::set<std::string>{"A", "C"}));
  CHECK((paper_markets(s, now + 3600000 - 1800000, {"C"}) == std::set<std::string>{"A", "B"}));
  CHECK(paper_markets(s, now, {}, 3).size() == 3 && paper_markets(s, now, {}, 3).count("F"));
}

// The engine runs D1: it picks its markets, joins both sides, buys at the ask after 5 quiet
// minutes, and offers a position out at once, by IOC after 90 s.
void paper_engine() {
  const std::string tk = "KXPAPER-1";
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"D1"};
  auto snap = std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m; m.ticker = tk; m.event_ticker = "KXPAPER"; m.status = "active"; m.exchange_index = 0;
  m.open_ms = OPEN; m.close_ms = OPEN + 86400000; m.price_ranges = CENT; m.yes_bid = 4000; m.yes_ask = 4300; m.volume_24h = 10;
  snap->markets[tk] = m;
  snap->clock.post_trusted = true;
  std::vector<GwMsg> posted;
  std::set<std::string> wanted;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(book(4000, 500, 4300, 500)); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  env.want = [&](std::set<std::string> w) { wanted = std::move(w); };
  env.select = [](const rest::Snapshot& s, std::int64_t now, const std::set<std::string>& keep) { return paper_markets(s, now, keep); };
  MakerParams p; p.size = 100; p.fill_target_ms = 0;
  MakerEngine e("D1", {}, env, p, paper_quote);
  auto places = [&] {
    std::vector<const PlaceIntent*> out;
    for (const auto& g : posted) if (const auto* pi = std::get_if<PlaceIntent>(&g)) out.push_back(pi);
    return out;
  };
  auto rest_as = [&](const PlaceIntent& pi) {   // gw's view of a working quote
    Gateway::SysOrder o; o.cid = pi.cid; o.lane = "D1"; o.ticker = pi.ticker; o.side = pi.side; o.price = pi.price;
    o.count = o.left = pi.count; o.state = OrdState::WORK;
    view->orders[pi.cid] = o;
  };
  std::int64_t t = OPEN + 1000;
  e.step(t);
  auto pl = places();
  CHECK(wanted == std::set<std::string>{tk} && pl.size() == 2);
  if (pl.size() == 2) {
    CHECK(pl[0]->lane == "D1" && pl[0]->side == Side::BUY && pl[0]->price == 4000 && pl[0]->tif == Tif::POST &&
          pl[1]->side == Side::SELL && pl[1]->price == 4300 && pl[1]->count == 100);
    rest_as(*pl[0]); rest_as(*pl[1]);
  }
  // 5 quiet minutes: an IOC buy at the ask, not reduce_only
  posted.clear();
  t += 300000;
  e.step(t);
  pl = places();
  CHECK(pl.size() == 1 && pl[0]->tif == Tif::IOC && !pl[0]->reduce_only && pl[0]->side == Side::BUY && pl[0]->price == 4300 &&
        pl[0]->override_guard);
  // it filled: long 1. Both quotes go for the second after our fill (plan: pull on our fill)
  view->positions["D1|" + tk] = 100;
  posted.clear();
  t += 1000;
  e.step(t);
  int cancels = 0;
  for (const auto& g : posted) if (std::holds_alternative<CancelIntent>(g)) ++cancels;
  CHECK(cancels == 2 && places().empty() && e.status()["last"][tk] == "our fill");
  for (auto& [cid, o] : view->orders) { o.terminal = true; o.state = OrdState::CXL; o.left = 0; }   // gw shows them ended
  // then the position is offered out at the ask
  posted.clear();
  t += 1500;
  e.step(t);
  pl = places();
  CHECK(pl.size() == 1 && pl[0]->tif == Tif::POST && pl[0]->side == Side::SELL && pl[0]->price == 4300 && pl[0]->count == 100);
  if (pl.size() == 1) rest_as(*pl[0]);
  // held 90 s: that offer is cancelled, and the position leaves by IOC at the bid, reduce_only
  posted.clear();
  t += 90000;
  e.step(t);
  pl = places();
  cancels = 0;
  for (const auto& g : posted) if (std::holds_alternative<CancelIntent>(g)) ++cancels;
  CHECK(cancels == 1 && pl.size() == 1 && pl[0]->tif == Tif::IOC && pl[0]->reduce_only && pl[0]->side == Side::SELL &&
        pl[0]->price == 4000 && pl[0]->count == 100);
  CHECK(e.status()["lane"] == "D1" && e.status()["places"].get<int>() >= 4);
}

// D1 at its market's last 30 minutes (review 2026-10-14): the market drops out of the pick, and a
// position there leaves by IOC at once instead of riding into the close; flat, it is let go.
void paper_engine_leaving() {
  const std::string tk = "KXPAPER-2";
  const std::int64_t close = OPEN + 3 * 3600000;
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"D1"};
  auto snap = std::make_shared<rest::Snapshot>();
  kalshi::MarketRow m; m.ticker = tk; m.event_ticker = "KXPAPER"; m.status = "active"; m.exchange_index = 0;
  m.open_ms = OPEN; m.close_ms = close; m.price_ranges = CENT; m.yes_bid = 4000; m.yes_ask = 4300; m.volume_24h = 10;
  snap->markets[tk] = m;
  snap->clock.post_trusted = true;
  std::vector<GwMsg> posted;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(book(4000, 500, 4300, 500)); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  env.select = [](const rest::Snapshot& s, std::int64_t now, const std::set<std::string>& keep) { return paper_markets(s, now, keep); };
  MakerParams p; p.size = 100; p.fill_target_ms = 0;
  MakerEngine e("D1", {}, env, p, paper_quote);
  auto places = [&] {
    std::vector<PlaceIntent> out;
    for (const auto& g : posted) if (const auto* pi = std::get_if<PlaceIntent>(&g)) out.push_back(*pi);
    return out;
  };
  e.step(OPEN + 1000);
  auto pl = places();
  CHECK(pl.size() == 2);
  for (auto& pi : pl) {   // both rest
    Gateway::SysOrder o; o.cid = pi.cid; o.lane = "D1"; o.ticker = tk; o.side = pi.side; o.price = pi.price;
    o.count = o.left = pi.count; o.state = OrdState::WORK;
    view->orders[pi.cid] = o;
  }
  // the bid filled (long 1), then the market is 29 minutes from its close: out of the pick
  view->positions["D1|" + tk] = 100;
  e.step(OPEN + 2000);
  posted.clear();
  e.step(close - 29 * 60000);
  pl = places();
  int cancels = 0;
  for (const auto& g : posted) if (std::holds_alternative<CancelIntent>(g)) ++cancels;
  CHECK(pl.size() == 1 && pl[0].tif == Tif::IOC && pl[0].reduce_only && pl[0].side == Side::SELL && pl[0].price == 4000 &&
        pl[0].count == 100 && cancels >= 1);
  // the exit IOC missed: the market is not forgotten while the position is there, and it tries again
  for (auto& [cid, o] : view->orders) { o.terminal = true; o.state = OrdState::CXL; o.left = 0; }
  posted.clear();
  e.step(close - 29 * 60000 + 6000);    // within 10 s of the last IOC: waits
  CHECK(places().empty() && e.status()["last"][tk] == "leaving: the exit IOC retries in 10 s");
  e.step(close - 29 * 60000 + 11000);
  pl = places();
  CHECK(pl.size() == 1 && pl[0].tif == Tif::IOC && pl[0].reduce_only);
  // flat: let go, and nothing more is sent there
  view->positions["D1|" + tk] = 0;
  posted.clear();
  e.step(close - 28 * 60000);
  e.step(close - 27 * 60000);
  // (its last word is the pause after our fill: it was let go in that step)
  CHECK(places().empty() && e.status()["last"][tk] == "our fill");
}

// Plan section 3: never join the back of a 1-tick queue deeper than 3x our size unless fair
// clears the price by the full edge floor. 0.40 x50 / 0.41 x50, 10 a side, the fill pace's floor
// down 0.5c: without the rule both sides would join (cap at the touch); a shallow queue may still
// be joined, and an anchor that puts fair a full floor off the price lets it through.
void deep_join() {
  MakerParams p;   // size 10, e 1c alone
  auto deep = book(4000, 5000, 4100, 5000);
  auto in = input(deep);
  in.edge_relax = 50;
  auto d = maker_quote(in, p);
  CHECK(d.quotes.empty());
  p.deep_join_x = 0;   // the rule off: both sides join the touch
  d = maker_quote(in, p);
  CHECK(d.quotes.size() == 2 && d.quotes[0].price == 4000 && d.quotes[1].price == 4100);
  p.deep_join_x = 3;
  auto shallow = book(4000, 3000, 4100, 3000);   // 3x our size: not deeper than 3x
  in = input(shallow); in.edge_relax = 50;
  d = maker_quote(in, p);
  CHECK(d.quotes.size() == 2);
  // an anchor at 0.40: fair (0.405 + 0.40) / 2 = 0.4025, so the ask at 0.41 clears 0.75c, over
  // the anchor floor (0.5c, no maker fee); the bid at 0.40 does not
  in = input(deep); in.edge_relax = 50; in.anchor = Px{4000};
  d = maker_quote(in, p);
  CHECK(d.quotes.size() == 1 && d.quotes[0].side == Side::SELL && d.quotes[0].price == 4100);
}

// Plan section 5(a): a 2-tick move of everyone else's touch pulls both sides (LIP-15 sets 2);
// a 1-tick move does not.
void touch_pull() {
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
  std::vector<GwMsg> posted;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(shown); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  MakerParams p; p.pull_touch_ticks = 2;
  MakerEngine e("M1", {"KXNATGAS15M"}, env, p);
  e.step(OPEN + 60000);
  CHECK(posted.size() == 2);
  for (const auto& g : posted)
    if (const auto* pi = std::get_if<PlaceIntent>(&g)) {
      Gateway::SysOrder o; o.cid = pi->cid; o.lane = "M1"; o.ticker = tk; o.side = pi->side; o.price = pi->price;
      o.count = o.left = pi->count; o.state = OrdState::WORK;
      view->orders[o.cid] = o;
    }
  // ours rest at 0.41 / 0.45 (inside); everyone else's bid moves 0.40 -> 0.39: one tick, no pull
  shown.bids = {{4100, 1000}, {3900, 500}};
  shown.asks = {{4500, 1000}, {4600, 500}};
  posted.clear();
  e.step(OPEN + 61000);
  int cancels = 0;
  for (const auto& g : posted) if (std::holds_alternative<CancelIntent>(g)) ++cancels;
  CHECK(e.status()["last"][tk] != "the touch moved 2 ticks");
  // then to 0.38: two ticks from 0.40, both go
  shown.bids = {{4100, 1000}, {3800, 500}};
  posted.clear();
  e.step(OPEN + 62000);
  cancels = 0;
  for (const auto& g : posted) if (std::holds_alternative<CancelIntent>(g)) ++cancels;
  CHECK(cancels == 2 && e.status()["last"][tk] == "the touch moved 2 ticks");
}

// The LIP probe's window cap: with 2 windows a day, a third market waits ("done for today"), the
// two already quoted go on; the next ET day starts again.
void window_cap() {
  auto view = std::make_shared<Gateway::SysView>();
  view->run = RunState::RUN;
  view->lanes_on = {"P1"};
  auto snap = std::make_shared<rest::Snapshot>();
  snap->clock.post_trusted = true;
  const std::int64_t T = OPEN;
  for (const char* t : {"KXNATGAS15M-26OCT101500-00", "KXNATGAS15M-26OCT101500-01", "KXNATGAS15M-26OCT101500-02"}) {
    kalshi::MarketRow m; m.ticker = t; m.event_ticker = "KXNATGAS15M-26OCT101500"; m.status = "active";
    m.open_ms = T; m.close_ms = T + 900000; m.price_ranges = CENT;
    snap->markets[t] = m;
  }
  std::vector<GwMsg> posted;
  MakerEngine::Env env;
  env.view = [&] { return std::shared_ptr<const Gateway::SysView>(view); };
  env.snapshot = [&] { return std::shared_ptr<const rest::Snapshot>(snap); };
  env.book = [&](const std::string&) { return std::optional<MdBook>(book(4000, 500, 4600, 500)); };
  env.private_up = [] { return true; };
  env.post = [&](GwMsg g) { posted.push_back(std::move(g)); return true; };
  MakerParams p; p.size = 3000; p.window_cap = 2;
  MakerEngine e("P1", {"KXNATGAS15M"}, env, p);
  e.step(T + 60000);
  std::set<std::string> quoted;
  for (const auto& g : posted) if (const auto* pi = std::get_if<PlaceIntent>(&g)) quoted.insert(pi->ticker);
  CHECK(quoted.size() == 2 && !quoted.count("KXNATGAS15M-26OCT101500-02"));
  CHECK(e.status()["last"]["KXNATGAS15M-26OCT101500-02"] == "done for today: 2 windows quoted");
}

int main() {
  grid(); quotes(); deep_join(); touch_pull(); window_cap(); anchor_and_pulls(); band(); engine_limits(); review_fixes(); engine_own_quotes(); fill_pace();
  paper(); paper_pick(); paper_engine(); paper_engine_leaving();
  return finish("deskd-maker");
}
