#include "deskd/strat/fill_model.h"

#include <algorithm>

namespace deskd {

FillEstimate estimate_fill(const std::vector<TapePrint>& tape, bool fills_from_buyers, Qty ahead, Qty size,
                           int horizon_s, std::int64_t now_ms, std::int64_t covered_from_ms,
                           std::int64_t lookback_ms, int step_ms) {
  FillEstimate e;
  if (horizon_s <= 0 || size <= 0 || step_ms <= 0) {
    e.why = "no horizon or size";
    return e;
  }
  const std::int64_t span = horizon_s * 1000LL;
  const std::int64_t from = std::max(covered_from_ms, now_ms - lookback_ms);
  const std::int64_t last_start = now_ms - span;
  if (covered_from_ms <= 0 || last_start < from) {
    e.why = "learning: not one whole " + std::to_string(horizon_s) + " s of this market's trades yet";
    return e;
  }
  // this side's prints since `from`: their times and running volume
  std::vector<std::int64_t> ts;
  std::vector<Qty> cum{0};
  for (const auto& p : tape) {
    if (p.taker_bought_yes != fills_from_buyers || p.ts_ms <= from || p.ts_ms > now_ms || p.qty <= 0) continue;
    ts.push_back(p.ts_ms);
    cum.push_back(cum.back() + p.qty);
  }
  const Qty need_full = std::max<Qty>(0, ahead) + size, need_any = std::max<Qty>(0, ahead) + 1;
  int full = 0, any = 0;
  std::vector<double> took;
  for (std::int64_t s = from; s <= last_start; s += step_ms) {
    ++e.windows;
    const std::size_t i = static_cast<std::size_t>(std::upper_bound(ts.begin(), ts.end(), s) - ts.begin());
    const std::size_t j = static_cast<std::size_t>(std::upper_bound(ts.begin(), ts.end(), s + span) - ts.begin());
    const Qty vol = cum[j] - cum[i];
    if (vol >= need_any) ++any;
    if (vol >= need_full) {
      ++full;
      // the print that completed it: the first k with cum[k + 1] - cum[i] >= need_full
      const auto at = std::lower_bound(cum.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                       cum.begin() + static_cast<std::ptrdiff_t>(j) + 1, cum[i] + need_full);
      const std::size_t k = static_cast<std::size_t>(at - cum.begin()) - 1;
      took.push_back(static_cast<double>(ts[k] - s) / 1000.0);
    }
  }
  if (e.windows > 0) {
    e.p_full = static_cast<double>(full) / e.windows;
    e.p_any = static_cast<double>(any) / e.windows;
  }
  if (!took.empty()) {
    std::nth_element(took.begin(), took.begin() + static_cast<std::ptrdiff_t>(took.size() / 2), took.end());
    e.median_s = took[took.size() / 2];
  }
  return e;
}

void FillModel::insert(Tape& t, const std::string& id, const TapePrint& p) {
  if (!id.empty()) {
    if (!t.ids.insert(id).second) return;   // the live feed and a backfill both carry it
    if (t.ids.size() > 50000) t.ids.clear();
  }
  if (t.prints.empty() || t.prints.back().ts_ms <= p.ts_ms) {
    t.prints.push_back(p);
    return;
  }
  const auto at = std::upper_bound(t.prints.begin(), t.prints.end(), p.ts_ms,
                                   [](std::int64_t ts, const TapePrint& q) { return ts < q.ts_ms; });
  t.prints.insert(at, p);
}

void FillModel::trim(Tape& t, std::int64_t now_ms) {
  const std::int64_t keep_from = now_ms - kKeepMs;
  while (!t.prints.empty() && t.prints.front().ts_ms < keep_from) t.prints.pop_front();
  if (t.covered_from_ms && t.covered_from_ms < keep_from) t.covered_from_ms = keep_from;
}

void FillModel::add(const std::string& tk, const std::string& trade_id, const TapePrint& p) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& t = tapes_[tk];
  if (!t.covered_from_ms) t.covered_from_ms = p.ts_ms;   // at the latest, the feed was on by its first print
  insert(t, trade_id, p);
  trim(t, std::max(p.ts_ms, t.prints.empty() ? p.ts_ms : t.prints.back().ts_ms));
}

void FillModel::backfill(const std::string& tk, std::int64_t covered_from_ms,
                         const std::vector<std::pair<std::string, TapePrint>>& prints) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& t = tapes_[tk];
  for (const auto& [id, p] : prints) insert(t, id, p);
  if (covered_from_ms > 0 && (!t.covered_from_ms || covered_from_ms < t.covered_from_ms)) t.covered_from_ms = covered_from_ms;
  t.backfilled = true;
  if (!t.prints.empty()) trim(t, t.prints.back().ts_ms);
}

void FillModel::cover(const std::string& tk, std::int64_t from_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& t = tapes_[tk];
  if (!t.covered_from_ms || from_ms < t.covered_from_ms) t.covered_from_ms = from_ms;
}

bool FillModel::needs_backfill(const std::string& tk) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = tapes_.find(tk);
  return it == tapes_.end() || !it->second.backfilled;
}

std::int64_t FillModel::covered_from(const std::string& tk) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = tapes_.find(tk);
  return it == tapes_.end() ? 0 : it->second.covered_from_ms;
}

FillEstimate FillModel::estimate(const std::string& tk, bool fills_from_buyers, Qty ahead, Qty size, int horizon_s,
                                 std::int64_t now_ms) const {
  std::vector<TapePrint> prints;
  std::int64_t covered = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = tapes_.find(tk);
    if (it != tapes_.end()) {
      prints.assign(it->second.prints.begin(), it->second.prints.end());
      covered = it->second.covered_from_ms;
    }
  }
  FillEstimate e = estimate_fill(prints, fills_from_buyers, ahead, size, horizon_s, now_ms, covered);
  if (e.why.empty() && e.windows < kMinWindows)
    e.why = "learning: " + std::to_string(e.windows) + " s of this market's trades so far";
  return e;
}

}  // namespace deskd
