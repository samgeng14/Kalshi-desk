#include <cmath>
#include <limits>

#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"

namespace desk {

CryptoLognormal::CryptoLognormal(const json& params, const std::string& user_agent)
    : http_(user_agent, 6) {
  // Fallback annual vols, only used until ~30 minutes of spot samples give a realized number.
  vol_cfg_ = {{"BTC", 0.55}, {"ETH", 0.75}, {"SOL", 1.0},  {"XRP", 0.9},   {"DOGE", 1.1}, {"LTC", 0.85},
              {"BCH", 0.9},  {"ADA", 1.0},  {"DOT", 1.0},  {"AVAX", 1.1},  {"LINK", 1.0}, {"MATIC", 1.1},
              {"SHIB", 1.3}, {"ATOM", 1.0}, {"UNI", 1.1},  {"AAVE", 1.05}};
  if (params.contains("annual_vol") && params["annual_vol"].is_object())
    for (auto& [k, v] : params["annual_vol"].items()) vol_cfg_[k] = v.get<double>();
  vol_band_ = params.value("vol_band", 0.25);
  basis_ = params.value("basis", 0.0004);            // Coinbase spot vs the settlement index (BRTI), as a log-price sd
  max_horizon_seconds_ = params.value("max_horizon_seconds", 36 * 3600);  // beyond this the 3h realized vol says nothing
  refresh_seconds_ = params.value("refresh_seconds", 20);
  spot_url_ = params.value("spot_url", std::string("https://api.coinbase.com/v2/prices/{sym}-USD/spot"));
  dvol_url_ = params.value("dvol_url", std::string("https://www.deribit.com/api/v2/public/get_volatility_index_data?currency={sym}&resolution=3600"));
}

std::string CryptoLognormal::symbol_for(const std::string& series_ticker) {
  // Every coin Coinbase quotes a USD spot for, longest ticker first so KXETHW does not match
  // as ETH... W. Kalshi names crypto series KX<COIN> with an optional suffix for the cadence.
  static const char* syms[] = {"DOGE", "AVAX", "LINK", "MATIC", "SHIB", "ATOM", "UNI", "AAVE", "ADA",
                               "XRP", "SOL", "LTC", "BCH", "DOT", "BTC", "ETH"};
  for (const char* s : syms) {
    std::string pre = std::string("KX") + s;
    if (series_ticker.compare(0, pre.size(), pre) == 0) return s;
  }
  return "";
}

bool CryptoLognormal::covers(const Market& m, const Series& s) const {
  if (s.category != "Crypto") return false;
  if (symbol_for(m.series_ticker).empty()) return false;
  if (m.strike_type != "greater" && m.strike_type != "less" && m.strike_type != "between") return false;
  // Only intraday/daily settlement markets; annual ones need a different model.
  return s.frequency == "hourly" || s.frequency == "daily" || s.frequency == "custom";
}

double CryptoLognormal::vol_for(const std::string& sym) const {
  // Prior: Deribit DVOL (30-day implied) when we have it, else the config number.
  double base = vol_cfg_.count(sym) ? vol_cfg_.at(sym) : 0.8;
  auto dv = vol_dvol_.find(sym);
  if (dv != vol_dvol_.end() && dv->second > 0.05) base = dv->second;
  // CRYPTO-2 first look (Sat 2026-09-05, four hourly events): the bracket curve implied 13-27%
  // annualized for the last 30 minutes while DVOL was 38%, and it tracked the realized vol of
  // the previous three hours (5-31%). A 30-day implied number is the wrong vol for a one-hour
  // bracket; use the short-horizon realized vol with a small vol-of-vol premium and keep DVOL
  // only as a sanity band.
  auto it = vol_realized_.find(sym);
  if (it == vol_realized_.end()) return base;
  double v = 1.15 * it->second;
  return std::min(1.5 * base, std::max(0.25 * base, v));
}

void CryptoLognormal::prepare(const std::vector<Market>& markets, Store& store) {
  long long now = now_s();
  std::map<std::string, bool> need;
  for (const auto& m : markets) {
    std::string sym = symbol_for(m.series_ticker);
    if (!sym.empty()) need[sym] = true;
  }
  for (auto& [sym, _] : need) {
    // Deribit DVOL, every 10 minutes (BTC and ETH only).
    if (!dvol_url_.empty() && (sym == "BTC" || sym == "ETH") && now - dvol_ts_[sym] >= 600) {
      dvol_ts_[sym] = now;
      try {
        std::string url = dvol_url_;
        auto pos = url.find("{sym}");
        if (pos != std::string::npos) url.replace(pos, 5, sym);
        url += "&start_timestamp=" + std::to_string((now - 3 * 3600) * 1000LL) +
               "&end_timestamp=" + std::to_string(now * 1000LL);
        HttpResponse r = http_.get(url);
        if (!r.ok()) throw std::runtime_error("dvol HTTP " + std::to_string(r.status));
        json j = json::parse(r.body, nullptr, false);
        const json& data = j["result"]["data"];
        if (data.is_array() && !data.empty()) {
          const json& last = data.back();  // [ts, open, high, low, close]
          double close = last.size() >= 5 ? last[4].get<double>() : 0.0;
          if (close > 1) {
            vol_dvol_[sym] = close / 100.0;
            store.exec("INSERT INTO ext_prices(ts,symbol,price) VALUES(?,?,?)", {now, "DVOL:" + sym, close});
          }
        }
      } catch (const std::exception& e) {
        LOG_WARN("dvol %s: %s", sym.c_str(), e.what());
      }
    }
    if (now - spot_ts_[sym] < refresh_seconds_) continue;
    std::string url = spot_url_;
    auto pos = url.find("{sym}");
    if (pos != std::string::npos) url.replace(pos, 5, sym);
    try {
      HttpResponse r = http_.get(url);
      if (!r.ok()) throw std::runtime_error("spot HTTP " + std::to_string(r.status));
      json j = json::parse(r.body, nullptr, false);
      double px = 0;
      if (j.contains("data") && j["data"].contains("amount")) px = parse_num(j["data"]["amount"].get<std::string>());
      else if (j.contains("price")) px = jnum(j, "price");
      if (px <= 0) throw std::runtime_error("no price in spot response");
      spot_[sym] = px;
      spot_ts_[sym] = now;
      spot_fails_[sym] = 0;
      store.exec("INSERT INTO ext_prices(ts,symbol,price) VALUES(?,?,?)", {now, sym, px});
      // Realized vol from ~1-minute samples over the last 3 hours (responsive to the intraday regime).
      json rows = store.query("SELECT ts,price FROM ext_prices WHERE symbol=? AND ts>? ORDER BY ts",
                              {sym, now - 3 * 3600});
      double sum2 = 0, sumdt = 0;
      int n = 0;
      long long lt = 0;
      double lp = 0;
      for (const auto& r : rows) {
        long long t = r["ts"].get<long long>();
        double p = r["price"].get<double>();
        if (lt == 0) { lt = t; lp = p; continue; }
        if (t - lt < 55) continue;
        double lr = std::log(p / lp);
        sum2 += lr * lr;
        sumdt += static_cast<double>(t - lt);
        ++n;
        lt = t;
        lp = p;
      }
      if (n >= 30 && sumdt > 0) {
        double var_per_sec = sum2 / sumdt;
        vol_realized_[sym] = std::sqrt(var_per_sec * 365.25 * 86400.0);
      }
    } catch (const std::exception& e) {
      LOG_WARN("crypto spot %s: %s", sym.c_str(), e.what());
      if (++spot_fails_[sym] >= 3) {
        // Three misses in a row: the feed does not quote this coin. Back off to once a minute
        // rather than spending a request on it every cycle.
        spot_ts_[sym] = now - refresh_seconds_ + 60;
      }
    }
  }
}

std::optional<Fair> CryptoLognormal::fair(const Market& m, const Series&, Store&) {
  std::string sym = symbol_for(m.series_ticker);
  auto it = spot_.find(sym);
  if (it == spot_.end()) return std::nullopt;
  long long now = now_s();
  if (now - spot_ts_[sym] > 120) return std::nullopt;  // stale spot: do not price
  long long T = parse_iso(m.close_time);
  if (T == 0) return std::nullopt;
  if (T - now < 45) return std::nullopt;  // nothing to do in the last seconds
  if (T - now > max_horizon_seconds_) return std::nullopt;  // long-dated (KXBTC yearly etc.): different model
  // The settlement index is not Coinbase spot: carry a fixed basis uncertainty so a strike a
  // few dollars away never prices at 0 or 1 in the last minutes. var = vol^2 tau + basis^2.
  double tau = static_cast<double>(T - now) / (365.25 * 86400.0) + (basis_ * basis_) / (vol_for(sym) * vol_for(sym));
  double spot = it->second;
  double lo = 0, hi = std::numeric_limits<double>::infinity();
  if (m.strike_type == "greater" && m.floor_strike) lo = *m.floor_strike;
  else if (m.strike_type == "less" && m.cap_strike) hi = *m.cap_strike;
  else if (m.strike_type == "between" && m.floor_strike && m.cap_strike) { lo = *m.floor_strike; hi = *m.cap_strike; }
  else return std::nullopt;
  double vol = vol_for(sym);
  double p = lognormal_prob_between(lo, hi, spot, vol, tau);
  double p_lo = lognormal_prob_between(lo, hi, spot, vol * (1 - vol_band_), tau);
  double p_hi = lognormal_prob_between(lo, hi, spot, vol * (1 + vol_band_), tau);
  Fair f;
  f.p = p;
  f.sigma = 0.5 * std::fabs(p_hi - p_lo) + 0.01;
  bool have_rv = vol_realized_.count(sym) > 0;
  char buf[200];
  std::snprintf(buf, sizeof buf, "%s spot %.2f vol %.0f%% (%s%s) tau %.1fmin", sym.c_str(), spot, 100 * vol,
                have_rv ? "realized 3h" : "no realized vol yet", vol_dvol_.count(sym) ? ", dvol band" : "",
                static_cast<double>(T - now) / 60.0);
  f.note = buf;
  f.cluster = m.series_ticker + "|" + event_date(m.event_ticker);  // same coin, same day
  if (!have_rv) {
    f.research_only = true;
    f.research_reason = "no short-horizon realized vol yet (needs ~30 min of spot samples)";
  }
  return f;
}

std::string CryptoLognormal::describe() const {
  std::string s = "lognormal off live spot + Deribit DVOL;";
  for (const auto& [sym, px] : spot_) {
    char buf[64];
    std::snprintf(buf, sizeof buf, " %s %.0f (vol %.0f%%)", sym.c_str(), px, 100 * vol_for(sym));
    s += buf;
  }
  return s;
}

}  // namespace desk
