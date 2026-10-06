// deskd bus: integer units, message lanes, the bounded two-lane queue, and the gateway's queue
// rules (Kill overtakes 100 queued orders and deletes them; a click followed at once by Esc
// never reaches the venue; a Stop overtakes a Start and the Start is answered, not timed out).
#include <atomic>
#include <chrono>
#include <climits>
#include <future>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/bus/msg.h"
#include "deskd/bus/queue.h"
#include "deskd/bus/units.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kill.h"

using namespace deskd;
using namespace std::chrono_literals;

static void test_units() {
  CHECK(parse_price("0.6400") == 6400);
  CHECK(parse_price("1.0000") == 10000);
  CHECK(parse_price("0.0000") == 0);
  CHECK(!parse_price("1.0001"));     // above a dollar
  CHECK(!parse_price("0.64"));       // exactly 4 decimals, as Kalshi's *_dollars
  CHECK(!parse_price(".6400"));
  CHECK(!parse_price("-0.6400"));
  CHECK(!parse_price("00.6400"));
  CHECK(price_str(6400) == "0.6400");
  CHECK(price_str(10000) == "1.0000");
  CHECK(price_str(1) == "0.0001");
  CHECK(parse_money("-3.2100") == -32100);
  CHECK(parse_money("142.3100") == 1423100);
  CHECK(!parse_money("-0.0000"));    // zero has one spelling
  CHECK(!parse_money("1.5"));
  CHECK(!parse_money("1234567890.0000"));   // at most 9 digits before the point
  CHECK(money_str(-32100) == "-3.2100");
  CHECK(money_str(0) == "0.0000");
  CHECK(money_str(-1) == "-0.0001");
  CHECK(money_str(LLONG_MIN).size() > 5);   // no overflow on the most negative value
  CHECK(qty_from_number(10) == 1000);
  CHECK(qty_from_number(85.5) == 8550);
  CHECK(qty_from_number(0.01) == 1);
  CHECK(qty_from_number(0.1 + 0.2) == 30);   // 0.30000000000000004 is 0.30 contracts
  CHECK(!qty_from_number(0.001));
  CHECK(!qty_from_number(-1));
  CHECK(qty_from_number(-1, true) == -100);
  CHECK(!qty_from_number(std::nan("")));
  CHECK(qty_to_number(1050) == 10.5);
}

static void test_lanes() {
  CHECK(lane_of(Kill{}) == Lane::PRIORITY);
  CHECK(lane_of(Stop{}) == Lane::PRIORITY);
  CHECK(lane_of(CancelIntent{}) == Lane::PRIORITY);
  CHECK(lane_of(OrderEvent{}) == Lane::PRIORITY);
  CHECK(lane_of(SendResult{}) == Lane::PRIORITY);
  CHECK(lane_of(Rearm{}) == Lane::PRIORITY);
  CHECK(lane_of(PlaceIntent{}) == Lane::NORMAL);
  CHECK(lane_of(AmendIntent{}) == Lane::NORMAL);
  CHECK(lane_of(Start{}) == Lane::NORMAL);   // so a later Stop overtakes it
}

static void test_queue() {
  // the priority lane overtakes: one Kill behind 100 orders is popped first
  {
    BoundedQueue<int> q(1000, 10);
    for (int i = 0; i < 100; ++i) CHECK(q.push(i));
    CHECK(q.push(-1, Lane::PRIORITY));
    CHECK(q.pop_for(0ms) == -1);
    CHECK(q.pop_for(0ms) == 0);   // then the normal lane, in order
    CHECK(q.pop_for(0ms) == 1);
    CHECK(q.size() == 98);
  }
  // a full lane drops, counts, and marks a gap once; the other lane still takes messages
  {
    BoundedQueue<int> q(3, 2);
    CHECK(q.push(1) && q.push(2) && q.push(3));
    CHECK(!q.take_gap());
    CHECK(!q.push(4));
    CHECK(q.dropped() == 1);
    CHECK(q.take_gap());
    CHECK(!q.take_gap());          // taken
    CHECK(q.push(9, Lane::PRIORITY));
    CHECK(q.pop_for(0ms) == 9);
    CHECK(q.pop_for(0ms) == 1);
    CHECK(q.push(5));              // room again
    CHECK(q.size() == 3);
  }
  // erase_if deletes from one lane only and hands back what it deleted
  {
    BoundedQueue<int> q(10, 10);
    for (int i = 0; i < 6; ++i) q.push(i);
    q.push(100, Lane::PRIORITY);
    std::vector<int> gone;
    CHECK(q.erase_if(Lane::NORMAL, [](int v) { return v % 2 == 0; }, &gone) == 3);
    CHECK((gone == std::vector<int>{0, 2, 4}));
    CHECK(q.size(Lane::PRIORITY) == 1);
    CHECK(q.pop_for(0ms) == 100);
    CHECK(q.pop_for(0ms) == 1);
  }
  // pop_for waits at most its timeout; close wakes a waiter and refuses pushes
  {
    BoundedQueue<int> q(4, 4);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!q.pop_for(50ms));
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= 45ms && waited < 1000ms);
    q.push(7);
    std::thread closer([&q] {
      std::this_thread::sleep_for(30ms);
      q.close();
    });
    CHECK(q.pop_for(0ms) == 7);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(!q.pop_for(5000ms));     // woken by close, not the timeout
    CHECK(std::chrono::steady_clock::now() - t1 < 2000ms);
    closer.join();
    CHECK(!q.push(8));
  }
  // many producers, one consumer: nothing lost below capacity
  {
    BoundedQueue<int> q(100000, 100000);
    std::vector<std::thread> ps;
    for (int t = 0; t < 4; ++t)
      ps.emplace_back([&q, t] {
        for (int i = 0; i < 5000; ++i) q.push(i, (i % 7 == 0) ? Lane::PRIORITY : Lane::NORMAL);
        (void)t;
      });
    int got = 0;
    while (got < 20000) {
      if (q.pop_for(1000ms)) ++got;
      else break;
    }
    for (auto& p : ps) p.join();
    CHECK(got == 20000);
    CHECK(q.dropped() == 0);
  }
}

static void test_kill_switch() {
  KillSwitch k;
  CHECK(!k.active());
  CHECK(k.rearm(1000) == "not KILLED");
  CHECK(k.trip(1791306000000));
  CHECK(k.active());
  CHECK(!k.trip(1791306005000));                       // a second KILL keeps the first's times
  CHECK(k.since_ms() == 1791306000000);
  CHECK(k.rearm_ms() == 1791306060000);
  CHECK(k.rearm(1791306059000) == "rearm in 1 s");     // the 60 s lockout
  CHECK(k.active());
  CHECK(k.rearm(1791306060000).empty());
  CHECK(!k.active());
  CHECK(k.rearm_ms() == 0);

  // KILLED outlives the process until a re-arm
  TempDir d;
  const std::string file = d.file("deskd.killed");
  {
    KillSwitch a;
    a.persist_to(file);
    CHECK(a.restore() == 0);                 // nothing to restore
    CHECK(a.trip(1791306000000));
  }
  {
    KillSwitch b;                            // the next start
    b.persist_to(file);
    CHECK(b.restore() == 1791306000000);
    CHECK(b.active() && b.rearm_ms() == 1791306060000);
    CHECK(!b.rearm(1791306000500).empty());
    CHECK(b.rearm(1791306061000).empty());
  }
  {
    KillSwitch c;
    c.persist_to(file);
    CHECK(c.restore() == 0);                 // re-armed: the next start is not KILLED
    CHECK(!c.active());
  }
  {
    std::FILE* f = std::fopen(file.c_str(), "w");
    std::fputs("garbage\n", f);
    std::fclose(f);
    KillSwitch e;
    e.persist_to(file);
    CHECK(e.restore() == 1);                 // unreadable still means KILLED (lockout over)
    CHECK(e.rearm(1791306000000).empty());
  }
}

namespace {
std::vector<PreflightRow> rows(bool all_ok) {
  std::vector<PreflightRow> r;
  for (const char* id : {"KEY", "CASH", "SHARD", "CLK", "FEED", "PRIV", "GROUPS"}) r.push_back({id, all_ok, all_ok ? "" : "x"});
  r.push_back({"DISK", false, "3.0 GB free < 10"});   // DISK only warns: never blocks a Start
  return r;
}
PlaceIntent order(const std::string& cid, Owner own, const std::string& tk) {
  PlaceIntent p;
  p.cid = cid;
  p.owner = own;
  p.ticker = tk;
  p.price = 6300;
  p.count = 1000;
  p.tif = Tif::POST;
  p.ttl_s = 15;
  return p;
}
// Blocks the gw thread until release() (a frozen gateway); returns once gw is inside it.
struct Freeze {
  std::promise<void> entered, release;
  explicit Freeze(Gateway& gw) {
    auto fut = release.get_future().share();
    gw.post(Probe{[this, fut] {
      entered.set_value();
      fut.wait();
    }});
    entered.get_future().wait();
  }
  void thaw() { release.set_value(); }
};
// waits until gw has handled n messages in all, or 2 s
bool settled(const Gateway& gw, std::uint64_t n) {
  for (int i = 0; i < 400; ++i) {
    if (gw.stats().handled.load() >= n) return true;
    std::this_thread::sleep_for(5ms);
  }
  return false;
}
}  // namespace

static void test_gateway_rules() {
  KillSwitch kill;
  std::atomic<bool> all_ok{false};   // read on the gw thread
  Gateway gw(kill, [&all_ok] { return rows(all_ok.load()); });
  Worker w("gw");
  w.start([&gw](Worker& self) { gw.run(self); });

  // Kill overtakes 100 queued orders and deletes them all: none is ever handled as an order
  {
    Freeze f(gw);
    for (int i = 0; i < 100; ++i) CHECK(gw.post(order("Mq" + std::to_string(i) + "abcd", Owner::MAN, "KXT-A")));
    CHECK(kill.trip(now_ms()));
    gw.post(Kill{now_ms(), "test"});
    CHECK(gw.inbox().size(Lane::NORMAL) == 100);
    f.thaw();
    CHECK(settled(gw, 2));   // the probe and the Kill; nothing else
    CHECK(gw.stats().erased_by_kill.load() == 100);
    CHECK(gw.stats().place_refused.load() == 0);
    CHECK(gw.inbox().size() == 0);
    CHECK(gw.run_view().state == RunState::KILLED);
    CHECK(kill.rearm(kill.rearm_ms()).empty());
    gw.post(Rearm{now_ms()});
    CHECK(settled(gw, 3));
    CHECK(gw.run_view().state == RunState::STOP);
  }
  // a click followed at once by Esc: the market cancel deletes the queued order; another
  // market's order and a SYS order on the same market are not covered by a MAN cancel
  {
    const std::uint64_t h0 = gw.stats().handled.load(), r0 = gw.stats().place_refused.load();
    Freeze f(gw);
    gw.post(order("Mclick-1", Owner::MAN, "KXT-A"));
    gw.post(order("Mclick-2", Owner::MAN, "KXT-B"));
    gw.post(order("SM1-click-3", Owner::SYS, "KXT-A"));
    CancelIntent esc;
    esc.scope = CancelIntent::Scope::MARKET;
    esc.venue = Venue::K;
    esc.ticker = "KXT-A";
    esc.own = CancelIntent::Own::MAN;
    gw.post(esc);
    f.thaw();
    CHECK(settled(gw, h0 + 4));   // probe, cancel, the two orders it left
    CHECK(gw.stats().erased_by_cancel.load() == 1);
    CHECK(gw.stats().place_refused.load() == r0 + 2);
  }
  // a Stop overtakes a Start sent before it: the Start is deleted and answered at once
  {
    Freeze f(gw);
    auto ps = std::make_shared<std::promise<ControlReply>>();
    auto pt = std::make_shared<std::promise<ControlReply>>();
    gw.post(Start{ps});
    gw.post(Stop{"operator", pt});
    f.thaw();
    auto fs = ps->get_future(), ft = pt->get_future();
    CHECK(ft.wait_for(1s) == std::future_status::ready);
    CHECK(fs.wait_for(1s) == std::future_status::ready);
    const ControlReply rs = fs.get(), rt = ft.get();
    CHECK(rs.code == "BAD_STATE");
    CHECK(rt.code.empty() && rt.run.state == RunState::STOP && rt.run.why == "operator");
    CHECK(gw.stats().erased_by_stop.load() == 1);
  }
  // Start: refused PREFLIGHT while a check fails (DISK never counts), RUN once they pass
  {
    auto p = std::make_shared<std::promise<ControlReply>>();
    gw.post(Start{p});
    auto f = p->get_future();
    CHECK(f.wait_for(1s) == std::future_status::ready);
    ControlReply r = f.get();
    CHECK(r.code == "PREFLIGHT");
    CHECK(r.preflight.size() == 8);
    CHECK(r.msg.find("DISK") == std::string::npos);
    all_ok.store(true);
    auto p2 = std::make_shared<std::promise<ControlReply>>();
    gw.post(Start{p2});
    auto f2 = p2->get_future();
    CHECK(f2.wait_for(1s) == std::future_status::ready);
    ControlReply r2 = f2.get();
    CHECK(r2.code.empty());
    CHECK(r2.run.state == RunState::RUN);
    // KILL while running: the kill switch reads first, without waiting for gw
    kill.trip(now_ms());
    CHECK(gw.run_view().state == RunState::KILLED);
    CHECK(gw.run_view().rearm_ms == kill.rearm_ms());
  }
  w.request_stop();
  w.join();
}

int main() {
  test_units();
  test_lanes();
  test_queue();
  test_kill_switch();
  test_gateway_rules();
  return finish("deskd-bus");
}
