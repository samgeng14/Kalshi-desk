#include "deskd/app/runtime.h"

#include <sys/stat.h>
#include <unistd.h>

#include "deskd/app/log.h"
#include "deskd/strat/strat.h"
#include "deskd/web/contract.h"

namespace deskd {

using json = nlohmann::json;

Deskd::Deskd(DeskdConfig cfg, std::unique_ptr<AccountLock> lock)
    : cfg_(std::move(cfg)), lock_(std::move(lock)), guard_(cfg_.live, lock_.get()) {
  started_ms_ = now_ms();
  // changes on every start: the page resyncs, and re-reads the token, when hb.epoch changes
  epoch_ = "e-" + std::to_string(started_ms_) + "-" + std::to_string(::getpid());
  token_ = random_token();
  kill_.persist_to(cfg_.data_dir + "/deskd.killed");
  for (const char* n : {"md-k", "priv-k", "gw", "send-k1", "send-k2", "send-k3", "send-cx", "strat", "store", "pub",
                        "ops", "web"})
    workers_.push_back(std::make_unique<Worker>(n));

  ops_ = std::make_unique<Ops>(cfg_.data_dir, cfg_.heartbeat_window_ms,
                               std::vector<const Worker*>{worker("gw"), worker("md-k"), worker("priv-k")}, feeds_);
  Ops* ops = ops_.get();
  gw_ = std::make_unique<Gateway>(kill_, [ops] { return ops->preflight(); });
  Gateway* gw = gw_.get();
  const bool live = cfg_.live;
  const Money stop = cfg_.daily_stop;
  pub_ = std::make_unique<Pub>(
      epoch_, feeds_,
      [gw, ops, live, stop] {
        HdrInputs h;
        h.live = live;
        h.run = gw->run_view();
        h.preflight = ops->preflight();
        h.daily_stop = stop;
        return h;
      },
      cfg_.max_streams);

  WebDeps wd;
  wd.ui_dir = cfg_.ui_dir;
  wd.threads = cfg_.web_threads;
  wd.keepalive_s = cfg_.keepalive_s;
  wd.token = token_;
  wd.pub = pub_.get();
  wd.gw = gw_.get();
  wd.kill = &kill_;
  wd.send_cx = &q_cx_;
  wd.status = [this] { return status(); };
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
  if (!lock_ || !lock_->held()) {
    if (err) *err = "no account lock: deskd starts only while it holds " + std::string(AccountLock::kFileName);
    return false;
  }
  struct stat st {};
  if (::stat((cfg_.ui_dir + "/index.html").c_str(), &st) != 0) {
    if (err) *err = "no index.html in the ui dir " + cfg_.ui_dir;
    return false;
  }
  // KILLED before this start stays KILLED until someone re-arms; restored before the port is
  // bound, so no request ever sees the desk un-killed
  const std::int64_t killed = kill_.restore();
  std::string berr;
  if (!web_->bind(cfg_.http_port, &berr)) {
    if (err) *err = berr;
    return false;
  }
  auto by = [this](const char* n) { return const_cast<Worker*>(worker(n)); };
  by("md-k")->start([this](Worker& w) { run_feed_placeholder(w, feeds_.KX); });
  by("priv-k")->start([this](Worker& w) { run_feed_placeholder(w, feeds_.KXP); });
  by("gw")->start([this](Worker& w) { gw_->run(w); });
  const AlertFn alert = [this](const std::string& lvl, const std::string& src, const std::string& msg) {
    pub_->alert(lvl, src, msg);
  };
  by("send-k1")->start([this, alert](Worker& w) { run_sender(w, q_k1_, k1_stats_, alert); });
  by("send-k2")->start([this, alert](Worker& w) { run_sender(w, q_k2_, k2_stats_, alert); });
  by("send-k3")->start([this, alert](Worker& w) { run_sender(w, q_k3_, k3_stats_, alert); });
  by("send-cx")->start([this, alert](Worker& w) { run_sender(w, q_cx_, cx_stats_, alert); });
  by("strat")->start([](Worker& w) { run_strat(w); });
  by("store")->start([this](Worker& w) { store_.run(w); });
  by("pub")->start([this](Worker& w) { pub_->run(w); });
  by("ops")->start([this](Worker& w) { ops_->run(w); });
  by("web")->start([this](Worker&) { web_->serve(); });
  web_->wait_ready();
  started_ = true;
  if (killed) {
    // cancel-all again (orders may have been placed by hand while deskd was down) and tell gw
    q_cx_.push(SendJob{SendJob::Kind::KILL, now_ms(), ""}, Lane::PRIORITY);
    gw_->post(Kill{killed, "restart"});
    pub_->alert("CRIT", "KILL", "KILLED before this start (" + iso_ms(killed) + "): re-arm to trade");
    DLOG_WARN("KILLED since %s (before this start): re-arm to trade", iso_ms(killed).c_str());
  }
  pub_->alert("INFO", "OPS",
              std::string("deskd up: ") + (cfg_.live ? "LIVE" : "DEMO") + ", no venue connections in this stage");
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
  DLOG_INFO("stopped");
}

json Deskd::status() const {
  json workers = json::array();
  const std::int64_t now = now_ms();
  for (const auto& w : workers_) {
    workers.push_back(json{{"name", w->name()},
                           {"running", w->running()},
                           {"progress", w->progress()},
                           {"quiet_ms", w->last_beat_ms() ? now - w->last_beat_ms() : -1}});
  }
  const auto& gs = gw_->stats();
  return json{{"epoch", epoch_},
              {"pid", static_cast<int>(::getpid())},
              {"mode", cfg_.live ? "LIVE" : "DEMO"},
              {"started_ms", started_ms_},
              {"srv_ms", now},
              {"data_dir", cfg_.data_dir},
              {"ui_dir", cfg_.ui_dir},
              {"lock", lock_ ? lock_->path() : ""},
              {"run", run_json(gw_->run_view())},
              {"heartbeat",
               {{"path", ops_->heartbeat_path()},
                {"written_ms", ops_->heartbeat_written_ms()},
                {"withheld", ops_->heartbeat_withheld()}}},
              {"gw",
               {{"handled", gs.handled.load()},
                {"queued", gw_->inbox().size()},
                {"dropped", gw_->inbox().dropped()},
                {"place_refused", gs.place_refused.load()}}},
              {"send_cx", {{"kills", cx_stats_.kills.load()}, {"last_kill_ms", cx_stats_.last_kill_ms.load()}}},
              {"workers", workers}};
}

}  // namespace deskd
