// gw's one measure of Systematic's risk, Gateway::sys_used (plan section 4's limits; contract
// hdr.sys and the lane row's used), on gw with no venue (send-k2's inbox is a queue the test
// reads and nothing sends from):
//   - on random books (the lanes' positions by market; Systematic orders resting, part filled
//     with fills not booked yet, ended with fills not booked yet, refused; Manual's beside them),
//     sys_used counts what gw's own loops counted before it (gateway.cpp at 49b980e, kept below
//     as Old): the venue's used, each lane's, and an order's lane budget, 70% ceiling, correlated
//     group and event cap
//   - through place_sys: the lane budget, the ceiling and the group refuse just past their edge
//     and not at it, an order counts only the part of it that grows its lane's position, and a
//     reducing order at a full budget passes
//   - the IOC lanes' $5 cost budget counts every fee charged: a rebate (a negative fee) is the
//     lane's P&L but gives none of it back (decisions 6)
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
const auto every = [](const std::string&) { return true; };
std::function<bool(const std::string&)> only(const std::string& id) { return [id](const std::string& x) { return x == id; }; }

std::shared_ptr<const rest::Snapshot> snapshot(std::int64_t now, Money cash) {
  auto s = std::make_shared<rest::Snapshot>();
  for (const std::string& tk : {A1, B1, B2}) {
    kalshi::MarketRow m;
    m.ticker = tk; m.event_ticker = tk.substr(0, tk.rfind('-')); m.status = "active"; m.exchange_index = 0;
    m.open_ms = now - 60000; m.close_ms = (now / 1000 + 3600) * 1000;
    m.price_ranges = {{0, 10000, 100}}; m.fee_type = "quadratic"; m.fee_multiplier = 1; m.fee_ms = now;
    s->markets[tk] = m;
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

struct Desk {
  KillSwitch kill;
  BoundedQueue<KalshiWriteJob> qs{4096, 64};   // send-k2's inbox: place() reads it, nothing sends
  std::unique_ptr<Gateway> gw;
  Worker wg{"gw-test"};
  std::shared_ptr<const rest::Snapshot> snap;   // none: no account read, so gw reconciles nothing
  std::mutex mu;
  std::map<std::string, json> rows;             // under mu: each lane's last row
  Desk(std::vector<LaneSpec> lanes, std::optional<Money> cash) {
    if (cash) snap = snapshot(now_ms(), *cash);
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
    gw->attach_manual(path);
    wg.start([this](Worker& w) { gw->run(w); });
  }
  ~Desk() {
    wg.request_stop();
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
  // lane_row's used (lane_json: open + reserved), and place_sys's lane budget before the order
  Micros lane(const std::string& id) const {
    Micros reserved = 0;
    for (const auto& [cid, o] : ledger.orders())
      if (o.intent.owner == Owner::SYS && !o.terminal() && o.intent.lane == id) reserved += o.reservation();
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

// sys_used against Old on random books, grown a step at a time
void same_numbers() {
  std::mt19937 rng(20261021);
  const std::vector<std::string> ids = {"R1", "R2", "R3"};
  const std::vector<std::string> tks = {A1, B1, B2, EV_B + "-T3", "KXRISKC-26OCT06-T1"};
  int compared = 0, ended = 0, reducing = 0;
  for (int desk = 0; desk < 12; ++desk) {
    Desk d({lane_spec("R1", 100000), lane_spec("R2", 100000), lane_spec("R3", 100000)}, std::nullopt);
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
        CHECK(d.gw->sys_used(every, every, nullptr, true) == old.used_all());
        if (d.gw->sys_used(every, every, nullptr, true) != d.gw->sys_used(every, every)) ++ended;
        for (const auto& id : ids) CHECK(d.gw->sys_used(only(id), every) == old.lane(id));
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
  // the books held what tells the measures apart: ended orders with fills not booked, and
  // orders that reduce
  CHECK(compared == 12 * 25 * 4 && ended > 20 && reducing > 20);
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
}  // namespace

int main() {
  same_numbers(); limits(); ceiling(); rebates();
  return finish("deskd-sys-risk");
}
