#include "deskd/app/runtime.h"
#include <algorithm>
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/gw/kalshi_sign.h"
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
  rest_ = std::make_unique<rest::State>(*factory_, *re, signer_.get(),
      [this](std::shared_ptr<const rest::Snapshot> s) { on_rest(std::move(s)); });
  private_ = std::make_unique<KalshiPrivate>(*we,*factory_,*signer_,feeds_.KXP,
      [this](const PrivateEvent&) {
        // Until the Manual ledger owns incremental fills, reconcile instead of pretending
        // a queued update has already been applied to positions and reservations.
        rest_->request_refresh();
        return false;
      },[this] { rest_->request_refresh(); });
  rest_->set_reconcile_guard([this] { return private_->fence(); },
                            [this](std::uint64_t fence) { return private_->reconcile(fence); });
  return true;
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
  std::map<int, Money> reserved;
  Money total_reserved = 0;
  bool unknown_shard = false;
  for (const auto& o : s->orders) if (o.status == kalshi::OrderStatus::RESTING) {
    const auto p = o.side == kalshi::BookSide::BID ? o.yes_price : kOneDollar-o.yes_price;
    const Money cost = (p * o.remaining + 99) / 100;
    total_reserved += cost;
    if (o.exchange_index) reserved[*o.exchange_index] += cost; else unknown_shard = true;
  }
  json shards = json::array();
  for (const auto& b : s->balances)
    shards.push_back({{"id","S" + std::to_string(b.first)},
                      {"cash",money_str(b.second)},
                      {"free",money_str(unknown_shard ? 0 : std::max<Money>(0,b.second-reserved[b.first]))}});
  h.cash_k = {{"total",money_str(s->balance.available)},
              {"free",money_str(std::max<Money>(0,s->balance.available-total_reserved))},
              // Venue data is connected, but the Manual write/ledger path is not enabled yet.
              {"ro",true},{"shards",shards}};
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
  for (const auto& order : s->orders) {
    auto row = rest_order_json(order,now_ms());
    row["rev"] = std::max<std::uint64_t>(1,s->revision);
    pub_->post(PubData{"ord",std::move(row)});
  }
  std::vector<json> positions;
  for (const auto& p : s->positions) positions.push_back(rest_position_json(p));
  pub_->post(PubReplace{"pos",std::move(positions),"K"});
}

void Deskd::on_book(const MdBook& b) {
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
  const bool pin = request.at("pin").get<bool>();
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
  pub_->post(PubData{"mkt",m});
  return {{"ok",true},{"mkt",m}};
}
}
