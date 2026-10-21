// deskd bus: integer units, message lanes, the bounded three-lane queue, the kill switch, and the
// gateway's queue rules (KILL deletes 100 queued orders without a message of its own; a click
// followed at once by Esc never reaches the venue; a Stop overtakes a Start and the Start is
// answered, not timed out; a re-arm never flips a later Start back to STOP), and send-cx taking a
// KILL with its inbox full.
#include <signal.h>

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
#include "deskd/net/send.h"

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
  CHECK(lane_of(Stop{}) == Lane::CONTROL);   // nothing else fills it
  CHECK(lane_of(CancelIntent{}) == Lane::PRIORITY);
  CHECK(lane_of(OrderEvent{}) == Lane::PRIORITY);
  CHECK(lane_of(SendResult{}) == Lane::PRIORITY);
  CHECK(lane_of(PlaceIntent{}) == Lane::NORMAL);
  CHECK(lane_of(AmendIntent{}) == Lane::NORMAL);
  CHECK(lane_of(Start{}) == Lane::NORMAL);   // so a later Stop overtakes it
  CHECK(lane_of(LaneMarkout{}) == Lane::NORMAL);   // nothing is late for it
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
  // the control lane: a Stop behind a full priority lane and 1,000 orders is next, and a flood of
  // private events cannot crowd it out (they fill their own lane and are dropped there)
  {
    BoundedQueue<int> q(1000, 8, 4);
    for (int i = 0; i < 1000; ++i) q.push(i);
    for (int i = 0; i < 20; ++i) q.push(5000 + i, Lane::PRIORITY);   // 8 fit, 12 dropped
    CHECK(q.dropped() == 12);
    CHECK(q.push(-7, Lane::CONTROL));
    CHECK(q.pop_for(0ms) == -7);
    CHECK(q.pop_for(0ms) == 5000);
    CHECK(q.size(Lane::CONTROL) == 0);
  }
  // kick wakes a waiting reader without a message
  {
    BoundedQueue<int> q(4, 4);
    std::thread kicker([&q] {
      std::this_thread::sleep_for(30ms);
      q.kick();
    });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!q.pop_for(5000ms));
    CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
    kicker.join();
    q.push(3);
    CHECK(q.pop_for(0ms) == 3);   // a kick leaves nothing behind
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
  const std::int64_t W = 1791306000000, M = 5000000;   // a wall time and a steady time
  KillSwitch k;
  int woken = 0;
  k.on_trip([&woken] { ++woken; });
  CHECK(!k.active());
  CHECK(k.generation() == 0 && k.state_gen() == 0);
  CHECK(k.rearm(W, M) == "not KILLED");
  CHECK(k.trip(W, M, "operator"));
  CHECK(k.active() && k.why() == "operator");
  CHECK(k.generation() == 1 && k.state_gen() == 1 && woken == 1);
  CHECK(!k.trip(W + 5000, M + 5000, "again"));        // a second KILL keeps the first's times ...
  CHECK(k.generation() == 2 && k.state_gen() == 1);   // ... but is a second cancel-all
  CHECK(woken == 2);
  CHECK(k.since_ms() == W && k.why() == "operator");
  CHECK(k.rearm_ms() == W + 60000);
  CHECK(k.rearm(W + 59000, M + 59000) == "rearm in 1 s");   // the 60 s lockout
  // the lockout runs on the steady clock: a wall clock stepped forward cannot shorten it, and
  // one stepped back cannot lengthen it
  CHECK(!k.rearm(W + 3600000, M + 1000).empty());
  CHECK(k.active());
  CHECK(k.rearm(W - 3600000, M + 60000).empty());
  CHECK(!k.active());
  CHECK(k.rearm_ms() == 0 && k.rearmed_ms() == W - 3600000);
  CHECK(k.state_gen() == 2 && k.generation() == 2);

  // the lockout lasts 60 s from the moment send-cx's cancel-all returns, too: the venue cancels
  // orders placed in the minute after a cancel-all
  {
    KillSwitch c;
    CHECK(c.trip(W, M, "operator"));
    c.cancel_all_done(W + 4000, M + 4000);
    CHECK(c.rearm_ms() == W + 64000);
    CHECK(c.rearm(W + 60000, M + 60000) == "rearm in 4 s");
    CHECK(c.rearm(W + 64000, M + 64000).empty());
    c.cancel_all_done(W + 70000, M + 70000);   // not KILLED: nothing to extend
    CHECK(!c.active() && c.rearm_ms() == 0);
  }

  // KILLED outlives the process until a re-arm
  TempDir d;
  const std::string file = d.file("kalshi-demo.killed");
  {
    KillSwitch a;
    a.persist_to(file);
    CHECK(a.restore(W, M) == 0);             // nothing to restore
    CHECK(a.generation() == 0);
    CHECK(a.trip(W, M, "operator"));
  }
  {
    KillSwitch b;                            // the next start, 10 s later
    b.persist_to(file);
    CHECK(b.restore(W + 10000, 777) == W);
    CHECK(b.active() && b.why() == "restart");
    CHECK(b.generation() == 1);              // send-cx cancels again at start
    CHECK(b.rearm_ms() == W + 60000);        // what is left of the lockout, by the wall clock
    CHECK(b.rearm(W + 50000, 777 + 49999) == "rearm in 1 s");
    CHECK(b.rearm(W + 61000, 777 + 50000).empty());
  }
  {
    KillSwitch c;
    c.persist_to(file);
    CHECK(c.restore(W, M) == 0);             // re-armed: the next start is not KILLED
    CHECK(!c.active());
  }
  {
    // a KILL, a re-arm, and a KILL again: the last one is on disk (the re-arm's unlink and the
    // trip's write share one lock, so a trip can no longer lose its file to a re-arm)
    KillSwitch e;
    e.persist_to(file);
    CHECK(e.trip(W, M, "operator"));
    CHECK(e.rearm(W + 60000, M + 60000).empty());
    CHECK(e.trip(W + 61000, M + 61000, "operator"));
    KillSwitch f;
    f.persist_to(file);
    CHECK(f.restore(W + 62000, 1) == W + 61000);
  }
  {
    // a restore long after the KILL: the lockout is over, only the re-arm is needed; a wall
    // clock far behind the file never makes it longer than a whole lockout
    KillSwitch g;
    g.persist_to(file);
    CHECK(g.restore(W + 3600000, 10) == W + 61000);
    CHECK(g.rearm(W + 3600000, 10).empty());
    KillSwitch h;
    h.persist_to(file);
    CHECK(h.restore(W, 10) == 0);
  }
  {
    std::FILE* f = std::fopen(file.c_str(), "w");
    std::fputs("garbage\n", f);
    std::fclose(f);
    KillSwitch e;
    e.persist_to(file);
    CHECK(e.restore(W, M) == 1);             // unreadable still means KILLED (lockout over)
    CHECK(e.rearm(W, M).empty());
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
  std::int64_t mono = mono_ms();   // the kill switch's steady clock, moved by hand past lockouts

  // KILL overtakes 100 queued orders and deletes them all, with no message of its own: the trip
  // kicks gw's inbox and gw reads the switch before the next message
  {
    Freeze f(gw);
    for (int i = 0; i < 100; ++i) CHECK(gw.post(order("Mq" + std::to_string(i) + "abcd", Owner::MAN, "KXT-A")));
    CHECK(gw.post(LaneMarkout{"C1", "f-1", "300s", -100, 100}));   // a lane's markout is no order: it stays
    CHECK(kill.trip(now_ms(), mono, "test"));
    CHECK(gw.inbox().size(Lane::NORMAL) == 101);
    f.thaw();
    CHECK(settled(gw, 2));   // the probe and the markout; nothing else
    for (int i = 0; i < 200 && gw.stats().kills_seen.load() == 0; ++i) std::this_thread::sleep_for(5ms);
    CHECK(gw.stats().kills_seen.load() == 1);
    CHECK(gw.stats().erased_by_kill.load() == 100);
    CHECK(gw.stats().place_refused.load() == 0);
    CHECK(gw.stats().handled.load() == 2);
    CHECK(gw.inbox().size() == 0);
    CHECK(gw.run_view().state == RunState::KILLED);
    mono += 60000;
    CHECK(kill.rearm(now_ms(), mono).empty());
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
  // a Stop deletes the Systematic orders queued before it, never a Manual one (plan section 2;
  // docs/invariants.md S2-05)
  {
    const std::uint64_t e0 = gw.stats().erased_by_stop.load(), h0 = gw.stats().handled.load();
    // which one went: the erased order is answered BAD_STATE by the Stop, the one left is handled
    // (refused RO here, with no venue path attached)
    auto sys = order("SM1-queued-1", Owner::SYS, "KXT-A");
    auto man = order("Mqueued-2", Owner::MAN, "KXT-A");
    sys.reply = std::make_shared<std::promise<OrderReply>>();
    man.reply = std::make_shared<std::promise<OrderReply>>();
    auto fsys = sys.reply->get_future(), fman = man.reply->get_future();
    Freeze f(gw);
    gw.post(std::move(sys));
    gw.post(std::move(man));
    gw.post(LaneMarkout{"C1", "f-2", "300s", 100, 100});
    auto pt = std::make_shared<std::promise<ControlReply>>();
    gw.post(Stop{"operator", pt});
    f.thaw();
    CHECK(pt->get_future().wait_for(1s) == std::future_status::ready);
    CHECK(gw.stats().erased_by_stop.load() == e0 + 1);
    CHECK(settled(gw, h0 + 4));   // probe, the Stop, the Manual order and the markout it left
    CHECK(fsys.wait_for(1s) == std::future_status::ready && fman.wait_for(1s) == std::future_status::ready);
    const auto rs = fsys.get(), rm = fman.get();
    CHECK(rs.code == "BAD_STATE");   // erased by the Stop
    CHECK(rm.code == "RO");          // handled: no venue path in this fixture
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
    kill.trip(now_ms(), mono, "operator");
    CHECK(gw.run_view().state == RunState::KILLED);
    CHECK(gw.run_view().rearm_ms == kill.rearm_ms());
    CHECK(gw.run_view().why == "operator");
    for (int i = 0; i < 200 && gw.stats().kills_seen.load() < 2; ++i) std::this_thread::sleep_for(5ms);
    CHECK(gw.stats().kills_seen.load() == 2);
  }
  // A re-arm, then a Start that gw answers RUN: nothing flips it back to STOP afterwards. Until
  // 2026-10-06 the route cleared the switch and then posted a Rearm message; a Start handled
  // between the two answered RUN and the Rearm then set STOP. Now gw applies the re-arm itself,
  // before the next message, so it is the same here even with gw frozen through both.
  {
    Freeze f(gw);
    mono += 60000;
    CHECK(kill.rearm(now_ms(), mono).empty());
    auto p = std::make_shared<std::promise<ControlReply>>();
    gw.post(Start{p});
    f.thaw();
    auto fut = p->get_future();
    CHECK(fut.wait_for(1s) == std::future_status::ready);
    const ControlReply r = fut.get();
    CHECK(r.code.empty() && r.run.state == RunState::RUN);
    std::this_thread::sleep_for(600ms);   // two of gw's idle turns: still RUN
    CHECK(gw.run_view().state == RunState::RUN);
  }
  w.request_stop();
  w.join();
}

// send-cx takes a KILL from the switch, never from its inbox: with the inbox full (cancels
// dropped, a gap marked) the KILL still goes, and before any queued job.
static void test_cancel_sender_kill() {
  KillSwitch kill;
  BoundedQueue<SendJob> q(8, 8);
  watch_kill(kill, q);
  SenderStats stats;
  for (int i = 0; i < 40; ++i) q.push(SendJob{SendJob::Kind::WRITE, now_ms(), "Mfull-" + std::to_string(i)},
                                      i % 2 ? Lane::PRIORITY : Lane::NORMAL);
  CHECK(q.size() == 16 && q.dropped() == 24);   // full
  CHECK(kill.trip(now_ms(), mono_ms(), "operator"));
  std::atomic<int> alerts{0};
  Worker cx("send-cx");
  cx.start([&](Worker& self) {
    run_cancel_sender(self, q, stats, kill, [&alerts](const std::string&, const std::string&, const std::string&) { ++alerts; });
  });
  for (int i = 0; i < 400 && stats.writes_refused.load() < 16; ++i) std::this_thread::sleep_for(5ms);
  CHECK(stats.kills.load() == 1);
  CHECK(stats.kill_gen_seen.load() == kill.generation());
  CHECK(stats.writes_before_kill.load() == 0);   // the KILL went before every queued job
  CHECK(stats.writes_refused.load() == 16);
  CHECK(alerts.load() == 1);
  // a second KILL while it waits on an empty inbox: woken by the trip, not by its 250 ms wait
  const auto t0 = std::chrono::steady_clock::now();
  kill.trip(now_ms(), mono_ms(), "operator");
  for (int i = 0; i < 400 && stats.kills.load() < 2; ++i) std::this_thread::sleep_for(1ms);
  CHECK(stats.kills.load() == 2);
  CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
  cx.request_stop();
  cx.join();
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);   // every deskd test main: an embedder never dies of a peer's close
  test_units();
  test_lanes();
  test_queue();
  test_kill_switch();
  test_gateway_rules();
  test_cancel_sender_kill();
  return finish("deskd-bus");
}
