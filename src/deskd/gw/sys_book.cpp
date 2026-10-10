#include "deskd/gw/sys_book.h"
#include <algorithm>
#include <cstdlib>

namespace deskd {
using json = nlohmann::json;

const std::vector<LaneSpec>& default_lanes() {
  static const std::vector<LaneSpec> L = {
      {"M1", "LIP-15", "REST", 55 * 10000, 4000, 5000, {"KXNATGAS15M"}, std::nullopt},
      {"M2", "BAND 96-99", "REST", 25 * 10000, 2000, 5000, {}, std::nullopt},
      {"S1", "ARB", "IOC", 15 * 10000, 2000, 5000, {}, Money{200}},
      {"C1", "COLLEGE TAKE", "IOC", 10 * 10000, 2000, 5000, {}, Money{300}},
      {"X1", "ESPN EXP", "IOC", 5 * 10000, 200, 1000, {}, Money{300}},
  };
  return L;
}

SysBook::SysBook(std::vector<LaneSpec> lanes) : lanes_(std::move(lanes)) {}

const LaneSpec* SysBook::lane(const std::string& id) const {
  for (const auto& l : lanes_) if (l.id == id) return &l;
  return nullptr;
}

bool SysBook::set_on(const std::string& id, bool on) {
  if (!lane(id)) return false;
  if (on) on_.insert(id); else on_.erase(id);
  return true;
}

const SysBook::Group* SysBook::find_group(const std::string& lane, int shard) const {
  const auto it = groups_.find({lane, shard});
  return it == groups_.end() ? nullptr : &it->second;
}

bool SysBook::groups_ready(const std::set<int>& shards, std::string* why) const {
  for (const auto& id : on_)
    for (const int shard : shards) {
      const auto* g = find_group(id, shard);
      if (!g || g->state != GroupState::ACTIVE) {
        if (why) {
          const char* st = !g ? "none" : g->state == GroupState::CREATING ? "creating" : g->state == GroupState::TRIGGERED ? "triggered"
                           : g->state == GroupState::TRIGGERING ? "triggering" : g->state == GroupState::RESETTING ? "resetting" : "none";
          *why = id + " shard " + std::to_string(shard) + ": order group " + st;
        }
        return false;
      }
    }
  if (why) *why = on_.empty() ? "no lane is on" : "";
  return true;
}

void SysBook::sent(const std::string& lane, std::int64_t at) {
  auto& d = days_[lane];
  ++d.sent;
  d.last_ms = at;
}

void SysBook::fill(const std::string& lane, const std::string& cid, const std::string& ticker, Side side, Qty count,
                   Px px, Micros fee, std::int64_t at) {
  if (count <= 0) return;
  auto& d = days_[lane];
  ++d.fills;
  d.hit.insert(cid);
  d.ctrs += count;
  d.fees += fee;
  d.last_ms = at;
  auto& inv = d.inv[ticker];
  Qty left = count;
  // close against the open position first, at its average
  if ((side == Side::BUY && inv.pos < 0) || (side == Side::SELL && inv.pos > 0)) {
    const Qty open = std::abs(inv.pos);
    const Qty c = std::min(left, open);
    const Micros avg_part = inv.cost / open * c + (inv.cost % open) * c / open;   // cost of the closed part
    const Micros at_px = px * c;
    d.real += side == Side::SELL ? at_px - avg_part : avg_part - at_px;
    inv.cost -= avg_part;
    inv.pos += side == Side::BUY ? c : -c;
    left -= c;
    if (!inv.pos) inv.cost = 0;
  }
  if (left > 0) {
    inv.pos += side == Side::BUY ? left : -left;
    inv.cost += px * left;
  }
}

void SysBook::roll(std::int64_t day_start_ms) {
  if (day_start_ms <= day_start_ms_) return;
  day_start_ms_ = day_start_ms;
  for (auto& [lane, d] : days_) {
    Day next;
    next.day_start_ms = day_start_ms;
    next.inv = std::move(d.inv);
    d = std::move(next);
  }
}

const SysBook::Day* SysBook::day(const std::string& lane) const {
  const auto it = days_.find(lane);
  return it == days_.end() ? nullptr : &it->second;
}

Qty SysBook::position(const std::string& lane, const std::string& ticker) const {
  const auto* d = day(lane);
  if (!d) return 0;
  const auto it = d->inv.find(ticker);
  return it == d->inv.end() ? 0 : it->second.pos;
}

Micros SysBook::unrealized(const std::string& lane, const MarkFn& mark) const {
  const auto* d = day(lane);
  if (!d) return 0;
  Micros u = 0;
  for (const auto& [tk, inv] : d->inv) {
    if (!inv.pos) continue;
    const auto m = mark ? mark(tk) : std::nullopt;
    if (!m) continue;   // no mark: no unrealized claimed either way
    const Micros value = *m * std::abs(inv.pos);
    u += inv.pos > 0 ? value - inv.cost : inv.cost - value;
  }
  return u;
}

Micros SysBook::open_worst(const std::string& lane) const {
  const auto* d = day(lane);
  if (!d) return 0;
  Micros w = 0;
  for (const auto& [tk, inv] : d->inv) {
    if (inv.pos > 0) w += inv.cost;
    else if (inv.pos < 0) w += kOneDollar * -inv.pos - inv.cost;
  }
  return w;
}

Micros SysBook::day_total(const MarkFn& mark) const {
  Micros t = 0;
  for (const auto& [lane, d] : days_) t += d.real - d.fees + unrealized(lane, mark);
  return t;
}

json SysBook::lane_json(const LaneSpec& l, RunState run, Micros reserved, int working, Micros eff_left,
                        const MarkFn& mark, const std::string& blocked) const {
  const auto* d = day(l.id);
  const Micros open = open_worst(l.id), used = open + reserved, unrl = unrealized(l.id, mark);
  const Micros fees = d ? d->fees : 0, real = d ? d->real - d->fees : 0;
  const Micros budget = l.budget * 100;   // Money (1/10,000 $) to micro-dollars
  const Micros cap_left = std::max<Micros>(0, std::min(budget - used, eff_left));
  std::string state = "ON", why;
  const bool is_on = on(l.id);
  if (!is_on) state = "OFF";
  else if (run != RunState::RUN) {
    state = "OFF";
    why = run == RunState::KILLED ? "KILLED" : run == RunState::HALTED ? "HALTED" : "STOP";
  } else if (!blocked.empty()) {
    state = "BLOCKED";
    why = blocked;
  } else if (cap_left <= 0) {
    state = "CAPPED";
    why = "budget used";
  }
  auto money = [](Micros m) { return money_str(m / 100); };
  return {{"id", l.id}, {"name", l.name}, {"venue", "K"}, {"state", state}, {"on", is_on}, {"style", l.style},
          {"budget", money(budget)}, {"used", money(used)}, {"open", money(open)}, {"wrk", working},
          {"sent", d ? d->sent : 0}, {"hit", d ? static_cast<int>(d->hit.size()) : 0}, {"fills", d ? d->fills : 0},
          {"ctrs", qty_to_number(d ? d->ctrs : 0)}, {"fees", money(fees)}, {"real", money(real)}, {"unrl", money(unrl)},
          {"day", money(real + unrl)}, {"lip", money(0)}, {"cap_left", money(cap_left)},
          {"thr", l.thr ? json(money_str(*l.thr)) : json(nullptr)},
          {"last_ms", d && d->last_ms ? json(d->last_ms) : json(nullptr)}, {"why", why}};
}

}  // namespace deskd
