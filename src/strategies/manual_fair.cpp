#include "desk/strategies.h"

namespace desk {

void ManualFair::prepare(const std::vector<Market>&, Store& store) {
  fairs_.clear();
  for (const auto& f : store.fairs()) fairs_[f.ticker] = f;
}

std::optional<Fair> ManualFair::fair(const Market& m, const Series&, Store&) {
  auto it = fairs_.find(m.ticker);
  if (it == fairs_.end()) return std::nullopt;
  Fair f;
  f.p = it->second.p;
  f.sigma = it->second.sigma > 0 ? it->second.sigma : 0.05;
  f.note = it->second.note.empty() ? "manual" : "manual: " + it->second.note;
  return f;
}

}  // namespace desk
