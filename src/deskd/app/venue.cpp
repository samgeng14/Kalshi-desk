#include "deskd/app/runtime.h"
#include <algorithm>
#include "deskd/app/log.h"
#include "deskd/app/projection.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/manual_book.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/rest/state.h"
#include "deskd/priv/kalshi_private.h"
#include "deskd/ref/pyth.h"
#include "deskd/strat/fair.h"
#include "deskd/ref/espn.h"

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
  md_->set_trade_callback([this](const MdTrade& t) { on_trade(t); });
  rest::Options ro;
  if (options_.rest_poll_ms > 0) ro.poll_ms = options_.rest_poll_ms;
  if (options_.rest_gap_ms >= 0) ro.request_gap_ms = options_.rest_gap_ms;
  rest_ = std::make_unique<rest::State>(*factory_, *re, signer_.get(),
      [this](std::shared_ptr<const rest::Snapshot> s) { on_rest(std::move(s)); }, ro);
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
  md_->set_idle_callback([this] { republish_refs(); });
  if (!options_.espn_url.empty()) {
    std::string why;
    if (auto ep = net::Endpoint::parse(options_.espn_url, &why)) {
      // the watched game markets, with the other team's market of each game
      espn_ = std::make_unique<EspnFeed>(*ep, [this] {
        std::vector<kalshi::MarketRow> out;
        const auto s = rest_->snapshot();
        std::set<std::string> events;
        for (const auto& tk : s->live) {
          const auto it = s->markets.find(tk);
          if (it != s->markets.end() && espn_league_for(tk)) events.insert(it->second.event_ticker);
        }
        for (const auto& [tk, m] : s->markets) if (events.count(m.event_ticker)) out.push_back(m);
        return out;
      });
    } else {
      DLOG_WARN("ESPN off: %s", why.c_str());
    }
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
    p.publish_lane = [this](const std::string& row) {
      auto j = json::parse(row, nullptr, false);
      if (!j.is_discarded()) pub_->post(PubData{"lane", std::move(j)});
    };
    p.daily_stop = cfg_.daily_stop;
    p.groups = &groups_;
  }
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
          groups_row()};
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
  // Today's fills (new ones only; each is keyed by fid) and today's P&L per market.
  auto day = std::make_shared<DayPnl>(day_pnl(*s, et_midnight_ms(now_ms()), [this](const std::string& tk) { return mid(tk); }));
  if (day->unknown_basis)
    DLOG_WARN("%zu of today's fills reduce a position whose cost before today is not recoverable: they count only their fee",
              day->unknown_basis);
  for (const auto& f : day->fills) {
    const auto fid = f.at("fid").get<std::string>();
    if (fills_sent_.insert(fid).second) pub_->post(PubData{"fill", f});
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
}

std::optional<Px> Deskd::mid(const std::string& ticker) const {
  const auto books = std::atomic_load(&books_);
  if (!books) return std::nullopt;
  const auto it = books->find(ticker);
  if (it == books->end() || it->second.stale || it->second.bids.empty() || it->second.asks.empty()) return std::nullopt;
  return (it->second.bids.front().px + it->second.asks.front().px) / 2;
}

void Deskd::on_trade(const MdTrade& t) {
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
  // gw reads the latest book of each market, stale ones included, so a gap blocks orders.
  {
    auto next = std::make_shared<std::map<std::string, MdBook>>();
    if (auto old = std::atomic_load(&books_)) *next = *old;
    (*next)[b.ticker] = b;
    std::atomic_store(&books_, std::shared_ptr<const std::map<std::string, MdBook>>(std::move(next)));
  }
  md_last_[b.ticker] = b;
  // STALE is a venue-wide heartbeat flag; never publish a gapped reconstruction as a price.
  if (b.stale) return;
  publish_book(b.ticker);
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
