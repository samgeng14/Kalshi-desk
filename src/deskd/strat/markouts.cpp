#include "deskd/strat/markouts.h"
#include <algorithm>

namespace deskd {
using json = nlohmann::json;
namespace {
constexpr std::int64_t kKeepMs = 180000, kSampleMs = 200;
constexpr std::int64_t kHorizonMs[] = {1000, 10000, 60000};
const char* kHorizon[] = {"1s", "10s", "60s"};
}  // namespace

void Markouts::mid(const std::string& ticker, std::int64_t at, Px m) {
  std::lock_guard<std::mutex> lk(m_);
  auto& h = mids_[ticker];
  if (!h.empty() && at < h.back().first) return;   // out of order
  if (!h.empty() && h.back().second == m) return;   // unchanged: the last sample still holds
  if (!h.empty() && at - h.back().first < kSampleMs) h.back().second = m;   // this slice's latest
  else h.emplace_back(at, m);
  // the window, plus the newest sample from before it (the mid then in force)
  while (h.size() >= 2 && at - h[1].first > kKeepMs) h.pop_front();
}

std::optional<Px> Markouts::mid_at(const std::string& ticker, std::int64_t at) const {
  std::lock_guard<std::mutex> lk(m_);
  return mid_at_locked(ticker, at);
}

std::optional<Px> Markouts::mid_at_locked(const std::string& ticker, std::int64_t at) const {
  const auto it = mids_.find(ticker);
  if (it == mids_.end()) return std::nullopt;
  std::optional<Px> last;
  for (const auto& s : it->second) {
    if (s.first > at) break;
    last = s.second;
  }
  if (!last || *last < 0) return std::nullopt;   // none yet, or the book was stale then
  return last;
}

void Markouts::fill(const std::string& fid, const std::string& lane, const std::string& ticker, Side side, Px px, Qty qty,
                    std::int64_t ts) {
  std::lock_guard<std::mutex> lk(m_);
  if (fid.empty() || qty <= 0 || seen_.count(fid)) return;
  seen_[fid] = true;
  pending_.push_back(Pending{fid, lane, ticker, side, px, qty, ts, 0});
}

std::vector<json> Markouts::step(std::int64_t now) {
  std::vector<json> out;
  std::lock_guard<std::mutex> lk(m_);
  for (auto& p : pending_) {
    while (p.done < 3 && now >= p.ts_ms + kHorizonMs[p.done] + 500) {   // half a second for late book updates
      const int h = p.done++;
      const auto m = mid_at_locked(p.ticker, p.ts_ms + kHorizonMs[h]);
      if (!m) continue;   // no mid then: skipped, never guessed
      const Money mo = p.side == Side::BUY ? *m - p.px : p.px - *m;   // Px and Money share 1/10,000 $
      out.push_back({{"fid", p.fid}, {"h", kHorizon[h]}, {"mo", money_str(mo)}, {"mid", price_str(*m)}});
      auto& mean = means_[p.lane.empty() ? "MAN" : p.lane][kHorizon[h]];
      ++mean.n;
      mean.contracts += p.qty;
      mean.sum += mo * p.qty;
    }
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [now](const Pending& p) {
    return p.done >= 3 || now - p.ts_ms > kKeepMs;
  }), pending_.end());
  if (seen_.size() > 50000) seen_.clear();   // fids are unique; the bound only stops growth
  return out;
}

json Markouts::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json lanes = json::object();
  for (const auto& [lane, hs] : means_)
    for (const auto& [h, m] : hs)
      lanes[lane][h] = {{"fills", m.n}, {"mean", m.contracts ? json(money_str(m.sum / m.contracts)) : json(nullptr)}};
  return {{"pending", pending_.size()}, {"lanes", lanes}};
}

}  // namespace deskd
