#pragma once
// deskd, composed: the owners of section 2's table, each on its own named thread.
//
//   md-k, priv-k     Kalshi market data and private feed (week 0: placeholders, feeds down)
//   ref-k            Kalshi's pyth_value reference for the 15-minute financial markets
//   gw               the gateway: run state, inbox lanes and deletions
//   send-k1          Manual's sender; send-k2, send-k3 Systematic's (week 0: placeholders)
//   send-cx          cancels and KILL, on its own path
//   strat            the fairs (financial and game), the makers, the LIP meter and the markouts,
//                    each engine at its own cadence (strat/strat.h)
//   store            the only writer of ops.sqlite
//   pub              the screen's streams, hb, hdr, snapshots
//   ops              heartbeat file, disk, preflight rows
//   web              cpp-httplib, 16 request threads of its own
//
// The app owns the account lock for its whole life and will not start without it; the KILL
// file sits beside the lock (<lock dir>/kalshi-<env>.killed), so KILLED belongs to the account,
// not to whichever data dir a restart happens to use.
//
// start() also makes the process safe to embed (tests, a replay or soak harness): SIGPIPE is
// ignored process-wide (cpp-httplib 0.18.3 sends with flags 0 and never sets SO_NOSIGPIPE, so a
// client that hangs up mid-write would end any process that did not), and log lines go through
// the logger thread (app/log.h).
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "deskd/app/config.h"
#include "deskd/app/projection.h"
#include "deskd/app/lock.h"
#include "deskd/app/ops.h"
#include "deskd/app/worker.h"
#include "deskd/bus/queue.h"
#include "deskd/gw/gateway.h"
#include "deskd/gw/kill.h"
#include "deskd/md/feed.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/net/kalshi_sender.h"
#include "deskd/net/pool.h"
#include "deskd/net/guard.h"
#include "deskd/net/conn.h"
#include "deskd/net/send.h"
#include "deskd/store/store.h"
#include "deskd/strat/calendar.h"
#include "deskd/strat/fill_model.h"
#include "deskd/strat/lip.h"
#include "deskd/strat/wcbb_model.h"
#include "deskd/web/pub.h"
#include "deskd/web/server.h"

namespace deskd {
class KalshiSigner;
class KalshiMd;
class KalshiPrivate;
class KalshiPyth;
class FinancialFair;
class GameFair;
class EspnFeed;
class MakerEngine;
class Markouts;
struct MdBook;
namespace rest { class State; struct Snapshot; }

// Kalshi's production hosts: main sets them with --live (and only then; the defaults below are
// the demo account's). init_venue checks the hosts belong to the credential's account.
inline constexpr const char* kProdRestUrl = "https://api.elections.kalshi.com/trade-api/v2";
inline constexpr const char* kProdWsUrl = "https://api.elections.kalshi.com/trade-api/ws/v2";

// Endpoint injection for loopback integration tests. The same SendGuard applies to every
// connection; this cannot grant production access. Normal runs use the demo defaults.
struct RuntimeOptions {
  std::string rest_url = "https://demo-api.kalshi.co/trade-api/v2";
  std::string ws_url = "https://demo-api.kalshi.co/trade-api/ws/v2";
  net::FactoryOptions factory;
  int rest_poll_ms = 0;   // 0 = the REST owner's default; tests shorten it
  int rest_listing_ms = 0;   // the listing's poll (and the held markets' reads); 0 = the default
  int rest_gap_ms = -1;   // -1 = the REST owner's default request spacing
  // When the financial fair began watching (strat/fair.h checks only windows that opened a
  // minute after it); 0 = deskd's start. Tests set it earlier.
  std::int64_t fair_watch_since_ms = 0;
  // ESPN's public scoreboards for the watched game markets (ref/espn.h); "" = off.
  std::string espn_url = "https://site.api.espn.com";
  int espn_scoreboard_ms = 0;   // a scoreboard's pacing; 0 = the feed's default (20 s); tests shorten it
  // gw checks the ledger's bounds after every message (plan section 8); tests and the soak
  bool check_each_message = false;
};

class Deskd {
 public:
  Deskd(DeskdConfig cfg, std::unique_ptr<AccountLock> lock, RuntimeOptions options = {});
  ~Deskd();
  Deskd(const Deskd&) = delete;
  Deskd& operator=(const Deskd&) = delete;

  // binds the port (cfg.http_port; 0 = any free one) and starts every worker
  bool start(std::string* err);
  void stop();   // web first (streams end, no new requests), then every owner; idempotent

  int port() const { return web_ ? web_->port() : 0; }
  const std::string& token() const { return token_; }
  const std::string& epoch() const { return epoch_; }
  const DeskdConfig& config() const { return cfg_; }
  nlohmann::json status() const;   // GET /ops/status, deskctl status

  // the parts, for tests and status
  Gateway& gw() { return *gw_; }
  KillSwitch& kill_switch() { return kill_; }
  Pub& pub() { return *pub_; }
  Ops& ops() { return *ops_; }
  Feeds& feeds() { return feeds_; }
  const SenderStats& cx_stats() const { return cx_stats_; }
  BoundedQueue<SendJob>& cx_queue() { return q_cx_; }
  const SendGuard& send_guard() const { return guard_; }
  const Worker* worker(const std::string& name) const;

 private:
  DeskdConfig cfg_;
  RuntimeOptions options_;
  std::unique_ptr<AccountLock> lock_;
  std::string epoch_, token_;
  std::int64_t started_ms_ = 0;
  SendGuard guard_;
  std::unique_ptr<net::ConnFactory> factory_;
  std::unique_ptr<KalshiSigner> signer_;
  std::unique_ptr<rest::State> rest_;
  std::unique_ptr<KalshiMd> md_;
  std::unique_ptr<KalshiPrivate> private_;
  std::unique_ptr<KalshiPyth> pyth_;
  std::unique_ptr<FinancialFair> fair_;
  std::unique_ptr<EspnFeed> espn_;
  std::unique_ptr<MakerEngine> lip_;   // lane M1's quoter, on the strat thread
  std::unique_ptr<MakerEngine> band_;  // lane M2's
  std::unique_ptr<MakerEngine> paper_; // lane D1's (demo only)
  std::unique_ptr<MakerEngine> probe_; // lane P1's, the LIP probe
  std::unique_ptr<Markouts> markouts_;   // every fill's 1/10/60/300 s markout, gross and net of its fee
  ReleaseCalendar calendar_;             // read once at start; const after
  LipMeter lip_meter_;                   // our LIP share each second (strat thread), read by status
  void lip_step(std::int64_t now_ms);    // strat thread
  std::string calendar_why_;
  // the WCBB model (read once at start; const after), and the game markets' pregame prices and
  // fairs from it (strat thread; its board read by ref_json on md)
  std::optional<WcbbModel> wcbb_;
  std::string wcbb_why_;
  std::unique_ptr<GameFair> game_fair_;
  FillModel fill_model_;                 // each market's trade tape: how likely an order is to fill
  std::map<std::string, std::set<std::string>> strat_want_;   // under live_mu_: by lane, the markets it quotes
  std::string key_error_;
  mutable std::mutex live_mu_;
  std::set<std::string> pins_, live_;
  Feeds feeds_;
  KillSwitch kill_;
  BoundedQueue<SendJob> q_k1_{256, 64}, q_k2_{1024, 64}, q_k3_{1024, 64}, q_cx_{256, 256};
  // With a venue credential: send-k1 and send-cx run KalshiSenders on these, each on its own
  // pool of pre-opened connections; send-cx's KILL is then a real cancel-all.
  BoundedQueue<KalshiWriteJob> q_man_{256, 64}, q_mcx_{256, 256}, q_sys_{512, 128};
  std::unique_ptr<net::SparePool> pool_k1_, pool_cx_, pool_k2_;
  std::unique_ptr<KalshiSender> man_sender_, cx_sender_, sys_sender_;
  GroupRegistry groups_;   // every Systematic order group, for KILL's sender
  std::shared_ptr<const std::map<std::string, MdBook>> books_;   // std::atomic_load / std::atomic_store
  // md thread only: each market's last book, and its prints since the last book message went out
  struct Tape {
    std::vector<MdTrade> pending;
    std::optional<MdTrade> last;
    Qty since_listing = 0;          // printed after the listing's volume_24h was read
    std::int64_t listing_ms = 0;
    std::set<std::string> seen;     // trade ids, against a replay after a resubscribe
  };
  std::map<std::string, MdBook> md_last_;
  std::map<std::string, Tape> tape_;
  std::map<std::string, std::string> ref_sent_;   // md thread: each book's ref as last published
  nlohmann::json ref_json(const std::string& ticker) const;
  void republish_refs();
  PreflightRow groups_row() const;
  std::shared_ptr<const DayPnl> day_;   // std::atomic_load / std::atomic_store; rest thread writes
  std::set<std::string> fills_sent_;    // rest thread only
  std::size_t unknown_basis_said_ = 0;  // rest thread only: the warning's count as last logged
  void on_trade(const MdTrade& t);
  void publish_book(const std::string& ticker);
  std::optional<Px> mid(const std::string& ticker) const;
  SenderStats k1_stats_, k2_stats_, k3_stats_, cx_stats_;
  StoreWorker store_;
  std::vector<std::unique_ptr<Worker>> workers_;   // in start order; web last
  std::unique_ptr<Ops> ops_;
  std::unique_ptr<Gateway> gw_;
  std::unique_ptr<Pub> pub_;
  std::unique_ptr<WebServer> web_;
  bool started_ = false, stopped_ = false;
  bool init_venue(std::string* err);
  void attach_manual();
  void refresh_live();
  void on_rest(std::shared_ptr<const rest::Snapshot> state);
  void on_book(const MdBook& book);
  nlohmann::json watch(const nlohmann::json& request);
  // GET /api/fill: the fill model at the asked price and around the touch (contract 1.6)
  nlohmann::json fill(const nlohmann::json& query);
  // a market's mkt row: rest::market_json with man (Manual took it over) from gw
  nlohmann::json mkt_row(const kalshi::MarketRow& m, bool pin, bool live) const;
  PreflightRow shard_row(const rest::Snapshot& s, bool fresh) const;
  void publish_market(const std::string& ticker);
  HdrInputs header() const;
  std::vector<PreflightRow> venue_preflight() const;
  // "demo", "prod" or "sim" (the fake exchange on loopback) by the venue's REST host; "" with
  // no credential. The page's desk-venue meta: the PAPER/LIVE tag reads it.
  std::string venue_name() const;
};

}  // namespace deskd
