// C1 COLLEGE TAKE (plan 5(d)) in the whole process, against the fake Kalshi over TLS with local
// stand-ins for ESPN and NCAA.com, on runtime_fixture.h's harness:
//   - deskd.college on with no gate file: C1 has no series to trade, LaneSet is refused
//     BAD_REQUEST naming the file, and no scoreboard is read hot; a gate that opens no series
//     names each one's reason
//   - with a live gate (its params.edge, never under 10c, each series' level and leader_only, the
//     models it measured): C1 trades its series, the scoreboards of its series are read hot, and a stale ask
//     against the WCBB model's fair once a game is on is exactly one SC1- IOC at the ask in C1's
//     order group, sized to the clip, filled, and a TAKE row in ops.sqlite's signals; the run's
//     team is not taken again
//   - KILL: the next score's take is held (nothing sent) and gw refuses C1 KILLED; scores that
//     stop coming (ESPN and NCAA.com both failing) pull SCORE_OLD, and nothing is sent after the
//     re-arm either
//   - the game cap (deskd.college.game_dollars, gw's event cap): SysView::worst over the game's
//     markets is what gw measures, and an order past it is refused SYS_CEILING
//   - the loss rule: C1 down past $20 retires (lane off, LaneSet refused), and a restart keeps it
//     retired though its gate still opens it
// Every run ends with gw's invariant checks at zero (the harness).
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "deskd/strat/lane_gate.h"
#include "deskd/strat/take.h"
#include "deskd/strat/volley_model.h"
#include "deskd/strat/wcbb_model.h"
#include "deskd/web/contract.h"
#include "runtime_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
using rt::Run;
using rt::sql_count;

std::string fixture(const std::string& name) { return std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/" + name; }

// "2026-10-22" -> "26OCT22", as Kalshi dates an event
std::string kalshi_date(const std::string& date) {
  static const char* kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  return date.substr(2, 2) + kMon[std::stoi(date.substr(5, 2)) - 1] + date.substr(8, 2);
}

// A gate file as tools/college_gate.py writes it (strat/lane_gate.h)
json gate(double edge, json series) {
  return {{"lane", "C1"}, {"stage", "live"}, {"verdict", "PASS"}, {"computed", "2026-10-22T10:00:00Z"},
          {"tool", "tests/deskd/test_runtime_college.cpp"}, {"params", {{"edge", edge}, {"exec_lag_s", 2}, {"horizon", "300s"}}},
          {"series", std::move(series)}, {"evidence", json::object()}};
}

void college() {
  const std::int64_t start = now_ms() + 600000;   // ten minutes from now: the pregame price is read
  const std::string date = et_date(now_ms());     // the games' day, Eastern: what the lane trades
  const std::string EV = "KXNCAAWBGAME-" + kalshi_date(date) + "UCLAUSC", UCLA = EV + "-UCLA", USC = EV + "-USC";
  const std::string EV2 = "KXNCAAWBGAME-" + kalshi_date(date) + "BAYLSU", LSU = EV2 + "-LSU";   // no scoreboard has it
  std::string why;
  const auto wcbb = load_wcbb_model(fixture("wcbb_synthetic.json"), &why);
  const auto volley = load_volley_model(fixture("volleyball_golden.json"), &why);
  CHECK(wcbb && volley);
  if (!wcbb || !volley) return;
  // ESPN: UCLA at USC, before its start, then on; NCAA.com the same game, the same score. Both
  // answer 503 while `down`.
  std::atomic<bool> on{false}, down{false};
  std::atomic<int> ucla{0}, usc{0};
  char iso[32];
  {
    const std::time_t t = start / 1000;
    std::tm b{};
    gmtime_r(&t, &b);
    std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%MZ", &b);
  }
  httplib::Server espn;
  espn.Get("/apis/site/v2/sports/basketball/womens-college-basketball/scoreboard", [&](const httplib::Request&, httplib::Response& res) {
    if (down) { res.status = 503; return; }
    auto side = [&](const char* ha, const char* abbr, const char* name, int score) {
      return json{{"homeAway", ha}, {"score", std::to_string(on ? score : 0)}, {"team", {{"abbreviation", abbr}, {"displayName", name}}}};
    };
    json c = {{"status", {{"period", on ? 4 : 0}, {"displayClock", on ? "1:00" : "0:00"}, {"type", {{"state", on ? "in" : "pre"}, {"shortDetail", "-"}}}}},
              {"competitors", {side("away", "UCLA", "UCLA Bruins", ucla), side("home", "USC", "USC Trojans", usc)}}};
    res.set_content(json{{"events", {{{"id", "401901"}, {"date", iso}, {"competitions", {c}}}}}}.dump(), "application/json");
  });
  const int espn_port = espn.bind_to_any_port("127.0.0.1");
  std::thread espn_th([&] { espn.listen_after_bind(); });
  httplib::Server ncaa;
  const std::string ymd = date.substr(0, 4) + "/" + date.substr(5, 2) + "/" + date.substr(8, 2);
  ncaa.Get("/casablanca/scoreboard/basketball-women/d1/" + ymd + "/scoreboard.json", [&](const httplib::Request&, httplib::Response& res) {
    if (down) { res.status = 503; return; }
    auto side = [&](const char* name, int score) {
      return json{{"score", on ? std::to_string(score) : ""}, {"names", {{"char6", name}, {"short", name}}}};
    };
    const json g = {{"gameID", "6400002"}, {"gameState", on ? "live" : "pre"}, {"currentPeriod", on ? "4th" : ""},
                    {"contestClock", on ? "1:00" : ""}, {"startTimeEpoch", std::to_string(start / 1000)},
                    {"home", side("USC", usc)}, {"away", side("UCLA", ucla)}};
    res.set_content(json{{"games", json::array({json{{"game", g}}})}}.dump(), "application/json");
  });
  const int ncaa_port = ncaa.bind_to_any_port("127.0.0.1");
  std::thread ncaa_th([&] { ncaa.listen_after_bind(); });
  {
    Run r(false, [&](fakex::Engine& e, std::int64_t now) {
      for (const auto& tk : {UCLA, USC, EV2 + "-BAY", LSU}) {
        auto spec = fx::spec(tk, 0);
        spec.close_ms = (now / 1000 + 4 * 3600) * 1000;
        e.add_market(spec, now);
      }
      // UCLA 0.33 / 0.41 x5 and USC 0.58 / 0.66: UCLA's effective book 0.34 / 0.41 (USC's bid gives
      // UCLA at 0.42, so buying UCLA at its ask is the cheaper route)
      e.street_add(UCLA, false, 3300, 500, now); e.street_add(UCLA, true, 5900, 500, now);
      e.street_add(USC, false, 5800, 3000, now); e.street_add(USC, true, 3400, 3000, now);
      e.street_add(EV2 + "-BAY", false, 2800, 3000, now); e.street_add(EV2 + "-BAY", true, 6800, 3000, now);
      e.street_add(LSU, false, 6800, 3000, now); e.street_add(LSU, true, 3000, 3000, now);   // LSU 0.68 / 0.70
    }, true, [&](DeskdConfig& c, RuntimeOptions& o) {
      c.paper_lane = false;   // D1 would pick these markets
      c.wcbb_model = fixture("wcbb_synthetic.json");
      c.college.enabled = true;
      c.college.volley_model = fixture("volleyball_golden.json");
      c.college.game = 12 * 10000;   // $12 a game: two games' worth passes the $20 loss limit
      o.espn_url = "http://127.0.0.1:" + std::to_string(espn_port);
      o.espn_scoreboard_ms = 300;
      o.ncaa_url = "http://127.0.0.1:" + std::to_string(ncaa_port);
      o.ncaa_scoreboard_ms = 300;
    });
    auto lane_on = [&] { return r.post("/api/lane/C1", {{"on", true}}); };
    auto college = [&] { return r.d->status()["college"]; };
    const std::string gate_path = r.dir.path + "/gates/C1.json";
    // ---- no gate file: C1 is gated, naming it; nothing of its series is read hot
    CHECK(r.until([&] { return r.d->status()["rest"]["ready"] == true; }, 15000));
    {
      auto l = lane_on();
      CHECK(l && l->status == 400);
      if (l && l->status == 400) {
        const auto err = json::parse(l->body)["err"];
        CHECK(err["code"] == "BAD_REQUEST" && err["msg"] == "lane C1 is gated: no gate file " + gate_path);
      }
      const auto c = college();
      CHECK(c["enabled"] == true && c["why"] == "no gate file " + gate_path && c["gate"].is_null() && c["engine"].is_object());
      CHECK(c["tradable"] == json({{"KXNCAAWBGAME", "no gate file " + gate_path}, {"KXNCAAWVMATCH", "no gate file " + gate_path}}));
      CHECK(r.d->status()["espn"]["fast_series"].empty() && r.d->status()["ncaa"]["fast_series"].empty());
    }
    // ---- a gate that opens no series: each one's reason
    std::filesystem::create_directories(r.dir.path + "/gates");
    std::ofstream(gate_path) << gate(0.2, {{"KXNCAAWBGAME", {{"pass", true}, {"why", nullptr}, {"level", "quarter"},
                                                             {"leader_only", false}, {"model_sha256", wcbb->sha256}}}}).dump();
    r.restart();
    {
      auto l = lane_on();
      CHECK(l && l->status == 400);
      const std::string want = "lane C1 is gated: " + gate_path + " opens no series (KXNCAAWBGAME: the gate's level \"quarter\" "
                               "is neither point nor set; KXNCAAWVMATCH: the gate did not measure it)";
      if (l && l->status == 400) CHECK(json::parse(l->body)["err"]["msg"] == want);
      CHECK(college()["gate"]["stage"] == "live");
    }
    // ---- a live gate: WCBB at point level, volleyball at set level leader-only; its 5c edge is
    // taken at 10c, the least any gate gives C1
    std::ofstream(gate_path) << gate(0.05, {{"KXNCAAWBGAME", {{"pass", true}, {"why", ""}, {"level", "point"}, {"leader_only", false},
                                                              {"model_sha256", wcbb->sha256}}},
                                            {"KXNCAAWVMATCH", {{"pass", true}, {"why", ""}, {"level", "set"}, {"leader_only", true},
                                                               {"model_sha256", volley->sha256}}}}).dump();
    r.restart();
    CHECK(r.ready());
    {
      const auto c = college();
      CHECK(c["why"] == "" && c["tradable"] == json({{"KXNCAAWBGAME", ""}, {"KXNCAAWVMATCH", ""}}));
      CHECK(c["rules"]["KXNCAAWBGAME"] == json({{"edge", "0.1000"}, {"level", "point"}, {"leader_only", false}}));
      CHECK(c["rules"]["KXNCAAWVMATCH"] == json({{"edge", "0.1000"}, {"level", "set"}, {"leader_only", true}}));
      CHECK(c["game"] == "12.0000" && c["clip"] == "2.0000" && c["volley_model"]["loaded"] == true);
      const json fast = json::array({"KXNCAAWBGAME", "KXNCAAWVMATCH"});
      CHECK(r.d->status()["espn"]["fast_series"] == fast && r.d->status()["ncaa"]["fast_series"] == fast);
      // the game fair prices both series, each on the model the gate measured
      const auto models = r.d->status()["game_fair"]["models"];
      CHECK(models["KXNCAAWBGAME"]["sha256"] == wcbb->sha256 && models["KXNCAAWVMATCH"]["sha256"] == volley->sha256);
    }
    auto l = lane_on();
    CHECK(l && l->status == 200);
    if (l && l->status == 200) {
      const auto row = json::parse(l->body)["lane"];
      CHECK(row["id"] == "C1" && row["on"] == true && row["budget"] == "40.0000" && row["thr"] == "0.1000");
    }
    // C1 on: its engine wants both games' books, the game fair reads the pregame price, and the
    // engine dates the touches while the game is not on yet
    auto game = [&](const std::string& ev) { return college()["engine"]["games"][ev]; };
    const bool pre = r.until([&] {
      return r.d->status()["game_fair"]["p0"].contains(EV) && game(EV)["pull"] == "STATE" && game(EV2)["pull"] == "NO_SCORE";
    }, 30000);
    if (!pre) std::fprintf(stderr, "pregame: %s\ngame_fair %s\n", college()["engine"].dump().c_str(), r.d->status()["game_fair"].dump().c_str());
    CHECK(pre);
    const double p0 = r.d->status()["game_fair"]["p0"][EV]["p0"].get<double>();
    CHECK(r.d->status()["game_fair"]["p0"][EV]["ticker"] == UCLA && std::fabs(p0 - 0.375) < 1e-9);
    std::string gwhy;
    CHECK(r.until([&] { return r.d->gw().groups_ready(&gwhy) && gwhy.empty(); }, 20000));
    httplib::Result st;
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    // ---- the game is on, UCLA 20 up with a minute left: its ask is far under the model's fair
    ucla = 80;
    usc = 60;
    on = true;
    const double fair = 1 - *wcbb_live_p(*wcbb, "in", 4, 60, -20, 1 - p0);
    CHECK(fair > 0.9);
    auto c1_orders = [&] {   // the engine's (the test's own are SC1-rt-)
      return r.venue.f->with_engine([](fakex::Engine& e, std::int64_t) {
        std::vector<fakex::Order> out;
        for (const fakex::Order* o : e.user_orders())
          if (o->client_order_id.rfind("SC1-", 0) == 0 && o->client_order_id.rfind("SC1-rt-", 0) != 0) out.push_back(*o);
        return out;
      });
    };
    const bool took = r.until([&] { return c1_orders().size() == 1 && c1_orders()[0].filled > 0; }, 30000);
    if (!took) std::fprintf(stderr, "no take: %s\nscores %s\n", college()["engine"].dump().c_str(), r.d->status()["scores"].dump().c_str());
    CHECK(took);
    const Qty count = clip_count(2 * 10000, 4100, 1.0, 0, true);   // $2 at 0.41: 4 contracts and their 7c fee
    CHECK(count == 400);
    std::string cid;
    if (took) {
      const fakex::Order o = c1_orders()[0];
      cid = o.client_order_id;
      const bool grouped = r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) { return e.group(o.order_group_id) != nullptr; });
      CHECK(o.ticker == UCLA && o.side == fakex::Side::BID && o.price == 4100 && o.tif == fakex::Tif::IOC && o.initial == count &&
            o.filled == count && !o.order_group_id.empty() && grouped);
    }
    {   // its row in ops.sqlite's signals table
      const std::string db = r.dir.path + "/ops.sqlite";
      const std::string q = "SELECT count(*) FROM signals WHERE lane='C1' AND action='TAKE' AND shadow=0 AND tk='" + UCLA +
                            "' AND side='BUY' AND px=0.41 AND qty=4 AND cid='" + cid + "'";
      CHECK(r.until([&] { return sql_count(db, q.c_str()) == 1; }, 10000));
    }
    // the run took UCLA: not again in it
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(c1_orders().size() == 1 && college()["engine"]["takes"] == 1);
    // ---- KILL: the next score state's take is held, and gw refuses C1 KILLED
    auto k = r.post("/api/kill", json::object());
    CHECK(k && k->status == 200);
    ucla = 82;
    const bool held = r.until([&] {
      const auto g = game(EV);
      return g["pull"] == "BLOCKED" && g["why"] == "Systematic is not running";
    }, 30000);
    if (!held) std::fprintf(stderr, "under KILL: %s\n", game(EV).dump().c_str());
    CHECK(held && c1_orders().size() == 1);
    auto sys = [&](const std::string& c, const std::string& tk, Px px, Qty n) {
      PlaceIntent p;
      p.cid = c; p.owner = Owner::SYS; p.lane = "C1"; p.ticker = tk; p.side = Side::BUY; p.price = px; p.count = n;
      p.tif = Tif::IOC; p.ttl_s = 0; p.reply = std::make_shared<std::promise<OrderReply>>();
      auto f = p.reply->get_future();
      CHECK(r.d->gw().post(std::move(p)));
      if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return OrderReply{"TIMEOUT", "", ""};
      return f.get();
    };
    CHECK(sys("SC1-rt-kill", USC, 6600, 100).code == "KILLED");
    // the scores stop: ESPN and NCAA.com both fail, and the game's score grows old
    down = true;
    const bool old = r.until([&] {
      const auto g = game(EV);
      return g["pull"] == "SCORE_OLD" && g["why"].get<std::string>().find(" old (at most 5000 ms)") != std::string::npos;
    }, 30000);
    if (!old) std::fprintf(stderr, "scores down: %s\n", game(EV).dump().c_str());
    CHECK(old);
    CHECK(r.until([&] { return r.d->kill_switch().confirmed_gen() == r.d->kill_switch().generation(); }));
    CHECK(r.d->kill_switch().rearm(now_ms(), mono_ms() + 61000).empty());
    CHECK(r.until([&] { return r.d->gw().groups_ready(&gwhy) && gwhy.empty(); }, 20000));
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(c1_orders().size() == 1 && game(EV)["pull"] == "SCORE_OLD");
    // ---- the game cap: $12 of worst case in a game, as SysView::worst measures it
    CHECK(sys("SC1-rt-usc", USC, 6600, 1400).code.empty());   // 14 at 0.66
    CHECK(sys("SC1-rt-lsu", LSU, 7000, 1600).code.empty());   // 16 at 0.70, the other game
    auto pos = [&](const std::string& tk) {
      const auto v = r.d->gw().sys_view();
      const auto it = v->positions.find("C1|" + tk);
      return it == v->positions.end() ? Qty{0} : it->second;
    };
    CHECK(r.until([&] { return pos(UCLA) == 400 && pos(USC) == 1400 && pos(LSU) == 1600; }, 10000));
    std::int64_t used = 0;
    {
      const auto v = r.d->gw().sys_view();
      for (const auto& tk : {UCLA, USC}) used += v->worst.count("C1|" + tk) ? v->worst.at("C1|" + tk) : 0;
    }
    CHECK(used >= 4 * 410000 + 14 * 660000 && used <= 12 * 1000000);   // the two orders' cost, and their fees
    // 4 more at 0.66: past the game's $12, inside the correlated group's $15 (checked before it)
    CHECK(used + 4 * 660000 > 12 * 1000000 && used + 4 * 660000 + 10000 < 15 * 1000000);
    const auto cap = sys("SC1-rt-cap", USC, 6600, 400);
    CHECK(cap.code == "SYS_CEILING" && cap.msg == "lane C1's worst case in event " + EV + " past 12.0000");
    CHECK(!r.venue.has_cid("SC1-rt-cap"));
    // ---- the loss rule: every market C1 holds marked at 0.015, it is down over $20: retired
    r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t now) {
      for (const auto& tk : {UCLA, USC, LSU}) e.street_snapshot(tk, {{100, 3000}}, {{9800, 3000}}, now);
    });
    const bool retired = r.until([&] {
      auto x = lane_on();
      return x && x->status == 400 && x->body.find("lane C1 is retired: down $") != std::string::npos;
    }, 20000);
    if (!retired) std::fprintf(stderr, "not retired: %s\n", r.d->status()["gw"].dump().c_str());
    CHECK(retired);
    const bool off = r.until([&] { return !r.d->gw().sys_view()->lanes_on.count("C1") && game(EV)["pull"] == "OFF"; });
    if (!off) std::fprintf(stderr, "retired, still: %s\n", game(EV).dump().c_str());
    CHECK(off);
    // a restart keeps it retired, though its gate still opens it
    r.restart();
    CHECK(r.until([&] { return r.d->status()["rest"]["ready"] == true; }, 15000));
    CHECK(college()["why"] == "");
    auto x = lane_on();
    CHECK(x && x->status == 400 && x->body.find("lane C1 is retired: down $") != std::string::npos &&
          x->body.find("deskctl lane-reset C1") != std::string::npos);
    CHECK(c1_orders().size() == 1);   // the engine's one take, all along
  }
  espn.stop();
  espn_th.join();
  ncaa.stop();
  ncaa_th.join();
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  college();
  return finish("deskd-runtime-college");
}
