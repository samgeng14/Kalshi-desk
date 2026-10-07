#include "deskd/web/pub.h"

#include <algorithm>
#include <chrono>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"

namespace deskd {

using json = nlohmann::json;

Pub::Pub(std::string epoch, const Feeds& feeds, HdrFn hdr, int max_streams, std::size_t stream_cap)
    : epoch_(std::move(epoch)),
      feeds_(feeds),
      hdr_fn_(std::move(hdr)),
      stream_cap_(stream_cap),
      streams_(max_streams),
      inbox_(4096, 256) {}

bool Pub::post(PubMsg m) {
  const bool waited_on = !std::holds_alternative<PubAlert>(m);
  return inbox_.push(std::move(m), waited_on ? Lane::PRIORITY : Lane::NORMAL);
}

void Pub::alert(const std::string& lvl, const std::string& src, const std::string& msg) {
  post(PubAlert{lvl, src, msg, now_ms()});
}

std::string Pub::hb_data(const std::string& sid, std::int64_t now_ms) const {
  return hb_json(epoch_, sid, now_ms, feeds_json(feeds_)).dump();
}

void Pub::broadcast(const std::string& type, const std::string& data_json, std::int64_t now) {
  for (const auto& s : streams_.open_streams()) s->q.offer(type, now, data_json);
}

void Pub::broadcast_hb(std::int64_t now) {
  const json feeds = feeds_json(feeds_);
  for (const auto& s : streams_.open_streams()) s->q.offer("hb", now, hb_json(epoch_, s->sid, now, feeds).dump());
  hb_sent_.fetch_add(1, std::memory_order_relaxed);
}

void Pub::maybe_hdr(bool force) {
  const std::int64_t mono = mono_ms();
  if (!force && mono - hdr_at_ < 250) return;   // at most 4/s
  json h = hdr_json(hdr_fn_());
  std::string d = h.dump();
  if (d == hdr_dump_) return;                  // only on change
  const bool first = hdr_dump_.empty();
  hdr_ = std::move(h);
  hdr_dump_ = std::move(d);
  hdr_at_ = mono;
  if (!first) {
    broadcast("hdr", hdr_dump_, now_ms());
    hdr_sent_.fetch_add(1, std::memory_order_relaxed);
  }
}

json Pub::alerts_json() const {
  json a = json::array();
  for (const auto& x : alerts_) a.push_back(x);
  return a;
}

void Pub::handle(PubMsg& m) {
  const std::int64_t now = now_ms();
  if (auto* s = std::get_if<PubSnapshot>(&m)) {
    PubReply r;
    auto st = streams_.stream_for(s->sid);
    if (!st) {
      r.status = 409;
      r.body = err_body("BAD_STATE", "no open stream for sid " + s->sid.substr(0, 64));
    } else {
      maybe_hdr(true);   // what the snapshot carries is what the stream has been sent
      r.body = snapshot_json(epoch_, s->sid, st->q.last_seq(), now, hdr_, alerts_json(), feeds_json(feeds_),
                             streams_.subs(s->sid));
    }
    if (s->reply) s->reply->set_value(std::move(r));
    return;
  }
  if (auto* sub = std::get_if<PubSubscribe>(&m)) {
    PubReply r;
    if (!streams_.set_subs(sub->sid, sub->subs)) {
      r.status = 409;
      r.body = err_body("BAD_STATE", "no open stream for sid " + sub->sid.substr(0, 64));
    } else {
      // Later: push one book for every newly subscribed market at once. No market is listed yet.
      r.body = json{{"ok", true}, {"subs", subs_json(sub->subs)}};
    }
    if (sub->reply) sub->reply->set_value(std::move(r));
    return;
  }
  if (auto* a = std::get_if<PubAlert>(&m)) {
    json j = alert_json("a-" + std::to_string(++alert_n_), a->lvl, a->src, a->msg, a->ts_ms ? a->ts_ms : now);
    broadcast("alert", j.dump(), now);
    alerts_.push_back(std::move(j));
    while (alerts_.size() > kAlertsKept) alerts_.pop_front();
  }
}

void Pub::run(Worker& self) {
  // the hb's schedule is on the steady clock, its srv_ms and ts_ms on the wall clock: a wall
  // clock stepped back no longer stops the hb (and with it every page) for as long as the step
  maybe_hdr(true);
  std::int64_t next_hb = mono_ms() + 1000, next_forget = 0;
  while (!self.stopping()) {
    std::int64_t mono = mono_ms();
    const std::int64_t wait = std::max<std::int64_t>(0, std::min<std::int64_t>(next_hb - mono, 250));
    if (auto m = inbox_.pop_for(std::chrono::milliseconds(wait))) handle(*m);
    mono = mono_ms();
    if (mono >= next_hb) {
      broadcast_hb(now_ms());
      next_hb += 1000;
      if (next_hb <= mono) next_hb = mono + 1000;   // after a stall: no burst of catch-up beats
    }
    maybe_hdr(false);
    if (mono >= next_forget) {
      next_forget = mono + 1000;
      streams_.forget_idle(mono, kForgetSidMs);
    }
    self.beat();
  }
}

}  // namespace deskd
