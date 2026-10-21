#include "deskd/gw/gateway.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "deskd/app/log.h"
#include "deskd/app/projection.h"
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

Gateway::~Gateway() {
  if (sys_ && sys_restore_error_.empty()) save_sys();
}

void Gateway::attach_manual(const ManualPath& path) {
  path_ = std::make_unique<ManualPath>(path);
  manual_ = std::make_unique<ManualBook>();
  budget_ = WriteBudget(path_->write_tier);
  write_cap_.store(budget_.cap());
  if (path_->sys_send) {
    sys_ = std::make_unique<SysBook>(path_->lanes.empty() ? default_lanes() : path_->lanes);
    std::atomic_store(&groups_view_, std::make_shared<const std::pair<bool, std::string>>(false, "order groups not created yet"));
    // The book a previous run left: its positions, and today's day and HALT. One that cannot be
    // read blocks Systematic (an empty book would reopen every limit and the daily stop).
    std::ifstream in(path_->sys_state_file);
    if (!path_->sys_state_file.empty() && in) {
      const auto j = nlohmann::json::parse(in, nullptr, false);
      const auto day0 = et_midnight_ms(now_ms());
      SysBook::Halt h;
      std::string why;
      if (!j.is_discarded() && sys_->restore(j, day0, &h, &why)) {
        sys_day_ms_ = day0;
        sys_saved_ms_ = j.value("saved_ms", std::int64_t{0});
        if (j.contains("man_held") && j["man_held"].is_array())
          for (const auto& t : j["man_held"]) if (t.is_string()) man_held_.insert(t.get<std::string>());
        publish_held();
        if (h.on) {
          own_ = RunView{RunState::HALTED, h.ms, 0, h.why};
          publish();
        }
        DLOG_INFO("Systematic book restored from %s: %zu position(s)%s", path_->sys_state_file.c_str(), sys_->held().size(),
                  h.on ? (", HALTED: " + h.why).c_str() : "");
      } else {
        sys_restore_error_ = "the saved Systematic book " + path_->sys_state_file + " cannot be read (" +
                             (j.is_discarded() ? std::string("not JSON") : why) + "): move it aside to start an empty one";
        DLOG_ERR("%s", sys_restore_error_.c_str());
      }
    }
    // today begins now, before any account read is reconciled into it (a later roll would wipe it)
    if (!sys_day_ms_) {
      sys_day_ms_ = et_midnight_ms(now_ms());
      sys_->roll(sys_day_ms_);
    }
  }
}

void Gateway::save_sys() {
  if (!sys_ || path_->sys_state_file.empty() || !sys_restore_error_.empty()) return;
  const SysBook::Halt h{own_.state == RunState::HALTED, own_.since_ms, own_.why};
  const auto now = now_ms();
  const std::string tmp = path_->sys_state_file + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    auto j = sys_->to_json(h, now);
    j["man_held"] = man_held_;
    out << j.dump();
    out.flush();
    if (!out) {
      DLOG_ERR("cannot write %s: Systematic's book is not saved", tmp.c_str());
      return;
    }
  }
  if (std::rename(tmp.c_str(), path_->sys_state_file.c_str()) != 0) {
    DLOG_ERR("cannot replace %s: Systematic's book is not saved", path_->sys_state_file.c_str());
    return;
  }
  sys_save_due_ = false;
  sys_save_ms_ = now;
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
// "SM1-..." -> "M1": Systematic's client ids carry their lane (the contract's S<lane>- prefix)
std::string lane_of_cid(const std::string& cid) {
  const auto dash = cid.find('-');
  return cid.size() > 2 && cid.front() == 'S' && dash != std::string::npos && dash > 1 ? cid.substr(1, dash - 1) : "";
}
// a fill's identity on both of its paths (the private feed and the REST read)
std::string fill_key(const kalshi::FillRow& f) { return f.trade_id.empty() ? f.fill_id : f.trade_id; }
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
    // a new account read, even with nothing else happening: it may settle an unknown order; and
    // the backup cancel of anything working past its expiry
    if (manual_ && now_ms() - snapshot_polled_ms_ >= 250) {
      snapshot_polled_ms_ = now_ms();
      sync_snapshot();
      expiry_sweep(snapshot_polled_ms_);
    }
    if (manual_ && now_ms() - timelines_swept_ms_ >= 1000) {   // timelines whose last stamps never came
      timelines_swept_ms_ = now_ms();
      std::vector<std::string> due;
      for (const auto& [cid, s] : stamps_)
        if (const auto* o = manual_->ledger().find(cid); o && o->terminal() && o->done_ms && timelines_swept_ms_ - o->done_ms > 5000)
          due.push_back(cid);
      for (const auto& cid : due) record_if_done(cid, true);
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
    // a daily-stop HALT outlives a KILL and its re-arm: it ends only at ET midnight
    if (own_.state != RunState::HALTED && (changed || own_.state == RunState::RUN)) {
      own_ = RunView{RunState::STOP, kill_.since_ms(), 0, ""};
      publish();
    }
    answer_erased_starts(gone, "KILLED", "KILL overtook this start");
    DLOG_WARN("KILL (%s): Systematic stopped, %zu queued orders and starts deleted", kill_.why().c_str(), n);
    return true;
  }
  // Re-armed. Later: reset every order group; GROUPS fails until order_group_updates confirms
  // them. A Start gw already answered RUN after the re-arm stands.
  if (changed && own_.state == RunState::STOP) {
    own_ = RunView{RunState::STOP, kill_.rearmed_ms(), 0, ""};
    publish();
  }
  if (changed && sys_) reset_groups();   // KILL's sender triggered them; the next Start needs them active
  return false;
}

void Gateway::note_latency(const KalshiWriteReply& r) {
  const auto* o = manual_->ledger().find(r.cid);
  if (!o || !r.ack_ms) return;
  LatSample s;
  if (o->intent.t_click_ms > 0 && r.ack_ms >= o->intent.t_click_ms) s.click_ack = static_cast<int>(r.ack_ms - o->intent.t_click_ms);
  s.recv_ack = static_cast<int>(std::max<std::int64_t>(0, r.ack_ms - o->intent.t_recv_ms));
  s.wire = static_cast<int>(r.exchange.write_to_done_ms());
  lat_.push_back(s);
  if (lat_.size() > 200) lat_.pop_front();
  auto pct = [](std::vector<int> v, double q) -> std::optional<int> {
    if (v.empty()) return std::nullopt;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(q * (v.size() - 1) + 0.5))];
  };
  std::vector<int> click, recv, wire, last20;
  for (std::size_t i = 0; i < lat_.size(); ++i) {
    if (lat_[i].click_ack >= 0) click.push_back(lat_[i].click_ack);
    recv.push_back(lat_[i].recv_ack);
    wire.push_back(lat_[i].wire);
    if (i + 20 >= lat_.size()) last20.push_back(lat_[i].recv_ack);
  }
  auto v = std::make_shared<Latency>();
  v->n = static_cast<int>(lat_.size());
  v->click_ack_p50 = pct(click, 0.5); v->click_ack_p90 = pct(click, 0.9);
  v->recv_ack_p50 = pct(recv, 0.5); v->recv_ack_p90 = pct(recv, 0.9);
  v->wire_p50 = pct(wire, 0.5); v->wire_p90 = pct(wire, 0.9);
  v->ack20_p50 = pct(last20, 0.5);
  std::atomic_store(&latency_, std::shared_ptr<const Latency>(std::move(v)));
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
  if (auto* t = std::get_if<TransferIntent>(&m)) return transfer(*t);
  if (auto* w = std::get_if<ManualWrite>(&m)) {
    if (w->reply && w->reply->outcome.http == 429) {
      const bool one = w->reply->op == WireOp::CREATE || w->reply->op == WireOp::CANCEL;
      budget_.venue_refused(one ? std::max(0, w->reply->shard) : 0, now_ms());
      stats_.venue_429.fetch_add(1);
      spend(0, 0);   // publish the emptied bucket
    }
    if (w->reply && w->reply->op == WireOp::TRANSFER) { transfer_reply(*w->reply); return; }
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
    } else if (l->on && !sys_->lane(l->id)->engine) {
      r.code = "BAD_REQUEST";
      r.msg = "lane " + l->id + " has no engine yet: nothing would trade it";
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
    if (manual_) {
      sync_snapshot();
      for (auto& cid : cancel_foreign(cx)) r.cids.push_back(std::move(cid));
    }
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
  if (auto* rel = std::get_if<ReleaseMarket>(&m)) {
    const bool held = man_held_.erase(rel->ticker) > 0;
    if (held) {
      DLOG_INFO("%s released to Systematic", rel->ticker.c_str());
      publish_held();
      sys_save_due_ = true;
      sys_dirty_ = true;
      if (path_ && path_->publish_market) path_->publish_market(rel->ticker);
    }
    if (rel->reply) { try { rel->reply->set_value(held); } catch (const std::future_error&) {} }
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
  record_if_done(cid);
  if (!path_->publish) return;
  const auto row = order_json(cid);
  if (!row.empty()) path_->publish(row);
}

void Gateway::record(const char* table, std::string json) {
  if (!path_->record) return;
  if (path_->record(table, std::move(json))) stats_.records_sent.fetch_add(1);
  else stats_.records_lost.fetch_add(1);
}

void Gateway::record_if_done(const std::string& cid, bool force) {
  const auto it = stamps_.find(cid);
  const auto* o = it == stamps_.end() || !it->second.replied ? nullptr : manual_->ledger().find(cid);
  if (!o || !o->terminal()) return;
  const auto& s = it->second;
  if (!force && !o->refused && (!s.priv || (o->filled > 0 && !s.fill) || (s.cx && !s.cx_ack))) return;
  auto j = o->json();
  auto ms = [](std::int64_t v) { return v ? nlohmann::json(v) : nlohmann::json(nullptr); };
  auto seg = [](double v) { return v >= 0 ? nlohmann::json(std::round(v * 1000) / 1000) : nlohmann::json(nullptr); };
  j["t"]["gw"] = ms(s.gw);
  j["t"]["priv"] = ms(s.priv);
  j["t"]["fill"] = ms(s.fill);
  j["t"]["cx"] = ms(s.cx);
  j["t"]["cx_ack"] = ms(s.cx_ack);
  j["wire"] = {{"w_ms", seg(s.w_ms)}, {"fb_ms", seg(s.fb_ms)}, {"rd_ms", seg(s.rd_ms)}};
  j["shard"] = o->shard;
  record("orders", j.dump());
  stamps_.erase(it);
}

void Gateway::place_manual(PlaceIntent& p) {
  sync_snapshot();
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
  if (sys_) takeover(p.ticker);
  stamps_[cid].gw = now_ms();
  publish_order(cid);   // PEND, with its reservation
  const int shard = placed.job->shard;
  if (!path_->send || !path_->send(std::move(*placed.job))) {
    KalshiWriteReply r;
    r.cid = cid;
    r.op = WireOp::CREATE;
    r.entered_ms = now_ms();
    r.local_code = "RO";
    r.outcome.kind = kalshi::WriteOutcome::Kind::NOT_SENT;
    stamps_[cid].replied = true;
    manual_->apply(r);
    publish_order(cid);
    DLOG_ERR("order %s not sent: send-k1's inbox is full", cid.c_str());
    answer(p.reply, OrderReply{"RO", "the Manual sender's queue is full", order_json(cid)});
    return;
  }
  stats_.manual_sent.fetch_add(1);
  spend(shard, WriteBudget::kCreate);
  if (p.reply) pending_[cid] = p.reply;
}

void Gateway::manual_reply(const KalshiWriteReply& r) {
  if (!manual_) return;
  using K = kalshi::WriteOutcome::Kind;
  if (const auto f = foreign_cx_.find(r.cid); r.op == WireOp::CANCEL && f != foreign_cx_.end()) {
    // not this run's order: the next account read (and the private feed) shows how it ended;
    // until then (10 s) a cancel that covers it again is not sent again
    f->second = now_ms();
    DLOG_INFO("cancel of %s, an order this run did not place: %s", r.cid.c_str(),
              r.outcome.kind == K::ACK ? "acknowledged" : (r.outcome.why.empty() ? r.local_code : r.outcome.why).c_str());
    return;
  }
  if (r.op == WireOp::CANCEL) {
    if (const auto st = stamps_.find(r.cid); st != stamps_.end() && !st->second.cx_ack)
      st->second.cx_ack = r.ack_ms ? r.ack_ms : now_ms();
    const auto why = cancel_why_.find(r.cid);
    if (r.cancel && !manual_->ledger().cancelled(r.cid, *r.cancel, why == cancel_why_.end() ? "USER" : why->second,
                                                 r.ack_ms ? r.ack_ms : now_ms()))
      DLOG_ERR("cancel reply for %s contradicts the order book: reconcile before trading", r.cid.c_str());
    publish_order(r.cid);
    return;   // a cancel that did not land leaves the order working until an order read or event says more
  }
  if (r.op != WireOp::CREATE || r.cid.empty()) return;   // cancel-all (KILL) is reconciled from the venue
  if (sys_ && r.outcome.kind == K::REFUSED && r.outcome.cls == RejectClass::SOFT_GROUP)
    if (const auto* o = manual_->ledger().find(r.cid); o && o->intent.owner == Owner::SYS) {
      auto& g = sys_->group(o->intent.lane, o->shard);
      if (g.state == SysBook::GroupState::ACTIVE) g.state = SysBook::GroupState::TRIGGERED;
      lanes_dirty_.insert(o->intent.lane);
    }
  if (const auto st = stamps_.find(r.cid); st != stamps_.end()) st->second.replied = true;
  if (const auto st = stamps_.find(r.cid); st != stamps_.end() && r.exchange.t_start_us) {
    const auto& x = r.exchange;
    if (x.t_written_us) st->second.w_ms = (x.t_written_us - x.t_start_us) / 1000.0;
    if (x.t_written_us && x.t_first_byte_us) st->second.fb_ms = (x.t_first_byte_us - x.t_written_us) / 1000.0;
    if (x.t_first_byte_us && x.t_done_us) st->second.rd_ms = (x.t_done_us - x.t_first_byte_us) / 1000.0;
  }
  const bool ok = manual_->apply(r);
  if (!ok) DLOG_ERR("reply for order %s contradicts the order book: reconcile before trading", r.cid.c_str());
  // every create that did not land, with its reason, for desk.py report
  if (r.outcome.kind == K::UNKNOWN)
    DLOG_WARN("order %s: outcome unknown (%s); it blocks its market until the private feed or an account read settles it",
              r.cid.c_str(), r.outcome.why.substr(0, 200).c_str());
  else if (r.outcome.kind != K::ACK)
    DLOG_INFO("order %s not placed: %s%s%s", r.cid.c_str(),
              r.local_code.empty() ? reject_class_name(r.outcome.cls) : r.local_code.c_str(),
              r.outcome.why.empty() ? "" : ": ", r.outcome.why.substr(0, 200).c_str());
  if (ok && r.outcome.kind == K::ACK && r.create) note_latency(r);
  publish_order(r.cid);
  flush_cancel(r.cid);   // a cancel asked for while the write was in flight goes out once the order id is known
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
  if (e.kind == PrivateEvent::Kind::GROUP) {
    if (sys_) group_event(e.group_id, e.group_event);
    return;
  }
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
        if (sys_->fill(o->intent.lane, o->intent.cid, e.fill.ticker,
                       e.fill.side == kalshi::BookSide::BID ? Side::BUY : Side::SELL, e.fill.count, e.fill.yes_price,
                       e.fill.fee, at, fill_key(e.fill)))
          sys_save_due_ = true;
        lanes_dirty_.insert(o->intent.lane);
      }
    }
  }
  stats_.private_applied.fetch_add(1);
  if (cid.empty()) {
    for (const auto& [c, o] : ledger.orders())
      if (o.oid == (e.kind == PrivateEvent::Kind::ORDER ? e.order.order_id : e.fill.order_id)) cid = c;
  }
  if (const auto st = cid.empty() ? stamps_.end() : stamps_.find(cid); st != stamps_.end()) {
    if (!st->second.priv) st->second.priv = at;
    if (e.kind == PrivateEvent::Kind::FILL && !st->second.fill) st->second.fill = at;
  }
  if (e.kind == PrivateEvent::Kind::FILL) {
    const auto* o = cid.empty() ? nullptr : ledger.find(cid);
    const auto& f = e.fill;
    record("fills", nlohmann::json{{"fid", fill_key(f)}, {"cid", cid}, {"oid", f.order_id}, {"tk", f.ticker},
                                   {"side", f.side == kalshi::BookSide::BID ? "BUY" : "SELL"}, {"qty", qty_to_number(f.count)},
                                   {"px", price_str(f.yes_price)}, {"fee", money_str(f.fee / 100)}, {"taker", f.taker},
                                   {"ts_ms", f.ts_ms}, {"recv_ms", at}, {"shard", f.exchange_index},
                                   {"own", o ? (o->intent.owner == Owner::SYS ? "SYS" : "MAN") : "EXT"},
                                   {"lane", o && !o->intent.lane.empty() ? nlohmann::json(o->intent.lane) : nlohmann::json(nullptr)}}
                        .dump());
  }
  if (!cid.empty()) {
    publish_order(cid);
    flush_cancel(cid);
  }
}

// Plan section 3: every post carries an exchange-side expiry, and a client cancel at the expiry
// plus 1.5 s backs it up ("nothing lives 2 s past expiry"). An order still working then is
// cancelled with why EXPIRY, once; one whose id is not known yet goes when it is (flush_cancel).
// One the venue did expire answers 404 (GONE), which is harmless.
void Gateway::expiry_sweep(std::int64_t now) {
  for (auto it = expiry_asked_.begin(); it != expiry_asked_.end();) {
    const auto* o = manual_->ledger().find(*it);
    it = !o || o->terminal() ? expiry_asked_.erase(it) : std::next(it);
  }
  std::vector<std::string> due;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (!o.terminal() && o.intent.tif == Tif::POST && o.intent.exp_ms && now >= o.intent.exp_ms + kExpiryBackupMs &&
        !cancel_sent_.count(cid) && !expiry_asked_.count(cid))
      due.push_back(cid);
  for (const auto& cid : due) {
    expiry_asked_.insert(cid);
    const auto* o = manual_->ledger().find(cid);
    DLOG_WARN("order %s still working %lld ms past its expiry: cancelling it (the backup to the exchange's expiry)",
              cid.c_str(), static_cast<long long>(now - o->intent.exp_ms));
    if (!o->cancel_requested) {
      cancel_why_[cid] = "EXPIRY";
      manual_->ledger().request_cancel(cid, "EXPIRY");
    }
    send_cancel(cid);
    publish_order(cid);
  }
}

// A cancel asked for before the order id was known (the create in flight, or its outcome unknown)
// goes out once something binds it: the create's reply, the private feed, or an account read.
void Gateway::flush_cancel(const std::string& cid) {
  if (const auto* o = manual_->ledger().find(cid))
    if (o->cancel_requested && !o->terminal() && !o->oid.empty() && !cancel_sent_.count(cid)) send_cancel(cid);
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

std::vector<std::string> Gateway::cancel_foreign(const CancelIntent& c) {
  std::vector<std::string> out;
  if (!snapshot_seen_ || !path_->cancel || (c.scope == CancelIntent::Scope::MARKET && c.venue != Venue::K)) return out;
  std::set<std::string> ours;
  for (const auto& [cid, o] : manual_->ledger().orders()) if (!o.oid.empty()) ours.insert(o.oid);
  const auto now = now_ms();
  for (auto it = foreign_cx_.begin(); it != foreign_cx_.end();) it = now - it->second > 60000 ? foreign_cx_.erase(it) : std::next(it);
  for (const auto& row : snapshot_seen_->orders) {
    if (row.status != kalshi::OrderStatus::RESTING || row.remaining <= 0 || ours.count(row.order_id)) continue;
    if (!row.client_order_id.empty() && manual_->ledger().find(row.client_order_id)) continue;
    const std::string cid = screen_cid(row);
    const char first = row.client_order_id.empty() ? 0 : row.client_order_id.front();
    const bool own_ok = c.own == CancelIntent::Own::ANY || (c.own == CancelIntent::Own::MAN && first == 'M') ||
                        (c.own == CancelIntent::Own::SYS && first == 'S');
    const Side side = row.side == kalshi::BookSide::BID ? Side::BUY : Side::SELL;
    bool covered = false;
    switch (c.scope) {
      case CancelIntent::Scope::CID: covered = c.cid == cid; break;
      case CancelIntent::Scope::MARKET:
        covered = c.ticker == row.ticker && own_ok && (!c.has_side || side == c.side) && (!c.px || row.yes_price == c.px);
        break;
      case CancelIntent::Scope::LANE: covered = first == 'S' && lane_of_cid(row.client_order_id) == c.lane; break;
      case CancelIntent::Scope::ALL: covered = own_ok; break;
    }
    if (!covered) continue;
    const auto sent = foreign_cx_.find(cid);
    if (sent != foreign_cx_.end() && now - sent->second < 10000) continue;   // asked already; its reply is coming
    KalshiWriteJob j;
    j.kind = KalshiWriteJob::Kind::CANCEL;
    j.cid = cid;
    j.oid = row.order_id;
    j.ticker = row.ticker;
    j.shard = row.exchange_index.value_or(0);
    const int shard = j.shard;
    if (path_->cancel(std::move(j))) {
      spend(shard, WriteBudget::kCancel);
      foreign_cx_[cid] = now;
      out.push_back(cid);
      DLOG_INFO("cancelling %s on %s, an order this run did not place", cid.c_str(), row.ticker.c_str());
    }
  }
  return out;
}

// The ticket's MOVE (contract POST /api/transfer): free cash from one shard to another on
// send-k1. Never during KILL; never more than the source's free cash as gw sees it (cash less what
// gw's working orders hold there). The page's answer waits for the venue's.
void Gateway::transfer(TransferIntent& t) {
  auto answer_t = [&](const char* code, const std::string& msg) {
    if (t.reply) { try { t.reply->set_value(TransferReply{code, msg, ""}); } catch (const std::future_error&) {} }
  };
  // the contract's codes for it: BAD_REQUEST, NO_CASH, RATE, EXCHANGE (and INTERNAL)
  if (!manual_) return answer_t("NO_CASH", "no Kalshi account in this deskd");
  if (!path_->send) return answer_t("EXCHANGE", "Manual orders are off: no write path");
  if (kill_.active()) return answer_t("EXCHANGE", "KILL lockout active: re-arm first");
  // the Manual path's own gate (manual_orders, send-k1 healthy): a transfer is a Manual write
  if (const auto h = path_->health ? path_->health() : ManualHealth{}; h.read_only || !h.manual_sender)
    return answer_t("EXCHANGE", "the Manual path is read-only: " + (h.why.empty() ? std::string("Manual orders are off") : h.why));
  if (t.from < 0 || t.to < 0 || t.from == t.to || t.amount <= 0) return answer_t("BAD_REQUEST", "a transfer names two shards and an amount");
  sync_snapshot();
  if (!snapshot_seen_ || !snapshot_seen_->account_ready) return answer_t("NO_CASH", "no account read yet");
  if (!snapshot_seen_->balances.count(t.from) || !snapshot_seen_->balances.count(t.to))
    return answer_t("BAD_REQUEST", "no shard S" + std::to_string(snapshot_seen_->balances.count(t.from) ? t.to : t.from));
  const Money free = manual_->shard_free(t.from) / 100;
  if (t.amount > free)
    return answer_t("NO_CASH", "S" + std::to_string(t.from) + " has $" + money_str(std::max<Money>(0, free)) + " free");
  KalshiWriteJob j;
  j.kind = KalshiWriteJob::Kind::TRANSFER;
  j.cid = "T" + std::to_string(++transfer_seq_);
  j.shard = t.from;
  j.transfer_to = t.to;
  j.amount = t.amount;
  j.kill_generation = kill_.generation();
  const auto cid = j.cid;
  if (!path_->send(std::move(j))) return answer_t("INTERNAL", "send-k1's queue is full");
  spend(0, WriteBudget::kOther);
  DLOG_INFO("transfer %s: $%s from S%d to S%d", cid.c_str(), money_str(t.amount).c_str(), t.from, t.to);
  transfers_[cid] = PendingTransfer{t.reply, t.from, t.to, t.amount};
}

void Gateway::transfer_reply(const KalshiWriteReply& r) {
  const auto it = transfers_.find(r.cid);
  if (it == transfers_.end()) return;
  const auto t = it->second;
  transfers_.erase(it);
  using K = kalshi::WriteOutcome::Kind;
  TransferReply out;
  if (r.outcome.kind == K::ACK) {
    // the shards as the last read has them, with the move applied; the next read confirms it
    nlohmann::json shards = nlohmann::json::array();
    if (snapshot_seen_)
      for (const auto& [shard, cash] : snapshot_seen_->balances) {
        const Money moved = shard == t.from ? -t.amount : shard == t.to ? t.amount : 0;
        const Money free = std::max<Money>(0, manual_->shard_free(shard) / 100 + moved);
        shards.push_back({{"id", "S" + std::to_string(shard)}, {"cash", money_str(std::max<Money>(0, cash + moved))},
                          {"free", money_str(free)}});
      }
    out.shards = shards.dump();
    DLOG_INFO("transfer %s done: $%s from S%d to S%d", r.cid.c_str(), money_str(t.amount).c_str(), t.from, t.to);
  } else if (r.outcome.kind == K::REFUSED && r.outcome.cls == RejectClass::RETRYABLE) {
    out.code = "RATE";
    out.msg = "the venue is rate-limiting writes; try again";
  } else {
    out.code = "EXCHANGE";
    out.msg = (r.outcome.kind == K::UNKNOWN ? "the venue did not confirm it (the next account read shows whether it moved): "
                                              : "") + (r.outcome.why.empty() ? r.local_code : r.outcome.why).substr(0, 200);
    DLOG_WARN("transfer %s: %s", r.cid.c_str(), out.msg.c_str());
  }
  if (path_->refresh) path_->refresh();
  if (t.reply) { try { t.reply->set_value(std::move(out)); } catch (const std::future_error&) {} }
}

void Gateway::spend(int shard, double cost) {
  const auto now = now_ms();
  budget_.spend(std::max(0, shard), cost, now);
  std::atomic_store(&write_view_, std::shared_ptr<const std::map<int, double>>(
                                      std::make_shared<const std::map<int, double>>(budget_.all(now))));
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
  const int shard = j.shard;
  if (via && via(std::move(j))) {
    spend(shard, WriteBudget::kCancel);
    if (const auto st = stamps_.find(cid); st != stamps_.end() && !st->second.cx) st->second.cx = now_ms();
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
    if (snapshot_seen_ && snapshot_seen_->account_ready && snapshot_seen_->account_ms != settled_ms_) {
      settled_ms_ = snapshot_seen_->account_ms;
      settle_uncertain(*snapshot_seen_);
      note_ledger();   // a contradiction it found stops the ledger: /ops/status and the log say so
    }
    if (sys_ && snapshot_seen_ && snapshot_seen_->account_ready && snapshot_seen_->account_ms != reconciled_ms_) {
      reconciled_ms_ = snapshot_seen_->account_ms;
      sys_reconcile(*snapshot_seen_);
    }
  }
}

// An order whose outcome is unknown (its write timed out, its reply was lost, or its cancel was
// not confirmed) blocks new orders on its market until something settles it. The private feed
// usually does; when it missed the update, each account read does: the order's own row settles
// it, and its absence from a complete read begun well after its deadline proves it never rested
// (OrderLedger::resolve_absent). Found 2026-10-13 on demo: an order stuck PEND refused every
// later order on its market as STALE for the rest of the run.
void Gateway::settle_uncertain(const rest::Snapshot& s) {
  auto& ledger = manual_->ledger();
  std::vector<std::string> due;
  for (const auto& [cid, o] : ledger.orders())
    if (o.uncertain && !o.terminal()) due.push_back(cid);
  for (const auto& cid : due) {
    const auto* o = ledger.find(cid);
    if (!o) continue;
    const kalshi::OrderRow* row = nullptr;
    for (const auto& r : s.orders)
      if (r.client_order_id == cid || (!o->oid.empty() && r.order_id == o->oid)) { row = &r; break; }
    if (row) {
      const bool healthy = ledger.healthy();
      if (ledger.observed(*row, s.account_ms)) {
        const auto* after = ledger.find(cid);
        DLOG_INFO("order %s: the account read shows it %s%s", cid.c_str(), after->json().value("st", "?").c_str(),
                  after->uncertain ? " (its cancel is not confirmed yet)" : "");
        publish_order(cid);
        flush_cancel(cid);   // cancelled while its outcome was unknown: the cancel goes now
      } else if (healthy && !ledger.healthy()) {
        DLOG_ERR("account read of order %s contradicts the order book: reconcile before trading", cid.c_str());
      }
      continue;
    }
    OrderLedger::History h;
    h.ticker = o->intent.ticker;
    h.shard = o->shard;
    h.since_ms = s.orders_from_ms;
    h.began_ms = s.account_began_ms;
    h.completed_ms = s.account_ms;
    h.orders_complete = h.fills_complete = true;   // rest refuses a read it could not page to the end
    for (const auto& r : s.orders) if (r.ticker == h.ticker) h.orders.push_back(r);
    for (const auto& f : s.fills) if (f.ticker == h.ticker) h.fills.push_back(f);
    if (ledger.resolve_absent(cid, h)) {
      DLOG_INFO("order %s: absent from a complete account read begun after its deadline, so it never rested (%s)",
                cid.c_str(), ledger.find(cid)->why.c_str());
      publish_order(cid);
    }
  }
}

// Each account read, against Systematic's book (plan section 3: "REST read of that ticker's
// fills"): every fill of today on a Systematic order counts once by trade id, so a fill the
// private feed lost (a socket gap, a fill before the 201) still moves the lane's position, and
// the first read after a start rebuilds the day the saved book could not have seen; and a market
// the lanes hold that has settled realizes their position at its result.
void Gateway::sys_reconcile(const rest::Snapshot& s) {
  const std::int64_t day0 = sys_->day_start_ms() ? sys_->day_start_ms() : et_midnight_ms(now_ms());
  std::map<std::string, std::string> cid_of;   // order id -> client order id
  for (const auto& o : s.orders) if (!o.client_order_id.empty()) cid_of[o.order_id] = o.client_order_id;
  for (const auto& [cid, o] : manual_->ledger().orders()) if (!o.oid.empty()) cid_of[o.oid] = cid;
  int applied = 0, settled = 0;
  for (const auto& f : s.fills) {
    if (f.ts_ms < day0) continue;   // yesterday's are in the positions carried over
    const auto it = cid_of.find(f.order_id);
    if (it == cid_of.end()) continue;
    const std::string lane = lane_of_cid(it->second);
    if (lane.empty() || !sys_->lane(lane)) continue;
    if (sys_->fill(lane, it->second, f.ticker, f.side == kalshi::BookSide::BID ? Side::BUY : Side::SELL, f.count,
                   f.yes_price, f.fee, f.ts_ms, fill_key(f))) {
      ++applied;
      lanes_dirty_.insert(lane);
    }
  }
  if (applied && sys_rebuilt_)
    DLOG_WARN("%d Systematic fill(s) the private feed did not deliver, applied from the REST read", applied);
  else if (applied)
    DLOG_INFO("Systematic's day: %d fill(s) from the venue's read since the book was saved", applied);
  for (const auto& [lane, tk] : sys_->held()) {
    const auto m = s.markets.find(tk);
    if (m == s.markets.end() || m->second.status == "active" || m->second.status == "open") continue;
    if (m->second.result != "yes" && m->second.result != "no") continue;
    const Micros pnl = sys_->settle(lane, tk, m->second.result == "yes" ? kOneDollar : 0, now_ms());
    DLOG_INFO("%s settled %s: lane %s realizes %s", tk.c_str(), m->second.result.c_str(), lane.c_str(),
              money_str(pnl / 100).c_str());
    lanes_dirty_.insert(lane);
    ++settled;
  }
  if (applied || settled || !sys_rebuilt_) {
    sys_dirty_ = true;
    sys_save_due_ = true;
  }
  sys_rebuilt_ = true;
}

std::set<int> Gateway::shards() const {
  std::set<int> out;
  if (snapshot_seen_) for (const auto& s : snapshot_seen_->exchange.shards) out.insert(s.exchange_index);
  if (out.empty()) out.insert(0);
  return out;
}

// The live mid, else the last one gw saw (a closed window's book is gone, and its last mid is the
// best price for it until it settles); none: the market was never priced (counted worthless).
std::optional<Px> Gateway::mark(const std::string& ticker) const {
  const auto b = path_->book ? path_->book(ticker) : std::nullopt;
  if (b && !b->stale && !b->bids.empty() && !b->asks.empty()) {
    const Px m = (b->bids.front().px + b->asks.front().px) / 2;
    last_mark_[ticker] = m;
    return m;
  }
  const auto it = last_mark_.find(ticker);
  return it == last_mark_.end() ? std::nullopt : std::optional<Px>(it->second);
}

void Gateway::sys_money(Micros* used, Micros* ceiling, Micros* eff) const {
  Micros u = 0;
  for (const auto& [cid, o] : manual_->ledger().orders()) if (o.intent.owner == Owner::SYS) u += o.reservation();
  for (const auto& x : sys_->lanes()) u += sys_->open_worst(x.id);
  // contract hdr.sys: ceiling = 0.70 x (cash + open positions' value), eff = min(ceiling, used + free)
  Micros c = 0, e = 0;
  if (snapshot_seen_) {
    const Micros equity = (snapshot_seen_->balance.available + snapshot_seen_->balance.portfolio_value.value_or(0)) * 100;
    c = equity * 7 / 10;
    e = std::min<Micros>(c, u + manual_->venue_free());
  }
  if (used) *used = u;
  if (ceiling) *ceiling = c;
  if (eff) *eff = e;
}

Micros Gateway::sys_fees() const {
  Micros f = 0;
  for (const auto& l : sys_->lanes()) if (const auto* d = sys_->day(l.id)) f += d->fees;
  return f;
}

std::string Gateway::lane_row(const std::string& id) const {
  const auto* l = sys_->lane(id);
  if (!l) return "";
  Micros reserved = 0, used_all = 0, eff = 0;
  int working = 0;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (o.intent.owner == Owner::SYS && !o.terminal() && o.intent.lane == id) { reserved += o.reservation(); ++working; }
  sys_money(&used_all, nullptr, &eff);
  // an IOC-style lane past the day's cost budget takes nothing more (contract: CAPPED)
  if (l->style == "IOC" && sys_fees() >= kCostBudget * 100) eff = used_all;
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
    // on the page too (a lane that cannot trade says why), once a minute per lane and code; not
    // the passing ones (between Stop and the maker seeing it, the first read after a start)
    const std::string c = code;
    if (path_->alert && c != "BAD_STATE" && c != "STALE") {
      auto& last = alerted_[p.lane + "|" + c];
      if (now_ms() - last >= 60000) {
        last = now_ms();
        path_->alert("WARN", p.lane, "refused " + c + ": " + msg);
      }
    }
    answer(p.reply, OrderReply{code, msg, ""});
  };
  if (kill_.active()) return refuse("KILLED", "KILL lockout active");
  if (own_.state == RunState::HALTED) return refuse("HALTED", "Systematic halted: " + own_.why);
  if (own_.state != RunState::RUN) return refuse("BAD_STATE", "a Systematic order while Systematic is not running");
  const auto* lane = sys_->lane(p.lane);
  if (!lane) return refuse("BAD_REQUEST", "no lane " + p.lane);
  if (!sys_->on(p.lane)) return refuse("BAD_STATE", "lane " + p.lane + " is off");
  if (lane->paper_only && path_->live) return refuse("FORBIDDEN", "lane " + p.lane + " trades only on the demo account");
  if (!sys_restore_error_.empty()) return refuse("RO", sys_restore_error_);
  sync_snapshot();
  if (!snapshot_seen_) return refuse("STALE", "no account read yet");
  if (!sys_rebuilt_) return refuse("STALE", "Systematic's day is rebuilt from the venue's fills first");
  if (man_held_.count(p.ticker)) return refuse("BAD_STATE", p.ticker + " is held by Manual until released");
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
  if (lane->style == "IOC" && sys_fees() >= kCostBudget * 100)
    return refuse("SYS_CEILING", "the IOC lanes' daily cost budget " + money_str(kCostBudget) + " is spent");
  Micros reserved_lane = 0, used_all = 0;
  Qty lane_pending = 0;
  for (const auto& [cid, o] : manual_->ledger().orders()) {
    if (o.intent.owner != Owner::SYS) continue;
    used_all += o.reservation();
    if (o.terminal()) continue;
    if (o.intent.lane == p.lane) reserved_lane += o.reservation();
    if (o.intent.ticker == p.ticker && o.intent.side == p.side) {
      pending += o.left;
      if (o.intent.lane == p.lane) lane_pending += o.left;
    }
  }
  const Qty grow = (p.side == Side::BUY ? 1 : -1) * (pending + p.count);
  if (std::abs(pos + grow) > lane->inventory && std::abs(pos + grow) > std::abs(pos))
    return refuse("SYS_CEILING", "inventory: " + qty_text(pos) + " held, " + qty_text(pending) + " working, limit " + qty_text(lane->inventory));
  // The lane's budget and the venue's 70% ceiling, at the worst case of the part of this order
  // that grows the lane's position (after its working orders on the same side): a reducing
  // order, the T-60 s flatten among them, is never refused for money (plan section 4).
  const Qty dir_pos = (p.side == Side::BUY ? 1 : -1) * sys_->position(p.lane, p.ticker) + lane_pending;
  const Qty grows = std::max<Qty>(0, dir_pos + p.count) - std::max<Qty>(0, dir_pos);
  const Micros worst = (p.side == Side::BUY ? p.price : kOneDollar - p.price) * grows;
  for (const auto& l : sys_->lanes()) used_all += sys_->open_worst(l.id);
  if (grows > 0 && sys_->open_worst(p.lane) + reserved_lane + worst > lane->budget * 100)
    return refuse("SYS_CEILING", "lane " + p.lane + " budget " + money_str(lane->budget));
  const Micros equity = (snapshot_seen_->balance.available + snapshot_seen_->balance.portfolio_value.value_or(0)) * 100;
  const Micros eff = std::min<Micros>(equity * 7 / 10, used_all + manual_->venue_free());
  if (grows > 0 && used_all + worst > eff)
    return refuse("SYS_CEILING", "past the effective Systematic ceiling " + money_str(eff / 100));
  // the correlated group (plan section 4, limit 2): every lane's worst case there, held and working
  if (grows > 0) {
    auto event_of = [this](const std::string& tk) {
      const auto it = snapshot_seen_->markets.find(tk);
      return it == snapshot_seen_->markets.end() ? std::string() : it->second.event_ticker;
    };
    const std::string grp = corr_group(p.ticker, m->second.event_ticker);
    const auto in_grp = [&](const std::string& tk) { return corr_group(tk, event_of(tk)) == grp; };
    Micros in_group = worst;
    for (const auto& l : sys_->lanes()) in_group += sys_->open_worst_in(l.id, in_grp);
    for (const auto& [cid, o] : manual_->ledger().orders())
      if (o.intent.owner == Owner::SYS && !o.terminal() && in_grp(o.intent.ticker)) in_group += o.reservation();
    if (in_group > corr_group_cap(grp))
      return refuse("SYS_CEILING", "group " + grp + " worst case past " + money_str(corr_group_cap(grp) / 100));
  }
  // the floor (plan section 4, limit 5): the account's equity under it halts Systematic
  if (path_->equity_floor > 0 && equity < path_->equity_floor * 100) {
    halt("equity floor: the account's equity " + money_str(equity / 100) + " is under " + money_str(path_->equity_floor));
    return refuse("HALTED", "Systematic halted by the equity floor");
  }
  // the daily loss stop (decision 1): Systematic's day, marked at the mids
  const auto mk = [this](const std::string& tk) { return mark(tk); };
  if (path_->daily_stop > 0 && sys_->day_total(mk) <= -path_->daily_stop * 100) {
    halt("daily stop");
    return refuse("HALTED", "Systematic halted by the daily stop");
  }
  // the write budget (plan section 3): Systematic leaves a fifth of each bucket to Manual and cancels
  if (!budget_.sys_may(shard, WriteBudget::kCreate, now_ms())) {
    stats_.sys_rate_refused.fetch_add(1);
    return refuse("RATE", "Systematic's 80% of shard " + std::to_string(shard) + "'s write budget is spent; room again in " +
                              std::to_string(budget_.sys_wait_ms(shard, WriteBudget::kCreate, now_ms())) + " ms");
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
  stamps_[cid].gw = now_ms();
  publish_order(cid);
  if (!path_->sys_send(std::move(*placed.job))) {
    KalshiWriteReply r;
    r.cid = cid;
    r.op = WireOp::CREATE;
    r.entered_ms = now_ms();
    r.local_code = "RO";
    r.outcome.kind = kalshi::WriteOutcome::Kind::NOT_SENT;
    stamps_[cid].replied = true;
    manual_->apply(r);
    publish_order(cid);
    return refuse("RO", "send-k2's queue is full");
  }
  stats_.sys_sent.fetch_add(1);
  spend(shard, WriteBudget::kCreate);
  sys_->sent(p.lane, now_ms());
  sys_save_due_ = true;
  lanes_dirty_.insert(p.lane);
  if (p.reply) pending_[cid] = p.reply;
}

int Gateway::cancel_sys(const std::function<bool(const std::string& lane)>& which, const char* why) {
  return cancel_sys_if([&which](const std::string& lane, const std::string&) { return which(lane); }, why);
}

int Gateway::cancel_sys_if(const std::function<bool(const std::string& lane, const std::string& ticker)>& which,
                           const char* why) {
  std::vector<std::string> cids;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (o.intent.owner == Owner::SYS && !o.terminal() && which(o.intent.lane, o.intent.ticker)) cids.push_back(cid);
  for (const auto& cid : cids) {
    cancel_why_[cid] = why;
    manual_->revoke(cid);   // a write not yet on the wire never leaves
    manual_->ledger().request_cancel(cid, why);
    const auto* o = manual_->ledger().find(cid);
    if (o && !o->oid.empty() && !cancel_sent_.count(cid)) send_cancel(cid);
    publish_order(cid);
  }
  return static_cast<int>(cids.size());
}

// A manual order admitted on a market Systematic quotes or holds, or that a lane which is on
// trades (its series): the market is Manual's until released (plan section 4: Kalshi nets one
// position per market). Systematic's orders there are cancelled (TAKEOVER) and each lane's
// position is handed to Manual at the mid (the last mid seen; never priced: as worthless), with a
// logged line; Manual owns any flatten.
void Gateway::takeover(const std::string& tk) {
  if (man_held_.count(tk)) return;
  bool here = false;
  for (const auto& [cid, o] : manual_->ledger().orders())
    if (o.intent.owner == Owner::SYS && !o.terminal() && o.intent.ticker == tk) here = true;
  const auto held = sys_->held();
  for (const auto& [lane, t] : held) if (t == tk) here = true;
  for (const auto& l : sys_->lanes())
    if (sys_->on(l.id))
      for (const auto& s : l.series)
        if (tk.size() > s.size() && tk.compare(0, s.size(), s) == 0 && tk[s.size()] == '-') here = true;
  if (!here) return;
  man_held_.insert(tk);
  const int n = cancel_sys_if([&tk](const std::string&, const std::string& t) { return t == tk; }, "TAKEOVER");
  for (const auto& [lane, t] : held) {
    if (t != tk) continue;
    const Qty pos = sys_->position(lane, tk);
    const auto m = mark(tk);
    const Px at = m ? *m : (pos > 0 ? 0 : kOneDollar);
    const Micros pnl = sys_->settle(lane, tk, at, now_ms());
    DLOG_WARN("Manual took over %s: lane %s's %s contracts handed to Manual at %s (%s realized)", tk.c_str(), lane.c_str(),
              qty_text(pos).c_str(), price_str(at).c_str(), money_str(pnl / 100).c_str());
    lanes_dirty_.insert(lane);
  }
  DLOG_WARN("Manual took over %s: %d Systematic order(s) cancelled; lanes stay out until it is released", tk.c_str(), n);
  publish_held();
  sys_dirty_ = true;
  sys_save_due_ = true;
  if (path_->publish_market) path_->publish_market(tk);
}

void Gateway::publish_held() {
  std::atomic_store(&man_view_, std::make_shared<const std::set<std::string>>(man_held_));
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
  spend(0, WriteBudget::kOther);
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

// order_group_updates: the venue's own word on a group. A trigger gw did not ask for is the
// group's contracts limit at work (too many fills in 15 s): the lane is BLOCKED until a Stop or
// HALT lets gw reset it, never reset while Systematic runs.
void Gateway::group_event(const std::string& id, const std::string& event) {
  for (auto& [key, g] : sys_->groups()) {
    if (g.id != id) continue;
    if (event == "triggered") {
      if (g.state == SysBook::GroupState::ACTIVE)
        DLOG_WARN("order group %s (%s, shard %d) triggered by the venue: %s blocked until Systematic stops",
                  id.c_str(), key.first.c_str(), key.second, key.first.c_str());
      if (g.state != SysBook::GroupState::RESETTING) g.state = SysBook::GroupState::TRIGGERED;
    } else if (event == "reset") {
      g.state = SysBook::GroupState::ACTIVE;
    } else if (event == "deleted") {
      if (path_->groups) path_->groups->remove(g.id);
      g = SysBook::Group{};
    }
    lanes_dirty_.insert(key.first);
    sys_dirty_ = true;
    return;
  }
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
  save_sys();   // a restart keeps the HALT
  cancel_sys([](const std::string&) { return true; }, "HALT");
  trigger_groups();
  for (const auto& l : sys_->lanes()) lanes_dirty_.insert(l.id);
  sys_dirty_ = true;
}

void Gateway::sys_tick(std::int64_t now) {
  if (now - sys_view_ms_ < 100) return;
  // a new ET day: the lanes' counters start again, and a daily-stop HALT ends (before the next
  // account read is reconciled into the new day)
  const auto midnight = et_midnight_ms(now);
  if (midnight > sys_day_ms_) {
    sys_day_ms_ = midnight;
    sys_->roll(midnight);
    if (own_.state == RunState::HALTED && own_.since_ms < midnight) {
      own_ = RunView{RunState::STOP, now, 0, "new day"};
      publish();
    }
    for (const auto& l : sys_->lanes()) lanes_dirty_.insert(l.id);
    sys_save_due_ = true;
  }
  sync_snapshot();
  const auto mk = [this](const std::string& tk) { return mark(tk); };
  if (own_.state == RunState::RUN && path_->daily_stop > 0 && sys_->day_total(mk) <= -path_->daily_stop * 100) halt("daily stop");
  if (own_.state == RunState::RUN && path_->equity_floor > 0 && snapshot_seen_ && snapshot_seen_->account_ready) {
    const Micros equity = (snapshot_seen_->balance.available + snapshot_seen_->balance.portfolio_value.value_or(0)) * 100;
    if (equity < path_->equity_floor * 100)
      halt("equity floor: the account's equity " + money_str(equity / 100) + " is under " + money_str(path_->equity_floor));
  }
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
        // created for a lane that is on (each restart would otherwise leave every lane's groups behind)
        if (g.state == SysBook::GroupState::NONE && sys_->on(l.id) && now - g.asked_ms > 5000) { group_job(l.id, shard, "create"); asked = true; }
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
    v->day = sys_->day_total(mk);
    for (const auto& l : sys_->lanes())
      if (const auto* dl = sys_->day(l.id)) v->lane_day[l.id] = dl->real - dl->fees + sys_->unrealized(l.id, mk);
    v->fees = sys_fees();
    v->held = man_held_;
    sys_money(&v->used, &v->ceiling, &v->eff);
    v->ts_ms = now;
    std::atomic_store(&sys_view_, std::shared_ptr<const SysView>(std::move(v)));
    sys_dirty_ = false;
    sys_view_ms_ = now;
    std::set<std::string> held;
    for (const auto& [lane, tk] : sys_->held()) held.insert(tk);
    const auto old_held = std::atomic_load(&sys_held_);
    if (!old_held || *old_held != held)
      std::atomic_store(&sys_held_, std::make_shared<const std::set<std::string>>(std::move(held)));
  }
  if (sys_save_due_ && now - sys_save_ms_ >= 2000) save_sys();
}

}  // namespace deskd
