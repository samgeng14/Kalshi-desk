#include "deskd/gw/sys_book.h"
#include <algorithm>
#include <cstdlib>

namespace deskd {
using json = nlohmann::json;

const std::vector<LaneSpec>& default_lanes() {
  static const std::vector<LaneSpec> L = {
      {"M1", "LIP-15", "REST", 55 * 10000, 4000, 5000, {"KXNATGAS15M"}, std::nullopt},
      {"M2", "BAND 96-99", "REST", 25 * 10000, 2000, 5000, {}, std::nullopt},
      // the LIP probe (plan section 5(a), decision 4): 30 a side in minutes 0-5 of LIP-15's
      // windows, about 40 windows a day, its own $15 loss cap (the engine's lane_pause); 40
      // contracts and the $25 LIP group cap (limits 2 and 3)
      {"P1", "LIP PROBE", "REST", 25 * 10000, 4000, 7500, {"KXNATGAS15M"}, std::nullopt},
      {"S1", "ARB", "IOC", 15 * 10000, 2000, 5000, {}, Money{200}, false},
      {"C1", "COLLEGE TAKE", "IOC", 10 * 10000, 2000, 5000, {}, Money{300}, false},
      {"X1", "ESPN EXP", "IOC", 5 * 10000, 200, 1000, {}, Money{300}, false},
  };
  return L;
}

LaneSpec paper_lane() {
  LaneSpec l{"D1", "PAPER", "REST", 20 * 10000, 200, 1000, {}, std::nullopt};
  l.paper_only = l.on_at_start = true;
  return l;
}

SysBook::SysBook(std::vector<LaneSpec> lanes) : lanes_(std::move(lanes)) {
  for (const auto& l : lanes_) if (l.on_at_start && l.engine) on_.insert(l.id);
}

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

bool SysBook::fill(const std::string& lane, const std::string& cid, const std::string& ticker, Side side, Qty count,
                   Px px, Micros fee, std::int64_t at, const std::string& trade_id) {
  if (count <= 0) return false;
  if (!trade_id.empty() && !trades_.insert(trade_id).second) return false;
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
  return true;
}

Micros SysBook::settle(const std::string& lane, const std::string& ticker, Px value, std::int64_t at) {
  auto dit = days_.find(lane);
  if (dit == days_.end()) return 0;
  auto it = dit->second.inv.find(ticker);
  if (it == dit->second.inv.end() || !it->second.pos) return 0;
  const Inv inv = it->second;
  const Micros worth = value * std::abs(inv.pos);
  const Micros pnl = inv.pos > 0 ? worth - inv.cost : inv.cost - worth;
  dit->second.real += pnl;
  dit->second.last_ms = at;
  dit->second.inv.erase(it);
  return pnl;
}

std::vector<std::pair<std::string, std::string>> SysBook::held() const {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [lane, d] : days_)
    for (const auto& [tk, inv] : d.inv) if (inv.pos) out.emplace_back(lane, tk);
  return out;
}

void SysBook::roll(std::int64_t day_start_ms) {
  if (day_start_ms <= day_start_ms_) return;
  day_start_ms_ = day_start_ms;
  trades_.clear();   // a REST read's fills from before this midnight are not counted again (gw skips them)
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
    // never priced: counted as worthless (a long loses its cost, a short pays $1 a contract)
    const Micros value = (m ? *m : (inv.pos > 0 ? 0 : kOneDollar)) * std::abs(inv.pos);
    u += inv.pos > 0 ? value - inv.cost : inv.cost - value;
  }
  return u;
}

std::string corr_group(const std::string& ticker, const std::string& event_ticker) {
  const std::string series = ticker.substr(0, ticker.find('-'));
  static const std::set<std::string> metals = {"KXGOLD15M", "KXSILVER15M", "KXCOPPER15M", "KXPLATINUM15M", "KXPALLADIUM15M"};
  static const std::set<std::string> fx = {"KXEURUSD15M", "KXGBPUSD15M", "KXUSDJPY15M", "KXAUDUSD15M", "KXUSDCAD15M"};
  static const std::set<std::string> energy = {"KXNATGAS15M", "KXWTI15M", "KXBRENT15M"};
  if (metals.count(series)) return "METALS";
  if (fx.count(series)) return "FX";
  if (energy.count(series)) return "ENERGY";
  if (!event_ticker.empty()) return event_ticker;
  const auto dash = ticker.rfind('-');
  return dash == std::string::npos ? ticker : ticker.substr(0, dash);
}

Micros corr_group_cap(const std::string& group) {
  const bool lip = group == "METALS" || group == "FX" || group == "ENERGY";
  return (lip ? 25 : 15) * 1000000LL;
}

Micros SysBook::open_worst_in(const std::string& lane, const std::function<bool(const std::string&)>& in) const {
  const auto* d = day(lane);
  if (!d) return 0;
  Micros w = 0;
  for (const auto& [tk, inv] : d->inv) {
    if (!in(tk)) continue;
    if (inv.pos > 0) w += inv.cost;
    else if (inv.pos < 0) w += kOneDollar * -inv.pos - inv.cost;
  }
  return w;
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

json SysBook::to_json(const Halt& halt, std::int64_t saved_ms) const {
  json lanes = json::object();
  for (const auto& [lane, d] : days_) {
    json inv = json::object();
    for (const auto& [tk, i] : d.inv) if (i.pos) inv[tk] = {i.pos, i.cost};
    lanes[lane] = {{"sent", d.sent}, {"fills", d.fills}, {"hit", d.hit}, {"ctrs", d.ctrs}, {"fees", d.fees},
                   {"real", d.real}, {"last_ms", d.last_ms}, {"inv", inv}};
  }
  return {{"v", 1}, {"saved_ms", saved_ms}, {"day_start_ms", day_start_ms_}, {"trades", trades_}, {"lanes", lanes},
          {"halt", halt.on ? json{{"ms", halt.ms}, {"why", halt.why}} : json(nullptr)}};
}

bool SysBook::restore(const json& j, std::int64_t day_start_ms, Halt* halt, std::string* why) {
  try {
    if (!j.is_object() || j.value("v", 0) != 1) {
      if (why) *why = "not a version 1 Systematic book";
      return false;
    }
    const bool today = j.at("day_start_ms").get<std::int64_t>() == day_start_ms;
    std::map<std::string, Day> days;
    for (const auto& [lane, l] : j.at("lanes").items()) {
      if (!this->lane(lane)) continue;   // a lane this build no longer has
      Day d;
      d.day_start_ms = day_start_ms;
      for (const auto& [tk, i] : l.at("inv").items()) d.inv[tk] = Inv{i.at(0).get<Qty>(), i.at(1).get<Micros>()};
      if (today) {
        d.sent = l.at("sent").get<int>();
        d.fills = l.at("fills").get<int>();
        d.hit = l.at("hit").get<std::set<std::string>>();
        d.ctrs = l.at("ctrs").get<Qty>();
        d.fees = l.at("fees").get<Micros>();
        d.real = l.at("real").get<Micros>();
        d.last_ms = l.at("last_ms").get<std::int64_t>();
      }
      days[lane] = std::move(d);
    }
    std::set<std::string> trades;
    if (today) trades = j.at("trades").get<std::set<std::string>>();
    Halt h;
    if (j.at("halt").is_object()) {
      const Halt saved{true, j["halt"].at("ms").get<std::int64_t>(), j["halt"].at("why").get<std::string>()};
      if (today || saved.floor()) h = saved;   // a floor HALT outlives the day
    }
    days_ = std::move(days);
    trades_ = std::move(trades);
    day_start_ms_ = day_start_ms;
    if (halt) *halt = h;
    return true;
  } catch (const json::exception& e) {
    if (why) *why = std::string("malformed: ") + e.what();
    return false;
  }
}

json SysBook::lane_json(const LaneSpec& l, RunState run, Micros reserved, int working, Micros eff_left,
                        const MarkFn& mark, const std::string& blocked, Micros lip) const {
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
          {"day", money(real + unrl)}, {"lip", money(lip)}, {"cap_left", money(cap_left)},
          {"thr", l.thr ? json(money_str(*l.thr)) : json(nullptr)},
          {"last_ms", d && d->last_ms ? json(d->last_ms) : json(nullptr)}, {"why", why}};
}

}  // namespace deskd
