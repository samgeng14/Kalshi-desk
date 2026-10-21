// deskd's Systematic soak (plan section 8: "4 hours on the fake exchange under ThreadSanitizer
// before Start goes live"). The whole process against the fake over TLS: lane M1 quotes natural
// gas's 15-minute windows (rolled as they close) while the street's book wanders and street
// takers hit the quotes. Every second it checks the invariants a long run must keep:
//   - the order ledger and send-k2 / send-cx healthy, KILL never tripped
//   - at most one resting Systematic quote per market and side on the venue
//   - gw's lane position equals the venue's position (allowing 5 s for the private feed)
//   - gw refuses none of the maker's quotes
// and at the end, Stop leaves nothing resting. ctest runs it for 60 s; for the real soak:
//   DESKD_SOAK_SECONDS=14400 TSAN_OPTIONS=suppressions=tests/deskd/tsan.supp ./deskd-soak
// (configure with -DDESKD_TSAN=ON for TSan; tsan.supp names the uninstrumented OpenSSL reports)
#include <httplib.h>
#include <sys/stat.h>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>
#include <nlohmann/json.hpp>
#include "check.h"
#include "deskd/app/lock.h"
#include "deskd/app/log.h"
#include "deskd/app/runtime.h"
#include "deskd/build_paths.h"
#include "net_fixture.h"
using namespace deskd;
using json = nlohmann::json;
namespace {
// RFC 8032 test vector, not an account credential (as in test_sender).
const char* pem="-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n-----END PRIVATE KEY-----\n";
const std::int64_t kWindow=900000;

std::string window_ticker(int n) { return "KXNATGAS15M-SOAK" + std::to_string(1000 + n) + "-00"; }

struct Soak {
  fx::Venue venue;
  TempDir dir;
  std::unique_ptr<Deskd> d;
  std::mt19937_64 rng{20261010};
  std::vector<std::string> windows;
  std::map<std::string, Px> mid;
  std::int64_t next_open=0;
  int violations=0, takes=0;
  bool invariant_seen=false;
  std::int64_t max_resting=0;

  void add_window(fakex::Engine& e,std::int64_t now) {
    const auto tk=window_ticker(static_cast<int>(windows.size()));
    auto spec=fx::spec(tk,0);
    spec.open_ms=next_open; spec.close_ms=next_open+kWindow;
    spec.strike_type="greater_or_equal"; spec.floor_strike=3.1;
    e.add_market(spec,now);
    windows.push_back(tk);
    mid[tk]=5000;
    street(e,tk,now);
    next_open+=kWindow;
  }
  // the street's book: 5 contracts a level, two levels a side, 3c either side of its mid
  void street(fakex::Engine& e,const std::string& tk,std::int64_t now) {
    const Px m=mid[tk];
    e.street_snapshot(tk,{{m-300,500},{m-400,500}},{{10000-(m+300),500},{10000-(m+400),500}},now);
  }
  explicit Soak() {
    CHECK(venue.ok);
    next_open=now_ms()-30000;
    venue.f->with_engine([&](fakex::Engine& e,std::int64_t now) { add_window(e,now); });
    const std::string key=dir.path+"/test-key.pem";
    { std::ofstream f(key); f<<pem; }
    ::chmod(key.c_str(),0600);
    DeskdConfig c;
    c.data_dir=dir.path; c.ui_dir=std::string(DESKD_SOURCE_DIR)+"/ui/desk"; c.http_port=0;
    c.env="demo"; c.key_id="soak"; c.pem_path=key; c.key_type="ed25519";
    c.systematic_orders=true;
    c.daily_stop=100000*10000;   // a soak checks machinery, not P&L: never HALT on a random walk
    RuntimeOptions o;
    const std::string base="https://127.0.0.1:"+std::to_string(venue.port());
    o.rest_url=base+"/trade-api/v2"; o.ws_url=base+"/trade-api/ws/v2"; o.factory=fx::trust_test_ca();
    o.rest_poll_ms=700; o.rest_gap_ms=0; o.espn_url="";
    o.check_each_message=true;   // plan section 8: the ledger's bounds after every gw message
    std::string err;
    auto lock=AccountLock::acquire(dir.path,"demo","deskd-soak",false,&err);
    d=std::make_unique<Deskd>(c,std::move(lock),o);
    if(!d->start(&err)) { std::fprintf(stderr,"deskd did not start: %s\n",err.c_str()); std::exit(2); }
  }
  ~Soak() { d->stop(); }
  httplib::Result post(const std::string& path,const json& body) {
    httplib::Client c("127.0.0.1",d->port()); c.set_read_timeout(6,0);
    return c.Post(path,httplib::Headers{{"X-Desk",d->token()}},body.dump(),"application/json");
  }
  bool until(const std::function<bool()>& f,int ms) {
    const auto end=mono_ms()+ms;
    do { if(f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } while(mono_ms()<end);
    return f();
  }
  // resting Systematic quotes on the venue, by "ticker|side"
  std::map<std::string,int> resting() {
    return venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      std::map<std::string,int> out;
      for(const fakex::Order* o:e.user_orders())
        if(o->status==fakex::OrdStatus::RESTING && o->client_order_id.rfind("SM1-",0)==0)
          ++out[o->ticker+(o->side==fakex::Side::BID ? "|bid" : "|ask")];
      return out;
    });
  }
  void violation(const std::string& what) {
    ++violations;
    std::fprintf(stderr,"SOAK VIOLATION at %s: %s\n",iso_ms(now_ms()).c_str(),what.c_str());
  }
};
}  // namespace

int main() {
  std::signal(SIGPIPE,SIG_IGN);
  const char* env=std::getenv("DESKD_SOAK_SECONDS");
  const int seconds=env ? std::max(10,std::atoi(env)) : 60;
  Soak s;
  CHECK(s.until([&]{ const auto st=s.d->status(); return st["rest"]["ready"]==true && st["feeds"]["KXP"]["up"]==true; },20000));
  auto lane=s.post("/api/lane/M1",{{"on",true}});
  CHECK(lane && lane->status==200);
  std::string why;
  CHECK(s.until([&]{ return s.d->gw().groups_ready(&why) && why.empty(); },20000));
  httplib::Result start;
  CHECK(s.until([&]{ start=s.post("/api/start",json::object()); return start && start->status==200; },15000));
  std::printf("soak: %d s of LIP-15 on the fake\n",seconds);
  const auto end=mono_ms()+seconds*1000LL;
  std::int64_t last_check=0, mismatch_since=0;
  std::uniform_int_distribution<int> step(-1,1), coin(0,7);
  while(mono_ms()<end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto now=now_ms();
    s.venue.f->pyth("Commodities.Index.NATGAS/USD",3.1,now);   // the venue's relay of the reference
    s.venue.f->with_engine([&](fakex::Engine& e,std::int64_t t) {
      // the next window two minutes before it opens; the street wanders, and sometimes takes
      if(s.next_open-t<120000) s.add_window(e,t);
      for(const auto& tk:s.windows) {
        const fakex::Market* m=e.market(tk);
        if(!m || m->state==fakex::Market::State::CLOSED) continue;
        s.mid[tk]=std::clamp<Px>(s.mid[tk]+100*step(s.rng),2000,8000);
        s.street(e,tk,t);
        if(coin(s.rng)==0) {
          const bool buy=coin(s.rng)%2;
          // hit whatever rests within 2c of the street's mid: our quotes inside the street's book
          if(e.street_take(tk,buy ? fakex::Side::BID : fakex::Side::ASK,buy ? s.mid[tk]+200 : s.mid[tk]-200,100*(1+coin(s.rng)%3),t)>0) ++s.takes;
        }
      }
    });
    if(now-last_check<1000) continue;
    last_check=now;
    const auto st=s.d->status();
    if(st["gw"]["ledger_healthy"]!=true) s.violation("ledger: "+st["gw"]["ledger_why"].get<std::string>());
    if(st["gw"]["invariant_violations"]!=0 && !s.invariant_seen) {   // counted by gw; reported once here
      s.invariant_seen=true;
      s.violation("after a gw message: "+st["gw"]["invariant_why"].get<std::string>());
    }
    if(st["senders"]["k2"]["healthy"]!=true) s.violation("send-k2: "+st["senders"]["k2"]["why"].get<std::string>());
    if(st["senders"]["cx"]["healthy"]!=true) s.violation("send-cx: "+st["senders"]["cx"]["why"].get<std::string>());
    if(s.d->kill_switch().active()) s.violation("KILL tripped: "+s.d->kill_switch().why());
    std::int64_t total=0;
    for(const auto& [key,n]:s.resting()) {
      total+=n;
      if(n>1) s.violation(std::to_string(n)+" resting quotes on "+key);
    }
    s.max_resting=std::max(s.max_resting,total);
    // gw's lane position against the venue's, per window
    const auto view=s.d->gw().sys_view();
    const auto venue_pos=s.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
      std::map<std::string,Qty> out;
      for(const auto& [key,p]:e.positions()) if(key.first==0) out[key.second]=p.pos;
      return out;
    });
    bool same=true;
    for(const auto& tk:s.windows) {
      const auto g=view && view->positions.count("M1|"+tk) ? view->positions.at("M1|"+tk) : 0;
      const auto v=venue_pos.count(tk) ? venue_pos.at(tk) : 0;
      if(g!=v) same=false;
    }
    if(same) mismatch_since=0;
    else if(!mismatch_since) mismatch_since=now;
    else if(now-mismatch_since>5000) { s.violation("gw's lane position differs from the venue's for 5 s"); mismatch_since=now; }
  }
  auto stop=s.post("/api/stop",json::object());
  CHECK(stop && stop->status==200);
  const bool down=s.until([&]{ return s.resting().empty(); },10000);
  if(!down) s.violation("quotes still resting 10 s after Stop");
  const auto st=s.d->status();
  const auto fills=s.venue.f->with_engine([](fakex::Engine& e,std::int64_t) {
    int n=0; for(const auto& f:e.fills()) if(f.client_order_id.rfind("SM1-",0)==0) ++n; return n; });
  std::printf("soak: %zu windows, maker places %d cancels %d refusals %d, %d street takes, %d of our fills, "
              "at most %lld resting, gw sys_sent %d, %d violations\n",
              s.windows.size(),st["maker"]["places"].get<int>(),st["maker"]["cancels"].get<int>(),
              st["maker"]["refusals"].get<int>(),s.takes,fills,static_cast<long long>(s.max_resting),
              st["gw"]["sys_sent"].get<int>(),s.violations);
  CHECK(s.violations==0);
  // gw never refused a quote: a refusal in a steady run is a gap somewhere (fee metadata going
  // stale between polls was one, 2026-10-10)
  CHECK(st["maker"]["refusals"]==0);
  CHECK(st["maker"]["places"].get<int>()>0 && fills>0);   // it quoted, and was traded against
  return finish("deskd-soak");
}
