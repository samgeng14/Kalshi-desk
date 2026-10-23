#include "deskd/strat/lip.h"

#include <algorithm>
#include <cmath>

#include "deskd/web/contract.h"

namespace deskd {

namespace {

struct SideScore {
  double total = 0, ours = 0;
  bool reaches = false;
};

// levels best first; ours: our resting size at each price on this side
SideScore score(const std::vector<MdLevel>& levels, const std::map<Px, Qty>& ours, Qty target, double discount) {
  SideScore s;
  if (levels.empty() || target <= 0) return s;
  const Px best = levels.front().px;
  Qty left = target;
  for (const auto& l : levels) {
    if (left <= 0) break;
    const Qty counted = std::min(l.qty, left);
    const auto o = ours.find(l.px);
    const Qty mine = o == ours.end() ? 0 : std::min(o->second, l.qty);
    // ours stand at the back of the level: the target reaches them last
    const Qty mine_counted = std::max<Qty>(0, counted - (l.qty - mine));
    const double cents = std::fabs(static_cast<double>(l.px - best)) / 100.0;
    const double w = std::pow(discount, cents);
    s.total += static_cast<double>(counted) * w;
    s.ours += static_cast<double>(mine_counted) * w;
    left -= counted;
  }
  s.reaches = left <= 0;
  return s;
}

}  // namespace

LipShare lip_share(const MdBook& book, const std::vector<LipOwn>& ours, Qty target, int discount_bps) {
  LipShare out;
  const double discount = std::clamp(discount_bps, 0, 10000) / 10000.0;
  std::map<Px, Qty> bids, asks;
  for (const auto& o : ours)
    if (o.left > 0) (o.bid ? bids : asks)[o.price] += o.left;
  const SideScore b = score(book.bids, bids, target, discount), a = score(book.asks, asks, target, discount);
  out.scores = b.reaches && a.reaches;
  out.bid = b.total > 0 ? b.ours / b.total : 0;
  out.ask = a.total > 0 ? a.ours / a.total : 0;
  return out;
}

void LipMeter::sample(const std::string& tk, const kalshi::IncentiveRow& p, const MdBook& book,
                      const std::vector<LipOwn>& ours, std::int64_t now) {
  if (now < p.start_ms || now >= p.end_ms || book.stale) return;
  const LipShare s = lip_share(book, ours, p.target_size, p.discount_bps);
  std::lock_guard<std::mutex> lk(mu_);
  auto& w = windows_[tk + "|" + std::to_string(p.start_ms)];
  w.program = p;
  if (w.last_ms && now - w.last_ms < 1000) return;   // one snapshot a second
  w.last_ms = now;
  ++w.seconds;
  w.last_share = s.share();
  if (!ours.empty()) ++w.quoted;
  if (!s.scores) return;
  ++w.scored;
  w.share_sum += s.share();
  const double per_second = static_cast<double>(p.period_reward) / (static_cast<double>(p.end_ms - p.start_ms) / 1000.0);
  w.est += per_second * s.share();
  const auto midnight = et_midnight_ms(now);
  if (midnight != day_ms_) { day_ms_ = midnight; day_lane_.clear(); }
  double size = 0;
  for (const auto& o : ours) size += static_cast<double>(std::max<Qty>(0, o.left));
  if (size > 0)
    for (const auto& o : ours) day_lane_[o.lane] += per_second * s.share() * static_cast<double>(std::max<Qty>(0, o.left)) / size;
}

std::map<std::string, Money> LipMeter::lane_day(std::int64_t now) const {
  std::map<std::string, Money> out;
  std::lock_guard<std::mutex> lk(mu_);
  if (et_midnight_ms(now) != day_ms_) return out;   // nothing yet today
  for (const auto& [lane, est] : day_lane_) out[lane] = static_cast<Money>(std::llround(est));
  return out;
}

nlohmann::json LipMeter::row(const std::string& key, const Window& w) {
  const auto& p = w.program;
  return {{"tk", key.substr(0, key.find('|'))}, {"start_ms", p.start_ms}, {"end_ms", p.end_ms},
          {"seconds", w.seconds}, {"scored_s", w.scored}, {"quoted_s", w.quoted},
          {"mean_share", w.scored ? std::round(w.share_sum / w.scored * 10000) / 10000 : 0.0},
          {"est", money_str(static_cast<Money>(std::llround(w.est)))}, {"reward", money_str(p.period_reward)},
          {"target", qty_to_number(p.target_size)}, {"discount_bps", p.discount_bps}};
}

std::vector<nlohmann::json> LipMeter::finished(std::int64_t now) {
  std::vector<nlohmann::json> out;
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = windows_.begin(); it != windows_.end();) {
    if (now >= it->second.program.end_ms) {
      out.push_back(row(it->first, it->second));
      it = windows_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

nlohmann::json LipMeter::status() const {
  nlohmann::json out = nlohmann::json::object();
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [key, w] : windows_) {
    auto r = row(key, w);
    r["share_now"] = std::round(w.last_share * 10000) / 10000;
    out[key.substr(0, key.find('|'))] = std::move(r);
  }
  return out;
}

}  // namespace deskd
