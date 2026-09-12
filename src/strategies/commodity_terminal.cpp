// The 15-minute commodity contracts: one terminal distribution for the underlying, and every
// contract on it priced from that distribution. See docs/commodity-settlement.md for what these
// contracts settle on and how the desk checks that it is reading that same feed.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"

namespace desk {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

std::vector<CommodityTerminal::Underlying> CommodityTerminal::default_underlyings() {
  // The series, the feed's name for the underlying, and what the contract's own rules must say.
  // The Pyth symbols are the exchange's own, read off each series' `settlement_sources` on
  // 2026-09-12 (docs/commodity-settlement.md). They are first in the pattern list so an exact
  // match wins; the looser patterns stay behind them for the day a spelling changes.
  //
  // The last number is a fallback tolerance only. What the desk actually uses is the rounding the
  // contract itself states ("rounded to the nearest N decimal places"), which is 2 for gold and
  // WTI, 3 for silver, 5 for natural gas and copper. Reading it off the contract is the same
  // discipline as everything else here: the rule is published, so do not retype it.
  //
  // Kalshi also lists KXPLATINUM15M (Metal.XPT/USD) and KXPALLADIUM15M (Metal.XPD/USD) - note
  // those two are the plain metal feeds, not .Index. ones. Neither had an open window when this
  // was written, so they are recorded here and not built; switching them on is one line each.
  return {
      {"gold", "commodity_terminal_gold", {"KXGOLD15M"}, {"Metal.Index.GOLD/USD", "gold", "xau"}, {"pyth", "gold"}, "GC=F", 0.02},
      {"silver", "commodity_terminal_silver", {"KXSILVER15M"}, {"Metal.Index.SILVER/USD", "silver", "xag"}, {"pyth", "silver"}, "SI=F", 0.005},
      {"wti", "commodity_terminal_wti", {"KXWTI15M"}, {"Commodities.Index.PYTHOIL/USD", "pythoil", "wti", "oil"}, {"pyth", "oil"}, "CL=F", 0.01},
      {"natgas", "commodity_terminal_natgas", {"KXNATGAS15M"}, {"Commodities.Index.NATGAS/USD", "natgas", "gas"}, {"pyth", "natgas"}, "NG=F", 0.002},
      {"copper", "commodity_terminal_copper", {"KXCOPPER15M"}, {"Commodities.Index.CU/USD", "copper", "/cu"}, {"pyth", "copper"}, "HG=F", 0.0005},
  };
}

CommodityTerminal::CommodityTerminal(Underlying u, ReferenceFeed* feed, const json& params,
                                     const std::string& user_agent)
    : u_(std::move(u)), feed_(feed), http_(user_agent, 4) {
  const json& p = params.is_object() ? params : json::object();
  // The volatility estimator's own settings, shared by every underlying unless one overrides them.
  const json v = p.contains("volatility") && p["volatility"].is_object() ? p["volatility"] : json::object();
  auto num = [](const json& j, const char* key, auto& out) {
    if (j.contains(key) && j[key].is_number()) out = static_cast<std::decay_t<decltype(out)>>(j[key].get<double>());
  };
  num(v, "sample_seconds", vol_.sample_seconds);
  num(v, "ewma_halflife_seconds", vol_.ewma_halflife_seconds);
  num(v, "short_floor_multiplier", vol_.short_floor_multiplier);
  num(v, "min_samples", vol_.min_samples);
  num(v, "min_span_seconds", vol_.min_span_seconds);
  num(v, "band_ratio", vol_.band_ratio);
  num(p, "min_seconds_to_close", min_seconds_to_close_);
  num(p, "max_seconds_to_close", max_seconds_to_close_);
  num(p, "replication_max_age_seconds", replication_max_age_);
  num(p, "replication_windows_required", windows_required_);

  const json ext = p.contains("external") && p["external"].is_object() ? p["external"] : json::object();
  if (ext.contains("enabled") && ext["enabled"].is_boolean()) external_enabled_ = ext["enabled"].get<bool>();
  num(ext, "every_seconds", external_every_seconds_);
  external_url_ = ext.value("url", std::string("https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=1m&range=1d"));

  // Per-underlying overrides: series, the feed's symbol, the cross-check, the tolerance.
  const json under = p.contains("underlyings") && p["underlyings"].is_object() ? p["underlyings"] : json::object();
  if (under.contains(u_.key) && under[u_.key].is_object()) {
    const json& o = under[u_.key];
    if (o.contains("series") && o["series"].is_array()) {
      u_.series.clear();
      for (const auto& x : o["series"]) if (x.is_string()) u_.series.push_back(x.get<std::string>());
    }
    if (o.contains("symbols") && o["symbols"].is_array()) {
      std::vector<std::string> pats;
      for (const auto& x : o["symbols"]) if (x.is_string()) pats.push_back(x.get<std::string>());
      if (!pats.empty()) u_.symbol_patterns = pats;   // a configured name is tried first, exactly
    }
    if (o.contains("external_symbol") && o["external_symbol"].is_string())
      u_.external_symbol = o["external_symbol"].get<std::string>();
    num(o, "strike_tolerance", u_.strike_tolerance);
  }
}

bool CommodityTerminal::covers(const Market& m, const Series&) const {
  if (std::find(u_.series.begin(), u_.series.end(), m.series_ticker) == u_.series.end()) return false;
  return terminal_payoff(m).has_value();
}

// "The settlement value is rounded to the nearest N decimal places." The contract says what a
// tick is on this underlying, so the strike check uses that rather than a constant someone typed:
// one unit in the last stated place. Returns nullopt when the contract does not say.
std::optional<double> CommodityTerminal::stated_tolerance(const Market& m) {
  static const std::map<std::string, int> kWords{{"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3},
                                                 {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
                                                 {"eight", 8}, {"nine", 9}, {"ten", 10}};
  const std::string r = lower(m.rules_secondary);
  const auto at = r.find("rounded to the nearest");
  if (at == std::string::npos) return std::nullopt;
  const auto dp = r.find("decimal place", at);
  if (dp == std::string::npos || dp - at > 60) return std::nullopt;
  std::string between = r.substr(at + 22, dp - at - 22);
  int places = -1;
  // A digit, or the word.
  for (size_t i = 0; i < between.size(); ++i)
    if (std::isdigit(static_cast<unsigned char>(between[i]))) {
      places = between[i] - '0';
      if (i + 1 < between.size() && std::isdigit(static_cast<unsigned char>(between[i + 1])))
        places = places * 10 + (between[i + 1] - '0');
      break;
    }
  if (places < 0)
    for (const auto& [word, n] : kWords)
      if (between.find(word) != std::string::npos) { places = n; break; }
  if (places < 0 || places > 12) return std::nullopt;
  return std::pow(10.0, -places);
}

double CommodityTerminal::tolerance_for(const Market& m) const {
  if (auto stated = stated_tolerance(m)) return *stated;
  return u_.strike_tolerance;
}

bool CommodityTerminal::rules_name_this_feed(const Market& m) const {
  const std::string rules = lower(m.rules_primary);
  if (rules.empty()) return false;
  for (const auto& token : u_.rules_tokens)
    if (rules.find(lower(token)) == std::string::npos) return false;
  return true;
}

// The settlement check, run once per window: Kalshi publishes the strike, which by the contract is
// the one-minute candle close of the settlement feed at the window open. If our own recording of
// that feed reproduces it, we are reading what the contract settles on.
void CommodityTerminal::check_replication(const std::vector<Market>& markets, long long now) {
  if (!feed_ || symbol_.empty()) return;
  // The newest window first. The store hands markets over in ticker order and keeps a window in
  // "open" until a reconcile catches up, so the list normally begins with a window that has
  // already closed; checking that one and stopping would leave the window we are actually pricing
  // unverified while the dashboard said "settlement confirmed".
  std::vector<std::pair<long long, const Market*>> windows;
  for (const auto& m : markets) {
    if (!covers(m, Series{})) continue;
    const long long open = parse_iso(m.open_time), close = parse_iso(m.close_time);
    if (open <= 0 || open > now) continue;
    if (now - open > 3000) continue;      // older than what we still hold in memory
    if (close > 0 && close <= now) continue;   // a window that has already closed is not the one we price
    windows.emplace_back(open, &m);
  }
  std::sort(windows.begin(), windows.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [open, mp] : windows) {
    const Market& m = *mp;
    auto payoff = terminal_payoff(m);
    if (!payoff || payoff->type == TerminalPayoff::Range) continue;
    const double strike = payoff->type == TerminalPayoff::Above ? payoff->lower : payoff->upper;
    if (!(strike > 0)) continue;
    auto close_px = feed_->candle_close(symbol_, open);
    if (!close_px) continue;
    auto previous = feed_->replication(symbol_);
    // Against the window, not against the clock: `checked_ts` is when we ran the check, which is
    // later than every open window's open time and therefore skipped all of them.
    if (previous && previous->window_open >= open) continue;   // this window, or a newer one, is done
    const double* close = &*close_px;
    const double tol = tolerance_for(m);
    feed_->note_replication(symbol_, strike, *close, tol, now, open);
    const double err = *close - strike;
    if (std::fabs(err) > tol)
      LOG_WARN("%s: the published strike %.5f is %.5f from our own candle close %.5f on %s; "
               "this model stays research until they agree",
               u_.model_name.c_str(), strike, err, *close, symbol_.c_str());
    else
      LOG_INFO("%s: settlement feed confirmed on %s (strike %.5f, our candle %.5f)",
               u_.model_name.c_str(), symbol_.c_str(), strike, *close);
  }
}

// A futures print for the same commodity. It is a cross-check and a diagnostic: it never replaces
// the settlement feed, and nothing is priced off it.
void CommodityTerminal::refresh_external(long long now, Store& store) {
  if (!external_enabled_ || u_.external_symbol.empty() || external_url_.empty()) return;
  if (now - external_ts_ < external_every_seconds_) return;
  external_ts_ = now;
  std::string url = external_url_;
  const auto pos = url.find("{sym}");
  if (pos != std::string::npos) url.replace(pos, 5, u_.external_symbol);
  try {
    HttpResponse r = http_.get(url);
    if (!r.ok()) throw std::runtime_error("HTTP " + std::to_string(r.status));
    json j = json::parse(r.body, nullptr, false);
    double px = 0;
    long long px_ts = 0;
    if (j.is_object() && j.contains("chart") && j["chart"].contains("result") && j["chart"]["result"].is_array() &&
        !j["chart"]["result"].empty()) {
      const json& res = j["chart"]["result"][0];
      if (res.contains("meta")) {
        px = res["meta"].value("regularMarketPrice", 0.0);
        px_ts = res["meta"].value("regularMarketTime", 0LL);
      }
    }
    if (!(px > 0)) throw std::runtime_error("no price in the response");
    external_price_ = px;
    external_price_ts_ = px_ts > 0 ? px_ts : now;
    external_fails_ = 0;
    external_error_.clear();
    store.exec("INSERT INTO ext_prices(ts,symbol,price) VALUES(?,?,?)", {now, u_.external_symbol, px});
  } catch (const std::exception& e) {
    external_error_ = e.what();
    if (++external_fails_ == 3)
      LOG_WARN("%s: the %s cross-check is unavailable (%s); it is a diagnostic, so nothing else changes",
               u_.model_name.c_str(), u_.external_symbol.c_str(), e.what());
    if (external_fails_ >= 3) external_ts_ = now + 240;   // back off to every five minutes
  }
}

// Priority 3 of the session-17 brief. The probability at a fixed set of moments before the close,
// with the state it was computed from and the market it was computed against, so that calibration
// can be measured per commodity, per time-to-close and per regime once the windows settle.
void CommodityTerminal::record_checkpoints(const std::vector<Market>& markets, Store& store, long long now) {
  for (const auto& m : markets) {
    if (!covers(m, Series{})) continue;
    const long long close = parse_iso(m.close_time);
    if (close <= 0) continue;
    const double left = static_cast<double>(close - now);
    if (left < -60 || left > checkpoints_.front() + 900) continue;   // not this window's business
    auto& done = checkpoints_done_[m.ticker];
    for (size_t i = 0; i < checkpoints_.size(); ++i) {
      if (done.count(static_cast<int>(i))) continue;
      if (left > checkpoints_[i]) continue;            // not there yet
      done.insert(static_cast<int>(i));
      auto e = estimate(m, now);
      if (!e) continue;   // no feed, no estimate: the gap is itself the record, in the row count
      auto rep = feed_ ? feed_->replication(symbol_) : std::nullopt;
      // Everything that decided the number, at the instant it was decided. `settled` is left for
      // the research join: `markets.result` fills in once the window resolves.
      store.exec(
          "INSERT INTO commodity_calibration(ts,ticker,event_ticker,series_ticker,model,model_version,"
          "checkpoint_seconds,seconds_to_close,tau_seconds,p_model,sigma_model,band,spot,strike,"
          "distance_sigma,vol_sigma,vol_warm,vol_rv_5m,vol_ewma,vol_samples,reference_ts,reference_age,"
          "reference_gap_ms,reference_fresh,replication_consecutive,settlement_verified,yes_bid,yes_ask,"
          "no_bid,no_ask,quote_ts,strike_type) "
          "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {now, m.ticker, m.event_ticker, m.series_ticker, u_.model_name, std::string("v1"),
           checkpoints_[i], left, e->dist.tau_seconds, e->p, 0.5 * e->band + 0.01, e->band,
           e->dist.spot, e->strike, e->sigma_move, e->vol.sigma, e->vol.warm ? 1LL : 0LL,
           e->vol.rv_5m, e->vol.ewma, static_cast<long long>(e->vol.samples),
           e->value.received_ts_ms / 1000, static_cast<double>(e->feed_age), e->value.gap_ms,
           e->value.fresh ? 1LL : 0LL, rep ? rep->consecutive_matches : 0LL,
           settlement_verified(m).value_or(false) ? 1LL : 0LL,
           m.yes_bid, m.yes_ask, m.no_bid, m.no_ask, m.quote_ts, m.strike_type});
    }
  }
  // Windows that have closed are not coming back; their bookkeeping should not outlive them.
  for (auto it = checkpoints_done_.begin(); it != checkpoints_done_.end();) {
    bool live = false;
    for (const auto& m : markets) if (m.ticker == it->first) { live = true; break; }
    it = live ? std::next(it) : checkpoints_done_.erase(it);
  }
}

void CommodityTerminal::prepare(const std::vector<Market>& markets, Store& store) {
  if (!feed_) return;
  const long long now = now_s();
  bool any = false;
  for (const auto& m : markets)
    if (std::find(u_.series.begin(), u_.series.end(), m.series_ticker) != u_.series.end()) { any = true; break; }
  if (!any) return;
  if (symbol_.empty()) {
    symbol_ = feed_->symbol_for(u_.symbol_patterns);
    if (!symbol_.empty())
      LOG_INFO("%s: reading %s from the reference feed", u_.model_name.c_str(), symbol_.c_str());
  }
  check_replication(markets, now);
  record_checkpoints(markets, store, now);
  refresh_external(now, store);
}

std::optional<bool> CommodityTerminal::settlement_verified(const Market& m) const {
  if (!rules_name_this_feed(m)) return false;
  if (!feed_ || symbol_.empty()) return false;
  auto rep = feed_->replication(symbol_);
  if (!rep || !rep->matched) return false;
  // A run, not a single match. Reproducing one published strike on one quiet minute says almost
  // nothing; reproducing fifty in a row is the claim this model rests on, and it costs about
  // twelve hours of recording per underlying to establish. That is the price of the claim.
  if (rep->consecutive_matches < windows_required_) return false;
  return now_s() - rep->checked_ts <= replication_max_age_;
}

// The estimate itself, with no opinion about whether it may be traded. `fair()` wraps it in the
// tradability rules; the calibration recorder needs the same number at moments those rules would
// refuse (ten seconds before a close, nothing is tradable and the probability is at its most
// interesting), so the two callers share one piece of arithmetic rather than two.
std::optional<CommodityTerminal::Estimate> CommodityTerminal::estimate(const Market& m, long long now) {
  if (!feed_) return std::nullopt;
  auto payoff = terminal_payoff(m);
  if (!payoff) return std::nullopt;
  // The clock is the window's close, never `expected_expiration_time`: on these series that field
  // is hours past the window (docs/commodity-settlement.md).
  const long long close = parse_iso(m.close_time);
  if (close <= 0) return std::nullopt;
  if (symbol_.empty()) symbol_ = feed_->symbol_for(u_.symbol_patterns);
  if (symbol_.empty()) return std::nullopt;   // the feed has never carried this underlying
  auto value = feed_->latest(symbol_);
  if (!value || !(value->price > 0)) return std::nullopt;

  Estimate e;
  e.payoff = *payoff;
  e.value = *value;
  e.close_ts = close;
  e.seconds_to_close = static_cast<double>(close - now);
  const long long observed_ts = value->received_ts_ms / 1000;
  e.feed_age = now - observed_ts;
  const auto ticks = feed_->ticks(symbol_, now - 3600);
  e.vol = estimate_volatility(ticks, now, vol_);

  e.dist.spot = value->price;
  e.dist.sigma = e.vol.sigma;
  // The walk starts where it was last seen, not where the clock is. The variance the contract is
  // exposed to is the time from that observation to the close, which on a stale-but-usable feed
  // near the end of a window is meaningfully more than the time left: at the limits allowed here
  // (20 s old, 20 s to run) reading tau off the clock understates the standard deviation by 41%,
  // and always in the direction that makes the model look more certain than it is.
  e.dist.tau_seconds = static_cast<double>(close - observed_ts);
  e.dist.mu = 0;   // v1: no drift is claimed on this horizon
  if (!e.dist.usable() || !(e.dist.sigma > 0)) return std::nullopt;

  e.strike = payoff->type == TerminalPayoff::Below ? payoff->upper : payoff->lower;
  e.p = e.dist.probability(*payoff);
  // The model's own uncertainty on that probability: what a +/- band on sigma does to it.
  TerminalDistribution lo = e.dist, hi = e.dist;
  lo.sigma = e.dist.sigma * (1 - vol_.band_ratio);
  hi.sigma = e.dist.sigma * (1 + vol_.band_ratio);
  e.band = std::fabs(hi.probability(*payoff) - lo.probability(*payoff));
  e.sigma_move = e.dist.sd() > 0 && e.strike > 0 ? std::fabs(std::log(e.dist.spot / e.strike)) / e.dist.sd() : 0;
  return e;
}

std::optional<Fair> CommodityTerminal::fair(const Market& m, const Series&, Store&) {
  const long long now = now_s();
  auto est = estimate(m, now);
  if (!est) return std::nullopt;
  if (est->seconds_to_close < min_seconds_to_close_ || est->seconds_to_close > max_seconds_to_close_)
    return std::nullopt;
  const auto& payoff = est->payoff;
  const TerminalDistribution& dist = est->dist;
  const VolEstimate& vol = est->vol;
  const auto& value = est->value;
  const double strike = est->strike, p = est->p, band = est->band;
  const double seconds_to_close = est->seconds_to_close;
  const long long feed_age = est->feed_age;

  Fair f;
  f.p = std::min(0.999, std::max(0.001, p));
  // What this sigma is, and what it is not. It is the spread in the probability caused by a
  // +/-25% error in the volatility, halved to a standard-error-like number, plus a floor. It does
  // NOT price the two assumptions underneath: that the drift is zero over the window, and that
  // fifteen-minute log returns on a Pyth index are normal. Both are the model's own claims, and
  // neither can be argued down to a number here - the calibration curve in COM-1 is what will say
  // whether they hold. So read it as vol uncertainty, and read the router's uncertainty buffer as
  // a deduction based on vol uncertainty alone.
  f.sigma = std::min(0.5, 0.5 * band + 0.01);
  f.cluster = u_.key;   // every window of the same underlying is one correlated cluster

  auto rep = feed_->replication(symbol_);
  const bool rules_ok = rules_name_this_feed(m);
  const bool fresh_check = rep && now - rep->checked_ts <= replication_max_age_;
  const bool replicated = rep && rep->matched && fresh_check;
  const bool proven = replicated && rep->consecutive_matches >= windows_required_;

  // `reasons` is the model's verdict on its own DATA, and the router blocks a row that has any.
  // Keep it to that: an estimate computed from a dead feed or from a volatility that does not
  // exist yet is not an estimate. Whether this is the right feed at all is a different question,
  // answered below by research_only, because those rows are worth pricing and recording even
  // though they are worth showing to nobody.
  json reasons = json::array();
  if (feed_age > feed_->settings().stale_after_seconds)
    reasons.push_back("reference feed is " + std::to_string(feed_age) + "s old (limit " +
                      std::to_string(feed_->settings().stale_after_seconds) + "s)");
  if (!vol.warm)
    for (const auto& p2 : vol.problems) reasons.push_back(p2);

  const double sigma_move = est->sigma_move;
  json diagnostics{{"underlying", u_.key},
                   {"symbol", symbol_},
                   {"reference_price", dist.spot},
                   {"reference_source_ts_ms", value.source_ts_ms},
                   {"reference_age_seconds", feed_age},
                   {"strike", strike},
                   {"payoff", payoff.to_json()},
                   {"seconds_to_close", seconds_to_close},
                   {"tau_from_observation", dist.tau_seconds},
                   {"distance", dist.spot - strike},
                   {"distance_in_sigma", sigma_move},
                   {"terminal_distribution", dist.to_json()},
                   {"volatility", vol.to_json()},
                   {"probability_band", band},
                   {"settlement", {{"rules_name_this_feed", rules_ok},
                                   {"replicated_strike", replicated},
                                   {"replication_error", rep ? rep->error : 0.0},
                                   {"replication_checked_ts", rep ? rep->checked_ts : 0},
                                   {"tolerance", tolerance_for(m)},
                                   {"tolerance_from_contract", stated_tolerance(m).has_value()},
                                   {"consecutive_matches", rep ? rep->consecutive_matches : 0},
                                   {"windows_required", windows_required_}}}};
  json external{{"symbol", u_.external_symbol}, {"enabled", external_enabled_}};
  if (external_price_ > 0) {
    external["price"] = external_price_;
    external["ts"] = external_price_ts_;
    external["age_seconds"] = now - external_price_ts_;
    external["basis"] = external_price_ - dist.spot;                  // external minus the reference
    external["basis_fraction"] = (external_price_ - dist.spot) / dist.spot;
  }
  if (!external_error_.empty()) external["error"] = external_error_;
  diagnostics["external"] = external;

  f.inputs = json{{"valid", reasons.empty()},
                  {"reasons", reasons},
                  {"auto_eligible", false},   // nothing here trades unattended in this session
                  {"source", "Kalshi pyth_value (" + symbol_ + ")"},
                  // The reference observation this estimate was computed from, in seconds: the
                  // probability adapter reads this key and it becomes the quote's underlying_ts,
                  // which is what the board and the router age the estimate against.
                  {"underlying_ts", value.received_ts_ms / 1000}};
  for (auto it = diagnostics.begin(); it != diagnostics.end(); ++it) f.inputs[it.key()] = it.value();

  char buf[320];
  std::snprintf(buf, sizeof buf, "%s %.5f vs strike %.5f (%.2f sigma, %.0fs left, sigma %.3f%%/min)",
                u_.key.c_str(), dist.spot, strike, sigma_move, seconds_to_close,
                100 * dist.sigma * std::sqrt(60.0));
  f.note = buf;

  if (!rules_ok || !proven) {
    f.research_only = true;
    f.research_reason =
        !rules_ok ? "the contract's rules do not name this model's feed"
        : !rep    ? "the settlement feed has not yet reproduced a published strike for this underlying"
        : !replicated
            ? "the settlement feed has not reproduced the exchange's own published strike (last error " +
                  fmt2(rep->error) + " against a tolerance of " + fmt2(tolerance_for(m)) + ")"
            : "settlement replication is still accumulating: " + std::to_string(rep->consecutive_matches) +
                  " consecutive windows of " + std::to_string(windows_required_) + " required";
  }
  return f;
}

json commodity_model_status(Store& store, const std::string& model, long long windows_required,
                            long long settlements_required) {
  auto one = [&](const std::string& sql, const std::vector<SqlValue>& args, const char* field) -> double {
    json r = store.query(sql, args);
    return r.empty() || r[0][field].is_null() ? 0.0 : r[0].value(field, 0.0);
  };
  // How far the settlement gate has got. The calibration rows carry it as it stood at the time,
  // so this survives a restart in a way the in-memory counter does not.
  const double consecutive = one(
      "SELECT replication_consecutive AS n FROM commodity_calibration WHERE model=? ORDER BY id DESC LIMIT 1",
      {model}, "n");
  // Settled windows with a checkpoint on them: the sample every later question is asked of.
  const double settled = one(
      "SELECT COUNT(DISTINCT c.ticker) AS n FROM commodity_calibration c JOIN markets m ON m.ticker=c.ticker "
      "WHERE c.model=? AND m.result IN ('yes','no')", {model}, "n");
  const double checkpoints = one("SELECT COUNT(*) AS n FROM commodity_calibration WHERE model=?", {model}, "n");

  // What the research has concluded, if it has concluded anything. Written by the replay, never
  // by the desk, and never promoted to a trading state by either.
  json verdict = json::parse(store.kv_get("commodity_verdict:" + model).value_or("{}"), nullptr, false);
  if (verdict.is_discarded() || !verdict.is_object()) verdict = json::object();
  const std::string researched = verdict.value("status", std::string());
  static const std::set<std::string> kResearch{"CALIBRATED_NO_ALPHA", "MISPRICING_DETECTED", "FAILED_COST"};
  static const std::set<std::string> kHuman{"PAPER_CANDIDATE", "LIVE_CANDIDATE"};

  std::string status, reason;
  const std::string decided = store.kv_get("commodity_promotion:" + model).value_or("");
  if (consecutive < windows_required) {
    status = "SETTLEMENT_UNVERIFIED";
    reason = std::to_string(static_cast<long long>(consecutive)) + " of " + std::to_string(windows_required) +
             " consecutive windows replicated";
  } else if (settled < settlements_required) {
    status = "DATA_ACCUMULATING";
    reason = std::to_string(static_cast<long long>(settled)) + " of " + std::to_string(settlements_required) +
             " settled windows";
  } else if (kHuman.count(decided)) {
    status = decided;
    reason = "set by hand";
  } else if (kResearch.count(researched)) {
    status = researched;
    reason = verdict.value("reason", std::string("from research/commodity_terminal.py"));
  } else {
    status = "DATA_ACCUMULATING";
    reason = "enough settled windows; the replay has not reported a verdict on them yet";
  }
  // The interim reports the brief asks for, so "how far along is this" is answerable at a glance
  // rather than only at the end.
  json milestones = json::array();
  for (long long mark : {50LL, 100LL, 200LL, 400LL})
    milestones.push_back(json{{"settlements", mark}, {"reached", settled >= static_cast<double>(mark)}});
  return json{{"model", model},
              {"status", status},
              {"reason", reason},
              {"consecutive_windows", consecutive},
              {"windows_required", windows_required},
              {"settled_windows", settled},
              {"settlements_required", settlements_required},
              {"checkpoints_recorded", checkpoints},
              {"milestones", milestones},
              {"research_verdict", verdict}};
}

std::string CommodityTerminal::describe() const {
  std::string s = u_.key + " terminal-price model on " + (symbol_.empty() ? std::string("(feed not seen yet)") : symbol_);
  if (feed_) {
    auto v = feed_->latest(symbol_);
    if (v) {
      char buf[96];
      std::snprintf(buf, sizeof buf, ", reference %.5f", v->price);
      s += buf;
    }
    auto rep = feed_->replication(symbol_);
    s += rep && rep->matched ? ", settlement confirmed" : ", settlement not confirmed";
  }
  return s;
}

}  // namespace desk
