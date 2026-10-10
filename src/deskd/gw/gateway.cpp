#include "deskd/gw/gateway.h"

#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kill.h"
#include "deskd/gw/manual_book.h"
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
    // No venue path for this owner yet (Systematic arrives with its lanes). Refused, and counted,
    // so a test or a status read sees it reached gw.
    stats_.place_refused.fetch_add(1);
    const bool killed = kill_.active();
    DLOG_WARN("order %s refused: %s", p->cid.c_str(), killed ? "KILLED" : "no venue path in this stage");
    answer(p->reply, OrderReply{killed ? "KILLED" : "RO", killed ? "KILL lockout active" : "no venue path is enabled", ""});
    return;
  }
  if (auto* w = std::get_if<ManualWrite>(&m)) {
    if (w->reply) manual_reply(*w->reply);
    note_ledger();
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
    if (s->reply) s->reply->set_value(ControlReply{"", "", run_view(), 0, {}});
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
    if (r.cancel && !manual_->ledger().cancelled(r.cid, *r.cancel, "USER", r.ack_ms ? r.ack_ms : now_ms()))
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
  if (path_->cancel && path_->cancel(std::move(j))) {
    cancel_sent_.insert(cid);
    stats_.manual_cancels.fetch_add(1);
    return;
  }
  DLOG_ERR("cancel for %s not queued: the exchange expiry is the backstop; use KILL if it must go now", cid.c_str());
}

}  // namespace deskd
