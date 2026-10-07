#include "deskd/gw/gateway.h"

#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kill.h"

namespace deskd {

Gateway::Gateway(KillSwitch& kill, PreflightFn preflight, std::size_t normal_cap, std::size_t priority_cap)
    : kill_(kill), preflight_(std::move(preflight)), inbox_(normal_cap, priority_cap) {
  own_.state = RunState::STOP;
  own_.since_ms = now_ms();
  publish();
  kill_.on_trip([this] { inbox_.kick(); });
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
// a deleted Start still has a web thread waiting on it: it gets its refusal now, not a timeout
void answer_erased_starts(std::vector<GwMsg>& gone, const char* code, const char* msg) {
  for (auto& g : gone) {
    auto* st = std::get_if<Start>(&g);
    if (st && st->reply) st->reply->set_value(ControlReply{code, msg, RunView{}, 0, {}});
  }
}
// does a cancel cover this queued new order?
bool cancel_covers(const CancelIntent& c, const PlaceIntent& p) {
  switch (c.scope) {
    case CancelIntent::Scope::CID: return p.cid == c.cid;
    case CancelIntent::Scope::MARKET: return p.venue == c.venue && p.ticker == c.ticker && own_matches(c.own, p.owner);
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

void Gateway::handle(GwMsg& m) {
  if (auto* p = std::get_if<PlaceIntent>(&m)) {
    // No venue path in week 0 (the senders arrive in week 1). Refused, and counted, so a test or
    // a status read sees it reached gw.
    stats_.place_refused.fetch_add(1);
    DLOG_WARN("order %s refused: %s", p->cid.c_str(), kill_.active() ? "KILLED" : "no venue path in this stage");
    return;
  }
  if (auto* c = std::get_if<CancelIntent>(&m)) {
    const CancelIntent cx = *c;
    const std::size_t n = inbox_.erase_if(Lane::NORMAL, [&cx](const GwMsg& q) {
      const auto* p = std::get_if<PlaceIntent>(&q);
      return p && cancel_covers(cx, *p);
    });
    stats_.erased_by_cancel.fetch_add(n);
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
  // AmendIntent, SendResult, OrderEvent: no orders exist in week 0.
}

}  // namespace deskd
