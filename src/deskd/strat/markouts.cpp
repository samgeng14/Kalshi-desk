#include "deskd/strat/markouts.h"
#include <algorithm>

namespace deskd {
using json = nlohmann::json;
namespace {
constexpr std::int64_t kKeepMs = 330000, kSampleMs = 200;   // the 300 s horizon plus half a minute for a late read
constexpr int kHorizons = 4;
constexpr std::int64_t kHorizonMs[kHorizons] = {1000, 10000, 60000, 300000};
const char* kHorizon[kHorizons] = {"1s", "10s", "60s", "300s"};
constexpr int kStreamed = 3;   // the contract's MO_H: the first three

// a / b rounded down, b > 0
std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a / b - (a % b < 0); }
}  // namespace

bool MarkoutRow::on_stream() const {
  return std::find(kHorizon, kHorizon + kStreamed, h) != kHorizon + kStreamed;
}

json MarkoutRow::pub() const {
  return {{"fid", fid}, {"h", h}, {"mo", money_str(mo)}, {"mid", price_str(mid)}};
}

json MarkoutRow::store() const {
  auto j = pub();
  j["lane"] = lane.empty() ? json(nullptr) : json(lane);
  j["net"] = money_str(net);
  j["qty"] = qty_to_number(qty);
  return j;
}

void Markouts::mid(const std::string& key, std::int64_t at, Px m) {
  std::lock_guard<std::mutex> lk(m_);
  auto& h = mids_[key];
  if (!h.empty() && at < h.back().first) return;   // out of order
  if (!h.empty() && h.back().second == m) return;   // unchanged: the last sample still holds
  if (!h.empty() && at - h.back().first < kSampleMs) h.back().second = m;   // this slice's latest
  else h.emplace_back(at, m);
  // the window, plus the newest sample from before it (the mid then in force)
  while (h.size() >= 2 && at - h[1].first > kKeepMs) h.pop_front();
}

std::optional<Px> Markouts::mid_at(const std::string& key, std::int64_t at) const {
  std::lock_guard<std::mutex> lk(m_);
  return mid_at_locked(key, at);
}

std::optional<Px> Markouts::mid_at_locked(const std::string& key, std::int64_t at) const {
  const auto it = mids_.find(key);
  if (it == mids_.end()) return std::nullopt;
  std::optional<Px> last;
  for (const auto& s : it->second) {
    if (s.first > at) break;
    last = s.second;
  }
  if (!last || *last < 0) return std::nullopt;   // none yet, or the book was stale then
  return last;
}

void Markouts::fill(const std::string& fid, const std::string& lane, const std::string& key, Side side, Px px, Qty qty,
                    std::int64_t ts, Money fee) {
  std::lock_guard<std::mutex> lk(m_);
  if (fid.empty() || qty <= 0 || seen_.count(fid)) return;
  seen_[fid] = true;
  pending_.push_back(Pending{fid, lane, key, side, px, qty, ts, fee, 0});
}

std::vector<MarkoutRow> Markouts::step(std::int64_t now) {
  std::vector<MarkoutRow> out;
  std::lock_guard<std::mutex> lk(m_);
  for (auto& p : pending_) {
    while (p.done < kHorizons && now >= p.ts_ms + kHorizonMs[p.done] + 500) {   // half a second for late book updates
      const int h = p.done++;
      const auto m = mid_at_locked(p.key, p.ts_ms + kHorizonMs[h]);
      if (!m) continue;   // no mid then: skipped, never guessed
      const Money mo = p.side == Side::BUY ? *m - p.px : p.px - *m;   // Px and Money share 1/10,000 $
      // the fee per contract rounded up, so a net markout is never better than it was
      const Money net = floor_div(mo * p.qty - p.fee * kOneContract, p.qty);
      out.push_back(MarkoutRow{p.fid, p.lane, kHorizon[h], mo, net, *m, p.qty});
      auto& mean = means_[p.lane.empty() ? "MAN" : p.lane][kHorizon[h]];
      ++mean.n;
      mean.contracts += p.qty;
      mean.sum += mo * p.qty;
      mean.net += net * p.qty;
    }
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [now](const Pending& p) {
    return p.done >= kHorizons || now - p.ts_ms > kKeepMs;
  }), pending_.end());
  if (seen_.size() > 50000) seen_.clear();   // fids are unique; the bound only stops growth
  return out;
}

Markouts::LaneMean Markouts::lane_mean(const std::string& lane, const std::string& horizon) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto l = means_.find(lane);
  if (l == means_.end()) return {};
  const auto h = l->second.find(horizon);
  if (h == l->second.end() || !h->second.contracts) return {};
  return {h->second.n, h->second.sum / h->second.contracts, h->second.net / h->second.contracts};
}

json Markouts::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json lanes = json::object();
  for (const auto& [lane, hs] : means_)
    for (const auto& [h, m] : hs)
      lanes[lane][h] = {{"fills", m.n}, {"mean", m.contracts ? json(money_str(m.sum / m.contracts)) : json(nullptr)},
                        {"net", m.contracts ? json(money_str(m.net / m.contracts)) : json(nullptr)}};
  return {{"pending", pending_.size()}, {"lanes", lanes}};
}

}  // namespace deskd
