// gw's one measure of Systematic's risk, Gateway::sys_used (plan section 4's limits; contract
// hdr.sys and the lane row's used), on gw with no venue (send-k2's inbox is a queue the test
// reads and nothing sends from):
//   - on random books (the lanes' positions by market; Systematic orders resting, part filled
//     with fills not booked yet, ended with fills not booked yet, refused; Manual's beside them),
//     sys_used counts what gw's own loops counted before it (gateway.cpp at 49b980e, kept below
//     as Old): the venue's used, each lane's, and an order's lane budget, 70% ceiling, correlated
//     group and event cap; and sys_worst (strat's SysView::worst) by lane and market adds up to
//     sys_used over any markets
//   - through place_sys: the lane budget, the ceiling and the group refuse just past their edge
//     and not at it, an order counts only the part of it that grows its lane's position, and a
//     reducing order at a full budget passes
//   - the IOC lanes' $5 cost budget counts every fee charged: a rebate (a negative fee) is the
//     lane's P&L but gives none of it back (decisions 6)
//   - by settlement state (gw/event_risk.h; decisions 5): with deskd.event_risk off, or for a lane
//     that does not count by state, every number is Old's, on random books with event rows and on
//     M2's KXHIGH positions; on, S1's stakes in an event the account read's row shapes count
//     max(0, their worst case over its states), so a complete basket is about $0 in the lane row,
//     the header, the budget, the ceiling and the group, and S1 takes a second basket; a leg that
//     adds nothing passes even past a limit; a missing or contradicted row counts market by market
//     again and refuses growth; ManualPath::lattice closes KXHIGH's gaps between whole degrees; and
//     an IOC leg's fills not booked yet count until they are
#include <future>
#include <mutex>
#include <random>
#include <nlohmann/json.hpp>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kill.h"
#include "deskd/gw/manual_book.h"
#include "deskd/gw/sys_book.h"
#include "deskd/rest/state.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
const std::string EV_A = "KXRISKA-26OCT06", EV_B = "KXRISKB-26OCT06";
const std::string A1 = EV_A + "-T1", B1 = EV_B + "-T1", B2 = EV_B + "-T2";   // B1 and B2: one event, one group
const std::string B3 = EV_B + "-T3";
// KXHIGHTST's day, the 2026-09-05 KXHIGHLAX ladder (tests/test_main.cpp:2007): under 79, 79-80 and
// over 80, one of which settles YES whatever the high. And KXHIGHGAP's, whose brackets 79-80 and
// 81-82 leave 80-81 paying nothing on the real line but no whole degree between them.
const std::string EV = "KXHIGHTST-26OCT22", LO = EV + "-T79", MID = EV + "-B79.5", HI = EV + "-T80";
const std::string GAP = "KXHIGHGAP-26OCT22";
struct Strike {
  std::string tk, type;
  std::optional<double> floor, cap;
};
const std::vector<Strike> kStrikes = {{LO, "less", std::nullopt, 79},
                                      {MID, "between", 79, 80},
                                      {HI, "greater", 80, std::nullopt},
                                      {GAP + "-T79", "less", std::nullopt, 79},
                                      {GAP + "-B79.5", "between", 79, 80},
                                      {GAP + "-B81.5", "between", 81, 82},
                                      {GAP + "-T82", "greater", 82, std::nullopt}};
const auto every = [](const std::string&) { return true; };
std::function<bool(const std::string&)> only(const std::string& id) { return [id](const std::string& x) { return x == id; }; }

// The account read: A1, B1 to B3 (no strikes) and the two days above; `rows`, the events whose
// rows rest has read (GET /events/{t}), each mutually exclusive.
std::shared_ptr<const rest::Snapshot> snapshot(std::int64_t now, Money cash, const std::set<std::string>& rows = {}) {
  auto s = std::make_shared<rest::Snapshot>();
  const auto add = [&](const std::string& tk) -> kalshi::MarketRow& {
    kalshi::MarketRow& m = s->markets[tk];
    m.ticker = tk; m.event_ticker = tk.substr(0, tk.rfind('-')); m.status = "active"; m.exchange_index = 0;
    m.open_ms = now - 60000; m.close_ms = (now / 1000 + 3600) * 1000;
    m.price_ranges = {{0, 10000, 100}}; m.fee_type = "quadratic"; m.fee_multiplier = 1; m.fee_ms = now;
    return m;
  };
  for (const std::string& tk : {A1, B1, B2, B3}) add(tk);
  for (const auto& k : kStrikes) {
    auto& m = add(k.tk);
    m.strike_type = k.type; m.floor_strike = k.floor; m.cap_strike = k.cap;
  }
  for (const auto& ev : rows) {
    auto& e = s->events[ev];
    e.event_ticker = ev; e.series_ticker = ev.substr(0, ev.find('-')); e.mutually_exclusive = true;
    for (const auto& [tk, m] : s->markets) if (m.event_ticker == ev) e.markets.push_back(m);
  }
  s->balance.available = cash; s->balances[0] = cash;
  s->exchange.exchange_active = s->exchange.trading_active = true;
  s->exchange.shards = {{0, true, true, std::nullopt}};
  s->clock.post_trusted = true;
  s->ready = s->account_ready = s->listing_ready = true;
  s->account_began_ms = now - 2000; s->account_ms = now - 1000;
  return s;
}

LaneSpec lane_spec(const std::string& id, Money budget) {
  return LaneSpec{id, "TEST " + id, "REST", budget, 2000, 5000, {}, std::nullopt};
}
// S1 counting its stakes by state, as its default row does, and M2 each market alone
std::vector<LaneSpec> arb_lanes() {
  auto s1 = lane_spec("S1", 15 * 10000);
  s1.by_state = true;
  return {s1, lane_spec("M2", 25 * 10000)};
}
void event_risk_on(ManualPath& p) { p.event_risk = true; }

struct Desk {
  KillSwitch kill;
  BoundedQueue<KalshiWriteJob> qs{4096, 64};   // send-k2's inbox: place() reads it, nothing sends
  std::unique_ptr<Gateway> gw;
  Worker wg{"gw-test"};
  std::shared_ptr<const rest::Snapshot> snap;   // none: no account read, so gw reconciles nothing
  std::mutex mu;
  std::map<std::string, json> rows;             // under mu: each lane's last row
  int holds = 0;                                // hold()'s fills so far, for their ids
  // setup: the path's other switches (deskd.event_risk, the lattice); events: the rows the account
  // read carries
  Desk(std::vector<LaneSpec> lanes, std::optional<Money> cash, const std::function<void(ManualPath&)>& setup = nullptr,
       const std::set<std::string>& events = {}) {
    if (cash) snap = snapshot(now_ms(), *cash, events);
    gw = std::make_unique<Gateway>(kill, [] { return std::vector<PreflightRow>{{"KEY", true, ""}}; });
    ManualPath path;
    path.snapshot = [this] { return std::atomic_load(&snap); };
    path.book = [](const std::string& tk) -> std::optional<MdBook> {
      MdBook b;
      b.ticker = tk; b.stale = false; b.bids = {{4000, 500}}; b.asks = {{4200, 300}};
      return b;
    };
    path.health = path.sys_health = [] {
      ManualHealth h; h.read_only = false; h.public_feed = h.private_feed = h.publisher = true;
      h.manual_sender = h.cancel_sender = true; return h;
    };
    path.sys_send = [this](KalshiWriteJob j) { return qs.push(std::move(j)); };
    path.sys_cancel = [this](KalshiWriteJob j) { return qs.push(std::move(j), Lane::PRIORITY); };
    path.publish_lane = [this](const std::string& row) {
      const auto j = json::parse(row);
      std::lock_guard<std::mutex> lk(mu);
      rows[j.at("id").get<std::string>()] = j;
    };
    path.lanes = std::move(lanes);
    path.check_each_message = true;
    if (setup) setup(path);
    gw->attach_manual(path);
    wg.start([this](Worker& w) { gw->run(w); });
  }
  ~Desk() {
    wg.request_stop();
    gw->inbox().kick();   // gw notices at once, not at its next 250 ms wake
    wg.join();
    CHECK(gw->stats().invariant_violations == 0);
  }
  template <class F> auto on_gw(F fn) {
    std::promise<decltype(fn())> p;
    auto f = p.get_future();
    CHECK(gw->post(Probe{[&] { p.set_value(fn()); }}));
    return f.get();
  }
  // Systematic running, and these lanes on with their order group on shard 0 active
  bool run(const std::vector<std::string>& lanes) {
    on_gw([&] {
      for (const auto& id : lanes) gw->sys_book()->group(id, 0) = {"og-" + id, SysBook::GroupState::ACTIVE, now_ms()};
      return true;
    });
    for (const auto& id : lanes) {
      auto p = std::make_shared<std::promise<LaneReply>>();
      auto f = p->get_future();
      CHECK(gw->post(LaneSet{id, true, p}));
      if (f.get().code != "") return false;
    }
    auto p = std::make_shared<std::promise<ControlReply>>();
    auto f = p->get_future();
    CHECK(gw->post(Start{p}));
    return f.get().code.empty();
  }
  // gw's refusal, or SENT: past every check, the create went to send-k2 (whose reply never comes
  // here, so the order stays working)
  OrderReply place(const std::string& lane, const std::string& cid, const std::string& tk, Side side, Px px, Qty n) {
    PlaceIntent p;
    p.cid = "S" + lane + "-" + cid; p.owner = Owner::SYS; p.lane = lane; p.ticker = tk; p.side = side; p.price = px;
    p.count = n; p.tif = Tif::POST; p.ttl_s = 60;
    p.reply = std::make_shared<std::promise<OrderReply>>();
    auto f = p.reply->get_future();
    const std::string sent = p.cid;
    CHECK(gw->post(std::move(p)));
    const auto end = mono_ms() + 4000;
    while (mono_ms() < end) {
      if (f.wait_for(std::chrono::milliseconds(2)) == std::future_status::ready) return f.get();
      while (const auto job = qs.try_pop()) if (job->cid == sent) return {"SENT", "", ""};
    }
    CHECK(!"gw answered");
    return {"TIMEOUT", "", ""};
  }
  // a new account read, once gw has taken it (its event rows and listing, for the shapes)
  bool read(std::shared_ptr<const rest::Snapshot> s) {
    std::atomic_store(&snap, std::move(s));
    return until([&] { return on_gw([&] { return gw->manual_book()->snapshot() == std::atomic_load(&snap).get(); }); });
  }
  // the lane's position, as if gw had booked a fill (no order of it in the ledger)
  void hold(const std::string& lane, const std::string& tk, Side side, Qty n, Px px) {
    const std::string id = std::to_string(++holds);
    CHECK(on_gw([&] { return gw->sys_book()->fill(lane, "S" + lane + "-held-" + id, tk, side, n, px, 0, now_ms(), "t-held-" + id); }));
  }
  Micros used(const std::string& lane) { return on_gw([&] { return gw->sys_used(only(lane), every); }); }
  json row(const std::string& lane) {
    std::lock_guard<std::mutex> lk(mu);
    const auto it = rows.find(lane);
    return it == rows.end() ? json() : it->second;
  }
  bool until(const std::function<bool()>& fn, int ms = 4000) {
    const auto end = mono_ms() + ms;
    do { if (fn()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while (mono_ms() < end);
    return fn();
  }
};

// gw's measures before sys_used: sys_money's, lane_row's and place_sys's own loops at 49b980e
struct Old {
  const SysBook& sys;
  const OrderLedger& ledger;
  // sys_money: every Systematic order's reservation, an ended one's too, and every lane's open worst case
  Micros used_all() const {
    Micros u = 0;
    for (const auto& [cid, o] : ledger.orders()) if (o.intent.owner == Owner::SYS) u += o.reservation();
    for (const auto& x : sys.lanes()) u += sys.open_worst(x.id);
    return u;
  }
  // lane_row's used (lane_json: open + reserved), and place_sys's lane budget before the order;
  // `ended`, with what the lane's ended orders still reserve
  Micros lane(const std::string& id, bool ended = false) const {
    Micros reserved = 0;
    for (const auto& [cid, o] : ledger.orders())
      if (o.intent.owner == Owner::SYS && (ended || !o.terminal()) && o.intent.lane == id) reserved += o.reservation();
    return sys.open_worst(id) + reserved;
  }
  // place_sys: the worst case of the part of p that grows its lane's position
  Micros worst(const PlaceIntent& p) const {
    Qty lane_pending = 0;
    for (const auto& [cid, o] : ledger.orders()) {
      if (o.intent.owner != Owner::SYS || o.terminal()) continue;
      if (o.intent.ticker == p.ticker && o.intent.side == p.side && o.intent.lane == p.lane) lane_pending += o.left;
    }
    const Qty dir_pos = (p.side == Side::BUY ? 1 : -1) * sys.position(p.lane, p.ticker) + lane_pending;
    const Qty grows = std::max<Qty>(0, dir_pos + p.count) - std::max<Qty>(0, dir_pos);
    return (p.side == Side::BUY ? p.price : kOneDollar - p.price) * grows;
  }
  // place_sys's correlated group: p and every lane's worst case in it, held and working
  Micros in_group(const PlaceIntent& p, const std::function<bool(const std::string&)>& in) const {
    Micros g = worst(p);
    for (const auto& l : sys.lanes()) g += sys.open_worst_in(l.id, in);
    for (const auto& [cid, o] : ledger.orders())
      if (o.intent.owner == Owner::SYS && !o.terminal() && in(o.intent.ticker)) g += o.reservation();
    return g;
  }
  // place_sys's event cap: p and its lane's worst case in the event, held and working
  Micros in_event(const PlaceIntent& p, const std::function<bool(const std::string&)>& in) const {
    Micros e = worst(p) + sys.open_worst_in(p.lane, in);
    for (const auto& [cid, o] : ledger.orders())
      if (o.intent.owner == Owner::SYS && !o.terminal() && o.intent.lane == p.lane && in(o.intent.ticker)) e += o.reservation();
    return e;
  }
};

// One order in the ledger in a random state: not sent yet; refused; resting, part filled, with
// some of its fills booked; an IOC ended with some of its fills booked; or cancelled.
void random_order(OrderLedger& ledger, std::mt19937& rng, const std::string& cid, Owner owner, const std::string& lane,
                  const std::string& tk) {
  PlaceIntent in;
  in.cid = cid; in.owner = owner; in.lane = lane; in.ticker = tk;
  in.side = rng() % 2 ? Side::BUY : Side::SELL;
  in.price = 10 * static_cast<Px>(1 + rng() % 999);
  in.count = 1 + static_cast<Qty>(rng() % 1500);
  in.tif = rng() % 3 ? Tif::POST : Tif::IOC;
  in.t_recv_ms = now_ms();
  if (in.tif == Tif::POST) in.exp_ms = in.t_recv_ms + 3600000;
  const Micros reserve = (in.side == Side::BUY ? in.price : kOneDollar - in.price) * in.count + rng() % 50000;
  if (!ledger.insert(in, 0, reserve)) { CHECK(!"the ledger took the order"); return; }
  const auto at = now_ms();
  const int how = rng() % 6;
  if (how == 0) return;
  if (how == 1) { CHECK(ledger.rejected(cid, "INSUFFICIENT_BALANCE", at)); return; }
  CHECK(ledger.sent(cid, at));
  kalshi::CreateAck ack;
  ack.order_id = "o-" + cid; ack.client_order_id = cid; ack.ts_ms = at;
  ack.fill_count = in.tif == Tif::POST ? rng() % in.count : rng() % (in.count + 1);   // a resting one has some left
  ack.remaining = in.tif == Tif::POST ? in.count - ack.fill_count : 0;
  CHECK(ledger.acknowledged(cid, ack, at));
  if (const Qty booked = rng() % (ack.fill_count + 1)) {
    kalshi::FillRow f;
    f.fill_id = "f-" + cid; f.trade_id = "t-" + cid; f.order_id = ack.order_id; f.ticker = tk;
    f.side = in.side == Side::BUY ? kalshi::BookSide::BID : kalshi::BookSide::ASK;
    f.count = booked; f.yes_price = in.price; f.ts_ms = at;
    CHECK(ledger.fill(f, cid, at) == OrderLedger::FillResult::APPLIED);
  }
  const auto* o = ledger.find(cid);
  if (how == 5 && o && !o->terminal())
    CHECK(ledger.cancelled(cid, kalshi::CancelAck{ack.order_id, cid, o->left, at}, "USER", at));
}

// sys_used against Old on random books, grown a step at a time. Pass 0: no account read. 1:
// deskd.event_risk on, event B's row in the read (B1 to B3 exclusive), and lanes that do not count
// by state. 2: lanes that do, with event_risk off. Every number is Old's (decisions 5). 3: both on:
// never more than Old with the ended orders' reservations (by state an ended order's fills not
// booked yet count in every measure), and less where the book hedges itself.
void same_numbers(int pass) {
  std::mt19937 rng(20261021 + pass);
  const std::vector<std::string> ids = {"R1", "R2", "R3"};
  const std::vector<std::string> tks = {A1, B1, B2, B3, "KXRISKC-26OCT06-T1"};
  int compared = 0, ended = 0, reducing = 0, netted = 0;
  for (int desk = 0; desk < 12; ++desk) {
    std::vector<LaneSpec> lanes = {lane_spec("R1", 100000), lane_spec("R2", 100000), lane_spec("R3", 100000)};
    for (auto& l : lanes) l.by_state = pass >= 2;
    Desk d(lanes, pass ? std::optional<Money>(1000 * 10000) : std::nullopt, pass % 2 ? event_risk_on : nullptr, {EV_B});
    if (pass) CHECK(d.read(std::atomic_load(&d.snap)));
    int n = 0;
    for (int step = 0; step < 25; ++step) {
      d.on_gw([&] {
        auto& sys = *d.gw->sys_book();
        auto& ledger = d.gw->manual_book()->ledger();
        for (int k = rng() % 4; k > 0; --k, ++n)   // a lane's fill: its position moves, or closes
          sys.fill(ids[rng() % ids.size()], "SX-" + std::to_string(n), tks[rng() % tks.size()], rng() % 2 ? Side::BUY : Side::SELL,
                   1 + rng() % 800, 10 * static_cast<Px>(1 + rng() % 999), 0, now_ms(), "trade-" + std::to_string(n));
        for (int k = rng() % 4; k > 0; --k, ++n) {
          // a lane's order, a lane this build does not have, or Manual's (which no measure counts)
          const int who = rng() % 8;
          const std::string lane = who < 6 ? ids[rng() % ids.size()] : who == 6 ? "Z9" : "";
          random_order(ledger, rng, (lane.empty() ? "M-" : "S" + lane + "-") + std::to_string(n), lane.empty() ? Owner::MAN : Owner::SYS,
                       lane, tks[rng() % tks.size()]);
        }
        const Old old{sys, ledger};
        if (d.gw->sys_used(every, every, nullptr, true) != d.gw->sys_used(every, every)) ++ended;
        if (pass == 3) {
          const Micros all = d.gw->sys_used(every, every, nullptr, true);
          CHECK(all <= old.used_all());
          if (all < old.used_all()) ++netted;
          for (const auto& id : ids) CHECK(d.gw->sys_used(only(id), every) <= old.lane(id, true));
          for (int k = 0; k < 4; ++k) {
            PlaceIntent p;
            p.owner = Owner::SYS; p.lane = ids[rng() % ids.size()]; p.ticker = tks[rng() % tks.size()];
            p.side = rng() % 2 ? Side::BUY : Side::SELL; p.price = 10 * static_cast<Px>(1 + rng() % 999);
            p.count = 1 + static_cast<Qty>(rng() % 1200);
            if (old.worst(p) == 0) ++reducing;
            CHECK(d.gw->sys_used(only(p.lane), every, &p) <= old.lane(p.lane, true) + old.worst(p));
            CHECK(d.gw->sys_used(every, every, &p, true) <= old.used_all() + old.worst(p));
            ++compared;
          }
          return true;
        }
        CHECK(d.gw->sys_used(every, every, nullptr, true) == old.used_all());
        for (const auto& id : ids) CHECK(d.gw->sys_used(only(id), every) == old.lane(id));
        // SysView::worst, by lane and market, adds up to the same measure over any markets (a
        // take engine's game_used is gw's event cap)
        const auto worst = d.gw->sys_worst();
        for (const std::string id : {"R1", "R2", "R3", "Z9"})
          for (unsigned mask = 0; mask < 1u << tks.size(); mask += 5) {
            const auto in = [&](const std::string& tk) {
              const auto i = std::find(tks.begin(), tks.end(), tk) - tks.begin();
              return i < static_cast<long>(tks.size()) && (mask >> i & 1u);
            };
            std::int64_t sum = 0;
            for (const auto& [key, w] : worst)
              if (key.compare(0, id.size() + 1, id + "|") == 0 && in(key.substr(id.size() + 1))) sum += w;
            CHECK(sum == d.gw->sys_used(only(id), in));
          }
        for (int k = 0; k < 4; ++k) {
          PlaceIntent p;
          p.owner = Owner::SYS; p.lane = ids[rng() % ids.size()]; p.ticker = tks[rng() % tks.size()];
          p.side = rng() % 2 ? Side::BUY : Side::SELL; p.price = 10 * static_cast<Px>(1 + rng() % 999);
          p.count = 1 + static_cast<Qty>(rng() % 1200);
          if (old.worst(p) == 0) ++reducing;
          CHECK(d.gw->sys_used(only(p.lane), every, &p) == old.lane(p.lane) + old.worst(p));
          CHECK(d.gw->sys_used(every, every, &p, true) == old.used_all() + old.worst(p));
          // a group or an event: some markets, the order's among them
          const auto mask = rng() | 1u << (std::find(tks.begin(), tks.end(), p.ticker) - tks.begin());
          const auto in = [&](const std::string& tk) {
            const auto i = std::find(tks.begin(), tks.end(), tk) - tks.begin();
            return i < static_cast<long>(tks.size()) && (mask >> i & 1u);
          };
          CHECK(d.gw->sys_used(every, in, &p) == old.in_group(p, in));
          CHECK(d.gw->sys_used(only(p.lane), in, &p) == old.in_event(p, in));
          // an order outside the slice adds nothing to it
          const auto out = [&](const std::string& tk) { return tk != p.ticker && in(tk); };
          CHECK(d.gw->sys_used(every, out, &p) == d.gw->sys_used(every, out));
          ++compared;
        }
        return true;
      });
    }
  }
  // the books held what tells the measures apart: ended orders with fills not booked, orders
  // that reduce, and (by state) hedges
  CHECK(compared == 12 * 25 * 4 && ended > 20 && reducing > 20);
  CHECK(pass < 3 || netted > 20);
}

// place_sys's limits through sys_used, at their edges
void limits() {
  Desk d({lane_spec("L1", 2 * 10000), lane_spec("L2", 10 * 10000), lane_spec("L3", 10 * 10000)}, 1000 * 10000);
  CHECK(d.run({"L1", "L2", "L3"}));
  // the lane's budget, $2: $1.60 working, then $0.41 more is past it and $0.40 is not
  CHECK(d.place("L1", "b-1", A1, Side::BUY, 4000, 400).code == "SENT");
  auto o = d.place("L1", "b-2", A1, Side::BUY, 4100, 100);
  CHECK(o.code == "SYS_CEILING" && o.msg == "lane L1 budget 2.0000");
  CHECK(d.place("L1", "b-3", A1, Side::BUY, 4000, 100).code == "SENT");
  CHECK(d.until([&] { return d.row("L1")["used"] == "2.0000" && d.row("L1")["wrk"] == 2; }));
  // long 5 at 0.40 besides: $4 used of $2; a sell of 3 reduces it and passes, a sell of 8 more
  // grows a short of 6 past the 3 working and is refused, and so is any buy
  CHECK(d.on_gw([&] { return d.gw->sys_book()->fill("L1", "SL1-held", A1, Side::BUY, 500, 4000, 0, now_ms(), "t-held"); }));
  CHECK(d.place("L1", "s-1", A1, Side::SELL, 4500, 300).code == "SENT");
  o = d.place("L1", "s-2", A1, Side::SELL, 4500, 800);
  CHECK(o.code == "SYS_CEILING" && o.msg == "lane L1 budget 2.0000");
  CHECK(d.place("L1", "b-4", A1, Side::BUY, 100, 100).code == "SYS_CEILING");
  CHECK(d.until([&] { return d.row("L1")["used"] == "5.6500" && d.row("L1")["open"] == "2.0000"; }));
  // event B's group, $15 over every lane: L2's $8 and L3's $8 pass it, $6.80 and $0.20 meet it,
  // $0.05 more passes it
  CHECK(d.place("L2", "g-1", B1, Side::BUY, 4000, 2000).code == "SENT");
  o = d.place("L3", "g-2", B2, Side::BUY, 4000, 2000);
  CHECK(o.code == "SYS_CEILING" && o.msg == "group " + EV_B + " worst case past 15.0000");
  CHECK(d.place("L3", "g-3", B2, Side::BUY, 4000, 1700).code == "SENT");
  CHECK(d.place("L3", "g-4", B2, Side::BUY, 2000, 100).code == "SENT");
  o = d.place("L3", "g-5", B2, Side::BUY, 500, 100);
  CHECK(o.code == "SYS_CEILING" && o.msg == "group " + EV_B + " worst case past 15.0000");
  CHECK(d.place("L3", "g-6", A1, Side::BUY, 500, 100).code == "SENT");   // another event's group
  // the header's used: every lane's
  CHECK(d.until([&] { const auto v = d.gw->sys_view(); return v && v->used == (56500 + 80000 + 70000 + 500) * 100; }));
  // and strat's view of it by lane and market (SysView::worst): L1 long 5 at 0.40 and working
  // $1.60 + $0.40 + the sell's $1.65
  CHECK(d.until([&] {
    const auto v = d.gw->sys_view();
    return v && v->worst == std::map<std::string, std::int64_t>{
                                {"L1|" + A1, 5650000}, {"L2|" + B1, 8000000}, {"L3|" + A1, 50000}, {"L3|" + B2, 7000000}};
  }));
}

// the 70% ceiling: $10 of cash is $7, past which no lane grows (each lane's budget is $10). The
// lanes went on, and Systematic started, while the cash was $1,000 (decisions 1: their budgets
// fit the ceiling then); the ceiling moves with the equity, and place_sys reads it now.
void ceiling() {
  Desk d({lane_spec("L1", 10 * 10000), lane_spec("L2", 10 * 10000)}, 1000 * 10000);
  CHECK(d.run({"L1", "L2"}));
  std::atomic_store(&d.snap, snapshot(now_ms(), 10 * 10000));
  CHECK(d.place("L1", "c-1", A1, Side::BUY, 4000, 1500).code == "SENT");   // $6.00
  auto o = d.place("L2", "c-2", B1, Side::BUY, 4000, 300);               // $7.20
  CHECK(o.code == "SYS_CEILING" && o.msg == "past the effective Systematic ceiling 7.0000");
  CHECK(d.place("L2", "c-3", B1, Side::BUY, 2500, 400).code == "SENT");    // $7.00
  CHECK(d.place("L2", "c-4", B1, Side::BUY, 100, 100).code == "SYS_CEILING");
  CHECK(d.until([&] {
    const auto v = d.gw->sys_view();
    return v && v->used == 7000000 && v->ceiling == 7000000 && v->eff == 7000000;
  }));
}

// $3.00 and $2.50 charged to an IOC lane, $1.00 rebated to a quoting one: $4.50 net, $5.50 spent
void rebates() {
  auto ioc = lane_spec("I1", 10 * 10000);
  ioc.style = "IOC";
  Desk d({ioc, lane_spec("L1", 10 * 10000)}, 1000 * 10000);
  CHECK(d.run({"I1", "L1"}));
  CHECK(d.on_gw([&] {
    auto& sys = *d.gw->sys_book();
    const auto t = now_ms();
    return sys.fill("I1", "SI1-1", A1, Side::BUY, 100, 4000, 3000000, t, "t-1") &&
           sys.fill("I1", "SI1-2", A1, Side::SELL, 100, 4100, 2500000, t, "t-2") &&
           sys.fill("L1", "SL1-1", B1, Side::BUY, 100, 4000, -1000000, t, "t-3");
  }));
  CHECK(d.until([&] { const auto v = d.gw->sys_view(); return v && v->fees == 5500000; }));   // hdr.cost used
  const auto o = d.place("I1", "i-1", A1, Side::BUY, 4000, 100);
  CHECK(o.code == "SYS_CEILING" && o.msg == "the IOC lanes' daily cost budget 5.0000 is spent");
  CHECK(d.place("L1", "q-1", B1, Side::BUY, 3900, 100).code == "SENT");   // a quoting lane is not capped
  CHECK(d.until([&] { return d.row("I1")["state"] == "CAPPED" && d.row("L1")["state"] == "ON"; }));
  CHECK(d.row("I1")["fees"] == "5.5000" && d.row("I1")["cap_left"] == "0.0000" && d.row("L1")["fees"] == "-1.0000");
}

// The 2026-09-05 KXHIGHLAX NO pair (tests/test_main.cpp:2025) in S1 and in M2: NO under 79 and NO
// 79-80, ten each at $8.50 ($1.50 brought in), is $17.00 market by market and $7.00 by state (one
// of the two wins whatever the high). deskd.event_risk off, every lane counts $17.00 as before; on,
// S1 nets and M2's KXHIGH positions keep today's numbers (decisions 5).
void kxhigh_book() {
  for (const bool on : {false, true}) {
    Desk d(arb_lanes(), 1000 * 10000, on ? event_risk_on : nullptr, {EV});
    CHECK(d.read(std::atomic_load(&d.snap)));
    for (const std::string lane : {"S1", "M2"}) {
      d.hold(lane, LO, Side::SELL, 1000, 1500);
      d.hold(lane, MID, Side::SELL, 1000, 1500);
    }
    CHECK(d.run({"S1", "M2"}));
    CHECK(d.on_gw([&] {
      const Old old{*d.gw->sys_book(), d.gw->manual_book()->ledger()};
      return old.lane("M2") == 17000000 && d.gw->sys_used(only("M2"), every) == old.lane("M2") &&
             d.gw->sys_used(only("S1"), every) == (on ? 7000000 : old.lane("S1")) &&
             d.gw->sys_used(every, every, nullptr, true) == (on ? 24000000 : old.used_all());
    }));
    const std::string s1 = on ? "7.0000" : "17.0000";
    CHECK(d.until([&] { return d.row("S1")["used"] == s1 && d.row("S1")["open"] == s1 && d.row("M2")["open"] == "17.0000"; }));
    CHECK(d.until([&] { const auto v = d.gw->sys_view(); return v && v->used == (on ? 24000000 : 34000000); }));
    // strat's view by market counts each market alone either way
    CHECK(d.until([&] {
      const auto v = d.gw->sys_view();
      return v && v->worst.count("S1|" + LO) && v->worst.at("S1|" + LO) == 8500000 && v->worst.at("S1|" + MID) == 8500000;
    }));
  }
}

// A held YES cover of KXHIGHTST, ten each at .30, .31 and .33: $9.40 market by market, a locked
// $0.60 by state. S1's $15 takes a second basket only by state; off, its second leg is past the
// budget. Without the event's row, or with one its strikes contradict, the same book counts market
// by market again: growth is refused and a sale that reduces still goes.
void second_basket() {
  for (const bool on : {false, true}) {
    Desk d(arb_lanes(), 1000 * 10000, on ? event_risk_on : nullptr, {EV});
    CHECK(d.read(std::atomic_load(&d.snap)));
    d.hold("S1", LO, Side::BUY, 1000, 3000);
    d.hold("S1", MID, Side::BUY, 1000, 3100);
    d.hold("S1", HI, Side::BUY, 1000, 3300);
    CHECK(d.run({"S1"}));
    CHECK(d.until([&] { return d.row("S1")["used"] == (on ? "0.0000" : "9.4000"); }));
    CHECK(d.place("S1", "b2-1", LO, Side::BUY, 3000, 1000).code == "SENT");
    auto o = d.place("S1", "b2-2", MID, Side::BUY, 3100, 1000);
    if (!on) {
      CHECK(o.code == "SYS_CEILING" && o.msg == "lane S1 budget 15.0000");   // $9.40 + $3.00 + $3.10
      continue;
    }
    CHECK(o.code == "SENT");
    CHECK(d.place("S1", "b2-3", HI, Side::BUY, 3300, 1000).code == "SENT");
    // every leg working: their $9.40 less the cheapest leg's $3.00 (it pays where it settles), less
    // the held lock's $0.60; the positions alone are a lock (open 0)
    CHECK(d.until([&] { return d.row("S1")["used"] == "5.8000" && d.row("S1")["open"] == "0.0000" && d.row("S1")["wrk"] == 3; }));
    CHECK(d.until([&] { const auto v = d.gw->sys_view(); return v && v->used == 5800000; }));
    // no row: $9.40 held and $9.40 working, market by market, so even one more contract elsewhere
    // (its 20 a market are full here) is past the budget
    CHECK(d.read(snapshot(now_ms(), 1000 * 10000)));
    CHECK(d.used("S1") == 18800000);
    o = d.place("S1", "b2-4", A1, Side::BUY, 3000, 100);
    CHECK(o.code == "SYS_CEILING" && o.msg == "lane S1 budget 15.0000");
    CHECK(d.place("S1", "s-1", HI, Side::SELL, 4500, 500).code == "SENT");   // 5 of the 10 held over 80
    // the row again: by state, the sale contingent at its $2.75 where HI settles YES
    CHECK(d.read(snapshot(now_ms(), 1000 * 10000, {EV})));
    CHECK(d.used("S1") == 8250000);
    // 79-80 read as 79-81 overlaps over 80, which the venue's flag says cannot be: no shape
    auto wide = std::make_shared<rest::Snapshot>(*snapshot(now_ms(), 1000 * 10000, {EV}));
    wide->markets[MID].cap_strike = 81;
    CHECK(d.read(wide));
    CHECK(d.used("S1") == 18800000 + 2750000);
  }
}

// Past the 70% ceiling ($10 of cash makes it $7, and M2 holds $8 in an event with no row), an S1
// leg the held cover already pays for adds nothing by state and goes; one that adds is refused;
// off, both are, and M2 grows nothing either way.
void covered_leg() {
  for (const bool on : {false, true}) {
    Desk d(arb_lanes(), 1000 * 10000, on ? event_risk_on : nullptr, {EV});
    CHECK(d.read(std::atomic_load(&d.snap)));
    d.hold("S1", LO, Side::BUY, 1000, 3000);
    d.hold("S1", MID, Side::BUY, 1000, 3100);
    d.hold("S1", HI, Side::BUY, 1000, 3300);
    d.hold("M2", A1, Side::BUY, 2000, 4000);
    CHECK(d.run({"S1", "M2"}));
    CHECK(d.read(snapshot(now_ms(), 10 * 10000, {EV})));
    const std::string past = "past the effective Systematic ceiling 7.0000";
    // one more under 79 at .30: it pays there, and the cover's $0.60 pays for it elsewhere
    auto o = d.place("S1", "c-1", LO, Side::BUY, 3000, 100);
    CHECK(on ? o.code == "SENT" : o.code == "SYS_CEILING" && o.msg == past);
    // three more: $0.60 at risk over 79
    o = d.place("S1", "c-2", LO, Side::BUY, 3000, 300);
    CHECK(o.code == "SYS_CEILING" && o.msg == past);
    o = d.place("M2", "c-3", B1, Side::BUY, 4000, 100);
    CHECK(o.code == "SYS_CEILING" && o.msg == past);
    CHECK(d.until([&] { const auto v = d.gw->sys_view(); return v && v->used == (on ? 8000000 : 17400000) && v->eff == 7000000; }));
  }
}

// The event's correlated group ($15, every lane's worst case there): S1's held cover counts $0 by
// state, so M2's two legs ($6.10) fit under it; off, the cover's $9.40 leaves no room for the second.
void group_netted() {
  for (const bool on : {false, true}) {
    Desk d(arb_lanes(), 1000 * 10000, on ? event_risk_on : nullptr, {EV});
    CHECK(d.read(std::atomic_load(&d.snap)));
    d.hold("S1", LO, Side::BUY, 1000, 3000);
    d.hold("S1", MID, Side::BUY, 1000, 3100);
    d.hold("S1", HI, Side::BUY, 1000, 3300);
    CHECK(d.run({"S1", "M2"}));
    CHECK(d.place("M2", "g-1", MID, Side::BUY, 3100, 1000).code == "SENT");
    const auto o = d.place("M2", "g-2", LO, Side::BUY, 3000, 1000);
    CHECK(on ? o.code == "SENT" : o.code == "SYS_CEILING" && o.msg == "group " + EV + " worst case past 15.0000");
    CHECK(d.until([&] { return d.row("M2")["used"] == (on ? "6.1000" : "3.1000"); }));
  }
}

// KXHIGHGAP's four markets cover every whole degree but not the real line: S1's YES on each, ten at
// .20, .25, .25 and .20, is $9.00 at risk (over 80 and under 81 none pays) until gw knows the
// series settles on whole degrees (ManualPath::lattice, A8's deskd.arb_lattice): then a $1.00 lock.
void lattice() {
  for (const double step : {0.0, 1.0}) {
    Desk d(arb_lanes(), 1000 * 10000,
           [step](ManualPath& p) {
             p.event_risk = true;
             p.lattice = [step](const std::string& series) { return series == "KXHIGHGAP" ? step : 0; };
           },
           {GAP});
    CHECK(d.read(std::atomic_load(&d.snap)));
    d.hold("S1", GAP + "-T79", Side::BUY, 1000, 2000);
    d.hold("S1", GAP + "-B79.5", Side::BUY, 1000, 2500);
    d.hold("S1", GAP + "-B81.5", Side::BUY, 1000, 2500);
    d.hold("S1", GAP + "-T82", Side::BUY, 1000, 2000);
    CHECK(d.used("S1") == (step ? 0 : 9000000));
  }
}

// An IOC leg whose 201 says all 10 filled before its fills arrive (it has ended): by state S1's
// own measure counts its $3.00 until the fill books the position (the lane budget, the group and
// the event cap), where market by market only the venue's used and the ceiling do.
void ioc_leg() {
  for (const bool on : {false, true}) {
    Desk d(arb_lanes(), 1000 * 10000, on ? event_risk_on : nullptr, {EV});
    CHECK(d.read(std::atomic_load(&d.snap)));
    const std::string cid = "SS1-leg-1";
    CHECK(d.on_gw([&] {
      auto& ledger = d.gw->manual_book()->ledger();
      PlaceIntent in;
      in.cid = cid; in.owner = Owner::SYS; in.lane = "S1"; in.ticker = LO; in.side = Side::BUY; in.price = 3000;
      in.count = 1000; in.tif = Tif::IOC; in.t_recv_ms = now_ms();
      kalshi::CreateAck ack;
      ack.order_id = "o-leg-1"; ack.client_order_id = cid; ack.fill_count = 1000; ack.remaining = 0; ack.ts_ms = now_ms();
      return ledger.insert(in, 0, 3000000) && ledger.sent(cid, now_ms()) && ledger.acknowledged(cid, ack, now_ms()) &&
             ledger.find(cid)->terminal();
    }));
    CHECK(d.used("S1") == (on ? 3000000 : 0));
    CHECK(d.on_gw([&] { return d.gw->sys_used(only("S1"), every, nullptr, true); }) == 3000000);
    CHECK(d.on_gw([&] {
      kalshi::FillRow f;
      f.fill_id = "f-leg-1"; f.trade_id = "t-leg-1"; f.order_id = "o-leg-1"; f.ticker = LO;
      f.side = kalshi::BookSide::BID; f.count = 1000; f.yes_price = 3000; f.fee = 70000; f.ts_ms = now_ms();
      return d.gw->manual_book()->ledger().fill(f, cid, now_ms()) == OrderLedger::FillResult::APPLIED &&
             d.gw->sys_book()->fill("S1", cid, LO, Side::BUY, 1000, 3000, 70000, now_ms(), "t-leg-1");
    }));
    CHECK(d.used("S1") == 3000000);
  }
}
}  // namespace

int main() {
  for (int pass = 0; pass < 4; ++pass) same_numbers(pass);
  limits(); ceiling(); rebates();
  kxhigh_book(); second_basket(); covered_leg(); group_netted(); lattice(); ioc_leg();
  return finish("deskd-sys-risk");
}
