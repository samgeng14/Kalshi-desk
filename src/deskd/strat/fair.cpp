#include "deskd/strat/fair.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include "deskd/app/log.h"
#include "deskd/rest/state.h"

namespace deskd {
using json = nlohmann::json;
namespace {
// The decimals a strike is written with (at most 8): the rounding a contract states, read off the
// number when the series' own rule is not in the table.
int decimals_of(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.8f", v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0') s.pop_back();
  const auto dot = s.find('.');
  return dot == std::string::npos ? 0 : static_cast<int>(s.size() - dot - 1);
}
std::vector<std::pair<long long, double>> seconds(const std::vector<std::pair<std::int64_t, double>>& ms) {
  std::vector<std::pair<long long, double>> out;
  out.reserve(ms.size());
  for (const auto& [t, v] : ms) out.emplace_back(t / 1000, v);
  return out;
}
Px to_px(double p) { return static_cast<Px>(std::llround(std::min(1.0, std::max(0.0, p)) * kOneDollar)); }
}  // namespace

const std::vector<Underlying>& financial_underlyings() {
  static const std::vector<Underlying> u = {
      {"gold", "KXGOLD15M", {"Metal.Index.GOLD/USD", "gold", "xau"}, 2},
      {"silver", "KXSILVER15M", {"Metal.Index.SILVER/USD", "silver", "xag"}, 3},
      {"wti", "KXWTI15M", {"Commodities.Index.PYTHOIL/USD", "pythoil", "wti"}, 2},
      {"natgas", "KXNATGAS15M", {"Commodities.Index.NATGAS/USD", "natgas"}, 5},
      {"copper", "KXCOPPER15M", {"Commodities.Index.CU/USD", "copper"}, 5},
      {"platinum", "KXPLATINUM15M", {"Metal.XPT/USD", "xpt", "platinum"}, -1},
      {"palladium", "KXPALLADIUM15M", {"Metal.XPD/USD", "xpd", "palladium"}, -1},
      {"eurusd", "KXEURUSD15M", {"FX.EUR/USD", "EUR/USD"}, -1},
      {"gbpusd", "KXGBPUSD15M", {"FX.GBP/USD", "GBP/USD"}, -1},
      {"usdjpy", "KXUSDJPY15M", {"FX.USD/JPY", "USD/JPY"}, -1},
  };
  return u;
}

const Underlying* underlying_for(const std::string& ticker) {
  for (const auto& u : financial_underlyings())
    if (ticker.size() > u.series.size() && ticker.compare(0, u.series.size(), u.series) == 0 &&
        ticker[u.series.size()] == '-')
      return &u;
  return nullptr;
}

bool FinancialFair::same_strike(double close, double strike, int decimals) {
  if (!std::isfinite(close) || !std::isfinite(strike) || close <= 0 || strike <= 0) return false;
  const int d = decimals >= 0 ? decimals : decimals_of(strike);
  return std::fabs(close - strike) <= std::pow(10.0, -d) + 1e-12;
}

std::optional<ModelFair> FinancialFair::price(const kalshi::MarketRow& m, const RefView& v, std::int64_t now,
                                              const VolSettings& vs, std::string* why) {
  auto no = [why](const std::string& w) -> std::optional<ModelFair> { if (why) *why = w; return std::nullopt; };
  const auto payoff = terminal_payoff(m.strike_type, m.floor_strike, m.cap_strike);
  if (!payoff) return no("no terminal payoff (" + (m.strike_type.empty() ? std::string("no strike type") : m.strike_type) + ")");
  if (!m.close_ms || *m.close_ms <= now) return no("closed");
  if (!v.fresh) return no("reference value not fresh");
  const auto vol = estimate_volatility(seconds(v.ticks), now / 1000, vs);
  if (!vol.warm) return no(vol.problems.empty() ? "volatility not warm" : vol.problems.front());
  TerminalDistribution d;
  d.spot = v.value;
  d.sigma = vol.sigma;
  d.tau_seconds = (*m.close_ms - now) / 1000.0;
  if (!d.usable()) return no("unusable distribution");
  auto at = [&](double sigma) { auto x = d; x.sigma = sigma; return x.probability(*payoff); };
  const double p = d.probability(*payoff);
  const double lo = at(vol.sigma * (1 - vs.band_ratio)), hi = at(vol.sigma * (1 + vs.band_ratio));
  ModelFair f;
  f.fair = to_px(p);
  f.sd = to_px(std::fabs(hi - lo) / 2);
  f.ts_ms = now;
  f.spot = d.spot;
  f.strike = m.floor_strike.value_or(m.cap_strike.value_or(0));
  f.sigma = vol.sigma;
  f.tau_s = d.tau_seconds;
  return f;
}

FinancialFair::FinancialFair(SymbolFn symbol, ViewFn view, std::int64_t watching_since_ms)
    : symbol_(std::move(symbol)), view_(std::move(view)), since_ms_(watching_since_ms),
      board_(std::make_shared<const FairBoard>()), move_sd_(std::make_shared<const std::map<std::string, double>>()) {}

std::shared_ptr<const FairBoard> FinancialFair::board() const {
  std::lock_guard<std::mutex> lk(m_);
  return board_;
}

std::shared_ptr<const std::map<std::string, double>> FinancialFair::move_sd() const {
  std::lock_guard<std::mutex> lk(m_);
  return move_sd_;
}

void FinancialFair::persist(const std::string& path) {
  std::lock_guard<std::mutex> lk(m_);
  state_path_ = path;
  std::ifstream in(path);
  if (!in) return;   // none yet
  const auto j = json::parse(in, nullptr, false);
  try {
    if (j.is_discarded() || j.value("v", 0) != 1) throw std::runtime_error("not a version 1 replication file");
    for (const auto& [key, r] : j.at("replication").items()) {
      Replication x;
      x.checked = r.at("checked").get<std::int64_t>();
      x.matched = r.at("matched").get<std::int64_t>();
      x.consecutive = r.at("consecutive").get<std::int64_t>();
      x.last_open_ms = r.at("last_open_ms").get<std::int64_t>();
      x.last_strike = r.at("last_strike").get<double>();
      x.last_close = r.at("last_close").get<double>();
      x.last_ok = r.at("last_ok").get<bool>();
      replication_[key] = x;
    }
  } catch (const std::exception& e) {
    // counting again from none only delays the gate; it never opens it early
    replication_.clear();
    state_error_ = path + " cannot be read (" + e.what() + "): replication counts start again";
    DLOG_WARN("%s", state_error_.c_str());
  }
}

void FinancialFair::save_locked() const {
  if (state_path_.empty()) return;
  json under = json::object();
  for (const auto& [key, r] : replication_)
    under[key] = {{"checked", r.checked}, {"matched", r.matched}, {"consecutive", r.consecutive},
                  {"last_open_ms", r.last_open_ms}, {"last_strike", r.last_strike}, {"last_close", r.last_close},
                  {"last_ok", r.last_ok}};
  const std::string tmp = state_path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json{{"v", 1}, {"replication", under}}.dump();
    if (!out) { DLOG_WARN("cannot write %s", tmp.c_str()); return; }
  }
  if (std::rename(tmp.c_str(), state_path_.c_str()) != 0) DLOG_WARN("cannot replace %s", state_path_.c_str());
}

void FinancialFair::step(const rest::Snapshot& s, std::int64_t now) {
  auto board = std::make_shared<FairBoard>();
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = checked_.begin(); it != checked_.end();)
    it = s.markets.count(it->first) ? std::next(it) : checked_.erase(it);
  why_.clear();
  auto move = std::make_shared<std::map<std::string, double>>();
  bool counted = false;
  std::map<std::string, std::string> symbols;   // underlying key -> symbol, this step
  std::map<std::string, std::optional<RefView>> hour;   // symbol -> its last hour, this step
  std::map<std::string, VolEstimate> vols;              // symbol -> its volatility, this step
  for (const auto& [ticker, m] : s.markets) {
    const Underlying* u = underlying_for(ticker);
    if (!u) continue;
    auto sym = symbols.find(u->key);
    if (sym == symbols.end()) sym = symbols.emplace(u->key, symbol_(u->patterns)).first;
    if (sym->second.empty()) { why_[ticker] = "no pyth_value symbol for " + u->key + " yet"; continue; }
    auto h = hour.find(sym->second);
    if (h == hour.end()) {
      h = hour.emplace(sym->second, view_(sym->second, now - 3600000, now)).first;
      if (h->second && h->second->fresh) vols[sym->second] = estimate_volatility(seconds(h->second->ticks), now / 1000, vol_);
    }
    // the move to the close, for every listed window (the strike check below is the fair's only)
    if (const auto vol = vols.find(sym->second); vol != vols.end() && vol->second.warm && m.close_ms && *m.close_ms > now)
      (*move)[ticker] = vol->second.sigma * std::sqrt((*m.close_ms - now) / 1000.0);
    // This window's strike against the candle close this feed saw at its open.
    if (!checked_.count(ticker) && m.open_ms && m.floor_strike && now >= *m.open_ms + 2000) {
      if (*m.open_ms - 60000 < since_ms_) {
        checked_[ticker] = false;   // opened before deskd was watching: cannot be checked
      } else if (const auto v = view_(sym->second, *m.open_ms - 120000, now)) {
        const auto close = minute_close(seconds(v->ticks), *m.open_ms / 1000);
        if (!close) {
          checked_[ticker] = false;   // no value in the minute before the open: not counted
        } else {
          const bool ok = same_strike(*close, *m.floor_strike, u->decimals);
          auto& r = replication_[u->key];
          ++r.checked;
          if (ok) { ++r.matched; ++r.consecutive; } else r.consecutive = 0;
          r.last_open_ms = *m.open_ms;
          r.last_strike = *m.floor_strike;
          r.last_close = *close;
          r.last_ok = ok;
          checked_[ticker] = ok;
          counted = true;
        }
      }
    }
    const auto c = checked_.find(ticker);
    if (c == checked_.end() || !c->second) {
      why_[ticker] = c == checked_.end() ? "window not open or strike not checked yet"
                                         : "this window's strike was not reproduced by the feed";
      continue;
    }
    const auto& v = h->second;
    if (!v) { why_[ticker] = "no reference value"; continue; }
    std::string why;
    if (auto f = price(m, *v, now, vol_, &why)) {
      f->underlying = u->key;
      (*board)[ticker] = *f;
    } else {
      why_[ticker] = why;
    }
  }
  board_ = std::move(board);
  move_sd_ = std::move(move);
  if (counted) save_locked();
}

bool FinancialFair::gated(const std::string& underlying) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = replication_.find(underlying);
  return it != replication_.end() && it->second.consecutive >= 50;
}

json FinancialFair::status() const {
  std::lock_guard<std::mutex> lk(m_);
  json under = json::object();
  for (const auto& [key, r] : replication_)
    under[key] = {{"checked", r.checked}, {"matched", r.matched}, {"consecutive", r.consecutive},
                  {"gate", r.consecutive >= 50}, {"last_open_ms", r.last_open_ms},
                  {"last_strike", r.last_strike}, {"last_close", r.last_close}, {"last_ok", r.last_ok}};
  json markets = json::object();
  for (const auto& [tk, f] : *board_)
    markets[tk] = {{"fair", price_str(f.fair)}, {"sd", price_str(f.sd)}, {"spot", f.spot}, {"strike", f.strike},
                   {"sigma", f.sigma}, {"tau_s", f.tau_s}};
  for (const auto& [tk, why] : why_) markets[tk] = {{"fair", nullptr}, {"why", why}};
  json st = {{"replication", under}, {"markets", markets}};
  if (!state_error_.empty()) st["replication_file_error"] = state_error_;
  return st;
}

}  // namespace deskd
