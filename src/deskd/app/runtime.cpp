#include "deskd/app/runtime.h"

#include <httplib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <csignal>

#include "deskd/app/log.h"
#include "deskd/strat/strat.h"
#include "deskd/web/contract.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/rest/state.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/priv/kalshi_private.h"
#include "deskd/ref/pyth.h"
#include "deskd/strat/fair.h"
#include "deskd/strat/game_fair.h"
#include "deskd/ref/espn.h"
#include "deskd/ref/ncaa.h"
#include "deskd/ref/scores.h"
#include "deskd/strat/maker.h"
#include "deskd/strat/markouts.h"
#include "deskd/strat/take.h"
#include "deskd/gw/sys_book.h"

namespace deskd {

using json = nlohmann::json;

Deskd::Deskd(DeskdConfig cfg, std::unique_ptr<AccountLock> lock, RuntimeOptions options)
    : cfg_(std::move(cfg)), options_(std::move(options)), lock_(std::move(lock)), guard_(cfg_.live, lock_.get()) {
  started_ms_ = now_ms();
  // changes on every start: the page resyncs, and re-reads the token, when hb.epoch changes
  epoch_ = "e-" + std::to_string(started_ms_) + "-" + std::to_string(::getpid());
  token_ = random_token();
  if (lock_) kill_.persist_to(lock_->sibling(".killed"));
  for (const char* n : {"md-k", "priv-k", "ref-k", "ref-espn", "ref-ncaa", "gw", "send-k1", "send-k2", "send-k3", "send-cx", "strat",
                        "store", "pub", "rest", "ops", "web"})
    workers_.push_back(std::make_unique<Worker>(n));

  // The heartbeat's owners (app/ops.h): the gateway, both Kalshi feeds, KILL's sender, the
  // screen's publisher, and web through a loopback probe.
  auto probe = [this]() -> bool {
    const int port = web_ ? web_->port() : 0;
    if (port <= 0) return false;
    httplib::Client c("127.0.0.1", port);   // Host 127.0.0.1:<port>; no Origin, no Sec-Fetch-Site
    c.set_connection_timeout(0, 200000);
    c.set_read_timeout(0, 200000);
    c.set_write_timeout(0, 200000);
    auto r = c.Get("/api/health");
    return r && r->status == 200;
  };
  auto ops_alert = [this](const std::string& lvl, const std::string& msg) {
    if (pub_) pub_->alert(lvl, "OPS", msg);
  };
  ops_ = std::make_unique<Ops>(
      cfg_.data_dir, cfg_.heartbeat_window_ms,
      std::vector<const Worker*>{worker("gw"), worker("md-k"), worker("priv-k"), worker("send-cx"), worker("pub")}, feeds_,
      probe, ops_alert, [this] { return venue_preflight(); });
  Ops* ops = ops_.get();
  gw_ = std::make_unique<Gateway>(kill_, [ops] { return ops->preflight(); });   // gw's kick on every trip
  watch_kill(kill_, q_cx_);                                                       // send-cx's kick
  pub_ = std::make_unique<Pub>(
      epoch_, feeds_, [this] { return header(); }, cfg_.max_streams);
  pub_->set_subs_callback([this](const std::string&, const Subs&) { refresh_live(); });

  WebDeps wd;
  wd.ui_dir = cfg_.ui_dir;
  wd.threads = cfg_.web_threads;
  wd.keepalive_s = cfg_.keepalive_s;
  wd.token = token_;
  wd.peer_url = peer_url_for(cfg_);
  wd.venue = venue_name();
  wd.pub = pub_.get();
  wd.gw = gw_.get();
  wd.kill = &kill_;
  wd.cx_stats = &cx_stats_;
  wd.status = [this] { return status(); };
  wd.universe = [this](const json& q) {
    if (!rest_) return universe_json(q, now_ms());
    std::shared_ptr<const FairBoard> board = fair_ ? fair_->board() : nullptr;
    return rest::universe(*rest_->snapshot(), q, now_ms(), [board](const std::string& tk) -> std::optional<Px> {
      if (!board) return std::nullopt;
      const auto it = board->find(tk);
      if (it == board->end()) return std::nullopt;
      return it->second.fair;
    });
  };
  wd.watch = [this](const json& q) { return watch(q); };
  wd.fill = [this](const json& q) { return fill(q); };
  web_ = std::make_unique<WebServer>(std::move(wd));
}

Deskd::~Deskd() { stop(); }

const Worker* Deskd::worker(const std::string& name) const {
  for (const auto& w : workers_)
    if (w->name() == name) return w.get();
  return nullptr;
}

bool Deskd::start(std::string* err) {
  if (started_) return true;
  std::signal(SIGPIPE, SIG_IGN);   // process-wide and idempotent: see runtime.h
  start_logger();
  if (!lock_ || !lock_->held()) {
    if (err) *err = "no account lock: deskd starts only while it holds the account's lock file (app/lock.h)";
    return false;
  }
  struct stat st {};
  if (::stat((cfg_.ui_dir + "/index.html").c_str(), &st) != 0) {
    if (err) *err = "no index.html in the ui dir " + cfg_.ui_dir;
    return false;
  }
  if (!init_venue(err)) return false;
  // KILLED before this start stays KILLED until someone re-arms; restored before the port is
  // bound, so no request ever sees the desk un-killed. send-cx and gw see the restored
  // generation on their first turn: send-cx cancels again (orders may have been placed by hand
  // while deskd was down) and gw starts STOP.
  const std::int64_t killed = kill_.restore(now_ms(), mono_ms());
  std::string berr;
  if (!web_->bind(cfg_.http_port, &berr)) {
    if (err) *err = berr;
    return false;
  }
  auto by = [this](const char* n) { return const_cast<Worker*>(worker(n)); };
  by("md-k")->start([this](Worker& w) {
    if (md_) md_->run(w); else run_feed_placeholder(w, feeds_.KX);
  });
  by("rest")->start([this](Worker& w) {
    if (rest_) rest_->run(w); else while (!w.stopping()) {
      w.beat(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  by("priv-k")->start([this](Worker& w) {
    if (private_) private_->run(w); else run_feed_placeholder(w, feeds_.KXP);
  });
  by("ref-k")->start([this](Worker& w) {
    if (pyth_) pyth_->run(w); else run_feed_placeholder(w, feeds_.PYTH);
  });
  by("ref-espn")->start([this](Worker& w) {
    if (espn_) espn_->run(w); else while (!w.stopping()) {
      w.beat(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  by("ref-ncaa")->start([this](Worker& w) {
    if (ncaa_) ncaa_->run(w); else while (!w.stopping()) {
      w.beat(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  by("gw")->start([this](Worker& w) { gw_->run(w); });
  const AlertFn alert = [this](const std::string& lvl, const std::string& src, const std::string& msg) {
    pub_->alert(lvl, src, msg);
  };
  if (pool_k1_) { pool_k1_->start(); pool_cx_->start(); }
  if (pool_k2_ && cfg_.systematic_orders) pool_k2_->start();
  by("send-k1")->start([this, alert](Worker& w) {
    if (man_sender_) man_sender_->run(w); else run_sender(w, q_k1_, k1_stats_, alert);
  });
  by("send-k2")->start([this, alert](Worker& w) {
    if (sys_sender_ && cfg_.systematic_orders) sys_sender_->run(w); else run_sender(w, q_k2_, k2_stats_, alert);
  });
  by("send-k3")->start([this, alert](Worker& w) { run_sender(w, q_k3_, k3_stats_, alert); });
  by("send-cx")->start([this, alert](Worker& w) {
    if (cx_sender_) cx_sender_->run(w); else run_cancel_sender(w, q_cx_, cx_stats_, kill_, alert);
  });
  // The strat thread's engines, each at its own cadence and in this order when due together: the
  // fair before the makers that read it; the merged scores, then the game fair, then C1's, X1's,
  // N1's and N2's takes, which use a fair only when it was priced on the score they read. An
  // engine with work in flight registers with paced().
  StratSteps steps;
  steps.every(250, [this] { if (fair_ && rest_) fair_->step(*rest_->snapshot(), now_ms()); });
  steps.every(250, [this] { if (scores_) scores_->step(now_ms()); });
  steps.every(250, [this] { if (game_fair_) game_fair_->step(now_ms()); });
  steps.every(250, [this] { if (c1_) c1_->step(now_ms()); });
  steps.every(250, [this] { if (x1_.engine) x1_.engine->step(now_ms()); });
  steps.every(250, [this] { if (n1_.engine) n1_.engine->step(now_ms()); });
  steps.every(250, [this] { if (n2_.engine) n2_.engine->step(now_ms()); });
  steps.every(250, [this] { if (lip_) lip_->step(now_ms()); });
  steps.every(250, [this] { if (band_) band_->step(now_ms()); });
  steps.every(250, [this] { if (paper_) paper_->step(now_ms()); });
  steps.every(250, [this] { if (probe_) probe_->step(now_ms()); });
  steps.every(250, [this] { lip_step(now_ms()); });
  steps.every(250, [this] {
    // every row to the store; the contract's horizons (1s, 10s, 60s) to the stream; a take lane's
    // own fills at its retire rule's horizon to gw (LaneMarkout, through its engine: C1's 300 s)
    if (markouts_)
      for (const auto& row : markouts_->step(now_ms())) {
        store_.inbox().push(StoreRecord{"markouts", row.store().dump()});
        if (row.on_stream()) pub_->post(PubData{"mo", row.pub()});
        if (c1_) c1_->markout(row);
        if (x1_.engine) x1_.engine->markout(row);
        if (n1_.engine) n1_.engine->markout(row);
        if (n2_.engine) n2_.engine->markout(row);
      }
  });
  by("strat")->start([steps](Worker& w) mutable { run_strat(w, [&steps] { return steps.step(mono_ms()); }); });
  if (!cfg_.data_dir.empty()) store_.set_path(cfg_.data_dir + "/ops.sqlite");
  store_.set_alert([this](const std::string& lvl, const std::string& msg) { if (pub_) pub_->alert(lvl, "STORE", msg); });
  by("store")->start([this](Worker& w) { store_.run(w); });
  by("pub")->start([this](Worker& w) { pub_->run(w); });
  by("ops")->start([this](Worker& w) { ops_->run(w); });
  by("web")->start([this](Worker&) { web_->serve(); });
  web_->wait_ready();
  started_ = true;
  if (killed) {
    pub_->alert("CRIT", "KILL", "KILLED before this start (" + iso_ms(killed) + "): re-arm to trade");
    DLOG_WARN("KILLED since %s (before this start): re-arm to trade", iso_ms(killed).c_str());
  }
  pub_->alert("INFO", "OPS",
              std::string("deskd up: ") + (cfg_.live ? "LIVE" : "DEMO") +
              (rest_ ? ", reconciling account and market data" : ", no demo credentials configured"));
  DLOG_INFO("up on http://127.0.0.1:%d/ (%s) ui=%s data=%s lock=%s", port(), cfg_.live ? "LIVE" : "DEMO",
            cfg_.ui_dir.c_str(), cfg_.data_dir.c_str(), lock_->path().c_str());
  return true;
}

void Deskd::stop() {
  if (!started_ || stopped_) return;
  stopped_ = true;
  web_->stop();   // streams end, the listener closes, the handlers drain
  for (auto& w : workers_) w->request_stop();
  for (auto& w : workers_) w->join();
  if (pool_k1_) { pool_k1_->stop(); pool_cx_->stop(); }
  if (pool_k2_ && cfg_.systematic_orders) pool_k2_->stop();
  DLOG_INFO("stopped");
  flush_logger(500);
}

namespace {
// gw's write buckets as last spent from (gw/write_budget.h): the size, and each shard's tokens
json write_budget_json(const Gateway& gw) {
  json tokens = json::object();
  if (const auto view = gw.write_tokens())
    for (const auto& [shard, t] : *view) tokens[std::to_string(shard)] = std::round(t * 10) / 10;
  return {{"cap", gw.write_cap()}, {"sys_share", WriteBudget::kSysShare}, {"tokens", tokens}};
}
// today's LIP estimate per lane, as money strings
json lip_lanes_json(const std::map<std::string, Money>& lanes) {
  json out = json::object();
  for (const auto& [lane, est] : lanes) out[lane.empty() ? "?" : lane] = money_str(est);
  return out;
}
}  // namespace

json Deskd::status() const {
  json workers = json::array();
  const std::int64_t now = now_ms(), mono = mono_ms();
  for (const auto& w : workers_) {
    workers.push_back(json{{"name", w->name()},
                           {"running", w->running()},
                           {"progress", w->progress()},
                           {"quiet_ms", w->last_beat_ms() ? mono - w->last_beat_ms() : -1}});
  }
  const auto& gs = gw_->stats();
  json result{{"epoch", epoch_},
              {"pid", static_cast<int>(::getpid())},
              {"mode", cfg_.live ? "LIVE" : "DEMO"},
              {"started_ms", started_ms_},
              {"srv_ms", now},
              {"data_dir", cfg_.data_dir},
              {"ui_dir", cfg_.ui_dir},
              {"lock", lock_ ? lock_->path() : ""},
              {"account", lock_ ? lock_->env() : ""},
              {"run", run_json(gw_->run_view())},
              {"heartbeat",
               {{"path", ops_->heartbeat_path()},
                {"written_ms", ops_->heartbeat_written_ms()},
                {"withheld", ops_->heartbeat_withheld()},
                {"web_probes_ok", ops_->web_probes_ok()},
                {"web_probes_failed", ops_->web_probes_failed()}}},
              {"gw",
               {{"handled", gs.handled.load()},
                {"queued", gw_->inbox().size()},
                {"dropped", gw_->inbox().dropped()},
                {"place_refused", gs.place_refused.load()},
                {"manual_sent", gs.manual_sent.load()},
                {"manual_cancels", gs.manual_cancels.load()},
                {"private_applied", gs.private_applied.load()},
                {"manual_orders", cfg_.manual_orders && man_sender_ != nullptr},
                {"sys_sent", gs.sys_sent.load()},
                {"sys_refused", gs.sys_refused.load()},
                {"sys_cancels", gs.sys_cancels.load()},
                {"group_jobs", gs.group_jobs.load()},
                {"sys_rate_refused", gs.sys_rate_refused.load()},
                {"records_sent", gs.records_sent.load()},
                {"records_lost", gs.records_lost.load()},
                {"venue_429", gs.venue_429.load()},
                {"write_budget", write_budget_json(*gw_)},
                {"systematic_orders", cfg_.systematic_orders && sys_sender_ != nullptr},
                {"ledger_healthy", gw_->ledger_why().empty()},
                {"ledger_why", gw_->ledger_why()},
                {"kills_seen", gs.kills_seen.load()},
                {"group_reset_fallbacks", gs.group_reset_fallbacks.load()},
                {"invariant_violations", gs.invariant_violations.load()},
                {"invariant_why", gw_->invariant_why()}}},
              {"send_cx",
               {{"kills", cx_stats_.kills.load()},
                {"kill_gen_seen", cx_stats_.kill_gen_seen.load()},
                {"last_kill_ms", cx_stats_.last_kill_ms.load()},
                {"queued", q_cx_.size()},
                {"dropped", q_cx_.dropped()}}},
              {"kill", {{"generation", kill_.generation()}, {"active", kill_.active()}}},
              {"calendar", {{"path", cfg_.calendar}, {"releases", calendar_.size()}, {"why", calendar_why_}}},
              {"lip", {{"programs", rest_ ? rest_->snapshot()->lip.size() : 0}, {"markets", lip_meter_.status()},
                       {"lanes", lip_lanes_json(lip_meter_.lane_day(now))},
                       {"note", "an estimate (strat/lip.h) until payouts are reconciled"}}},
              {"store",
               {{"path", store_.path()},
                {"received", store_.received()},
                {"written", store_.written()},
                {"failed", store_.failed()},
                {"why", store_.why()},
                {"size_bytes", store_.size_bytes()},
                {"research_copy", {{"path", store_.copy_path()}, {"ms", store_.copy_ms()}, {"why", store_.copy_why()}}}}},
              {"log", {{"dropped", log_dropped()}}},
              {"workers", workers}};
  result["feeds"] = feeds_json(feeds_);
  auto sender = [](const KalshiSender* s) {
    if (!s) return json(nullptr);
    return json{{"healthy", s->healthy()}, {"why", s->fail_reason()}, {"attempted", s->attempted()}};
  };
  result["pyth"] = pyth_ ? pyth_->status(now) : json(nullptr);
  result["fair"] = fair_ ? fair_->status() : json(nullptr);
  result["espn"] = espn_ ? espn_->status() : json(nullptr);
  result["ncaa"] = ncaa_ ? ncaa_->status() : json(nullptr);
  result["scores"] = scores_ ? scores_->status() : json(nullptr);
  result["maker"] = lip_ ? lip_->status() : json(nullptr);
  result["band"] = band_ ? band_->status() : json(nullptr);
  result["paper"] = paper_ ? paper_->status() : json(nullptr);
  result["probe"] = probe_ ? probe_->status() : json(nullptr);
  result["wcbb"] = {{"path", cfg_.wcbb_model}, {"loaded", wcbb_.has_value()}, {"why", wcbb_why_},
                    {"version", wcbb_ ? wcbb_->version : 0}};
  result["game_fair"] = game_fair_ ? game_fair_->status() : json(nullptr);
  result["college"] = college_status();
  result["nba"] = nba_status();
  result["markouts"] = markouts_ ? markouts_->status() : json(nullptr);
  if (const auto l = gw_->latency()) {
    auto ms = [](const std::optional<int>& v) { return v ? json(*v) : json(nullptr); };
    result["latency"] = {{"n", l->n}, {"click_ack_p50", ms(l->click_ack_p50)}, {"click_ack_p90", ms(l->click_ack_p90)},
                         {"recv_ack_p50", ms(l->recv_ack_p50)}, {"recv_ack_p90", ms(l->recv_ack_p90)},
                         {"wire_p50", ms(l->wire_p50)}, {"wire_p90", ms(l->wire_p90)}};
  } else {
    result["latency"] = {{"n", 0}};
  }
  result["senders"] = {{"k1", sender(man_sender_.get())}, {"cx", sender(cx_sender_.get())},
                       {"k2", sender(cfg_.systematic_orders ? sys_sender_.get() : nullptr)}};
  result["publisher_healthy"] = pub_->healthy();
  if (rest_) {
    const auto s = rest_->snapshot();
    result["rest"] = {{"ready",s->ready},{"account_ms",s->account_ms},{"listing_ms",s->listing_ms},
                       {"markets",s->markets.size()},{"revision",s->revision},{"error",s->error},
                       {"metadata_error",s->metadata_error}};
    result["rest"]["live"]=s->live.size();
    std::size_t fee_ready=0;
    for(const auto& tk:s->live) {
      const auto it=s->markets.find(tk);
      if(it!=s->markets.end() && rest::fee_ready(it->second,now)) ++fee_ready;
    }
    result["rest"]["fee_ready_live"]=fee_ready;
    std::size_t books_live=0;   // the book board gw reads: markets with a current (not stale) book
    if (const auto books = std::atomic_load(&books_))
      for (const auto& b : *books) if (!b.second.stale) ++books_live;
    result["books_live"]=books_live;
    result["clock"] = {{"offset_ms",s->clock.offset_ms ? json(*s->clock.offset_ms) : json(nullptr)},
                        {"uncertainty_ms",s->clock.uncertainty_ms ? json(*s->clock.uncertainty_ms) : json(nullptr)},
                        {"measured_ms",s->clock.measured_ms},{"post_trusted",s->clock.post_trusted}};
  }
  return result;
}

}  // namespace deskd
