// S1 ARB (plan 5(c)) in the whole process, against the fake Kalshi over TLS, on
// runtime_fixture.h's harness. Seven events of one weather series, each three brackets (under 79,
// 79 to 80, over 80) that the venue flags mutually exclusive and whose strikes tile the line, each
// opened in turn by asks summing under $1 (a YES cover of 10 at 0.28, 0.30, 0.32; the 0.28's 10 the
// scarcest leg) and closed again once its basket is done:
//   - arb_mode scan with Systematic orders off: arb_scans rows for the lock (YES_COVER, never
//     fired) and nothing sent; with them on, S1's LaneSet refused as gated by scan mode
//   - trade, S1 on and Start: the scarcest leg alone, then the other two; every bracket +10 on the
//     venue, each in S1's order group, fills of lane S1, S1's used $0 (a complete basket counted
//     by its states), its group never triggered, the basket DONE in ops.sqlite's baskets
//   - a missed leg completed: the touch bracket B's create would take pulled, with liquidity a
//     tick worse: B completed a tick through, every bracket +10, legging -$0.10
//   - a missed leg unwound: nothing within a tick: A and C sold back, the event flat on the venue,
//     and the legging what s1.json keeps
//   - an UNKNOWN leg: B's create answered by nothing: no second order on B while it is uncertain;
//     the account read settles it filled, and the basket is complete
//   - KILL mid-basket (B's create stalled at the venue): C never leaves, S1's group is triggered;
//     re-armed and started, the stranded legs are unwound to equal holdings
//   - Stop mid-basket: C never leaves; started again, the stranded legs are unwound
//   - the legging pause: a restart with deskd.arb_legging_pause under what legging lost: the next
//     lock is not traded and /ops/status arb says paused; S1 turned off and on, it trades
// Every run ends with gw's invariant checks at zero (the harness).
#include <algorithm>
#include <cmath>
#include <csignal>
#include <fstream>
#include "runtime_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
using rt::Run;
using rt::sql_count;

// "2026-10-22" -> "26OCT22", as Kalshi dates an event
std::string kalshi_date(const std::string& date) {
  static const char* kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  return date.substr(2, 2) + kMon[std::stoi(date.substr(5, 2)) - 1] + date.substr(8, 2);
}

Px px(double d) { return static_cast<Px>(std::llround(d * kOneDollar)); }

// one event: its ticker and its three brackets, low strike first
struct Ev {
  std::string id, a, b, c;
  std::vector<std::string> tks() const { return {a, b, c}; }
};

// a market's street book: YES bids and YES asks, each (dollars, contracts); what is not named leaves
void book(fakex::Engine& e, const std::string& tk, std::vector<std::pair<double, int>> bids, std::vector<std::pair<double, int>> asks,
          std::int64_t now) {
  std::vector<std::pair<Px, Qty>> yes, no;
  for (const auto& [p, q] : bids) yes.push_back({px(p), q * kOneContract});
  for (const auto& [p, q] : asks) no.push_back({kOneDollar - px(p), q * kOneContract});   // a YES ask is a NO bid
  e.street_snapshot(tk, yes, no, now);
}

// no lock: the asks sum to $1.20
void quiet(fakex::Engine& e, const Ev& v, std::int64_t now) {
  book(e, v.a, {{.26, 30}}, {{.40, 30}}, now);
  book(e, v.b, {{.28, 30}}, {{.40, 30}}, now);
  book(e, v.c, {{.30, 30}}, {{.40, 30}}, now);
}

// a lock: a cover of 10 at 0.28 + 0.30 + 0.32 (A's 10 the scarcest), about 53c after fees; B's next
// level after its touch as given; spreads of 3c or less, so an unwind may sell into the bids
void open(fakex::Engine& e, const Ev& v, std::vector<std::pair<double, int>> b_behind, std::int64_t now) {
  std::vector<std::pair<double, int>> b_asks = {{.30, 30}};
  b_asks.insert(b_asks.end(), b_behind.begin(), b_behind.end());
  book(e, v.a, {{.26, 30}}, {{.28, 10}, {.29, 20}}, now);
  book(e, v.b, {{.28, 30}}, b_asks, now);
  book(e, v.c, {{.30, 30}}, {{.32, 30}}, now);
}

void s1() {
  const std::string day = kalshi_date(et_date(now_ms()));   // KXHIGH settles by the ticker's day: today's
  std::vector<Ev> evs;
  for (const char* x : {"A", "B", "C", "D", "E", "F", "G"}) {
    const std::string id = "KXHIGHTST-" + day + x;
    evs.push_back({id, id + "-T79", id + "-B79.5", id + "-T80"});
  }
  const Ev &E1 = evs[0], &E2 = evs[1], &E3 = evs[2], &E4 = evs[3], &E5 = evs[4], &E6 = evs[5], &E7 = evs[6];
  Run r(false, [&](fakex::Engine& e, std::int64_t now) {
    for (const auto& v : evs) {
      fakex::EventSpec es;
      es.event_ticker = v.id;
      es.title = "Highest temperature in Testville";
      es.mutually_exclusive = true;
      e.add_event(es);
      for (const auto& tk : v.tks()) {
        auto spec = fx::spec(tk, 0);
        spec.close_ms = (now / 1000 + 3600) * 1000;
        if (tk == v.a) { spec.strike_type = "less"; spec.cap_strike = 79; }
        if (tk == v.b) { spec.strike_type = "between"; spec.floor_strike = 79; spec.cap_strike = 80; }
        if (tk == v.c) { spec.strike_type = "greater"; spec.floor_strike = 80; }
        e.add_market(spec, now);
      }
      quiet(e, v, now);
    }
    open(e, E1, {}, now);
  }, false, [&](DeskdConfig& c, RuntimeOptions& o) {
    c.paper_lane = false;   // D1 would pick these markets
    c.arb_mode = "scan";
    c.arb_series = {"KXHIGHTST"};
    o.espn_url = "";
    o.ncaa_url = "";
  });
  const std::string db = r.dir.path + "/ops.sqlite";
  auto arb_status = [&] { return r.d->status()["arb"]; };
  auto engine = [&] { return arb_status()["engine"]; };
  auto with = [&](const std::function<void(fakex::Engine&, std::int64_t)>& f) {
    r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t now) { f(e, now); return 0; });
  };
  // the venue's orders in a market (every one is S1's here), and S1's position there
  auto orders = [&](const std::string& tk) {
    return r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) {
      std::vector<fakex::Order> out;
      for (const fakex::Order* o : e.user_orders())
        if (o->ticker == tk) out.push_back(*o);
      return out;
    });
  };
  auto pos = [&](const std::string& tk) {
    return r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) {
      const auto it = e.positions().find({0, tk});
      return it == e.positions().end() ? Qty{0} : it->second.pos;
    });
  };
  auto holds = [&](const Ev& v, Qty q) { return pos(v.a) == q && pos(v.b) == q && pos(v.c) == q; };
  // the event's basket, as ops.sqlite's baskets keeps its last row
  auto basket = [&](const Ev& v, const std::string& state, const std::string& why) {
    const std::string q = "SELECT count(*) FROM baskets WHERE event='" + v.id + "' AND state='" + state +
                          "' AND json_extract(json,'$.why')='" + why + "'";
    return sql_count(db, q.c_str()) == 1;
  };
  // the engine has published its n-th finished basket: s1.json was saved before it (the store may
  // write a basket's row first)
  auto done = [&](int n) { return r.until([&] { return engine()["done"] == n; }, 10000); };
  auto lane_on = [&](bool on) { return r.post("/api/lane/S1", {{"on", on}}); };
  auto start = [&] {
    std::string why;
    CHECK(r.until([&] { return r.d->gw().groups_ready(&why) && why.empty(); }, 20000));
    httplib::Result st;
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    CHECK(r.until([&] { return r.d->gw().sys_view()->run == RunState::RUN; }));
  };

  // ---- scan, Systematic orders off: E1's lock logged, never fired, nothing sent
  CHECK(r.until([&] { return r.d->status()["rest"]["ready"] == true; }, 15000));
  {
    const auto a = arb_status();
    CHECK(a["mode"] == "scan" && a["why"] == "" && a["series"] == json::array({"KXHIGHTST"}) && a["max_qty"] == 10 &&
          a["max_legs"] == 6 && a["thr"] == "0.0100" && a["legging_pause"] == "5.0000" && a["event_risk"] == false &&
          a["fee_precision"] == "0.0100" && a["lattice"] == json({{"KXHIGH", 1.0}, {"KXLOW", 1.0}}));
  }
  const std::string scanned = "SELECT count(*) FROM arb_scans WHERE event='" + E1.id +
                              "' AND kind='YES_COVER' AND mode='scan' AND qty=10 AND fired=0 AND why='scan mode'";
  const bool logged = r.until([&] { return sql_count(db, scanned.c_str()) >= 1; }, 30000);
  if (!logged) std::fprintf(stderr, "scan: %s\nrest %s\n", engine().dump().c_str(), r.d->status()["rest"].dump().c_str());
  CHECK(logged);
  CHECK(engine()["events"][E2.id]["kind"].is_null() && engine()["events"][E2.id]["why"] != "");   // no lock in E2
  CHECK(r.d->status()["rest"]["events"] == 7);   // every scanned event's row
  CHECK(r.venue.orders() == 0);
  // ---- scan with Systematic orders on: S1 stays off, gated by the mode
  r.c.systematic_orders = true;
  r.restart();
  CHECK(r.until([&] { return r.d->status()["rest"]["ready"] == true; }, 15000));
  {
    auto l = lane_on(true);
    const json err = l && l->status == 400 ? json::parse(l->body)["err"] : json::object();
    CHECK(err.value("code", "") == "BAD_REQUEST" &&
          err.value("msg", "") == "lane S1 is gated: deskd.arb_mode is scan: S1 only logs the baskets it finds (arb_scans)");
  }
  CHECK(r.venue.orders() == 0);

  // ---- trade: S1 on, Start, and E1's cover bought, the scarcest leg first
  r.c.arb_mode = "trade";
  r.c.event_risk = true;
  r.restart();
  CHECK(r.ready());
  CHECK(arb_status()["mode"] == "trade" && arb_status()["event_risk"] == true);
  {
    auto l = lane_on(true);
    CHECK(l && l->status == 200);
    if (l && l->status == 200) {
      const auto row = json::parse(l->body)["lane"];
      CHECK(row["id"] == "S1" && row["on"] == true && row["budget"] == "15.0000" && row["thr"] == "0.0100");
    }
  }
  start();
  const bool bought = r.until([&] { return holds(E1, 10 * kOneContract); }, 30000);
  if (!bought) std::fprintf(stderr, "E1: %s\n", engine().dump().c_str());
  CHECK(bought);
  CHECK(r.until([&] { return basket(E1, "DONE", "complete: every leg holds 10"); }, 10000));
  CHECK(done(1));
  {
    const auto a = orders(E1.a), b = orders(E1.b), c = orders(E1.c);
    CHECK(a.size() == 1 && b.size() == 1 && c.size() == 1);
    if (a.size() == 1 && b.size() == 1 && c.size() == 1) {
      CHECK(a[0].seq < b[0].seq && a[0].seq < c[0].seq);   // the scarcest leg alone, first
      for (const auto* o : {&a[0], &b[0], &c[0]})
        CHECK(o->client_order_id.rfind("SS1-", 0) == 0 && o->side == fakex::Side::BID && o->tif == fakex::Tif::IOC &&
              o->initial == 10 * kOneContract && o->filled == 10 * kOneContract && !o->order_group_id.empty());
      CHECK(a[0].price == px(.28) && b[0].price == px(.30) && c[0].price == px(.32));
      const bool triggered = r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) {
        const auto* g = e.group(a[0].order_group_id);
        return !g || g->triggered;
      });
      CHECK(!triggered);
    }
  }
  CHECK(r.until([&] { return sql_count(db, ("SELECT count(*) FROM fills WHERE lane='S1' AND tk LIKE '" + E1.id + "-%'").c_str()) == 3; }));
  {   // a complete basket counted by its states: S1 uses nothing
    auto l = lane_on(true);
    CHECK(l && l->status == 200 && json::parse(l->body)["lane"]["used"] == "0.0000");
  }
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E1, now); });

  // ---- a missed leg completed: B's touch pulled, 0.31 a tick behind it
  {
    fakex::HttpFault f;
    f.kind = fakex::HttpFault::Kind::PULL_TOUCH;
    f.ticker = E2.b;
    r.venue.f->add_fault(f);
  }
  with([&](fakex::Engine& e, std::int64_t now) { open(e, E2, {{.31, 30}}, now); });
  const bool completed = r.until([&] { return basket(E2, "DONE", "complete: every leg holds 10"); }, 30000);
  if (!completed) std::fprintf(stderr, "E2: %s\n", engine().dump().c_str());
  CHECK(completed);
  CHECK(done(2) && holds(E2, 10 * kOneContract));
  {
    const auto b = orders(E2.b);
    CHECK(b.size() == 2 && b[0].filled == 0 && b[1].price == px(.31) && b[1].filled == 10 * kOneContract &&
          b[1].client_order_id.back() == 'c');
    const auto w = r.venue.f->wire_log();
    CHECK(std::any_of(w.begin(), w.end(), [&](const std::string& l) { return l == "http pull-touch " + E2.b + " 30.00"; }));
  }
  CHECK(sql_count(db, ("SELECT count(*) FROM baskets WHERE event='" + E2.id + "' AND legging=-0.1").c_str()) == 1);
  CHECK(engine()["legging"] == "-0.1000");
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E2, now); });

  // ---- a missed leg unwound: B's touch pulled, nothing within a tick (0.33): A and C sold back
  {
    fakex::HttpFault f;
    f.kind = fakex::HttpFault::Kind::PULL_TOUCH;
    f.ticker = E3.b;
    r.venue.f->add_fault(f);
  }
  with([&](fakex::Engine& e, std::int64_t now) { open(e, E3, {{.33, 30}}, now); });
  const bool unwound = r.until([&] { return basket(E3, "DONE", "unwound: every leg holds 0"); }, 30000);
  if (!unwound) std::fprintf(stderr, "E3: %s\n", engine().dump().c_str());
  CHECK(unwound);
  CHECK(done(3) && holds(E3, 0));
  {
    // B filled nothing (a completion goes only if md's book has not yet shown the pull); A and C
    // each sold once
    const auto a = orders(E3.a), b = orders(E3.b), c = orders(E3.c);
    CHECK(a.size() == 2 && c.size() == 2 && !b.empty() && b.size() <= 2 &&
          std::all_of(b.begin(), b.end(), [](const fakex::Order& o) { return o.filled == 0; }));
    if (a.size() == 2 && c.size() == 2)
      CHECK(a[1].side == fakex::Side::ASK && a[1].price == px(.25) && c[1].side == fakex::Side::ASK && c[1].price == px(.29) &&
            a[1].client_order_id.find("l0u1") != std::string::npos && c[1].client_order_id.find("l2u1") != std::string::npos);
  }
  {   // legging: E2's tick, and E3's round trips (A 0.28 to 0.26, C 0.32 to 0.30) with their fees
    const double legging = std::stod(engine()["legging"].get<std::string>());
    CHECK(legging < -0.5 && legging > -2);
    std::ifstream in(r.dir.path + "/s1.json");
    const auto s1 = json::parse(in, nullptr, false);
    CHECK(!s1.is_discarded() && s1["baskets"].empty() && std::fabs(s1["legging"].get<double>() / 1e6 - legging) < 0.0001);
  }
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E3, now); });

  // ---- an UNKNOWN leg: B's create taken and never answered
  {
    fakex::HttpFault f;
    f.kind = fakex::HttpFault::Kind::DROP_AFTER_ACCEPT;
    f.ticker = E4.b;
    r.venue.f->add_fault(f);
  }
  with([&](fakex::Engine& e, std::int64_t now) { open(e, E4, {}, now); });
  const bool settled = r.until([&] { return basket(E4, "DONE", "complete: every leg holds 10"); }, 30000);
  if (!settled) std::fprintf(stderr, "E4: %s\n", engine().dump().c_str());
  CHECK(settled);
  CHECK(done(4) && holds(E4, 10 * kOneContract) && orders(E4.b).size() == 1);   // one order on B, the uncertain one
  CHECK(sql_count(db, ("SELECT count(*) FROM baskets WHERE event='" + E4.id + "' AND legging=0").c_str()) == 1);
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E4, now); });

  // ---- KILL mid-basket: B's create stalled at the venue for a second (inside send-k2's 1.5 s for
  // an answer: a longer stall makes B uncertain until gw's two-minute rule), C behind it on send-k2
  auto stalled = [&](const Ev& v) {
    fakex::HttpFault f;
    f.kind = fakex::HttpFault::Kind::STALL_BEFORE;
    f.ticker = v.b;
    f.stall_ms = 1000;
    r.venue.f->add_fault(f);
    with([&](fakex::Engine& e, std::int64_t now) { open(e, v, {}, now); });
    // A filled, and B on the wire: the basket waits on B's answer
    const bool mid = r.until([&] {
      const auto a = orders(v.a);
      return a.size() == 1 && a[0].filled == 10 * kOneContract && r.venue.f->faults_pending() == 0;
    }, 30000);
    if (!mid) std::fprintf(stderr, "%s: %s\n", v.id.c_str(), engine().dump().c_str());
    return mid;
  };
  CHECK(stalled(E5));
  auto k = r.post("/api/kill", json::object());
  CHECK(k && k->status == 200);
  const std::string group = orders(E5.a).empty() ? "" : orders(E5.a)[0].order_group_id;
  CHECK(r.until([&] {
    return r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) { const auto* g = e.group(group); return g && g->triggered; });
  }, 10000));
  CHECK(r.until([&] { return engine()["baskets"].size() == 1 && engine()["baskets"][0]["state"] == "FROZEN"; }, 10000));
  CHECK(orders(E5.c).empty() && orders(E5.b).size() <= 1);   // C never left
  CHECK(r.until([&] { return r.d->kill_switch().confirmed_gen() == r.d->kill_switch().generation(); }));
  CHECK(r.d->kill_switch().rearm(now_ms(), mono_ms() + 61000).empty());
  start();
  const bool unwound5 = r.until([&] { return basket(E5, "DONE", "unwound: every leg holds 0"); }, 30000);
  if (!unwound5) std::fprintf(stderr, "E5: %s\n", engine().dump().c_str());
  CHECK(unwound5);
  CHECK(done(5) && holds(E5, 0) && orders(E5.c).empty());
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E5, now); });

  // ---- Stop mid-basket: the same stall; C's create, behind B's, never leaves
  CHECK(stalled(E6));
  auto stop = r.post("/api/stop", json::object());
  CHECK(stop && stop->status == 200);
  CHECK(r.until([&] { return engine()["baskets"].size() == 1 && engine()["baskets"][0]["state"] == "FROZEN"; }, 10000));
  CHECK(orders(E6.c).empty());
  const std::size_t stopped_orders = r.venue.orders();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  CHECK(r.venue.orders() == stopped_orders);   // nothing goes while Systematic is stopped
  start();
  const bool unwound6 = r.until([&] { return basket(E6, "DONE", "unwound: every leg holds 0"); }, 30000);
  if (!unwound6) std::fprintf(stderr, "E6: %s\n", engine().dump().c_str());
  CHECK(unwound6);
  CHECK(done(6) && holds(E6, 0) && orders(E6.c).empty());
  with([&](fakex::Engine& e, std::int64_t now) { quiet(e, E6, now); });

  // ---- the legging pause: legging lost more than a $1 pause allows, across a restart
  const json lost = engine()["legging"];
  CHECK(std::stod(lost.get<std::string>()) <= -1.0);
  r.c.arb_legging_pause = 1 * 10000;
  r.restart();
  CHECK(r.ready());
  CHECK(r.until([&] { return engine()["paused"] == true; }) && engine()["legging"] == lost);
  auto l7 = lane_on(true);
  CHECK(l7 && l7->status == 200);
  start();
  with([&](fakex::Engine& e, std::int64_t now) { open(e, E7, {}, now); });
  auto why7 = [&] { return engine()["events"][E7.id].value("why", std::string()); };
  const bool held = r.until([&] { return why7().rfind("paused: legging", 0) == 0; }, 30000);
  if (!held) std::fprintf(stderr, "E7: %s\n", engine().dump().c_str());
  CHECK(held);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  CHECK(orders(E7.a).empty() && engine()["paused"] == true);
  // S1 turned off and on while paused (this engine saw it on): new baskets may fire
  l7 = lane_on(false);
  CHECK(l7 && l7->status == 200);
  CHECK(r.until([&] { return why7() == "lane S1 is off"; }, 10000));
  l7 = lane_on(true);
  CHECK(l7 && l7->status == 200);
  const bool reset = r.until([&] { return basket(E7, "DONE", "complete: every leg holds 10"); }, 30000);
  if (!reset) std::fprintf(stderr, "E7: %s\n", engine().dump().c_str());
  CHECK(reset);
  CHECK(done(1));   // this run's first
  CHECK(engine()["paused"] == false && engine()["legging"] == "0.0000" && holds(E7, 10 * kOneContract));
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  s1();
  return finish("deskd-runtime-arb");
}
