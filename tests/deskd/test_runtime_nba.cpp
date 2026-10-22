// X1 ESPN EXP (plan 5(e)) and N1 NBA MODEL (decisions 2) in the whole process, against the fake
// Kalshi over TLS with a local stand-in for ESPN, on runtime_fixture.h's harness:
//   - deskd.nba_lanes on with no gate files: neither lane has an engine, LaneSet is refused
//     BAD_REQUEST naming each file, no NBA scoreboard is read hot and nothing is sent; a FAIL gate
//     gives its tool's why, a gate measured on another model or with params the lane cannot trade
//     as measured (a direction neither follow nor fade, a thr of 0) says so
//   - X1's gate at stage shadow (fade, 12c) and N1's live (10c on the NBA model loaded): X1 stays
//     gated and only records, N1 trades; NBA's scoreboard is read hot and the game fair prices
//     NBA on the model N1's gate measured
//   - the game on, Boston 5 up with 5:00 left: exactly one SN1- IOC BUY at Boston's stale 0.50 ask
//     in N1's order group, sized to the clip, filled, and its TAKE row in ops.sqlite's signals;
//     X1's fade of ESPN's number (Lakers) is a SHADOW row and never an order
//   - Stop: the next score's take is a SHADOW row, nothing sent
//   - KILL: N1's group is triggered and gw refuses N1 KILLED; ESPN going silent pulls SCORE_OLD,
//     with no signal and nothing sent after the re-arm and Start
//   - the game cap in gw (N1's event cap, $10): an order past it is refused SYS_CEILING
// Every run ends with gw's invariant checks at zero (the harness).
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "deskd/strat/bball_model.h"
#include "deskd/strat/lane_gate.h"
#include "deskd/strat/take.h"
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

// A gate file as tools/x1_backtest.py and tools/nba_gate.py write it (strat/lane_gate.h)
json gate(const std::string& lane, const std::string& stage, json params, const std::string& model_sha = "") {
  json g = {{"lane", lane}, {"stage", stage}, {"verdict", "PASS"}, {"why", nullptr}, {"computed", "2026-10-22T10:00:00Z"},
            {"tool", "tests/deskd/test_runtime_nba.cpp"}, {"params", std::move(params)}, {"evidence", json::object()}};
  if (!model_sha.empty()) g["model_sha256"] = model_sha;
  return g;
}

void nba() {
  const std::int64_t start = now_ms() + 600000;   // ten minutes from now: the pregame price is read
  const std::string date = et_date(now_ms());     // the game's day, Eastern: what the lanes trade
  const std::string EV = "KXNBAGAME-" + kalshi_date(date) + "LALBOS", BOS = EV + "-BOS", LAL = EV + "-LAL";
  std::string why;
  const auto model = load_bball_model(fixture("nba_synthetic.json"), "nba", &why);
  CHECK(model.has_value());
  if (!model) return;
  // ESPN: the Lakers at Boston, before the start, then on in the fourth quarter with ESPN's last
  // play giving Boston 88%. It answers 503 while `down`; the summary is not served (its number
  // is the scoreboard's).
  std::atomic<bool> on{false}, down{false};
  std::atomic<int> bos{0}, lal{0};
  char iso[32];
  {
    const std::time_t t = start / 1000;
    std::tm b{};
    gmtime_r(&t, &b);
    std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%MZ", &b);
  }
  httplib::Server espn;
  espn.Get("/apis/site/v2/sports/basketball/nba/scoreboard", [&](const httplib::Request&, httplib::Response& res) {
    if (down) { res.status = 503; return; }
    auto side = [&](const char* ha, const char* abbr, const char* name, int score) {
      return json{{"homeAway", ha}, {"score", std::to_string(on ? score : 0)}, {"team", {{"abbreviation", abbr}, {"displayName", name}}}};
    };
    json c = {{"status", {{"period", on ? 4 : 0}, {"displayClock", on ? "5:00" : "0:00"}, {"type", {{"state", on ? "in" : "pre"}, {"shortDetail", "-"}}}}},
              {"competitors", {side("away", "LAL", "Los Angeles Lakers", lal), side("home", "BOS", "Boston Celtics", bos)}}};
    if (on) c["situation"] = {{"lastPlay", {{"id", "4019010" + std::to_string(bos + lal)}, {"probability", {{"homeWinPercentage", 0.88}, {"tiePercentage", 0.0}}}}}};
    res.set_content(json{{"events", {{{"id", "401901"}, {"date", iso}, {"competitions", {c}}}}}}.dump(), "application/json");
  });
  const int espn_port = espn.bind_to_any_port("127.0.0.1");
  std::thread espn_th([&] { espn.listen_after_bind(); });
  {
    Run r(false, [&](fakex::Engine& e, std::int64_t now) {
      for (const auto& tk : {BOS, LAL}) {
        auto spec = fx::spec(tk, 0);
        spec.close_ms = (now / 1000 + 4 * 3600) * 1000;
        e.add_market(spec, now);
      }
      // Boston 0.48 / 0.50 and the Lakers 0.50 / 0.52, 30 deep: Boston's effective book 0.48 / 0.50
      e.street_add(BOS, false, 4800, 3000, now); e.street_add(BOS, true, 5000, 3000, now);
      e.street_add(LAL, false, 5000, 3000, now); e.street_add(LAL, true, 4800, 3000, now);
    }, true, [&](DeskdConfig& c, RuntimeOptions& o) {
      c.paper_lane = false;   // D1 would pick these markets
      c.nba_lanes = true;
      c.nba_model = fixture("nba_synthetic.json");
      o.espn_url = "http://127.0.0.1:" + std::to_string(espn_port);
      o.espn_scoreboard_ms = 300;
      o.ncaa_url = "";
    });
    auto lane_on = [&](const std::string& lane) { return r.post("/api/lane/" + lane, {{"on", true}}); };
    auto refused = [&](const std::string& lane, const std::string& want) {   // LaneSet on: BAD_REQUEST, saying want
      auto l = lane_on(lane);
      const json err = l && l->status == 400 ? json::parse(l->body)["err"] : json::object();
      if (err.value("code", "") == "BAD_REQUEST" && err.value("msg", "") == want) return true;
      std::fprintf(stderr, "LaneSet %s: %d %s\nwant %s\n", lane.c_str(), l ? l->status : 0, l ? l->body.c_str() : "", want.c_str());
      return false;
    };
    auto nba = [&] { return r.d->status()["nba"]; };
    auto lane_orders = [&](const char* prefix) {   // the engine's (the test's own are SN1-rt-)
      return r.venue.f->with_engine([prefix](fakex::Engine& e, std::int64_t) {
        std::vector<fakex::Order> out;
        for (const fakex::Order* o : e.user_orders())
          if (o->client_order_id.rfind(prefix, 0) == 0 && o->client_order_id.find("-rt-") == std::string::npos) out.push_back(*o);
        return out;
      });
    };
    const std::string x1_path = r.dir.path + "/gates/X1.json", n1_path = r.dir.path + "/gates/N1.json";
    // ---- no gate files: both gated, naming them; nothing read hot, nothing sent
    CHECK(r.until([&] { return r.d->status()["rest"]["ready"] == true; }, 15000));
    {
      CHECK(refused("X1", "lane X1 is gated: no gate file " + x1_path));
      CHECK(refused("N1", "lane N1 is gated: no gate file " + n1_path));
      const auto n = nba();
      CHECK(n["enabled"] == true && n["why"] == "" && n["series"] == json::array({"KXNBAGAME"}) && n["shadow"] == true);
      CHECK(n["model"]["loaded"] == true && n["model"]["sha256"] == model->sha256);
      CHECK(n["X1"]["why"] == "no gate file " + x1_path && n["X1"]["engine"].is_null() && n["X1"]["rule"].is_null());
      CHECK(n["N1"]["why"] == "no gate file " + n1_path && n["N1"]["engine"].is_null());
      CHECK(r.d->status()["espn"]["fast_series"].empty());
    }
    // ---- a FAIL gate (its tool's why) and a gate measured on another model
    std::filesystem::create_directories(r.dir.path + "/gates");
    {
      json fail = gate("X1", "shadow", json::object());
      fail["verdict"] = "FAIL";
      fail["why"] = "both directions lost on the test dates";
      std::ofstream(x1_path) << fail.dump();
      std::ofstream(n1_path) << gate("N1", "live", {{"thr", 0.1}}, std::string(64, 'a')).dump();
    }
    r.restart();
    CHECK(refused("X1", "lane X1 is gated: " + x1_path + ": verdict FAIL: both directions lost on the test dates"));
    CHECK(refused("N1", "lane N1 is gated: " + n1_path + ": measured another model (aaaaaaaaaaaa, loaded " + model->sha256.substr(0, 12) + ")"));
    // ---- gates that load but cannot be traded as measured
    std::ofstream(x1_path) << gate("X1", "live", {{"direction", "sideways"}, {"thr", 0.1}}).dump();
    std::ofstream(n1_path) << gate("N1", "live", {{"thr", 0}}, model->sha256).dump();
    r.restart();
    CHECK(refused("X1", "lane X1 is gated: " + x1_path + ": the gate's direction \"sideways\" is neither follow nor fade"));
    CHECK(refused("N1", "lane N1 is gated: " + n1_path + ": the gate's params.thr 0 is not a price between 0 and 1 (dollars a contract)"));
    CHECK(nba()["X1"]["gate"]["stage"] == "live" && nba()["X1"]["rule"].is_null() && nba()["N1"]["engine"].is_null());
    CHECK(lane_orders("SX1-").empty() && lane_orders("SN1-").empty());
    // ---- X1 at stage shadow, fading ESPN by 12c (x1_backtest.py's synthetic gate); N1 live at 10c
    std::ofstream(x1_path) << gate("X1", "shadow", {{"direction", "fade"}, {"thr", 0.12}}).dump();
    std::ofstream(n1_path) << gate("N1", "live", {{"thr", 0.1}}, model->sha256).dump();
    r.restart();
    CHECK(r.ready());
    {
      const auto n = nba();
      CHECK(n["X1"]["rule"] == json({{"mode", "ESPN_FADE"}, {"edge", "0.1200"}}) && n["X1"]["engine"]["shadow"] == true);
      CHECK(n["X1"]["why"] == x1_path + ": stage shadow (shadow signals only until its gate is live)");
      CHECK(n["N1"]["rule"] == json({{"mode", "MODEL"}, {"edge", "0.1000"}}) && n["N1"]["why"] == "" && n["N1"]["engine"].is_object());
      CHECK(r.d->status()["espn"]["fast_series"] == json::array({"KXNBAGAME"}));
      CHECK(r.d->status()["game_fair"]["models"]["KXNBAGAME"]["sha256"] == model->sha256);
    }
    CHECK(refused("X1", "lane X1 is gated: " + x1_path + ": stage shadow (shadow signals only until its gate is live)"));
    auto l = lane_on("N1");
    CHECK(l && l->status == 200);
    if (l && l->status == 200) {
      const auto row = json::parse(l->body)["lane"];
      CHECK(row["id"] == "N1" && row["on"] == true && row["budget"] == "20.0000" && row["thr"] == "0.1000");
    }
    // N1 on and X1 shadowing: both want the game's books, the game fair reads the pregame price;
    // X1's gate is the first thing its game shows
    auto game = [&](const char* lane, const std::string& ev) { return nba()[lane]["engine"]["games"][ev]; };
    const bool pre = r.until([&] {
      return r.d->status()["game_fair"]["p0"].contains(EV) && game("N1", EV)["pull"] == "STATE" &&
             game("X1", EV)["pull"] == "NOT_TRADABLE";
    }, 30000);
    if (!pre) std::fprintf(stderr, "pregame: %s\ngame_fair %s\n", nba().dump().c_str(), r.d->status()["game_fair"].dump().c_str());
    CHECK(pre);
    const double p0 = r.d->status()["game_fair"]["p0"][EV]["p0"].get<double>();
    CHECK(r.d->status()["game_fair"]["p0"][EV]["ticker"] == BOS && std::fabs(p0 - 0.49) < 1e-9);
    std::string gwhy;
    CHECK(r.until([&] { return r.d->gw().groups_ready(&gwhy) && gwhy.empty(); }, 20000));
    httplib::Result st;
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    // the engines read gw's view: a score in play before it shows the run would be N1's in shadow
    CHECK(r.until([&] { return r.d->gw().sys_view()->run == RunState::RUN; }));
    // ---- the game is on, Boston 5 up with 5:00 left: its 0.50 ask is far under the model's fair
    bos = 100;
    lal = 95;
    on = true;
    const double fair = *bball_live_p(*model, "in", 4, 300, 5, p0);
    CHECK(fair > 0.85);
    const bool took = r.until([&] { const auto o = lane_orders("SN1-"); return o.size() == 1 && o[0].filled > 0; }, 30000);
    if (!took) std::fprintf(stderr, "no take: %s\nscores %s\n", nba().dump().c_str(), r.d->status()["scores"].dump().c_str());
    CHECK(took);
    const Qty count = clip_count(2 * 10000, 5000, 1.0, 400, true);   // $2 at 0.50: 3 contracts and their 6c fee
    CHECK(count == 300);
    std::string cid;
    if (took) {
      const fakex::Order o = lane_orders("SN1-")[0];
      cid = o.client_order_id;
      const bool grouped = r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) { return e.group(o.order_group_id) != nullptr; });
      CHECK(o.ticker == BOS && o.side == fakex::Side::BID && o.price == 5000 && o.tif == fakex::Tif::IOC && o.initial == count &&
            o.filled == count && !o.order_group_id.empty() && grouped);
    }
    const std::string db = r.dir.path + "/ops.sqlite";
    {   // N1's TAKE row, and X1's fade of ESPN (Kalshi's Lakers mid 0.51 is 39c over ESPN's 12%) in shadow
      const std::string take = "SELECT count(*) FROM signals WHERE lane='N1' AND mode='MODEL' AND action='TAKE' AND shadow=0 AND tk='" +
                               BOS + "' AND side='BUY' AND px=0.5 AND qty=3 AND cid='" + cid + "'";
      CHECK(r.until([&] { return sql_count(db, take.c_str()) == 1; }, 10000));
      const std::string fade = "SELECT count(*) FROM signals WHERE lane='X1' AND mode='ESPN_FADE' AND action='SHADOW' AND shadow=1 AND tk='" +
                               LAL + "' AND side='BUY' AND px=0.52 AND qty=3 AND edge=0.39 AND cid IS NULL";
      CHECK(r.until([&] { return sql_count(db, fade.c_str()) == 1; }, 10000));
    }
    CHECK(lane_orders("SX1-").empty());
    // ---- Stop: once the cool-down is over, the next score's take is recorded in shadow, not sent
    auto stop = r.post("/api/stop", json::object());
    CHECK(stop && stop->status == 200);
    CHECK(r.until([&] { return game("N1", EV)["pull"] == "FIRED"; }, 15000));   // N1's 10 s between takes in a game
    bos = 102;
    const bool held = r.until([&] {
      const auto g = game("N1", EV);
      return g["pull"] == "BLOCKED" && g["why"] == "Systematic is not running (recorded in shadow)";
    }, 30000);
    if (!held) std::fprintf(stderr, "stopped: %s\n", game("N1", EV).dump().c_str());
    CHECK(held);
    CHECK(r.until([&] { return sql_count(db, "SELECT count(*) FROM signals WHERE lane='N1' AND action='SHADOW' AND shadow=1") == 1; }, 10000));
    CHECK(lane_orders("SN1-").size() == 1);
    // ---- KILL: N1's group is triggered, and gw refuses N1 KILLED
    CHECK(r.until([&] { return r.d->gw().groups_ready(&gwhy) && gwhy.empty(); }, 20000));
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    const std::string group = took ? lane_orders("SN1-")[0].order_group_id : "";
    auto triggered = [&] {
      return r.venue.f->with_engine([&](fakex::Engine& e, std::int64_t) { const auto* g = e.group(group); return g && g->triggered; });
    };
    CHECK(!triggered());
    auto k = r.post("/api/kill", json::object());
    CHECK(k && k->status == 200);
    CHECK(r.until(triggered, 10000));
    auto sys = [&](const std::string& c, const std::string& tk, Px px, Qty n) {
      PlaceIntent p;
      p.cid = c; p.owner = Owner::SYS; p.lane = "N1"; p.ticker = tk; p.side = Side::BUY; p.price = px; p.count = n;
      p.tif = Tif::IOC; p.ttl_s = 0; p.reply = std::make_shared<std::promise<OrderReply>>();
      auto f = p.reply->get_future();
      CHECK(r.d->gw().post(std::move(p)));
      if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return OrderReply{"TIMEOUT", "", ""};
      return f.get();
    };
    CHECK(sys("SN1-rt-kill", LAL, 5200, 100).code == "KILLED");
    // ESPN goes silent: the score grows old, and no signal comes of it
    down = true;
    const bool old = r.until([&] {
      const auto g = game("N1", EV);
      return g["pull"] == "SCORE_OLD" && g["why"].get<std::string>().find(" old (at most 5000 ms)") != std::string::npos;
    }, 30000);
    if (!old) std::fprintf(stderr, "ESPN down: %s\n", nba().dump().c_str());
    CHECK(old);
    const long long signals = sql_count(db, "SELECT count(*) FROM signals");
    CHECK(r.until([&] { return r.d->kill_switch().confirmed_gen() == r.d->kill_switch().generation(); }));
    CHECK(r.d->kill_switch().rearm(now_ms(), mono_ms() + 61000).empty());
    CHECK(r.until([&] { return r.d->gw().groups_ready(&gwhy) && gwhy.empty(); }, 20000));
    CHECK(r.until([&] { st = r.post("/api/start", json::object()); return st && st->status == 200; }, 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(game("N1", EV)["pull"] == "SCORE_OLD" && lane_orders("SN1-").size() == 1 && lane_orders("SX1-").empty());
    CHECK(sql_count(db, "SELECT count(*) FROM signals") == signals);
    // ---- the game cap in gw: N1's worst case in one game at most $10 (its event cap), as
    // SysView::worst measures it
    CHECK(sys("SN1-rt-lal", LAL, 5200, 1000).code.empty());   // 10 at 0.52
    auto pos = [&](const std::string& tk) {
      const auto v = r.d->gw().sys_view();
      const auto it = v->positions.find("N1|" + tk);
      return it == v->positions.end() ? Qty{0} : it->second;
    };
    CHECK(r.until([&] { return pos(BOS) == 300 && pos(LAL) == 1000; }, 10000));
    std::int64_t used = 0;
    {
      const auto v = r.d->gw().sys_view();
      for (const auto& tk : {BOS, LAL}) used += v->worst.count("N1|" + tk) ? v->worst.at("N1|" + tk) : 0;
    }
    CHECK(used >= 3 * 500000 + 10 * 520000 && used <= 10 * 1000000);   // the two orders' cost, and their fees
    // 7 more of Boston at 0.50 (inside its 10-contract inventory): past the game's $10, inside the
    // correlated group's $15 (checked before it)
    CHECK(used + 7 * 500000 > 10 * 1000000 && used + 7 * 500000 + 20000 < 15 * 1000000);
    const auto cap = sys("SN1-rt-cap", BOS, 5000, 700);
    CHECK(cap.code == "SYS_CEILING" && cap.msg == "lane N1's worst case in event " + EV + " past 10.0000");
    CHECK(!r.venue.has_cid("SN1-rt-cap"));
    CHECK(lane_orders("SN1-").size() == 1 && lane_orders("SX1-").empty());   // the engine's one take, all along
  }
  espn.stop();
  espn_th.join();
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  nba();
  return finish("deskd-runtime-nba");
}
