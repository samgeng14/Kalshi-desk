#include "deskd/gw/gateway.h"

#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kill.h"
#include "deskd/gw/manual_book.h"
#include "deskd/gw/sys_book.h"
#include "deskd/web/contract.h"
#include "deskd/priv/kalshi_private.h"
#include "deskd/rest/state.h"

namespace deskd {

Gateway::Gateway(KillSwitch& kill, PreflightFn preflight, std::size_t normal_cap, std::size_t priority_cap)
    : kill_(kill), preflight_(std::move(preflight)), inbox_(normal_cap, priority_cap) {
  own_.state = RunState::STOP;
  own_.since_ms = now_ms();
  publish();
  kill_.on_trip([this] { inbox_.kick(); });
}

Gateway::~Gateway() = default;

void Gateway::attach_manual(const ManualPath& path) {
  path_ = std::make_unique<ManualPath>(path);
  manual_ = std::make_unique<ManualBook>();
  if (path_->sys_send) {
    sys_ = std::make_unique<SysBook>(path_->lanes.empty() ? default_lanes() : path_->lanes);
    std::atomic_store(&groups_view_, std::make_shared<const std::pair<bool, std::string>>(false, "order groups not created yet"));
  }
}

bool Gateway::post(GwMsg m) {
  const Lane lane = lane_of(m);
  return inbox_.push(std::move(m), lane);
}

void Gateway::publish() { std::atomic_store(&view_, std::make_shared<const RunView>(own_)); }

RunView Gateway::run_view() const {
  const std::int64_t killed = kill_.since_ms();
  if (killed) return RunView{RunState::KILLED, killed, kill_.rearm_ms(), kill_.why()};
  auto v = std::atomic_load(&view_);
  return v ? *v : RunView{};
}

namespace {
std::string qty_text(Qty q) { return nlohmann::json(qty_to_number(q)).dump(); }
bool own_matches(CancelIntent::Own f, Owner o) {
  return f == CancelIntent::Own::ANY || (f == CancelIntent::Own::MAN && o == Owner::MAN) ||
         (f == CancelIntent::Own::SYS && o == Owner::SYS);
}
void answer(const OrderPromise& p, OrderReply r) {
  if (!p) return;
  try { p->set_value(std::move(r)); } catch (const std::future_error&) {}   // answered already
}
// a deleted Start or new order still has a web thread waiting on it: it gets its refusal now,
// not a timeout. Nothing of a deleted order reached a venue.
void answer_erased_starts(std::vector<GwMsg>& gone, const char* code, const char* msg) {
  for (auto& g : gone) {
    auto* st = std::get_if<Start>(&g);
    if (st && st->reply) st->reply->set_value(ControlReply{code, msg, RunView{}, 0, {}});
    if (auto* p = std::get_if<PlaceIntent>(&g)) answer(p->reply, OrderReply{code, msg, ""});
  }
}
// does a cancel cover this queued new order?
bool cancel_covers(const CancelIntent& c, const PlaceIntent& p) {
  switch (c.scope) {
    case CancelIntent::Scope::CID: return p.cid == c.cid;
    case CancelIntent::Scope::MARKET:
      return p.venue == c.venue && p.ticker == c.ticker && own_matches(c.own, p.owner) &&
             (!c.has_side || p.side == c.side) && (!c.px || p.price == c.px);
    case CancelIntent::Scope::LANE: return p.owner == Owner::SYS && p.lane == c.lane;
    case CancelIntent::Scope::ALL: return own_matches(c.own, p.owner);
  }
  return false;
}
}  // namespace

void Gateway::run(Worker& self) {
  sync_kill();   // a KILL restored at start
  while (!self.stopping()) {
    auto m = inbox_.pop_for(std::chrono::milliseconds(250));
    // Before the message: a KILL that tripped while it was being popped deletes it too, if it is
    // a new order or a Start, exactly as it deletes the ones still queued.
    if (sync_kill() && m && (std::holds_alternative<PlaceIntent>(*m) || std::holds_alternative<Start>(*m))) {
      std::vector<GwMsg> gone;
      gone.push_back(std::move(*m));
      m.reset();
      stats_.erased_by_kill.fetch_add(1);
      answer_erased_starts(gone, "KILLED", "KILL overtook this start");
    }
    if (m) {
      handle(*m);
      stats_.handled.fetch_add(1, std::memory_order_relaxed);
    }
    if (sys_) sys_tick(now_ms());
    self.beat();
  }
}


bool Gateway::sync_kill() {
  const std::uint64_t g = kill_.generation(), st = kill_.state_gen();
  if (g == kill_gen_seen_ && st == kill_state_seen_) return false;
  const bool changed = st != kill_state_seen_;
  kill_gen_seen_ = g;
  kill_state_seen_ = st;
  if (kill_.active()) {
    std::vector<GwMsg> gone;
    const std::size_t n = inbox_.erase_if(Lane::NORMAL, [](const GwMsg& q) {
      return std::holds_alternative<PlaceIntent>(q) || std::holds_alternative<Start>(q);
    }, &gone);
    stats_.erased_by_kill.fetch_add(n);
    // Writes not yet on the wire never leave: the senders check the generation before each byte.
    stats_.kills_seen.fetch_add(1);
    if (changed || own_.state == RunState::RUN) {
      own_ = RunView{RunState::STOP, kill_.since_ms(), 0, ""};
      publish();
    }
    answer_erased_starts(gone, "KILLED", "KILL overtook this start");
    DLOG_WARN("KILL (%s): Systematic stopped, %zu queued orders and starts deleted", kill_.why().c_str(), n);
    return true;
  }
  // Re-armed. Later: reset every order group; GROUPS fails until order_group_updates confirms
  // them. A Start gw already answered RUN after the re-arm stands.
  if (changed && own_.state != RunState::RUN) {
    own_ = RunView{RunState::STOP, kill_.rearmed_ms(), 0, ""};
    publish();
  }
  if (changed && sys_) reset_groups();   // KILL's sender triggered them; the next Start needs them active
  return false;
}

// The first time the ledger stops, its reason is logged and kept for /ops/status.
void Gateway::note_ledger() {
  if (!manual_ || manual_->ledger().healthy() || std::atomic_load(&ledger_why_)) return;
  const auto& why = manual_->ledger().why_unhealthy();
  DLOG_ERR("Manual order ledger stopped (orders refused RO until deskd restarts): %s", why.c_str());
  std::atomic_store(&ledger_why_, std::make_shared<const std::string>(why));
}

void Gateway::handle(GwMsg& m) {
  if (auto* p = std::get_if<PlaceIntent>(&m)) {
    if (manual_ && p->owner == Owner::MAN) return place_manual(*p);
    if (manual_ && sys_ && p->owner == Owner::SYS) return place_sys(*p);
    // No venue path for this owner yet (Systematic arrives with its lanes). Refused, and counted,
    // so a test or a status read sees it reached gw.
    stats_.place_refused.fetch_add(1);
    const bool killed = kill_.active();
    DLOG_WARN("order %s refused: %s", p->cid.c_str(), killed ? "KILLED" : "no venue path in this stage");
    answer(p->reply, OrderReply{killed ? "KILLED" : "RO", killed ? "KILL lockout active" : "no venue path is enabled", ""});
    return;
  }
  if (auto* w = std::get_if<ManualWrite>(&m)) {
    if (w->reply && w->reply->op == WireOp::GROUP) group_reply(*w->reply);
    else if (w->reply) manual_reply(*w->reply);
    note_ledger();
    return;
  }
  if (auto* l = std::get_if<LaneSet>(&m)) {
    LaneReply r;
    if (!sys_ || !sys_->lane(l->id)) {
      r.code = "NOT_FOUND";
      r.msg = "no lane " + l->id;
    } else {
      sys_->set_on(l->id, l->on);
      // off cancels that lane's working orders (contract: why LANE_OFF)
      if (!l->on) { const auto id = l->id; r.n = cancel_sys([&id](const std::string& lane) { return lane == id; }, "LANE_OFF"); }
      DLOG_INFO("lane %s %s", l->id.c_str(), l->on ? "on" : "off");
      lanes_dirty_.insert(l->id);
      sys_dirty_ = true;
      r.lane = lane_row(l->id);
    }
    if (l->reply) { try { l->reply->set_value(std::move(r)); } catch (const std::future_error&) {} }
    return;
  }
  if (auto* e = std::get_if<PrivateMsg>(&m)) {
    if (e->event && manual_) manual_private(*e->event);
    note_ledger();
    return;
  }
  if (auto* c = std::get_if<CancelIntent>(&m)) {
    const CancelIntent cx = *c;
    std::vector<GwMsg> gone;
    const std::size_t n = inbox_.erase_if(Lane::NORMAL, [&cx](const GwMsg& q) {
      const auto* p = std::get_if<PlaceIntent>(&q);
      return p && cancel_covers(cx, *p);
    }, &gone);
    stats_.erased_by_cancel.fetch_add(n);
    answer_erased_starts(gone, "STALE", "a cancel overtook this order before it was sent");
    CancelReply r;
    if (cx.scope == CancelIntent::Scope::MARKET && cx.venue == Venue::K) {
      // contract: a market filter naming an unknown market is NOT_FOUND
      const auto snapshot = manual_ && path_->snapshot ? path_->snapshot() : nullptr;
      if (!snapshot || !snapshot->markets.count(cx.ticker)) {
        r.code = "NOT_FOUND";
        r.msg = "no such market: " + cx.ticker;
      }
    }
    for (const auto& g : gone) r.cids.push_back(std::get<PlaceIntent>(g).cid);
    for (auto& cid : cancel_manual(cx)) r.cids.push_back(std::move(cid));
    if (c->reply) {
      try { c->reply->set_value(std::move(r)); } catch (const std::future_error&) {}
    }
    return;
  }
  if (auto* s = std::get_if<Stop>(&m)) {
    std::vector<GwMsg> gone;
    const std::size_t n = inbox_.erase_if(Lane::NORMAL, [](const GwMsg& q) {
      const auto* p = std::get_if<PlaceIntent>(&q);
      return std::holds_alternative<Start>(q) || (p && p->owner == Owner::SYS);
    }, &gone);
    stats_.erased_by_stop.fetch_add(n);
    if (own_.state != RunState::HALTED) own_ = RunView{RunState::STOP, now_ms(), 0, s->why.empty() ? "operator" : s->why};
    publish();
    answer_erased_starts(gone, "BAD_STATE", "a Stop overtook this start");
    // Systematic's working orders, and its groups (the venue cancels whatever is in them);
    // never a cancel-all, which could cancel the next Manual order
    int cancelled = 0;
    if (sys_) {
      cancelled = cancel_sys([](const std::string&) { return true; }, "STOP");
      trigger_groups();
      for (const auto& l : sys_->lanes()) lanes_dirty_.insert(l.id);
      sys_dirty_ = true;
    }
    if (s->reply) s->reply->set_value(ControlReply{"", "", run_view(), cancelled, {}});
    return;
  }
  if (auto* st = std::get_if<Start>(&m)) {
    ControlReply r;
    if (kill_.active()) {
      r.code = "KILLED";
      r.msg = "KILL lockout active; re-arm first";
    } else if (own_.state == RunState::HALTED) {
      r.code = "HALTED";
      r.msg = "Systematic halted by the daily stop";
    } else if (own_.state != RunState::RUN) {
      r.preflight = preflight_ ? preflight_() : std::vector<PreflightRow>{};
      std::string failing;
      for (const auto& row : r.preflight)
        if (!row.ok && row.id != "DISK") failing += (failing.empty() ? "" : " ") + row.id;   // DISK only warns
      if (r.preflight.empty()) failing = "no preflight";
      if (!failing.empty()) {
        r.code = "PREFLIGHT";
        r.msg = "preflight fails: " + failing;
      } else {
        own_ = RunView{RunState::RUN, now_ms(), 0, ""};
        publish();
        r.preflight.clear();
      }
    }
    r.run = run_view();
    if (st->reply) st->reply->set_value(std::move(r));
    return;
  }
  if (auto* pr = std::get_if<Probe>(&m)) {
    if (pr->fn) pr->fn();
    return;
  }
  // AmendIntent, SendResult, OrderEvent: not used by the Manual path yet (amends are not
  // offered; private events reach gw through the REST reconcile until the ledger takes them).
}

// ---- the Manual path ------------------------------------------------------------------------

std::string Gateway::order_json(const std::string& cid) const {
  const auto* o = manual_ ? manual_->ledger().find(cid) : nullptr;
  return o ? o->json().dump() : std::string();
}

void Gateway::publish_order(const std::string& cid) {
  if (!cid.empty() && cid.front() == 'S') sys_dirty_ = true;
  if (!path_->publish) return;
  const auto row = order_json(cid);
  if (!row.empty()) path_->publish(row);
}

void Gateway::place_manual(PlaceIntent& p) {
  auto snapshot = path_->snapshot ? path_->snapshot() : nullptr;
  if (snapshot != snapshot_seen_) {
    manual_->set_snapshot(snapshot);
    snapshot_seen_ = std::move(snapshot);
  }
  if (path_->book) {
    if (auto b = path_->book(p.ticker)) manual_->set_book(*b);
  }
  ManualHealth h = path_->health ? path_->health() : ManualHealth{};
  h.killed = kill_.active();
  h.kill_generation = kill_.generation();
  auto placed = manual_->place(p, h, now_ms(), mono_ms());
  const auto& a = placed.admission;
  if (!a.ok()) {
    stats_.place_refused.fetch_add(1);
    DLOG_INFO("order %s refused: %s (%s)", p.cid.c_str(), a.code.c_str(), a.message.c_str());
    answer(p.reply, OrderReply{a.code, a.message, ""});
    return;
  }
  const std::string cid = a.intent.cid;
  {
    auto owned = std::make_shared<std::set<std::string>>();
    if (auto old = std::atomic_load(&owned_)) *owned = *old;
    owned->insert(cid);
    std::atomic_store(&owned_, std::shared_ptr<const std::set<std::string>>(std::move(owned)));
  }
  publish_order(cid);   // PEND, with its reservation
  if (!path_->send || !path_->send(std::move(*placed.job))) {
    KalshiWriteReply r;
    r.cid = cid;
    r.op = WireOp::CREATE;
    r.entered_ms = now_ms();
    r.local_code = "RO";
    r.outcome.kind = kalshi::WriteOutcome::Kind::NOT_SENT;
    manual_->apply(r);
    publish_order(cid);
    DLOG_ERR("order %s not sent: send-k1's inbox is full", cid.c_str());
    answer(p.reply, OrderReply{"RO", "the Manual sender's queue is full", order_json(cid)});
    return;
  }
  stats_.manual_sent.fetch_add(1);
  if (p.reply) pending_[cid] = p.reply;
}

void Gateway::manual_reply(const KalshiWriteReply& r) {
  if (!manual_) return;
  using K = kalshi::WriteOutcome::Kind;
  if (r.op == WireOp::CANCEL) {
    const auto why = cancel_why_.find(r.cid);
    if (r.cancel && !manual_->ledger().cancelled(r.cid, *r.cancel, why == cancel_why_.end() ? "USER" : why->second,
                                                 r.ack_ms ? r.ack_ms : now_ms()))
      DLOG_ERR("cancel reply for %s contradicts the order book: reconcile before trading", r.cid.c_str());
    publish_order(r.cid);
    return;   // a cancel that did not land leaves the order working until an order read or event says more
  }
  if (r.op != WireOp::CREATE || r.cid.empty()) return;   // cancel-all (KILL) is reconciled from the venue
  const bool ok = manual_->apply(r);
  if (!ok) DLOG_ERR("reply for order %s contradicts the order book: reconcile before trading", r.cid.c_str());
  publish_order(r.cid);
  // A cancel asked for while the write was in flight goes out once the order id is known.
  if (const auto* o = manual_->ledger().find(r.cid))
    if (o->cancel_requested && !o->terminal() && !o->oid.empty() && !cancel_sent_.count(r.cid)) send_cancel(r.cid);
  const auto it = pending_.find(r.cid);
  if (it == pending_.end()) return;
  OrderReply out;
  out.ord = order_json(r.cid);
  if (!ok) {
    out.code = "INTERNAL";
    out.msg = "the venue's answer contradicts the order book; reconciliation required";
  } else if (r.outcome.kind == K::ACK && r.create) {
    // accepted: WORK, PART, FILL or CXL (IOC/FOK remainder) are in ord
  } else if (r.outcome.kind == K::ACK || r.outcome.kind == K::UNKNOWN) {
    out.code = "EXCHANGE";
    out.msg = "the venue did not confirm this order in time; the private feed will settle it";
  } else {
    const auto code = ManualBook::refusal_code(r);
    if (code != "POST_CROSS" && code != "STP" && code != "USER") {
      out.code = code;
      out.msg = r.outcome.why.empty() ? code : r.outcome.why.substr(0, 200);
    }
  }
  answer(it->second, std::move(out));
  pending_.erase(it);
}

// The private feed's order updates and fills, for the orders gw placed. Positions themselves come
// from the REST revision each event asks for; here they settle UNKNOWN, resting, expired and
// cancelled orders, and dedupe executions by trade id. Foreign orders and fills are not ours.
void Gateway::manual_private(const PrivateEvent& e) {
  auto& ledger = manual_->ledger();
  const bool healthy = ledger.healthy();
  const std::int64_t at = e.received_ms ? e.received_ms : now_ms();
  std::string cid;
  if (e.kind == PrivateEvent::Kind::ORDER) {
    if (!ledger.observed(e.order, at)) {
      if (healthy && !ledger.healthy())
        DLOG_ERR("private order update for %s contradicts the order book: reconcile before trading", e.order.order_id.c_str());
      return;
    }
    const auto* o = ledger.find(e.order.client_order_id);
    cid = o ? o->intent.cid : e.cid;
  } else {
    const auto r = ledger.fill(e.fill, e.cid, at);
    if (r == OrderLedger::FillResult::INVALID) {
      DLOG_ERR("private fill %s contradicts the order book: reconcile before trading", e.fill.trade_id.c_str());
      return;
    }
    if (r != OrderLedger::FillResult::APPLIED) return;
    cid = e.cid;
    if (sys_) {
      const OrderLedger::Order* o = cid.empty() ? nullptr : ledger.find(cid);
      if (!o) for (const auto& [c, x] : ledger.orders()) if (x.oid == e.fill.order_id) o = &x;
      if (o && o->intent.owner == Owner::SYS) {
        sys_->fill(o->intent.lane, o->intent.cid, e.fill.ticker,
                   e.fill.side == kalshi::BookSide::BID ? Side::BUY : Side::SELL, e.fill.count, e.fill.yes_price, e.fill.fee, at);
        lanes_dirty_.insert(o->intent.lane);
      }
    }
  }
  stats_.private_applied.fetch_add(1);
  if (cid.empty()) {
    for (const auto& [c, o] : ledger.orders())
      if (o.oid == (e.kind == PrivateEvent::Kind::ORDER ? e.order.order_id : e.fill.order_id)) cid = c;
  }
  if (!cid.empty()) {
    publish_order(cid);
    if (const auto* o = ledger.find(cid))
      if (o->cancel_requested && !o->terminal() && !o->oid.empty() && !cancel_sent_.count(cid)) send_cancel(cid);
  }
}

std::vector<std::string> Gateway::cancel_manual(const CancelIntent& c) {
  std::vector<std::string> cids;
  if (!manual_) return cids;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (!o.terminal() && cancel_covers(c, o.intent)) cids.push_back(cid);
  for (const auto& cid : cids) {
    manual_->revoke(cid);   // a write not yet on the wire never leaves
    manual_->ledger().request_cancel(cid);
    const auto* o = manual_->ledger().find(cid);
    if (o && !o->oid.empty()) send_cancel(cid);
    publish_order(cid);
  }
  return cids;
}

void Gateway::send_cancel(const std::string& cid) {
  const auto* o = manual_->ledger().find(cid);
  if (!o || o->oid.empty()) return;
  KalshiWriteJob j;
  j.kind = KalshiWriteJob::Kind::CANCEL;
  j.cid = cid;
  j.oid = o->oid;
  j.ticker = o->intent.ticker;
  j.shard = o->shard;
  const bool sys = o->intent.owner == Owner::SYS;
  const auto& via = sys ? path_->sys_cancel : path_->cancel;   // Systematic never uses send-cx
  if (via && via(std::move(j))) {
    cancel_sent_.insert(cid);
    (sys ? stats_.sys_cancels : stats_.manual_cancels).fetch_add(1);
    return;
  }
  DLOG_ERR("cancel for %s not queued: the exchange expiry is the backstop; use KILL if it must go now", cid.c_str());
}

// ---- Systematic ----------------------------------------------------------------------------

void Gateway::sync_snapshot() {
  auto snapshot = path_->snapshot ? path_->snapshot() : nullptr;
  if (snapshot != snapshot_seen_) {
    manual_->set_snapshot(snapshot);
    snapshot_seen_ = std::move(snapshot);
  }
}

std::set<int> Gateway::shards() const {
  std::set<int> out;
  if (snapshot_seen_) for (const auto& s : snapshot_seen_->exchange.shards) out.insert(s.exchange_index);
  if (out.empty()) out.insert(0);
  return out;
}

std::optional<Px> Gateway::mark(const std::string& ticker) const {
  if (!path_->book) return std::nullopt;
  const auto b = path_->book(ticker);
  if (!b || b->stale || b->bids.empty() || b->asks.empty()) return std::nullopt;
  return (b->bids.front().px + b->asks.front().px) / 2;
}

std::string Gateway::lane_row(const std::string& id) const {
  const auto* l = sys_->lane(id);
  if (!l) return "";
  Micros reserved = 0, used_all = 0;
  int working = 0;
  for (const auto& [cid, o] : manual_->ledger().orders()) {
    if (o.intent.owner != Owner::SYS) continue;
    if (!o.terminal() && o.intent.lane == id) { reserved += o.reservation(); ++working; }
    used_all += o.reservation();
  }
  for (const auto& x : sys_->lanes()) used_all += sys_->open_worst(x.id);
  // the effective Systematic ceiling (contract hdr.sys): min(70% of equity, used + free)
  Micros eff = 0;
  if (snapshot_seen_) {
    const Micros equity = (snapshot_seen_->balance.available + snapshot_seen_->balance.portfolio_value.value_or(0)) * 100;
    eff = std::min<Micros>(equity * 7 / 10, used_all + manual_->venue_free());
  }
  std::string blocked;
  if (sys_->on(id) && own_.state == RunState::RUN)
    for (const int shard : shards()) {
      const auto* g = sys_->find_group(id, shard);
      if (!g || g->state != SysBook::GroupState::ACTIVE) { blocked = "order group on shard " + std::to_string(shard) + " not active"; break; }
    }
  const auto mk = [this](const std::string& tk) { return mark(tk); };
  return sys_->lane_json(*l, run_view().state, reserved, working, eff - used_all, mk, blocked).dump();
}

void Gateway::place_sys(PlaceIntent& p) {
  auto refuse = [&](const char* code, const std::string& msg) {
    stats_.sys_refused.fetch_add(1);
    DLOG_INFO("Systematic %s refused: %s (%s)", p.cid.c_str(), code, msg.c_str());
    answer(p.reply, OrderReply{code, msg, ""});
  };
  if (kill_.active()) return refuse("KILLED", "KILL lockout active");
  if (own_.state == RunState::HALTED) return refuse("HALTED", "Systematic halted by the daily stop");
  if (own_.state != RunState::RUN) return refuse("BAD_STATE", "a Systematic order while Systematic is not running");
  const auto* lane = sys_->lane(p.lane);
  if (!lane) return refuse("BAD_REQUEST", "no lane " + p.lane);
  if (!sys_->on(p.lane)) return refuse("BAD_STATE", "lane " + p.lane + " is off");
  sync_snapshot();
  if (!snapshot_seen_) return refuse("STALE", "no account read yet");
  const auto m = snapshot_seen_->markets.find(p.ticker);
  if (m == snapshot_seen_->markets.end() || !m->second.exchange_index) return refuse("UNKNOWN_MARKET", "no Kalshi market " + p.ticker);
  if (!lane->series.empty() &&
      std::none_of(lane->series.begin(), lane->series.end(), [&](const std::string& s) {
        return p.ticker.size() > s.size() && p.ticker.compare(0, s.size(), s) == 0 && p.ticker[s.size()] == '-';
      }))
    return refuse("BAD_REQUEST", "lane " + p.lane + " does not trade " + p.ticker);
  const int shard = *m->second.exchange_index;
  const auto* group = sys_->find_group(p.lane, shard);
  if (!group || group->state != SysBook::GroupState::ACTIVE)
    return refuse("RO", "lane " + p.lane + "'s order group on shard " + std::to_string(shard) + " is not active");
  // Inventory (plan section 4): Systematic's net position per market, with its working orders on
  // the same side, may not grow past the lane's limit.
  Qty pos = 0, pending = 0;
  for (const auto& l : sys_->lanes()) pos += sys_->position(l.id, p.ticker);
  Micros reserved_lane = 0, used_all = 0;
  for (const auto& [cid, o] : manual_->ledger().orders()) {
    if (o.intent.owner != Owner::SYS) continue;
    used_all += o.reservation();
    if (o.terminal()) continue;
    if (o.intent.lane == p.lane) reserved_lane += o.reservation();
    if (o.intent.ticker == p.ticker && o.intent.side == p.side) pending += o.left;
  }
  const Qty grow = (p.side == Side::BUY ? 1 : -1) * (pending + p.count);
  if (std::abs(pos + grow) > lane->inventory && std::abs(pos + grow) > std::abs(pos))
    return refuse("SYS_CEILING", "inventory: " + qty_text(pos) + " held, " + qty_text(pending) + " working, limit " + qty_text(lane->inventory));
  // the lane's budget and the venue's 70% ceiling, at this order's worst case
  const Micros worst = (p.side == Side::BUY ? p.price : kOneDollar - p.price) * p.count;
  for (const auto& l : sys_->lanes()) used_all += sys_->open_worst(l.id);
  if (sys_->open_worst(p.lane) + reserved_lane + worst > lane->budget * 100)
    return refuse("SYS_CEILING", "lane " + p.lane + " budget " + money_str(lane->budget));
  const Micros equity = (snapshot_seen_->balance.available + snapshot_seen_->balance.portfolio_value.value_or(0)) * 100;
  const Micros eff = std::min<Micros>(equity * 7 / 10, used_all + manual_->venue_free());
  if (used_all + worst > eff) return refuse("SYS_CEILING", "past the effective Systematic ceiling " + money_str(eff / 100));
  // the daily loss stop (decision 1): Systematic's day, marked at the mids
  const auto mk = [this](const std::string& tk) { return mark(tk); };
  if (path_->daily_stop > 0 && sys_->day_total(mk) <= -path_->daily_stop * 100) {
    halt("daily stop");
    return refuse("HALTED", "Systematic halted by the daily stop");
  }
  if (path_->book) {
    if (auto b = path_->book(p.ticker)) manual_->set_book(*b);
  }
  ManualHealth h = path_->sys_health ? path_->sys_health() : ManualHealth{};
  h.killed = kill_.active();
  h.kill_generation = kill_.generation();
  auto placed = manual_->place(p, h, now_ms(), mono_ms());
  const auto& a = placed.admission;
  if (!a.ok()) return refuse(a.code.c_str(), a.message);
  const std::string cid = a.intent.cid;
  {
    auto owned = std::make_shared<std::set<std::string>>();
    if (auto old = std::atomic_load(&owned_)) *owned = *old;
    owned->insert(cid);
    std::atomic_store(&owned_, std::shared_ptr<const std::set<std::string>>(std::move(owned)));
  }
  placed.job->create.order_group_id = group->id;
  publish_order(cid);
  if (!path_->sys_send(std::move(*placed.job))) {
    KalshiWriteReply r;
    r.cid = cid;
    r.op = WireOp::CREATE;
    r.entered_ms = now_ms();
    r.local_code = "RO";
    r.outcome.kind = kalshi::WriteOutcome::Kind::NOT_SENT;
    manual_->apply(r);
    publish_order(cid);
    return refuse("RO", "send-k2's queue is full");
  }
  stats_.sys_sent.fetch_add(1);
  sys_->sent(p.lane, now_ms());
  lanes_dirty_.insert(p.lane);
  if (p.reply) pending_[cid] = p.reply;
}

int Gateway::cancel_sys(const std::function<bool(const std::string& lane)>& which, const char* why) {
  std::vector<std::string> cids;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (o.intent.owner == Owner::SYS && !o.terminal() && which(o.intent.lane)) cids.push_back(cid);
  for (const auto& cid : cids) {
    cancel_why_[cid] = why;
    manual_->revoke(cid);   // a write not yet on the wire never leaves
    manual_->ledger().request_cancel(cid);
    const auto* o = manual_->ledger().find(cid);
    if (o && !o->oid.empty() && !cancel_sent_.count(cid)) send_cancel(cid);
    publish_order(cid);
  }
  return static_cast<int>(cids.size());
}

void Gateway::group_job(const std::string& lane, int shard, const std::string& action) {
  auto& g = sys_->group(lane, shard);
  KalshiWriteJob j;
  j.kind = KalshiWriteJob::Kind::GROUP;
  j.group_action = action;
  j.group_id = g.id;
  j.lane = lane;
  j.shard = shard;
  const auto* l = sys_->lane(lane);
  j.group_limit = l ? l->group_limit : 5000;
  j.cid = "G" + lane + "-" + std::to_string(shard) + "-" + action;
  j.kill_generation = kill_.generation();
  if (!path_->sys_send(std::move(j))) {
    DLOG_WARN("order group %s %s on shard %d not queued: send-k2's queue is full", action.c_str(), lane.c_str(), shard);
    return;
  }
  stats_.group_jobs.fetch_add(1);
  g.asked_ms = now_ms();
  g.state = action == "create" ? SysBook::GroupState::CREATING
          : action == "trigger" ? SysBook::GroupState::TRIGGERING : SysBook::GroupState::RESETTING;
}

void Gateway::group_reply(const KalshiWriteReply& r) {
  using K = kalshi::WriteOutcome::Kind;
  const bool ok = r.outcome.kind == K::ACK;
  const bool gone = r.outcome.kind == K::REFUSED && r.outcome.cls == RejectClass::GONE;
  SysBook::Group* g = nullptr;
  std::string lane;
  for (auto& [key, x] : sys_->groups())
    if ((r.group_action == "create" && key.first == r.lane && key.second == r.shard) ||
        (r.group_action != "create" && !r.group_id.empty() && x.id == r.group_id)) { g = &x; lane = key.first; }
  if (!g) return;
  if (r.group_action == "create") {
    if (ok) {
      g->id = r.group_id;
      g->state = SysBook::GroupState::ACTIVE;
      if (path_->groups) path_->groups->add(g->id, r.shard);
      DLOG_INFO("order group %s for %s on shard %d", g->id.c_str(), lane.c_str(), r.shard);
    } else {
      g->state = SysBook::GroupState::NONE;   // asked again after the pause in sys_tick
      DLOG_WARN("order group for %s on shard %d not created: %s", lane.c_str(), r.shard,
                (r.outcome.why.empty() ? r.local_code : r.outcome.why).c_str());
    }
  } else if (r.group_action == "trigger") {
    if (ok) g->state = SysBook::GroupState::TRIGGERED;
    else if (gone) { if (path_->groups) path_->groups->remove(g->id); *g = SysBook::Group{}; }
    else DLOG_WARN("order group %s not triggered: %s", g->id.c_str(), r.outcome.why.c_str());
  } else if (r.group_action == "reset") {
    if (ok) g->state = SysBook::GroupState::ACTIVE;
    else if (gone) { if (path_->groups) path_->groups->remove(g->id); *g = SysBook::Group{}; }
    else { g->state = SysBook::GroupState::TRIGGERED; DLOG_WARN("order group %s not reset: %s", g->id.c_str(), r.outcome.why.c_str()); }
  }
  lanes_dirty_.insert(lane);
}

void Gateway::trigger_groups() {
  std::vector<std::pair<std::string, int>> keys;
  for (const auto& [key, g] : sys_->groups())
    if (!g.id.empty() && (g.state == SysBook::GroupState::ACTIVE || g.state == SysBook::GroupState::RESETTING)) keys.push_back(key);
  for (const auto& [lane, shard] : keys) group_job(lane, shard, "trigger");
}

void Gateway::reset_groups() {
  std::vector<std::pair<std::string, int>> keys;
  for (const auto& [key, g] : sys_->groups())
    if (!g.id.empty() && g.state != SysBook::GroupState::RESETTING) keys.push_back(key);
  for (const auto& [lane, shard] : keys) group_job(lane, shard, "reset");
}

void Gateway::halt(const std::string& why) {
  if (own_.state == RunState::HALTED) return;
  DLOG_WARN("Systematic HALTED: %s", why.c_str());
  own_ = RunView{RunState::HALTED, now_ms(), 0, why};
  publish();
  cancel_sys([](const std::string&) { return true; }, "HALT");
  trigger_groups();
  for (const auto& l : sys_->lanes()) lanes_dirty_.insert(l.id);
  sys_dirty_ = true;
}

void Gateway::sys_tick(std::int64_t now) {
  if (now - sys_view_ms_ < 100) return;
  sync_snapshot();
  // a new ET day: the lanes' counters start again, and a daily-stop HALT ends
  const auto midnight = et_midnight_ms(now);
  if (sys_->day(sys_->lanes().front().id) == nullptr || sys_->day(sys_->lanes().front().id)->day_start_ms < midnight) {
    sys_->roll(midnight);
    if (own_.state == RunState::HALTED && own_.since_ms < midnight) {
      own_ = RunView{RunState::STOP, now, 0, "new day"};
      publish();
    }
    for (const auto& l : sys_->lanes()) lanes_dirty_.insert(l.id);
  }
  const auto mk = [this](const std::string& tk) { return mark(tk); };
  if (own_.state == RunState::RUN && path_->daily_stop > 0 && sys_->day_total(mk) <= -path_->daily_stop * 100) halt("daily stop");
  // Order groups: one per lane and shard, created one at a time while Systematic can write;
  // triggered ones are reset once nothing runs (after a Stop or HALT; KILL's at its re-arm).
  const bool can_write = !kill_.active() && (!path_->sys_health || path_->sys_health().manual_sender);
  if (can_write && now - groups_paced_ms_ >= 250) {
    for (const auto& l : sys_->lanes()) {
      bool asked = false;
      for (const int shard : shards()) {
        auto& g = sys_->group(l.id, shard);
        const bool stuck = g.state != SysBook::GroupState::NONE && g.state != SysBook::GroupState::ACTIVE &&
                           g.state != SysBook::GroupState::TRIGGERED && now - g.asked_ms > 15000;
        if (stuck) {   // a reply that never came: ask again (a create may leave an empty group behind)
          DLOG_WARN("order group for %s on shard %d: no reply in 15 s, asking again", l.id.c_str(), shard);
          g.state = g.id.empty() ? SysBook::GroupState::NONE : SysBook::GroupState::TRIGGERED;
        }
        if (g.state == SysBook::GroupState::NONE && now - g.asked_ms > 5000) { group_job(l.id, shard, "create"); asked = true; }
        else if (g.state == SysBook::GroupState::TRIGGERED && own_.state != RunState::RUN && now - g.asked_ms > 1000) {
          group_job(l.id, shard, "reset");
          asked = true;
        }
        if (asked) break;
      }
      if (asked) { groups_paced_ms_ = now; break; }
    }
  }
  std::string why;
  const bool ready = sys_->groups_ready(shards(), &why);
  const auto old = std::atomic_load(&groups_view_);
  if (!old || old->first != ready || old->second != why)
    std::atomic_store(&groups_view_, std::make_shared<const std::pair<bool, std::string>>(ready, why));
  // lane rows: a changed one at most twice a second, every one every 5 s (their marks move)
  for (const auto& l : sys_->lanes()) {
    auto& last = lane_published_[l.id];
    const bool due = (lanes_dirty_.count(l.id) && now - last >= 500) || now - last >= 5000;
    if (!due || !path_->publish_lane) continue;
    path_->publish_lane(lane_row(l.id));
    last = now;
    lanes_dirty_.erase(l.id);
  }
  // strat's view
  if (sys_dirty_ || now - sys_view_ms_ >= 250) {
    auto v = std::make_shared<SysView>();
    v->run = run_view().state;
    v->lanes_on = sys_->on_lanes();
    for (const auto& [cid, o] : manual_->ledger().orders()) {
      if (o.intent.owner != Owner::SYS || (o.terminal() && now - o.done_ms > 120000)) continue;
      SysOrder s;
      s.cid = cid; s.lane = o.intent.lane; s.ticker = o.intent.ticker; s.side = o.intent.side; s.price = o.intent.price;
      s.count = o.intent.count; s.left = o.left; s.filled = o.filled; s.state = o.state; s.terminal = o.terminal();
      s.cancel_requested = o.cancel_requested; s.done_ms = o.done_ms;
      v->orders[cid] = std::move(s);
    }
    for (const auto& l : sys_->lanes())
      if (const auto* d = sys_->day(l.id))
        for (const auto& [tk, inv] : d->inv) if (inv.pos) v->positions[l.id + "|" + tk] = inv.pos;
    v->ts_ms = now;
    std::atomic_store(&sys_view_, std::shared_ptr<const SysView>(std::move(v)));
    sys_dirty_ = false;
    sys_view_ms_ = now;
  }
}

}  // namespace deskd
