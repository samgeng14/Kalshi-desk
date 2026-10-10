#include "deskd/app/runtime.h"
#include <algorithm>
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/manual_book.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/rest/state.h"
#include "deskd/priv/kalshi_private.h"

namespace deskd {
using json = nlohmann::json;

bool Deskd::init_venue(std::string* err) {
  // A credential-less desk remains a local, read-only skeleton (including all week-0 tests).
  if (cfg_.key_id.empty() && cfg_.pem_path.empty()) return true;
  if (cfg_.env != "demo" || cfg_.live) {
    if (err) *err = "this rebuild stage connects only a demo credential without --live";
    return false;
  }
  signer_ = KalshiSigner::load(cfg_.key_id, cfg_.pem_path, cfg_.key_type, &key_error_);
  if (!signer_) { if (err) *err = key_error_; return false; }
  auto re = net::Endpoint::parse(options_.rest_url, err);
  auto we = net::Endpoint::parse(options_.ws_url, err);
  if (!re || !we) return false;
  factory_ = std::make_unique<net::ConnFactory>(guard_, options_.factory);
  if (!factory_->ok(err)) return false;
  md_ = std::make_unique<KalshiMd>(*we, *factory_, *signer_, feeds_.KX,
                                  [this](const MdBook& b) { on_book(b); });
  rest::Options ro;
  if (options_.rest_poll_ms > 0) ro.poll_ms = options_.rest_poll_ms;
  rest_ = std::make_unique<rest::State>(*factory_, *re, signer_.get(),
      [this](std::shared_ptr<const rest::Snapshot> s) { on_rest(std::move(s)); }, ro);
  private_ = std::make_unique<KalshiPrivate>(*we,*factory_,*signer_,feeds_.KXP,
      [this](const PrivateEvent& e) {
        // gw settles its own orders from the event; positions and cash still come from the
        // complete REST read the event asks for.
        rest_->request_refresh();
        return gw_->post(PrivateMsg{std::make_shared<const PrivateEvent>(e)});
      },[this] { rest_->request_refresh(); });
  rest_->set_reconcile_guard([this] { return private_->fence(); },
                            [this](std::uint64_t fence) { return private_->reconcile(fence); });
  // The write path: send-k1 (Manual) and send-cx (cancels and KILL), each with its own pool.
  net::PoolConfig pc;
  pc.endpoint = *re;
  pc.name = "k1";
  pool_k1_ = std::make_unique<net::SparePool>(*factory_, pc);
  pc.name = "cx";
  pool_cx_ = std::make_unique<net::SparePool>(*factory_, pc);
  auto reply = [this](KalshiWriteReply r) {
    return gw_->post(ManualWrite{std::make_shared<const KalshiWriteReply>(std::move(r))});
  };
  auto alert = [this](const std::string& msg) { pub_->alert("CRIT", "SEND", msg); };
  man_sender_ = std::make_unique<KalshiSender>(KalshiSender::Role::MANUAL, q_man_, *pool_k1_, *signer_, kill_, reply, alert);
  cx_sender_ = std::make_unique<KalshiSender>(KalshiSender::Role::CANCEL, q_mcx_, *pool_cx_, *signer_, kill_, reply, alert);
  cx_sender_->set_stats(&cx_stats_);
  attach_manual();
  return true;
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
    return h;
  };
  p.send = [this](KalshiWriteJob j) { return q_man_.push(std::move(j)); };
  p.cancel = [this](KalshiWriteJob j) { return q_mcx_.push(std::move(j), Lane::PRIORITY); };
  p.publish = [this](const std::string& row) {
    auto j = json::parse(row, nullptr, false);
    if (!j.is_discarded()) pub_->post(PubData{"ord", std::move(j)});
  };
  gw_->attach_manual(p);
}

HdrInputs Deskd::header() const {
  HdrInputs h;
  h.live = cfg_.live;
  h.run = gw_->run_view();
  h.preflight = ops_->preflight();
  h.daily_stop = cfg_.daily_stop;
  h.kx_lag_ms = feeds_.KX.view().lag_ms;
  if (!rest_) return h;
  const auto s = rest_->snapshot();
  // Read-only until the operator turns Manual orders on (deskd.manual_orders) with a write path.
  h.cash_k=rest_cash_json(*s,!(cfg_.manual_orders && man_sender_));
  h.clk_off_ms = s->clock.offset_ms;
  return h;
}

std::vector<PreflightRow> Deskd::venue_preflight() const {
  if (!rest_) return {};
  const auto s = rest_->snapshot();
  const bool fresh = s->account_ready && now_ms() - s->account_ms < 30000;
  const bool clock = s->clock.post_trusted && now_ms() - s->clock.measured_ms < 30000;
  return {{"KEY",signer_ != nullptr,signer_ ? "" : "no demo key"},
          {"CASH",fresh && s->balance.available > 0,fresh ? "" : "account reconcile pending"},
          {"SHARD",fresh && !s->balances.empty(),fresh ? "" : "shard balances stale"},
          {"CLK",clock,clock ? "" : "venue clock uncertainty exceeds 500 ms"},
          {"GROUPS",false,"Systematic groups are not enabled in this stage"}};
}

void Deskd::refresh_live() {
  if (!rest_ || !md_) return;
  const auto s = rest_->snapshot();
  std::set<std::string> selected;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    selected = pins_;
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
    if (it != s->markets.end()) pub_->post(PubData{"mkt",rest::market_json(it->second,false,false,now_ms())});
  }
  rest_->set_live(selected);
  if (selected.empty()) md_->clear_watch();
  else md_->set_watch(std::vector<std::string>(selected.begin(),selected.end()));
}

void Deskd::on_rest(std::shared_ptr<const rest::Snapshot>) {
  if (!rest_) return;
  refresh_live();
  const auto s = rest_->snapshot();
  for (const auto& tk : s->live) {
    const auto it = s->markets.find(tk);
    if (it != s->markets.end())
      pub_->post(PubData{"mkt",rest::market_json(it->second,s->pins.count(tk),true,now_ms())});
  }
  if (!s->account_ready) return;
  const auto owned = gw_->manual_cids();   // gw publishes its own orders' rows
  for (const auto& order : s->orders) {
    if (owned && owned->count(order.client_order_id)) continue;
    auto row = rest_order_json(order,now_ms());
    row["rev"] = std::max<std::uint64_t>(1,s->revision);
    pub_->post(PubData{"ord",std::move(row)});
  }
  std::vector<json> positions;
  for (const auto& p : s->positions) positions.push_back(rest_position_json(p));
  pub_->post(PubReplace{"pos",std::move(positions),"K"});
}

void Deskd::on_book(const MdBook& b) {
  // gw reads the latest book of each market, stale ones included, so a gap blocks orders.
  {
    auto next = std::make_shared<std::map<std::string, MdBook>>();
    if (auto old = std::atomic_load(&books_)) *next = *old;
    (*next)[b.ticker] = b;
    std::atomic_store(&books_, std::shared_ptr<const std::map<std::string, MdBook>>(std::move(next)));
  }
  // STALE is a venue-wide heartbeat flag; never publish a gapped reconstruction as a price.
  if (b.stale) return;
  json bids = json::array(), asks = json::array();
  for (const auto& level : b.bids) bids.push_back({price_str(level.px),qty_to_number(level.qty)});
  for (const auto& level : b.asks) asks.push_back({price_str(level.px),qty_to_number(level.qty)});
  pub_->post(PubData{"book",{{"v","K"},{"tk",b.ticker},{"depth","FULL"},
      {"bids",bids},{"asks",asks},{"trades",json::array()},{"last",nullptr},{"vol",0},
      {"ref",{{"fair",nullptr},{"sd",nullptr},{"espn",nullptr},{"espn_ts_ms",nullptr},{"game",nullptr}}},
      {"ex_ms",b.venue_ms.value_or(b.last_change_ms)}}});
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
  const auto m = rest::market_json(it->second,pin,current->live.count(tk),now_ms());
  pub_->post(PubData{"mkt",m});   // the market's row follows on the stream
  return {{"ok",true},{"pin",pin}};
}
}
