#include "deskd/app/runtime.h"
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <variant>
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/manual_book.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/rest/state.h"
#include "deskd/priv/kalshi_private.h"
#include "deskd/ref/pyth.h"
#include "deskd/strat/fair.h"
#include "deskd/strat/game_fair.h"
#include "deskd/ref/espn.h"
#include "deskd/ref/ncaa.h"
#include "deskd/ref/scores.h"
#include "deskd/strat/maker.h"
#include "deskd/strat/markouts.h"
#include "deskd/gw/sys_book.h"

namespace deskd {
using json = nlohmann::json;

bool Deskd::init_venue(std::string* err) {
  // A credential-less desk remains a local, read-only skeleton (including all week-0 tests).
  if (cfg_.key_id.empty() && cfg_.pem_path.empty()) return true;
  // The credential names its account and --live must agree with it: a demo key runs without
  // --live (paper), a production key only with it (live). Either way the venue's hosts must be
  // that account's; loopback (the fake exchange: tests, --sim) is the one other venue allowed.
  if (cfg_.env != "demo" && cfg_.env != "prod") {
    if (err) *err = "the config's env is \"" + cfg_.env + "\": it must be \"demo\" (paper) or \"prod\" (live)";
    return false;
  }
  if (cfg_.live != (cfg_.env == "prod")) {
    if (err) *err = cfg_.live ? "--live takes a production credential (env \"prod\"); this config's is a demo one"
                              : "a production credential (env \"prod\") runs only with --live";
    return false;
  }
  auto re = net::Endpoint::parse(options_.rest_url, err);
  auto we = net::Endpoint::parse(options_.ws_url, err);
  if (!re || !we) return false;
  for (const net::Endpoint* ep : {&*re, &*we}) {
    const HostClass cls = classify_host(ep->host);
    if (cls == HostClass::LOOPBACK) continue;
    if (cls != (cfg_.live ? HostClass::PRODUCTION : HostClass::DEMO)) {
      if (err) *err = ep->host + " is not a " + (cfg_.live ? "production" : "demo") + " Kalshi host; this credential trades the " +
                      (cfg_.live ? "production" : "demo") + " account";
      return false;
    }
  }
  signer_ = KalshiSigner::load(cfg_.key_id, cfg_.pem_path, cfg_.key_type, &key_error_);
  if (!signer_) { if (err) *err = key_error_; return false; }
  factory_ = std::make_unique<net::ConnFactory>(guard_, options_.factory);
  if (!factory_->ok(err)) return false;
  md_ = std::make_unique<KalshiMd>(*we, *factory_, *signer_, feeds_.KX,
                                  [this](const MdBook& b) { on_book(b); });
  md_->set_trade_callback([this](const MdTrade& t) { on_trade(t); });
  rest::Options ro;
  if (options_.rest_poll_ms > 0) ro.poll_ms = options_.rest_poll_ms;
  if (options_.rest_listing_ms > 0) ro.listing_poll_ms = options_.rest_listing_ms;
  if (options_.rest_gap_ms >= 0) ro.request_gap_ms = options_.rest_gap_ms;
  ro.lip_series = cfg_.lip_series;   // LIP programs for the share estimate (strat/lip.h)
  rest_ = std::make_unique<rest::State>(*factory_, *re, signer_.get(),
      [this](std::shared_ptr<const rest::Snapshot> s) { on_rest(std::move(s)); }, ro);
  // the fill model's last hour of a market's prints, read once its book first arrives
  rest_->set_trades_callback([this](const std::string& tk, std::int64_t from, std::vector<kalshi::TradeRow> rows) {
    std::vector<std::pair<std::string, TapePrint>> prints;
    prints.reserve(rows.size());
    for (const auto& r : rows) prints.push_back({r.trade_id, TapePrint{r.ts_ms, r.count, r.taker_bought_yes}});
    fill_model_.backfill(tk, from, prints);
  });
  private_ = std::make_unique<KalshiPrivate>(*we,*factory_,*signer_,feeds_.KXP,
      [this](const PrivateEvent& e) {
        // gw settles its own orders from the event; positions and cash still come from the
        // complete REST read the event asks for.
        rest_->request_refresh();
        return gw_->post(PrivateMsg{std::make_shared<const PrivateEvent>(e)});
      },[this] { rest_->request_refresh(); });
  pyth_ = std::make_unique<KalshiPyth>(*we, *factory_, *signer_, feeds_.PYTH);
  fair_ = std::make_unique<FinancialFair>(
      [this](const std::vector<std::string>& patterns) { return pyth_->symbol_for(patterns); },
      [this](const std::string& symbol, std::int64_t since, std::int64_t now) { return pyth_->view(symbol, since, now); },
      options_.fair_watch_since_ms ? options_.fair_watch_since_ms : now_ms());
  fair_->persist(cfg_.data_dir + "/replication.json");   // the 50-window gate outlives a restart
  md_->set_idle_callback([this] { republish_refs(); });
  markouts_ = std::make_unique<Markouts>();
  // the data releases that pull the makers' quotes: a bad file refuses the start (the operator
  // wrote it to be obeyed); a missing one runs without, and says so
  if (!cfg_.calendar.empty()) {
    calendar_why_ = calendar_.load(cfg_.calendar);
    if (!calendar_why_.empty()) {
      if (::access(cfg_.calendar.c_str(), F_OK) == 0) {
        if (err) *err = "calendar: " + calendar_why_;
        return false;
      }
      DLOG_WARN("no data-release calendar (%s): the makers quote through releases", calendar_why_.c_str());
    }
  }
  // the women's college basketball model: its fair on KXNCAAWBGAME ladders (docs/wcbb_model.md)
  if (!cfg_.wcbb_model.empty()) {
    wcbb_ = load_wcbb_model(cfg_.wcbb_model, &wcbb_why_);
    if (wcbb_)
      DLOG_INFO("WCBB model v%d (%s, %d games, held-out Brier %.4f vs the mid's %.4f)", wcbb_->version, wcbb_->trained.c_str(),
                wcbb_->n_games, wcbb_->brier_model, wcbb_->brier_mid);
    else if (::access(cfg_.wcbb_model.c_str(), F_OK) == 0)
      DLOG_ERR("WCBB model refused: %s (no fair on women's college ladders)", wcbb_why_.c_str());
    else
      DLOG_INFO("no WCBB model (%s): train one with tools/train_wcbb.py", wcbb_why_.c_str());
  }
  // the volleyball model (tools/volleyball.py --write; strat/volley_model.h): its fair on
  // volleyball ladders, and C1's
  if (!cfg_.college.volley_model.empty()) {
    volley_ = load_volley_model(cfg_.college.volley_model, &volley_why_);
    if (volley_)
      DLOG_INFO("volleyball model v%d (%s)", volley_->version, volley_->sha256.substr(0, 12).c_str());
    else if (::access(cfg_.college.volley_model.c_str(), F_OK) == 0)
      DLOG_ERR("volleyball model refused: %s (no fair on volleyball ladders)", volley_why_.c_str());
    else
      DLOG_INFO("no volleyball model (%s): write one with tools/volleyball.py --write", volley_why_.c_str());
  }
  // the NBA model (tools/train_bball.py --league nba; strat/bball_model.h): its fair on NBA
  // ladders, and N1's
  if (!cfg_.nba_model.empty()) {
    nba_ = load_bball_model(cfg_.nba_model, "nba", &nba_why_);
    if (nba_)
      DLOG_INFO("NBA model v%d (%s, %d games, held-out Brier %.4f vs the mid's %.4f)", nba_->version, nba_->trained.c_str(),
                nba_->n_games, nba_->brier_model, nba_->brier_mid);
    else if (::access(cfg_.nba_model.c_str(), F_OK) == 0)
      DLOG_ERR("NBA model refused: %s (no fair on NBA ladders)", nba_why_.c_str());
    else
      DLOG_INFO("no NBA model (%s): train one with tools/train_bball.py --league nba", nba_why_.c_str());
  }
  // each series' game model (strat/game_fair.h): the WCBB and NBA models' leagues, the volleyball
  // model's series; C1's gate names the model it measured on each, N1's the NBA model, and the
  // series of the lanes with an engine score hot
  std::map<std::string, GameModel> models;
  if (wcbb_) models.emplace(bball_league(wcbb_->league)->series, *wcbb_);
  if (nba_) models.emplace(bball_league(nba_->league)->series, *nba_);
  if (volley_)
    for (const auto& lg : espn_leagues())
      if (lg.sport == "volleyball") models.emplace(lg.series, *volley_);
  std::map<std::string, std::string> model_sha;
  for (const auto& [series, m] : models) model_sha[series] = std::visit([](const auto& x) { return x.sha256; }, m);
  if (college_on()) load_college(model_sha);
  if (nba_on()) load_nba();
  std::set<std::string> fast;   // the 3 s scoreboards (ESPN's load rule): only an engine's series
  for (const auto& [series, why] : c1_tradable_)
    if (why.empty()) fast.insert(series);
  const std::set<std::string> ncaa_fast = fast;   // NCAA.com has only the college series
  // X1's and N1's games in play; not N2's alone: it takes only before the start, on a score at
  // most a minute old, which the 20 s scoreboard gives
  if (x1_.rule || n1_.rule) fast.insert(cfg_.nba_series.begin(), cfg_.nba_series.end());
  // the watched game markets of the series a scoreboard covers, with the other team's market of
  // each game; each new score state a feed sees goes to ops.sqlite's scores table
  auto watched_games = [this](bool (*covers)(const std::string&)) {
    return [this, covers] {
      std::vector<kalshi::MarketRow> out;
      const auto s = rest_->snapshot();
      std::set<std::string> events;
      for (const auto& tk : s->live) {
        const auto it = s->markets.find(tk);
        if (it != s->markets.end() && covers(tk)) events.insert(it->second.event_ticker);
      }
      for (const auto& [tk, m] : s->markets) if (events.count(m.event_ticker)) out.push_back(m);
      return out;
    };
  };
  const EspnFeed::ScoreChangeFn record_score = [this](const EspnGame& g) {
    store_.inbox().push(StoreRecord{"scores", score_row(g).dump()});
  };
  if (!options_.espn_url.empty()) {
    std::string why;
    if (auto ep = net::Endpoint::parse(options_.espn_url, &why)) {
      EspnFeed::Options eo;
      if (options_.espn_scoreboard_ms > 0) eo.scoreboard_ms = options_.espn_scoreboard_ms;
      eo.fast_series = fast;
      espn_ = std::make_unique<EspnFeed>(*ep, watched_games([](const std::string& tk) { return espn_league_for(tk) != nullptr; }), eo);
      espn_->set_on_change(record_score);
    } else {
      DLOG_WARN("ESPN off: %s", why.c_str());
    }
  }
  if (!options_.ncaa_url.empty()) {
    std::string why;
    if (auto ep = net::Endpoint::parse(options_.ncaa_url, &why)) {
      NcaaFeed::Options no;
      if (options_.ncaa_scoreboard_ms > 0) no.scoreboard_ms = options_.ncaa_scoreboard_ms;
      no.fast_series = ncaa_fast;
      ncaa_ = std::make_unique<NcaaFeed>(*ep, watched_games([](const std::string& tk) { return ncaa_sport_for(tk) != nullptr; }), no);
      ncaa_->set_on_change(record_score);
    } else {
      DLOG_WARN("NCAA.com off: %s", why.c_str());
    }
  }
  // each game market's one score, ESPN's and NCAA.com's merged (ref/scores.h): what the game fair
  // and the game lanes read; a source fresh within C1's score age
  if (espn_ || ncaa_) {
    ScoreMerge::Env se;
    if (espn_) se.espn = [this] { return espn_->board(); };
    if (ncaa_) se.ncaa = [this] { return ncaa_->board(); };
    scores_ = std::make_unique<ScoreMerge>(std::move(se), cfg_.college.max_score_age_s * 1000LL);
  }
  // the game markets' pregame prices and fairs (strat/game_fair.h), on each series with a loaded
  // model: the WCBB model's KXNCAAWBGAME, the NBA model's KXNBAGAME, the volleyball model's
  // KXNCAAWVMATCH
  if (scores_ && !models.empty()) {
    GameFair::Env env;
    env.snapshot = [this] { return rest_->snapshot(); };
    env.books = [this] { return std::atomic_load(&books_); };
    env.scores = [this] { return scores_->board(); };
    game_fair_ = std::make_unique<GameFair>(std::move(env), std::move(models));
    if (!cfg_.data_dir.empty()) game_fair_->persist(cfg_.data_dir + "/game_p0.json");   // p0 outlives a restart mid-game
  }
  rest_->set_reconcile_guard([this] { return private_->fence(); },
                            [this](std::uint64_t fence) { return private_->reconcile(fence); });
  // The write path: send-k1 (Manual) and send-cx (cancels and KILL), each with its own pool.
  net::PoolConfig pc;
  pc.endpoint = *re;
  pc.name = "k1";
  pool_k1_ = std::make_unique<net::SparePool>(*factory_, pc);
  pc.name = "cx";
  pool_cx_ = std::make_unique<net::SparePool>(*factory_, pc);
  pc.name = "k2";
  pool_k2_ = std::make_unique<net::SparePool>(*factory_, pc);
  auto reply = [this](KalshiWriteReply r) {
    return gw_->post(ManualWrite{std::make_shared<const KalshiWriteReply>(std::move(r))});
  };
  auto alert = [this](const std::string& msg) { pub_->alert("CRIT", "SEND", msg); };
  man_sender_ = std::make_unique<KalshiSender>(KalshiSender::Role::MANUAL, q_man_, *pool_k1_, *signer_, kill_, reply, alert);
  cx_sender_ = std::make_unique<KalshiSender>(KalshiSender::Role::CANCEL, q_mcx_, *pool_cx_, *signer_, kill_, reply, alert);
  cx_sender_->set_stats(&cx_stats_);
  cx_sender_->set_groups(&groups_);
  // send-k2: Systematic's creates, cancels and order groups (never send-k1 or send-cx)
  sys_sender_ = std::make_unique<KalshiSender>(KalshiSender::Role::SYSTEMATIC, q_sys_, *pool_k2_, *signer_, kill_, reply, alert);
  attach_manual();
  return true;
}

namespace {
// C1's take rule in one series (plan 5(d), take_params' COLLEGE): deskd.college's amounts, gw's
// inventory limit (LaneSpec::inventory), and from its gate the edge (params.edge, dollars a
// contract, never under 10c), the level the series was measured at ("set": only near a set's end)
// and leader_only; nullopt and why when the gate's entry cannot be traded as it was measured
std::optional<TakeParams> college_rule(const LaneGate* g, const std::string& series, const DeskdConfig::College& c,
                                       const LaneSpec& lane, std::string* why) {
  TakeParams p = take_params(TakeMode::COLLEGE);
  p.clip = c.clip;
  p.game_cap = c.game * 100;   // u to micro-dollars
  p.inventory = lane.inventory;
  p.max_score_age_ms = c.max_score_age_s * 1000LL;
  if (!g) return p;
  const json& edge = g->params.at("edge");   // load_lane_gate refuses a gate without one
  if (!edge.is_number()) {
    *why = "the gate's params.edge is not a number (dollars a contract)";
    return std::nullopt;
  }
  p.edge = std::max<Money>(static_cast<Money>(std::llround(edge.get<double>() * 10000.0)), 1000);
  const auto s = g->series.find(series);
  if (s == g->series.end()) return p;
  if (s->second.level == "set") {
    p.points_required = false;
  } else if (!s->second.level.empty() && s->second.level != "point") {
    *why = "the gate's level \"" + s->second.level + "\" is neither point nor set";
    return std::nullopt;
  }
  p.leader_only = s->second.leader_only;
  return p;
}

// X1's or N1's take rule (take_params' ESPN_FOLLOW or ESPN_FADE for X1, by the direction its gate
// measured, params.direction "follow" or "fade"; MODEL for N1): gw's inventory limit and game cap
// (LaneSpec::inventory, event_cap), and the gate's thr as the edge (params.thr, dollars a
// contract). N2's is take_params' FAV_PREGAME as decisions 2 set it (5 contracts of a team priced
// in [0.90, 0.95), held to settlement) with gw's inventory limit; its gate must have measured
// exactly that (params.count in contracts, fav_lo and fav_hi in dollars), and its game cap stays
// take_params' $10, what tools/nba_gate.py n2 replayed (gw has no event cap for N2: its 5
// contracts a market bound a game). nullopt and why when the gate's params cannot be traded as
// they were measured.
std::optional<TakeParams> nba_rule(const LaneGate& g, const LaneSpec& lane, std::string* why) {
  if (lane.id == "N2") {
    TakeParams p = take_params(TakeMode::FAV_PREGAME);
    p.inventory = lane.inventory;
    // load_lane_gate refuses a gate without these
    const std::pair<const char*, std::int64_t> measured[] = {
        {"count", p.fixed_count / kOneContract}, {"fav_lo", p.fav_lo}, {"fav_hi", p.fav_hi}};
    for (const auto& [key, trades] : measured) {
      const json& v = g.params.at(key);
      const bool count = std::string(key) == "count";
      const double scale = count ? 1.0 : 10000.0;
      if (!v.is_number() || std::llround(v.get<double>() * scale) != trades) {
        *why = "the gate's params." + std::string(key) + " " + v.dump() + " is not what N2 trades (" +
               (count ? std::to_string(trades) + " contracts" : price_str(static_cast<Px>(trades))) + ")";
        return std::nullopt;
      }
    }
    return p;
  }
  TakeMode mode = TakeMode::MODEL;
  if (lane.id == "X1") {
    const json& d = g.params.at("direction");   // load_lane_gate refuses a gate without one
    if (d == "follow") mode = TakeMode::ESPN_FOLLOW;
    else if (d == "fade") mode = TakeMode::ESPN_FADE;
    else {
      *why = "the gate's direction " + d.dump() + " is neither follow nor fade";
      return std::nullopt;
    }
  }
  const json& thr = g.params.at("thr");
  const Money edge = thr.is_number() ? static_cast<Money>(std::llround(thr.get<double>() * 10000.0)) : 0;
  if (edge <= 0 || edge >= kOneDollar) {
    *why = "the gate's params.thr " + thr.dump() + " is not a price between 0 and 1 (dollars a contract)";
    return std::nullopt;
  }
  TakeParams p = take_params(mode);
  p.edge = edge;
  p.inventory = lane.inventory;
  p.game_cap = lane.event_cap;
  return p;
}
}  // namespace

std::string Deskd::gates_dir() const { return cfg_.gates_dir.empty() ? cfg_.data_dir + "/gates" : cfg_.gates_dir; }

// C1's gate and what it opens (strat/lane_gate.h): a configured series is tradable when the gate
// is live, the series passed it on the model loaded for it (model_sha: series -> its sha256), and
// its rule can be built; otherwise c1_tradable_ says why not, and with none tradable c1_why_ does:
// the gate's own reason, or each series'.
void Deskd::load_college(const std::map<std::string, std::string>& model_sha) {
  std::string why;
  c1_gate_ = load_lane_gate(lane_gate_path(gates_dir(), "C1"), "C1", "", {"edge"}, &why);
  std::map<std::string, std::string> loaded;
  for (const auto& series : cfg_.college.series)
    if (model_sha.count(series)) loaded[series] = model_sha.at(series);
  const auto measured = c1_gate_ ? tradable_series(*c1_gate_, loaded) : std::map<std::string, std::string>{};
  const auto& lanes = default_lanes();
  const LaneSpec& c1 = *std::find_if(lanes.begin(), lanes.end(), [](const LaneSpec& l) { return l.id == "C1"; });
  std::string each;
  bool any = false;
  for (const auto& series : cfg_.college.series) {
    const auto m = measured.find(series);
    std::string w = !c1_gate_ ? lane_gate_why(c1_gate_, why) : m != measured.end() ? m->second : "the gate did not measure it";
    std::string rule_why;
    auto rule = college_rule(c1_gate_ ? &*c1_gate_ : nullptr, series, cfg_.college, c1, &rule_why);
    if (!rule) {
      if (w.empty()) w = rule_why;
      rule = college_rule(nullptr, series, cfg_.college, c1, &rule_why);   // the defaults, for a series it may not trade
    }
    c1_rules_[series] = *rule;
    c1_tradable_[series] = w;
    any = any || w.empty();
    if (!w.empty()) each += (each.empty() ? "" : "; ") + series + ": " + w;
  }
  if (any) c1_why_.clear();
  else if (!c1_gate_ || !c1_gate_->live()) c1_why_ = lane_gate_why(c1_gate_, why);
  else c1_why_ = c1_gate_->path + " opens no series (" + each + ")";
  if (any) {
    std::string open;
    for (const auto& [series, w] : c1_tradable_)
      if (w.empty()) open += (open.empty() ? "" : ", ") + series;
    DLOG_INFO("C1's gate opens %s%s", open.c_str(), each.empty() ? "" : (" (not " + each + ")").c_str());
  } else {
    DLOG_WARN("C1 gated: %s", c1_why_.c_str());
  }
}

json Deskd::college_status() const {
  json gate = nullptr, tradable = json::object(), rules = json::object();
  if (c1_gate_)
    gate = {{"path", c1_gate_->path}, {"stage", c1_gate_->stage}, {"verdict", c1_gate_->verdict},
            {"computed", c1_gate_->computed}, {"tool", c1_gate_->tool}};
  for (const auto& [series, why] : c1_tradable_) tradable[series] = why;
  for (const auto& [series, p] : c1_rules_)
    rules[series] = {{"edge", money_str(p.edge)}, {"level", p.points_required ? "point" : "set"}, {"leader_only", p.leader_only}};
  std::string why = c1_why_;
  if (!cfg_.college.enabled) why = "deskd.college.enabled is off";
  else if (!cfg_.systematic_orders) why = "Systematic orders are off (deskd.systematic_orders)";
  return {{"enabled", cfg_.college.enabled},
          {"why", why},
          {"gates_dir", gates_dir()},
          {"gate", gate},
          {"tradable", tradable},
          {"rules", rules},
          {"clip", money_str(cfg_.college.clip)},
          {"game", money_str(cfg_.college.game)},
          {"budget", money_str(cfg_.college.budget)},
          {"max_score_age_s", cfg_.college.max_score_age_s},
          {"volley_model", {{"path", cfg_.college.volley_model}, {"loaded", volley_.has_value()}, {"why", volley_why_},
                            {"sha256", volley_ ? volley_->sha256 : ""}}},
          {"engine", c1_ ? c1_->status() : json(nullptr)}};
}

// X1's, N1's and N2's gates and the rules they open (strat/lane_gate.h): X1's direction and thr;
// N1's thr, measured on the NBA model loaded; N2's count and price band, no model. A gate that
// loads gives the lane its rule and engine: a live one opens the lane, one at stage shadow only
// lets the engine record (why says so). Otherwise why is the gate's reason, or the model's for N1.
void Deskd::load_nba() {
  const auto& lanes = default_lanes();
  for (NbaLane* n : {&x1_, &n1_, &n2_}) {
    const LaneSpec& spec = *std::find_if(lanes.begin(), lanes.end(), [n](const LaneSpec& l) { return l.id == n->id; });
    const bool model = n->id == "N1";
    const std::vector<std::string> params = n->id == "X1"   ? std::vector<std::string>{"direction", "thr"}
                                            : n->id == "N1" ? std::vector<std::string>{"thr"}
                                                            : std::vector<std::string>{"count", "fav_lo", "fav_hi"};
    if (model && !nba_) {
      n->why = "no NBA model" + (nba_why_.empty() ? std::string() : " (" + nba_why_ + ")");
    } else {
      std::string why;
      n->gate = load_lane_gate(lane_gate_path(gates_dir(), n->id), n->id, model ? nba_->sha256 : "", params, &why);
      n->why = lane_gate_why(n->gate, why);
      if (n->gate) {
        n->rule = nba_rule(*n->gate, spec, &why);
        if (!n->rule) n->why = n->gate->path + ": " + why;
      }
    }
    if (n->rule && n->why.empty()) {
      const std::string what = n->rule->mode == TakeMode::FAV_PREGAME
                                   ? "band " + price_str(n->rule->fav_lo) + "-" + price_str(n->rule->fav_hi)
                                   : "edge " + money_str(n->rule->edge);
      DLOG_INFO("%s's gate opens it: %s, %s", n->id.c_str(), take_mode_str(n->rule->mode), what.c_str());
    } else {
      DLOG_WARN("%s gated: %s", n->id.c_str(), n->why.c_str());
    }
  }
}

json Deskd::nba_status() const {
  auto lane = [](const NbaLane& n) {
    json gate = nullptr, rule = nullptr;
    if (n.gate)
      gate = {{"path", n.gate->path}, {"stage", n.gate->stage}, {"verdict", n.gate->verdict},
              {"computed", n.gate->computed}, {"tool", n.gate->tool}};
    if (n.rule && n.rule->mode == TakeMode::FAV_PREGAME)   // N2: a price band and a count, no edge
      rule = {{"mode", take_mode_str(n.rule->mode)}, {"count", qty_to_number(n.rule->fixed_count)},
              {"band", {price_str(n.rule->fav_lo), price_str(n.rule->fav_hi)}}};
    else if (n.rule)
      rule = {{"mode", take_mode_str(n.rule->mode)}, {"edge", money_str(n.rule->edge)}};
    return json{{"why", n.why}, {"gate", gate}, {"rule", rule}, {"engine", n.engine ? n.engine->status() : json(nullptr)}};
  };
  std::string why;
  if (!cfg_.nba_lanes) why = "deskd.nba_lanes is off";
  else if (!cfg_.systematic_orders) why = "Systematic orders are off (deskd.systematic_orders)";
  return {{"enabled", cfg_.nba_lanes},
          {"why", why},
          {"series", cfg_.nba_series},
          {"shadow", cfg_.lane_shadow},
          {"gates_dir", gates_dir()},
          {"model", {{"path", cfg_.nba_model}, {"loaded", nba_.has_value()}, {"why", nba_why_},
                     {"version", nba_ ? nba_->version : 0}, {"sha256", nba_ ? nba_->sha256 : ""}}},
          {"X1", lane(x1_)},
          {"N1", lane(n1_)},
          {"N2", lane(n2_)}};
}

void Deskd::attach_manual() {
  ManualPath p;
  p.snapshot = [this] { return rest_->snapshot(); };
  p.book = [this](const std::string& tk) -> std::optional<MdBook> {
    const auto books = std::atomic_load(&books_);
    if (!books) return std::nullopt;
    const auto it = books->find(tk);
    if (it == books->end()) return std::nullopt;
    return it->second;
  };
  p.health = [this] {
    ManualHealth h;
    h.read_only = !cfg_.manual_orders;
    const auto kx = feeds_.KX.view(), kxp = feeds_.KXP.view();
    h.public_feed = kx.up && !kx.gap;
    h.private_feed = kxp.up && !kxp.gap;
    h.publisher = pub_->healthy();
    h.manual_sender = man_sender_->healthy();
    h.cancel_sender = cx_sender_->healthy();
    auto stopped = [this](const char* who, const KalshiSender& s) {
      std::string why = std::string(who) + " stopped: " + s.fail_reason();
      why += kill_.active() ? "; re-arm KILL once its cancel-all is confirmed (press KILL again if it was not)"
                            : "; press KILL, then re-arm once its cancel-all is confirmed";
      return why;
    };
    if (h.read_only) h.why = "Manual orders are off (deskd.manual_orders in config/desk.json)";
    else if (!h.publisher) h.why = "the page publisher dropped updates; restart deskd";
    else if (!h.manual_sender) h.why = stopped("send-k1", *man_sender_);
    else if (!h.cancel_sender) h.why = stopped("send-cx", *cx_sender_);
    return h;
  };
  p.write_tier = cfg_.write_tier == "advanced" ? WriteBudget::advanced() : WriteBudget::basic();
  p.live = cfg_.live;
  p.check_each_message = options_.check_each_message;
  p.record = [this](const std::string& table, std::string row) { return store_.inbox().push(StoreRecord{table, std::move(row)}); };
  // gw's alerts on the page: store falling behind on any desk, a lane's refusals with Systematic
  p.alert = [this](const std::string& lvl, const std::string& src, const std::string& msg) { pub_->alert(lvl, src, msg); };
  if (cfg_.systematic_orders) {
    p.sys_send = [this](KalshiWriteJob j) { return q_sys_.push(std::move(j)); };
    p.sys_cancel = [this](KalshiWriteJob j) { return q_sys_.push(std::move(j), Lane::PRIORITY); };
    p.sys_health = [this] {
      ManualHealth h;
      h.read_only = false;
      const auto kx = feeds_.KX.view(), kxp = feeds_.KXP.view();
      h.public_feed = kx.up && !kx.gap;
      h.private_feed = kxp.up && !kxp.gap;
      h.publisher = pub_->healthy();
      h.manual_sender = h.cancel_sender = sys_sender_->healthy();
      if (!h.publisher) h.why = "the page publisher dropped updates; restart deskd";
      else if (!h.manual_sender)
        h.why = "send-k2 stopped: " + sys_sender_->fail_reason() +
                (kill_.active() ? "; re-arm KILL once its cancel-all is confirmed" : "; press KILL, then re-arm once its cancel-all is confirmed");
      return h;
    };
    p.publish_market = [this](const std::string& tk) { publish_market(tk); };
    p.lane_lip = [this](const std::string& lane) {
      const auto m = lip_meter_.lane_day(now_ms());
      const auto it = m.find(lane);
      return it == m.end() ? Money{0} : it->second;
    };
    p.publish_lane = [this](const std::string& row) {
      auto j = json::parse(row, nullptr, false);
      if (!j.is_discarded()) pub_->post(PubData{"lane", std::move(j)});
    };
    p.daily_stop = cfg_.daily_stop;
    p.equity_floor = cfg_.equity_floor;
    p.groups = &groups_;
    p.sys_state_file = cfg_.data_dir + "/systematic.json";
    p.lanes = default_lanes();
    for (auto& l : p.lanes) {
      if (l.id == "M1" || l.id == "P1") l.series = cfg_.lip_series;
      if (l.id == "M2") l.series = cfg_.band_series;
      // C1 (plan 5(d)): the series its gate lets it trade, within deskd.college's amounts; no
      // engine without one, and then the gate's reason (LaneSet and place_sys refuse it)
      if (l.id == "C1" && college_on()) {
        l.series.clear();
        for (const auto& [series, why] : c1_tradable_)
          if (why.empty()) l.series.push_back(series);
        l.engine = !l.series.empty();
        l.gate_why = c1_why_;
        l.budget = cfg_.college.budget;
        l.event_cap = cfg_.college.game * 100;   // u to micro-dollars
        if (l.engine) l.thr = c1_rules_.at(l.series.front()).edge;   // the gate's, one for every series
      }
      // X1, N1 and N2 (plan 5(e), decisions 2): NBA's series and an engine once the gate gives a
      // rule, and the gate's reason until it is live (LaneSet and place_sys refuse it); N2's rule
      // is a price band, not an edge (no thr)
      if ((l.id == "X1" || l.id == "N1" || l.id == "N2") && nba_on()) {
        const NbaLane& n = l.id == "X1" ? x1_ : l.id == "N1" ? n1_ : n2_;
        l.series = n.rule ? cfg_.nba_series : std::vector<std::string>{};
        l.engine = n.rule.has_value();
        l.gate_why = n.why;
        if (n.rule && n.rule->mode != TakeMode::FAV_PREGAME) l.thr = n.rule->edge;
      }
    }
    if (!cfg_.live && cfg_.paper_lane) p.lanes.push_back(paper_lane());   // D1: the demo account only
  }
  if (cfg_.systematic_orders) {
    // LIP-15 (lane M1): the 15-minute series it trades (deskd.lip_series), quoted by the maker engine
    MakerEngine::Env env;
    env.view = [this] { return gw_->sys_view(); };
    env.snapshot = [this] { return rest_->snapshot(); };
    env.book = [this](const std::string& tk) -> std::optional<MdBook> {
      const auto books = std::atomic_load(&books_);
      if (!books) return std::nullopt;
      const auto it = books->find(tk);
      if (it == books->end()) return std::nullopt;
      return it->second;
    };
    env.anchor = [this](const std::string& tk) {
      MakerEngine::Env::Anchor a;
      const Underlying* u = underlying_for(tk);
      if (!u || !fair_) return a;   // no Pyth underlying: no reference rule applies
      // the underlying's Pyth value by the gap rule, gate or no gate; never seen counts as a gap
      const auto symbol = pyth_->symbol_for(u->patterns);
      const auto ref = symbol.empty() ? std::nullopt : pyth_->view(symbol, -1, now_ms());
      a.fresh = ref && ref->fresh;
      const auto board = fair_->board();
      const auto f = board->find(tk);
      if (ref) a.spot = ref->value;
      // the 0.5 sigma pull's yardstick, on every window (checked strike or not)
      const auto move = fair_->move_sd();
      if (const auto sd = move->find(tk); sd != move->end()) a.sd_log = sd->second;
      if (!fair_->gated(u->key)) return a;
      a.gated = true;
      if (f != board->end()) a.fair = f->second.fair;
      return a;
    };
    env.private_up = [this] { const auto v = feeds_.KXP.view(); return v.up && !v.gap; };
    env.post = [this](GwMsg m) { return gw_->post(std::move(m)); };
    env.fill_p = [this](const std::string& tk, bool buy, Qty ahead, Qty size, int ttl_s) {
      const auto e = fill_model_.estimate(tk, !buy, ahead, size, ttl_s, now_ms());
      return e.why.empty() ? e.p_full : 0.0;
    };
    env.alert = [this](const std::string& lvl, const std::string& msg) { pub_->alert(lvl, "SYS", msg); };
    env.release = [this](const std::string& tk, std::int64_t now) { return calendar_.active(tk, now); };
    auto want = [this](const std::string& lane) {
      return [this, lane](std::set<std::string> markets) {
        bool changed = false;
        {
          std::lock_guard<std::mutex> lk(live_mu_);
          changed = markets != strat_want_[lane];
          if (changed) strat_want_[lane] = std::move(markets);
        }
        if (changed) refresh_live();
      };
    };
    env.want = want("M1");
    env.markout = [this] { const auto m = markouts_->lane_mean("M1", "60s"); return std::make_pair(m.fills, m.mean); };
    MakerParams lip;
    lip.mo_after = 200;   // plan section 5: "Rule after 200 fills" retires LIP-15 below -1.5c
    lip.mo_floor = 150;
    lip.ttl_s = 15;       // plan section 2: maker orders rest 5-15 s (late fills are the toxic ones)
    lip.pull_touch_ticks = 2;   // plan section 5(a): a 2-tick touch move pulls both sides
    lip_ = std::make_unique<MakerEngine>("M1", cfg_.lip_series, env, lip);
    // the LIP probe (lane P1, off until the operator turns it on): LIP-15's windows at 30 a side,
    // two-sided in minutes 0-5 only (then only the reducing side), 40 windows a day, paused once
    // its day is down $15 (plan section 5(a))
    MakerParams probe = lip;
    probe.size = 3000;
    probe.two_sided_ms = 300000;
    probe.window_cap = 40;
    probe.lane_pause = 15 * 1000000LL;
    probe.mo_after = 0;   // a measurement of share and payout, not the markout rule
    auto probe_env = env;
    probe_env.want = want("P1");
    probe_env.markout = [this] { const auto m = markouts_->lane_mean("P1", "60s"); return std::make_pair(m.fills, m.mean); };
    probe_ = std::make_unique<MakerEngine>("P1", cfg_.lip_series, probe_env, probe);
    // BAND 96-99 (lane M2): the weather highs first (deskd.band_series); no anchor; 5 a market,
    // $15 an event (a city's day), and it stops quoting once its day is down $20 (plan section 5)
    MakerParams band;
    band.size = 500;
    band.event_cap = 15 * 1000000LL;
    band.lane_pause = 20 * 1000000LL;
    band.mo_after = 100;  // "pause below -1c after 100 fills"
    band.mo_floor = 100;
    auto band_env = env;
    band_env.anchor = {};
    band_env.want = want("M2");
    band_env.markout = [this] { const auto m = markouts_->lane_mean("M2", "60s"); return std::make_pair(m.fills, m.mean); };
    band_env.release = [this](const std::string& tk, std::int64_t now) { return calendar_.active(tk, now); };
    band_ = std::make_unique<MakerEngine>("M2", cfg_.band_series, band_env, band, band_quote);
    // D1 PAPER (demo only): 1 lot at the touch both sides in two liquid markets it picks, an IOC
    // buy after 5 quiet minutes, any position out within 90 s, so orders go in and out where the
    // operator can watch them (2026-10-14: M1's book on demo is empty and M2's markets are not
    // listed there, so Systematic on PAPER never sent an order)
    if (!cfg_.live && cfg_.paper_lane) {
      MakerParams paper;
      paper.size = kOneContract;
      paper.fill_target_ms = 0;
      auto paper_env = env;
      paper_env.anchor = {};
      paper_env.release = {};
      paper_env.want = want("D1");
      paper_env.markout = [this] { const auto m = markouts_->lane_mean("D1", "60s"); return std::make_pair(m.fills, m.mean); };
      paper_env.select = [](const rest::Snapshot& s, std::int64_t now, const std::set<std::string>& keep) {
        return paper_markets(s, now, keep);
      };
      paper_ = std::make_unique<MakerEngine>("D1", std::vector<std::string>{}, paper_env, paper, paper_quote);
    }
    // C1 COLLEGE TAKE (plan 5(d)): IOC takes of thin college games' stale quotes against the game
    // fair, on the merged score, each series on the rule its gate measured; its fills' markouts at
    // its retire rule's horizon (300 s) go to gw
    if (college_on()) {
      TakeEngine::Env te;
      te.view = env.view;
      te.snapshot = env.snapshot;
      te.book = env.book;
      te.fairs = [this] { return game_fair_ ? game_fair_->board() : nullptr; };
      te.scores = [this] { return scores_ ? scores_->board() : nullptr; };
      te.tradable = [this] { return c1_tradable_; };
      te.private_up = env.private_up;
      te.post = env.post;
      te.want = want("C1");
      te.markout = [this] { const auto m = markouts_->lane_mean("C1", "60s"); return std::make_pair(m.fills, m.mean); };
      te.mid_at = [this](const std::string& tk, std::int64_t at) { return markouts_->mid_at(tk, at); };
      te.record = [this](const std::string& table, std::string row) { return store_.inbox().push(StoreRecord{table, std::move(row)}); };
      te.alert = env.alert;
      TakeOptions o;
      for (const auto& l : p.lanes)
        if (l.id == "C1") o.mo_h = l.retire_h;
      c1_ = std::make_unique<TakeEngine>("C1", c1_rules_, std::move(te), o);
    }
    // X1 ESPN EXP (plan 5(e)), N1 NBA MODEL and N2 NBA FAV 90-95 (decisions 2): IOC takes in NBA
    // games against ESPN's number, followed or faded as X1's gate measured, or against the NBA
    // model's fair (N1), on the merged score; N2 buys a 90-95c favourite before the start, 5
    // contracts held to settlement. With deskd.lane_shadow, what each would take while it may
    // not post goes to the signals table. N1 holds new takes while its fills' net 60 s markout
    // averages at or under 0 after 100 (nba.json's hold, plan 5(d)'s rule); gw retires each past
    // its trades or its loss (N2: 60 trades or $15).
    for (NbaLane* n : {&x1_, &n1_, &n2_}) {
      if (!nba_on() || !n->rule) continue;
      const std::string id = n->id;
      std::map<std::string, std::string> tradable;   // every series on the lane's one gate
      for (const auto& series : cfg_.nba_series) tradable[series] = n->why;
      TakeEngine::Env te;
      te.view = env.view;
      te.snapshot = env.snapshot;
      te.book = env.book;
      te.fairs = [this] { return game_fair_ ? game_fair_->board() : nullptr; };
      te.scores = [this] { return scores_ ? scores_->board() : nullptr; };
      te.tradable = [tradable] { return tradable; };
      te.private_up = env.private_up;
      te.post = env.post;
      te.want = want(id);
      te.markout = [this, id] { const auto m = markouts_->lane_mean(id, "60s"); return std::make_pair(m.fills, m.net); };
      te.mid_at = [this](const std::string& tk, std::int64_t at) { return markouts_->mid_at(tk, at); };
      te.record = [this](const std::string& table, std::string row) { return store_.inbox().push(StoreRecord{table, std::move(row)}); };
      te.alert = env.alert;
      TakeOptions o;
      o.shadow = cfg_.lane_shadow;
      for (const auto& l : p.lanes)
        if (l.id == id) o.mo_h = l.retire_h;   // none today: gw counts their trades and losses
      if (id == "N1") {
        o.mo_after = 100;
        o.mo_floor = 0;
      }
      n->engine = std::make_unique<TakeEngine>(id, cfg_.nba_series, std::move(te), *n->rule, o);
    }
  }
  p.send = [this](KalshiWriteJob j) { return q_man_.push(std::move(j)); };
  p.refresh = [this] { rest_->request_refresh(); };
  p.cancel = [this](KalshiWriteJob j) { return q_mcx_.push(std::move(j), Lane::PRIORITY); };
  p.publish = [this](const std::string& row) {
    auto j = json::parse(row, nullptr, false);
    if (!j.is_discarded()) pub_->post(PubData{"ord", std::move(j)});
  };
  gw_->attach_manual(p);
  // a restart reads back to the saved book's last orders, so their later fills keep their lane
  if (gw_->sys_saved_ms()) rest_->set_orders_since(gw_->sys_saved_ms() - 65000);
}

HdrInputs Deskd::header() const {
  HdrInputs h;
  h.live = cfg_.live;
  h.run = gw_->run_view();
  h.preflight = ops_->preflight();
  h.daily_stop = cfg_.daily_stop;
  h.kx_lag_ms = feeds_.KX.view().lag_ms;
  if (const auto l = gw_->latency()) h.gw_ack_ms = l->ack20_p50;
  if (!rest_) return h;
  const auto s = rest_->snapshot();
  // Read-only until the operator turns Manual orders on (deskd.manual_orders) with a write path.
  h.cash_k=rest_cash_json(*s,!(cfg_.manual_orders && man_sender_));
  h.clk_off_ms = s->clock.offset_ms;
  // Manual's day: realized today plus open positions at the mid; risk: the open worst case plus
  // what working orders hold (cash less free).
  Money unrl = 0, maxl = 0;
  for (const auto& p : s->positions) {
    const auto row = rest_position_json(p, mid(p.ticker));
    unrl += parse_money(row.at("unrl").get<std::string>()).value_or(0);
    maxl += parse_money(row.at("maxl").get<std::string>()).value_or(0);
  }
  const auto day = std::atomic_load(&day_);
  h.day_man = (day ? day->real_total : 0) + unrl;
  const auto total = parse_money(h.cash_k.value("total", std::string("0.0000"))).value_or(0);
  const auto free = parse_money(h.cash_k.value("free", std::string("0.0000"))).value_or(0);
  h.risk = maxl + std::max<Money>(0, total - free);
  if (cfg_.systematic_orders)
    if (const auto v = gw_->sys_view()) {
      HdrInputs::Sys sys;
      sys.day = v->day / 100;
      sys.fees = v->fees / 100;
      sys.budget = Gateway::kCostBudget;
      sys.used = v->used / 100;
      sys.ceiling = v->ceiling / 100;
      sys.eff = v->eff / 100;
      for (const auto& [lane, est] : lip_meter_.lane_day(now_ms())) sys.lip += est;
      h.sys = sys;
    }
  return h;
}

std::vector<PreflightRow> Deskd::venue_preflight() const {
  if (!rest_) return {};
  const auto s = rest_->snapshot();
  const bool fresh = s->account_ready && now_ms() - s->account_ms < 30000;
  const bool clock = s->clock.post_trusted && now_ms() - s->clock.measured_ms < 30000;
  return {{"KEY",signer_ != nullptr,signer_ ? "" : "no demo key"},
          {"CASH",fresh && s->balance.available > 0,fresh ? "" : "account reconcile pending"},
          shard_row(*s,fresh),
          {"CLK",clock,clock ? "" : "venue clock uncertainty exceeds 500 ms"},
          groups_row()};
}

// SHARD (contract: every shard Systematic uses has cash): each lane that is on, over the open
// markets of its series, needs a quote's worth of free cash on the shard each one is listed on.
// Orders never move cash between shards, and a lane on a $0 shard was refused SHARD_CASH quote
// after quote with nothing on the page but Sent 0 (conformance review, 2026-10-10).
PreflightRow Deskd::shard_row(const rest::Snapshot& s, bool fresh) const {
  if (!fresh) return {"SHARD", false, "shard balances stale"};
  if (s.balances.empty()) return {"SHARD", false, "no shard balances"};
  const auto v = cfg_.systematic_orders ? gw_->sys_view() : nullptr;
  if (!v) return {"SHARD", true, ""};
  constexpr Money kQuote = 5 * 10000;   // a 10-lot at 0.50, the band's 5 at 0.97, or C1's largest clip
  std::map<std::string, std::vector<std::string>> series = {{"M1", cfg_.lip_series}, {"P1", cfg_.lip_series}, {"M2", cfg_.band_series}};
  for (const auto& [name, why] : c1_tradable_)
    if (why.empty()) series["C1"].push_back(name);
  for (const NbaLane* n : {&x1_, &n1_, &n2_})
    if (n->rule) series[n->id] = cfg_.nba_series;
  const auto now = now_ms();
  for (const auto& lane : v->lanes_on) {
    const auto it = series.find(lane);
    if (it == series.end()) continue;
    for (const auto& [tk, m] : s.markets) {
      const bool mine = std::any_of(it->second.begin(), it->second.end(), [&](const std::string& x) {
        return tk.size() > x.size() && tk.compare(0, x.size(), x) == 0 && tk[x.size()] == '-';
      });
      if (!mine || !m.exchange_index || (m.status != "active" && m.status != "open") || !m.close_ms || *m.close_ms <= now) continue;
      const auto b = s.balances.find(*m.exchange_index);
      const Money cash = b == s.balances.end() ? 0 : b->second;
      if (cash < kQuote)
        return {"SHARD", false, "S" + std::to_string(*m.exchange_index) + " $" + money_str(cash) + " (" + lane + " " + tk + ")"};
    }
  }
  return {"SHARD", true, ""};
}

PreflightRow Deskd::groups_row() const {
  if (!cfg_.systematic_orders) return {"GROUPS", false, "Systematic orders are off (deskd.systematic_orders in config/desk.json)"};
  std::string why;
  const bool ok = gw_->groups_ready(&why);
  return {"GROUPS", ok, why};
}

void Deskd::refresh_live() {
  if (!rest_ || !md_) return;
  const auto s = rest_->snapshot();
  std::set<std::string> selected;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    selected = pins_;
    for (const auto& [lane, markets] : strat_want_) selected.insert(markets.begin(), markets.end());
  }
  for (const auto& p : s->positions) if (p.position) selected.insert(p.ticker);
  for (const auto& o : s->orders)
    if (o.status == kalshi::OrderStatus::RESTING) selected.insert(o.ticker);
  for (const auto& stream : pub_->streams().open_streams()) {
    const auto sub = pub_->streams().subs(stream->sid);
    for (const auto& ref : sub.ladders) if (ref.v == Venue::K) selected.insert(ref.tk);
    for (const auto& ref : sub.tops) if (ref.v == Venue::K) selected.insert(ref.tk);
  }
  // A small default live set; the full listing remains REST-paged. Never subscribe the whole
  // exchange merely because no ticker has been pinned. Roll this set on every REST refresh.
  std::vector<const kalshi::MarketRow*> candidates;
  const auto today = et_date(now_ms()), tomorrow = et_date_after(today);
  for (const auto& pair : s->markets) {
    const auto& m = pair.second;
    const auto date = rest::outcome_date(m);
    if ((m.status == "active" || m.status == "open") && m.close_ms && *m.close_ms > now_ms() &&
        (date == today || date == tomorrow)) candidates.push_back(&m);
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
    if (a->volume != b->volume) return a->volume.value_or(0) > b->volume.value_or(0);
    return a->ticker < b->ticker;
  });
  for (std::size_t i = 0; i < candidates.size() && i < 8; ++i) selected.insert(candidates[i]->ticker);
  for (auto it = selected.begin(); it != selected.end();) {
    if (!s->markets.count(*it)) it = selected.erase(it); else ++it;
  }
  std::set<std::string> removed;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    if (selected == live_) return;
    for (const auto& tk : live_) if (!selected.count(tk)) removed.insert(tk);
    live_ = selected;
  }
  for (const auto& tk : removed) {
    const auto it = s->markets.find(tk);
    if (it != s->markets.end()) pub_->post(PubData{"mkt",mkt_row(it->second,false,false)});
  }
  rest_->set_live(selected);
  if (selected.empty()) md_->clear_watch();
  else md_->set_watch(std::vector<std::string>(selected.begin(),selected.end()));
}

void Deskd::on_rest(std::shared_ptr<const rest::Snapshot>) {
  if (!rest_) return;
  if (const auto held = gw_->sys_held()) rest_->set_held(*held);   // read until they settle
  refresh_live();
  const auto s = rest_->snapshot();
  for (const auto& tk : s->live) {
    const auto it = s->markets.find(tk);
    if (it != s->markets.end())
      pub_->post(PubData{"mkt",mkt_row(it->second,s->pins.count(tk),true)});
  }
  if (!s->account_ready) return;
  const auto owned = gw_->manual_cids();   // gw publishes its own orders' rows
  for (const auto& order : s->orders) {
    if (owned && owned->count(order.client_order_id)) continue;
    auto row = rest_order_json(order,now_ms());
    row["rev"] = std::max<std::uint64_t>(1,s->revision);
    pub_->post(PubData{"ord",std::move(row)});
  }
  // Today's fills (new ones only; each is keyed by fid) and today's P&L per market.
  auto day = std::make_shared<DayPnl>(day_pnl(*s, et_midnight_ms(now_ms()), [this](const std::string& tk) { return mid(tk); }));
  if (day->unknown_basis && day->unknown_basis != unknown_basis_said_)   // once per count, not every poll
    DLOG_WARN("%zu of today's fills reduce a position whose cost before today is not recoverable: they count only their fee",
              day->unknown_basis);
  unknown_basis_said_ = day->unknown_basis;
  for (const auto& f : day->fills) {
    const auto fid = f.at("fid").get<std::string>();
    if (!fills_sent_.insert(fid).second) continue;
    pub_->post(PubData{"fill", f});
    if (markouts_) {
      const auto px = parse_price(f.at("px").get<std::string>());
      const auto qty = qty_from_number(f.at("qty").get<double>());
      const auto fee = parse_money(f.at("fee").get<std::string>());   // the fill's, for the net markouts
      if (px && qty && fee)
        markouts_->fill(fid, f.at("lane").is_string() ? f.at("lane").get<std::string>() : "", f.at("tk").get<std::string>(),
                        f.at("side") == "BUY" ? Side::BUY : Side::SELL, *px, *qty, f.at("ts_ms").get<std::int64_t>(), *fee);
    }
  }
  std::vector<json> positions;
  std::set<std::pair<std::string, int>> listed;
  for (const auto& p : s->positions) {
    const std::pair<std::string, int> key{p.ticker, p.exchange_index};
    listed.insert(key);
    const auto r = day->real.find(key), f = day->fees.find(key);
    positions.push_back(rest_position_json(p, mid(p.ticker), r == day->real.end() ? 0 : r->second,
                                           f == day->fees.end() ? 0 : f->second));
  }
  // A market traded flat today keeps its row, with today's realized P&L (contract pos).
  for (const auto& [key, real] : day->real) {
    if (listed.count(key)) continue;
    kalshi::PositionRow flat;
    flat.ticker = key.first;
    flat.exchange_index = key.second;
    positions.push_back(rest_position_json(flat, std::nullopt, real, day->fees.at(key)));
  }
  std::atomic_store(&day_, std::shared_ptr<const DayPnl>(std::move(day)));
  pub_->post(PubReplace{"pos",std::move(positions),"K"});
  // The risk book (contract risk): each event's positions with the P&L of its worst and best
  // outcome, and cash per shard. A market alone pays $1 a contract or nothing against what the
  // position cost; an event's markets are summed as if independent, a bound on both ends for a
  // ladder of exclusive outcomes (never tighter than the truth).
  std::map<std::string, std::pair<Micros, Micros>> outcomes;
  std::map<std::string, json> members;
  for (const auto& p : s->positions) {
    if (!p.position) continue;
    const auto it = s->markets.find(p.ticker);
    const std::string og = it != s->markets.end() && !it->second.event_ticker.empty() ? it->second.event_ticker : p.ticker;
    const Micros cost = std::max<Micros>(0, p.exposure);
    auto& o = outcomes[og];
    o.first -= cost;
    o.second += kOneDollar * std::abs(p.position) - cost;
    members[og].push_back({{"v", "K"}, {"tk", p.ticker}});
  }
  json groups = json::array();
  for (const auto& [og, o] : outcomes)
    groups.push_back({{"id", og}, {"name", og}, {"worst", money_str(o.first / 100)}, {"best", money_str(o.second / 100)},
                      {"members", members[og]}});
  json shards = json::array();
  for (const auto& sh : rest_cash_json(*s, true).value("shards", json::array()))
    shards.push_back({{"v", "K"}, {"id", sh["id"]}, {"cash", sh["cash"]}, {"free", sh["free"]}});
  pub_->post(PubData{"risk", {{"groups", std::move(groups)}, {"shards", std::move(shards)}}});
}

std::optional<Px> Deskd::mid(const std::string& ticker) const {
  const auto books = std::atomic_load(&books_);
  if (!books) return std::nullopt;
  const auto it = books->find(ticker);
  if (it == books->end() || it->second.stale || it->second.bids.empty() || it->second.asks.empty()) return std::nullopt;
  return (it->second.bids.front().px + it->second.asks.front().px) / 2;
}

void Deskd::on_trade(const MdTrade& t) {
  fill_model_.add(t.ticker, t.trade_id, TapePrint{t.ts_ms, t.qty, t.taker_bought_yes});
  auto& tape = tape_[t.ticker];
  if (!t.trade_id.empty()) {
    if (!tape.seen.insert(t.trade_id).second) return;   // already shown
    if (tape.seen.size() > 20000) tape.seen.clear();
  }
  tape.pending.push_back(t);
  if (tape.pending.size() > 200) tape.pending.erase(tape.pending.begin());
  if (!tape.last || t.ts_ms >= tape.last->ts_ms) tape.last = t;
  if (t.ts_ms > tape.listing_ms) tape.since_listing += t.qty;
  const auto book = md_last_.find(t.ticker);
  if (book != md_last_.end() && !book->second.stale) publish_book(t.ticker);
}

void Deskd::publish_book(const std::string& ticker) {
  const auto bk = md_last_.find(ticker);
  if (bk == md_last_.end()) return;
  const MdBook& b = bk->second;
  auto& tape = tape_[ticker];
  auto print = [](const MdTrade& t) {
    return json{{"px", price_str(t.px)}, {"qty", qty_to_number(t.qty)}, {"side", t.taker_bought_yes ? "BUY" : "SELL"},
                {"ts_ms", t.ts_ms}};
  };
  json bids = json::array(), asks = json::array(), trades = json::array();
  for (const auto& level : b.bids) bids.push_back({price_str(level.px),qty_to_number(level.qty)});
  for (const auto& level : b.asks) asks.push_back({price_str(level.px),qty_to_number(level.qty)});
  for (const auto& t : tape.pending) trades.push_back(print(t));
  tape.pending.clear();
  // vol: Kalshi's 24-hour volume from the latest listing, plus prints seen since it was read
  // (the contract's "today" is approximated by the venue's 24 h figure).
  Qty vol = 0;
  if (rest_) {
    const auto s = rest_->snapshot();
    if (tape.listing_ms != s->listing_ms) { tape.listing_ms = s->listing_ms; tape.since_listing = 0; }
    const auto m = s->markets.find(ticker);
    if (m != s->markets.end() && m->second.volume_24h) vol = *m->second.volume_24h;
  }
  vol += tape.since_listing;
  const json ref = ref_json(ticker);
  ref_sent_[ticker] = ref.dump();
  pub_->post(PubData{"book",{{"v","K"},{"tk",ticker},{"depth","FULL"},
      {"bids",bids},{"asks",asks},{"trades",trades},{"last",tape.last ? print(*tape.last) : json(nullptr)},
      {"vol",qty_to_number(std::max<Qty>(0, vol))},
      {"ref",ref},
      {"ex_ms",b.venue_ms.value_or(b.last_change_ms)}}});
}

// A book's reference values: the model's fair and sd where a model stands behind one
// (strat/fair.h), ESPN's game state and win probability on game markets (ref/espn.h). Never a
// placeholder: what no source gives is null.
json Deskd::ref_json(const std::string& ticker) const {
  json ref = {{"fair",nullptr},{"sd",nullptr},{"espn",nullptr},{"espn_ts_ms",nullptr},{"game",nullptr}};
  if (fair_) {
    const auto board = fair_->board();
    const auto f = board->find(ticker);
    if (f != board->end()) {
      ref["fair"] = price_str(f->second.fair);
      ref["sd"] = price_str(f->second.sd);
    }
  }
  if (espn_) {
    const auto board = espn_->board();
    const auto e = board->find(ticker);
    if (e != board->end()) {
      ref["game"] = {{"per",e->second.per},{"clock",e->second.clock},{"score",e->second.score},{"src","ESPN"}};
      if (e->second.espn) ref["espn"] = price_str(*e->second.espn);
      if (e->second.espn && e->second.espn_ts_ms) ref["espn_ts_ms"] = e->second.espn_ts_ms;
    }
  }
  if (game_fair_) {   // a game model's fair while the game is on, from its pregame price
    const auto board = game_fair_->board();
    const auto g = board->find(ticker);
    if (g != board->end() && g->second.shown()) ref["fair"] = price_str(static_cast<Px>(std::llround(*g->second.p * kOneDollar)));
  }
  return ref;
}

// md thread: a book whose reference values moved since it was last published goes out again.
void Deskd::republish_refs() {
  for (const auto& [ticker, b] : md_last_) {
    if (b.stale) continue;
    const auto sent = ref_sent_.find(ticker);
    if (sent == ref_sent_.end() || sent->second != ref_json(ticker).dump()) publish_book(ticker);
  }
}

void Deskd::on_book(const MdBook& b) {
  // gw reads the latest book of each market, stale ones included, so a gap blocks orders; the game
  // fair takes its pregame prices from the same board (strat/game_fair.h), stale books left out.
  {
    auto next = std::make_shared<std::map<std::string, MdBook>>();
    if (auto old = std::atomic_load(&books_)) *next = *old;
    (*next)[b.ticker] = b;
    std::atomic_store(&books_, std::shared_ptr<const std::map<std::string, MdBook>>(std::move(next)));
  }
  if (!md_last_.count(b.ticker)) {
    // a market's first book: the live prints count from now, the hour before comes by REST
    fill_model_.cover(b.ticker, now_ms());
    if (rest_ && fill_model_.needs_backfill(b.ticker)) rest_->request_trades(b.ticker, now_ms() - 3600000);
  }
  md_last_[b.ticker] = b;
  if (markouts_) {
    const bool two = !b.stale && !b.bids.empty() && !b.asks.empty();
    markouts_->mid(b.ticker, now_ms(), two ? (b.bids.front().px + b.asks.front().px) / 2 : Px{-1});
  }
  // STALE is a venue-wide heartbeat flag; never publish a gapped reconstruction as a price.
  if (b.stale) return;
  publish_book(b.ticker);
}

std::string Deskd::venue_name() const {
  if (cfg_.key_id.empty() && cfg_.pem_path.empty()) return "";
  std::string why;
  const auto ep = net::Endpoint::parse(options_.rest_url, &why);
  if (!ep) return "";
  switch (classify_host(ep->host)) {
    case HostClass::LOOPBACK: return "sim";
    case HostClass::DEMO: return "demo";
    case HostClass::PRODUCTION: return "prod";
    case HostClass::UNKNOWN: break;
  }
  return "";
}

json Deskd::mkt_row(const kalshi::MarketRow& m, bool pin, bool live) const {
  auto row = rest::market_json(m, pin, live, now_ms());
  const auto held = gw_->manual_held();
  row["man"] = held && held->count(m.ticker) > 0;
  return row;
}

void Deskd::publish_market(const std::string& tk) {
  if (!rest_) return;
  const auto s = rest_->snapshot();
  const auto it = s->markets.find(tk);
  if (it != s->markets.end()) pub_->post(PubData{"mkt", mkt_row(it->second, s->pins.count(tk), s->live.count(tk))});
}

json Deskd::fill(const json& q) {
  const auto tk = q.at("tk").get<std::string>();
  if (!rest_) return err_body("UNKNOWN_MARKET", tk + ": no venue connected");
  const auto snap = rest_->snapshot();
  const auto mit = snap->markets.find(tk);
  if (mit == snap->markets.end()) return err_body("UNKNOWN_MARKET", tk + " is not listed");
  const std::int64_t now = now_ms();
  if (fill_model_.needs_backfill(tk)) rest_->request_trades(tk, now - 3600000);
  const auto books = std::atomic_load(&books_);
  const auto bit = books ? books->find(tk) : decltype(books->find(tk)){};
  if (!books || bit == books->end()) return err_body("STALE", tk + ": no book yet (open its ladder or watch it)");
  const MdBook& b = bit->second;
  if (b.stale) return err_body("STALE", tk + ": its book is stale");
  const bool buy = q.at("side") == "BUY";
  const auto asked = kalshi::parse_px(q.at("px").get<std::string>());
  const Qty qty = static_cast<Qty>(std::llround(q.at("qty").get<double>() * kOneContract));
  const int ttl = q.at("ttl").get<int>();
  if (!asked || qty <= 0) return err_body("BAD_REQUEST", "px and qty");
  const auto& grid = mit->second.price_ranges;
  const auto& own = buy ? b.bids : b.asks;   // the side the order rests on, best first
  const auto& opp = buy ? b.asks : b.bids;
  auto better = [buy](Px a, Px c) { return buy ? a > c : a < c; };   // a more aggressive than c
  auto crosses = [&](Px p) { return !opp.empty() && !better(opp.front().px, p) ; };
  FillEstimate shown;   // the estimate at the first resting price: its why and windows
  bool have_shown = false;
  auto point = [&](Px p) {
    json j{{"px", price_str(p)}};
    if (crosses(p)) {   // marketable: fills now up to the depth at or better than p
      Qty depth = 0;
      for (const auto& l : opp) if (!better(l.px, p)) depth += l.qty;
      j["p_full"] = depth >= qty ? 1.0 : 0.0;
      j["p_any"] = depth > 0 ? 1.0 : 0.0;
      j["med_s"] = 0.0;
      j["ahead"] = 0;
      return j;
    }
    Qty ahead = 0;   // price-time priority: everything at p or better on our side is first
    for (const auto& l : own) if (!better(p, l.px)) ahead += l.qty;
    const auto e = fill_model_.estimate(tk, !buy, ahead, qty, ttl, now);
    if (!have_shown) { shown = e; have_shown = true; }
    auto r3 = [](double x) { return std::round(x * 1000.0) / 1000.0; };
    j["p_full"] = r3(e.p_full);
    j["p_any"] = r3(e.p_any);
    j["med_s"] = e.median_s < 0 ? json(nullptr) : json(std::round(e.median_s * 10.0) / 10.0);
    j["ahead"] = qty_to_number(ahead);
    return j;
  };
  // the curve, most aggressive first: one tick better than our touch (unless that crosses), the
  // touch, then up to three ticks behind it
  std::vector<Px> prices;
  auto step = [&](Px p, bool toward) -> std::optional<Px> {   // one grid tick more (toward) or less aggressive
    const bool up = buy == toward;
    return up ? grid_ceil(p + 1, grid) : grid_floor(p - 1, grid);
  };
  std::optional<Px> touch = own.empty() ? std::nullopt : std::optional<Px>(own.front().px);
  if (touch) {
    if (auto in = step(*touch, true); in && !crosses(*in)) prices.push_back(*in);
    prices.push_back(*touch);
  } else if (!opp.empty()) {
    if (auto in = step(opp.front().px, false)) prices.push_back(*in);
  }
  for (int i = 0; i < 3 && !prices.empty(); ++i)
    if (auto behind = step(prices.back(), false); behind && *behind > 0 && *behind < kOneDollar) prices.push_back(*behind);
  json curve = json::array();
  json p80 = nullptr;
  for (const Px p : prices) curve.push_back(point(p));
  const json at = point(*asked);
  if (have_shown && shown.why.empty())
    for (auto it = curve.rbegin(); it != curve.rend(); ++it)   // least aggressive first
      if ((*it)["p_full"].get<double>() >= 0.8) { p80 = (*it)["px"]; break; }
  const std::int64_t from = fill_model_.covered_from(tk);
  json take{{"px", opp.empty() ? json(nullptr) : json(price_str(opp.front().px))},
            {"depth", opp.empty() ? 0.0 : qty_to_number(opp.front().qty)}};
  return json{{"ok", true},
              {"fill", {{"tk", tk}, {"side", buy ? "BUY" : "SELL"}, {"ttl", ttl}, {"qty", qty_to_number(qty)},
                        {"at", at}, {"curve", curve}, {"p80_px", p80}, {"take", take},
                        {"tape_s", from > 0 && now > from ? (now - from) / 1000 : 0}, {"windows", shown.windows},
                        {"why", have_shown ? shown.why : std::string()}}}};
}

json Deskd::watch(const json& request) {
  const auto tk = request.at("tk").get<std::string>();
  if (!rest_ || request.at("v") != "K") return err_body("UNKNOWN_MARKET",tk);
  const auto s = rest_->snapshot();
  const auto it = s->markets.find(tk);
  if (it == s->markets.end()) return err_body("UNKNOWN_MARKET",tk);
  const bool pin = request.at("on").get<bool>();   // contract 1.5: {v, tk, on}
  const auto row = rest::market_json(it->second,pin,true,now_ms());
  if (pin && (row.at("status") != "OPEN" || !it->second.close_ms || *it->second.close_ms <= now_ms()))
    return err_body("MARKET_CLOSED",tk);
  std::set<std::string> pins;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    if (pin) pins_.insert(tk); else pins_.erase(tk);
    pins = pins_;
  }
  rest_->set_pins(std::move(pins));
  refresh_live();
  const auto current = rest_->snapshot();
  const auto m = mkt_row(it->second,pin,current->live.count(tk));
  pub_->post(PubData{"mkt",m});   // the market's row follows on the stream
  return {{"ok",true},{"pin",pin}};
}
// Our LIP share on each program's market, once a second (strat/lip.h), from the venue's book (our
// quotes in it) and Systematic's resting orders; each window that ends goes to store.
void Deskd::lip_step(std::int64_t now) {
  if (!rest_ || !gw_) return;
  const auto snap = rest_->snapshot();
  if (!snap->lip.empty()) {
    const auto books = std::atomic_load(&books_);
    const auto view = gw_->sys_view();
    for (const auto& [tk, program] : snap->lip) {
      if (!books) break;
      const auto b = books->find(tk);
      if (b == books->end()) continue;
      std::vector<LipOwn> ours;
      if (view)
        for (const auto& [cid, o] : view->orders)
          if (o.ticker == tk && !o.terminal && o.left > 0 && (o.state == OrdState::WORK || o.state == OrdState::PART))
            ours.push_back({o.side == Side::BUY, o.price, o.left, o.lane});
      lip_meter_.sample(tk, program, b->second, ours, now);
    }
  }
  for (auto& row : lip_meter_.finished(now)) {
    DLOG_INFO("LIP %s: %s s scored of %s, mean share %s, est $%s of $%s", row["tk"].get<std::string>().c_str(),
              row["scored_s"].dump().c_str(), row["seconds"].dump().c_str(), row["mean_share"].dump().c_str(),
              row["est"].get<std::string>().c_str(), row["reward"].get<std::string>().c_str());
    store_.inbox().push(StoreRecord{"lip_windows", row.dump()});
  }
}
}
