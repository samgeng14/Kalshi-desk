#include "deskd/gw/sys_book.h"
#include <algorithm>
#include <cstdlib>

namespace deskd {
using json = nlohmann::json;

// Each lane's budget (decisions 1), limits and rules. A lane whose engine is not built yet has
// engine=false and cannot be switched on; deskd sets a lane's series, gate and engine where it
// builds the engine (app/venue.cpp).
const std::vector<LaneSpec>& default_lanes() {
  static const std::vector<LaneSpec> L = [] {
    constexpr Micros kDollar = 1000000;
    // the LIP probe (plan section 5(a), decision 4): 30 a side in minutes 0-5 of LIP-15's
    // windows, about 40 windows a day, its own $15 loss cap (the engine's lane_pause); 40
    // contracts and the $25 LIP group cap (limits 2 and 3)
    LaneSpec p1{"P1", "LIP PROBE", "REST", 25 * 10000, 4000, 7500, {"KXNATGAS15M"}, std::nullopt};
    // arbitrage (plan 5(c)): baskets of up to 6 legs of 10 contracts, 1c a contract after fees;
    // 2.5 x 10 x 6 x 2 passes in its group's 15 s window; its $15 shared with W1 (family ARB)
    LaneSpec s1{"S1", "ARB", "IOC", 15 * 10000, 2000, 30000, {}, Money{100}, false};
    s1.family = "ARB";
    // thin college games (plan 5(d)): 20c net of the fee, $10 a game, ~$40 open; retired after
    // 100 trades if the net 300 s markout is not positive, or once down $20
    LaneSpec c1{"C1", "COLLEGE TAKE", "IOC", 40 * 10000, 2000, 5000, {}, Money{2000}, false};
    c1.event_cap = 10 * kDollar;
    c1.retire_rule = LaneSpec::RetireRule::MARKOUT;
    c1.retire_trades = 100;
    c1.retire_h = "300s";
    c1.retire_loss = 20 * kDollar;
    // the ESPN experiment (plan 5(e)): $1-2 takes of at most 4 contracts, and every NBA lane's $10
    // game cap (its $5 budget binds first); retired after 100 trades or $20 lost
    LaneSpec x1{"X1", "ESPN EXP", "IOC", 5 * 10000, 400, 1000, {}, Money{300}, false};
    x1.event_cap = 10 * kDollar;
    x1.retire_rule = LaneSpec::RetireRule::CAP;
    x1.retire_trades = 100;
    x1.retire_loss = 20 * kDollar;
    // the NBA model (decisions 2, on 5(d)'s template): 10c net, 10 contracts a market, $10 a game;
    // retired once down $20 (its engine holds it on its markouts)
    LaneSpec n1{"N1", "NBA MODEL", "IOC", 20 * 10000, 1000, 2000, {}, Money{1000}, false};
    n1.event_cap = 10 * kDollar;
    n1.retire_loss = 20 * kDollar;
    // pregame 90-95c NBA favourites (decisions 2): a forward test of 5 contracts, held to
    // settlement; retired after 60 trades or $15 lost. A price band, not an edge: no thr
    LaneSpec n2{"N2", "NBA FAV 90-95", "IOC", 15 * 10000, 500, 1000, {}, std::nullopt, false};
    n2.retire_rule = LaneSpec::RetireRule::CAP;
    n2.retire_trades = 60;
    n2.retire_loss = 15 * kDollar;
    // Polymarket reward quoting (decision 6): blocked until poly_checks records a GTD lead of 60 s
    // or less (decisions 3)
    LaneSpec q1{"Q1", "PM QUOTE", "REST", 20 * 10000, 2000, 5000, {}, std::nullopt, false};
    q1.venue = "P";
    // the two-venue weather NO basket (plan 5(c), decisions 4): a 1c lock after both venues'
    // fees, inside the ARB family's $15
    LaneSpec w1{"W1", "WX 2-VENUE", "IOC", 10 * 10000, 2000, 5000, {}, Money{100}, false};
    w1.venue = "KP";
    w1.family = "ARB";
    return std::vector<LaneSpec>{
        {"M1", "LIP-15", "REST", 55 * 10000, 4000, 5000, {"KXNATGAS15M"}, std::nullopt},
        {"M2", "BAND 96-99", "REST", 25 * 10000, 2000, 5000, {}, std::nullopt},
        p1, s1, c1, x1, n1, n2, q1, w1};
  }();
  return L;
}

LaneSpec paper_lane() {
  LaneSpec l{"D1", "PAPER", "REST", 20 * 10000, 200, 1000, {}, std::nullopt};
  l.paper_only = l.on_at_start = true;
  return l;
}

SysBook::SysBook(std::vector<LaneSpec> lanes) : lanes_(std::move(lanes)) {
  for (const auto& l : lanes_) if (l.on_at_start && l.engine && l.gate_why.empty()) on_.insert(l.id);
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
  auto& life = lives_[lane];
  ++d.fills;
  d.hit.insert(cid);
  if (life.hit.add(cid)) {
    ++life.trades;
    if (!life.first_ms) life.first_ms = at;
  }
  d.ctrs += count;
  d.fees += fee;
  d.charged += std::max<Micros>(0, fee);
  life.fees += fee;
  d.last_ms = at;
  auto& inv = d.inv[ticker];
  Qty left = count;
  // close against the open position first, at its average
  if ((side == Side::BUY && inv.pos < 0) || (side == Side::SELL && inv.pos > 0)) {
    const Qty open = std::abs(inv.pos);
    const Qty c = std::min(left, open);
    const Micros avg_part = inv.cost / open * c + (inv.cost % open) * c / open;   // cost of the closed part
    const Micros at_px = px * c;
    const Micros real = side == Side::SELL ? at_px - avg_part : avg_part - at_px;
    d.real += real;
    life.real += real;
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
  lives_[lane].real += pnl;
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

Micros SysBook::unrealized(const std::string& lane, const MarkFn& mark, Micros* upside) const {
  if (upside) *upside = 0;
  const auto* d = day(lane);
  if (!d) return 0;
  Micros u = 0;
  for (const auto& [tk, inv] : d->inv) {
    if (!inv.pos) continue;
    const auto m = mark ? mark(tk) : std::nullopt;
    // never priced: counted as worthless (a long loses its cost, a short pays $1 a contract); at
    // its best it is $1 a contract better (a long wins, a short pays nothing)
    if (!m && upside) *upside += kOneDollar * std::abs(inv.pos);
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

bool SysBook::Ids::add(const std::string& id) {
  if (!in.insert(id).second) return false;
  order.push_back(id);
  while (order.size() > kLifeIds) {
    in.erase(order.front());
    order.pop_front();
  }
  return true;
}

const SysBook::Life* SysBook::life(const std::string& lane) const {
  const auto it = lives_.find(lane);
  return it == lives_.end() ? nullptr : &it->second;
}

bool SysBook::markout(const std::string& lane, const std::string& fid, const std::string& h, Money mo_net, Qty qty) {
  const auto* l = this->lane(lane);
  if (!l || l->retire_h.empty() || h != l->retire_h || qty <= 0) return false;
  auto& life = lives_[lane];
  if (!life.mo_fids.add(fid)) return false;
  ++life.mo_n;
  life.mo_ctrs += qty;
  life.mo_sum += mo_net * qty;
  return true;
}

std::string SysBook::retire_due(const LaneSpec& l, const MarkFn& mark) const {
  const auto* f = life(l.id);
  if (f && f->retired) return "";
  // the loss rule, at any count: realized less fees over its life, its open positions at the mark.
  // A retirement lasts, so unlike the daily stop it never counts a position with no mark as
  // worthless (after a restart md has no book yet and gw's last mids are gone; a closed market has
  // none until it settles, a one-sided book none at all): it counts each at its best, and so waits
  // only while that could bring the lane back inside its limit
  if (l.retire_loss > 0) {
    Micros upside = 0;
    const Micros best = (f ? f->real - f->fees : 0) + unrealized(l.id, mark, &upside) + upside;
    if (best <= -l.retire_loss)
      return "down $" + money_str(-best / 100) + " over its life, its limit $" + money_str(l.retire_loss / 100);
  }
  if (!f || l.retire_trades <= 0 || f->trades < l.retire_trades) return "";
  if (l.retire_rule == LaneSpec::RetireRule::CAP)
    return "its cap of " + std::to_string(l.retire_trades) + " trades";
  if (l.retire_rule == LaneSpec::RetireRule::MARKOUT && f->mo_n >= kRetireMarked && f->mo_sum <= 0)
    return std::to_string(f->trades) + " trades, mean net " + l.retire_h + " markout " + money_str(f->mo_sum / f->mo_ctrs) +
           " over " + std::to_string(f->mo_n) + " marked";
  return "";
}

bool SysBook::retire(const std::string& lane, std::int64_t at, const std::string& why) {
  if (!this->lane(lane)) return false;
  auto& life = lives_[lane];
  if (life.retired) return false;
  life.retired = true;
  life.retired_ms = at;
  life.retired_why = why;
  on_.erase(lane);
  return true;
}

bool SysBook::reset_life(const std::string& lane) {
  if (!this->lane(lane)) return false;
  lives_.erase(lane);
  return true;
}

json SysBook::to_json(const Halt& halt, std::int64_t saved_ms) const {
  json lanes = json::object();
  for (const auto& [lane, d] : days_) {
    json inv = json::object();
    for (const auto& [tk, i] : d.inv) if (i.pos) inv[tk] = {i.pos, i.cost};
    lanes[lane] = {{"sent", d.sent}, {"fills", d.fills}, {"hit", d.hit}, {"ctrs", d.ctrs}, {"fees", d.fees},
                   {"charged", d.charged}, {"real", d.real}, {"last_ms", d.last_ms}, {"inv", inv}};
  }
  json lives = json::object();
  for (const auto& [lane, f] : lives_)
    lives[lane] = {{"trades", f.trades}, {"real", f.real}, {"fees", f.fees}, {"hit", f.hit.order}, {"first_ms", f.first_ms},
                   {"mo_n", f.mo_n}, {"mo_ctrs", f.mo_ctrs}, {"mo_sum", f.mo_sum}, {"mo_fids", f.mo_fids.order},
                   {"retired", f.retired}, {"retired_ms", f.retired_ms}, {"retired_why", f.retired_why}};
  return {{"v", 1}, {"saved_ms", saved_ms}, {"day_start_ms", day_start_ms_}, {"trades", trades_}, {"lanes", lanes},
          {"life", lives}, {"halt", halt.on ? json{{"ms", halt.ms}, {"why", halt.why}} : json(nullptr)}};
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
        // a book saved before rebates were kept apart: every fee then was a charge
        d.charged = l.contains("charged") ? l.at("charged").get<Micros>() : d.fees;
        d.real = l.at("real").get<Micros>();
        d.last_ms = l.at("last_ms").get<std::int64_t>();
      }
      days[lane] = std::move(d);
    }
    // lives, on any day; none in a book saved before they were kept, and each lane starts afresh
    std::map<std::string, Life> lives;
    if (j.contains("life")) {
      if (!j.at("life").is_object()) {
        if (why) *why = "malformed: its life is not an object";
        return false;
      }
      for (const auto& [lane, x] : j.at("life").items()) {
        if (!this->lane(lane)) continue;
        Life f;
        f.trades = x.at("trades").get<int>();
        f.real = x.at("real").get<Micros>();
        f.fees = x.at("fees").get<Micros>();
        for (const auto& id : x.at("hit").get<std::vector<std::string>>()) f.hit.add(id);
        f.first_ms = x.at("first_ms").get<std::int64_t>();
        f.mo_n = x.at("mo_n").get<std::int64_t>();
        f.mo_ctrs = x.at("mo_ctrs").get<Qty>();
        f.mo_sum = x.at("mo_sum").get<std::int64_t>();
        for (const auto& id : x.at("mo_fids").get<std::vector<std::string>>()) f.mo_fids.add(id);
        f.retired = x.at("retired").get<bool>();
        f.retired_ms = x.at("retired_ms").get<std::int64_t>();
        f.retired_why = x.at("retired_why").get<std::string>();
        lives[lane] = std::move(f);
      }
    }
    std::set<std::string> trades;
    if (today) trades = j.at("trades").get<std::set<std::string>>();
    Halt h;
    if (j.at("halt").is_object()) {
      const Halt saved{true, j["halt"].at("ms").get<std::int64_t>(), j["halt"].at("why").get<std::string>()};
      if (today || saved.floor()) h = saved;   // a floor HALT outlives the day
    }
    days_ = std::move(days);
    lives_ = std::move(lives);
    for (const auto& [lane, f] : lives_) if (f.retired) on_.erase(lane);
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
  const auto* f = life(l.id);
  if (f && f->retired) {
    state = "OFF";
    why = "retired: " + f->retired_why;
  } else if (!l.gate_why.empty()) {
    state = "OFF";
    why = l.gate_why;
  } else if (!is_on) {
    state = "OFF";
  } else if (run != RunState::RUN) {
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
  return {{"id", l.id}, {"name", l.name}, {"venue", l.venue}, {"state", state}, {"on", is_on}, {"style", l.style},
          {"budget", money(budget)}, {"used", money(used)}, {"open", money(open)}, {"wrk", working},
          {"sent", d ? d->sent : 0}, {"hit", d ? static_cast<int>(d->hit.size()) : 0}, {"fills", d ? d->fills : 0},
          {"ctrs", qty_to_number(d ? d->ctrs : 0)}, {"fees", money(fees)}, {"real", money(real)}, {"unrl", money(unrl)},
          {"day", money(real + unrl)}, {"lip", money(lip)}, {"cap_left", money(cap_left)},
          {"thr", l.thr ? json(money_str(*l.thr)) : json(nullptr)},
          {"last_ms", d && d->last_ms ? json(d->last_ms) : json(nullptr)}, {"why", why}};
}

}  // namespace deskd
